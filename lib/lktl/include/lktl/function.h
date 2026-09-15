/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

// lk::function<R(Args...), Size>: an owning std::function that never touches the heap. The
// callable lives inside the object, in Size bytes (two words by default), so a lambda
// capturing a this pointer and one more word fits and anything larger is a compile error
// that prints both sizes, never an allocation. Move only; one indirect call per invocation.
//
// Patterns:
//
//   // a callback stored on an object, with this captured
//   class disk {
//       lk::function<void(status_t)> on_done_;
//       void start() {
//           on_done_ = [this](status_t s) { finish(s); };
//           on_done_ = lk::method<&disk::finish>(this);      // same, without restating args
//       }
//   };
//
//   // a plain function, a lambda with a word of state, a method on another object
//   lk::function<int(int)> f = add_one;
//   lk::function<void()> g = [p, n]() { p->count += n; };
//   lk::function<void(bdev_t *)> h = lk::method<&cache::flush>(cache_);
//
//   // more capture room, when the callable needs it
//   lk::function<void(), 4 * sizeof(void *)> big = [a, b, c, d]() { ... };
//
//   // empty, test, call, clear
//   lk::function<void()> cb;      // empty; calling it panics
//   if (cb) {
//       cb();
//   }
//   cb = nullptr;
//
//   // handing one to something that keeps it is a move
//   register_completion(std::move(on_done_));
//
// For a callback the callee only uses before it returns, take an lk::function_ref instead:
// two words, nothing copied, no size limit. A function refuses to store a function_ref, since
// that would dangle. An empty function is constant-initialized, so a global one is valid
// before constructors run, though like any object with a destructor it registers with
// __cxa_atexit at boot. Callables need at most 8 byte alignment.

#include <lk/debug.h>
#include <lktl/method.h>
#include <new>
#include <stddef.h>
#include <type_traits>
#include <utility>

namespace lk {

template <typename Signature>
class function_ref; // lktl/function_ref.h

namespace internal {

template <typename T>
inline constexpr bool is_function_ref_v = false;
template <typename Signature>
inline constexpr bool is_function_ref_v<function_ref<Signature>> = true;

constexpr size_t default_function_size = 2 * sizeof(void *);

constexpr size_t round_up_to_word(size_t n) {
    return n == 0 ? sizeof(void *) : (n + sizeof(void *) - 1) / sizeof(void *) * sizeof(void *);
}

// wide enough for a captured 64-bit value on the 32-bit targets, without inflating the
// object to max_align_t
constexpr size_t function_storage_align =
    alignof(long long) > alignof(void *) ? alignof(long long) : alignof(void *);

// The operations on a stored callable, one static table per callable type. move() leaves
// from destroyed; to is raw storage.
template <typename R, typename... Args>
struct function_ops {
    R (*invoke)(void *storage, Args... args);
    void (*move)(void *from, void *to);
    void (*destroy)(void *storage);
};

template <typename R, typename... Args>
[[noreturn]] R function_invoke_empty(void *, Args...) {
    panic("lk::function called while empty\n");
}
inline void function_move_nothing(void *, void *) {}
inline void function_destroy_nothing(void *) {}

// the table an empty function points at; a single object per signature
template <typename R, typename... Args>
inline constexpr function_ops<R, Args...> empty_function_ops = {
    &function_invoke_empty<R, Args...>,
    &function_move_nothing,
    &function_destroy_nothing,
};

template <typename C, typename R, typename... Args>
struct function_target {
    static R invoke(void *storage, Args... args) {
        C &callable = *static_cast<C *>(storage);
        if constexpr (std::is_void_v<R>) {
            callable(std::forward<Args>(args)...);
        } else {
            return callable(std::forward<Args>(args)...);
        }
    }
    static void move(void *from, void *to) {
        C &callable = *static_cast<C *>(from);
        new (to) C(std::move(callable));
        callable.~C();
    }
    static void destroy(void *storage) { static_cast<C *>(storage)->~C(); }

    static constexpr function_ops<R, Args...> ops = { &invoke, &move, &destroy };
};

// The capacity checks live in templates whose parameters are the two numbers, so a failure
// names both sizes in every compiler's instantiation trace, next to the message.
template <size_t callable_bytes, size_t storage_bytes>
struct function_capacity {
    static_assert(callable_bytes <= storage_bytes,
                  "callable too large for this lk::function: raise its Size template argument "
                  "(bytes; two words by default) or capture less");
    static constexpr bool ok = callable_bytes <= storage_bytes;
};

template <size_t callable_align, size_t storage_align>
struct function_alignment {
    static_assert(callable_align <= storage_align,
                  "callable needs more alignment than lk::function storage provides");
    static constexpr bool ok = callable_align <= storage_align;
};

// satisfied when C can be called with Args and the result converts to R (anything, for void)
template <typename C, typename R, typename... Args>
using function_callable_t = std::enable_if_t<
    std::is_void_v<R> ||
    std::is_convertible_v<decltype(std::declval<C &>()(std::declval<Args>()...)), R>>;

} // namespace internal

template <typename Signature, size_t Size = internal::default_function_size>
class function;

template <typename R, typename... Args, size_t Size>
class function<R(Args...), Size> {
public:
    using result_type = R;
    static constexpr size_t storage_size = internal::round_up_to_word(Size);

