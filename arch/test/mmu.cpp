/*
 * Copyright (c) 2020 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#if ARCH_HAS_MMU

#include <arch/mmu.h>

#include <lk/cpp.h>
#include <lk/debug.h>
#include <lk/err.h>
#include <lktl/auto_call.h>
#include <lib/unittest.h>
#include <arch/ops.h>
#include <kernel/event.h>
#include <kernel/mp.h>
#include <kernel/thread.h>
#include <kernel/vm.h>

namespace {

bool create_user_aspace() {
    BEGIN_TEST;

    if (arch_mmu_supports_user_aspaces()) {
        arch_aspace_t as;
        status_t err = arch_mmu_init_aspace(&as, USER_ASPACE_BASE, USER_ASPACE_SIZE, 0);
        ASSERT_EQ(NO_ERROR, err, "init aspace");

        err = arch_mmu_destroy_aspace(&as);
        EXPECT_EQ(NO_ERROR, err, "destroy");
    }

    END_TEST;
}

bool map_user_pages() {
    BEGIN_TEST;

    if (arch_mmu_supports_user_aspaces()) {
        arch_aspace_t as;
        status_t err = arch_mmu_init_aspace(&as, USER_ASPACE_BASE, USER_ASPACE_SIZE, 0);
        ASSERT_EQ(NO_ERROR, err, "init aspace");

        auto aspace_cleanup = lk::make_auto_call([&]() { arch_mmu_destroy_aspace(&as); });

        // allocate a batch of pages
        struct list_node pages = LIST_INITIAL_VALUE(pages);
        size_t count = pmm_alloc_pages(4, &pages);
        ASSERT_EQ(4U, count, "alloc pages");
        ASSERT_EQ(4U, list_length(&pages), "page list");

        auto pages_cleanup = lk::make_auto_call([&]() { pmm_free(&pages); });

        // map the pages into the address space
        vaddr_t va = USER_ASPACE_BASE;
        vm_page_t *p;
        list_for_every_entry(&pages, p, vm_page_t, node) {
            err = arch_mmu_map(&as, va, vm_page_to_paddr(p), 1, ARCH_MMU_FLAG_PERM_USER);
            EXPECT_LE(NO_ERROR, err, "map page");
            va += PAGE_SIZE;
        }

        // query the pages to make sure they match
        va = USER_ASPACE_BASE;
        list_for_every_entry(&pages, p, vm_page_t, node) {
            paddr_t pa;
            uint flags;
            err = arch_mmu_query(&as, va, &pa, &flags);
            EXPECT_EQ(NO_ERROR, err, "query");
            EXPECT_EQ(vm_page_to_paddr(p), pa, "pa");
            EXPECT_EQ(ARCH_MMU_FLAG_PERM_USER, flags, "flags");
            va += PAGE_SIZE;

            //unittest_printf("\npa %#lx, flags %#x", pa, flags);
        }

        // unmap them again, which also frees the tables they needed
        err = arch_mmu_unmap(&as, USER_ASPACE_BASE, count);
        EXPECT_LE(NO_ERROR, err, "unmap");

        // destroy the now empty aspace
        aspace_cleanup.cancel();
        err = arch_mmu_destroy_aspace(&as);
        EXPECT_EQ(NO_ERROR, err, "destroy");

        // free the pages we allocated before
        pages_cleanup.cancel();
        size_t freed = pmm_free(&pages);
        ASSERT_EQ(count, freed, "free");
    }

    END_TEST;
}

bool map_region_query_result(vmm_aspace_t *aspace, uint arch_flags) {
    BEGIN_TEST;
    void *ptr = NULL;

    // create a region of an arbitrary page in kernel aspace
    EXPECT_EQ(NO_ERROR, vmm_alloc(aspace, "test region", PAGE_SIZE, &ptr, 0, /* vmm_flags */ 0, arch_flags), "map region");
    EXPECT_NONNULL(ptr, "not null");

    // query the page to see if it's realistic
    {
        paddr_t pa = 0;
        uint flags = ~arch_flags;
        EXPECT_EQ(NO_ERROR, arch_mmu_query(&aspace->arch_aspace, (vaddr_t)ptr, &pa, &flags), "arch_query");
        EXPECT_NE(0U, pa, "valid pa");
        EXPECT_EQ(arch_flags, flags, "query flags");
    }

    // free this region we made
    EXPECT_EQ(NO_ERROR, vmm_free_region(aspace, (vaddr_t)ptr), "free region");

    // query that the page is not there anymore
    {
        paddr_t pa = 0;
        uint flags = ~arch_flags;
        EXPECT_EQ(ERR_NOT_FOUND, arch_mmu_query(&aspace->arch_aspace, (vaddr_t)ptr, &pa, &flags), "arch_query");
    }

    END_TEST;
}

