/*
 * Copyright (c) 2009-2015 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#include <lib/fs.h>

#include <assert.h>
#include <kernel/mutex.h>
#include <lib/bio.h>
#include <lk/debug.h>
#include <lk/err.h>
#include <lk/init.h>
#include <lk/list.h>
#include <lk/trace.h>
#include <stdlib.h>
#include <string.h>

#define LOCAL_TRACE 0

// The mount namespace is a tree of nodes, one per path component the layer
// knows about. A node either has a filesystem mounted at it or is scaffolding:
// an intermediate directory that exists only because a mount lives somewhere
// beneath it (mounting at /mnt/foo creates "mnt" and "foo"). Path resolution
// walks the tree by component, asking the filesystem to look up one component
// at a time, each resolved name getting a node with its vnode attached.
// Listing a scaffold node enumerates its children, which is what makes "ls /"
// work with no filesystem mounted at "/".
//
// Lifetime is reference counted: a node that nothing references is parked in
// a bounded LRU cache (see FS_NODE_CACHE_SIZE) so a rewalk of a recent path
// skips the filesystem lookups, and pruned when it falls out of it. With the
// cache disabled the resident tree is the scaffolding plus whatever a walk is
// currently standing on. Open file handles reference the vnode, not the node,
// so a file stays alive (and deduplicated by id) for as long as any handle is
// open even after its node is gone.
struct fs_node {
    struct list_node node;      // in parent->children
    struct list_node lru_node;  // in the node cache lru when ref == 0
    struct fs_node *parent;     // NULL only for the root
    struct list_node children;  // of struct fs_node
    int ref;                    // children + mounted fs + open dirhandles + walks
    struct fs_mount *mounted;   // filesystem mounted at this node, if any
    struct fs_vnode *vnode;     // object this node names, NULL for scaffolding
    char name[];                // single path component, "" for the root
};

// How many unreferenced name nodes to keep cached so a rewalk of a recent
// path skips the filesystem lookups. Zero disables caching entirely: a node
// is pruned the moment nothing references it. Override with the
// FS_NODE_CACHE_SIZE build variable.
#ifndef FS_NODE_CACHE_SIZE
#if LK_EMBEDDED
#define FS_NODE_CACHE_SIZE 0
#else
#define FS_NODE_CACHE_SIZE 64
#endif
#endif

struct fs_mount {
    struct list_node node;      // in the mounts list

    char *path;                 // normalized mount point path, for display
    struct fs_node *mountpoint; // node this filesystem is mounted at
    bdev_t *dev;
    fscookie *cookie;
    int ref;
    const struct fs_impl *fs;
    const struct fs_api *api;

    struct fs_vnode *root;      // root vnode
    struct list_node vnodes;    // every live vnode of this mount, for dedup by id

    // fs_unmount has run: the mount is gone as far as the namespace is
    // concerned and only the deferred teardown behind still-open handles is
    // outstanding. A second unmount must not drop its references again
    bool unmounting;
};

struct filehandle {
    struct fs_mount *mount;
    struct fs_vnode *vnode;
};

struct dirhandle {
    struct fs_mount *mount;     // NULL for a scaffold directory handle
    dircookie *cookie;          // fs cursor, or a scaffold_dircookie
    struct fs_vnode *vnode;     // the directory, NULL for a scaffold handle
};

// protects the node tree, all refcounts, the mount list, the vnode lists and
// the open scaffold dir cookies; held across the namespace ops
// (lookup/create/mkdir/unlink/rmdir, and readlink, which the walk calls to
// follow a symlink), but never across file I/O
static mutex_t fs_lock = MUTEX_INITIAL_VALUE(fs_lock);
static struct list_node mounts = LIST_INITIAL_VALUE(mounts);

static struct fs_node fs_root = {
    .node = LIST_INITIAL_CLEARED_VALUE,
    .lru_node = LIST_INITIAL_CLEARED_VALUE,
    .parent = NULL,
    .children = LIST_INITIAL_VALUE(fs_root.children),
    .ref = 1, // never freed
    .mounted = NULL,
    .vnode = NULL,
};

// unreferenced but cached nodes, oldest at the head; all under fs_lock
static struct list_node node_cache_lru = LIST_INITIAL_VALUE(node_cache_lru);
static int node_cache_count;
static int node_cache_max = FS_NODE_CACHE_SIZE;

// an open directory handle on a scaffold node; the cursor is a live pointer
// into the child list, fixed up when the node it points at is pruned
struct scaffold_dircookie {
    struct list_node node;      // in active_scaffold_cookies
    struct fs_node *dir;        // holds a ref
    struct fs_node *next_child; // next child to report, NULL = exhausted
};
static struct list_node active_scaffold_cookies = LIST_INITIAL_VALUE(active_scaffold_cookies);

// defined by the linker, wrapping all structs in the "fs_impl" section
extern const struct fs_impl __start_fs_impl[] __WEAK;
extern const struct fs_impl __stop_fs_impl[] __WEAK;

static const struct fs_impl *find_fs(const char *name) {
    for (const struct fs_impl *fs = __start_fs_impl; fs != __stop_fs_impl; fs++) {
        if (!strcmp(name, fs->name)) {
            return fs;
        }
    }
    return NULL;
}

void fs_dump_list(void) {
    for (const struct fs_impl *fs = __start_fs_impl; fs != __stop_fs_impl; fs++) {
        puts(fs->name);
    }
}

void fs_dump_mounts(void) {
    printf("%-16s%s\n", "Filesystem", "Path");
    mutex_acquire(&fs_lock);
    struct fs_mount *mount;
    list_for_every_entry(&mounts, mount, struct fs_mount, node) {
        printf("%-16s%s\n", mount->fs->name, mount->path);
    }
    mutex_release(&fs_lock);
}

// ---------------------------------------------------------------------------
// vnode lifetime, all called with fs_lock held unless noted
// ---------------------------------------------------------------------------

status_t fs_vnode_create(uint64_t id, enum fs_vnode_type type, void *priv,
                         struct fs_vnode **out) {
    struct fs_vnode *vn = malloc(sizeof(*vn));
    if (!vn) {
        return ERR_NO_MEMORY;
    }

    list_clear_node(&vn->node);
    vn->mount = NULL;
    vn->ref = 1; // the reference the filesystem hands back to the layer
    vn->cookie = NULL;
    vn->id = id;
    vn->type = type;
    vn->priv = priv;

    *out = vn;
    return NO_ERROR;
}

void fs_vnode_destroy(struct fs_vnode *vn) {
    DEBUG_ASSERT(vn->mount == NULL);
    free(vn);
}

// Take ownership of a vnode freshly returned by the filesystem: bind it to
// its mount and deduplicate by id, so two lookups of one object always yield
// one vnode. Returns the canonical vnode carrying the caller's reference.
static struct fs_vnode *vnode_adopt_locked(struct fs_mount *mount, struct fs_vnode *vn) {
    DEBUG_ASSERT(vn->mount == NULL);

    vn->mount = mount;
    vn->cookie = mount->cookie;

    if (vn->id != 0) {
        struct fs_vnode *existing;
        list_for_every_entry(&mount->vnodes, existing, struct fs_vnode, node) {
            if (existing->id == vn->id) {
                existing->ref++;
                // discard the duplicate the filesystem just handed us
                if (mount->api->release) {
                    mount->api->release(vn);
                }
                free(vn);
                return existing;
            }
        }
    }

    list_add_tail(&mount->vnodes, &vn->node);
    return vn;
}

static void vnode_release_locked(struct fs_vnode *vn) {
    DEBUG_ASSERT(vn->ref > 0);
    if (--vn->ref == 0) {
        list_delete(&vn->node);
        if (vn->mount->api->release) {
            vn->mount->api->release(vn);
        }
        free(vn);
    }
}

static void vnode_put(struct fs_vnode *vn) {
    mutex_acquire(&fs_lock);
    vnode_release_locked(vn);
    mutex_release(&fs_lock);
}

// ---------------------------------------------------------------------------
// node tree primitives, all called with fs_lock held
// ---------------------------------------------------------------------------

static struct fs_node *node_find_child(struct fs_node *parent, const char *name, size_t len) {
    struct fs_node *child;
    list_for_every_entry(&parent->children, child, struct fs_node, node) {
        if (strlen(child->name) == len && memcmp(child->name, name, len) == 0) {
            return child;
        }
    }
    return NULL;
}

// Creates a child with one reference, held by the caller; also accounts the
// child's reference on the parent.
static struct fs_node *node_add_child(struct fs_node *parent, const char *name, size_t len) {
    struct fs_node *child = malloc(sizeof(struct fs_node) + len + 1);
    if (!child) {
        return NULL;
    }

    memcpy(child->name, name, len);
    child->name[len] = 0;
    list_clear_node(&child->lru_node);
    child->parent = parent;
    list_initialize(&child->children);
    child->ref = 1;
    child->mounted = NULL;
    child->vnode = NULL;

    list_add_tail(&parent->children, &child->node);
    parent->ref++;

    return child;
}

// take a reference on an existing node, reviving it if it was parked in the
// node cache
static void node_ref(struct fs_node *node) {
    if (node->ref++ == 0) {
        list_delete(&node->lru_node);
        node_cache_count--;
    }
}

// Called before a child node is unlinked: any open scaffold dir cursor
// pointing at it is advanced to the next sibling.
static void scaffold_child_removed(struct fs_node *child) {
    struct fs_node *next = list_next_type(&child->parent->children, &child->node,
                                          struct fs_node, node);
    struct scaffold_dircookie *dc;
    list_for_every_entry(&active_scaffold_cookies, dc, struct scaffold_dircookie, node) {
        if (dc->next_child == child) {
            dc->next_child = next;
        }
    }
}

// unlink and free a node whose count is zero and which is not on the lru;
// returns the parent, whose reference the node held and the caller now owns
static struct fs_node *node_prune_one(struct fs_node *node) {
    struct fs_node *parent = node->parent;

    DEBUG_ASSERT(parent); // the root holds a permanent self reference
    DEBUG_ASSERT(list_is_empty(&node->children));
    DEBUG_ASSERT(!node->mounted);

    scaffold_child_removed(node);
    list_delete(&node->node);
    if (node->vnode) {
        vnode_release_locked(node->vnode);
    }
    free(node);

    return parent;
}

// Drop a reference. A filesystem name whose count hits zero is parked in the
// node cache (when one is configured) so a rewalk skips the lookup; anything
// else, and the oldest cache entries once over capacity, is pruned, which
// drops the reference on the vnode and, iteratively, on the parent. Only
// leaves can be at zero -- a child holds its parent's count up -- so cached
// nodes never have children.
static void node_release(struct fs_node *node) {
    for (;;) {
        while (node && --node->ref == 0) {
            if (node->vnode && node_cache_max > 0) {
                list_add_tail(&node_cache_lru, &node->lru_node);
                node_cache_count++;
                break;
            }
            node = node_prune_one(node);
        }

        if (node_cache_count <= node_cache_max) {
            return;
        }

        struct fs_node *evict = list_remove_head_type(&node_cache_lru, struct fs_node, lru_node);
        node_cache_count--;
        // continue by dropping the reference the evicted node held on its parent
        node = node_prune_one(evict);
    }
}

static void node_put(struct fs_node *node) {
    mutex_acquire(&fs_lock);
    node_release(node);
    mutex_release(&fs_lock);
}

// the filesystem object a node presents: a mounted filesystem's root, or the
// vnode a walk attached
static struct fs_vnode *node_vnode(struct fs_node *node) {
    return node->mounted ? node->mounted->root : node->vnode;
}

void fs_set_node_cache_size(int size) {
    if (size < 0) {
        size = 0;
    }

    mutex_acquire(&fs_lock);
    node_cache_max = size;
    while (node_cache_count > node_cache_max) {
        struct fs_node *evict = list_remove_head_type(&node_cache_lru, struct fs_node, lru_node);
        node_cache_count--;
        node_release(node_prune_one(evict));
    }
    mutex_release(&fs_lock);
}

int fs_get_node_cache_size(void) {
    return node_cache_max;
}

// ---------------------------------------------------------------------------
// the walk
// ---------------------------------------------------------------------------

enum fs_walk_kind {
    FS_WALK_FAILED = 0, // status in err
    FS_WALK_NODE,       // resolved the whole path to a node (ref held); mount
                        // and vnode refs held too unless it is pure scaffolding
    FS_WALK_PARENT,     // stopped at the parent directory inside a filesystem:
                        // node + mount refs held, last is the final component
};

struct fs_walk_result {
    enum fs_walk_kind kind;
    status_t err;
    struct fs_mount *mount;
    struct fs_node *node;  // FS_WALK_NODE / FS_WALK_PARENT
    struct fs_vnode *vnode; // FS_WALK_NODE, ref held; NULL for pure scaffolding
    const char *last;      // FS_WALK_PARENT; points into the caller's buffer
};

#define FS_WALK_FLAG_PARENT 0x1

// How many symlinks one walk may follow before giving up, which is what bounds
// a loop.
#define FS_MAX_SYMLINK_DEPTH 8

// Rewrite path in place, replacing the symlink component starting at comp
// (with rest naming everything after it) by the link's target, and renormalize
// the result. An absolute target drops the whole prefix, so it resolves
// against the mount namespace root rather than the filesystem's own root; the
// caller restarts the walk from the root either way, which is also what makes
// ".." inside a target behave lexically like it does anywhere else.
static status_t walk_splice_symlink(char *path, size_t path_size, const char *comp,
                                    const char *rest, struct fs_vnode *link) {
    if (!link->mount->api->readlink) {
        return ERR_NOT_SUPPORTED;
    }

    char target[FS_MAX_PATH_LEN];
    ssize_t tlen = link->mount->api->readlink(link, target, sizeof(target));
    if (tlen < 0) {
        return (status_t)tlen;
    }
    if (tlen == 0) {
        // a link to nowhere names nothing
        return ERR_NOT_FOUND;
    }

    size_t prefix_len = (target[0] == '/') ? 0 : (size_t)(comp - path);
    size_t rest_len = strlen(rest);
    if (prefix_len + (size_t)tlen + rest_len + 1 > path_size) {
        return ERR_NOT_ENOUGH_BUFFER;
    }

    // move the tail out to its new place first, since it overlaps the target
    memmove(path + prefix_len + tlen, rest, rest_len + 1);
    memcpy(path + prefix_len, target, tlen);
    fs_normalize_path(path);

    return NO_ERROR;
}

// Walk a normalized path through the tree, asking each filesystem to resolve
// one component at a time. The buffer is scratch: the walk pokes
// temporary NUL terminators into it, and following a symlink rewrites it
// outright, so a caller that needs the path afterwards must keep its own copy.
// The result's last pointer points into it and stays valid until then.
static void fs_walk(char *path, size_t path_size, uint flags, struct fs_walk_result *res) {
    memset(res, 0, sizeof(*res));
    res->kind = FS_WALK_FAILED;
    res->err = ERR_NOT_FOUND;

    // the normalized form of "/" is the empty string, which names the root
    // node; everything else must be absolute
    if (path[0] != '/' && path[0] != 0) {
        return;
    }

    mutex_acquire(&fs_lock);

    struct fs_node *cur = &fs_root;
    cur->ref++;
    char *pos = path; // always at a '/' or the terminator
    uint symlink_depth = 0;

    for (;;) {
        struct fs_vnode *curvn = node_vnode(cur);

        if (pos[0] == 0) {
            // resolved the whole path to this node
            if (flags & FS_WALK_FLAG_PARENT) {
                // the parent of "/" (or of a mount point seen from outside)
                // is not inside any filesystem
                node_release(cur);
                mutex_release(&fs_lock);
                res->err = ERR_NOT_FOUND;
                return;
            }
            if (curvn) {
                // take the vnode reference here, under the lock that the
                // resolution ran under, rather than leaving the caller to
                // pick it up afterwards: in that window a concurrent remove
                // would see a node reference it cannot classify and unlink
                // the object out from under this walk
                curvn->mount->ref++;
                curvn->ref++;
                res->mount = curvn->mount;
                res->vnode = curvn;
            }
            mutex_release(&fs_lock);
            res->kind = FS_WALK_NODE;
            res->node = cur;
            return;
        }

        // normalized paths have no empty components
        char *comp = pos + 1;
        char *end = strchr(comp, '/');
        size_t len = end ? (size_t)(end - comp) : strlen(comp);
        char *rest = comp + len; // at the next '/' or the terminator

        // parent mode stops before the last component
        if ((flags & FS_WALK_FLAG_PARENT) && rest[0] == 0) {
            if (!curvn) {
                // the parent would be scaffolding, which no filesystem backs
                node_release(cur);
                mutex_release(&fs_lock);
                res->err = ERR_NOT_FOUND;
                return;
            }
            if (curvn->type != FS_VNODE_DIR) {
                node_release(cur);
                mutex_release(&fs_lock);
                res->err = ERR_NOT_DIR;
                return;
            }
            curvn->mount->ref++;
            mutex_release(&fs_lock);
            res->kind = FS_WALK_PARENT;
            res->mount = curvn->mount;
            res->node = cur;
            res->last = comp;
            return;
        }

        struct fs_node *child = node_find_child(cur, comp, len);
        if (child) {
            node_ref(child);
        } else {
            if (!curvn) {
                // a scaffold miss: the layer knows nothing below here
                node_release(cur);
                mutex_release(&fs_lock);
                res->err = ERR_NOT_FOUND;
                return;
            }
            if (curvn->type != FS_VNODE_DIR) {
                node_release(cur);
                mutex_release(&fs_lock);
                res->err = ERR_NOT_DIR;
                return;
            }

            // ask the filesystem for this one component
            char saved = *rest;
            *rest = 0;
            struct fs_vnode *vn;
            status_t err = curvn->mount->api->lookup(curvn, comp, &vn);
            *rest = saved;
            if (err < 0) {
                node_release(cur);
                mutex_release(&fs_lock);
                res->err = err;
                return;
            }

            vn = vnode_adopt_locked(curvn->mount, vn);
            child = node_add_child(cur, comp, len);
            if (!child) {
                vnode_release_locked(vn);
                node_release(cur);
                mutex_release(&fs_lock);
                res->err = ERR_NO_MEMORY;
                return;
            }
            child->vnode = vn; // the node owns this reference
        }

        // a symlink names something else: splice its target into the path and
        // start over from the root
        struct fs_vnode *childvn = node_vnode(child);
        if (childvn && childvn->type == FS_VNODE_SYMLINK) {
            status_t err;
            if (++symlink_depth > FS_MAX_SYMLINK_DEPTH) {
                err = ERR_RECURSE_TOO_DEEP;
            } else {
                err = walk_splice_symlink(path, path_size, comp, rest, childvn);
            }
            node_release(child);
            node_release(cur);
            if (err < 0) {
                mutex_release(&fs_lock);
                res->err = err;
                return;
            }

            cur = &fs_root;
            cur->ref++;
            pos = path;
            continue;
        }

        node_release(cur);
        cur = child;
        pos = rest;
    }
}

// Consume a FS_WALK_NODE result, keeping the vnode reference the walk already
// took; the node is released (and pruned if nothing else holds it, the vnode
// surviving through the returned reference).
static struct fs_vnode *walk_node_to_vnode(struct fs_walk_result *res) {
    DEBUG_ASSERT(res->vnode);
    node_put(res->node);
    return res->vnode;
}

// decrement the ref to the mount structure, which may
// cause an unmount operation
static void put_mount(struct fs_mount *mount) {
    mutex_acquire(&fs_lock);
    if ((--mount->ref) == 0) {
        LTRACEF("last ref, unmounting fs at '%s'\n", mount->path);

        struct fs_node *mountpoint = mount->mountpoint;

        // evict every cached name belonging to this mount; pruning one can
        // park its parent, so rescan until a pass finds nothing
        bool again = true;
        while (again) {
            again = false;
            struct fs_node *n, *temp;
            list_for_every_entry_safe(&node_cache_lru, n, temp, struct fs_node, lru_node) {
                if (n->vnode && n->vnode->mount == mount) {
                    list_delete(&n->lru_node);
                    node_cache_count--;
                    node_release(node_prune_one(n));
                    again = true;
                    break;
                }
            }
        }

        mountpoint->mounted = NULL;

        list_delete(&mount->node);
        vnode_release_locked(mount->root);
        DEBUG_ASSERT(list_is_empty(&mount->vnodes));
        mount->api->unmount(mount->cookie);
        free(mount->path);
        if (mount->dev) {
            bio_close(mount->dev);
        }
        free(mount);

        // prunes the scaffolding up to the nearest still-referenced node
        node_release(mountpoint);
    }
    mutex_release(&fs_lock);
}

static status_t mount(const char *path, const char *device, const struct fs_impl *fs, enum fs_mount_options options) {
    struct fs_mount *mount;
    char temppath[FS_MAX_PATH_LEN];

    strlcpy(temppath, path, sizeof(temppath));
    fs_normalize_path(temppath);

    if (temppath[0] != '/') {
        return ERR_BAD_PATH;
    }

    /* open a bio device if the string is nonnull */
    bdev_t *dev = NULL;
    if (device && device[0] != '\0') {
        dev = bio_open(device);
        if (!dev) {
            return ERR_NOT_FOUND;
        }
    }

    /* call into the fs implementation */
    fscookie *cookie;
    struct fs_vnode *root = NULL;
    status_t err = fs->api->mount(dev, options, &cookie, &root);
    if (err < 0) {
        if (dev) {
            bio_close(dev);
        }
        return err;
    }

    // Bind the root to its filesystem now rather than leaving it to
    // vnode_adopt_locked at the end: every error unwind from here on calls
    // release(), and a filesystem is entitled to reach its own instance
    // through the cookie there.
    if (root) {
        root->cookie = cookie;
    }

    if (!root || root->type != FS_VNODE_DIR) {
        // a filesystem must produce a directory as its root
        err = ERR_NOT_VALID;
        goto err_unmount;
    }

    /* create the mount structure */
    mount = malloc(sizeof(struct fs_mount));
    if (!mount) {
        err = ERR_NO_MEMORY;
        goto err_unmount;
    }
    mount->path = strdup(temppath);
    if (!mount->path) {
        free(mount);
        err = ERR_NO_MEMORY;
        goto err_unmount;
    }
    mount->dev = dev;
    mount->cookie = cookie;
    mount->ref = 1;
    mount->fs = fs;
    mount->api = fs->api;
    mount->root = NULL;
    mount->unmounting = false;
    list_initialize(&mount->vnodes);

    /* walk to the mount point, creating scaffold nodes for any missing
     * components, and attach the mount there */
    mutex_acquire(&fs_lock);

    struct fs_node *cur = &fs_root;
    cur->ref++;
    const char *pos = temppath;
    while (pos[0] != 0) {
        if (cur->mounted || cur->vnode) {
            // raced with another mount that now covers this path, or the path
            // leads into a filesystem
            node_release(cur);
            mutex_release(&fs_lock);
            free(mount->path);
            free(mount);
            err = ERR_ALREADY_MOUNTED;
            goto err_unmount;
        }

        const char *comp = pos + 1;
        const char *end = strchr(comp, '/');
        size_t len = end ? (size_t)(end - comp) : strlen(comp);

        struct fs_node *child = node_find_child(cur, comp, len);
        if (child) {
            node_ref(child);
        } else {
            child = node_add_child(cur, comp, len);
        }
        node_release(cur);
        if (!child) {
            mutex_release(&fs_lock);
            free(mount->path);
            free(mount);
            err = ERR_NO_MEMORY;
            goto err_unmount;
        }

        cur = child;
        pos = comp + len;
    }

    // Children on a node that has no vnode of its own mean scaffolding, and the
    // walk above only ever builds scaffolding on the way to a mount, so a mount
    // lives somewhere below here. Covering it would leave a namespace whose
    // listing and whose resolution disagree about what is at this path.
    if (cur->mounted || cur->vnode || !list_is_empty(&cur->children)) {
        node_release(cur);
        mutex_release(&fs_lock);
        free(mount->path);
        free(mount);
        err = ERR_ALREADY_MOUNTED;
        goto err_unmount;
    }

    /* the walk's reference on the mount point becomes the mount's */
    mount->mountpoint = cur;
    cur->mounted = mount;

    /* adopt the root vnode */
    if (root) {
        mount->root = vnode_adopt_locked(mount, root);
    }

    list_add_head(&mounts, &mount->node);

    mutex_release(&fs_lock);

    return 0;

