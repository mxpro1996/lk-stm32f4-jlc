/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

// Tests for lk::auto_call in <lktl/auto_call.h>.

#include <lktl/auto_call.h>

#include <lib/unittest.h>
#include <type_traits>
#include <utility>

namespace {

// an assignable callable, for the move assignment tests; capturing lambdas are not
struct adder {
    int *target;
    int amount;
    void operator()() const { *target += amount; }
};

using counter_call = lk::auto_call<void (*)()>;
static_assert(!std::is_copy_constructible_v<counter_call>);
static_assert(!std::is_copy_assignable_v<counter_call>);
static_assert(std::is_nothrow_move_constructible_v<counter_call>);
static_assert(std::is_nothrow_move_assignable_v<counter_call>);

bool runs_at_scope_exit() {
    BEGIN_TEST;

    int count = 0;
    {
        auto cleanup = lk::make_auto_call([&count]() { count++; });
        EXPECT_EQ(0, count);
    }
    EXPECT_EQ(1, count);

    // the C++17 spelling without the helper
    {
        lk::auto_call cleanup([&count]() { count += 10; });
        EXPECT_EQ(1, count);
    }
    EXPECT_EQ(11, count);

    END_TEST;
}

bool cancel_and_call() {
    BEGIN_TEST;

    int count = 0;
    {
        auto cleanup = lk::make_auto_call([&count]() { count++; });
        cleanup.cancel();
    }
    EXPECT_EQ(0, count);

    // call() runs it once, early, and destruction does not run it again
    {
        auto cleanup = lk::make_auto_call([&count]() { count++; });
        cleanup.call();
        EXPECT_EQ(1, count);
        cleanup.call();
        EXPECT_EQ(1, count);
    }
    EXPECT_EQ(1, count);

    // cancel after call is a no-op, call after cancel does nothing
    {
        auto cleanup = lk::make_auto_call([&count]() { count++; });
        cleanup.cancel();
        cleanup.call();
    }
    EXPECT_EQ(1, count);

    END_TEST;
}

bool move_construct() {
    BEGIN_TEST;

    int count = 0;
    {
        auto outer = lk::make_auto_call([&count]() { count++; });
        {
            auto inner = std::move(outer);
            EXPECT_EQ(0, count);
        }
        // the moved-to object ran it; the moved-from one is cancelled
        EXPECT_EQ(1, count);
    }
    EXPECT_EQ(1, count);

    END_TEST;
}

bool move_assign() {
    BEGIN_TEST;

    int first = 0;
    int second = 0;
    {
        lk::auto_call target(adder{ &first, 1 });
        lk::auto_call source(adder{ &first, 100 });
        // assigning over a pending auto_call runs its callable right away
        target = std::move(source);
        EXPECT_EQ(1, first);
    }
    // the moved-in callable ran once on destruction of target, not again for source
    EXPECT_EQ(101, first);

    {
        lk::auto_call target(adder{ &second, 1 });
        target.cancel();
        lk::auto_call source(adder{ &second, 10 });
        target = std::move(source);
        EXPECT_EQ(0, second);
    }
    EXPECT_EQ(10, second);

    END_TEST;
}

BEGIN_TEST_CASE(auto_call_tests)
RUN_TEST(runs_at_scope_exit)
RUN_TEST(cancel_and_call)
RUN_TEST(move_construct)
RUN_TEST(move_assign)
END_TEST_CASE(auto_call_tests)

} // namespace