bool map_region_expect_failure(vmm_aspace_t *aspace, uint arch_flags, int expected_error) {
    BEGIN_TEST;
    void *ptr = NULL;

    // create a region of an arbitrary page in kernel aspace
    EXPECT_EQ(expected_error, vmm_alloc(aspace, "test region", PAGE_SIZE, &ptr, 0, /* vmm_flags */ 0, arch_flags), "map region");
    EXPECT_NULL(ptr, "null");

    END_TEST;
}

bool map_query_pages() {
    BEGIN_TEST;

    vmm_aspace_t *kaspace = vmm_get_kernel_aspace();
    ASSERT_NONNULL(kaspace, "kaspace");

    // try mapping pages in the kernel address space with various permissions and read them back via arch query
    EXPECT_TRUE(map_region_query_result(kaspace, 0), "0");
    EXPECT_TRUE(map_region_query_result(kaspace, ARCH_MMU_FLAG_PERM_RO), "1");
    if (arch_mmu_supports_nx_mappings()) {
        EXPECT_TRUE(map_region_query_result(kaspace, ARCH_MMU_FLAG_PERM_NO_EXECUTE), "2");
        EXPECT_TRUE(map_region_query_result(kaspace, ARCH_MMU_FLAG_PERM_RO | ARCH_MMU_FLAG_PERM_NO_EXECUTE), "3");
    } else {
        EXPECT_TRUE(map_region_expect_failure(kaspace, ARCH_MMU_FLAG_PERM_NO_EXECUTE, ERR_INVALID_ARGS), "2");
        EXPECT_TRUE(map_region_expect_failure(kaspace, ARCH_MMU_FLAG_PERM_RO | ARCH_MMU_FLAG_PERM_NO_EXECUTE, ERR_INVALID_ARGS), "3");
    }

    EXPECT_TRUE(map_region_query_result(kaspace, ARCH_MMU_FLAG_PERM_USER), "4");
    EXPECT_TRUE(map_region_query_result(kaspace, ARCH_MMU_FLAG_PERM_USER | ARCH_MMU_FLAG_PERM_RO), "5");
    if (arch_mmu_supports_nx_mappings()) {
        EXPECT_TRUE(map_region_query_result(kaspace, ARCH_MMU_FLAG_PERM_USER | ARCH_MMU_FLAG_PERM_NO_EXECUTE), "6");
        EXPECT_TRUE(map_region_query_result(kaspace, ARCH_MMU_FLAG_PERM_USER | ARCH_MMU_FLAG_PERM_RO | ARCH_MMU_FLAG_PERM_NO_EXECUTE), "7");
    } else {
        EXPECT_TRUE(map_region_expect_failure(kaspace, ARCH_MMU_FLAG_PERM_USER | ARCH_MMU_FLAG_PERM_NO_EXECUTE, ERR_INVALID_ARGS), "6");
        EXPECT_TRUE(map_region_expect_failure(kaspace, ARCH_MMU_FLAG_PERM_USER | ARCH_MMU_FLAG_PERM_RO | ARCH_MMU_FLAG_PERM_NO_EXECUTE, ERR_INVALID_ARGS), "7");
    }

    END_TEST;
}

// The tests below read and write user addresses from kernel mode while a user
// aspace is active. That assumes the kernel may access user memory directly,
// which holds as long as nothing turns on SMAP (x86), PAN (arm64) or clears
// SUM (riscv).

