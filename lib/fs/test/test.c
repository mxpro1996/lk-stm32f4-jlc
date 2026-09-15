/*
 * Copyright (c) 2009-2022 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#include <lib/fs.h>
#include <lk/err.h>

#include <lib/unittest.h>
#include <stdio.h>
#include <string.h>

// returns true if the input path passed through the path normalization
// routine matches the expected output.
static bool test_normalize(const char *in, const char *out) {
    char path[1024];

    strlcpy(path, in, sizeof(path));
    fs_normalize_path(path);
    return !strcmp(path, out);
}

static bool test_path_normalize(void) {
    BEGIN_TEST;

    EXPECT_TRUE(test_normalize("/", ""), "");
    EXPECT_TRUE(test_normalize("/test", "/test"), "");
    EXPECT_TRUE(test_normalize("/test/", "/test"), "");
    EXPECT_TRUE(test_normalize("test/", "test"), "");
    EXPECT_TRUE(test_normalize("test", "test"), "");
    EXPECT_TRUE(test_normalize("/test//", "/test"), "");
    EXPECT_TRUE(test_normalize("/test/foo", "/test/foo"), "");
    EXPECT_TRUE(test_normalize("/test/foo/", "/test/foo"), "");
    EXPECT_TRUE(test_normalize("/test/foo/bar", "/test/foo/bar"), "");
    EXPECT_TRUE(test_normalize("/test/foo/bar//", "/test/foo/bar"), "");
    EXPECT_TRUE(test_normalize("/test//foo/bar//", "/test/foo/bar"), "");
    EXPECT_TRUE(test_normalize("/test//./foo/bar//", "/test/foo/bar"), "");
    EXPECT_TRUE(test_normalize("/test//./.foo/bar//", "/test/.foo/bar"), "");
    EXPECT_TRUE(test_normalize("/test//./..foo/bar//", "/test/..foo/bar"), "");
    EXPECT_TRUE(test_normalize("/test//./../foo/bar//", "/foo/bar"), "");
    EXPECT_TRUE(test_normalize("/test/../foo", "/foo"), "");
    EXPECT_TRUE(test_normalize("/test/bar/../foo", "/test/foo"), "");
    EXPECT_TRUE(test_normalize("../foo", "foo"), "");
    EXPECT_TRUE(test_normalize("../foo/", "foo"), "");
    EXPECT_TRUE(test_normalize("/../foo", "foo"), "");
    EXPECT_TRUE(test_normalize("/../foo/", "foo"), "");
    EXPECT_TRUE(test_normalize("/../../foo", "foo"), "");
    EXPECT_TRUE(test_normalize("/bleh/../../foo", "foo"), "");
    EXPECT_TRUE(test_normalize("/bleh/bar/../../foo", "/foo"), "");
    EXPECT_TRUE(test_normalize("/bleh/bar/../../foo/..", ""), "");
    EXPECT_TRUE(test_normalize("/bleh/bar/../../foo/../meh", "/meh"), "");

    // edge cases: empty, standalone dots
    EXPECT_TRUE(test_normalize("", ""), "");
    EXPECT_TRUE(test_normalize(".", ""), "");
    EXPECT_TRUE(test_normalize("..", ""), "");
    EXPECT_TRUE(test_normalize("/.", ""), "");
    EXPECT_TRUE(test_normalize("/..", ""), "");

    // three dots is a regular filename
    EXPECT_TRUE(test_normalize("...", "..."), "");
    EXPECT_TRUE(test_normalize("/a/.../b", "/a/.../b"), "");

    // dot-prefixed filenames preserved
    EXPECT_TRUE(test_normalize(".hidden", ".hidden"), "");
    EXPECT_TRUE(test_normalize("/a/.hidden", "/a/.hidden"), "");
    EXPECT_TRUE(test_normalize("..hidden", "..hidden"), "");

    // dotdot at end of path
    EXPECT_TRUE(test_normalize("a/..", ""), "");
    EXPECT_TRUE(test_normalize("/a/..", ""), "");
    EXPECT_TRUE(test_normalize("/a/b/..", "/a"), "");

    // multiple dotdots chained
    EXPECT_TRUE(test_normalize("a/b/../../c", "c"), "");
    EXPECT_TRUE(test_normalize("/a/b/../../c", "/c"), "");
    EXPECT_TRUE(test_normalize("a/b/c/../../d", "a/d"), "");

    // dotdot past root clamped
    EXPECT_TRUE(test_normalize("a/../../b", "b"), "");
    EXPECT_TRUE(test_normalize("/a/b/../../../c", "c"), "");

    // mixed dots and dotdots
    EXPECT_TRUE(test_normalize("/a/./b/../c", "/a/c"), "");
    EXPECT_TRUE(test_normalize("a/./b/./c", "a/b/c"), "");
    EXPECT_TRUE(test_normalize("/a/b/./../c", "/a/c"), "");

    // consecutive separators in various positions
    EXPECT_TRUE(test_normalize("///", ""), "");
    EXPECT_TRUE(test_normalize("a///b", "a/b"), "");

    END_TEST;
}

#define TEST_MNT  "/test"
#define TEST_FILE TEST_MNT "/stdio_file_tests.txt"

static inline void test_stdio_fs_teardown(void *ptr) {
    fs_remove_file(TEST_FILE);
    fs_unmount(TEST_MNT);
}

static bool test_stdio_fs(void) {
    __attribute__((cleanup(test_stdio_fs_teardown))) BEGIN_TEST;

    // Setup
    const char *content = "Hello World\n";
    const size_t content_len = strlen(content);
    fs_mount(TEST_MNT, "memfs", NULL, FS_MOUNT_OPTION_NONE);

    // Tests
    FILE *stream = fopen(TEST_FILE, "w");
    ASSERT_NE(NULL, stream, "failed to open/create file " TEST_MNT);

    char buf[1024];
    // test stdout fprintf, fputs
    EXPECT_EQ(content_len, fprintf(stdout, "%s", content), "");
    EXPECT_EQ(content_len, fputs(content, stdout), "");

    // test fwrite
    EXPECT_EQ(content_len, fwrite(content, 1, content_len, stream), "");

    // test fread
    ASSERT_EQ(NO_ERROR, fseek(stream, 0, SEEK_SET), "fseek failed");
    EXPECT_EQ(content_len, fread(buf, 1, content_len, stream), "");
    EXPECT_BYTES_EQ((const uint8_t *)content, (const uint8_t *)buf, content_len,
                    "fread content mismatched");

    // testing fputs
    ASSERT_EQ(NO_ERROR, fseek(stream, 0, SEEK_SET), "fseek failed");
    EXPECT_EQ(content_len, fputs(content, stream), "");

    // testing fgets
    ASSERT_EQ(NO_ERROR, fseek(stream, 0, SEEK_SET), "fseek failed");
    ASSERT_NE(NULL, fgets(buf, content_len + 1, stream), "");
    EXPECT_BYTES_EQ((const uint8_t *)content, (const uint8_t *)buf, content_len,
                    "fgets content mismatched");

    END_TEST;
}

#define BUSY_MNT  "/fs_busy_test"
#define BUSY_FILE BUSY_MNT "/file"

static void test_remove_while_open_teardown(void *ptr) {
    fs_remove_file(BUSY_FILE);
    fs_unmount(BUSY_MNT);
}

static bool test_remove_while_open(void) {
    __attribute__((cleanup(test_remove_while_open_teardown))) BEGIN_TEST;

    ASSERT_EQ(NO_ERROR, fs_mount(BUSY_MNT, "memfs", NULL, FS_MOUNT_OPTION_NONE), "mount");

    filehandle *h;
    ASSERT_EQ(NO_ERROR, fs_create_file(BUSY_FILE, &h, 16), "create");

    // removing a file with an open handle must be refused, not freed underneath it
    EXPECT_EQ(ERR_BUSY, fs_remove_file(BUSY_FILE), "remove while open");

    // the handle must still be usable afterwards
    char buf[16];
    EXPECT_EQ(16, fs_read_file(h, buf, 0, sizeof(buf)), "read after failed remove");

    // a second open of the same file holds it busy on its own
    filehandle *h2;
    ASSERT_EQ(NO_ERROR, fs_open_file(BUSY_FILE, &h2), "second open");
    EXPECT_EQ(NO_ERROR, fs_close_file(h), "close first handle");
    EXPECT_EQ(ERR_BUSY, fs_remove_file(BUSY_FILE), "remove with second handle open");
    EXPECT_EQ(NO_ERROR, fs_close_file(h2), "close second handle");

    // with every handle closed the remove goes through
    EXPECT_EQ(NO_ERROR, fs_remove_file(BUSY_FILE), "remove after close");
    EXPECT_EQ(ERR_NOT_FOUND, fs_open_file(BUSY_FILE, &h), "open after remove");

    END_TEST;
}

#define TREE_MNT "/fs_tree_test"

static void test_hierarchy_teardown(void *ptr) {
    fs_remove_file(TREE_MNT "/a/b/f");
    fs_remove_dir(TREE_MNT "/a/b");
    fs_remove_dir(TREE_MNT "/a");
    fs_unmount(TREE_MNT);
}

// Directory tree behavior through the fs layer, using memfs as the
// reference implementation of the vnode interface.
static bool test_hierarchy(void) {
    __attribute__((cleanup(test_hierarchy_teardown))) BEGIN_TEST;

    ASSERT_EQ(NO_ERROR, fs_mount(TREE_MNT, "memfs", NULL, FS_MOUNT_OPTION_NONE), "mount");

    // nested directories, and a file at the bottom
    ASSERT_EQ(NO_ERROR, fs_make_dir(TREE_MNT "/a"), "mkdir a");
    ASSERT_EQ(NO_ERROR, fs_make_dir(TREE_MNT "/a/b"), "mkdir a/b");

    filehandle *h;
    ASSERT_EQ(NO_ERROR, fs_create_file(TREE_MNT "/a/b/f", &h, 0), "create nested file");
    char msg[] = "down here";
    EXPECT_EQ((ssize_t)sizeof(msg), fs_write_file(h, msg, 0, sizeof(msg)), "write");
    EXPECT_EQ(NO_ERROR, fs_close_file(h), "close");

    // read it back through the full path
    ASSERT_EQ(NO_ERROR, fs_open_file(TREE_MNT "/a/b/f", &h), "reopen nested file");
    char buf[sizeof(msg)];
    EXPECT_EQ((ssize_t)sizeof(msg), fs_read_file(h, buf, 0, sizeof(buf)), "read");
    EXPECT_EQ(0, memcmp(msg, buf, sizeof(msg)), "content");
    EXPECT_EQ(NO_ERROR, fs_close_file(h), "close reopened");

    // enumerate an inner directory
    dirhandle *dh;
    struct dirent ent;
    ASSERT_EQ(NO_ERROR, fs_open_dir(TREE_MNT "/a", &dh), "open dir a");
    ASSERT_EQ(NO_ERROR, fs_read_dir(dh, &ent), "read dir a");
    EXPECT_EQ(0, strcmp(ent.name, "b"), "sole entry is b");
    EXPECT_EQ(ERR_NOT_FOUND, fs_read_dir(dh, &ent), "eof");
    EXPECT_EQ(NO_ERROR, fs_close_dir(dh), "close dir a");

    // a directory stats as one
    ASSERT_EQ(NO_ERROR, fs_open_file(TREE_MNT "/a", &h), "open dir as handle");
    struct file_stat st;
    EXPECT_EQ(NO_ERROR, fs_stat_file(h, &st), "stat dir");
    EXPECT_TRUE(st.is_dir, "is_dir");
    EXPECT_EQ(NO_ERROR, fs_close_file(h), "close dir handle");

    // name collisions and type confusion are refused with the right errors
    EXPECT_EQ(ERR_ALREADY_EXISTS, fs_make_dir(TREE_MNT "/a"), "mkdir existing");
    EXPECT_EQ(ERR_ALREADY_EXISTS, fs_create_file(TREE_MNT "/a", &h, 0), "create over dir");
    EXPECT_EQ(ERR_NOT_ALLOWED, fs_remove_dir(TREE_MNT "/a"), "rmdir non-empty");
    EXPECT_EQ(ERR_NOT_FILE, fs_remove_file(TREE_MNT "/a/b"), "remove on a dir");
    EXPECT_EQ(ERR_NOT_DIR, fs_remove_dir(TREE_MNT "/a/b/f"), "rmdir on a file");
    EXPECT_EQ(ERR_NOT_DIR, fs_open_file(TREE_MNT "/a/b/f/deeper", &h), "walk through a file");

    // tear it down bottom-up; every level must really be gone
    EXPECT_EQ(NO_ERROR, fs_remove_file(TREE_MNT "/a/b/f"), "remove file");
    EXPECT_EQ(ERR_NOT_FOUND, fs_open_file(TREE_MNT "/a/b/f", &h), "file gone");
    EXPECT_EQ(NO_ERROR, fs_remove_dir(TREE_MNT "/a/b"), "rmdir b");
    EXPECT_EQ(ERR_NOT_FOUND, fs_open_dir(TREE_MNT "/a/b", &dh), "b gone");
    EXPECT_EQ(NO_ERROR, fs_remove_dir(TREE_MNT "/a"), "rmdir a");
    EXPECT_EQ(ERR_NOT_FOUND, fs_open_dir(TREE_MNT "/a", &dh), "a gone");

    END_TEST;
}

#define SHARE_MNT "/fs_share_test"

static void test_shared_object_teardown(void *ptr) {
    fs_remove_file(SHARE_MNT "/f");
    fs_unmount(SHARE_MNT);
}

// Two handles on one file must observe each other's writes: the layer
// deduplicates vnodes by id, so both handles reach one object.
static bool test_shared_object(void) {
    __attribute__((cleanup(test_shared_object_teardown))) BEGIN_TEST;

    ASSERT_EQ(NO_ERROR, fs_mount(SHARE_MNT, "memfs", NULL, FS_MOUNT_OPTION_NONE), "mount");

    filehandle *h1, *h2;
    ASSERT_EQ(NO_ERROR, fs_create_file(SHARE_MNT "/f", &h1, 0), "create");
    ASSERT_EQ(NO_ERROR, fs_open_file(SHARE_MNT "/f", &h2), "second open");

    char msg[] = "shared";
    EXPECT_EQ((ssize_t)sizeof(msg), fs_write_file(h1, msg, 0, sizeof(msg)), "write via h1");

    struct file_stat st;
    EXPECT_EQ(NO_ERROR, fs_stat_file(h2, &st), "stat via h2");
    EXPECT_EQ(sizeof(msg), st.size, "size seen via h2");

    char buf[sizeof(msg)];
    EXPECT_EQ((ssize_t)sizeof(msg), fs_read_file(h2, buf, 0, sizeof(buf)), "read via h2");
    EXPECT_EQ(0, memcmp(msg, buf, sizeof(msg)), "content seen via h2");

    EXPECT_EQ(NO_ERROR, fs_close_file(h1), "close h1");
    EXPECT_EQ(NO_ERROR, fs_close_file(h2), "close h2");

    EXPECT_EQ(NO_ERROR, fs_remove_file(SHARE_MNT "/f"), "remove");

    END_TEST;
}

#define DIR_MNT "/fs_readdir_test"

static void test_readdir_teardown(void *ptr) {
    fs_remove_file(DIR_MNT "/a");
    fs_remove_file(DIR_MNT "/b");
    fs_remove_file(DIR_MNT "/c");
    fs_unmount(DIR_MNT);
}

static bool test_readdir(void) {
    __attribute__((cleanup(test_readdir_teardown))) BEGIN_TEST;

    ASSERT_EQ(NO_ERROR, fs_mount(DIR_MNT, "memfs", NULL, FS_MOUNT_OPTION_NONE), "mount");

    const char *names[] = { DIR_MNT "/a", DIR_MNT "/b", DIR_MNT "/c" };
    for (size_t i = 0; i < countof(names); i++) {
        filehandle *h;
        ASSERT_EQ(NO_ERROR, fs_create_file(names[i], &h, 0), "create");
        ASSERT_EQ(NO_ERROR, fs_close_file(h), "close");
    }

    dirhandle *dh;
    ASSERT_EQ(NO_ERROR, fs_open_dir(DIR_MNT, &dh), "open dir");
    bool seen[countof(names)] = {};
    size_t entries = 0;
    struct dirent ent;
    while (fs_read_dir(dh, &ent) == NO_ERROR) {
        entries++;
        if (!strcmp(ent.name, "a")) seen[0] = true;
        if (!strcmp(ent.name, "b")) seen[1] = true;
        if (!strcmp(ent.name, "c")) seen[2] = true;
    }
    EXPECT_EQ(3u, entries, "entry count");
    EXPECT_TRUE(seen[0] && seen[1] && seen[2], "every created file enumerated");

    // end of directory is ERR_NOT_FOUND, and stays that way on a re-read
    EXPECT_EQ(ERR_NOT_FOUND, fs_read_dir(dh, &ent), "eof");
    EXPECT_EQ(ERR_NOT_FOUND, fs_read_dir(dh, &ent), "eof is stable");
    EXPECT_EQ(NO_ERROR, fs_close_dir(dh), "close dir");

    END_TEST;
}

#define UMNT_MNT  "/fs_unmount_test"
#define UMNT_FILE UMNT_MNT "/file"

static void test_unmount_with_open_handle_teardown(void *ptr) {
    fs_remove_file(UMNT_FILE);
    fs_unmount(UMNT_MNT);
}

// Unmounting with a handle still open must never free the filesystem under
// the handle. Today the layer defers the teardown until the last close and
// returns NO_ERROR; a future version may refuse with ERR_BUSY instead. Either
// way the handle stays usable and the mount is gone once both the close and a
// (possibly second) unmount have happened.
static bool test_unmount_with_open_handle(void) {
    __attribute__((cleanup(test_unmount_with_open_handle_teardown))) BEGIN_TEST;

    ASSERT_EQ(NO_ERROR, fs_mount(UMNT_MNT, "memfs", NULL, FS_MOUNT_OPTION_NONE), "mount");

    filehandle *h;
    ASSERT_EQ(NO_ERROR, fs_create_file(UMNT_FILE, &h, 16), "create");

    status_t err = fs_unmount(UMNT_MNT);
    EXPECT_TRUE(err == NO_ERROR || err == ERR_BUSY, "unmount with open handle");

    // the handle must still work
    char buf[16];
    EXPECT_EQ(16, fs_read_file(h, buf, 0, sizeof(buf)), "read after unmount attempt");
    EXPECT_EQ(NO_ERROR, fs_close_file(h), "close");

    // after the last close the mount can be (or already has been) torn down
    err = fs_unmount(UMNT_MNT);
    EXPECT_TRUE(err == NO_ERROR || err == ERR_NOT_FOUND, "final unmount");

    // and the path is really gone
    EXPECT_EQ(ERR_NOT_FOUND, fs_open_file(UMNT_FILE, &h), "open after unmount");

    END_TEST;
}

#define DBLU_MNT  "/fs_double_unmount_test"
#define DBLU_FILE DBLU_MNT "/file"

static void test_double_unmount_teardown(void *ptr) {
    fs_remove_file(DBLU_FILE);
    fs_unmount(DBLU_MNT);
}

// A second unmount of a mount whose teardown is still deferred behind an open
// handle must not spend its references a second time: doing so drove the count
// to zero underneath the handle, tearing the filesystem down with a live vnode
// still on its list.
static bool test_double_unmount(void) {
    __attribute__((cleanup(test_double_unmount_teardown))) BEGIN_TEST;

    ASSERT_EQ(NO_ERROR, fs_mount(DBLU_MNT, "memfs", NULL, FS_MOUNT_OPTION_NONE), "mount");

    filehandle *h;
    ASSERT_EQ(NO_ERROR, fs_create_file(DBLU_FILE, &h, 16), "create");

    status_t err = fs_unmount(DBLU_MNT);
    ASSERT_TRUE(err == NO_ERROR || err == ERR_BUSY, "first unmount");

    if (err == NO_ERROR) {
        // the teardown is deferred: the mount is already spoken for and a
        // second unmount has nothing of its own to return
        EXPECT_EQ(ERR_NOT_FOUND, fs_unmount(DBLU_MNT), "second unmount");
        EXPECT_EQ(ERR_NOT_FOUND, fs_unmount(DBLU_MNT), "third unmount");
    }

    // whatever the first unmount answered, the handle is still live
    char buf[16];
    EXPECT_EQ(16, fs_read_file(h, buf, 0, sizeof(buf)), "read after unmount");
    EXPECT_EQ(NO_ERROR, fs_close_file(h), "close");

    EXPECT_EQ(ERR_NOT_FOUND, fs_open_file(DBLU_FILE, &h), "open after unmount");

    END_TEST;
}

#define ABOVE_BASE "/fs_mount_above_test"

static void test_mount_above_teardown(void *ptr) {
    fs_unmount(ABOVE_BASE "/a/b");
    fs_unmount(ABOVE_BASE "/a");
}

// Mounting over scaffolding that already leads to a mount would shadow it: the
// covering filesystem answers every lookup while the mount underneath stays in
// the mount list and keeps its files alive, so listing and resolution disagree.
static bool test_mount_above(void) {
    __attribute__((cleanup(test_mount_above_teardown))) BEGIN_TEST;

    ASSERT_EQ(NO_ERROR, fs_mount(ABOVE_BASE "/a/b", "memfs", NULL, FS_MOUNT_OPTION_NONE),
              "deep mount");

    // both the scaffolding directly above the mount and the level above that
    EXPECT_EQ(ERR_ALREADY_MOUNTED,
              fs_mount(ABOVE_BASE "/a", "memfs", NULL, FS_MOUNT_OPTION_NONE),
              "mount directly above");
    EXPECT_EQ(ERR_ALREADY_MOUNTED,
              fs_mount(ABOVE_BASE, "memfs", NULL, FS_MOUNT_OPTION_NONE),
              "mount further above");

    // the refusal must not have disturbed what is already there
    filehandle *h;
    ASSERT_EQ(NO_ERROR, fs_create_file(ABOVE_BASE "/a/b/file", &h, 16), "create below");
    EXPECT_EQ(NO_ERROR, fs_close_file(h), "close");
    EXPECT_EQ(NO_ERROR, fs_remove_file(ABOVE_BASE "/a/b/file"), "remove below");

    // once the subtree is empty the same path is free again
    ASSERT_EQ(NO_ERROR, fs_unmount(ABOVE_BASE "/a/b"), "unmount");
    EXPECT_EQ(NO_ERROR, fs_mount(ABOVE_BASE "/a", "memfs", NULL, FS_MOUNT_OPTION_NONE),
              "mount above once free");
    EXPECT_EQ(NO_ERROR, fs_unmount(ABOVE_BASE "/a"), "unmount again");

    END_TEST;
}

static void test_rootfs_teardown(void *ptr) {
    fs_unmount("/tmp");
    fs_unmount("/data");
}

static bool test_rootfs(void) {
    __attribute__((cleanup(test_rootfs_teardown))) BEGIN_TEST;

    // root listing must work even before we add any filesystems
    dirhandle *dh;
    ASSERT_EQ(NO_ERROR, fs_open_dir("/", &dh), "open root with no mounts");
    struct dirent ent;
    // drain any pre-existing entries (other tests may have left mounts)
    while (fs_read_dir(dh, &ent) == NO_ERROR) {
    }
    fs_close_dir(dh);

    // mount two filesystems; both must appear as directories in root listing
    ASSERT_EQ(NO_ERROR, fs_mount("/tmp", "memfs", NULL, FS_MOUNT_OPTION_NONE), "mount /tmp");
    ASSERT_EQ(NO_ERROR, fs_mount("/data", "memfs", NULL, FS_MOUNT_OPTION_NONE), "mount /data");

    ASSERT_EQ(NO_ERROR, fs_open_dir("/", &dh), "open root dir with mounts");
    bool found_tmp = false, found_data = false;
    while (fs_read_dir(dh, &ent) == NO_ERROR) {
        if (strcmp(ent.name, "tmp") == 0) {
            found_tmp = true;
        }
        if (strcmp(ent.name, "data") == 0) {
            found_data = true;
        }
    }
    fs_close_dir(dh);
    EXPECT_TRUE(found_tmp, "tmp in root listing");
    EXPECT_TRUE(found_data, "data in root listing");

    END_TEST;
}

// Verifies that mount/unmount operations occurring during an open dir
// iteration do not crash or corrupt the iterator. Scaffold children are kept
// in creation order and the cursor is a live pointer into that list:
//   - a node removed while the cursor points at it is advanced past
//   - a mount added behind the cursor (at the tail) is seen later in the pass
static void test_rootfs_live_iter_teardown(void *ptr) {
    // best-effort; ignore errors for mounts that may already be gone
    fs_unmount("/live_a");
    fs_unmount("/live_b");
    fs_unmount("/live_c");
    fs_unmount("/live_d");
}

static bool test_rootfs_live_iter(void) {
    __attribute__((cleanup(test_rootfs_live_iter_teardown))) BEGIN_TEST;

    struct dirent ent;

    // Set up three mounts; scaffold children list in creation order a, b, c.
    ASSERT_EQ(NO_ERROR, fs_mount("/live_a", "memfs", NULL, FS_MOUNT_OPTION_NONE), "mount live_a");
    ASSERT_EQ(NO_ERROR, fs_mount("/live_b", "memfs", NULL, FS_MOUNT_OPTION_NONE), "mount live_b");
    ASSERT_EQ(NO_ERROR, fs_mount("/live_c", "memfs", NULL, FS_MOUNT_OPTION_NONE), "mount live_c");

    // Open the iterator; the cursor starts at the first child (live_a).
    dirhandle *dh;
    ASSERT_EQ(NO_ERROR, fs_open_dir("/", &dh), "open root dir");

    // Read the first live_* entry (other tests may have left mounts behind,
    // which sort before ours in creation order); it must be live_a, after
    // which the cursor points at live_b.
    bool seen_a = false;
    while (fs_read_dir(dh, &ent) == NO_ERROR) {
        if (strncmp(ent.name, "live_", 5) == 0) {
            seen_a = (strcmp(ent.name, "live_a") == 0);
            break;
        }
    }

    // Remove the mount whose node the cursor points at (live_b); the cursor
    // must be advanced off it before the node is pruned.
    EXPECT_EQ(NO_ERROR, fs_unmount("/live_b"), "unmount live_b mid-iter");

    // Add a new mount; its node lands at the tail, behind the cursor, so this
    // same pass will reach it.
    EXPECT_EQ(NO_ERROR, fs_mount("/live_d", "memfs", NULL, FS_MOUNT_OPTION_NONE), "mount live_d mid-iter");

    // Drain: live_b must not appear, live_c and live_d must.
    bool seen_b = false, seen_c = false, seen_d = false;
    while (fs_read_dir(dh, &ent) == NO_ERROR) {
        if (strcmp(ent.name, "live_b") == 0) {
            seen_b = true;
        }
        if (strcmp(ent.name, "live_c") == 0) {
            seen_c = true;
        }
        if (strcmp(ent.name, "live_d") == 0) {
            seen_d = true;
        }
    }
    fs_close_dir(dh);

    EXPECT_TRUE(seen_a, "live_a seen first");
    EXPECT_FALSE(seen_b, "live_b not seen after being unmounted mid-iter");
    EXPECT_TRUE(seen_c, "live_c seen after live_b removal");
    EXPECT_TRUE(seen_d, "live_d added mid-iter is reached");

    END_TEST;
}

#define CACHE_MNT "/fs_cache_test"

// One full life cycle through the mount: build a tree, rewalk it enough
// times to cycle any bounded cache, enumerate, unlink through names that may
// be cached, and unmount with names still cached (the teardown sweep).
static bool run_cache_exercise(void) {
    BEGIN_TEST;

    ASSERT_EQ(NO_ERROR, fs_mount(CACHE_MNT, "memfs", NULL, FS_MOUNT_OPTION_NONE), "mount");

    ASSERT_EQ(NO_ERROR, fs_make_dir(CACHE_MNT "/d"), "mkdir");

    char path[64];
    filehandle *h;
    for (int i = 0; i < 12; i++) {
        snprintf(path, sizeof(path), CACHE_MNT "/d/f%d", i);
        ASSERT_EQ(NO_ERROR, fs_create_file(path, &h, 4), "create");
        ASSERT_EQ(NO_ERROR, fs_close_file(h), "close");
    }

    // rewalk every path a few times; with a small cache this cycles entries
    // in and out, with none it prunes and re-looks-up every time
    char buf[4];
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < 12; i++) {
            snprintf(path, sizeof(path), CACHE_MNT "/d/f%d", i);
            ASSERT_EQ(NO_ERROR, fs_open_file(path, &h), "reopen");
            EXPECT_EQ(4, fs_read_file(h, buf, 0, sizeof(buf)), "read");
            ASSERT_EQ(NO_ERROR, fs_close_file(h), "close reopened");
        }
    }

    // a directory with this many entries enumerates completely
    dirhandle *dh;
    struct dirent ent;
    ASSERT_EQ(NO_ERROR, fs_open_dir(CACHE_MNT "/d", &dh), "opendir");
    int entries = 0;
    while (fs_read_dir(dh, &ent) == NO_ERROR) {
        entries++;
    }
    EXPECT_EQ(12, entries, "entry count");
    ASSERT_EQ(NO_ERROR, fs_close_dir(dh), "closedir");

    // unlink half of it through possibly-cached names, then verify the
    // listing shrank to match
    for (int i = 0; i < 6; i++) {
        snprintf(path, sizeof(path), CACHE_MNT "/d/f%d", i);
        ASSERT_EQ(NO_ERROR, fs_remove_file(path), "remove");
        EXPECT_EQ(ERR_NOT_FOUND, fs_open_file(path, &h), "removed name gone");
    }
    ASSERT_EQ(NO_ERROR, fs_open_dir(CACHE_MNT "/d", &dh), "opendir again");
    entries = 0;
    while (fs_read_dir(dh, &ent) == NO_ERROR) {
        entries++;
    }
    EXPECT_EQ(6, entries, "entry count after unlink");
    ASSERT_EQ(NO_ERROR, fs_close_dir(dh), "closedir again");

    // unmount with the remaining names likely still cached
    EXPECT_EQ(NO_ERROR, fs_unmount(CACHE_MNT), "unmount");

    END_TEST;
}

static void test_node_cache_teardown(void *ptr) {
    fs_unmount(CACHE_MNT);
    fs_set_node_cache_size(*(int *)ptr);
}

// The node cache must be behaviorally invisible: the same exercise passes
// with it disabled, small enough to thrash, and at the build default.
static bool test_node_cache(void) {
    int saved __attribute__((cleanup(test_node_cache_teardown))) = fs_get_node_cache_size();
    BEGIN_TEST;

    fs_set_node_cache_size(0);
    EXPECT_TRUE(run_cache_exercise(), "cache disabled");

    fs_set_node_cache_size(4);
    EXPECT_TRUE(run_cache_exercise(), "small cache");

    fs_set_node_cache_size(saved);
    EXPECT_TRUE(run_cache_exercise(), "default cache");

    END_TEST;
}

#define NS_BASE "/ns_test"

static void test_mount_namespace_teardown(void *ptr) {
    fs_remove_file(NS_BASE "/deep/mnt/file");
    fs_unmount(NS_BASE "/deep/mnt");
    fs_unmount(NS_BASE "/deep/mnt2");
}

static bool test_mount_namespace(void) {
    __attribute__((cleanup(test_mount_namespace_teardown))) BEGIN_TEST;

    // mounting deep creates the intermediate scaffold directories
    ASSERT_EQ(NO_ERROR, fs_mount(NS_BASE "/deep/mnt", "memfs", NULL, FS_MOUNT_OPTION_NONE),
              "deep mount");

    // every intermediate level lists its single child
    dirhandle *dh;
    struct dirent ent;
    ASSERT_EQ(NO_ERROR, fs_open_dir(NS_BASE, &dh), "open scaffold");
    ASSERT_EQ(NO_ERROR, fs_read_dir(dh, &ent), "read scaffold");
    EXPECT_EQ(0, strcmp(ent.name, "deep"), "child is deep");
    EXPECT_EQ(ERR_NOT_FOUND, fs_read_dir(dh, &ent), "one child only");
    EXPECT_EQ(NO_ERROR, fs_close_dir(dh), "close scaffold");

    ASSERT_EQ(NO_ERROR, fs_open_dir(NS_BASE "/deep", &dh), "open inner scaffold");
    ASSERT_EQ(NO_ERROR, fs_read_dir(dh, &ent), "read inner scaffold");
    EXPECT_EQ(0, strcmp(ent.name, "mnt"), "child is mnt");
    EXPECT_EQ(NO_ERROR, fs_close_dir(dh), "close inner scaffold");

    // a file created through the deep path lands in the filesystem
    filehandle *h;
    ASSERT_EQ(NO_ERROR, fs_create_file(NS_BASE "/deep/mnt/file", &h, 16), "create");
    EXPECT_EQ(NO_ERROR, fs_close_file(h), "close file");

    // mounting on or below an existing mount is refused
    EXPECT_EQ(ERR_ALREADY_MOUNTED,
              fs_mount(NS_BASE "/deep/mnt", "memfs", NULL, FS_MOUNT_OPTION_NONE),
              "mount on mount");
    EXPECT_EQ(ERR_ALREADY_MOUNTED,
              fs_mount(NS_BASE "/deep/mnt/sub", "memfs", NULL, FS_MOUNT_OPTION_NONE),
              "mount below mount");

    // a second mount under the same scaffold shares it
    ASSERT_EQ(NO_ERROR, fs_mount(NS_BASE "/deep/mnt2", "memfs", NULL, FS_MOUNT_OPTION_NONE),
              "sibling mount");

    // unmounting by a path inside a mount is refused
    EXPECT_EQ(ERR_NOT_FOUND, fs_unmount(NS_BASE "/deep/mnt/file"), "unmount inner path");

    // unmounting prunes exactly the scaffolding nothing else needs
    ASSERT_EQ(NO_ERROR, fs_remove_file(NS_BASE "/deep/mnt/file"), "remove file");
    EXPECT_EQ(NO_ERROR, fs_unmount(NS_BASE "/deep/mnt"), "unmount first");
    dirhandle *dh2;
    EXPECT_EQ(ERR_NOT_FOUND, fs_open_dir(NS_BASE "/deep/mnt", &dh2), "mnt gone");
    ASSERT_EQ(NO_ERROR, fs_open_dir(NS_BASE "/deep", &dh), "scaffold still present");
    EXPECT_EQ(NO_ERROR, fs_close_dir(dh), "close remaining scaffold");
    EXPECT_EQ(NO_ERROR, fs_unmount(NS_BASE "/deep/mnt2"), "unmount second");
    EXPECT_EQ(ERR_NOT_FOUND, fs_open_dir(NS_BASE, &dh), "scaffold fully pruned");

    END_TEST;
}

BEGIN_TEST_CASE(fs_tests);
RUN_TEST(test_path_normalize);
RUN_TEST(test_stdio_fs);
RUN_TEST(test_remove_while_open);
RUN_TEST(test_hierarchy);
RUN_TEST(test_shared_object);
RUN_TEST(test_readdir);
RUN_TEST(test_unmount_with_open_handle);
RUN_TEST(test_double_unmount);
RUN_TEST(test_mount_above);
RUN_TEST(test_rootfs);
RUN_TEST(test_rootfs_live_iter);
RUN_TEST(test_mount_namespace);
RUN_TEST(test_node_cache);
END_TEST_CASE(fs_tests);
