# Intrusive Lists

LK keeps its lists intrusive: the link fields live inside the objects being listed, so
adding an object to a list allocates nothing. The C list in
[`lk/list.h`](../top/include/lk/list.h) is used throughout the kernel, and
[`lktl/list.h`](../lib/lktl/include/lktl/list.h) in `lib/lktl` gives C++ code a typed
container over the same structure. Both sides see the same `struct list_node`, so a list
built by one can be walked by the other.

## The C list

```c
struct list_node {
    struct list_node *prev;
    struct list_node *next;
};
```

- The list is circular and the head is itself a `list_node`. An empty list points at itself:
  `struct list_node head = LIST_INITIAL_VALUE(head);` or `list_initialize(&head)`.
- An object joins a list through an embedded node, and gets back from the node with
  `containerof(node, type, member)`. A node that is not in any list has both pointers
  cleared (`LIST_INITIAL_CLEARED_VALUE`, `list_clear_node()`), which is what
  `list_in_list()` tests. `list_delete()` clears the node it unlinks.
- The head is not embedded in an object, so no `containerof()` of it is ever valid. The
  iteration macros stop before forming one.

| Operation | Functions |
|---|---|
| Add | `list_add_head()`, `list_add_tail()`, `list_add_before()`, `list_add_after()` |
| Remove | `list_delete()` (no head needed), `list_remove_head()`, `list_remove_tail()` |
| Look | `list_peek_head()`, `list_peek_tail()`, `list_next()`, `list_prev()`, `list_is_empty()`, `list_length()` |
| Typed | `list_remove_head_type()`, `list_peek_head_type()`, `list_next_type()`, ... apply `containerof()` and yield `NULL` at the end |
| Iterate | `list_for_every()` over nodes, `list_for_every_entry()` over objects, `_safe` variants that survive deleting the current element |

```c
struct widget {
    struct list_node node;
    int value;
};

struct list_node widgets = LIST_INITIAL_VALUE(widgets);

list_add_tail(&widgets, &w->node);

struct widget *w;
list_for_every_entry(&widgets, w, struct widget, node) {
    printf("%d\n", w->value);
}
```

## C++ Wrapper

`containerof()` is `offsetof()`, which is only defined for standard-layout types. A C++
class with virtual functions, a base class carrying data, or members of mixed access
control is not one, and the compiler says so with `-Winvalid-offsetof`. The C++ face avoids
the problem for classes by deriving from a hook instead of embedding a node. The hook is a
standard-layout class whose only member is the `list_node`, so `containerof()` is defined on
the hook, and a `static_cast` from the hook to the derived object is defined for any class.

```cpp
#include <lktl/list.h>

class device : public lk::list_hook<> {
public:
    virtual ~device();
    ...
};

lk::list<device> devices;          // constant-initialized when global

devices.push_back(dev);
for (device &d : devices) {
    d.dump();
}
device *first = devices.front();   // nullptr when empty
lk::list<device>::remove(dev);     // like list_delete(), needs no head
```

### `lk::list_hook<Tag>`

The base-class hook. Derive from it publicly, never virtually. A class that sits on several
lists at once derives from one hook per list, told apart by a tag type:

```cpp
struct child_tag {};
struct sibling_tag {};

class node : public lk::list_hook<child_tag>, public lk::list_hook<sibling_tag> { ... };

using child_list = lk::list<node, lk::base_hook_traits<node, child_tag>>;
using sibling_list = lk::list<node, lk::base_hook_traits<node, sibling_tag>>;
```

- `in_list()` reports membership, the same test as `list_in_list()`. With several hooks,
  cast to the wanted one first: `static_cast<lk::list_hook<child_tag> &>(n).in_list()`.
- `list_node_ptr()` returns the raw node for handing the object to the C API.
- Copying a hook is disabled; a copy of a linked node would have neighbors that do not
  point back at it.
- In debug builds the hook's destructor asserts that the object is not still linked. The
  destructor is compiled out in release builds, so the hook is trivially destructible
  there and a global object carrying one needs no runtime initialization.

### `lk::list<T, Traits>`

Owns its head. `Traits` maps between `T *` and `list_node *` and defaults to
`lk::base_hook_traits<T>` (the hook with the default tag).

| Method | Behavior |
|---|---|
| `push_front(T *)`, `push_back(T *)` | Link at either end; the object must not already be on a list |
| `insert_before(T *pos, T *)`, `insert_after(T *pos, T *)` | Link relative to an element |
| `remove(T *)` | Unlink; static, needs no head |
| `pop_front()`, `pop_back()` | Unlink and return an end, `nullptr` when empty |
| `front()`, `back()`, `next(T *)`, `prev(T *)` | Look without unlinking, `nullptr` when there is nothing |
| `erase(iterator)` | Unlink the element and return the position after it |
| `clear()` | Unlink everything, so every element reads as not in a list |
| `is_empty()`, `size_slow()` | `size_slow()` walks the list |
| `begin()`, `end()`, `make_iterator(T *)` | Bidirectional iterators; `end()` is the head |
| `c_head()` | The raw `list_node *` for the C API |

A list can hold elements of its own type while the class is still being defined, so a tree
node may contain `lk::list<node, ...> children_;`. Iterating a `const` list yields
`const T &`.

Removing while walking is the same idiom as `list_for_every_entry_safe()`:

```cpp
for (auto it = l.begin(); it != l.end();) {
    if (it->expired()) {
        it = l.erase(it);
    } else {
        ++it;
    }
}
```

The empty list's head points at itself, which is an address constant, so a namespace-scope
`lk::list` is constant-initialized and usable from init hooks that run before
`call_constructors()`. The list is not copyable or movable for the same reason.

### `lk::list_view<T, Traits>`

The same operations over a head that lives somewhere else, typically in a C struct. A view
is a copyable one-pointer value that does not own the list:

```cpp
LK_LIST_MEMBER_TRAITS(thread_list_traits, thread_t, thread_list_node);

lk::list_view<thread_t, thread_list_traits> threads(&thread_list);
for (thread_t &t : threads) { ... }
```

### `LK_LIST_MEMBER_TRAITS(name, type, member)`

Defines a traits type for a `list_node` member of a standard-layout type, which is what a
C struct is. It uses `containerof()`, and a `static_assert` rejects anything that is not
standard-layout. Expand it at namespace scope, once per name in a translation unit. Anything
that fails the check should derive from `lk::list_hook` instead.

### Crossing the language boundary

- C++ to C: pass `list.c_head()` where C expects the head, and `obj->list_node_ptr()` where
  it expects a node. `list_c_interop.c` in `lib/lktl/test/` walks and reverses a list that
  C++ built, with nothing but the C macros.
- C to C++: wrap a C head in a `lk::list_view`, and map a node handed back by C with
  `lk::base_hook_traits<T>::to_object(node)` or the member traits' `to_object()`.
- A module using it declares `MODULE_DEPS += lib/lktl`.