// Fill a fresh page with a pattern and map it at va in the aspace.
vm_page_t *map_pattern_page(vmm_aspace_t *as, vaddr_t va, int pattern) {
    vm_page_t *p = pmm_alloc_page();
    if (!p) {
        return nullptr;
    }
    volatile int *kv = static_cast<volatile int *>(paddr_to_kvaddr(vm_page_to_paddr(p)));
    *kv = pattern;
    if (arch_mmu_map(&as->arch_aspace, va, vm_page_to_paddr(p), 1, ARCH_MMU_FLAG_PERM_USER) < 0) {
        pmm_free_page(p);
        return nullptr;
    }
    return p;
}

int read_user(vaddr_t va) {
    return *reinterpret_cast<volatile int *>(va);
}

// create a user aspace, map a page, make the aspace active and access the page through its low address
bool context_switch() {
    BEGIN_TEST;

    if (!arch_mmu_supports_user_aspaces()) {
        END_TEST;
    }

    vmm_aspace_t *as = nullptr;
    ASSERT_EQ(NO_ERROR, vmm_create_aspace(&as, "context_switch", 0), "create aspace");
    auto aspace_cleanup = lk::make_auto_call([&]() {
        vmm_set_active_aspace(nullptr);
        vmm_free_aspace(as);
    });

    vm_page_t *p = pmm_alloc_page();
    ASSERT_NONNULL(p, "page");
    auto page_cleanup = lk::make_auto_call([&]() { pmm_free_page(p); });

    int err = arch_mmu_map(&as->arch_aspace, USER_ASPACE_BASE, vm_page_to_paddr(p), 1, ARCH_MMU_FLAG_PERM_USER);
    ASSERT_LE(NO_ERROR, err, "map");

    // switch through the vmm so the scheduler knows which aspace this thread holds
    EXPECT_NULL(vmm_set_active_aspace(as), "no user aspace before");

    // write a known value to the kvaddr portion of the page and read it back through the low address
    volatile int *kv = static_cast<volatile int *>(paddr_to_kvaddr(vm_page_to_paddr(p)));
    *kv = 99;
    EXPECT_EQ(99, read_user(USER_ASPACE_BASE), "readback");
    *kv = 0xaa;
    EXPECT_EQ(0xaa, read_user(USER_ASPACE_BASE), "readback 2");

    // write to the page and read it back from the kernel side
    *reinterpret_cast<volatile int *>(USER_ASPACE_BASE) = 0x55;
    EXPECT_EQ(0x55, *kv, "readback 3");

    // switch back to the kernel aspace
    EXPECT_EQ(as, vmm_set_active_aspace(nullptr), "switch back");

    // unmap the page so the aspace is empty, then destroy it
    err = arch_mmu_unmap(&as->arch_aspace, USER_ASPACE_BASE, 1);
    EXPECT_LE(NO_ERROR, err, "unmap");
    aspace_cleanup.cancel();
    EXPECT_EQ(NO_ERROR, vmm_free_aspace(as), "free aspace");

    page_cleanup.cancel();
    EXPECT_EQ(1U, pmm_free_page(p), "free page");

    END_TEST;
}

// Unmap a page and map a different one at the same address while the aspace is
// active: a translation left in the TLB by the unmap would still read the old page.
bool unmap_user() {
    BEGIN_TEST;

    if (!arch_mmu_supports_user_aspaces()) {
        END_TEST;
    }

    vmm_aspace_t *as = nullptr;
    ASSERT_EQ(NO_ERROR, vmm_create_aspace(&as, "unmap_user", 0), "create aspace");
    auto aspace_cleanup = lk::make_auto_call([&]() {
        vmm_set_active_aspace(nullptr);
        arch_mmu_unmap(&as->arch_aspace, USER_ASPACE_BASE, 1);
        vmm_free_aspace(as);
    });

    vm_page_t *p1 = map_pattern_page(as, USER_ASPACE_BASE, 0x11);
    ASSERT_NONNULL(p1, "map first page");
    auto p1_cleanup = lk::make_auto_call([&]() { pmm_free_page(p1); });

    vmm_set_active_aspace(as);
    EXPECT_EQ(0x11, read_user(USER_ASPACE_BASE), "first page");

    EXPECT_LE(NO_ERROR, arch_mmu_unmap(&as->arch_aspace, USER_ASPACE_BASE, 1), "unmap");
    paddr_t pa;
    EXPECT_EQ(ERR_NOT_FOUND, arch_mmu_query(&as->arch_aspace, USER_ASPACE_BASE, &pa, nullptr), "gone");

    vm_page_t *p2 = map_pattern_page(as, USER_ASPACE_BASE, 0x22);
    ASSERT_NONNULL(p2, "map second page");
    auto p2_cleanup = lk::make_auto_call([&]() { pmm_free_page(p2); });
    EXPECT_EQ(0x22, read_user(USER_ASPACE_BASE), "second page, not a stale translation of the first");

    END_TEST;
}

