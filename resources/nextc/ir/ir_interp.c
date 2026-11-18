#include "ir.h"

#include <math.h>

/* Reference interpreter for MIR: simple, slow, no optimizations. It is the
 * oracle of the differential tests (ADR-010). */

constexpr uint64_t CG_FUNCTION_HANDLE_TAG = 0xFFFF000000000000ull;
constexpr uint32_t CG_INTERP_MAX_DEPTH = 200;
constexpr uint64_t CG_INTERP_MAX_STEPS = 200000000ull;

typedef struct cg_interp {
    const cg_module_t *module;
    const cg_interp_host_t *host;
    cg_diag_t *diag;
    uint8_t **data_memory; /* per symbol */
    uint64_t steps;
    uint32_t depth;
    bool failed;
} cg_interp_t;

static uint64_t cg_mask(cg_type_t type, uint64_t value) {
    switch (type) {
    case CG_T_I1:
        return value & 1;
    case CG_T_I8:
        return value & 0xFF;
    case CG_T_I16:
        return value & 0xFFFF;
    case CG_T_I32:
    case CG_T_F32:
        return value & 0xFFFFFFFFull;
    default:
        return value;
    }
}

static int64_t cg_sext(cg_type_t type, uint64_t value) {
    switch (type) {
    case CG_T_I1:
        return (value & 1) ? -1 : 0;
    case CG_T_I8:
        return (int8_t)value;
    case CG_T_I16:
        return (int16_t)value;
    case CG_T_I32:
        return (int32_t)value;
    default:
        return (int64_t)value;
    }
}

static double cg_to_double(cg_type_t type, uint64_t bits) {
    if (type == CG_T_F32) {
        uint32_t narrow = (uint32_t)bits;
        float value;
        memcpy(&value, &narrow, 4);
        return value;
    }
    double value;
    memcpy(&value, &bits, 8);
    return value;
}

static uint64_t cg_from_double(cg_type_t type, double value) {
    if (type == CG_T_F32) {
        float narrow = (float)value;
        uint32_t bits;
        memcpy(&bits, &narrow, 4);
        return bits;
    }
    uint64_t bits;
    memcpy(&bits, &value, 8);
    return bits;
}

static void cg_interp_fail(cg_interp_t *interp, const char *message) {
    if (!interp->failed) {
        cg_diag_error(interp->diag, "interpreter: %s", message);
    }
    interp->failed = true;
}

static uint64_t cg_load_memory(cg_type_t type, const uint8_t *address) {
    uint64_t value = 0;
    memcpy(&value, address, type == CG_T_I1 ? 1 : cg_type_size(type));
    return cg_mask(type, value);
}

static void cg_store_memory(cg_type_t type, uint8_t *address, uint64_t value) {
    memcpy(address, &value, type == CG_T_I1 ? 1 : cg_type_size(type));
}

static bool cg_interp_function(cg_interp_t *interp, const cg_func_t *func,
                               const uint64_t *args, uint32_t arg_count,
                               uint64_t *result);

static bool cg_interp_call_symbol(cg_interp_t *interp, cg_symbol_t symbol,
                                  const uint64_t *args, uint32_t count,
                                  cg_type_t ret_type, uint64_t *result) {
    const cg_func_t *callee = cg_module_function_of(interp->module, symbol);
    if (callee != nullptr) {
        return cg_interp_function(interp, callee, args, count, result);
    }
    if (interp->host != nullptr && interp->host->call != nullptr &&
        interp->host->call(interp->host->user,
                           cg_symbol_name(interp->module, symbol), args, count,
                           ret_type, result)) {
        return true;
    }
    cg_interp_fail(interp, "call to an unresolved function");
    return false;
}

