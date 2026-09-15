/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

#include <kernel/spinlock.h>
#include <lk/compiler.h>
#include <stdint.h>
#include <sys/types.h>

__BEGIN_CDECLS

/* Address space identifiers, as tagged into the TLB by the arch mmu code.
 * 0 is never handed out, the kernel aspace uses 1 and user aspaces are
 * allocated from 2 upwards. */
#define ASID_UNUSED     0
#define ASID_KERNEL     1
#define ASID_FIRST_USER 2

/* Widest id space any arch has; the real ceiling is set per allocator at init. */
#define ASID_MAX        UINT16_MAX

#define ASID_BITMAP_WORDS (((size_t)ASID_MAX + 1) / (sizeof(ulong) * 8))

/* Bitmap allocator for user asids. Safe to call from any context: the lock is a
 * spinlock and nothing here blocks. Zero the struct or call the init before use. */
typedef struct asid_allocator {
    spin_lock_t lock;
    uint16_t last;  /* the next search starts after this, so a freed id is not reused at once */
    uint16_t max;   /* highest id handed out, inclusive */
    ulong bitmap[ASID_BITMAP_WORDS];
} asid_allocator_t;

/* max_asid is the highest id the hardware implements, e.g. 0xff or 0xffff. */
void asid_allocator_init(asid_allocator_t *a, uint16_t max_asid);

/* Hand out an unused id in [ASID_FIRST_USER, max]; ERR_NO_MEMORY once all are taken. */
status_t asid_alloc(asid_allocator_t *a, uint16_t *asid);

/* Return an id from asid_alloc(). */
void asid_free(asid_allocator_t *a, uint16_t asid);

__END_CDECLS
