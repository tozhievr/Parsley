#include "cg_base.h"

#include <stdarg.h>
#include <stdio.h>

bool cg_vec_reserve_raw(void **data, uint32_t *capacity, uint32_t needed,
                        size_t element_size) {
    uint32_t grown;
    void *memory;
    if (needed <= *capacity) {
        return true;
    }
    grown = *capacity == 0 ? 8 : *capacity;
    while (grown < needed) {
        if (grown > UINT32_MAX / 2) {
            return false;
        }
        grown *= 2;
    }
    if ((size_t)grown > SIZE_MAX / element_size) {
        return false;
    }
    memory = realloc(*data, (size_t)grown * element_size);
    if (memory == nullptr) {
        return false;
    }
    *data = memory;
    *capacity = grown;
    return true;
}

void cg_buffer_free(cg_buffer_t *buffer) { cg_vec_free(&buffer->bytes); }

bool cg_buffer_u8(cg_buffer_t *buffer, uint8_t value) {
    return cg_vec_push(&buffer->bytes, value);
}

bool cg_buffer_u16(cg_buffer_t *buffer, uint16_t value) {
    bool first = cg_buffer_u8(buffer, (uint8_t)value);
    bool second = cg_buffer_u8(buffer, (uint8_t)(value >> 8));
    return first && second;
}

bool cg_buffer_u32(cg_buffer_t *buffer, uint32_t value) {
    bool first = cg_buffer_u16(buffer, (uint16_t)value);
    bool second = cg_buffer_u16(buffer, (uint16_t)(value >> 16));
    return first && second;
}

bool cg_buffer_u64(cg_buffer_t *buffer, uint64_t value) {
    bool first = cg_buffer_u32(buffer, (uint32_t)value);
    bool second = cg_buffer_u32(buffer, (uint32_t)(value >> 32));
    return first && second;
}

bool cg_buffer_bytes(cg_buffer_t *buffer, const void *data, size_t size) {
    uint32_t start;
    if (size == 0) {
        return true;
    }
    if (size > UINT32_MAX - buffer->bytes.count) {
        buffer->bytes.oom = true;
        return false;
    }
    start = cg_vec_extend(&buffer->bytes, (uint32_t)size);
    if (start == CG_INVALID) {
        return false;
    }
    memcpy(buffer->bytes.data + start, data, size);
    return true;
}

bool cg_buffer_zeros(cg_buffer_t *buffer, size_t size) {
    if (size == 0) {
        return true;
    }
    if (size > UINT32_MAX - buffer->bytes.count) {
        buffer->bytes.oom = true;
        return false;
    }
    return cg_vec_extend(&buffer->bytes, (uint32_t)size) != CG_INVALID;
}

bool cg_buffer_align(cg_buffer_t *buffer, uint32_t alignment) {
    uint32_t aligned = cg_align_up_u32(buffer->bytes.count, alignment);
    return cg_buffer_zeros(buffer, aligned - buffer->bytes.count);
}

void cg_buffer_patch32(cg_buffer_t *buffer, size_t offset, uint32_t value) {
    memcpy(buffer->bytes.data + offset, &value, 4);
}

bool cg_bitset_init(cg_bitset_t *set, uint32_t bit_count) {
    size_t words = ((size_t)bit_count + 63) / 64;
    set->bit_count = bit_count;
    set->words = calloc(words == 0 ? 1 : words, sizeof(uint64_t));
    return set->words != nullptr;
}

void cg_bitset_free(cg_bitset_t *set) {
    free(set->words);
    set->words = nullptr;
    set->bit_count = 0;
}

void cg_bitset_clear(cg_bitset_t *set) {
    size_t words = ((size_t)set->bit_count + 63) / 64;
    memset(set->words, 0, words * sizeof(uint64_t));
}

