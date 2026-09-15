/*
 * Copyright (c) 2008-2014 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

#ifndef ASSEMBLY

#include <lk/compiler.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

__BEGIN_CDECLS

/* Fast routines that all arches must implement inline in arch_ops.h. */
static inline ulong arch_cycle_count(void);
static inline uint arch_curr_cpu_num(void);

/* Use to align structures on cache lines to avoid cpu aliasing. */
#define __CPU_ALIGN __ALIGNED(CACHE_LINE)

void arch_disable_cache(uint flags);
void arch_enable_cache(uint flags);

void arch_clean_cache_range(addr_t start, size_t len);
void arch_clean_invalidate_cache_range(addr_t start, size_t len);
void arch_invalidate_cache_range(addr_t start, size_t len);
void arch_sync_cache_range(addr_t start, size_t len);

void arch_idle(void);

__END_CDECLS

#endif // !ASSEMBLY

/* for the above arch enable/disable routines */
#define ARCH_CACHE_FLAG_ICACHE 1
#define ARCH_CACHE_FLAG_DCACHE 2
#define ARCH_CACHE_FLAG_UCACHE (ARCH_CACHE_FLAG_ICACHE | ARCH_CACHE_FLAG_DCACHE)

/* include the arch specific implementations */
/* TODO: untangle the mutual include between this header and every arch_ops.h,
 * which includes this file back at its top. Anything here that depends on what
 * the arch defined, like the default below, has to be gated on a macro the arch
 * sets before that include, so a generic overridable default cannot simply
 * follow the include. */
#include <arch/arch_ops.h>

/* Hint that the caller is spinning waiting on another cpu. An arch with a
 * pause or yield instruction defines its own in arch_ops.h and sets
 * ARCH_HAS_SPINLOOP_PAUSE before including this file; everyone else gets this
 * empty one. */
#if !defined(ASSEMBLY) && !defined(ARCH_HAS_SPINLOOP_PAUSE)
static inline void arch_spinloop_pause(void) {}
#endif
