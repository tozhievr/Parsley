#pragma once

/* Foundation of the code generator: typed ids, growable vectors, bit sets,
 * a string interner and a byte buffer. Depends on nothing but libc23. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

constexpr uint32_t CG_INVALID = UINT32_MAX;

typedef uint32_t cg_value_t;  /* SSA value / virtual register index */
typedef uint32_t cg_block_t;  /* basic block index */
typedef uint32_t cg_inst_id_t; /* instruction index */
typedef uint32_t cg_symbol_t; /* module symbol index */

/* Vectors never abort: a failed allocation sets `oom` and drops the item, the
 * owner checks the flag at stage boundaries. */
#define CG_VEC(T)                                                              \
    struct {                                                                   \
        T *data;                                                               \
        uint32_t count;                                                        \
        uint32_t capacity;                                                     \
        bool oom;                                                              \
    }

bool cg_vec_reserve_raw(void **data, uint32_t *capacity, uint32_t needed,
                        size_t element_size);

#define cg_vec_reserve(vec, n)                                                 \
    cg_vec_reserve_raw((void **)&(vec)->data, &(vec)->capacity, (n),           \
                       sizeof(*(vec)->data))

#define cg_vec_push(vec, ...)                                                  \
    (cg_vec_reserve((vec), (vec)->count + 1)                                   \
         ? ((vec)->data[(vec)->count++] = (__VA_ARGS__), true)                 \
         : ((vec)->oom = true, false))

#define cg_vec_free(vec)                                                       \
    do {                                                                       \
        free((vec)->data);                                                     \
        (vec)->data = nullptr;                                                 \
        (vec)->count = 0;                                                      \
        (vec)->capacity = 0;                                                   \
    } while (0)

/* Appends `n` zeroed elements and returns the index of the first one, or
 * CG_INVALID on failure. */
#define cg_vec_extend(vec, n)                                                  \
    (cg_vec_reserve((vec), (vec)->count + (n))                                 \
         ? (memset((vec)->data + (vec)->count, 0,                              \
                   (size_t)(n) * sizeof(*(vec)->data)),                        \
            (vec)->count += (n), (vec)->count - (n))                           \
         : ((vec)->oom = true, CG_INVALID))

typedef CG_VEC(uint32_t) cg_u32_vec_t;
typedef CG_VEC(uint8_t) cg_byte_vec_t;

/* Append-only little-endian byte buffer. */
typedef struct cg_buffer {
    cg_byte_vec_t bytes;
} cg_buffer_t;

void cg_buffer_free(cg_buffer_t *buffer);
bool cg_buffer_u8(cg_buffer_t *buffer, uint8_t value);
bool cg_buffer_u16(cg_buffer_t *buffer, uint16_t value);
bool cg_buffer_u32(cg_buffer_t *buffer, uint32_t value);
bool cg_buffer_u64(cg_buffer_t *buffer, uint64_t value);
bool cg_buffer_bytes(cg_buffer_t *buffer, const void *data, size_t size);
bool cg_buffer_zeros(cg_buffer_t *buffer, size_t size);
bool cg_buffer_align(cg_buffer_t *buffer, uint32_t alignment);
void cg_buffer_patch32(cg_buffer_t *buffer, size_t offset, uint32_t value);
static inline size_t cg_buffer_size(const cg_buffer_t *buffer) {
    return buffer->bytes.count;
}

/* Dense bit set. */
typedef struct cg_bitset {
    uint64_t *words;
    uint32_t bit_count;
} cg_bitset_t;

bool cg_bitset_init(cg_bitset_t *set, uint32_t bit_count);
void cg_bitset_free(cg_bitset_t *set);
void cg_bitset_clear(cg_bitset_t *set);
static inline bool cg_bitset_test(const cg_bitset_t *set, uint32_t bit) {
    return (set->words[bit >> 6] >> (bit & 63)) & 1u;
}
static inline void cg_bitset_set(cg_bitset_t *set, uint32_t bit) {
    set->words[bit >> 6] |= (uint64_t)1 << (bit & 63);
}
static inline void cg_bitset_reset(cg_bitset_t *set, uint32_t bit) {
    set->words[bit >> 6] &= ~((uint64_t)1 << (bit & 63));
}
/* dst |= src; returns true if dst changed. */
bool cg_bitset_union(cg_bitset_t *dst, const cg_bitset_t *src);
/* dst |= (src & ~mask); returns true if dst changed. */
bool cg_bitset_union_minus(cg_bitset_t *dst, const cg_bitset_t *src,
                           const cg_bitset_t *mask);

/* Interned strings with stable, insertion-ordered ids. */
typedef struct cg_strings {
    CG_VEC(char *) items;
    CG_VEC(uint32_t) lengths;
    uint32_t *table;
    uint32_t table_size;
} cg_strings_t;

void cg_strings_free(cg_strings_t *strings);
uint32_t cg_strings_intern(cg_strings_t *strings, const char *text,
                           size_t length);
static inline const char *cg_strings_get(const cg_strings_t *strings,
                                         uint32_t id) {
    return strings->items.data[id];
}
static inline uint32_t cg_strings_length(const cg_strings_t *strings,
                                         uint32_t id) {
    return strings->lengths.data[id];
}

/* Diagnostics: a bounded text log, no allocation. */
typedef struct cg_diag {
    char text[1024];
    uint32_t length;
    uint32_t error_count;
} cg_diag_t;

void cg_diag_error(cg_diag_t *diag, const char *format, ...);

static inline uint32_t cg_align_up_u32(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}
static inline int64_t cg_align_up_i64(int64_t value, int64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}
