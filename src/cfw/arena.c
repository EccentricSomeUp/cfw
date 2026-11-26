/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/arena.h>

#include <cfw/checked_arithmetic.h>
#include <cfw/max_align.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CFW_ARENA_DEFAULT_BLOCK_SIZE ((size_t)65536)
#define CFW_ARENA_ALIGN _Alignof(cfw_max_align_t)
#define CFW_ARENA_ALIGN_MASK (CFW_ARENA_ALIGN - 1)

_Static_assert((CFW_ARENA_ALIGN & CFW_ARENA_ALIGN_MASK) == 0,
               "arena alignment must be a power of two");

typedef struct cfw_arena_block {
    struct cfw_arena_block *next;
    size_t capacity;
    size_t used;
} cfw_arena_block_t;

struct cfw_arena {
    cfw_arena_block_t *blocks;
    size_t block_size;
};

static size_t cfw_arena_data_offset(void)
{
    return (sizeof(cfw_arena_block_t) + CFW_ARENA_ALIGN_MASK) &
           ~(size_t)CFW_ARENA_ALIGN_MASK;
}

static void *cfw_arena_block_data(cfw_arena_block_t *block)
{
    return (uint8_t *)block + cfw_arena_data_offset();
}

static cfw_arena_block_t *cfw_arena_new_block(size_t capacity)
{
    size_t total;
    cfw_arena_block_t *block;
    cfw_status_t status;

    status = cfw_checked_add_size(cfw_arena_data_offset(), capacity, &total);
    if (status != CFW_OK) {
        return NULL;
    }

    block = (cfw_arena_block_t *)malloc(total);
    if (block == NULL) {
        return NULL;
    }

    block->next = NULL;
    block->capacity = capacity;
    block->used = 0;
    return block;
}

static size_t cfw_arena_align_up(size_t size, int *overflow)
{
    size_t padded;

    *overflow = 0;
    if (size > SIZE_MAX - CFW_ARENA_ALIGN_MASK) {
        *overflow = 1;
        return 0;
    }
    padded = (size + CFW_ARENA_ALIGN_MASK) & ~(size_t)CFW_ARENA_ALIGN_MASK;
    return padded;
}

cfw_arena_t *cfw_arena_create(size_t block_size)
{
    cfw_arena_t *arena;

    if (block_size == 0) {
        block_size = CFW_ARENA_DEFAULT_BLOCK_SIZE;
    }

    arena = (cfw_arena_t *)malloc(sizeof(*arena));
    if (arena == NULL) {
        return NULL;
    }

    arena->blocks = NULL;
    arena->block_size = block_size;
    return arena;
}

void cfw_arena_destroy(cfw_arena_t *arena)
{
    cfw_arena_block_t *block;
    cfw_arena_block_t *next;

    if (arena == NULL) {
        return;
    }

    block = arena->blocks;
    while (block != NULL) {
        next = block->next;
        free(block);
        block = next;
    }
    free(arena);
}

cfw_status_t cfw_arena_alloc(cfw_arena_t *arena, size_t size, void **out)
{
    cfw_arena_block_t *block;
    size_t aligned;
    size_t capacity;
    int overflow;

    if (arena == NULL || out == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    aligned = cfw_arena_align_up(size, &overflow);
    if (overflow) {
        return CFW_ERROR_OVERFLOW;
    }

    block = arena->blocks;
    if (block == NULL || block->capacity - block->used < aligned) {
        if (aligned > arena->block_size) {
            capacity = aligned;
        } else {
            capacity = arena->block_size;
        }

        block = cfw_arena_new_block(capacity);
        if (block == NULL) {
            return CFW_ERROR_NOMEM;
        }
        block->next = arena->blocks;
        arena->blocks = block;
    }

    *out = (uint8_t *)cfw_arena_block_data(block) + block->used;
    block->used += aligned;
    return CFW_OK;
}

cfw_status_t cfw_arena_alloc_zeroed(cfw_arena_t *arena, size_t size,
                                    void **out)
{
    cfw_status_t status;

    status = cfw_arena_alloc(arena, size, out);
    if (status != CFW_OK) {
        return status;
    }

    memset(*out, 0, size);
    return CFW_OK;
}

void cfw_arena_reset(cfw_arena_t *arena)
{
    cfw_arena_block_t *block;

    if (arena == NULL) {
        return;
    }

    for (block = arena->blocks; block != NULL; block = block->next) {
        block->used = 0;
    }
}
