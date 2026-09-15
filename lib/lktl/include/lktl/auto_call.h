//
// Copyright (c) 2021 Travis Geiselbrecht
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT
//
// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#pragma once

#include <type_traits>
#include <utility>

namespace lk {

// Runs a callable when it goes out of scope, unless cancelled first: the way to undo setup
// on every exit path of a function without repeating it before each return.
//
//   auto cleanup = lk::make_auto_call([&]() { release(thing); });
//   ...
//   cleanup.cancel();   // success: keep thing
//
// The callable runs at most once: on destruction, on an explicit call(), or when another
// auto_call is move-assigned over this one. A moved-from auto_call is cancelled. Move
// assignment needs an assignable callable, which a lambda with captures is not, so it is
// only available with function pointers and functor objects. The class is [[nodiscard]]
// because a temporary would run the callable at the end of the statement that created it,
// which is never what was meant.
template <typename T>
class [[nodiscard]] auto_call {
public:
    explicit auto_call(T c) : call_(std::move(c)) {}
    ~auto_call() { call(); }

    auto_call(auto_call &&other) noexcept(std::is_nothrow_move_constructible_v<T>)
        : call_(std::move(other.call_)), armed_(other.armed_) {
        other.cancel();
    }
    auto_call &operator=(auto_call &&other) noexcept(std::is_nothrow_move_assignable_v<T>) {
        if (this != &other) {
            call();
            call_ = std::move(other.call_);
            armed_ = other.armed_;
            other.cancel();
        }
        return *this;
    }

    auto_call(const auto_call &) = delete;
    auto_call &operator=(const auto_call &) = delete;

    // run the callable now unless it already ran or was cancelled; it will not run again
    void call() {
        if (armed_) {
            armed_ = false;
            call_();
        }
    }

    // the callable will not run
    void cancel() { armed_ = false; }

private:
    T call_;
    bool armed_ = true;
};

// lk::auto_call cleanup([&]() { ... }); deduces T on its own in C++17; this spelling predates
// that and reads the same.
template <typename T>
[[nodiscard]] inline auto_call<T> make_auto_call(T c) {
    return auto_call<T>(std::move(c));
}

} // namespace lk
