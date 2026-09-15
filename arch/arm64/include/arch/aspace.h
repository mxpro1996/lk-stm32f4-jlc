/*
 * Copyright (c) 2015-2016 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

#include <lk/compiler.h>
#include <lk/list.h>
#include <arch/arm64/mmu.h>

__BEGIN_CDECLS

struct arch_aspace {
    /* pointer to the translation table */
    paddr_t tt_phys;
    pte_t *tt_virt;

    uint flags;

    /* range of address space */
    vaddr_t base;
    size_t size;

    /* tag for this aspace's TLB entries; ASID_KERNEL for the kernel aspace, whose
     * entries are global and carry none */
    uint16_t asid;

    /* cpus that currently have this aspace loaded, kept by arch_mmu_context_switch() */
    volatile int active_cpus;
};

static inline int arch_aspace_active_cpus(const struct arch_aspace *aspace) {
    return __atomic_load_n(&aspace->active_cpus, __ATOMIC_RELAXED);
}

__END_CDECLS

