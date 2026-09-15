/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

#include <kernel/thread.h>
#include <lib/bio.h>
#include <lib/cmdline.h>
#include <lib/fs.h>
#include <lib/unittest.h>
#include <lk/err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Tests run against a disk image created by mkimage.py in this directory; the
// layout and content expectations here mirror what that script builds. The
// driver is read-only, so on top of the in-guest checks the host side
// (scripts/run-ext2-tests.py) verifies the image hash is unchanged by the run.

// Returns the device name from the command line if configured and the device
// can be opened, otherwise NULL. Result is cached after the first call.
static const char *get_test_device(void) {
    static bool checked = false;
    static char device_name[128];
    static const char *result = NULL;

    if (!checked) {
        checked = true;
        size_t len = 0;
        status_t st = cmdline_get_string("test.ext2.device", device_name, sizeof(device_name), &len);
        if (st == NO_ERROR && len > 0) {
            bdev_t *bio = bio_open(device_name);
            if (bio) {
                bio_close(bio);
                result = device_name;
            }
        }
    }
    return result;
}

// Returns true if test.ext2.required is set on the command line, which turns a
// missing test.ext2.device into a hard failure instead of a silent skip.
static bool device_required(void) {
    static bool checked = false;
    static bool required = false;

    if (!checked) {
        checked = true;
        bool val = false;
        if (cmdline_get_bool("test.ext2.required", &val) == NO_ERROR) {
            required = val;
        }
    }
    return required;
}

#define test_path "/e2"

// Distinctive marker so a host-side harness can grep a boot log and tell a real
// run from a skipped one. Keep the spelling in sync with scripts/run-ext2-tests.py.
#define EXT2_SKIP_MARKER "EXT2-TEST-SKIPPED(no test.ext2.device)"

#define SKIP_TEST_IF_NO_DEVICE()                                                     \
    do {                                                                             \
        if (!get_test_device()) {                                                    \
            if (device_required()) {                                                 \
                UNITTEST_FAIL_TRACEF("test.ext2.required is set but test.ext2.device " \
                                     "is unset or the device could not be opened\n"); \
                return false;                                                        \
            }                                                                        \
            unittest_printf(" " EXT2_SKIP_MARKER " ");                               \
            return true;                                                             \
        }                                                                            \
    } while (0)

// Same position dependent pattern as mkimage.py generates.
static uint8_t pattern_byte(uint32_t seed, uint64_t offset) {
    uint32_t x = seed ^ (uint32_t)(offset * 2654435761u);
    x ^= x >> 15;
    x *= 2246822519u;
    x ^= x >> 13;
    return (uint8_t)x;
}

// Read [offset, offset+len) of an open file and compare against the pattern.
static bool check_pattern_window(filehandle *h, uint32_t seed, uint64_t offset, size_t len) {
    BEGIN_TEST;

    uint8_t *buf = malloc(len);
    ASSERT_NONNULL(buf, "window buffer");

    ssize_t r = fs_read_file(h, buf, offset, len);
    if ((size_t)r != len) {
        free(buf);
        UNITTEST_FAIL_TRACEF("read at %llu returned %zd, expected %zu\n", offset, r, len);
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        if (buf[i] != pattern_byte(seed, offset + i)) {
            unittest_printf("\n        content mismatch at offset %llu: expected %#x got %#x ",
                            offset + i, pattern_byte(seed, offset + i), buf[i]);
            free(buf);
            EXPECT_TRUE(false, "pattern mismatch");
            END_TEST;
        }
    }

    free(buf);
    END_TEST;
}

