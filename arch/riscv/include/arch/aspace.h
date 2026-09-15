/*
 * Copyright (c) 2020 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

#include <lk/compiler.h>
#include <lk/list.h>
#include <arch/riscv/mmu.h>

__BEGIN_CDECLS

struct arch_aspace {
    int magic;

    // pointer to the translation table
    paddr_t pt_phys;
    volatile riscv_pte_t *pt_virt;

    uint flags;

    // list of page tables allocated for this aspace
    struct list_node pt_list;

    // range of address space
    vaddr_t base;
    size_t size;

    // satp asid this aspace runs under. kernel_asid() for the kernel aspace,
    // 0 for every user aspace when the cpu has too few asid bits to hand them out
    uint16_t asid;

    // cpus that currently have this aspace loaded, kept by arch_mmu_context_switch()
    volatile int active_cpus;
};

static inline int arch_aspace_active_cpus(const struct arch_aspace *aspace) {
    return __atomic_load_n(&aspace->active_cpus, __ATOMIC_RELAXED);
}

#define RISCV_ASPACE_MAGIC 'RVAS'

__END_CDECLS


