/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

#include <lk/compiler.h>
#include <lk/list.h>
#include <stddef.h>

__BEGIN_CDECLS

// List helpers compiled as C, so the list tests can hand a list across the language
// boundary in both directions.
size_t list_c_interop_length(struct list_node *head);
void list_c_interop_reverse(struct list_node *head);
void list_c_interop_append(struct list_node *head, struct list_node *node);
struct list_node *list_c_interop_pop_head(struct list_node *head);

__END_CDECLS