err_unmount:
    if (root) {
        if (fs->api->release) {
            fs->api->release(root);
        }
        free(root);
    }
    fs->api->unmount(cookie);
    if (dev) {
        bio_close(dev);
    }
    return err;
}

status_t fs_format_device(const char *fsname, const char *device, const void *args) {
    const struct fs_impl *fs = find_fs(fsname);
    if (!fs) {
        return ERR_NOT_FOUND;
    }

    if (fs->api->format == NULL) {
        return ERR_NOT_SUPPORTED;
    }

    bdev_t *dev = NULL;
    if (device && device[0] != '\0') {
        dev = bio_open(device);
        if (!dev) {
            return ERR_NOT_FOUND;
        }
    }

    return fs->api->format(dev, args);
}

status_t fs_mount(const char *path, const char *fsname, const char *device, enum fs_mount_options options) {
    const struct fs_impl *fs = find_fs(fsname);
    if (!fs) {
        return ERR_NOT_FOUND;
    }

    return mount(path, device, fs, options);
}

status_t fs_unmount(const char *path) {
    char temppath[FS_MAX_PATH_LEN];

    strlcpy(temppath, path, sizeof(temppath));
    fs_normalize_path(temppath);

    struct fs_walk_result res;
    fs_walk(temppath, sizeof(temppath), 0, &res);

    struct fs_mount *mount;
    switch (res.kind) {
        case FS_WALK_NODE: {
            mutex_acquire(&fs_lock);

            // only the mount point itself may be unmounted, not a path inside
            // it, and only once: a mount whose teardown is deferred behind an
            // open handle has already had these two references spent
            const bool ours = res.node->mounted && !res.node->mounted->unmounting;
            if (ours) {
                DEBUG_ASSERT(res.mount == res.node->mounted);
                res.node->mounted->unmounting = true;
            }

            // the walk's vnode reference has to go back before the mount
            // references do, or the last put tears the mount down with its
            // root still referenced
            if (res.vnode) {
                vnode_release_locked(res.vnode);
            }
            node_release(res.node);
            mutex_release(&fs_lock);

            if (!ours) {
                // scaffolding, a name inside a filesystem, or already unmounted
                if (res.mount) {
                    put_mount(res.mount);
                }
                return ERR_NOT_FOUND;
            }
            mount = res.mount;
            break;
        }
        default:
            return res.err;
    }

    // return the ref that the walk added and one extra
    put_mount(mount);
    put_mount(mount);

    return 0;
}