static bool cg_interp_function(cg_interp_t *interp, const cg_func_t *func,
                               const uint64_t *args, uint32_t arg_count,
                               uint64_t *result) {
    uint64_t *values;
    uint64_t *phi_temp;
    uint8_t **frame_allocations = nullptr;
    uint32_t allocation_count = 0;
    cg_block_t block = 0;
    cg_block_t previous = CG_INVALID;
    bool ok = false;
    if (interp->depth >= CG_INTERP_MAX_DEPTH) {
        cg_interp_fail(interp, "call depth exceeded");
        return false;
    }
    if (arg_count < func->param_count) {
        cg_interp_fail(interp, "too few arguments");
        return false;
    }
    values = calloc(func->values.count + 1, sizeof(uint64_t));
    phi_temp = calloc(func->values.count + 1, sizeof(uint64_t));
    if (values == nullptr || phi_temp == nullptr) {
        free(values);
        free(phi_temp);
        cg_interp_fail(interp, "out of memory");
        return false;
    }
    ++interp->depth;
    *result = 0;
    while (!interp->failed) {
        const cg_ir_block_t *ir_block = &func->blocks.data[block];
        uint32_t index = 0;
        /* Phis read the values from before the block was entered. */
        for (; index < ir_block->insts.count; ++index) {
            const cg_inst_t *inst = &func->insts.data[ir_block->insts.data[index]];
            if (inst->op != CG_OP_PHI) {
                break;
            }
            for (uint32_t pair = 0; pair < inst->c; ++pair) {
                if (func->operands.data[inst->b + pair * 2 + 1] == previous) {
                    phi_temp[inst->dst] =
                        values[func->operands.data[inst->b + pair * 2]];
                }
            }
        }
        for (uint32_t phi = 0; phi < index; ++phi) {
            const cg_inst_t *inst = &func->insts.data[ir_block->insts.data[phi]];
            values[inst->dst] = phi_temp[inst->dst];
        }
        for (; index < ir_block->insts.count && !interp->failed; ++index) {
            const cg_inst_t *inst = &func->insts.data[ir_block->insts.data[index]];
            cg_type_t type = (cg_type_t)inst->type;
            cg_type_t operand_type =
                inst->a < func->values.count ? cg_value_type(func, inst->a)
                                             : CG_T_VOID;
            uint64_t a = inst->a < func->values.count ? values[inst->a] : 0;
            uint64_t b = inst->b < func->values.count ? values[inst->b] : 0;
            uint64_t out = 0;
            bool defines = inst->dst != CG_INVALID;
            if (++interp->steps > CG_INTERP_MAX_STEPS) {
                cg_interp_fail(interp, "step limit exceeded");
                break;
            }
            switch (inst->op) {
            case CG_OP_ICONST:
                out = cg_mask(type, (uint64_t)inst->imm);
                break;
            case CG_OP_FCONST:
                out = (uint64_t)inst->imm;
                break;
            case CG_OP_PARAM:
                out = cg_mask(type, args[inst->imm]);
                break;
            case CG_OP_SYMADDR: {
                cg_symbol_t symbol = (cg_symbol_t)inst->imm;
                if (interp->module->symbols.data[symbol].kind == CG_SYM_FUNC) {
                    out = CG_FUNCTION_HANDLE_TAG | symbol;
                } else if (interp->data_memory[symbol] != nullptr) {
                    out = (uint64_t)(uintptr_t)interp->data_memory[symbol];
                } else {
                    cg_interp_fail(interp, "address of an unresolved symbol");
                }
                break;
            }
            case CG_OP_ALLOCA: {
                uint8_t *memory = calloc((size_t)inst->imm + 64 + ((size_t)1 << inst->a), 1);
                uint8_t **grown = realloc(frame_allocations,
                                          (allocation_count + 1) * sizeof(*grown));
                if (memory == nullptr || grown == nullptr) {
                    free(memory);
                    cg_interp_fail(interp, "out of memory");
                    break;
                }
                frame_allocations = grown;
                frame_allocations[allocation_count++] = memory;
                {
                    uint64_t align = (uint64_t)1 << (inst->a < 4 ? 4 : inst->a);
                    out = ((uint64_t)(uintptr_t)memory + align - 1) & ~(align - 1);
                }
                break;
            }
            case CG_OP_ADD:
                out = cg_mask(type, a + b);
                break;
            case CG_OP_SUB:
                out = cg_mask(type, a - b);
                break;
            case CG_OP_MUL:
                out = cg_mask(type, a * b);
                break;
            case CG_OP_SDIV:
            case CG_OP_SREM: {
                int64_t x = cg_sext(type, a);
                int64_t y = cg_sext(type, b);
                if (y == 0 || (y == -1 && x == INT64_MIN)) {
                    cg_interp_fail(interp, "division trap");
                    break;
                }
                out = cg_mask(type, (uint64_t)(inst->op == CG_OP_SDIV ? x / y
                                                                      : x % y));
                break;
            }
            case CG_OP_UDIV:
            case CG_OP_UREM:
                if (b == 0) {
                    cg_interp_fail(interp, "division trap");
                    break;
                }
                out = inst->op == CG_OP_UDIV ? a / b : a % b;
                break;
            case CG_OP_AND:
                out = a & b;
                break;
            case CG_OP_OR:
                out = a | b;
                break;
            case CG_OP_XOR:
                out = a ^ b;
                break;
            case CG_OP_SHL:
                out = cg_mask(type, a << (b & (cg_type_size(type) * 8 - 1)));
                break;
            case CG_OP_LSHR:
                out = a >> (b & (cg_type_size(type) * 8 - 1));
                break;
            case CG_OP_ASHR:
                out = cg_mask(type, (uint64_t)(cg_sext(type, a) >>
                                               (b & (cg_type_size(type) * 8 - 1))));
                break;
            case CG_OP_FADD:
            case CG_OP_FSUB:
            case CG_OP_FMUL:
            case CG_OP_FDIV: {
                double x = cg_to_double(type, a);
                double y = cg_to_double(type, b);
                double r;
                if (type == CG_T_F32) {
                    float fx = (float)x;
                    float fy = (float)y;
                    float fr = inst->op == CG_OP_FADD   ? fx + fy
                               : inst->op == CG_OP_FSUB ? fx - fy
                               : inst->op == CG_OP_FMUL ? fx * fy
                                                        : fx / fy;
                    r = fr;
                } else {
                    r = inst->op == CG_OP_FADD   ? x + y
                        : inst->op == CG_OP_FSUB ? x - y
                        : inst->op == CG_OP_FMUL ? x * y
                                                 : x / y;
                }
                out = cg_from_double(type, r);
                break;
            }
            case CG_OP_FNEG:
                out = type == CG_T_F32 ? (a ^ 0x80000000ull)
                                       : (a ^ 0x8000000000000000ull);
                break;
            case CG_OP_ICMP: {
                int64_t sx = cg_sext(operand_type, a);
                int64_t sy = cg_sext(operand_type, b);
                switch (inst->cc) {
                case CG_ICMP_EQ: out = a == b; break;
                case CG_ICMP_NE: out = a != b; break;
                case CG_ICMP_SLT: out = sx < sy; break;
                case CG_ICMP_SLE: out = sx <= sy; break;
                case CG_ICMP_SGT: out = sx > sy; break;
                case CG_ICMP_SGE: out = sx >= sy; break;
                case CG_ICMP_ULT: out = a < b; break;
                case CG_ICMP_ULE: out = a <= b; break;
                case CG_ICMP_UGT: out = a > b; break;
                default: out = a >= b; break;
                }
                break;
            }
            case CG_OP_FCMP: {
                double x = cg_to_double(operand_type, a);
                double y = cg_to_double(operand_type, b);
                bool unordered = x != x || y != y;
                switch (inst->cc) {
                case CG_FCMP_OEQ: out = !unordered && x == y; break;
                case CG_FCMP_ONE: out = !unordered && x != y; break;
                case CG_FCMP_UNE: out = unordered || x != y; break;
                case CG_FCMP_OLT: out = !unordered && x < y; break;
                case CG_FCMP_OLE: out = !unordered && x <= y; break;
                case CG_FCMP_OGT: out = !unordered && x > y; break;
                case CG_FCMP_OGE: out = !unordered && x >= y; break;
                case CG_FCMP_ORD: out = !unordered; break;
                default: out = unordered; break;
                }
                break;
            }
            case CG_OP_SELECT:
                out = (a & 1) ? b : values[inst->c];
                break;
            case CG_OP_ZEXT:
                out = a;
                break;
            case CG_OP_SEXT:
                out = cg_mask(type, (uint64_t)cg_sext(operand_type, a));
                break;
            case CG_OP_TRUNC:
                out = cg_mask(type, a);
                break;
            case CG_OP_SITOFP:
                out = cg_from_double(
                    type, type == CG_T_F32
                              ? (double)(float)cg_sext(operand_type, a)
                              : (double)cg_sext(operand_type, a));
                break;
            case CG_OP_UITOFP:
                out = cg_from_double(type, type == CG_T_F32 ? (double)(float)a
                                                            : (double)a);
                break;
            case CG_OP_FPTOSI:
            case CG_OP_FPTOUI:
                out = cg_mask(type, (uint64_t)(int64_t)cg_to_double(operand_type, a));
                break;
            case CG_OP_FPEXT:
                out = cg_from_double(CG_T_F64, cg_to_double(CG_T_F32, a));
                break;
            case CG_OP_FPTRUNC:
                out = cg_from_double(CG_T_F32, cg_to_double(CG_T_F64, a));
                break;
            case CG_OP_BITCAST:
                out = cg_mask(type, a);
                break;
            case CG_OP_LOAD:
                out = cg_load_memory(type, (const uint8_t *)(uintptr_t)(a + (uint64_t)inst->imm));
                break;
            case CG_OP_STORE:
                cg_store_memory(type, (uint8_t *)(uintptr_t)(a + (uint64_t)inst->imm), b);
                break;
            case CG_OP_MEMCPY:
                memmove((void *)(uintptr_t)a, (const void *)(uintptr_t)b,
                        (size_t)inst->imm);
                break;
            case CG_OP_MEMSET:
                memset((void *)(uintptr_t)a, (int)(b & 0xFF), (size_t)inst->imm);
                break;
            case CG_OP_CALL: {
                uint64_t call_args[64];
                cg_symbol_t symbol;
                uint64_t target;
                if (inst->c > 64) {
                    cg_interp_fail(interp, "too many call arguments");
                    break;
                }
                for (uint32_t i = 0; i < inst->c; ++i) {
                    call_args[i] = values[func->operands.data[inst->b + i]];
                }
                if (inst->imm >= 0) {
                    symbol = (cg_symbol_t)inst->imm;
                } else {
                    target = a;
                    if ((target & 0xFFFF000000000000ull) != CG_FUNCTION_HANDLE_TAG) {
                        cg_interp_fail(interp, "indirect call to a non-function");
                        break;
                    }
                    symbol = (cg_symbol_t)(target & 0xFFFFFFFFull);
                }
                if (!cg_interp_call_symbol(interp, symbol, call_args, inst->c,
                                           type, &out)) {
                    break;
                }
                out = cg_mask(type, out);
                break;
            }
            case CG_OP_BR:
                previous = block;
                block = inst->a;
                goto next_block;
            case CG_OP_CONDBR:
                previous = block;
                block = (a & 1) ? inst->b : inst->c;
                goto next_block;
            case CG_OP_RET:
                *result = inst->a == CG_INVALID ? 0 : a;
                ok = true;
                goto finish;
            case CG_OP_UNREACHABLE:
                cg_interp_fail(interp, "reached unreachable");
                break;
            case CG_OP_VAAREA:
                cg_interp_fail(interp, "variadic functions are not interpreted");
                break;
            case CG_OP_XCHG:
            case CG_OP_XADD:
            case CG_OP_CAS: {
                uint8_t *address = (uint8_t *)(uintptr_t)a;
                uint64_t old = cg_load_memory(type, address);
                uint64_t desired = inst->op == CG_OP_XCHG   ? b
                                   : inst->op == CG_OP_XADD ? cg_mask(type, old + b)
                                                            : values[inst->c];
                if (inst->op != CG_OP_CAS || old == b) {
                    cg_store_memory(type, address, desired);
                }
                out = old;
                break;
            }
            case CG_OP_FENCE:
                break;
            case CG_OP_STACKSAVE:
                out = allocation_count;
                break;
            case CG_OP_STACKRESTORE:
                while (allocation_count > a) {
                    free(frame_allocations[--allocation_count]);
                }
                break;
            case CG_OP_DYNALLOCA: {
                uint8_t *memory = calloc((size_t)a + 64, 1);
                uint8_t **grown = realloc(frame_allocations,
                                          (allocation_count + 1) * sizeof(*grown));
                if (memory == nullptr || grown == nullptr) {
                    free(memory);
                    cg_interp_fail(interp, "out of memory");
                    break;
                }
                frame_allocations = grown;
                frame_allocations[allocation_count++] = memory;
                out = ((uint64_t)(uintptr_t)memory + 15) & ~(uint64_t)15;
                break;
            }
            default:
                break;
            }
            if (defines && !interp->failed) {
                values[inst->dst] = out;
            }
        }
        if (interp->failed) {
            break;
        }
        cg_interp_fail(interp, "fell off the end of a block");
    next_block:;
    }
finish:
    for (uint32_t index = 0; index < allocation_count; ++index) {
        free(frame_allocations[index]);
    }
    free(frame_allocations);
    free(values);
    free(phi_temp);
    --interp->depth;
    return ok && !interp->failed;
}

