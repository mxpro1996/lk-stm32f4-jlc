/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

// Tests for the C++ intrusive list in <lktl/list.h>, including its interop with the C
// API in <lk/list.h>. Fixtures stay tiny: ut all runs on the shell thread's stack, which is
// 1KB on cortex-m.

#include <lktl/list.h>

#include <lib/unittest.h>
#include <lk/list.h>
#include <type_traits>

#include "list_c_interop.h"

namespace {

// Deliberately not standard-layout: containerof on it would be undefined behavior and warns
// under -Winvalid-offsetof, which is what the base hook exists to avoid.
class widget : public lk::list_hook<> {
public:
    explicit widget(int value) : value_(value) {}
    virtual ~widget() = default;

    int value() const { return value_; }
    void set_value(int value) { value_ = value; }

private:
    int value_;
};
static_assert(!std::is_standard_layout<widget>::value);

// on two lists at once, one hook per tag
struct tag_a {};
struct tag_b {};
struct dual_item : lk::list_hook<tag_a>, lk::list_hook<tag_b> {
    explicit dual_item(int i) : id(i) {}
    int id;
};
using list_a = lk::list<dual_item, lk::base_hook_traits<dual_item, tag_a>>;
using list_b = lk::list<dual_item, lk::base_hook_traits<dual_item, tag_b>>;

// a C style struct with an embedded node, reached through containerof
struct c_item {
    int value;
    struct list_node node;
};
LK_LIST_MEMBER_TRAITS(c_item_traits, c_item, node);

// a list at namespace scope is constant-initialized: no constructor runs for it
struct global_item : lk::list_hook<> {
    int value = 0;
};
lk::list<global_item> g_list;

static_assert(sizeof(lk::list<global_item>) == sizeof(list_node));
static_assert(sizeof(lk::list_hook<>) == sizeof(list_node));
static_assert(std::is_trivially_destructible<lk::list<global_item>>::value);
static_assert(!std::is_copy_constructible<lk::list<global_item>>::value);
static_assert(std::is_trivially_copyable<lk::list_view<global_item>>::value);
#if !DEBUG_ASSERT_IMPLEMENTED
static_assert(std::is_trivially_destructible<lk::list_hook<>>::value);
#endif

bool empty_list() {
    BEGIN_TEST;

    lk::list<widget> l;
    EXPECT_TRUE(l.is_empty());
    EXPECT_EQ(0u, l.size_slow());
    EXPECT_NULL(l.front());
    EXPECT_NULL(l.back());
    EXPECT_NULL(l.pop_front());
    EXPECT_NULL(l.pop_back());
    EXPECT_TRUE(l.begin() == l.end());
    EXPECT_TRUE(l.c_head()->next == l.c_head());
    EXPECT_TRUE(l.c_head()->prev == l.c_head());
    l.clear();
    EXPECT_TRUE(l.is_empty());

    END_TEST;
}

bool push_pop() {
    BEGIN_TEST;

    widget w[4] = { widget(0), widget(1), widget(2), widget(3) };
    lk::list<widget> l;

    EXPECT_FALSE(w[1].in_list());
    l.push_back(&w[1]);
    EXPECT_TRUE(w[1].in_list());
    l.push_back(&w[2]);
    l.push_front(&w[0]);
    l.push_back(&w[3]);
    EXPECT_FALSE(l.is_empty());
    EXPECT_EQ(4u, l.size_slow());
    EXPECT_TRUE(l.front() == &w[0]);
    EXPECT_TRUE(l.back() == &w[3]);

    EXPECT_TRUE(l.pop_front() == &w[0]);
    EXPECT_FALSE(w[0].in_list());
    EXPECT_TRUE(l.pop_back() == &w[3]);
    EXPECT_FALSE(w[3].in_list());
    EXPECT_TRUE(l.pop_front() == &w[1]);
    EXPECT_TRUE(l.pop_front() == &w[2]);
    EXPECT_NULL(l.pop_front());
    EXPECT_TRUE(l.is_empty());
    for (const widget &x : w) {
        EXPECT_FALSE(x.in_list());
    }

    END_TEST;
}

bool insert_remove() {
    BEGIN_TEST;

    widget w[4] = { widget(0), widget(1), widget(2), widget(3) };
    lk::list<widget> l;

    l.push_back(&w[0]);
    l.push_back(&w[3]);
    l.insert_after(&w[0], &w[1]);
    l.insert_before(&w[3], &w[2]);
    EXPECT_EQ(4u, l.size_slow());
    int expected = 0;
    for (const widget &x : l) {
        EXPECT_EQ(expected, x.value());
        expected++;
    }
    EXPECT_EQ(4, expected);

    // unlinking needs no head
    lk::list<widget>::remove(&w[1]);
    EXPECT_FALSE(w[1].in_list());
    l.remove(&w[2]);
    EXPECT_EQ(2u, l.size_slow());
    EXPECT_TRUE(l.front() == &w[0]);
    EXPECT_TRUE(l.back() == &w[3]);
    EXPECT_TRUE(l.next(&w[0]) == &w[3]);
    EXPECT_TRUE(l.prev(&w[3]) == &w[0]);

    l.clear();
    EXPECT_TRUE(l.is_empty());
    for (const widget &x : w) {
        EXPECT_FALSE(x.in_list());
    }

    END_TEST;
}

bool iteration() {
    BEGIN_TEST;

    widget w[3] = { widget(10), widget(20), widget(30) };
    lk::list<widget> l;
    for (widget &x : w) {
        l.push_back(&x);
    }

    // forward, mutating through the reference
    int sum = 0;
    for (widget &x : l) {
        sum += x.value();
        x.set_value(x.value() + 1);
    }
    EXPECT_EQ(60, sum);
    EXPECT_EQ(11, w[0].value());

    // a const list hands out const elements
    const lk::list<widget> &cl = l;
    static_assert(std::is_same<decltype(*cl.begin()), const widget &>::value);
    static_assert(std::is_same<decltype(*l.begin()), widget &>::value);
    sum = 0;
    for (const widget &x : cl) {
        sum += x.value();
    }
    EXPECT_EQ(63, sum);
    EXPECT_TRUE(cl.front() == &w[0]);
    EXPECT_TRUE(cl.back() == &w[2]);

    // an iterator converts to a const_iterator and compares with one
    lk::list<widget>::const_iterator cit = l.begin();
    EXPECT_TRUE(cit == cl.begin());
    EXPECT_TRUE(l.begin() == cl.begin());
    EXPECT_TRUE(cl.end() != l.begin());
    static_assert(!std::is_convertible_v<lk::list<widget>::const_iterator, lk::list<widget>::iterator>);

    // backwards from end()
    auto it = l.end();
    --it;
    EXPECT_TRUE(&*it == &w[2]);
    --it;
    EXPECT_EQ(21, it->value());
    it--;
    EXPECT_TRUE(it == l.begin());
    it++;
    EXPECT_TRUE(&*it == &w[1]);
    EXPECT_TRUE(l.make_iterator(&w[1]) == it);

    // the neighbors, and nothing past either end
    EXPECT_NULL(l.prev(&w[0]));
    EXPECT_TRUE(l.next(&w[0]) == &w[1]);
    EXPECT_TRUE(l.prev(&w[2]) == &w[1]);
    EXPECT_NULL(l.next(&w[2]));

    l.clear();

    END_TEST;
}

bool erase_during_iteration() {
    BEGIN_TEST;

    widget w[6] = { widget(0), widget(1), widget(2), widget(3), widget(4), widget(5) };
    lk::list<widget> l;
    for (widget &x : w) {
        l.push_back(&x);
    }

    // drop the odd values: the list_for_every_entry_safe idiom
    for (auto it = l.begin(); it != l.end();) {
        if (it->value() & 1) {
            it = l.erase(it);
        } else {
            ++it;
        }
    }
    EXPECT_EQ(3u, l.size_slow());
    int expected = 0;
    for (const widget &x : l) {
        EXPECT_EQ(expected, x.value());
        expected += 2;
    }
    EXPECT_FALSE(w[1].in_list());
    EXPECT_TRUE(w[2].in_list());

    l.clear();
    EXPECT_TRUE(l.is_empty());
    for (const widget &x : w) {
        EXPECT_FALSE(x.in_list());
    }

    END_TEST;
}

bool two_tags() {
    BEGIN_TEST;

    dual_item items[3] = { dual_item(0), dual_item(1), dual_item(2) };
    list_a a;
    list_b b;
    for (dual_item &i : items) {
        a.push_back(&i);
        b.push_front(&i);
    }
    EXPECT_EQ(3u, a.size_slow());
    EXPECT_EQ(3u, b.size_slow());
    EXPECT_TRUE(a.front() == &items[0]);
    EXPECT_TRUE(b.front() == &items[2]);

    // membership is per hook
    a.remove(&items[1]);
    EXPECT_FALSE(static_cast<lk::list_hook<tag_a> &>(items[1]).in_list());
    EXPECT_TRUE(static_cast<lk::list_hook<tag_b> &>(items[1]).in_list());
    EXPECT_EQ(2u, a.size_slow());
    EXPECT_EQ(3u, b.size_slow());
    int expected = 2;
    for (const dual_item &i : b) {
        EXPECT_EQ(expected, i.id);
        expected--;
    }

    a.clear();
    b.clear();
    for (const dual_item &i : items) {
        EXPECT_FALSE(static_cast<const lk::list_hook<tag_a> &>(i).in_list());
        EXPECT_FALSE(static_cast<const lk::list_hook<tag_b> &>(i).in_list());
    }

    END_TEST;
}

bool member_hook_traits() {
    BEGIN_TEST;

    c_item items[3] = {
        { 0, LIST_INITIAL_CLEARED_VALUE },
        { 1, LIST_INITIAL_CLEARED_VALUE },
        { 2, LIST_INITIAL_CLEARED_VALUE },
    };
    lk::list<c_item, c_item_traits> l;
    for (c_item &i : items) {
        l.push_back(&i);
    }
    EXPECT_EQ(3u, l.size_slow());

    // the C API sees the same list through the same head
    EXPECT_EQ(3u, list_length(l.c_head()));
    c_item *head_item = list_peek_head_type(l.c_head(), c_item, node);
    EXPECT_TRUE(head_item == &items[0]);
    int expected = 0;
    for (const c_item &i : l) {
        EXPECT_EQ(expected, i.value);
        expected++;
    }

    EXPECT_TRUE(l.pop_back() == &items[2]);
    EXPECT_FALSE(list_in_list(&items[2].node));
    l.clear();
    EXPECT_TRUE(l.is_empty());

    END_TEST;
}

bool list_view_over_c_head() {
    BEGIN_TEST;

    // a head that belongs to C code, filled with the C API
    struct list_node raw = LIST_INITIAL_VALUE(raw);
    c_item items[3] = {
        { 0, LIST_INITIAL_CLEARED_VALUE },
        { 1, LIST_INITIAL_CLEARED_VALUE },
        { 2, LIST_INITIAL_CLEARED_VALUE },
    };
    for (c_item &i : items) {
        list_add_tail(&raw, &i.node);
    }

    lk::list_view<c_item, c_item_traits> view(&raw);
    EXPECT_TRUE(view.c_head() == &raw);
    EXPECT_EQ(3u, view.size_slow());
    int expected = 0;
    for (const c_item &i : view) {
        EXPECT_EQ(expected, i.value);
        expected++;
    }

    // a view is a value; a copy is the same list
    lk::list_view<c_item, c_item_traits> copy = view;
    EXPECT_TRUE(copy.pop_front() == &items[0]);
    EXPECT_EQ(2u, view.size_slow());
    EXPECT_EQ(2u, list_length(&raw));

    view.clear();
    EXPECT_TRUE(list_is_empty(&raw));

    END_TEST;
}

bool c_interop() {
    BEGIN_TEST;

    widget w[3] = { widget(0), widget(1), widget(2) };

    // built by C++, walked and reversed by C
    lk::list<widget> l;
    for (widget &x : w) {
        l.push_back(&x);
    }
    EXPECT_EQ(3u, list_c_interop_length(l.c_head()));
    list_c_interop_reverse(l.c_head());
    EXPECT_TRUE(l.front() == &w[2]);
    EXPECT_TRUE(l.back() == &w[0]);
    int expected = 2;
    for (const widget &x : l) {
        EXPECT_EQ(expected, x.value());
        expected--;
    }
    l.clear();

    // built by C on a raw head, walked by C++ through a view
    struct list_node raw = LIST_INITIAL_VALUE(raw);
    for (widget &x : w) {
        list_c_interop_append(&raw, x.list_node_ptr());
    }
    lk::list_view<widget> view(&raw);
    EXPECT_EQ(3u, view.size_slow());
    expected = 0;
    for (const widget &x : view) {
        EXPECT_EQ(expected, x.value());
        expected++;
    }

    // a node handed back by C maps to its object
    struct list_node *n = list_c_interop_pop_head(&raw);
    EXPECT_TRUE(lk::base_hook_traits<widget>::to_object(n) == &w[0]);
    EXPECT_FALSE(w[0].in_list());
    EXPECT_TRUE(view.pop_front() == &w[1]);
    view.clear();
    EXPECT_TRUE(list_is_empty(&raw));

    END_TEST;
}

bool global_list() {
    BEGIN_TEST;

    EXPECT_TRUE(g_list.is_empty());
    global_item items[2];
    items[0].value = 1;
    items[1].value = 2;
    g_list.push_back(&items[0]);
    g_list.push_back(&items[1]);
    int sum = 0;
    for (const global_item &i : g_list) {
        sum += i.value;
    }
    EXPECT_EQ(3, sum);
    g_list.clear();
    EXPECT_TRUE(g_list.is_empty());

    END_TEST;
}

BEGIN_TEST_CASE(list_tests)
RUN_TEST(empty_list)
RUN_TEST(push_pop)
RUN_TEST(insert_remove)
RUN_TEST(iteration)
RUN_TEST(erase_during_iteration)
RUN_TEST(two_tags)
RUN_TEST(member_hook_traits)
RUN_TEST(list_view_over_c_head)
RUN_TEST(c_interop)
RUN_TEST(global_list)
END_TEST_CASE(list_tests)

} // namespace
