/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

// C++ face of the intrusive doubly linked list in <lk/list.h>. Nodes and heads are plain
// struct list_node and every link operation is one of the C inlines, so a list built on
// one side of the language boundary can be walked on the other.
//
// Patterns:
//
//   // a class on a list: derive from the hook (any class; offsetof is never used)
//   class foo : public lk::list_hook<> {
//       ...
//   };
//   lk::list<foo> foos;                    // constant-initialized when global
//
//   foos.push_back(&f);                    // f must not be on a list already
//   foo *first = foos.front();             // nullptr when empty
//   foo *next = foos.next(&f);             // nullptr at the end
//   for (foo &x : foos) { ... }
//   for (auto it = foos.begin(); it != foos.end();) {       // remove while walking
//       if (done(*it)) {
//           it = foos.erase(it);
//       } else {
//           ++it;
//       }
//   }
//   foos.remove(&f);                       // needs no head, like list_delete()
//   while (foo *x = foos.pop_front()) { ... }
//   if (f.in_list()) { ... }
//
//   // on two lists at once: one hook per tag
//   struct child_tag {};
//   class node : public lk::list_hook<>, public lk::list_hook<child_tag> { ... };
//   lk::list<node, lk::base_hook_traits<node, child_tag>> children;
//
//   // a C struct with an embedded node (standard-layout only; uses containerof)
//   struct bar { int v; struct list_node node; };
//   LK_LIST_MEMBER_TRAITS(bar_traits, bar, node);
//   lk::list<bar, bar_traits> bars;
//
//   // a list that C owns, walked from C++; and a C++ list handed to C
//   lk::list_view<bar, bar_traits> view(&head_in_a_c_struct);
//   for (bar &b : view) { ... }
//   list_for_every(foos.c_head(), node) { ... }              // C side, raw nodes
//   list_add_tail(&c_head, f.list_node_ptr());               // C side, a hooked object
//
// A list at namespace scope is constant-initialized, so it is valid before
// call_constructors() has run.

#include <assert.h>
#include <lk/compiler.h>
#include <lk/list.h>
#include <sys/types.h>
#include <type_traits>

namespace lk {

// Base-class hook. Derive publicly (never virtually) to join a list; use a distinct Tag per
// list the class can be on at the same time. The node is the hook's only member, so the hook
// is standard-layout and the node sits at its address: containerof() is defined on it, and a
// class deriving from the hook is free to name list_node itself. prev/next stay private; the
// C API gets the node through list_node_ptr(). Copying is disabled because a copy of a linked
// node would have neighbors that do not point back.
template <typename Tag = void>
class list_hook {
public:
    constexpr list_hook() : node_{ nullptr, nullptr } {}
#if DEBUG_ASSERT_IMPLEMENTED
    // An object must leave every list before it goes away or the list keeps a dangling
    // pointer. The check needs a destructor, and any destructor makes a global object
    // register itself with __cxa_atexit at boot, so it only exists in debug builds.
    ~list_hook() { DEBUG_ASSERT(!in_list()); }
#endif
    list_hook(const list_hook &) = delete;
    list_hook &operator=(const list_hook &) = delete;

    // same test as list_in_list(): an unlinked node has both pointers cleared
    bool in_list() const { return !(node_.prev == nullptr && node_.next == nullptr); }

    list_node *list_node_ptr() { return &node_; }
    const list_node *list_node_ptr() const { return &node_; }

    // node -> hook
    static list_hook *from_node(list_node *n) {
        static_assert(__is_standard_layout(list_hook), "containerof needs standard layout");
        return containerof(n, list_hook, node_);
    }

private:
    list_node node_;
};

// Traits for a type that derives from list_hook<Tag>. The checks sit inside the function
// bodies so that a class can hold an lk::list of its own type while it is still incomplete.
template <typename T, typename Tag = void>
struct base_hook_traits {
    using hook = list_hook<Tag>;

    static list_node *to_node(T *obj) {
        static_assert(__is_base_of(hook, T), "T must derive publicly from lk::list_hook<Tag>");
        return static_cast<hook *>(obj)->list_node_ptr();
    }
    static T *to_object(list_node *n) {
        static_assert(__is_base_of(hook, T), "T must derive publicly from lk::list_hook<Tag>");
        return static_cast<T *>(hook::from_node(n));
    }
};

namespace internal {

// Head storage for lk::list. The empty list points at itself, which is an address constant,
// so a global list is constant-initialized. Not copyable: the self pointers would dangle.
struct owned_head {
    constexpr owned_head() : head_{ &head_, &head_ } {}
    owned_head(const owned_head &) = delete;
    owned_head &operator=(const owned_head &) = delete;

    // the C API takes a non-const node even for reads
    list_node *head() const { return const_cast<list_node *>(&head_); }

    list_node head_;
};

// Head storage for lk::list_view: a pointer to a head that lives elsewhere, typically in a
// C struct. A view is a plain copyable value.
struct borrowed_head {
    constexpr explicit borrowed_head(list_node *h) : head_(h) {}

    list_node *head() const { return head_; }

    list_node *head_;
};

} // namespace internal

// The list operations, shared by lk::list and lk::list_view. Elements are handled by
// pointer; lookups return nullptr on an empty list or at the ends rather than asserting.
template <typename T, typename Traits, typename Head>
class basic_list : private Head {
public:
    // Bidirectional iterator carrying the raw node. The object is formed only on deref,
    // never for end(), which is the head and not embedded in any object.
    template <typename U>
    class basic_iterator {
    public:
        constexpr basic_iterator() = default;
        constexpr explicit basic_iterator(list_node *n) : node_(n) {}