// Open a path and verify its entire content matches the given bytes.
static bool check_file_content(const char *path, const void *expected, size_t len) {
    BEGIN_TEST;

    filehandle *h;
    ASSERT_EQ(NO_ERROR, fs_open_file(path, &h), path);

    struct file_stat st;
    EXPECT_EQ(NO_ERROR, fs_stat_file(h, &st), "stat");
    EXPECT_EQ(len, st.size, "size");
    EXPECT_FALSE(st.is_dir, "not a dir");

    uint8_t *buf = malloc(len + 1);
    if (!buf) {
        fs_close_file(h);
        ASSERT_NONNULL(NULL, "content buffer");
    }

    // read one byte extra to prove the file really ends where stat says
    ssize_t r = fs_read_file(h, buf, 0, len + 1);
    EXPECT_EQ((ssize_t)len, r, "read length");
    if (r == (ssize_t)len && memcmp(buf, expected, len) != 0) {
        EXPECT_TRUE(false, "content mismatch");
    }

    free(buf);
    EXPECT_EQ(NO_ERROR, fs_close_file(h), "close");

    END_TEST;
}

static const char hello_content[] = "hello ext2\n";
#define HELLO_LEN (sizeof(hello_content) - 1)
static const char deep_content[] = "deep file\n";
#define DEEP_LEN (sizeof(deep_content) - 1)

static bool mount_test_fs(void) {
    return fs_mount(test_path, "ext2", get_test_device(), FS_MOUNT_OPTION_NONE) == NO_ERROR;
}

static bool test_ext2_mount(void) {
    BEGIN_TEST;
    SKIP_TEST_IF_NO_DEVICE();

    ASSERT_EQ(NO_ERROR, fs_mount(test_path, "ext2", get_test_device(), FS_MOUNT_OPTION_NONE), "mount");
    EXPECT_EQ(NO_ERROR, fs_unmount(test_path), "unmount");

    // an intrinsically read-only filesystem must accept a read-only mount
    ASSERT_EQ(NO_ERROR, fs_mount(test_path, "ext2", get_test_device(), FS_MOUNT_OPTION_READ_ONLY), "ro mount");
    EXPECT_EQ(NO_ERROR, fs_unmount(test_path), "ro unmount");

    END_TEST;
}

static bool test_ext2_read_files(void) {
    BEGIN_TEST;
    SKIP_TEST_IF_NO_DEVICE();

    ASSERT_TRUE(mount_test_fs(), "mount");

    EXPECT_TRUE(check_file_content(test_path "/hello.txt", hello_content, HELLO_LEN),
                "root file");
    EXPECT_TRUE(check_file_content(test_path "/dir1/dir2/dir3/deep.txt", deep_content, DEEP_LEN),
                "nested file");

    filehandle *h;
    EXPECT_EQ(ERR_NOT_FOUND, fs_open_file(test_path "/does_not_exist", &h), "missing file");
    EXPECT_EQ(ERR_NOT_FOUND, fs_open_file(test_path "/dir1/does_not_exist", &h), "missing nested");

    EXPECT_EQ(NO_ERROR, fs_unmount(test_path), "unmount");

    END_TEST;
}

static bool test_ext2_pattern_files(void) {
    BEGIN_TEST;
    SKIP_TEST_IF_NO_DEVICE();

    ASSERT_TRUE(mount_test_fs(), "mount");

    // the small one fits in direct blocks at either block size
    filehandle *h;
    ASSERT_EQ(NO_ERROR, fs_open_file(test_path "/pattern_small.bin", &h), "open small");
    struct file_stat st;
    EXPECT_EQ(NO_ERROR, fs_stat_file(h, &st), "stat small");
    EXPECT_EQ(4000u, st.size, "small size");
    EXPECT_TRUE(check_pattern_window(h, 0x1111, 0, 4000), "small content");
    EXPECT_EQ(NO_ERROR, fs_close_file(h), "close small");

    // the large one needs single and double indirect blocks at either block
    // size: with 1KB blocks the boundaries are at 12KB and 268KB, with 4KB
    // blocks at 48KB and ~4.14MB. sample windows across all of them.
    const uint64_t large_size = 6 * 1024 * 1024;
    ASSERT_EQ(NO_ERROR, fs_open_file(test_path "/pattern_large.bin", &h), "open large");
    EXPECT_EQ(NO_ERROR, fs_stat_file(h, &st), "stat large");
    EXPECT_EQ(large_size, st.size, "large size");

    EXPECT_TRUE(check_pattern_window(h, 0x2222, 0, 64 * 1024), "direct + 1k boundaries");
    EXPECT_TRUE(check_pattern_window(h, 0x2222, 240 * 1024, 64 * 1024), "1k double indirect boundary");
    EXPECT_TRUE(check_pattern_window(h, 0x2222, 4 * 1024 * 1024, 128 * 1024), "4k double indirect boundary");
    EXPECT_TRUE(check_pattern_window(h, 0x2222, large_size - 16 * 1024, 16 * 1024), "tail");

    // reading at EOF returns zero bytes, and reading past the tail is clamped
    uint8_t tailbuf[64];
    EXPECT_EQ(0, fs_read_file(h, tailbuf, large_size, sizeof(tailbuf)), "read at eof");
    EXPECT_EQ(32, fs_read_file(h, tailbuf, large_size - 32, sizeof(tailbuf)), "read clamped at eof");

    EXPECT_EQ(NO_ERROR, fs_close_file(h), "close large");
    EXPECT_EQ(NO_ERROR, fs_unmount(test_path), "unmount");

    END_TEST;
}