// Two aspaces mapping the same address to different pages; switching between
// them must never show the other's page.
bool two_aspaces() {
    BEGIN_TEST;

    if (!arch_mmu_supports_user_aspaces()) {
        END_TEST;
    }

    vmm_aspace_t *a = nullptr;
    vmm_aspace_t *b = nullptr;
    ASSERT_EQ(NO_ERROR, vmm_create_aspace(&a, "two_aspaces_a", 0), "create a");
    auto a_cleanup = lk::make_auto_call([&]() {
        vmm_set_active_aspace(nullptr);
        arch_mmu_unmap(&a->arch_aspace, USER_ASPACE_BASE, 1);
        vmm_free_aspace(a);
    });
    ASSERT_EQ(NO_ERROR, vmm_create_aspace(&b, "two_aspaces_b", 0), "create b");
    auto b_cleanup = lk::make_auto_call([&]() {
        arch_mmu_unmap(&b->arch_aspace, USER_ASPACE_BASE, 1);
        vmm_free_aspace(b);
    });

    vm_page_t *pa = map_pattern_page(a, USER_ASPACE_BASE, 0xaa);
    ASSERT_NONNULL(pa, "map a");
    auto pa_cleanup = lk::make_auto_call([&]() { pmm_free_page(pa); });
    vm_page_t *pb = map_pattern_page(b, USER_ASPACE_BASE, 0xbb);
    ASSERT_NONNULL(pb, "map b");
    auto pb_cleanup = lk::make_auto_call([&]() { pmm_free_page(pb); });

    for (int i = 0; i < 4; i++) {
        vmm_set_active_aspace(a);
        EXPECT_EQ(0xaa, read_user(USER_ASPACE_BASE), "a's page");
        vmm_set_active_aspace(b);
        EXPECT_EQ(0xbb, read_user(USER_ASPACE_BASE), "b's page");
    }
    // via the kernel-only state in between as well
    vmm_set_active_aspace(nullptr);
    vmm_set_active_aspace(a);
    EXPECT_EQ(0xaa, read_user(USER_ASPACE_BASE), "a's page after kernel only");

    END_TEST;
}

// Create, use and destroy aspaces in a loop with alternating pages behind the
// same address. Where user aspaces share one asid this reuses it every time;
// with per aspace asids it exercises allocation and release.
bool aspace_churn() {
    BEGIN_TEST;

    if (!arch_mmu_supports_user_aspaces()) {
        END_TEST;
    }

    for (int i = 0; i < 32; i++) {
        vmm_aspace_t *as = nullptr;
        ASSERT_EQ(NO_ERROR, vmm_create_aspace(&as, "churn", 0), "create aspace");
        const int pattern = 0x1000 + i;
        vm_page_t *p = map_pattern_page(as, USER_ASPACE_BASE, pattern);
        ASSERT_NONNULL(p, "map");

        vmm_set_active_aspace(as);
        int seen = read_user(USER_ASPACE_BASE);
        vmm_set_active_aspace(nullptr);

        EXPECT_LE(NO_ERROR, arch_mmu_unmap(&as->arch_aspace, USER_ASPACE_BASE, 1), "unmap");
        EXPECT_EQ(NO_ERROR, vmm_free_aspace(as), "free aspace");
        pmm_free_page(p);

        if (seen != pattern) {
            EXPECT_EQ(pattern, seen, "page seen through a fresh aspace");
            break;
        }
    }

    END_TEST;
}

