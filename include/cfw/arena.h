/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_ARENA_H
#define CFW_ARENA_H

#include <cfw/status.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * cfw_arena_t is an opaque bump allocator. Allocations are carved from large
 * internally managed blocks and are never individually freed; all storage is
 * released by cfw_arena_destroy or reclaimed for reuse by cfw_arena_reset.
 *
 * Every allocation is aligned to the maximum fundamental alignment
 * (cfw_max_align_t in cfw/max_align.h). There is no supported
 * way to free or realloc an individual allocation; design call sites so
 * arena lifetimes match request/parse lifetimes.
 *
 * The arena is not thread-safe; concurrent use requires external
 * synchronization.
 */
typedef struct cfw_arena cfw_arena_t;

CFW_API cfw_arena_t *cfw_arena_create(size_t block_size);
CFW_API void cfw_arena_destroy(cfw_arena_t *arena);
CFW_API cfw_status_t cfw_arena_alloc(cfw_arena_t *arena, size_t size,
                                     void **out);
CFW_API cfw_status_t cfw_arena_alloc_zeroed(cfw_arena_t *arena, size_t size,
                                            void **out);
CFW_API void cfw_arena_reset(cfw_arena_t *arena);

#ifdef __cplusplus
}
#endif

#endif /* CFW_ARENA_H */
