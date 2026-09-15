/*
 * Copyright (c) 2014 Google Inc. All rights reserved
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#include <lk/asm.h>

.text

/* Returns 0 on success, nonzero if the lock is held. A failed stxr is retried so
 * a lost race never reads as "held"; giving up clears the exclusive monitor. */
FUNCTION(arch_spin_trylock)
	mov	x2, x0
	mov	x1, #1
1:
	ldaxr	x0, [x2]
	cbnz	x0, 2f
	stxr	w0, x1, [x2]
	cbnz	w0, 1b
	ret
2:
	clrex
	ret

/* Wait for the lock with wfe while it is held. The ldaxr arms the exclusive
 * monitor, so the owner's release store generates the wake-up event. A failed
 * stxr means another PE just wrote the word: retry the load directly rather
 * than waiting for an event, since the monitor has already been lost and a
 * model that does not latch that as an event would sleep here until an
 * interrupt arrives (QEMU 11.1 does exactly that). */
FUNCTION(arch_spin_lock)
	mov	x1, #1
	sevl
1:
	wfe
2:
	ldaxr	x2, [x0]
	cbnz	x2, 1b
	stxr	w2, x1, [x0]
	cbnz	w2, 2b
	ret

FUNCTION(arch_spin_unlock)
	stlr	xzr, [x0]
	ret
