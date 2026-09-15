/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

// Tests for lk::function in <lktl/function.h> and lk::function_ref in
// <lktl/function_ref.h>.

#include <lktl/function.h>
#include <lktl/function_ref.h>
#include <lktl/method.h>

#include <lib/unittest.h>
#include <type_traits>
#include <utility>

namespace {

int add_one(int x) { return x + 1; }
void bump(int &x) { x++; }
int bump_and_report(int &x) { return ++x; }

static_assert(sizeof(lk::function<void()>) <= 4 * sizeof(void *));
static_assert(!std::is_copy_constructible_v<lk::function<void()>>);
static_assert(std::is_nothrow_move_constructible_v<lk::function<void()>>);
static_assert(sizeof(lk::function_ref<void()>) == 2 * sizeof(void *));
static_assert(std::is_trivially_copyable_v<lk::function_ref<void()>>);

// a callable whose constructions and destructions are counted, to watch the stored copy
struct counted {
    static int live;
    int *hits;

    explicit counted(int *h) : hits(h) { live++; }
    counted(counted &&other) : hits(other.hits) { live++; }
    ~counted() { live--; }

    void operator()() { (*hits)++; }
};
int counted::live = 0;

// an empty function at namespace scope is constant-initialized
lk::function<int(int)> g_fn;

bool function_basics() {
    BEGIN_TEST;

    lk::function<int(int)> f;
    EXPECT_FALSE(f);
    f = add_one;
    EXPECT_TRUE(f);
    EXPECT_EQ(2, f(1));
    f = [](int x) { return x * 2; };
    EXPECT_EQ(6, f(3));

    // a capture by reference sees later writes
    int base = 10;
    f = [&base](int x) { return base + x; };
    EXPECT_EQ(11, f(1));
    base = 20;
    EXPECT_EQ(21, f(1));

    f = nullptr;
    EXPECT_FALSE(f);
    int (*null_fn)(int) = nullptr;
    f = null_fn;
    EXPECT_FALSE(f);

    // a function pointer variable stores the same way a function name does
    int (*fn_ptr)(int) = add_one;
    f = fn_ptr;
    EXPECT_EQ(4, f(3));

    // reference arguments, and a result the signature discards
    lk::function<void(int &)> g = bump;
    int v = 0;
    g(v);
    EXPECT_EQ(1, v);
    lk::function<void(int &)> h = [](int &x) { x += 10; return x; };
    h(v);
    EXPECT_EQ(11, v);
    lk::function<void(int &)> i = bump_and_report;
    i(v);
    EXPECT_EQ(12, v);

    END_TEST;
}

bool function_state() {
    BEGIN_TEST;

    // a mutable lambda keeps its state inside the function object
    lk::function<int()> counter = [n = 0]() mutable { return ++n; };
    EXPECT_EQ(1, counter());
    EXPECT_EQ(2, counter());

    // the two word budget: a this-like pointer and a word of state fit
    int hits = 0;
    int *p = &hits;
    lk::function<void()> f = [p, k = 5]() { *p += k; };
    f();
    EXPECT_EQ(5, hits);

    // a bigger budget when asked for
    long a = 1, b = 2, c = 3, d = 4;
    lk::function<long(), 4 * sizeof(long)> wide = [a, b, c, d]() { return a + b + c + d; };
    EXPECT_EQ(10, wide());

    END_TEST;
}

bool function_lifetime() {
    BEGIN_TEST;

    int hits = 0;
    counted::live = 0;
    {
        // constructed, moved in, the temporary gone
        lk::function<void()> f = counted(&hits);
        EXPECT_EQ(1, counted::live);
        f();
        EXPECT_EQ(1, hits);

        // moving transfers the callable and empties the source
        lk::function<void()> g = std::move(f);
        EXPECT_FALSE(f);
        EXPECT_TRUE(g);
        EXPECT_EQ(1, counted::live);
        g();
        EXPECT_EQ(2, hits);

        // move assignment destroys what the target held
        lk::function<void()> h = counted(&hits);
        EXPECT_EQ(2, counted::live);
        h = std::move(g);
        EXPECT_EQ(1, counted::live);
        EXPECT_FALSE(g);
        h();
        EXPECT_EQ(3, hits);

        // so does assigning a new callable, and reset
        h = counted(&hits);
        EXPECT_EQ(1, counted::live);
        h.reset();
        EXPECT_EQ(0, counted::live);
        EXPECT_FALSE(h);
    }
    EXPECT_EQ(0, counted::live);

    END_TEST;
}

bool function_global() {
    BEGIN_TEST;

    EXPECT_FALSE(g_fn);
    g_fn = add_one;
    EXPECT_EQ(2, g_fn(1));
    g_fn = nullptr;
    EXPECT_FALSE(g_fn);

    END_TEST;
}

struct widget {
    int total = 0;
    int add(int x) {
        total += x;
        return total;
    }
    int peek() const { return total; }
    void clear() { total = 0; }
};

// the C API shapes, cookie first and cookie last
typedef int (*add_cookie_first_t)(void *cookie, int x);
typedef int (*add_cookie_last_t)(int x, void *cookie);
typedef int (*peek_cookie_first_t)(void *cookie);
typedef void (*clear_cookie_last_t)(void *cookie);

int apply(lk::function_ref<int(int)> fn, int v) { return fn(v); }

void visit3(lk::function_ref<void(int)> fn) {
    for (int i = 0; i < 3; i++) {
        fn(i);
    }
}

bool function_ref_basics() {
    BEGIN_TEST;

    // a plain function, a pointer to one, a lambda written in the argument list, a const
    // callable
    EXPECT_EQ(2, apply(add_one, 1));
    int (*fn_ptr)(int) = add_one;
    EXPECT_EQ(3, apply(fn_ptr, 2));
    EXPECT_EQ(9, apply([](int x) { return x * x; }, 3));
    const auto plus_five = [](int x) { return x + 5; };
    EXPECT_EQ(6, apply(plus_five, 1));

    // captures by reference, and a callable far larger than any inline budget
    int sum = 0;
    visit3([&sum](int i) { sum += i; });
    EXPECT_EQ(3, sum);
    long table[8] = { 0, 10, 20, 30, 40, 50, 60, 70 };
    auto lookup = [table](int i) { return static_cast<int>(table[i]); };
    EXPECT_EQ(20, apply(lookup, 2));

    // copies refer to the same callable
    int hits = 0;
    auto count = [&hits](int i) { hits += i; };
    lk::function_ref<void(int)> a = count;
    lk::function_ref<void(int)> b = a;
    a(1);
    b(2);
    EXPECT_EQ(3, hits);

    lk::function_ref<void(int)> empty;
    EXPECT_FALSE(empty);
    EXPECT_TRUE(a);

    END_TEST;
}

bool method_binding() {
    BEGIN_TEST;

    widget w;

    // a method with this captured, stored in a function: one word of capture
    static_assert(sizeof(decltype(lk::method<&widget::add>(&w))) == sizeof(void *));
    lk::function<int(int)> add = lk::method<&widget::add>(&w);
    EXPECT_EQ(3, add(3));
    EXPECT_EQ(7, add(4));
    EXPECT_EQ(7, w.total);

    // a const method on a const object, a void method
    const widget &cw = w;
    lk::function<int()> peek = lk::method<&widget::peek>(&cw);
    EXPECT_EQ(7, peek());
    lk::function<void()> clear = lk::method<&widget::clear>(&w);
    clear();
    EXPECT_EQ(0, w.total);

    // passed where a function_ref is wanted
    EXPECT_EQ(2, apply(lk::method<&widget::add>(&w), 2));

    // the C API shapes: the pointer has exactly the typedef's type, the object is the cookie
    add_cookie_first_t first = lk::method_cookie_first<&widget::add>;
    EXPECT_EQ(3, first(&w, 1));
    add_cookie_last_t last = lk::method_cookie_last<&widget::add>;
    EXPECT_EQ(5, last(2, &w));
    peek_cookie_first_t cpeek = lk::method_cookie_first<&widget::peek>;
    EXPECT_EQ(5, cpeek(&w));
    clear_cookie_last_t cclear = lk::method_cookie_last<&widget::clear>;
    cclear(&w);
    EXPECT_EQ(0, w.total);

    END_TEST;
}

BEGIN_TEST_CASE(function_tests)
RUN_TEST(function_basics)
RUN_TEST(function_state)
RUN_TEST(function_lifetime)
RUN_TEST(function_global)
RUN_TEST(function_ref_basics)
RUN_TEST(method_binding)
END_TEST_CASE(function_tests)

} // namespace