// ranges that leave the aspace are refused before anything is touched
bool out_of_range() {
    BEGIN_TEST;

    if (!arch_mmu_supports_user_aspaces()) {
        END_TEST;
    }

    vmm_aspace_t *as = nullptr;
    ASSERT_EQ(NO_ERROR, vmm_create_aspace(&as, "out_of_range", 0), "create aspace");
    auto aspace_cleanup = lk::make_auto_call([&]() { vmm_free_aspace(as); });

    vm_page_t *p = pmm_alloc_page();
    ASSERT_NONNULL(p, "page");
    auto page_cleanup = lk::make_auto_call([&]() { pmm_free_page(p); });
    const paddr_t pa = vm_page_to_paddr(p);
    arch_aspace_t *arch = &as->arch_aspace;

    // starts inside, runs past the top
    const vaddr_t last = USER_ASPACE_BASE + USER_ASPACE_SIZE - PAGE_SIZE;
    EXPECT_EQ(ERR_OUT_OF_RANGE, arch_mmu_map(arch, last, pa, 2, ARCH_MMU_FLAG_PERM_USER), "map past the top");
    EXPECT_EQ(ERR_OUT_OF_RANGE, arch_mmu_unmap(arch, last, 2), "unmap past the top");

    // entirely outside, in the kernel's half
    EXPECT_EQ(ERR_OUT_OF_RANGE, arch_mmu_map(arch, KERNEL_ASPACE_BASE, pa, 1, ARCH_MMU_FLAG_PERM_USER), "map kernel address");
    EXPECT_EQ(ERR_OUT_OF_RANGE, arch_mmu_unmap(arch, KERNEL_ASPACE_BASE, 1), "unmap kernel address");
    paddr_t qpa;
    EXPECT_EQ(ERR_OUT_OF_RANGE, arch_mmu_query(arch, KERNEL_ASPACE_BASE, &qpa, nullptr), "query kernel address");

    // just below the aspace, when there is room below it
    if (USER_ASPACE_BASE >= PAGE_SIZE) {
        EXPECT_EQ(ERR_OUT_OF_RANGE, arch_mmu_map(arch, USER_ASPACE_BASE - PAGE_SIZE, pa, 1, ARCH_MMU_FLAG_PERM_USER), "map below");
    }

    // the last page by itself is fine
    EXPECT_LE(NO_ERROR, arch_mmu_map(arch, last, pa, 1, ARCH_MMU_FLAG_PERM_USER), "map last page");
    EXPECT_EQ(NO_ERROR, arch_mmu_query(arch, last, &qpa, nullptr), "query last page");
    EXPECT_EQ(pa, qpa, "last page pa");
    EXPECT_LE(NO_ERROR, arch_mmu_unmap(arch, last, 1), "unmap last page");

    END_TEST;
}

// Pages far apart need their own intermediate tables; after unmapping them the
// aspace must be back to just its top level table, which destroy asserts.
bool table_reclaim() {
    BEGIN_TEST;

    if (!arch_mmu_supports_user_aspaces()) {
        END_TEST;
    }

    vmm_aspace_t *as = nullptr;
    ASSERT_EQ(NO_ERROR, vmm_create_aspace(&as, "table_reclaim", 0), "create aspace");
    auto aspace_cleanup = lk::make_auto_call([&]() { vmm_free_aspace(as); });

    // spread over the aspace so no two share a lower level table; the size
    // need not be a multiple of four pages, so round the middle ones down
    const vaddr_t vas[] = {
        USER_ASPACE_BASE,
        ROUNDDOWN(USER_ASPACE_BASE + USER_ASPACE_SIZE / 4, PAGE_SIZE),
        ROUNDDOWN(USER_ASPACE_BASE + USER_ASPACE_SIZE / 2, PAGE_SIZE),
        USER_ASPACE_BASE + USER_ASPACE_SIZE - PAGE_SIZE,
    };
    vm_page_t *pages[countof(vas)] = {};
    auto pages_cleanup = lk::make_auto_call([&]() {
        for (auto *p : pages) {
            if (p) pmm_free_page(p);
        }
    });

    for (size_t i = 0; i < countof(vas); i++) {
        pages[i] = map_pattern_page(as, vas[i], (int)i);
        ASSERT_NONNULL(pages[i], "map");
    }
    vmm_set_active_aspace(as);
    for (size_t i = 0; i < countof(vas); i++) {
        EXPECT_EQ((int)i, read_user(vas[i]), "read back");
    }
    vmm_set_active_aspace(nullptr);
    for (size_t i = 0; i < countof(vas); i++) {
        EXPECT_LE(NO_ERROR, arch_mmu_unmap(&as->arch_aspace, vas[i], 1), "unmap");
        paddr_t pa;
        EXPECT_EQ(ERR_NOT_FOUND, arch_mmu_query(&as->arch_aspace, vas[i], &pa, nullptr), "gone");
    }

    aspace_cleanup.cancel();
    EXPECT_EQ(NO_ERROR, vmm_free_aspace(as), "free aspace with only the top table left");

    END_TEST;
}