        // an iterator converts to a const_iterator, never the other way round
        template <typename V, typename = std::enable_if_t<std::is_same_v<U, const V>>>
        constexpr basic_iterator(const basic_iterator<V> &other) : node_(other.node()) {}

        U &operator*() const { return *Traits::to_object(node_); }
        U *operator->() const { return Traits::to_object(node_); }

        basic_iterator &operator++() {
            node_ = node_->next;
            return *this;
        }
        basic_iterator operator++(int) {
            basic_iterator old = *this;
            node_ = node_->next;
            return old;
        }
        basic_iterator &operator--() {
            node_ = node_->prev;
            return *this;
        }
        basic_iterator operator--(int) {
            basic_iterator old = *this;
            node_ = node_->prev;
            return old;
        }

        // hidden friends, so a mixed iterator / const_iterator comparison converts either side
        friend bool operator==(const basic_iterator &a, const basic_iterator &b) {
            return a.node_ == b.node_;
        }
        friend bool operator!=(const basic_iterator &a, const basic_iterator &b) {
            return a.node_ != b.node_;
        }

        list_node *node() const { return node_; }

    private:
        list_node *node_ = nullptr;
    };

    using iterator = basic_iterator<T>;
    using const_iterator = basic_iterator<const T>;

    // lk::list is default constructed empty; lk::list_view takes the head to wrap
    using Head::Head;

    // the raw head, for handing the list to the C API
    list_node *c_head() const { return Head::head(); }

    bool is_empty() const { return list_is_empty(c_head()); }
    size_t size_slow() const { return list_length(c_head()); }

    void push_front(T *item) {
        list_node *n = Traits::to_node(item);
        DEBUG_ASSERT(!list_in_list(n));
        list_add_head(c_head(), n);
    }
    void push_back(T *item) {
        list_node *n = Traits::to_node(item);
        DEBUG_ASSERT(!list_in_list(n));
        list_add_tail(c_head(), n);
    }

    // insert relative to an element that is already in the list
    static void insert_after(T *pos, T *item) {
        list_node *n = Traits::to_node(item);
        DEBUG_ASSERT(!list_in_list(n));
        list_add_after(Traits::to_node(pos), n);
    }
    static void insert_before(T *pos, T *item) {
        list_node *n = Traits::to_node(item);
        DEBUG_ASSERT(!list_in_list(n));
        list_add_before(Traits::to_node(pos), n);
    }

    // unlink an element; like list_delete() this needs no head
    static void remove(T *item) {
        list_node *n = Traits::to_node(item);
        DEBUG_ASSERT(list_in_list(n));
        list_delete(n);
    }

    T *pop_front() { return to_object_or_null(list_remove_head(c_head())); }
    T *pop_back() { return to_object_or_null(list_remove_tail(c_head())); }

    T *front() { return to_object_or_null(list_peek_head(c_head())); }
    T *back() { return to_object_or_null(list_peek_tail(c_head())); }
    const T *front() const { return to_object_or_null(list_peek_head(c_head())); }
    const T *back() const { return to_object_or_null(list_peek_tail(c_head())); }

    // the neighbors of an element, nullptr at either end
    T *next(T *item) const {
        return to_object_or_null(list_next(c_head(), Traits::to_node(item)));
    }
    T *prev(T *item) const {
        return to_object_or_null(list_prev(c_head(), Traits::to_node(item)));
    }

    // Unlinks *it and returns the position after it, so removing while walking is
    //   for (auto it = l.begin(); it != l.end();) { if (...) it = l.erase(it); else ++it; }
    iterator erase(iterator it) {
        list_node *n = it.node();
        DEBUG_ASSERT(n != c_head());
        list_node *following = n->next;
        list_delete(n);
        return iterator(following);
    }

    // unlink everything, one node at a time so each reads as not in a list afterwards
    void clear() {
        while (list_remove_head(c_head()) != nullptr) {
        }
    }

    iterator make_iterator(T *item) { return iterator(Traits::to_node(item)); }

    iterator begin() { return iterator(c_head()->next); }
    iterator end() { return iterator(c_head()); }
    const_iterator begin() const { return const_iterator(c_head()->next); }
    const_iterator end() const { return const_iterator(c_head()); }
    const_iterator cbegin() const { return begin(); }
    const_iterator cend() const { return end(); }

private:
    static T *to_object_or_null(list_node *n) { return n ? Traits::to_object(n) : nullptr; }
};

// A list that owns its head. Traits defaults to the base hook with the default tag; pass
// base_hook_traits<T, tag> for another hook, or member traits for a C struct.
template <typename T, typename Traits = base_hook_traits<T>>
using list = basic_list<T, Traits, internal::owned_head>;

// The same operations over a head that lives elsewhere, for walking a C owned list.
template <typename T, typename Traits = base_hook_traits<T>>
using list_view = basic_list<T, Traits, internal::borrowed_head>;

} // namespace lk

// Traits for a list_node member of a standard-layout type, which is what a C struct is.
// Expand at namespace scope, once per name per translation unit. For anything else derive
// from lk::list_hook: containerof is undefined behavior on non-standard-layout types.
#define LK_LIST_MEMBER_TRAITS(name, type, member)                                              \
    struct name {                                                                              \
        static list_node *to_node(type *obj) { return &obj->member; }                         \
        static type *to_object(list_node *n) {                                                 \
            static_assert(__is_standard_layout(type),                                          \
                          "containerof needs a standard-layout type; use lk::list_hook");      \
            return containerof(n, type, member);                                               \
        }                                                                                      \
    }
