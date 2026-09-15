/*
 * Copyright (c) 2024, Google Inc. All rights reserved.
 * Author: codycswong@google.com (Cody Wong)
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#pragma once

#include <dev/virtio/9p.h>
#include <kernel/mutex.h>
#include <lib/fs.h>
#include <lk/list.h>

// Fids are a server side resource, so they are recycled rather than handed out
// from an ever-increasing counter. One bit per fid, and fid 0 is usable: the
// protocol reserves only P9_FID_NOFID. A live object costs one fid, two once
// it has been read or enumerated, which puts the ceiling well above anything
// the layer's node cache and open handles can hold at once.
#define V9FS_MAX_FIDS 512
#define V9FS_FID_WORDS (V9FS_MAX_FIDS / 32)
#define V9FS_NO_FID    P9_FID_NOFID

typedef struct v9fs {
    struct virtio_device *dev;
    bdev_t *bdev;

    mutex_t lock;                     // fid allocator, and the open below

    uint32_t fid_map[V9FS_FID_WORDS]; // 1 = in use
    uint32_t fid_hint;                // where the next search starts
} v9fs_t;

/* Per object state, hung off fs_vnode::priv. The layer deduplicates vnodes by
 * id, so one of these backs every handle open on the object. */
typedef struct v9fs_vnode {
    /* The fid this object was walked to, which is never opened for I/O.
     * 9P2000 says a walk may not start from a fid that has been opened, so
     * keeping the walk origin and the I/O fid apart is what lets a directory
     * still be walked through after something has enumerated it. */
    uint32_t path_fid;

    /* Cloned off path_fid and opened on first use, with flags that follow the
     * vnode's type, and shared from then on: reads, writes and readdir all
     * carry an explicit offset, so one opened fid serves every handle.
     * V9FS_NO_FID until someone needs it. */
    uint32_t io_fid;
    uint32_t iounit;

    virtio_9p_qid_t qid;
} v9fs_vnode_t;

typedef struct v9fs_dircookie {
    v9fs_t *v9fs;
    uint32_t fid;      // the directory vnode's io fid; the vnode outlives us
    uint64_t offset;   // server side cursor, carried in every Treaddir

    uint32_t head;
    uint32_t tail;
    uint8_t data[PAGE_SIZE];
} v9fs_dircookie_t;

static inline v9fs_vnode_t *v9fs_vnode_of(struct fs_vnode *vn) {
    return (v9fs_vnode_t *)vn->priv;
}

/* The layer fills in the cookie of every vnode it adopts, so a vnode always
 * knows its mount. */
static inline v9fs_t *v9fs_of(struct fs_vnode *vn) {
    return (v9fs_t *)vn->cookie;
}

/* fid allocator */
uint32_t get_unused_fid(v9fs_t *v9fs);
/* Return a fid number the server never took (or has already clunked). */
void free_fid(v9fs_t *v9fs, uint32_t fid);
/* Clunk a fid on the server and return its number to the allocator. */
void put_fid(v9fs_t *v9fs, uint32_t fid);

/* One RPC, checking that the reply is of the expected type. A server that
 * refuses answers with Rlerror, whose Linux errno is the only explanation it
 * gives, so it is mapped to a status rather than flattened to one error. */
status_t v9fs_rpc(v9fs_t *v9fs, const virtio_9p_msg_t *tmsg,
                  virtio_9p_msg_t *rmsg, uint32_t expect) __NONNULL();

/* Did the server answer at all? A reply of any kind, Rlerror included, proves
 * it processed the request and therefore did whatever the request says it does
 * even on failure -- Tclunk and Tremove both release their fid that way. A
 * transport level failure leaves that unknown, and the caller of a zero
 * initialised rmsg can tell the two apart because no message type is zero.
 *
 * The distinction is what makes recycling fid numbers safe: a number may only
 * go back into the allocator once the server is known to be done with it. */
static inline bool v9fs_server_replied(const virtio_9p_msg_t *rmsg) {
    return rmsg->msg_type != 0;
}

/* Walk one name from `fid` into a freshly allocated fid, or clone `fid` when
 * `name` is NULL. On failure nothing is left allocated on either side. */
status_t v9fs_walk_fid(v9fs_t *v9fs, uint32_t fid, const char *name,
                       uint32_t *out_fid, virtio_9p_qid_t *out_qid) __NONNULL((1,4));

/* Wrap a fid the caller has walked to in a vnode for the layer. Takes
 * ownership of both fids: on failure they are clunked. */
status_t v9fs_vnode_create(v9fs_t *v9fs, uint32_t path_fid, uint32_t io_fid,
                           uint32_t iounit, const virtio_9p_qid_t *qid,
                           struct fs_vnode **out) __NONNULL((1,5,6));

/* Open the vnode's io fid if it is not open already. */
status_t v9fs_vnode_open(struct fs_vnode *vn) __NONNULL();

/* volume ops */
status_t v9fs_mount(bdev_t *dev, enum fs_mount_options options,
                    fscookie **cookie, struct fs_vnode **root);
status_t v9fs_unmount(fscookie *cookie);
void v9fs_release(struct fs_vnode *vn);

/* namespace ops */
status_t v9fs_lookup(struct fs_vnode *dir, const char *name, struct fs_vnode **out);
status_t v9fs_create(struct fs_vnode *dir, const char *name, uint64_t len,
                     struct fs_vnode **out);
status_t v9fs_mkdir(struct fs_vnode *dir, const char *name, struct fs_vnode **out);
status_t v9fs_unlink(struct fs_vnode *dir, const char *name, struct fs_vnode *child);
status_t v9fs_rmdir(struct fs_vnode *dir, const char *name, struct fs_vnode *child);

/* file ops */
ssize_t v9fs_read(struct fs_vnode *vn, void *buf, off_t offset, size_t len);
ssize_t v9fs_write(struct fs_vnode *vn, const void *buf, off_t offset, size_t len);
status_t v9fs_stat(struct fs_vnode *vn, struct file_stat *stat);

/* directory ops */
status_t v9fs_opendir(struct fs_vnode *vn, dircookie **dcookie);
status_t v9fs_readdir(dircookie *dcookie, struct dirent *ent);
status_t v9fs_closedir(dircookie *dcookie);
