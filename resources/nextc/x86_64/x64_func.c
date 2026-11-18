#include "x64.h"

/* Machine function containers and the ABI tables (data, not code). */

#define N X64_ROLE_N
#define U X64_ROLE_U
#define D X64_ROLE_D
#define M X64_ROLE_M
static const x64_opinfo_t x64_opinfos[X64_OP_COUNT] = {
#define X(name, text, r0, r1, r2) [X64_##name] = {text, {r0, r1, r2}},
    X64_OPS(X)
#undef X
};
#undef N
#undef U
#undef D
#undef M

const x64_opinfo_t *x64_opinfo(x64_op_t op) { return &x64_opinfos[op]; }

#define GPR(r) ((uint32_t)1 << (r))
#define XMM(n) ((uint32_t)1 << (X64_XMM0 + (n)))

const x64_abi_t x64_abi_win64 = {
    .name = "win64",
    .int_args = {X64_RCX, X64_RDX, X64_R8, X64_R9},
    .int_arg_count = 4,
    .float_args = {X64_XMM0, X64_XMM0 + 1, X64_XMM0 + 2, X64_XMM0 + 3},
    .float_arg_count = 4,
    .shared_positions = true,
    .variadic_floats_in_int = true,
    .variadic_sets_al = false,
    .shadow_space = 32,
    .callee_saved = GPR(X64_RBX) | GPR(X64_RSI) | GPR(X64_RDI) | GPR(X64_R12) |
                    GPR(X64_R13) | GPR(X64_R14) | GPR(X64_R15) | XMM(6) |
                    XMM(7) | XMM(8) | XMM(9) | XMM(10) | XMM(11) | XMM(12) |
                    XMM(13) | XMM(14) | XMM(15),
    .allocatable_gpr = GPR(X64_RAX) | GPR(X64_RCX) | GPR(X64_RDX) |
                       GPR(X64_RBX) | GPR(X64_RSI) | GPR(X64_RDI) | GPR(X64_R8) |
                       GPR(X64_R9) | GPR(X64_R12) | GPR(X64_R13) | GPR(X64_R14) |
                       GPR(X64_R15),
    .allocatable_xmm = 0x3FFF, /* xmm0..xmm13 */
};

const x64_abi_t x64_abi_sysv = {
    .name = "sysv_amd64",
    .int_args = {X64_RDI, X64_RSI, X64_RDX, X64_RCX, X64_R8, X64_R9},
    .int_arg_count = 6,
    .float_args = {X64_XMM0, X64_XMM0 + 1, X64_XMM0 + 2, X64_XMM0 + 3,
                   X64_XMM0 + 4, X64_XMM0 + 5, X64_XMM0 + 6, X64_XMM0 + 7},
    .float_arg_count = 8,
    .shared_positions = false,
    .variadic_floats_in_int = false,
    .variadic_sets_al = true,
    .shadow_space = 0,
    .callee_saved = GPR(X64_RBX) | GPR(X64_R12) | GPR(X64_R13) | GPR(X64_R14) |
                    GPR(X64_R15),
    .allocatable_gpr = GPR(X64_RAX) | GPR(X64_RCX) | GPR(X64_RDX) |
                       GPR(X64_RBX) | GPR(X64_RSI) | GPR(X64_RDI) | GPR(X64_R8) |
                       GPR(X64_R9) | GPR(X64_R12) | GPR(X64_R13) | GPR(X64_R14) |
                       GPR(X64_R15),
    .allocatable_xmm = 0x3FFF,
};

uint32_t x64_abi_assign(const x64_abi_t *abi, const cg_type_t *types,
                        uint32_t count, bool variadic_call,
                        x64_arg_loc_t *locations) {
    uint32_t int_used = 0;
    uint32_t float_used = 0;
    uint32_t stack = 0;
    for (uint32_t index = 0; index < count; ++index) {
        bool is_float = cg_type_is_float(types[index]);
        x64_arg_loc_t *location = &locations[index];
        uint32_t slot = abi->shared_positions ? index
                                              : (is_float ? float_used : int_used);
        uint32_t limit = is_float ? abi->float_arg_count : abi->int_arg_count;
        location->reg = -1;
        location->mirror_reg = -1;
        location->stack_offset = 0;
        if (slot < limit) {
            location->reg = is_float ? abi->float_args[slot] : abi->int_args[slot];
            if (is_float && variadic_call && abi->variadic_floats_in_int &&
                slot < abi->int_arg_count) {
                location->mirror_reg = abi->int_args[slot];
            }
            if (is_float) {
                ++float_used;
            } else {
                ++int_used;
            }
        } else {
            location->stack_offset = stack;
            stack += 8;
            if (!abi->shared_positions) {
                /* counters stay saturated */
            }
        }
    }
    return stack;
}

void x64_func_init(x64_func_t *func, cg_symbol_t symbol, const x64_abi_t *abi) {
    memset(func, 0, sizeof(*func));
    func->symbol = symbol;
    func->abi = abi;
}

void x64_func_free(x64_func_t *func) {
    for (uint32_t index = 0; index < func->blocks.count; ++index) {
        cg_vec_free(&func->blocks.data[index].insts);
    }
    cg_vec_free(&func->blocks);
    cg_vec_free(&func->layout);
    cg_vec_free(&func->vregs);
    cg_vec_free(&func->slots);
}

bool x64_func_ok(const x64_func_t *func) {
    if (func->oom || func->blocks.oom || func->layout.oom || func->vregs.oom ||
        func->slots.oom) {
        return false;
    }
    for (uint32_t index = 0; index < func->blocks.count; ++index) {
        if (func->blocks.data[index].insts.oom) {
            return false;
        }
    }
    return true;
}

uint32_t x64_new_block(x64_func_t *func) {
    x64_block_t block = {};
    uint32_t id = func->blocks.count;
    return cg_vec_push(&func->blocks, block) ? id : CG_INVALID;
}

uint32_t x64_new_vreg(x64_func_t *func, bool is_xmm) {
    x64_vreg_t vreg = {.is_xmm = is_xmm, .assigned = -1, .spill_slot = -1,
                       .hint = X64_NOREG};
    uint32_t id = func->vregs.count;
    return cg_vec_push(&func->vregs, vreg) ? X64_VREG_BASE + id : CG_INVALID;
}

uint32_t x64_new_slot(x64_func_t *func, uint32_t size, uint32_t align) {
    x64_slot_t slot = {.size = size, .align = align};
    uint32_t id = func->slots.count;
    return cg_vec_push(&func->slots, slot) ? id : CG_INVALID;
}