status_t fs_open_file(const char *path, filehandle **handle) {
    char temppath[FS_MAX_PATH_LEN];

    strlcpy(temppath, path, sizeof(temppath));
    fs_normalize_path(temppath);

    LTRACEF("path %s temppath %s\n", path, temppath);

    struct fs_walk_result res;
    fs_walk(temppath, sizeof(temppath), 0, &res);

    filehandle *f;
    switch (res.kind) {
        case FS_WALK_NODE: {
            if (!res.mount) {
                // pure scaffolding is not a file
                node_put(res.node);
                return ERR_NOT_FOUND;
            }
            struct fs_vnode *vn = walk_node_to_vnode(&res);

            f = malloc(sizeof(*f));
            if (!f) {
                vnode_put(vn);
                put_mount(res.mount);
                return ERR_NO_MEMORY;
            }
            f->vnode = vn;
            f->mount = res.mount;
            break;
        }
        default:
            return res.err;
    }

    *handle = f;
    return 0;
}

status_t fs_create_file(const char *path, filehandle **handle, uint64_t len) {
    char temppath[FS_MAX_PATH_LEN];

    strlcpy(temppath, path, sizeof(temppath));
    fs_normalize_path(temppath);

    struct fs_walk_result res;
    fs_walk(temppath, sizeof(temppath), FS_WALK_FLAG_PARENT, &res);

    filehandle *f;
    switch (res.kind) {
        case FS_WALK_PARENT: {
            if (!res.mount->api->create) {
                node_put(res.node);
                put_mount(res.mount);
                return ERR_NOT_SUPPORTED;
            }

            mutex_acquire(&fs_lock);
            struct fs_vnode *vn;
            status_t err = res.mount->api->create(node_vnode(res.node), res.last, len, &vn);
            if (err < 0) {
                node_release(res.node);
                mutex_release(&fs_lock);
                put_mount(res.mount);
                return err;
            }
            vn = vnode_adopt_locked(res.mount, vn);
            node_release(res.node);
            mutex_release(&fs_lock);

            f = malloc(sizeof(*f));
            if (!f) {
                vnode_put(vn);
                put_mount(res.mount);
                return ERR_NO_MEMORY;
            }
            f->vnode = vn;
            f->mount = res.mount;
            break;
        }
        default:
            return res.err;
    }

    *handle = f;
    return 0;
}

