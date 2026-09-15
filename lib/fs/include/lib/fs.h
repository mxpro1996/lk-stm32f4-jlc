/*
 * Copyright (c) 2009-2015 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

#include <lk/compiler.h>
#include <lk/list.h>
#include <stdbool.h>
#include <sys/types.h>

#define FS_MAX_PATH_LEN 128
#define FS_MAX_FILE_LEN 64

__BEGIN_CDECLS

// Generic FS ioctls
enum fs_ioctl_num {
    FS_IOCTL_NULL = 0,
    FS_IOCTL_GET_FILE_ADDR,
    FS_IOCTL_IS_LINEAR, // If supported, determine if the underlying device is in linear mode.
};

struct file_stat {
    bool is_dir;
    uint64_t size;
    uint64_t capacity;
};

struct fs_stat {
    uint64_t free_space;
    uint64_t total_space;

    uint32_t free_inodes;
    uint32_t total_inodes;
};

struct dirent {
    char name[FS_MAX_FILE_LEN];
};

typedef struct filehandle filehandle;
typedef struct dirhandle dirhandle;

enum fs_mount_options {
    FS_MOUNT_OPTION_NONE = 0,
    FS_MOUNT_OPTION_READ_ONLY = 1 << 0,
};

status_t fs_format_device(const char *fsname, const char *device, const void *args) __NONNULL((1));
status_t fs_mount(const char *path, const char *fs, const char *device, enum fs_mount_options options) __NONNULL((1)) __NONNULL((2));
status_t fs_unmount(const char *path) __NONNULL();
status_t fs_file_ioctl(filehandle *handle, int request, void *argp) __NONNULL((1)) __NONNULL((3));

/* file api */
status_t fs_create_file(const char *path, filehandle **handle, uint64_t len) __NONNULL();
status_t fs_open_file(const char *path, filehandle **handle) __NONNULL();
status_t fs_remove_file(const char *path) __NONNULL();
status_t fs_remove_dir(const char *path) __NONNULL();
ssize_t fs_read_file(filehandle *handle, void *buf, off_t offset, size_t len) __NONNULL();
ssize_t fs_write_file(filehandle *handle, const void *buf, off_t offset, size_t len) __NONNULL();
status_t fs_close_file(filehandle *handle) __NONNULL();
status_t fs_stat_file(filehandle *handle, struct file_stat *) __NONNULL((1));
status_t fs_truncate_file(filehandle *handle, uint64_t len) __NONNULL((1));

/* dir api */
status_t fs_make_dir(const char *path) __NONNULL();
status_t fs_open_dir(const char *path, dirhandle **handle) __NONNULL();
status_t fs_read_dir(dirhandle *handle, struct dirent *ent) __NONNULL();
status_t fs_close_dir(dirhandle *handle) __NONNULL();

status_t fs_stat_fs(const char *mountpoint, struct fs_stat *stat) __NONNULL((1)) __NONNULL((2));

/* walk through a path string, removing duplicate path separators, flattening . and .. references */
void fs_normalize_path(char *path) __NONNULL();

/* file system api */
typedef struct fscookie fscookie;
typedef struct dircookie dircookie;
struct bdev;

/* The vnode interface: the fs layer owns path resolution and hands each
 * filesystem one component at a time, always relative to a directory vnode.
 * See lib/fs/plan.md for the design. */

enum fs_vnode_type {
    FS_VNODE_FILE = 0,
    FS_VNODE_DIR,
    FS_VNODE_SYMLINK,
};

/* One filesystem object (file, directory, symlink). Allocated by the layer on
 * the filesystem's behalf via fs_vnode_create(); the filesystem owns priv and
 * fills id/type, the layer owns everything else including the lifetime: when
 * the last reference is dropped the release() op runs and the vnode is freed. */
struct fs_vnode {
    // layer owned
    struct list_node node;    // in the mount's live vnode list
    struct fs_mount *mount;
    int ref;

    // filesystem provided
    fscookie *cookie;         // the filesystem instance this vnode belongs to (layer filled)
    uint64_t id;              // stable identity for deduplication, 0 = none
    uint8_t type;             // enum fs_vnode_type
    void *priv;               // filesystem private per-object state
};

