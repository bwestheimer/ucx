/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2018. ALL RIGHTS RESERVED.
 * Copyright (C) Tactical Computing Labs, LLC. 2022. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */


#ifndef UCM_BISTRO_BISTRO_H_
#define UCM_BISTRO_BISTRO_H_

#include <stdint.h>

#include <ucs/type/status.h>

typedef struct ucm_bistro_restore_point ucm_bistro_restore_point_t;

#if defined(__powerpc64__)
#  include "bistro_ppc64.h"
#elif defined(__aarch64__)
#  include "bistro_aarch64.h"
#elif defined(__x86_64__)
#  include "bistro_x86_64.h"
#elif defined(__riscv)
#  include "bistro_rv64.h"
#else
#  error "Unsupported architecture"
#endif


/* Maximal length of the code written by @ref ucm_bistro_patch */
#define UCM_BISTRO_PATCH_MAX_LEN 32


/* Pending patch of a single function in a @ref ucm_bistro_batch_t */
typedef struct {
    void                       *func_ptr;      /* Function to patch */
    const char                 *symbol;        /* Function name */
    size_t                     patch_len;      /* Patch length */
    void                       *orig_func;     /* Trampoline to original code */
    void                       **orig_func_p;  /* Where to set orig_func */
    ucm_bistro_restore_point_t *restore_point; /* Restore point, or NULL */
    ucm_bistro_restore_point_t **rp;           /* Where to set restore_point */
    uint8_t                    patch[UCM_BISTRO_PATCH_MAX_LEN]; /* Patch */
} ucm_bistro_batch_entry_t;


/* Set of patches applied together under a single grace period */
typedef struct {
    ucm_bistro_batch_entry_t *entries;   /* Caller-provided array */
    unsigned                 count;      /* Number of pending patches */
    unsigned                 max_count;  /* Size of the entries array */
} ucm_bistro_batch_t;


/* Context for copying code to another location while preserving its previous
   functionality */
typedef struct {
    const void *src_p;   /* Pointer to current source instruction */
    const void *src_end; /* Upper limit for source instructions */
    void       *dst_p;   /* Pointer to current destination instruction */
    void       *dst_end; /* Upper limit for destination instructions */
} ucm_bistro_relocate_context_t;


/**
 * Initialize an empty patch batch.
 *
 * @param batch      Batch to initialize.
 * @param entries    Array to hold the pending patches.
 * @param max_count  Number of elements in @a entries.
 */
void ucm_bistro_batch_init(ucm_bistro_batch_t *batch,
                           ucm_bistro_batch_entry_t *entries,
                           unsigned max_count);


/**
 * Prepare a patch like @ref ucm_bistro_patch does, without modifying the code
 * of @a func_ptr. The patch is applied by @ref ucm_bistro_batch_commit, and
 * @a orig_func_p and @a rp are set only when it succeeds. On architectures
 * without a patch lock (ppc64) the patch is applied immediately.
 *
 * @param batch        Batch to add the patch to.
 * @param func_ptr     Pointer to function to patch.
 * @param hook         User-defined function-replacer.
 * @param symbol       Function name, must remain valid until commit.
 * @param orig_func_p  If non-NULL, set to a trampoline which calls the
 *                     original function, as in @ref ucm_bistro_patch.
 * @param rp           Restore point used to restore original function.
 *                     Optional, may be NULL.
 *
 * @return UCS_ERR_ALREADY_EXISTS if the patch overlaps a pending one,
 *         UCS_ERR_EXCEEDS_LIMIT if the batch is full, or an error returned by
 *         @ref ucm_bistro_patch.
 */
ucs_status_t ucm_bistro_batch_add(ucm_bistro_batch_t *batch, void *func_ptr,
                                  void *hook, const char *symbol,
                                  void **orig_func_p,
                                  ucm_bistro_restore_point_t **rp);


/**
 * Apply all pending patches of the batch: lock every patched function, wait
 * one grace period, then copy the patches and unlock. If making any function
 * writable fails, no code is modified and the batch is cancelled. If restoring
 * the protection fails afterwards, the error is returned but the patches are
 * applied and published, so the caller must not add them again. The batch is
 * empty when this function returns.
 *
 * @param batch  Batch to commit.
 *
 * @return Error code as defined by @ref ucs_status_t
 */
ucs_status_t ucm_bistro_batch_commit(ucm_bistro_batch_t *batch);


/**
 * Drop all pending patches of the batch without modifying any code.
 *
 * @param batch  Batch to cancel.
 */
void ucm_bistro_batch_cancel(ucm_bistro_batch_t *batch);


/**
 * @return Nonzero if @ref ucm_bistro_batch_add defers patches to commit, or
 *         zero if it applies them immediately (no patch lock).
 */
int ucm_bistro_batch_is_deferred();


/**
 * Restore original function body using restore point created
 * by @ref ucm_bistro_patch
 *
 * @param rp     restore point, is removed after success operation
 *               completed
 *
 * @return Error code as defined by @ref ucs_status_t
 */
ucs_status_t ucm_bistro_restore(ucm_bistro_restore_point_t *rp);


/**
 * Remove restore point created by @ref ucm_bistro_patch without
 * restore original function body
 *
 * @param rp     restore point
 *
 * @return Error code as defined by @ref ucs_status_t
 */
ucs_status_t ucm_bistro_remove_restore_point(ucm_bistro_restore_point_t *rp);


/**
 * Get patch address for restore point
 *
 * @param rp     restore point
 *
 * @return Address of patched function body
 */
void *ucm_bistro_restore_addr(ucm_bistro_restore_point_t *rp);


/**
 * Allocate executable memory which can be used to create trampolines or
 * temporary functions.
 *
 * @param size   Memory size to allocated
 *
 * @return Pointer to allocated memory, or NULL if failed.
 */
void *ucm_bistro_allocate_code(size_t size);


/**
 * Relocate a single instruction to new address. The implementation is
 * architecture-specific.
 *
 * @param ctx Instruction relocation context
 *
 * @return ucs_status_t UCS_OK if successful, otherwise an error code.
 */
ucs_status_t ucm_bistro_relocate_one(ucm_bistro_relocate_context_t *ctx);


/*
 * Relocate at least 'min_src_length' code instructions from 'src' to 'dst',
 * possibly changing some of them to new instructions.
 * Uses a  simplified disassembler which supports only typical instructions
 * found in function prologue.
 */
ucs_status_t
ucm_bistro_relocate_code(void *dst, const void *src, size_t min_src_length,
                         size_t max_dst_length, size_t *dst_length_p,
                         size_t *src_length_p, const char *symbol,
                         ucm_bistro_relocate_context_t *ctx);

#endif
