/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

#if WITH_KERNEL_VM

#include <kernel/vm/asid.h>
#include <lib/unittest.h>
#include <lk/err.h>
#include <string.h>

/* 8KB of bitmap, so keep one instance rather than one per test */
static asid_allocator_t test_alloc;

/* ids come out in order starting at the first user id */
static bool test_asid_sequential(void) {
    BEGIN_TEST;

    asid_allocator_init(&test_alloc, ASID_MAX);

    uint16_t asid = ASID_UNUSED;
    for (uint expected = ASID_FIRST_USER; expected < ASID_FIRST_USER + 8; expected++) {
        EXPECT_EQ(NO_ERROR, asid_alloc(&test_alloc, &asid), "alloc");
        EXPECT_EQ(expected, asid, "asid");
    }

    END_TEST;
}

/* a freed id is not the next one handed out; the search goes round first */
static bool test_asid_rotate(void) {
    BEGIN_TEST;

    asid_allocator_init(&test_alloc, ASID_MAX);

    uint16_t a, b, c;
    ASSERT_EQ(NO_ERROR, asid_alloc(&test_alloc, &a), "alloc a");
    ASSERT_EQ(NO_ERROR, asid_alloc(&test_alloc, &b), "alloc b");
    asid_free(&test_alloc, a);
    ASSERT_EQ(NO_ERROR, asid_alloc(&test_alloc, &c), "alloc c");
    EXPECT_EQ(b + 1, c, "next id, not the freed one");

    END_TEST;
}

/* a tiny id space: exhaustion, wrap around, and reuse after free */
static bool test_asid_small_space(void) {
    BEGIN_TEST;

    asid_allocator_init(&test_alloc, 5);

    uint16_t asid = ASID_UNUSED;
    for (uint expected = 2; expected <= 5; expected++) {
        EXPECT_EQ(NO_ERROR, asid_alloc(&test_alloc, &asid), "alloc");
        EXPECT_EQ(expected, asid, "asid");
    }
    EXPECT_EQ(ERR_NO_MEMORY, asid_alloc(&test_alloc, &asid), "exhausted");

    /* only 3 is free: the search wraps to find it */
    asid_free(&test_alloc, 3);
    EXPECT_EQ(NO_ERROR, asid_alloc(&test_alloc, &asid), "alloc after free");
    EXPECT_EQ(3, asid, "wrapped to the freed id");

    /* free 2 and 4: the search continues past 3 to 4, then wraps to 2 */
    asid_free(&test_alloc, 2);
    asid_free(&test_alloc, 4);
    EXPECT_EQ(NO_ERROR, asid_alloc(&test_alloc, &asid), "alloc");
    EXPECT_EQ(4, asid, "continues after the last id");
    EXPECT_EQ(NO_ERROR, asid_alloc(&test_alloc, &asid), "alloc");
    EXPECT_EQ(2, asid, "wraps to the lowest free id");
    EXPECT_EQ(ERR_NO_MEMORY, asid_alloc(&test_alloc, &asid), "exhausted again");

    END_TEST;
}

/* every id in an 8 bit space is handed out exactly once */
static bool test_asid_exhaust_8bit(void) {
    BEGIN_TEST;

    asid_allocator_init(&test_alloc, 0xff);

    bool seen[256] = {};
    int count = 0;
    uint16_t asid = ASID_UNUSED;
    while (asid_alloc(&test_alloc, &asid) == NO_ERROR) {
        ASSERT_LE(ASID_FIRST_USER, asid, "below the user range");
        ASSERT_GE(0xff, asid, "above the maximum");
        EXPECT_FALSE(seen[asid], "handed out twice");
        seen[asid] = true;
        count++;
    }
    EXPECT_EQ(256 - ASID_FIRST_USER, count, "ids handed out");

    /* release them all and the full space is available again */
    for (uint i = ASID_FIRST_USER; i <= 0xff; i++) {
        asid_free(&test_alloc, i);
    }
    EXPECT_EQ(NO_ERROR, asid_alloc(&test_alloc, &asid), "alloc after freeing all");

    END_TEST;
}

/* the full 16 bit space, which crosses many bitmap words */
static bool test_asid_exhaust_16bit(void) {
    BEGIN_TEST;

    asid_allocator_init(&test_alloc, ASID_MAX);

    uint16_t asid = ASID_UNUSED;
    int count = 0;
    uint16_t last = ASID_FIRST_USER - 1;
    int out_of_order = 0;
    while (asid_alloc(&test_alloc, &asid) == NO_ERROR) {
        if (asid != last + 1) {
            out_of_order++;
        }
        last = asid;
        count++;
    }
    EXPECT_EQ(ASID_MAX + 1 - ASID_FIRST_USER, count, "ids handed out");
    EXPECT_EQ(0, out_of_order, "ids not sequential");

    /* one hole in the middle of a word is found again */
    asid_free(&test_alloc, 0x1234);
    EXPECT_EQ(NO_ERROR, asid_alloc(&test_alloc, &asid), "alloc into the hole");
    EXPECT_EQ(0x1234, asid, "the freed id");
    EXPECT_EQ(ERR_NO_MEMORY, asid_alloc(&test_alloc, &asid), "exhausted");

    END_TEST;
}

BEGIN_TEST_CASE(asid_tests)
RUN_TEST(test_asid_sequential);
RUN_TEST(test_asid_rotate);
RUN_TEST(test_asid_small_space);
RUN_TEST(test_asid_exhaust_8bit);
RUN_TEST(test_asid_exhaust_16bit);
END_TEST_CASE(asid_tests)

#endif // WITH_KERNEL_VM
