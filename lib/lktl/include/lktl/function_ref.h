/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

// lk::function_ref<R(Args...)>: a non-owning reference to a callable. Two words, the
// callable's address and a thunk, trivially copyable, so it passes in registers and costs
// what two pointer arguments cost. It is the parameter type for a callback the callee only
// uses before it returns, a visitor being the usual case.
//
// Patterns:
//
//   // the parameter
//   status_t for_every_device(lk::function_ref<status_t(device *)> fn);
//
//   // the ways to call it
//   for_every_device([&](device *d) { count++; return NO_ERROR; });   // lambda in the call
//   for_every_device(lk::method<&probe::visit>(this));                // a method of this
//   for_every_device(check_device);                                   // a plain function
//   for_every_device(on_device_);                                     // an lk::function
//
//   // inside the callee
//   for (device &d : devices_) {
//       status_t err = fn(&d);
//       ...
//   }
//
// Nothing is copied, so there is no size limit on the callable, but the reference is only
// good while the callable lives: a lambda written in the argument list lives to the end of
// that statement. Never keep a function_ref in a member, return one, or hand one to another
// thread; take an lk::function by value and move it instead. A small loop in the same
// translation unit is smaller as a template than through either.

#include <assert.h>
#include <lktl/method.h>
#include <stdint.h>
#include <type_traits>
#include <utility>

namespace lk {

template <typename Signature>
class function_ref;

template <typename R, typename... Args>
class function_ref<R(Args...)> {
public:
    using result_type = R;

    constexpr function_ref() = default;

    // from any callable object: a lambda, an lk::function, anything with an operator()
    template <typename C, typename D = std::remove_reference_t<C>,
              typename = std::enable_if_t<!std::is_same_v<std::decay_t<C>, function_ref> &&
                                          !std::is_function_v<D>>,
              typename = std::enable_if_t<
                  std::is_void_v<R> ||
                  std::is_convertible_v<decltype(std::declval<D &>()(std::declval<Args>()...)), R>>>
    function_ref(C &&callable) noexcept
        : callable_(reinterpret_cast<intptr_t>(&callable)), callback_(&invoke<D>) {}

    // from a plain function, taken as a pointer and never as a reference: on thumb a
    // function's address carries the interworking bit in bit 0, so a reference bound to the
    // function itself is a reference to an odd address
    template <typename F, typename = std::enable_if_t<std::is_function_v<F>>,
              typename = std::enable_if_t<
                  std::is_void_v<R> ||
                  std::is_convertible_v<decltype(std::declval<F *>()(std::declval<Args>()...)), R>>>
    function_ref(F *fn) noexcept
        : callable_(reinterpret_cast<intptr_t>(fn)), callback_(&invoke_fn<F>) {}

    explicit operator bool() const { return callback_ != nullptr; }

    R operator()(Args... args) const {
        DEBUG_ASSERT(callback_ != nullptr);
        return callback_(callable_, std::forward<Args>(args)...);
    }

private:
    template <typename D>
    static R invoke(intptr_t callable, Args... args) {
        D &c = *reinterpret_cast<D *>(callable);
        if constexpr (std::is_void_v<R>) {
            c(std::forward<Args>(args)...);
        } else {
            return c(std::forward<Args>(args)...);
        }
    }

    // the plain function case, where callable is the function's own address
    template <typename F>
    static R invoke_fn(intptr_t callable, Args... args) {
        F *fn = reinterpret_cast<F *>(callable);
        if constexpr (std::is_void_v<R>) {
            fn(std::forward<Args>(args)...);
        } else {
            return fn(std::forward<Args>(args)...);
        }
    }

    intptr_t callable_ = 0;
    R (*callback_)(intptr_t, Args...) = nullptr;
};

} // namespace lk
