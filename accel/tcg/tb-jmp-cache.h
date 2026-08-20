/*
 * The per-CPU TranslationBlock jump cache.
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ACCEL_TCG_TB_JMP_CACHE_H
#define ACCEL_TCG_TB_JMP_CACHE_H

#include "qemu/rcu.h"
#include "exec/cpu-common.h"

/* 12 bits is measured, not inherited. helper_lookup_tb_ptr is the single
 * largest cost on the saturated vCPU thread - 20.5% of it on Morrowind - so
 * this looks like an obvious lever, and it is not:
 *
 *   15 bits: regressed everything (p50 +7%, 1%-low -11%); 32k entries thrash
 *            a Cortex-A76's 512 KB L2 where 4k stay hot.
 *   13 bits: two paired runs, entirely within noise (p50 +0% / -4%,
 *            fps +2% / +7%, no consistent direction).
 *
 * Capacity is not the problem; the call itself is. Reducing how often guest
 * code leaves a block without a direct jump is the only thing that would move
 * it, and that is a property of the guest, not of this table. */
#define TB_JMP_CACHE_BITS 12
#define TB_JMP_CACHE_SIZE (1 << TB_JMP_CACHE_BITS)

/*
 * Invalidated in parallel; all accesses to 'tb' must be atomic.
 * A valid entry is read/written by a single CPU, therefore there is
 * no need for qatomic_rcu_read() and pc is always consistent with a
 * non-NULL value of 'tb'.  Strictly speaking pc is only needed for
 * CF_PCREL, but it's used always for simplicity.
 */
typedef struct CPUJumpCache {
    struct rcu_head rcu;
    struct {
        TranslationBlock *tb;
        vaddr pc;
    } array[TB_JMP_CACHE_SIZE];
} CPUJumpCache;

#endif /* ACCEL_TCG_TB_JMP_CACHE_H */
