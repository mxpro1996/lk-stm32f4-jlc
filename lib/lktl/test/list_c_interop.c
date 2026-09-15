/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#include "list_c_interop.h"

#include <stddef.h>

size_t list_c_interop_length(struct list_node *head) {
    return list_length(head);
}

// Pops every node and pushes it at the head of a scratch list, which reverses the order,
// then moves them back.
void list_c_interop_reverse(struct list_node *head) {
    struct list_node scratch = LIST_INITIAL_VALUE(scratch);
    struct list_node *node;

    while ((node = list_remove_head(head)) != NULL) {
        list_add_head(&scratch, node);
    }
    while ((node = list_remove_head(&scratch)) != NULL) {
        list_add_tail(head, node);
    }
}

void list_c_interop_append(struct list_node *head, struct list_node *node) {
    list_add_tail(head, node);
}

struct list_node *list_c_interop_pop_head(struct list_node *head) {
    return list_remove_head(head);
}