bool cg_interp_run(const cg_module_t *module, cg_symbol_t function,
                   const uint64_t *args, uint32_t arg_count, uint64_t *result,
                   const cg_interp_host_t *host, cg_diag_t *diag) {
    cg_interp_t interp = {.module = module, .host = host, .diag = diag};
    const cg_func_t *func = cg_module_function_of(module, function);
    bool ok;
    uint64_t scratch = 0;
    if (func == nullptr) {
        cg_diag_error(diag, "interpreter: function has no body");
        return false;
    }
    interp.data_memory =
        calloc(module->symbols.count + 1, sizeof(uint8_t *));
    if (interp.data_memory == nullptr) {
        return false;
    }
    for (cg_symbol_t symbol = 0; symbol < module->symbols.count; ++symbol) {
        const cg_symbol_info_t *info = &module->symbols.data[symbol];
        if (info->kind == CG_SYM_DATA && info->defined) {
            interp.data_memory[symbol] = calloc((size_t)info->size + 16, 1);
            if (interp.data_memory[symbol] != nullptr &&
                info->section != CG_DATA_BSS && info->size != 0) {
                memcpy(interp.data_memory[symbol],
                       module->data.bytes.data + info->data_offset, info->size);
            }
        }
    }
    for (uint32_t index = 0; index < module->relocs.count; ++index) {
        const cg_data_reloc_t *reloc = &module->relocs.data[index];
        uint64_t address = 0;
        if (interp.data_memory[reloc->owner] == nullptr) {
            continue;
        }
        if (module->symbols.data[reloc->target].kind == CG_SYM_FUNC) {
            address = CG_FUNCTION_HANDLE_TAG | reloc->target;
        } else {
            address = (uint64_t)(uintptr_t)interp.data_memory[reloc->target];
        }
        address += (uint64_t)reloc->addend;
        memcpy(interp.data_memory[reloc->owner] + reloc->offset, &address, 8);
    }
    ok = cg_interp_function(&interp, func, args, arg_count,
                            result != nullptr ? result : &scratch);
    for (cg_symbol_t symbol = 0; symbol < module->symbols.count; ++symbol) {
        free(interp.data_memory[symbol]);
    }
    free(interp.data_memory);
    return ok;
}