status_t fs_make_dir(const char *path) {
    char temppath[FS_MAX_PATH_LEN];

    strlcpy(temppath, path, sizeof(temppath));
    fs_normalize_path(temppath);

    struct fs_walk_result res;
    fs_walk(temppath, sizeof(temppath), FS_WALK_FLAG_PARENT, &res);

    switch (res.kind) {
        case FS_WALK_PARENT: {
            if (!res.mount->api->mkdir) {
                node_put(res.node);
                put_mount(res.mount);
                return ERR_NOT_SUPPORTED;
            }

            mutex_acquire(&fs_lock);
            struct fs_vnode *vn;
            status_t err = res.mount->api->mkdir(node_vnode(res.node), res.last, &vn);
            if (err >= 0) {
                vn = vnode_adopt_locked(res.mount, vn);
                vnode_release_locked(vn);
            }
            node_release(res.node);
            mutex_release(&fs_lock);
            put_mount(res.mount);
            return err;
        }
        default:
            return res.err;
    }
}

// shared with fs_remove_dir; expects the child to be of the given type and
// dispatches to the matching filesystem op
static status_t remove_common(const char *path, bool dir) {
    char temppath[FS_MAX_PATH_LEN];

    strlcpy(temppath, path, sizeof(temppath));
    fs_normalize_path(temppath);

    struct fs_walk_result res;
    fs_walk(temppath, sizeof(temppath), FS_WALK_FLAG_PARENT, &res);

    switch (res.kind) {
        case FS_WALK_PARENT: {
            status_t (*op)(struct fs_vnode *, const char *, struct fs_vnode *) =
                dir ? res.mount->api->rmdir : res.mount->api->unlink;
            if (!op) {
                node_put(res.node);
                put_mount(res.mount);
                return ERR_NOT_SUPPORTED;
            }

            mutex_acquire(&fs_lock);

            struct fs_vnode *dirvn = node_vnode(res.node);
            status_t err;

            // if the name is parked in the node cache, evict it: the entry is
            // about to be invalidated. a node still referenced is a directory
            // with cached descendants, which the filesystem will refuse as
            // non-empty; it is accounted for in the handle check below.
            struct fs_node *child = node_find_child(res.node, res.last, strlen(res.last));
            if (child && child->ref == 0) {
                list_delete(&child->lru_node);
                node_cache_count--;
                struct fs_node *parent = node_prune_one(child);
                DEBUG_ASSERT(parent == res.node);
                parent->ref--; // the reference the cached child held; ours keeps it alive
                child = NULL;
            }

            // resolve the child so it can be checked and passed to the fs
            struct fs_vnode *cvn;
            err = res.mount->api->lookup(dirvn, res.last, &cvn);
            if (err < 0) {
                goto out;
            }
            cvn = vnode_adopt_locked(res.mount, cvn);

            if (dir && cvn->type != FS_VNODE_DIR) {
                err = ERR_NOT_DIR;
                vnode_release_locked(cvn);
                goto out;
            }
            if (!dir && cvn->type == FS_VNODE_DIR) {
                err = ERR_NOT_FILE;
                vnode_release_locked(cvn);
                goto out;
            }

            // the layer refuses to unlink anything still in use: the only
            // references a free object can have are our transient one and its
            // own tree node's. A walk that resolved to it holds one too, which
            // is what makes a concurrent open visible here rather than a bare
            // node reference this check could not classify
            if (cvn->ref > 1 + (child ? 1 : 0)) {
                err = ERR_BUSY;
                vnode_release_locked(cvn);
                goto out;
            }

            err = op(dirvn, res.last, cvn);
            // on success nothing can still name the child: a referenced node
            // would have made the filesystem refuse the op as non-empty
            DEBUG_ASSERT(err < 0 || !child);
            vnode_release_locked(cvn);

        out:
            node_release(res.node);
            mutex_release(&fs_lock);
            put_mount(res.mount);
            return err;
        }
        default:
            return res.err;
    }
}

