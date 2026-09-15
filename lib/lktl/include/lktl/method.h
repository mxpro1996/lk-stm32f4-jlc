/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

// "Call me back on this object": binding a method and its object as a callback, without
// writing the marshalling function by hand each time.
//
// lk::method<&T::m>(obj) is a callable that runs obj->m(args...). It stores only the object
// pointer, one word, so it fits a default sized lk::function, and it does not restate the
// method's parameter list:
//
//   class disk {
//       void io_done(bdev_t *dev, ssize_t result);
//       lk::function<void(bdev_t *, ssize_t)> on_done_;
//       ...
//       on_done_ = lk::method<&disk::io_done>(this);       // stored
//       for_every_device(lk::method<&disk::visit>(this));  // passed to a function_ref
//   };
//
// For a C API that takes a plain function plus a void *cookie, lk::method_cookie_first<&T::m>
// and lk::method_cookie_last<&T::m> are the function pointer to register, with the object as
// the cookie. The other parameters and the result are the method's own, so the pointer has
// exactly the type the C API declares:
//
//   // typedef void (*bio_async_callback_t)(void *cookie, bdev_t *dev, ssize_t status);
//   bio_read_async(dev, buf, off, len, lk::method_cookie_first<&disk::io_done>, this);
//
//   // typedef void (*pci_visit_routine)(pci_location_t loc, void *cookie);
//   pci_bus_mgr_visit_devices(lk::method_cookie_last<&probe::visit>, this);
//
// A const method takes a const object; an overloaded method needs a cast to pick the
// overload, as with any pointer to member.

#include <utility>

namespace lk {

template <auto Method, typename T>
constexpr auto method(T *obj) {
    return [obj](auto &&...args) -> decltype(auto) {
        return (obj->*Method)(std::forward<decltype(args)>(args)...);
    };
}

namespace internal {

template <typename M>
struct method_traits;

template <typename R, typename T, typename... Args>
struct method_traits<R (T::*)(Args...)> {
    template <auto Method>
    static R cookie_first(void *cookie, Args... args) {
        return (static_cast<T *>(cookie)->*Method)(std::forward<Args>(args)...);
    }
    template <auto Method>
    static R cookie_last(Args... args, void *cookie) {
        return (static_cast<T *>(cookie)->*Method)(std::forward<Args>(args)...);
    }
};

template <typename R, typename T, typename... Args>
struct method_traits<R (T::*)(Args...) const> {
    template <auto Method>
    static R cookie_first(void *cookie, Args... args) {
        return (static_cast<const T *>(cookie)->*Method)(std::forward<Args>(args)...);
    }
    template <auto Method>
    static R cookie_last(Args... args, void *cookie) {
        return (static_cast<const T *>(cookie)->*Method)(std::forward<Args>(args)...);
    }
};

} // namespace internal

template <auto Method>
inline constexpr auto method_cookie_first =
    &internal::method_traits<decltype(Method)>::template cookie_first<Method>;

template <auto Method>
inline constexpr auto method_cookie_last =
    &internal::method_traits<decltype(Method)>::template cookie_last<Method>;

} // namespace lk