    constexpr function() : storage_{}, ops_(&internal::empty_function_ops<R, Args...>) {}
    constexpr function(decltype(nullptr)) : function() {}

    // from any callable object: a lambda, or anything else with an operator()
    template <typename C, typename D = std::decay_t<C>,
              typename = std::enable_if_t<!std::is_same_v<D, function> &&
                                          !std::is_function_v<std::remove_reference_t<C>>>,
              typename = internal::function_callable_t<D, R, Args...>>
    function(C &&callable) : ops_(&internal::empty_function_ops<R, Args...>) {
        assign(std::forward<C>(callable));
    }

    // From a plain function, taken as a pointer and never as a reference: on thumb a
    // function's address carries the interworking bit in bit 0, so a reference bound to the
    // function itself is a reference to an odd address. A null pointer gives an empty
    // function.
    template <typename F, typename = std::enable_if_t<std::is_function_v<F>>,
              typename = internal::function_callable_t<F *, R, Args...>>
    function(F *fn) : ops_(&internal::empty_function_ops<R, Args...>) { assign(fn); }

    function(function &&other) noexcept { take(other); }
    function &operator=(function &&other) noexcept {
        if (this != &other) {
            ops_->destroy(storage_);
            take(other);
        }
        return *this;
    }

    template <typename C, typename D = std::decay_t<C>,
              typename = std::enable_if_t<!std::is_same_v<D, function> &&
                                          !std::is_function_v<std::remove_reference_t<C>>>,
              typename = internal::function_callable_t<D, R, Args...>>
    function &operator=(C &&callable) {
        ops_->destroy(storage_);
        ops_ = &internal::empty_function_ops<R, Args...>;
        assign(std::forward<C>(callable));
        return *this;
    }
    template <typename F, typename = std::enable_if_t<std::is_function_v<F>>,
              typename = internal::function_callable_t<F *, R, Args...>>
    function &operator=(F *fn) {
        ops_->destroy(storage_);
        ops_ = &internal::empty_function_ops<R, Args...>;
        assign(fn);
        return *this;
    }
    function &operator=(decltype(nullptr)) {
        reset();
        return *this;
    }

    function(const function &) = delete;
    function &operator=(const function &) = delete;

    ~function() { ops_->destroy(storage_); }

    // drop the callable, leaving the function empty
    void reset() {
        ops_->destroy(storage_);
        ops_ = &internal::empty_function_ops<R, Args...>;
    }

    explicit operator bool() const { return ops_ != &internal::empty_function_ops<R, Args...>; }

    R operator()(Args... args) const { return ops_->invoke(storage_, std::forward<Args>(args)...); }

private:
    template <typename C>
    void assign(C &&callable) {
        using D = std::decay_t<C>;
        static_assert(!internal::is_function_ref_v<D>,
                      "store the callable itself; a function_ref only borrows it and would dangle");
        // a failed check reports the sizes; skipping the body then keeps the compiler from
        // piling a placement-new warning on top of the assertion
        constexpr bool fits = internal::function_capacity<sizeof(D), storage_size>::ok &&
                              internal::function_alignment<alignof(D), internal::function_storage_align>::ok;
        if constexpr (fits) {
            // a plain function reaches here as a pointer, so this is the null check the
            // class documents; a null one leaves the function empty
            if constexpr (std::is_pointer_v<D>) {
                if (callable == nullptr) {
                    return;
                }
            }
            new (storage_) D(std::forward<C>(callable));
            ops_ = &internal::function_target<D, R, Args...>::ops;
        }
    }

    // move other's callable into this, which must hold nothing; other ends up empty
    void take(function &other) {
        ops_ = other.ops_;
        ops_->move(other.storage_, storage_);
        other.ops_ = &internal::empty_function_ops<R, Args...>;
    }

    // mutable so a const function can still hand the callable a non-const pointer to itself
    alignas(internal::function_storage_align) mutable unsigned char storage_[storage_size];
    const internal::function_ops<R, Args...> *ops_;
};

} // namespace lk