status_t fs_remove_file(const char *path) {
    return remove_common(path, false);
}

status_t fs_remove_dir(const char *path) {
    return remove_common(path, true);
}

status_t fs_file_ioctl(filehandle *handle, int request, void *argp) {
    LTRACEF("filehandle %p, request %d, argp, %p\n", handle, request, argp);

    if (!handle->mount->api->ioctl) {
        return ERR_NOT_SUPPORTED;
    }
    return handle->mount->api->ioctl(handle->vnode, request, argp);
}

status_t fs_truncate_file(filehandle *handle, uint64_t len) {
    LTRACEF("filehandle %p, length %llu\n", handle, len);

    if (!handle->mount->api->truncate) {
        return ERR_NOT_SUPPORTED;
    }
    return handle->mount->api->truncate(handle->vnode, len);
}

ssize_t fs_read_file(filehandle *handle, void *buf, off_t offset, size_t len) {
    return handle->mount->api->read(handle->vnode, buf, offset, len);
}

ssize_t fs_write_file(filehandle *handle, const void *buf, off_t offset, size_t len) {
    if (!handle->mount->api->write) {
        return ERR_NOT_SUPPORTED;
    }
    return handle->mount->api->write(handle->vnode, buf, offset, len);
}

status_t fs_close_file(filehandle *handle) {
    vnode_put(handle->vnode);
    put_mount(handle->mount);
    free(handle);
    return 0;
}