static bool test_ext2_symlinks(void) {
    BEGIN_TEST;
    SKIP_TEST_IF_NO_DEVICE();

    ASSERT_TRUE(mount_test_fs(), "mount");

    // a relative link resolves against the directory holding it. ".." is
    // flattened lexically by the layer and never reaches the driver.
    EXPECT_TRUE(check_file_content(test_path "/links/rel_link", hello_content, HELLO_LEN),
                "relative symlink");
    EXPECT_TRUE(check_file_content(test_path "/links/rel_deep", deep_content, DEEP_LEN),
                "multi component relative symlink");
    // a chain inside the layer's symlink depth limit
    EXPECT_TRUE(check_file_content(test_path "/links/chain1", hello_content, HELLO_LEN),
                "symlink chain");
    // a target longer than 60 bytes is stored in data blocks, not the inode
    EXPECT_TRUE(check_file_content(test_path "/links/long_link", hello_content, HELLO_LEN),
                "block-stored symlink");

    // a loop must fail cleanly, not hang or overflow
    filehandle *h;
    EXPECT_EQ(ERR_RECURSE_TOO_DEEP, fs_open_file(test_path "/links/loop1", &h), "symlink loop");

    // a symlink to a directory can be walked through as an intermediate
    // component, not just resolved as the last one
    EXPECT_TRUE(check_file_content(test_path "/links/dirlink/dir2/dir3/deep.txt",
                                   deep_content, DEEP_LEN),
                "symlink as an intermediate component");
    // and a directory symlink names a directory
    dirhandle *dh = NULL;
    EXPECT_EQ(NO_ERROR, fs_open_dir(test_path "/links/dirlink", &dh), "opendir through a symlink");
    if (dh) {
        EXPECT_EQ(NO_ERROR, fs_close_dir(dh), "closedir");
    }

    EXPECT_EQ(NO_ERROR, fs_unmount(test_path), "unmount");

    END_TEST;
}

// The fs layer resolves an absolute symlink target against the mount
// namespace root rather than the filesystem's own root, which is a change
// from the driver's old private walker. abs_link points at
// "/dir1/dir2/dir3/deep.txt": a real path inside the image, but not one that
// names anything while the image is mounted at /e2.
static const char cross_content[] = "from the other filesystem\n";
#define CROSS_LEN (sizeof(cross_content) - 1)

