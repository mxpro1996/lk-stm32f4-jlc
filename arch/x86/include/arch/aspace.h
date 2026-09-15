/*
 * Copyright (c) 2016 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

#include <arch/x86/mmu.h>
#include <lk/compiler.h>
#include <sys/types.h>

__BEGIN_CDECLS

struct arch_aspace {
    /* pointer to the root page table */
    paddr_t cr3_phys;
    map_addr_t *cr3;

    uint flags;

    /* range of address space */
    vaddr_t base;
    size_t size;
};

/* not tracked on x86 */
static inline int arch_aspace_active_cpus(const struct arch_aspace *aspace) {
    return 0;
}

__END_CDECLS