status_t fs_stat_file(filehandle *handle, struct file_stat *stat) {
    // zero the struct so fields a filesystem does not fill in (e.g. capacity)
    // read as zero instead of stack garbage
    memset(stat, 0, sizeof(*stat));

    if (!handle->mount->api->stat) {
        return ERR_NOT_SUPPORTED;
    }
    return handle->mount->api->stat(handle->vnode, stat);
}

// ---------------------------------------------------------------------------
// scaffold directory handles: listing a node of the mount namespace itself
// ---------------------------------------------------------------------------

// takes over the caller's reference on the node
static status_t scaffold_opendir(struct fs_node *node, struct scaffold_dircookie **out) {
    struct scaffold_dircookie *dc = malloc(sizeof(*dc));
    if (!dc) {
        return ERR_NO_MEMORY;
    }

    dc->dir = node;

    mutex_acquire(&fs_lock);
    dc->next_child = list_peek_head_type(&node->children, struct fs_node, node);
    list_add_tail(&active_scaffold_cookies, &dc->node);
    mutex_release(&fs_lock);

    *out = dc;
    return NO_ERROR;
}

static status_t scaffold_readdir(struct scaffold_dircookie *dc, struct dirent *ent) {
    mutex_acquire(&fs_lock);

    struct fs_node *child = dc->next_child;
    if (!child) {
        mutex_release(&fs_lock);
        return ERR_NOT_FOUND;
    }

    strlcpy(ent->name, child->name, sizeof(ent->name));
    dc->next_child = list_next_type(&dc->dir->children, &child->node, struct fs_node, node);

    mutex_release(&fs_lock);
    return NO_ERROR;
}