static bool test_ext2_absolute_symlink(void) {
    BEGIN_TEST;
    SKIP_TEST_IF_NO_DEVICE();

    ASSERT_TRUE(mount_test_fs(), "mount");

    filehandle *h;
    EXPECT_EQ(ERR_NOT_FOUND, fs_open_file(test_path "/links/abs_link", &h),
              "absolute target does not resolve inside the mount");

    // build that path out of a second filesystem: if the link now resolves,
    // it left the ext2 mount entirely
    ASSERT_EQ(NO_ERROR, fs_mount("/dir1", "memfs", NULL, FS_MOUNT_OPTION_NONE), "mount memfs");
    EXPECT_EQ(NO_ERROR, fs_make_dir("/dir1/dir2"), "mkdir dir2");
    EXPECT_EQ(NO_ERROR, fs_make_dir("/dir1/dir2/dir3"), "mkdir dir3");

    ASSERT_EQ(NO_ERROR, fs_create_file("/dir1/dir2/dir3/deep.txt", &h, CROSS_LEN), "create target");
    EXPECT_EQ((ssize_t)CROSS_LEN, fs_write_file(h, cross_content, 0, CROSS_LEN), "write target");
    EXPECT_EQ(NO_ERROR, fs_close_file(h), "close target");

    EXPECT_TRUE(check_file_content(test_path "/links/abs_link", cross_content, CROSS_LEN),
                "absolute symlink crosses into the other mount");

    EXPECT_EQ(NO_ERROR, fs_unmount("/dir1"), "unmount memfs");
    EXPECT_EQ(NO_ERROR, fs_unmount(test_path), "unmount");

    END_TEST;
}

static bool test_ext2_dirs(void) {
    BEGIN_TEST;
    SKIP_TEST_IF_NO_DEVICE();

    ASSERT_TRUE(mount_test_fs(), "mount");

    // a directory can be opened and stat says so
    filehandle *h;
    ASSERT_EQ(NO_ERROR, fs_open_file(test_path "/dir1", &h), "open dir");
    struct file_stat st;
    EXPECT_EQ(NO_ERROR, fs_stat_file(h, &st), "stat dir");
    EXPECT_TRUE(st.is_dir, "is_dir");
    // but reading it as a file fails
    char buf[16];
    EXPECT_GT(0, fs_read_file(h, buf, 0, sizeof(buf)), "read on dir fails");
    EXPECT_EQ(NO_ERROR, fs_close_file(h), "close dir");

    // walking through a file as if it were a directory fails
    EXPECT_GT(0, fs_open_file(test_path "/hello.txt/sub", &h), "file as dir component");

    // opening a file as a directory fails too
    dirhandle *dh;
    EXPECT_EQ(ERR_NOT_DIR, fs_open_dir(test_path "/hello.txt", &dh), "opendir on a file");

    EXPECT_EQ(NO_ERROR, fs_unmount(test_path), "unmount");

    END_TEST;
}

// Read a directory to exhaustion, checking that every expected name turns up
// exactly once and that "." and ".." never do.
static bool check_dir_listing(const char *path, const char *const *expected, size_t count) {
    BEGIN_TEST;

    dirhandle *dh;
    ASSERT_EQ(NO_ERROR, fs_open_dir(path, &dh), path);

    bool *seen = malloc(count * sizeof(bool));
    if (!seen) {
        fs_close_dir(dh);
        ASSERT_NONNULL(NULL, "seen buffer");
    }
    memset(seen, 0, count * sizeof(bool));

    struct dirent ent;
    status_t err;
    int entries = 0;
    // the test images hold nothing near this many entries in one directory,
    // so the bound only exists to stop a broken cursor from looping forever
    while (entries < 256 && (err = fs_read_dir(dh, &ent)) == NO_ERROR) {
        entries++;
        if (!strcmp(ent.name, ".") || !strcmp(ent.name, "..")) {
            unittest_printf("\n        readdir returned '%s' ", ent.name);
            EXPECT_TRUE(false, "dot entry listed");
            continue;
        }
        for (size_t i = 0; i < count; i++) {
            if (!strcmp(ent.name, expected[i])) {
                EXPECT_FALSE(seen[i], expected[i]);
                seen[i] = true;
            }
        }
    }
    EXPECT_EQ(ERR_NOT_FOUND, err, "end of directory");

    // and it stays at the end
    EXPECT_EQ(ERR_NOT_FOUND, fs_read_dir(dh, &ent), "stable end of directory");

    for (size_t i = 0; i < count; i++) {
        if (!seen[i]) {
            unittest_printf("\n        missing entry '%s' ", expected[i]);
            EXPECT_TRUE(false, "entry not listed");
        }
    }

    free(seen);
    EXPECT_EQ(NO_ERROR, fs_close_dir(dh), "closedir");

    END_TEST;
}

