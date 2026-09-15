/*
 * Copyright (c) 2015 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

#include <assert.h>
#include <kernel/mutex.h>
#include <lib/fs.h>
#include <lk/debug.h>
#include <lk/err.h>
#include <lk/list.h>
#include <lk/trace.h>
#include <stdlib.h>
#include <string.h>

#define LOCAL_TRACE 0

// A RAM backed filesystem on the vnode interface, and its reference
// implementation: the fs layer owns all path resolution and hands this code
// one component at a time, so there is no path parsing here at all.
//
// Objects (files and directories) are owned by the tree: a directory holds a
// list of named entries pointing at objects. Vnodes are views the layer keeps
// while something references an object; since the layer refuses to unlink
// anything with open handles, an object is freed exactly when its entry is
// removed, and release() has nothing to do (the layer frees the vnode
// itself).
//
// The layer's lock covers all namespace operations. The per-mount lock here
// exists because file I/O and directory enumeration run outside that lock:
// it guards file contents and the entry lists those paths read.

typedef struct memfs {
    mutex_t lock;
    struct memfs_object *root;

    // all open directory cursors, so unlink can fix up any that point at the
    // entry being removed
    struct list_node dcookies;
} memfs_t;

typedef struct memfs_object {
    memfs_t *fs;
    struct memfs_object *parent; // NULL for the root
    bool is_dir;

    // directory: named entries
    struct list_node entries;

    // file: contents
    uint8_t *ptr;
    size_t len;
} memfs_object_t;

typedef struct memfs_dirent {
    struct list_node node; // in the parent object's entries
    memfs_object_t *obj;
    char name[];
} memfs_dirent_t;

struct dircookie {
    struct list_node node; // in the mount's dcookies
    memfs_object_t *dir;
    memfs_dirent_t *next; // next entry to report, NULL = exhausted
};

// every object doubles as its own stable identity for vnode deduplication;
// an address is only reused after the old object is freed, which the layer's
// locking orders after the last vnode carrying it is gone
static uint64_t obj_id(memfs_object_t *obj) {
    return (uint64_t)(uintptr_t)obj;
}

static status_t obj_to_vnode(memfs_object_t *obj, struct fs_vnode **out) {
    return fs_vnode_create(obj_id(obj), obj->is_dir ? FS_VNODE_DIR : FS_VNODE_FILE,
                           obj, out);
}

static memfs_object_t *obj_alloc(memfs_t *fs, memfs_object_t *parent, bool is_dir) {
    memfs_object_t *obj = malloc(sizeof(*obj));
    if (!obj) {
        return NULL;
    }

    obj->fs = fs;
    obj->parent = parent;
    obj->is_dir = is_dir;
    list_initialize(&obj->entries);
    obj->ptr = NULL;
    obj->len = 0;

    return obj;
}

static memfs_dirent_t *find_entry(memfs_object_t *dir, const char *name) {
    memfs_dirent_t *ent;
    list_for_every_entry(&dir->entries, ent, memfs_dirent_t, node) {
        if (!strcmp(name, ent->name)) {
            return ent;
        }
    }
    return NULL;
}

// link a new object into a directory under the given name
static status_t link_object(memfs_object_t *dir, const char *name, memfs_object_t *obj) {
    size_t len = strlen(name);
    memfs_dirent_t *ent = malloc(sizeof(*ent) + len + 1);
    if (!ent) {
        return ERR_NO_MEMORY;
    }

    memcpy(ent->name, name, len + 1);
    ent->obj = obj;

    mutex_acquire(&dir->fs->lock);
    list_add_tail(&dir->entries, &ent->node);
    mutex_release(&dir->fs->lock);

    return NO_ERROR;
}

static status_t memfs_mount(struct bdev *dev, enum fs_mount_options options,
                            fscookie **cookie, struct fs_vnode **root) {
    if (options != 0) {
        return ERR_INVALID_ARGS;
    }
    LTRACEF("dev %p, cookie %p\n", dev, cookie);

    memfs_t *fs = malloc(sizeof(*fs));
    if (!fs) {
        return ERR_NO_MEMORY;
    }

    mutex_init(&fs->lock);
    list_initialize(&fs->dcookies);

    fs->root = obj_alloc(fs, NULL, true);
    if (!fs->root) {
        free(fs);
        return ERR_NO_MEMORY;
    }

    status_t err = obj_to_vnode(fs->root, root);
    if (err < 0) {
        free(fs->root);
        free(fs);
        return err;
    }

    *cookie = (fscookie *)fs;
    return NO_ERROR;
}

static status_t memfs_unmount(fscookie *cookie) {
    memfs_t *fs = (memfs_t *)cookie;

    LTRACEF("cookie %p\n", cookie);

    // the layer only unmounts once every handle is closed
    DEBUG_ASSERT(list_is_empty(&fs->dcookies));

    // free the whole tree iteratively; a recursive free would put one frame
    // on the stack per directory level
    memfs_object_t *cur = fs->root;
    while (cur) {
        memfs_dirent_t *ent = list_remove_head_type(&cur->entries, memfs_dirent_t, node);
        if (ent) {
            memfs_object_t *obj = ent->obj;
            free(ent);
            if (obj->is_dir) {
                cur = obj; // descend; the entry naming it is already gone
                continue;
            }
            free(obj->ptr);
            free(obj);
            continue;
        }

        // this directory is empty now; back out and free it
        memfs_object_t *parent = cur->parent;
        free(cur);
        cur = parent;
    }

    free(fs);
    return NO_ERROR;
}

static status_t memfs_lookup(struct fs_vnode *dir, const char *name, struct fs_vnode **out) {
    memfs_object_t *dirobj = (memfs_object_t *)dir->priv;

    LTRACEF("dir %p name '%s'\n", dirobj, name);

    DEBUG_ASSERT(dirobj->is_dir);

    mutex_acquire(&dirobj->fs->lock);
    memfs_dirent_t *ent = find_entry(dirobj, name);
    mutex_release(&dirobj->fs->lock);

    if (!ent) {
        return ERR_NOT_FOUND;
    }

    return obj_to_vnode(ent->obj, out);
}

static status_t memfs_create(struct fs_vnode *dir, const char *name, uint64_t len,
                             struct fs_vnode **out) {
    memfs_object_t *dirobj = (memfs_object_t *)dir->priv;

    LTRACEF("dir %p name '%s' len %llu\n", dirobj, name, len);

    if (len >= ULONG_MAX) {
        return ERR_NO_MEMORY;
    }
    if (find_entry(dirobj, name)) {
        return ERR_ALREADY_EXISTS;
    }

    memfs_object_t *obj = obj_alloc(dirobj->fs, dirobj, false);
    if (!obj) {
        return ERR_NO_MEMORY;
    }

    if (len > 0) {
        obj->ptr = calloc(1, len);
        if (!obj->ptr) {
            free(obj);
            return ERR_NO_MEMORY;
        }
        obj->len = len;
    }

    status_t err = obj_to_vnode(obj, out);
    if (err < 0) {
        goto err_free;
    }

    err = link_object(dirobj, name, obj);
    if (err < 0) {
        fs_vnode_destroy(*out);
        goto err_free;
    }

    return NO_ERROR;

err_free:
    free(obj->ptr);
    free(obj);
    return err;
}

static status_t memfs_mkdir(struct fs_vnode *dir, const char *name, struct fs_vnode **out) {
    memfs_object_t *dirobj = (memfs_object_t *)dir->priv;

    LTRACEF("dir %p name '%s'\n", dirobj, name);

    if (find_entry(dirobj, name)) {
        return ERR_ALREADY_EXISTS;
    }

    memfs_object_t *obj = obj_alloc(dirobj->fs, dirobj, true);
    if (!obj) {
        return ERR_NO_MEMORY;
    }

    status_t err = obj_to_vnode(obj, out);
    if (err < 0) {
        free(obj);
        return err;
    }

    err = link_object(dirobj, name, obj);
    if (err < 0) {
        fs_vnode_destroy(*out);
        free(obj);
        return err;
    }

    return NO_ERROR;
}

// shared by unlink and rmdir; the layer has already type checked the child
// and refused if it has open handles
static status_t remove_entry(struct fs_vnode *dir, const char *name, struct fs_vnode *child) {
    memfs_object_t *dirobj = (memfs_object_t *)dir->priv;
    memfs_object_t *obj = (memfs_object_t *)child->priv;
    memfs_t *fs = dirobj->fs;

    mutex_acquire(&fs->lock);

    memfs_dirent_t *ent = find_entry(dirobj, name);
    if (!ent) {
        mutex_release(&fs->lock);
        return ERR_NOT_FOUND;
    }
    DEBUG_ASSERT(ent->obj == obj);

    if (obj->is_dir && !list_is_empty(&obj->entries)) {
        mutex_release(&fs->lock);
        return ERR_NOT_ALLOWED;
    }

    // advance any open cursor pointing at the entry being removed
    struct dircookie *dc;
    list_for_every_entry(&fs->dcookies, dc, struct dircookie, node) {
        if (dc->next == ent) {
            dc->next = list_next_type(&dirobj->entries, &ent->node, memfs_dirent_t, node);
        }
    }

    list_delete(&ent->node);

    mutex_release(&fs->lock);

    free(ent);
    free(obj->ptr);
    free(obj);

    return NO_ERROR;
}

static status_t memfs_unlink(struct fs_vnode *dir, const char *name, struct fs_vnode *child) {
    LTRACEF("dir %p name '%s'\n", dir->priv, name);
    return remove_entry(dir, name, child);
}

static status_t memfs_rmdir(struct fs_vnode *dir, const char *name, struct fs_vnode *child) {
    LTRACEF("dir %p name '%s'\n", dir->priv, name);
    return remove_entry(dir, name, child);
}

static ssize_t memfs_read(struct fs_vnode *vn, void *buf, off_t off, size_t len) {
    memfs_object_t *obj = (memfs_object_t *)vn->priv;

    LTRACEF("obj %p buf %p offset %lld len %zu\n", obj, buf, off, len);

    if (obj->is_dir) {
        return ERR_NOT_FILE;
    }
    if (off < 0) {
        return ERR_INVALID_ARGS;
    }

    mutex_acquire(&obj->fs->lock);

    if (off >= (off_t)obj->len) {
        len = 0;
    } else if (off + len > obj->len) {
        len = obj->len - off;
    }

    if (len > 0) {
        memcpy(buf, obj->ptr + off, len);
    }

    mutex_release(&obj->fs->lock);

    return len;
}

static ssize_t memfs_write(struct fs_vnode *vn, const void *buf, off_t off, size_t len) {
    memfs_object_t *obj = (memfs_object_t *)vn->priv;

    LTRACEF("obj %p buf %p offset %lld len %zu\n", obj, buf, off, len);

    if (obj->is_dir) {
        return ERR_NOT_FILE;
    }
    if (off < 0) {
        return ERR_INVALID_ARGS;
    }
    if (len == 0) {
        return 0;
    }

    // the contents are one size_t sized allocation, so a write ending past
    // what size_t can name cannot be satisfied. Without this off + len
    // truncates on a 32 bit target and the realloc below is short by 4GB
    if ((uint64_t)off > (uint64_t)ULONG_MAX - len) {
        return ERR_NO_MEMORY;
    }
    const size_t end = (size_t)off + len;

    mutex_acquire(&obj->fs->lock);

    // see if this write will extend the file
    if (end > obj->len) {
        void *ptr = realloc(obj->ptr, end);
        if (!ptr) {
            mutex_release(&obj->fs->lock);
            return ERR_NO_MEMORY;
        }

        // zero out any gap created by writing past the end of the file
        if ((size_t)off > obj->len) {
            memset((uint8_t *)ptr + obj->len, 0, (size_t)off - obj->len);
        }

        obj->ptr = ptr;
        obj->len = end;
    }

    memcpy(obj->ptr + off, buf, len);

    mutex_release(&obj->fs->lock);

    return len;
}

static status_t memfs_truncate(struct fs_vnode *vn, uint64_t len) {
    memfs_object_t *obj = (memfs_object_t *)vn->priv;

    LTRACEF("obj %p, len %llu\n", obj, len);

    if (obj->is_dir) {
        return ERR_NOT_FILE;
    }

    status_t rc = NO_ERROR;

    mutex_acquire(&obj->fs->lock);

    // can't use truncate to grow a file
    if (len > obj->len) {
        rc = ERR_INVALID_ARGS;
        goto finish;
    }

    if (len == 0) {
        free(obj->ptr);
        obj->ptr = NULL;
    } else {
        void *ptr = realloc(obj->ptr, len);
        if (unlikely(ptr == NULL)) {
            rc = ERR_NO_MEMORY;
            goto finish;
        }
        obj->ptr = ptr;
    }

    obj->len = len;

finish:
    mutex_release(&obj->fs->lock);
    return rc;
}

static status_t memfs_stat(struct fs_vnode *vn, struct file_stat *stat) {
    memfs_object_t *obj = (memfs_object_t *)vn->priv;

    LTRACEF("obj %p stat %p\n", obj, stat);

    mutex_acquire(&obj->fs->lock);
    stat->is_dir = obj->is_dir;
    stat->size = obj->len;
    stat->capacity = obj->len;
    mutex_release(&obj->fs->lock);

    return NO_ERROR;
}

static status_t memfs_opendir(struct fs_vnode *vn, dircookie **dcookie) {
    memfs_object_t *dirobj = (memfs_object_t *)vn->priv;

    LTRACEF("dir %p dcookie %p\n", dirobj, dcookie);

    DEBUG_ASSERT(dirobj->is_dir);

    struct dircookie *dc = malloc(sizeof(*dc));
    if (!dc) {
        return ERR_NO_MEMORY;
    }

    dc->dir = dirobj;

    mutex_acquire(&dirobj->fs->lock);
    dc->next = list_peek_head_type(&dirobj->entries, memfs_dirent_t, node);
    list_add_head(&dirobj->fs->dcookies, &dc->node);
    mutex_release(&dirobj->fs->lock);

    *dcookie = dc;
    return NO_ERROR;
}

static status_t memfs_readdir(dircookie *dcookie, struct dirent *ent) {
    struct dircookie *dc = dcookie;

    LTRACEF("dircookie %p ent %p\n", dc, ent);

    mutex_acquire(&dc->dir->fs->lock);

    memfs_dirent_t *cur = dc->next;
    if (!cur) {
        mutex_release(&dc->dir->fs->lock);
        return ERR_NOT_FOUND;
    }

    strlcpy(ent->name, cur->name, sizeof(ent->name));
    dc->next = list_next_type(&dc->dir->entries, &cur->node, memfs_dirent_t, node);

    mutex_release(&dc->dir->fs->lock);
    return NO_ERROR;
}

static status_t memfs_closedir(dircookie *dcookie) {
    struct dircookie *dc = dcookie;

    LTRACEF("dircookie %p\n", dc);

    mutex_acquire(&dc->dir->fs->lock);
    list_delete(&dc->node);
    mutex_release(&dc->dir->fs->lock);

    free(dc);
    return NO_ERROR;
}

static const struct fs_api memfs_api = {
    .mount = memfs_mount,
    .unmount = memfs_unmount,

    .lookup = memfs_lookup,
    .create = memfs_create,
    .mkdir = memfs_mkdir,
    .unlink = memfs_unlink,
    .rmdir = memfs_rmdir,

    .read = memfs_read,
    .write = memfs_write,
    .truncate = memfs_truncate,
    .stat = memfs_stat,

    .opendir = memfs_opendir,
    .readdir = memfs_readdir,
    .closedir = memfs_closedir,
};

STATIC_FS_IMPL(memfs, &memfs_api);