static void scaffold_closedir(struct scaffold_dircookie *dc) {
    mutex_acquire(&fs_lock);
    list_delete(&dc->node);
    node_release(dc->dir);
    mutex_release(&fs_lock);
    free(dc);
}

status_t fs_open_dir(const char *path, dirhandle **handle) {
    char temppath[FS_MAX_PATH_LEN];

    strlcpy(temppath, path, sizeof(temppath));
    fs_normalize_path(temppath);

    LTRACEF("path %s temppath %s\n", path, temppath);

    struct fs_walk_result res;
    fs_walk(temppath, sizeof(temppath), 0, &res);

    dirhandle *d;
    switch (res.kind) {
        case FS_WALK_NODE: {
            if (!res.mount) {
                // a directory of the mount namespace itself
                struct scaffold_dircookie *dc;
                status_t err = scaffold_opendir(res.node, &dc);
                if (err < 0) {
                    node_put(res.node);
                    return err;
                }

                d = malloc(sizeof(*d));
                if (!d) {
                    scaffold_closedir(dc);
                    return ERR_NO_MEMORY;
                }
                d->cookie = (dircookie *)dc;
                d->mount = NULL; // sentinel: scaffold handle
                d->vnode = NULL;
                break;
            }

            struct fs_vnode *vn = walk_node_to_vnode(&res);
            if (vn->type != FS_VNODE_DIR) {
                vnode_put(vn);
                put_mount(res.mount);
                return ERR_NOT_DIR;
            }
            if (!res.mount->api->opendir) {
                vnode_put(vn);
                put_mount(res.mount);
                return ERR_NOT_SUPPORTED;
            }

            dircookie *cookie;
            status_t err = res.mount->api->opendir(vn, &cookie);
            if (err < 0) {
                vnode_put(vn);
                put_mount(res.mount);
                return err;
            }

            d = malloc(sizeof(*d));
            if (!d) {
                res.mount->api->closedir(cookie);
                vnode_put(vn);
                put_mount(res.mount);
                return ERR_NO_MEMORY;
            }
            d->cookie = cookie;
            d->mount = res.mount;
            d->vnode = vn;
            break;
        }
        default:
            return res.err;
    }

    *handle = d;
    return 0;
}