static bool test_ext2_readdir(void) {
    BEGIN_TEST;
    SKIP_TEST_IF_NO_DEVICE();

    ASSERT_TRUE(mount_test_fs(), "mount");

    // the root directory, which the driver's old private walker could not
    // reach through any api at all
    static const char *const root_entries[] = {
        "lost+found", "hello.txt", "dir1", "links",
        "pattern_small.bin", "pattern_large.bin",
        "a_directory_with_a_name_long_enough_to_defeat_inline_symlinks",
    };
    EXPECT_TRUE(check_dir_listing(test_path, root_entries, countof(root_entries)),
                "root listing");

    static const char *const links_entries[] = {
        "rel_link", "rel_deep", "abs_link", "dirlink",
        "chain1", "chain2", "chain3", "loop1", "loop2", "long_link",
    };
    EXPECT_TRUE(check_dir_listing(test_path "/links", links_entries, countof(links_entries)),
                "links listing");

    static const char *const dir1_entries[] = { "dir2" };
    EXPECT_TRUE(check_dir_listing(test_path "/dir1", dir1_entries, countof(dir1_entries)),
                "nested listing");

    // two cursors over one directory are independent
    dirhandle *a, *b;
    ASSERT_EQ(NO_ERROR, fs_open_dir(test_path "/dir1/dir2/dir3", &a), "opendir a");
    ASSERT_EQ(NO_ERROR, fs_open_dir(test_path "/dir1/dir2/dir3", &b), "opendir b");
    struct dirent ea, eb;
    EXPECT_EQ(NO_ERROR, fs_read_dir(a, &ea), "read a");
    EXPECT_EQ(NO_ERROR, fs_read_dir(b, &eb), "read b");
    EXPECT_EQ(0, strcmp(ea.name, "deep.txt"), "a first entry");
    EXPECT_EQ(0, strcmp(eb.name, "deep.txt"), "b first entry");
    EXPECT_EQ(ERR_NOT_FOUND, fs_read_dir(a, &ea), "a exhausted");
    EXPECT_EQ(NO_ERROR, fs_close_dir(a), "close a");
    EXPECT_EQ(ERR_NOT_FOUND, fs_read_dir(b, &eb), "b exhausted");
    EXPECT_EQ(NO_ERROR, fs_close_dir(b), "close b");

    EXPECT_EQ(NO_ERROR, fs_unmount(test_path), "unmount");

    END_TEST;
}

static bool test_ext2_readonly_ops(void) {
    BEGIN_TEST;
    SKIP_TEST_IF_NO_DEVICE();

    ASSERT_TRUE(mount_test_fs(), "mount");

    // every mutating operation is unsupported on the read-only driver
    filehandle *h;
    EXPECT_EQ(ERR_NOT_SUPPORTED, fs_create_file(test_path "/newfile", &h, 0), "create");
    EXPECT_EQ(ERR_NOT_SUPPORTED, fs_remove_file(test_path "/hello.txt"), "remove");
    EXPECT_EQ(ERR_NOT_SUPPORTED, fs_make_dir(test_path "/newdir"), "mkdir");
    EXPECT_EQ(ERR_NOT_SUPPORTED, fs_remove_dir(test_path "/dir1"), "rmdir");

    ASSERT_EQ(NO_ERROR, fs_open_file(test_path "/hello.txt", &h), "open");
    char buf[4] = "abc";
    EXPECT_EQ(ERR_NOT_SUPPORTED, fs_write_file(h, buf, 0, sizeof(buf)), "write");
    EXPECT_EQ(ERR_NOT_SUPPORTED, fs_truncate_file(h, 0), "truncate");
    EXPECT_EQ(NO_ERROR, fs_close_file(h), "close");

    EXPECT_EQ(NO_ERROR, fs_unmount(test_path), "unmount");

    END_TEST;
}

