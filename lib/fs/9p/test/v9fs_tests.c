/*
 * Copyright (c) 2024 Cody Wong
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#include <lib/bio.h>
#include <lib/cmdline.h>
#include <lib/fs.h>
#include <lib/unittest.h>
#include <lk/err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Unit tests for lib/fs/9p, run against a live virtio-9p host share (e.g.
// scripts/do-qemuarm -f <dir>). The suite skips when no 9p device is present,
// so it is safe in every ut all run; set test.v9fs.required=1 on the kernel
// command line to turn a missing device into a hard failure.
//
// The suite cleans up after itself, and removes anything a previous run left
// behind before it starts, so it can be run repeatedly against one share.
//
// The do-qemu* scripts export the share with security_model=mapped, which
// keeps each file's mode in a user.virtfs.* extended attribute. Point -f at a
// filesystem that has no xattrs (an NFS mount, say) and every create fails
// with an I/O error that looks like a driver bug and is not one.

#define V9FS_MOUNT_POINT "/v9p"
#define V9FS_NAME        "9p"
#define V9P_BDEV_NAME    "v9p0"

#define TEST_FILE V9FS_MOUNT_POINT "/lk_v9fs_test_file"
#define TEST_DIR  V9FS_MOUNT_POINT "/lk_v9fs_test_dir"

// Distinctive marker so a host-side harness can grep a boot log and tell a
// real run from a skipped one.
#define V9FS_SKIP_MARKER "V9FS-TEST-SKIPPED(no 9p device)"

// Returns true if the 9p block device exists. Result is cached after the
// first call.
static bool have_device(void) {
    static bool checked = false;
    static bool present = false;

    if (!checked) {
        checked = true;
        bdev_t *bio = bio_open(V9P_BDEV_NAME);
        if (bio) {
            bio_close(bio);
            present = true;
        }
    }
    return present;
}

static bool device_required(void) {
    static bool checked = false;
    static bool required = false;

    if (!checked) {
        checked = true;
        bool val = false;
        if (cmdline_get_bool("test.v9fs.required", &val) == NO_ERROR) {
            required = val;
        }
    }
    return required;
}

#define SKIP_TEST_IF_NO_DEVICE()                                                  \
    do {                                                                          \
        if (!have_device()) {                                                     \
            if (device_required()) {                                              \
                UNITTEST_FAIL_TRACEF("test.v9fs.required is set but no 9p "       \
                                     "device is present\n");                      \
                return false;                                                     \
            }                                                                     \
            unittest_printf(" " V9FS_SKIP_MARKER " ");                            \
            return true;                                                          \
        }                                                                         \
    } while (0)

static bool mount_v9fs(void) {
    return fs_mount(V9FS_MOUNT_POINT, V9FS_NAME, V9P_BDEV_NAME,
                    FS_MOUNT_OPTION_NONE) == NO_ERROR;
}

// The share's content is not under our control, so file data is verified with
// a position dependent pattern we write ourselves.
static uint8_t pattern_byte(uint32_t seed, uint64_t offset) {
    uint32_t x = seed ^ (uint32_t)(offset * 2654435761u);
    x ^= x >> 15;
    x *= 2246822519u;
    x ^= x >> 13;
    return (uint8_t)x;
}

// The suite removes everything it creates, so a run leaves the share as it
// found it. A previous run that died partway can still have left something
// behind, though, so anything about to be created is removed first.
static void remove_if_present(const char *path) {
    fs_remove_file(path);
}

static void remove_dir_if_present(const char *path) {
    fs_remove_dir(path);
}

static bool test_v9fs_mount(void) {
    BEGIN_TEST;
    SKIP_TEST_IF_NO_DEVICE();

    ASSERT_TRUE(mount_v9fs(), "mount");
    EXPECT_EQ(NO_ERROR, fs_unmount(V9FS_MOUNT_POINT), "unmount");

    // a second cycle proves unmount really released the attach fid
    ASSERT_TRUE(mount_v9fs(), "second mount");

    EXPECT_EQ(NO_ERROR, fs_unmount(V9FS_MOUNT_POINT), "second unmount");

    // Mounting over an occupied point is refused by the layer only after
    // v9fs_mount has already attached and built a root vnode, so the failure
    // path has to release that vnode. 9p is the one filesystem whose release()
    // reaches its instance through the vnode's cookie, which makes this the
    // case that catches the layer handing it an unbound one. Something other
    // than 9p has to be holding the point: a second attach to the same device
    // fails inside the driver, before the layer gets a chance to object.
    ASSERT_EQ(NO_ERROR, fs_mount(V9FS_MOUNT_POINT, "memfs", NULL, FS_MOUNT_OPTION_NONE),
              "occupy the mount point");
    EXPECT_EQ(ERR_ALREADY_MOUNTED,
              fs_mount(V9FS_MOUNT_POINT, V9FS_NAME, V9P_BDEV_NAME, FS_MOUNT_OPTION_NONE),
              "mount over an occupied point");
    EXPECT_EQ(NO_ERROR, fs_unmount(V9FS_MOUNT_POINT), "release the mount point");

    // the refused attempt must have detached cleanly, attach fid and all
    ASSERT_TRUE(mount_v9fs(), "mount after the refusal");
    EXPECT_EQ(NO_ERROR, fs_unmount(V9FS_MOUNT_POINT), "final unmount");

    END_TEST;
}

static bool test_v9fs_file_io(void) {
    BEGIN_TEST;
    SKIP_TEST_IF_NO_DEVICE();

    ASSERT_TRUE(mount_v9fs(), "mount");
    remove_if_present(TEST_FILE);

    // creating at the top of the mount exercises the root-level create path
    // (which used to send "/name", slash included, to the server)
    filehandle *h;
    ASSERT_EQ(NO_ERROR, fs_create_file(TEST_FILE, &h, 0), "create");

    // creating over an existing name is refused rather than truncating it
    filehandle *h2;
    EXPECT_EQ(ERR_ALREADY_EXISTS, fs_create_file(TEST_FILE, &h2, 0), "create twice");

    enum { kLen = 4096 };
    uint8_t *buf = malloc(kLen);
    ASSERT_NONNULL(buf, "buffer");

    for (size_t i = 0; i < kLen; i++) {
        buf[i] = pattern_byte(0x9999, i);
    }
    EXPECT_EQ((ssize_t)kLen, fs_write_file(h, buf, 0, kLen), "write");

    // stat goes to the server; a failure here used to leave the file mutex held
    struct file_stat st;
    EXPECT_EQ(NO_ERROR, fs_stat_file(h, &st), "stat");
    EXPECT_EQ((uint64_t)kLen, st.size, "stat size");
    EXPECT_FALSE(st.is_dir, "not a dir");

    EXPECT_EQ(NO_ERROR, fs_close_file(h), "close");

    // read it back through a fresh handle. writes are write-through, so this
    // reaches the host's copy rather than anything cached on this side
    ASSERT_EQ(NO_ERROR, fs_open_file(TEST_FILE, &h), "reopen");
    memset(buf, 0, kLen);
    EXPECT_EQ((ssize_t)kLen, fs_read_file(h, buf, 0, kLen), "read");
    bool match = true;
    for (size_t i = 0; i < kLen; i++) {
        if (buf[i] != pattern_byte(0x9999, i)) {
            unittest_printf("\n        content mismatch at offset %zu ", i);
            match = false;
            break;
        }
    }
    EXPECT_TRUE(match, "content");

    // an unaligned range running off the end of the file reads what is there
    // and stops; past the end it reads nothing
    memset(buf, 0, kLen);
    EXPECT_EQ((ssize_t)96, fs_read_file(h, buf, 4000, 100), "tail read");
    EXPECT_EQ(pattern_byte(0x9999, 4000), buf[0], "tail content");
    EXPECT_EQ((ssize_t)0, fs_read_file(h, buf, kLen, 16), "read past the end");

    EXPECT_EQ(NO_ERROR, fs_close_file(h), "close reopened");

    free(buf);

    EXPECT_EQ(ERR_NOT_FOUND, fs_open_file(V9FS_MOUNT_POINT "/lk_v9fs_no_such_file", &h),
              "missing file");

    EXPECT_EQ(NO_ERROR, fs_remove_file(TEST_FILE), "remove");
    EXPECT_EQ(ERR_NOT_FOUND, fs_open_file(TEST_FILE, &h), "removed file is gone");

    EXPECT_EQ(NO_ERROR, fs_unmount(V9FS_MOUNT_POINT), "unmount");

    END_TEST;
}

static bool test_v9fs_dirs(void) {
    BEGIN_TEST;
    SKIP_TEST_IF_NO_DEVICE();

    ASSERT_TRUE(mount_v9fs(), "mount");
    remove_if_present(TEST_DIR "/nested");
    remove_dir_if_present(TEST_DIR);

    // root-level mkdir exercises the same leading-slash path as create
    ASSERT_EQ(NO_ERROR, fs_make_dir(TEST_DIR), "mkdir");
    EXPECT_EQ(ERR_ALREADY_EXISTS, fs_make_dir(TEST_DIR), "mkdir twice");

    filehandle *h;
    ASSERT_EQ(NO_ERROR, fs_create_file(TEST_DIR "/nested", &h, 0), "nested create");
    EXPECT_EQ(NO_ERROR, fs_close_file(h), "nested close");

    // enumerate the directory: the file is listed, "." and ".." are not, and
    // the end of the listing is ERR_NOT_FOUND like every other filesystem
    dirhandle *dh;
    ASSERT_EQ(NO_ERROR, fs_open_dir(TEST_DIR, &dh), "opendir");
    bool seen = false, saw_dot = false, saw_dotdot = false;
    struct dirent ent;
    status_t rerr;
    while ((rerr = fs_read_dir(dh, &ent)) == NO_ERROR) {
        if (!strcmp(ent.name, "nested")) seen = true;
        if (!strcmp(ent.name, ".")) saw_dot = true;
        if (!strcmp(ent.name, "..")) saw_dotdot = true;
    }
    EXPECT_EQ(ERR_NOT_FOUND, rerr, "readdir eof");
    EXPECT_TRUE(seen, "nested file enumerated");
    EXPECT_FALSE(saw_dot, "no . entry");
    EXPECT_FALSE(saw_dotdot, "no .. entry");
    EXPECT_EQ(NO_ERROR, fs_close_dir(dh), "closedir");

    // "." and ".." are still resolvable as path components, because the layer
    // flattens them before the filesystem ever sees them
    ASSERT_EQ(NO_ERROR, fs_open_file(TEST_DIR "/./nested", &h), "open via .");
    EXPECT_EQ(NO_ERROR, fs_close_file(h), "close via .");
    ASSERT_EQ(NO_ERROR, fs_open_file(TEST_DIR "/../lk_v9fs_test_dir/nested", &h),
              "open via ..");
    EXPECT_EQ(NO_ERROR, fs_close_file(h), "close via ..");

    // a file is not openable as a directory
    EXPECT_EQ(ERR_NOT_DIR, fs_open_dir(TEST_DIR "/nested", &dh), "file as dir");

    // and the two removal ops do not cross over
    EXPECT_EQ(ERR_NOT_FILE, fs_remove_file(TEST_DIR), "remove_file on a dir");
    EXPECT_EQ(ERR_NOT_DIR, fs_remove_dir(TEST_DIR "/nested"), "remove_dir on a file");

    EXPECT_EQ(NO_ERROR, fs_remove_file(TEST_DIR "/nested"), "remove nested");
    EXPECT_EQ(NO_ERROR, fs_remove_dir(TEST_DIR), "rmdir");

    EXPECT_EQ(NO_ERROR, fs_unmount(V9FS_MOUNT_POINT), "unmount");

    END_TEST;
}

// A directory is walked through and enumerated with two different fids,
// because 9P2000 says a walk may not start from a fid that has been opened
// for I/O. This is the ordering that would exercise the rule -- though not
// against qemu, which permits the walk out of an opened fid regardless, so
// collapsing the two fids would not fail here. It would on a server that
// enforces the rule.
static bool test_v9fs_walk_after_opendir(void) {
    BEGIN_TEST;
    SKIP_TEST_IF_NO_DEVICE();

    ASSERT_TRUE(mount_v9fs(), "mount");
    remove_if_present(TEST_DIR "/nested");
    remove_dir_if_present(TEST_DIR);

    ASSERT_EQ(NO_ERROR, fs_make_dir(TEST_DIR), "mkdir");
    filehandle *h;
    ASSERT_EQ(NO_ERROR, fs_create_file(TEST_DIR "/nested", &h, 0), "create");
    EXPECT_EQ(NO_ERROR, fs_close_file(h), "close");

    // enumerate first, which is what opens the directory
    dirhandle *dh;
    ASSERT_EQ(NO_ERROR, fs_open_dir(TEST_DIR, &dh), "opendir");
    struct dirent ent;
    EXPECT_EQ(NO_ERROR, fs_read_dir(dh, &ent), "readdir");

    // then walk through it while it is still open
    EXPECT_EQ(NO_ERROR, fs_open_file(TEST_DIR "/nested", &h), "open through open dir");
    EXPECT_EQ(NO_ERROR, fs_close_file(h), "close");

    EXPECT_EQ(NO_ERROR, fs_close_dir(dh), "closedir");

    // and again after it has been closed, since the vnode keeps the opened fid
    EXPECT_EQ(NO_ERROR, fs_open_file(TEST_DIR "/nested", &h), "open after closedir");
    EXPECT_EQ(NO_ERROR, fs_close_file(h), "close");

    EXPECT_EQ(NO_ERROR, fs_remove_file(TEST_DIR "/nested"), "remove nested");
    EXPECT_EQ(NO_ERROR, fs_remove_dir(TEST_DIR), "rmdir");
    EXPECT_EQ(NO_ERROR, fs_unmount(V9FS_MOUNT_POINT), "unmount");

    END_TEST;
}

// Two handles on one name resolve to one vnode, which is what makes the
// layer's busy check work: the ERR_BUSY below is only reachable if both
// handles are counted against one object.
static bool test_v9fs_shared_vnode(void) {
    BEGIN_TEST;
    SKIP_TEST_IF_NO_DEVICE();

    ASSERT_TRUE(mount_v9fs(), "mount");
    remove_if_present(TEST_FILE);

    filehandle *h1, *h2;
    ASSERT_EQ(NO_ERROR, fs_create_file(TEST_FILE, &h1, 0), "create");
    ASSERT_EQ(NO_ERROR, fs_open_file(TEST_FILE, &h2), "open second");

    const char *msg = "shared";
    EXPECT_EQ((ssize_t)6, fs_write_file(h1, msg, 0, 6), "write via first");

    char buf[8] = {0};
    EXPECT_EQ((ssize_t)6, fs_read_file(h2, buf, 0, 6), "read via second");
    EXPECT_EQ(0, memcmp(buf, msg, 6), "content via second");

    // the layer refuses to remove anything with a handle open on it
    EXPECT_EQ(ERR_BUSY, fs_remove_file(TEST_FILE), "remove while open");
    EXPECT_EQ(NO_ERROR, fs_close_file(h1), "close first");
    EXPECT_EQ(ERR_BUSY, fs_remove_file(TEST_FILE), "remove with one still open");
    EXPECT_EQ(NO_ERROR, fs_close_file(h2), "close second");
    EXPECT_EQ(NO_ERROR, fs_remove_file(TEST_FILE), "remove after close");

    EXPECT_EQ(NO_ERROR, fs_unmount(V9FS_MOUNT_POINT), "unmount");

    END_TEST;
}

// A transfer larger than a page, which is chunked at whatever the server said
// it would accept in one message at open time (qemu answers ~124KB, so this is
// one round trip rather than the sixteen a page-sized chunk would take).
static bool test_v9fs_large_io(void) {
    BEGIN_TEST;
    SKIP_TEST_IF_NO_DEVICE();

    ASSERT_TRUE(mount_v9fs(), "mount");
    remove_if_present(TEST_FILE);

    enum { kLen = 64 * 1024 };
    uint8_t *buf = malloc(kLen);
    ASSERT_NONNULL(buf, "buffer");

    filehandle *h;
    ASSERT_EQ(NO_ERROR, fs_create_file(TEST_FILE, &h, 0), "create");

    for (size_t i = 0; i < kLen; i++) {
        buf[i] = pattern_byte(0x5151, i);
    }
    EXPECT_EQ((ssize_t)kLen, fs_write_file(h, buf, 0, kLen), "write");
    EXPECT_EQ(NO_ERROR, fs_close_file(h), "close");

    ASSERT_EQ(NO_ERROR, fs_open_file(TEST_FILE, &h), "reopen");
    memset(buf, 0, kLen);
    EXPECT_EQ((ssize_t)kLen, fs_read_file(h, buf, 0, kLen), "read");

    bool match = true;
    for (size_t i = 0; i < kLen; i++) {
        if (buf[i] != pattern_byte(0x5151, i)) {
            unittest_printf("\n        content mismatch at offset %zu ", i);
            match = false;
            break;
        }
    }
    EXPECT_TRUE(match, "content");

    struct file_stat st;
    EXPECT_EQ(NO_ERROR, fs_stat_file(h, &st), "stat");
    EXPECT_EQ((uint64_t)kLen, st.size, "size");

    EXPECT_EQ(NO_ERROR, fs_close_file(h), "close");
    free(buf);

    EXPECT_EQ(NO_ERROR, fs_remove_file(TEST_FILE), "remove");
    EXPECT_EQ(NO_ERROR, fs_unmount(V9FS_MOUNT_POINT), "unmount");

    END_TEST;
}

// A refused removal has to leave the directory usable. Tremove clunks the fid
// it is handed even when the removal fails, so it is handed a clone rather
// than the vnode's own.
static bool test_v9fs_rmdir_not_empty(void) {
    BEGIN_TEST;
    SKIP_TEST_IF_NO_DEVICE();

    ASSERT_TRUE(mount_v9fs(), "mount");
    remove_if_present(TEST_DIR "/nested");
    remove_dir_if_present(TEST_DIR);

    ASSERT_EQ(NO_ERROR, fs_make_dir(TEST_DIR), "mkdir");
    filehandle *h;
    ASSERT_EQ(NO_ERROR, fs_create_file(TEST_DIR "/nested", &h, 0), "create");
    EXPECT_EQ(NO_ERROR, fs_close_file(h), "close");

    EXPECT_EQ(ERR_NOT_ALLOWED, fs_remove_dir(TEST_DIR), "rmdir non-empty");

    // the directory survived the refusal: it can still be walked through,
    // enumerated, and finally removed
    EXPECT_EQ(NO_ERROR, fs_open_file(TEST_DIR "/nested", &h), "open after refusal");
    EXPECT_EQ(NO_ERROR, fs_close_file(h), "close");

    dirhandle *dh;
    EXPECT_EQ(NO_ERROR, fs_open_dir(TEST_DIR, &dh), "opendir after refusal");
    struct dirent ent;
    EXPECT_EQ(NO_ERROR, fs_read_dir(dh, &ent), "readdir after refusal");
    EXPECT_EQ(NO_ERROR, fs_close_dir(dh), "closedir");

    EXPECT_EQ(NO_ERROR, fs_remove_file(TEST_DIR "/nested"), "remove nested");
    EXPECT_EQ(NO_ERROR, fs_remove_dir(TEST_DIR), "rmdir");

    EXPECT_EQ(NO_ERROR, fs_unmount(V9FS_MOUNT_POINT), "unmount");

    END_TEST;
}

BEGIN_TEST_CASE(v9fs_tests)
RUN_TEST(test_v9fs_mount)
RUN_TEST(test_v9fs_file_io)
RUN_TEST(test_v9fs_dirs)
RUN_TEST(test_v9fs_walk_after_opendir)
RUN_TEST(test_v9fs_shared_vnode)
RUN_TEST(test_v9fs_large_io)
RUN_TEST(test_v9fs_rmdir_not_empty)
END_TEST_CASE(v9fs_tests)