status_t fs_read_dir(dirhandle *handle, struct dirent *ent) {
    if (!handle->mount) {
        return scaffold_readdir((struct scaffold_dircookie *)handle->cookie, ent);
    }

    return handle->mount->api->readdir(handle->cookie, ent);
}

status_t fs_close_dir(dirhandle *handle) {
    if (!handle->mount) {
        scaffold_closedir((struct scaffold_dircookie *)handle->cookie);
        free(handle);
        return NO_ERROR;
    }

    handle->mount->api->closedir(handle->cookie);
    vnode_put(handle->vnode);
    put_mount(handle->mount);
    free(handle);
    return 0;
}

status_t fs_stat_fs(const char *mountpoint, struct fs_stat *stat) {
    LTRACEF("mountpoint %s stat %p\n", mountpoint, stat);

    char temppath[FS_MAX_PATH_LEN];

    strlcpy(temppath, mountpoint, sizeof(temppath));
    fs_normalize_path(temppath);

    struct fs_mount *mount;
    struct fs_walk_result res;
    fs_walk(temppath, sizeof(temppath), 0, &res);

    switch (res.kind) {
        case FS_WALK_NODE:
            if (!res.mount) {
                node_put(res.node);
                return ERR_NOT_FOUND;
            }
            mount = res.mount;
            node_put(res.node);
            vnode_put(res.vnode);
            break;
        default:
            return res.err;
    }

    if (!mount->api->fs_stat) {
        put_mount(mount);
        return ERR_NOT_SUPPORTED;
    }

    memset(stat, 0, sizeof(*stat));
    status_t result = mount->api->fs_stat(mount->cookie, stat);

    put_mount(mount);

    return result;
}

void fs_normalize_path(char *path) {
    int outpos;
    int pos;
    char c;
    bool done;
    enum {
        INITIAL,
        FIELD_START,
        IN_FIELD,
        SEP,
        SEEN_SEP,
        DOT,
        SEEN_DOT,
        DOTDOT,
        SEEN_DOTDOT,
    } state;

    state = INITIAL;
    pos = 0;
    outpos = 0;
    done = false;

    /* remove duplicate path separators, flatten empty fields (only composed of .), backtrack fields with .., remove trailing slashes */
    while (!done) {
        c = path[pos];
        switch (state) {
            case INITIAL:
                if (c == '/') {
                    state = SEP;
                } else if (c == '.') {
                    state = DOT;
                } else {
                    state = FIELD_START;
                }
                break;
            case FIELD_START:
                if (c == '.') {
                    state = DOT;
                } else if (c == 0) {
                    done = true;
                } else {
                    state = IN_FIELD;
                }
                break;
            case IN_FIELD:
                if (c == '/') {
                    state = SEP;
                } else if (c == 0) {
                    done = true;
                } else {
                    path[outpos++] = c;
                    pos++;
                }
                break;
            case SEP:
                pos++;
                path[outpos++] = '/';
                state = SEEN_SEP;
                break;
            case SEEN_SEP:
                if (c == '/') {
                    // eat it
                    pos++;
                } else if (c == 0) {
                    done = true;
                } else {
                    state = FIELD_START;
                }
                break;
            case DOT:
                pos++; // consume the dot
                state = SEEN_DOT;
                break;
            case SEEN_DOT:
                if (c == '.') {
                    // dotdot now
                    state = DOTDOT;
                } else if (c == '/') {
                    // a field composed entirely of a .
                    // consume the / and move directly to the SEEN_SEP state
                    pos++;
                    state = SEEN_SEP;
                } else if (c == 0) {
                    done = true;
                } else {
                    // a field prefixed with a .
                    // emit a . and move directly into the IN_FIELD state
                    path[outpos++] = '.';
                    state = IN_FIELD;
                }
                break;
            case DOTDOT:
                pos++; // consume the dot
                state = SEEN_DOTDOT;
                break;
            case SEEN_DOTDOT:
                if (c == '/' || c == 0) {
                    // a field composed entirely of '..'
                    // search back and consume a field we've already emitted
                    if (outpos > 0) {
                        // we have already consumed at least one field
                        outpos--;

                        // walk backwards until we find the next field boundary
                        while (outpos > 0) {
                            if (path[outpos - 1] == '/') {
                                break;
                            }
                            outpos--;
                        }
                    }
                    if (c == 0) {
                        done = true;
                    } else {
                        pos++;
                        state = SEEN_SEP;
                    }
                } else {
                    // a field prefixed with ..
                    // emit the .. and move directly to the IN_FIELD state
                    path[outpos++] = '.';
                    path[outpos++] = '.';
                    state = IN_FIELD;
                }
                break;
        }
    }

    /* don't end with trailing slashes */
    if (outpos > 0 && path[outpos - 1] == '/') {
        outpos--;
    }

    path[outpos++] = 0;
}