// A handful of threads reading one mount at once. The driver serializes on a
// per-mount lock, without which they would race in the shared block cache and,
// because that cache holds only four blocks and asserts once every one of them
// is referenced, could exhaust it outright. Each reader walks the path afresh
// every iteration so lookup runs concurrently too, and the windows are spread
// across the file so the readers contend for different indirect blocks.
#define CONCURRENT_READERS 4
#define CONCURRENT_ITERATIONS 8
#define CONCURRENT_WINDOW (16 * 1024)
#define CONCURRENT_SEED 0x2222  // the seed pattern_large.bin was built with

// Reads one window of pattern_large.bin, whose offset the argument points at.
// Returns NO_ERROR only if every byte of every read matched the pattern.
static int concurrent_reader(void *raw) {
    const uint64_t offset = *(const uint64_t *)raw;

    uint8_t *buf = malloc(CONCURRENT_WINDOW);
    if (!buf) {
        return ERR_NO_MEMORY;
    }

    int result = NO_ERROR;
    for (uint i = 0; i < CONCURRENT_ITERATIONS && result == NO_ERROR; i++) {
        filehandle *h;
        result = fs_open_file(test_path "/pattern_large.bin", &h);
        if (result != NO_ERROR) {
            break;
        }

        ssize_t r = fs_read_file(h, buf, offset, CONCURRENT_WINDOW);
        fs_close_file(h);
        if (r != (ssize_t)CONCURRENT_WINDOW) {
            result = ERR_IO;
            break;
        }

        for (size_t j = 0; j < CONCURRENT_WINDOW; j++) {
            if (buf[j] != pattern_byte(CONCURRENT_SEED, offset + j)) {
                result = ERR_CHECKSUM_FAIL;
                break;
            }
        }
    }

    free(buf);
    return result;
}

static bool test_ext2_concurrent_readers(void) {
    BEGIN_TEST;
    SKIP_TEST_IF_NO_DEVICE();

    ASSERT_TRUE(mount_test_fs(), "mount");

    uint64_t offsets[CONCURRENT_READERS];
    thread_t *threads[CONCURRENT_READERS];
    uint started = 0;

    for (uint i = 0; i < CONCURRENT_READERS; i++) {
        offsets[i] = (uint64_t)i * 1024 * 1024;

        char name[32];
        snprintf(name, sizeof(name), "ext2 reader %u", i);
        threads[i] = thread_create(name, concurrent_reader, &offsets[i],
                                   DEFAULT_PRIORITY, DEFAULT_STACK_SIZE);
        if (!threads[i]) {
            break;
        }
        started++;
        thread_resume(threads[i]);
    }

    for (uint i = 0; i < started; i++) {
        int retcode = ERR_GENERIC;
        EXPECT_EQ(NO_ERROR, thread_join(threads[i], &retcode, INFINITE_TIME), "join");
        EXPECT_EQ(NO_ERROR, retcode, "reader result");
    }
    EXPECT_EQ(CONCURRENT_READERS, started, "all readers started");

    EXPECT_EQ(NO_ERROR, fs_unmount(test_path), "unmount");

    END_TEST;
}

BEGIN_TEST_CASE(ext2_tests)
RUN_TEST(test_ext2_mount)
RUN_TEST(test_ext2_read_files)
RUN_TEST(test_ext2_pattern_files)
RUN_TEST(test_ext2_symlinks)
RUN_TEST(test_ext2_absolute_symlink)
RUN_TEST(test_ext2_dirs)
RUN_TEST(test_ext2_readdir)
RUN_TEST(test_ext2_readonly_ops)
RUN_TEST(test_ext2_concurrent_readers)
END_TEST_CASE(ext2_tests)