bool cg_bitset_union(cg_bitset_t *dst, const cg_bitset_t *src) {
    size_t words = ((size_t)dst->bit_count + 63) / 64;
    bool changed = false;
    for (size_t index = 0; index < words; ++index) {
        uint64_t merged = dst->words[index] | src->words[index];
        changed |= merged != dst->words[index];
        dst->words[index] = merged;
    }
    return changed;
}

bool cg_bitset_union_minus(cg_bitset_t *dst, const cg_bitset_t *src,
                           const cg_bitset_t *mask) {
    size_t words = ((size_t)dst->bit_count + 63) / 64;
    bool changed = false;
    for (size_t index = 0; index < words; ++index) {
        uint64_t merged =
            dst->words[index] | (src->words[index] & ~mask->words[index]);
        changed |= merged != dst->words[index];
        dst->words[index] = merged;
    }
    return changed;
}

static uint32_t cg_string_hash(const char *text, size_t length) {
    uint32_t hash = 2166136261u;
    for (size_t index = 0; index < length; ++index) {
        hash = (hash ^ (uint8_t)text[index]) * 16777619u;
    }
    return hash;
}

void cg_strings_free(cg_strings_t *strings) {
    for (uint32_t index = 0; index < strings->items.count; ++index) {
        free(strings->items.data[index]);
    }
    cg_vec_free(&strings->items);
    cg_vec_free(&strings->lengths);
    free(strings->table);
    strings->table = nullptr;
    strings->table_size = 0;
}

static bool cg_strings_rehash(cg_strings_t *strings, uint32_t size) {
    uint32_t *table = malloc((size_t)size * sizeof(uint32_t));
    if (table == nullptr) {
        return false;
    }
    memset(table, 0xFF, (size_t)size * sizeof(uint32_t));
    for (uint32_t id = 0; id < strings->items.count; ++id) {
        uint32_t slot = cg_string_hash(strings->items.data[id],
                                       strings->lengths.data[id]) &
                        (size - 1);
        while (table[slot] != CG_INVALID) {
            slot = (slot + 1) & (size - 1);
        }
        table[slot] = id;
    }
    free(strings->table);
    strings->table = table;
    strings->table_size = size;
    return true;
}

uint32_t cg_strings_intern(cg_strings_t *strings, const char *text,
                           size_t length) {
    uint32_t slot;
    uint32_t id;
    char *copy;
    if (strings->table_size == 0 ||
        strings->items.count * 2 >= strings->table_size) {
        if (!cg_strings_rehash(
                strings,
                strings->table_size == 0 ? 64 : strings->table_size * 2)) {
            return CG_INVALID;
        }
    }
    slot = cg_string_hash(text, length) & (strings->table_size - 1);
    while ((id = strings->table[slot]) != CG_INVALID) {
        if (strings->lengths.data[id] == length &&
            memcmp(strings->items.data[id], text, length) == 0) {
            return id;
        }
        slot = (slot + 1) & (strings->table_size - 1);
    }
    copy = malloc(length + 1);
    if (copy == nullptr) {
        return CG_INVALID;
    }
    memcpy(copy, text, length);
    copy[length] = '\0';
    id = strings->items.count;
    if (!cg_vec_push(&strings->items, copy)) {
        free(copy);
        return CG_INVALID;
    }
    if (!cg_vec_push(&strings->lengths, (uint32_t)length)) {
        --strings->items.count;
        free(copy);
        return CG_INVALID;
    }
    strings->table[slot] = id;
    return id;
}

void cg_diag_error(cg_diag_t *diag, const char *format, ...) {
    va_list arguments;
    int written;
    if (diag == nullptr) {
        return;
    }
    ++diag->error_count;
    if (diag->length + 2 >= sizeof(diag->text)) {
        return;
    }
    va_start(arguments, format);
    written = vsnprintf(diag->text + diag->length,
                        sizeof(diag->text) - diag->length - 2, format,
                        arguments);
    va_end(arguments);
    if (written > 0) {
        diag->length += (uint32_t)written;
        diag->text[diag->length++] = '\n';
        diag->text[diag->length] = '\0';
    }
}