// A thread on another cpu keeps the aspace loaded and a translation of va
// cached while this cpu unmaps the page and maps another one at the same
// address. The unmap's shootdown must reach that cpu or it reads the old page.
struct shootdown_args {
    vmm_aspace_t *aspace;
    vaddr_t va;
    event_t loaded;
    volatile int phase;   // 0: hold off, 1: read again and finish
    volatile int first;
    volatile int second;
};

static int shootdown_thread(void *arg) {
    auto *args = static_cast<shootdown_args *>(arg);

    vmm_set_active_aspace(args->aspace);
    args->first = read_user(args->va);
    event_signal(&args->loaded, true);

    // keep the aspace loaded, and the translation cached, without touching va
    while (__atomic_load_n(&args->phase, __ATOMIC_ACQUIRE) == 0) {
        arch_spinloop_pause();
    }
    args->second = read_user(args->va);

    vmm_set_active_aspace(nullptr);
    return 0;
}

bool smp_shootdown() {
    BEGIN_TEST;

    if (!arch_mmu_supports_user_aspaces()) {
        END_TEST;
    }

    // stay off the cpu the other thread runs on, or landing there would switch it out
    thread_t *self = get_current_thread();
    thread_set_pinned_cpu(self, arch_curr_cpu_num());
    thread_yield();
    auto unpin = lk::make_auto_call([&]() { thread_set_pinned_cpu(self, -1); });
    const mp_cpu_mask_t others = mp_get_active_mask() & ~(1U << arch_curr_cpu_num());
    if (others == 0) {
        unittest_printf(" (needs a second cpu)");
        END_TEST;
    }
    const uint cpu = __builtin_ctz(others);

    shootdown_args args = {};
    ASSERT_EQ(NO_ERROR, vmm_create_aspace(&args.aspace, "shootdown", 0), "create aspace");
    auto aspace_cleanup = lk::make_auto_call([&]() {
        arch_mmu_unmap(&args.aspace->arch_aspace, USER_ASPACE_BASE, 1);
        vmm_free_aspace(args.aspace);
    });
    args.va = USER_ASPACE_BASE;
    event_init(&args.loaded, false, 0);

    vm_page_t *p1 = map_pattern_page(args.aspace, args.va, 0x11);
    ASSERT_NONNULL(p1, "map first page");
    auto p1_cleanup = lk::make_auto_call([&]() { pmm_free_page(p1); });

    thread_t *t = thread_create("shootdown", shootdown_thread, &args, DEFAULT_PRIORITY, DEFAULT_STACK_SIZE);
    ASSERT_NONNULL(t, "thread_create");
    thread_set_pinned_cpu(t, cpu);
    thread_resume(t);
    ASSERT_EQ(NO_ERROR, event_wait_timeout(&args.loaded, 5000), "other cpu loaded the aspace");
    EXPECT_EQ(0x11, args.first, "other cpu read the first page");

    // replace the page under it
    EXPECT_LE(NO_ERROR, arch_mmu_unmap(&args.aspace->arch_aspace, args.va, 1), "unmap");
    vm_page_t *p2 = map_pattern_page(args.aspace, args.va, 0x22);
    ASSERT_NONNULL(p2, "map second page");
    auto p2_cleanup = lk::make_auto_call([&]() { pmm_free_page(p2); });

    __atomic_store_n(&args.phase, 1, __ATOMIC_RELEASE);
    int retcode = -1;
    EXPECT_EQ(NO_ERROR, thread_join(t, &retcode, 5000), "join");
    EXPECT_EQ(0x22, args.second, "other cpu saw the new page, not its cached translation");

    END_TEST;
}

