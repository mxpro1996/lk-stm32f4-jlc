/*
 * Copyright (c) 2025 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

#include <arch/m68k/mmu.h>

#include <lk/compiler.h>
#include <sys/types.h>

__BEGIN_CDECLS

struct arch_aspace {
    /* virtual address range covered by this address space */
    vaddr_t base;
    size_t size;

    /* pointer to the translation table */
    paddr_t pgtable_phys;
    void *pgtable_virt;

    uint32_t flags;
};

/* not tracked on m68k */
static inline int arch_aspace_active_cpus(const struct arch_aspace *aspace) {
    return 0;
}

__END_CDECLS