/* Allocate a vnode to return from mount/lookup/create/mkdir. The filesystem
 * must return a fresh vnode from each call; the layer deduplicates by id. */
status_t fs_vnode_create(uint64_t id, enum fs_vnode_type type, void *priv,
                         struct fs_vnode **out) __NONNULL((4));

/* Destroy a vnode that was never handed back to the layer (error unwinding
 * inside a filesystem op). Invalid on a vnode the layer has seen. */
void fs_vnode_destroy(struct fs_vnode *vn) __NONNULL();

/* The operations a filesystem provides. Most are optional: a NULL member makes
 * the corresponding call return ERR_NOT_SUPPORTED, so the minimum viable
 * filesystem is a read-only flat one with mount/unmount/lookup/read.
 *
 * Required: mount, unmount, lookup, read, and -- if opendir is provided --
 * readdir and closedir, which are called without a NULL check once a directory
 * handle exists. Everything else may be NULL.
 */
struct fs_api {
    // volume ops
    status_t (*format)(struct bdev *, const void *args);
    status_t (*mount)(struct bdev *, enum fs_mount_options options, fscookie **,
                      struct fs_vnode **root);
    status_t (*unmount)(fscookie *);
    status_t (*fs_stat)(fscookie *, struct fs_stat *);

    // namespace ops: always one component, always relative to a directory vnode.
    // unlink/rmdir receive the child so the fs can refuse or invalidate it; the
    // layer has already refused if the child has open handles.
    status_t (*lookup)(struct fs_vnode *dir, const char *name, struct fs_vnode **out);
    status_t (*create)(struct fs_vnode *dir, const char *name, uint64_t len,
                       struct fs_vnode **out);
    status_t (*mkdir)(struct fs_vnode *dir, const char *name, struct fs_vnode **out);
    status_t (*unlink)(struct fs_vnode *dir, const char *name, struct fs_vnode *child);
    status_t (*rmdir)(struct fs_vnode *dir, const char *name, struct fs_vnode *child);
    status_t (*rename)(struct fs_vnode *olddir, const char *oldname, struct fs_vnode *child,
                       struct fs_vnode *newdir, const char *newname); // optional
    ssize_t (*readlink)(struct fs_vnode *link, char *buf, size_t len); // optional
    void (*release)(struct fs_vnode *); // last reference dropped

    // file ops, directly on the vnode (reads and writes carry the offset, so
    // there is no per-open state and no open/close pair)
    ssize_t (*read)(struct fs_vnode *, void *, off_t, size_t);
    ssize_t (*write)(struct fs_vnode *, const void *, off_t, size_t);
    status_t (*truncate)(struct fs_vnode *, uint64_t);
    status_t (*stat)(struct fs_vnode *, struct file_stat *);
    status_t (*ioctl)(struct fs_vnode *, int, void *);

    // directory enumeration needs a cursor, so it keeps a cookie.
    // readdir reports end-of-directory as ERR_NOT_FOUND.
    status_t (*opendir)(struct fs_vnode *, dircookie **);
    status_t (*readdir)(dircookie *, struct dirent *);
    status_t (*closedir)(dircookie *);
};

struct fs_impl {
    const char *name;
    const struct fs_api *api;
};

/* define in your fs implementation to register your api with the fs layer */
#define STATIC_FS_IMPL(_name, _api) const struct fs_impl __fs_impl_##_name __ALIGNED(sizeof(void *)) __SECTION("fs_impl") = \
                                        {.name = #_name, .api = _api}

/* Resize the layer's node cache at runtime (primarily a test and bringup
 * knob; the compile-time default comes from the FS_NODE_CACHE_SIZE build
 * variable). Shrinking evicts immediately; zero disables caching. */
void fs_set_node_cache_size(int size);
int fs_get_node_cache_size(void);

/* list all registered file systems */
void fs_dump_list(void);

/* list all mount poiints */
void fs_dump_mounts(void);

__END_CDECLS