// Holds an aspace loaded on its (pinned) cpu until told to let go. It spins
// rather than blocks: a blocked thread is switched out, and the switch unloads
// the aspace from the cpu, which is exactly the state this test must avoid.
struct busy_aspace_args {
    vmm_aspace_t *aspace;
    event_t loaded;
    volatile int release;
};

static int busy_aspace_thread(void *arg) {
    auto *args = static_cast<busy_aspace_args *>(arg);

    vmm_set_active_aspace(args->aspace);
    event_signal(&args->loaded, true);
    while (!__atomic_load_n(&args->release, __ATOMIC_ACQUIRE)) {
        arch_spinloop_pause();
    }
    vmm_set_active_aspace(nullptr);
    return 0;
}

bool free_active_aspace() {
    BEGIN_TEST;

    if (!arch_mmu_supports_user_aspaces()) {
        END_TEST;
    }

    // freeing an aspace the current thread has loaded switches it off first and succeeds
    vmm_aspace_t *as = nullptr;
    ASSERT_EQ(NO_ERROR, vmm_create_aspace(&as, "free_active", 0), "create");
    vmm_set_active_aspace(as);
    EXPECT_EQ(NO_ERROR, vmm_free_aspace(as), "free while active on this cpu");
    EXPECT_NULL(get_current_thread()->aspace, "switched off");

    // with another cpu holding it, the free is refused until that cpu lets go.
    // Pin ourselves so we cannot land on the cpu holding it and unload it by
    // being scheduled there.
    thread_t *self = get_current_thread();
    thread_set_pinned_cpu(self, arch_curr_cpu_num());
    thread_yield();
    const uint my_cpu = arch_curr_cpu_num();
    const mp_cpu_mask_t others = mp_get_active_mask() & ~(1U << my_cpu);
    if (others != 0) {
        const uint cpu = __builtin_ctz(others);

        busy_aspace_args args = {};
        ASSERT_EQ(NO_ERROR, vmm_create_aspace(&args.aspace, "free_busy", 0), "create");
        event_init(&args.loaded, false, 0);

        thread_t *t = thread_create("busy_aspace", busy_aspace_thread, &args,
                                    DEFAULT_PRIORITY, DEFAULT_STACK_SIZE);
        ASSERT_NONNULL(t, "thread_create");
        thread_set_pinned_cpu(t, cpu);
        thread_resume(t);
        ASSERT_EQ(NO_ERROR, event_wait_timeout(&args.loaded, 5000), "aspace loaded on other cpu");

        // only an arch that counts loaded cpus can refuse; the others report 0
        if (arch_aspace_active_cpus(&args.aspace->arch_aspace) > 0) {
            EXPECT_EQ(ERR_BUSY, vmm_free_aspace(args.aspace), "free while loaded elsewhere");
        } else {
            unittest_printf(" (arch does not track loaded cpus)");
        }

        __atomic_store_n(&args.release, 1, __ATOMIC_RELEASE);
        int retcode = -1;
        EXPECT_EQ(NO_ERROR, thread_join(t, &retcode, 5000), "join");
        EXPECT_EQ(0, retcode, "thread return");

        EXPECT_EQ(NO_ERROR, vmm_free_aspace(args.aspace), "free after release");
    }
    thread_set_pinned_cpu(self, -1);

    END_TEST;
}

BEGIN_TEST_CASE(arch_mmu_tests)
RUN_TEST(create_user_aspace);
RUN_TEST(map_user_pages);
RUN_TEST(map_query_pages);
RUN_TEST(context_switch);
RUN_TEST(unmap_user);
RUN_TEST(two_aspaces);
RUN_TEST(aspace_churn);
RUN_TEST(out_of_range);
RUN_TEST(table_reclaim);
RUN_TEST(free_active_aspace);
RUN_TEST(smp_shootdown);
END_TEST_CASE(arch_mmu_tests)

} // namespace

#endif // ARCH_HAS_MMU
