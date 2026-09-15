/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

#include <kernel/vm/asid.h>

#include <assert.h>
#include <lk/err.h>
#include <lk/trace.h>
#include <string.h>

#define LOCAL_TRACE 0

#define BITS_PER_WORD (sizeof(ulong) * 8)

static inline bool asid_is_set(const asid_allocator_t *a, uint asid) {
    return (a->bitmap[asid / BITS_PER_WORD] >> (asid % BITS_PER_WORD)) & 1;
}

static inline void asid_set(asid_allocator_t *a, uint asid) {
    a->bitmap[asid / BITS_PER_WORD] |= 1UL << (asid % BITS_PER_WORD);
}

static inline void asid_clear(asid_allocator_t *a, uint asid) {
    a->bitmap[asid / BITS_PER_WORD] &= ~(1UL << (asid % BITS_PER_WORD));
}

/* lowest clear bit in [start, end], or -1 */
static int find_free(const asid_allocator_t *a, uint start, uint end) {
    uint i = start;
    while (i <= end) {
        ulong free = ~a->bitmap[i / BITS_PER_WORD] >> (i % BITS_PER_WORD);
        if (free) {
            uint found = i + __builtin_ctzl(free);
            return (found <= end) ? (int)found : -1;
        }
        /* whole rest of the word is taken, move to the next one */
        i = (i / BITS_PER_WORD + 1) * BITS_PER_WORD;
    }
    return -1;
}

void asid_allocator_init(asid_allocator_t *a, uint16_t max_asid) {
    DEBUG_ASSERT(max_asid >= ASID_FIRST_USER);

    spin_lock_init(&a->lock);
    a->last = ASID_FIRST_USER - 1;
    a->max = max_asid;
    memset(a->bitmap, 0, sizeof(a->bitmap));
}

status_t asid_alloc(asid_allocator_t *a, uint16_t *asid) {
    DEBUG_ASSERT(a->max >= ASID_FIRST_USER);

    arch_interrupt_saved_state_t state = spin_lock_irqsave(&a->lock);

    /* continue from the last id handed out and wrap around once */
    int found = find_free(a, a->last + 1, a->max);
    if (found < 0) {
        found = find_free(a, ASID_FIRST_USER, a->last);
    }
    if (found < 0) {
        spin_unlock_irqrestore(&a->lock, state);
        return ERR_NO_MEMORY;
    }

    asid_set(a, found);
    a->last = found;

    spin_unlock_irqrestore(&a->lock, state);

    LTRACEF("asid %#x\n", found);
    *asid = found;
    return NO_ERROR;
}

void asid_free(asid_allocator_t *a, uint16_t asid) {
    LTRACEF("asid %#x\n", asid);

    DEBUG_ASSERT(asid >= ASID_FIRST_USER && asid <= a->max);

    arch_interrupt_saved_state_t state = spin_lock_irqsave(&a->lock);

    DEBUG_ASSERT(asid_is_set(a, asid));
    asid_clear(a, asid);

    spin_unlock_irqrestore(&a->lock, state);
}
