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
#include <dev/virtio/9p.h>

#include <assert.h>
#include <kernel/mutex.h>
#include <lib/fs.h>
#include <lk/err.h>
#include <lk/init.h>
#include <lk/list.h>
#include <lk/trace.h>
#include <stdlib.h>
#include <string.h>

#include "v9fs_priv.h"

#define LOCAL_TRACE 0

// Linux errno values, which is what 9P2000.L carries in Rlerror. Only the ones
// the fs layer has a distinct answer for are named.
#define P9_EPERM        1
#define P9_ENOENT       2
#define P9_EIO          5
#define P9_EACCES      13
#define P9_EEXIST      17
#define P9_ENOTDIR     20
#define P9_EISDIR      21
#define P9_EINVAL      22
#define P9_ENFILE      23
#define P9_EMFILE      24
#define P9_ENOSPC      28
#define P9_EROFS       30
#define P9_ENAMETOOLONG 36
#define P9_ENOSYS      38
#define P9_ENOTEMPTY   39

// The server explains a refusal only through this errno, so it is worth
// translating: without it every refusal looks alike, and "the directory is not
// empty" and "the name is already taken" are answers the fs layer's callers
// act on differently.
static status_t p9_errno_to_status(uint32_t ecode) {
    switch (ecode) {
        case P9_EPERM:
        case P9_EACCES:        return ERR_ACCESS_DENIED;
        case P9_ENOENT:        return ERR_NOT_FOUND;
        case P9_EEXIST:        return ERR_ALREADY_EXISTS;
        case P9_ENOTDIR:       return ERR_NOT_DIR;
        case P9_EISDIR:        return ERR_NOT_FILE;
        case P9_EINVAL:        return ERR_INVALID_ARGS;
        case P9_ENFILE:
        case P9_EMFILE:
        case P9_ENOSPC:        return ERR_NO_RESOURCES;
        case P9_EROFS:
        case P9_ENOTEMPTY:     return ERR_NOT_ALLOWED;
        case P9_ENAMETOOLONG:  return ERR_BAD_PATH;
        case P9_ENOSYS:        return ERR_NOT_SUPPORTED;
        case P9_EIO:
        default:               return ERR_IO;
    }
}

status_t v9fs_rpc(v9fs_t *v9fs, const virtio_9p_msg_t *tmsg,
                  virtio_9p_msg_t *rmsg, uint32_t expect) {
    status_t err = virtio_9p_rpc(v9fs->dev, tmsg, rmsg);
    if (err != NO_ERROR) {
        return err;
    }

    if (rmsg->msg_type == expect) {
        return NO_ERROR;
    }

    if (rmsg->msg_type == P9_RLERROR) {
        LTRACEF("request %u refused: errno %u\n", tmsg->msg_type,
                rmsg->msg.rlerror.ecode);
        return p9_errno_to_status(rmsg->msg.rlerror.ecode);
    }

    TRACEF("request %u answered with unexpected type %u\n", tmsg->msg_type,
           rmsg->msg_type);
    return ERR_IO;
}

uint32_t get_unused_fid(v9fs_t *v9fs) {
    mutex_acquire(&v9fs->lock);

    // scan from the hint so a steady open/close cycle does not rescan the
    // used prefix on every allocation
    for (uint32_t i = 0; i < V9FS_MAX_FIDS; i++) {
        uint32_t fid = (v9fs->fid_hint + i) % V9FS_MAX_FIDS;
        uint32_t *word = &v9fs->fid_map[fid / 32];
        uint32_t bit = 1u << (fid % 32);

        if (!(*word & bit)) {
            *word |= bit;
            v9fs->fid_hint = (fid + 1) % V9FS_MAX_FIDS;
            mutex_release(&v9fs->lock);
            return fid;
        }
    }

    mutex_release(&v9fs->lock);
    return V9FS_NO_FID;
}

void free_fid(v9fs_t *v9fs, uint32_t fid) {
    if (fid == V9FS_NO_FID) {
        return;
    }

    DEBUG_ASSERT(fid < V9FS_MAX_FIDS);

    mutex_acquire(&v9fs->lock);
    DEBUG_ASSERT(v9fs->fid_map[fid / 32] & (1u << (fid % 32)));
    v9fs->fid_map[fid / 32] &= ~(1u << (fid % 32));
    mutex_release(&v9fs->lock);
}

void put_fid(v9fs_t *v9fs, uint32_t fid) {
    if (fid == V9FS_NO_FID) {
        return;
    }

    virtio_9p_msg_t tclunk = {
        .msg_type = P9_TCLUNK,
        .tag = P9_TAG_DEFAULT,
        .msg.tclunk = {
            .fid = fid,
        }};
    virtio_9p_msg_t rclunk = {};

    // a clunk that the server refuses is worth knowing about but is not worth
    // taking the system down for: the caller is usually tearing something down
    // and has nothing better to do with the failure
    status_t err = v9fs_rpc(v9fs, &tclunk, &rclunk, P9_RCLUNK);
    if (err != NO_ERROR) {
        TRACEF("clunk of fid %u failed: %d\n", fid, err);
    }

    const bool replied = v9fs_server_replied(&rclunk);

    virtio_9p_msg_destroy(&rclunk);

    if (replied) {
        // the server released the fid, whatever it thought of the request
        free_fid(v9fs, fid);
    } else {
        // the request may or may not have reached the server, so the number
        // has to be abandoned: handing it out again could collide with a fid
        // that is still live over there
        TRACEF("clunk of fid %u went unanswered, leaking the number\n", fid);
    }
}

status_t v9fs_walk_fid(v9fs_t *v9fs, uint32_t fid, const char *name,
                       uint32_t *out_fid, virtio_9p_qid_t *out_qid) {
    uint32_t newfid = get_unused_fid(v9fs);
    if (newfid == V9FS_NO_FID) {
        return ERR_NO_RESOURCES;
    }

    virtio_9p_msg_t twalk = {
        .msg_type = P9_TWALK,
        .tag = P9_TAG_DEFAULT,
        .msg.twalk = {
            .fid = fid,
            .newfid = newfid,
            .nwname = name ? 1 : 0,
            .wname = {name},
        }};
    virtio_9p_msg_t rwalk = {};

    status_t err = v9fs_rpc(v9fs, &twalk, &rwalk, P9_RWALK);
    if (err == NO_ERROR && rwalk.msg.rwalk.nwqid != twalk.msg.twalk.nwname) {
        // a short walk means the name did not resolve; the server keeps the
        // new fid only when every element was walked
        err = ERR_NOT_FOUND;
    }

    if (err != NO_ERROR) {
        const bool replied = v9fs_server_replied(&rwalk);
        virtio_9p_msg_destroy(&rwalk);
        if (replied) {
            // a walk the server answered and did not complete leaves the new
            // fid untaken, so the number goes straight back
            free_fid(v9fs, newfid);
        }
        return err;
    }

    if (out_qid && name) {
        *out_qid = rwalk.msg.rwalk.qid[0];
    }
    *out_fid = newfid;

    virtio_9p_msg_destroy(&rwalk);
    return NO_ERROR;
}

status_t v9fs_vnode_create(v9fs_t *v9fs, uint32_t path_fid, uint32_t io_fid,
                           uint32_t iounit, const virtio_9p_qid_t *qid,
                           struct fs_vnode **out) {
    v9fs_vnode_t *v = malloc(sizeof(v9fs_vnode_t));
    if (!v) {
        put_fid(v9fs, io_fid);
        put_fid(v9fs, path_fid);
        return ERR_NO_MEMORY;
    }

    v->path_fid = path_fid;
    v->io_fid = io_fid;
    v->iounit = iounit;
    v->qid = *qid;

    // The qid path is the server's identity for the object, which is what the
    // layer deduplicates on. It is biased by one because an id of zero means
    // "no identity" to the layer, and the whole 64-bit range is the server's
    // to choose from; that moves the one value this cannot represent from a
    // plausible id to an implausible one.
    enum fs_vnode_type type =
        (qid->type & P9_QTDIR) ? FS_VNODE_DIR : FS_VNODE_FILE;

    status_t err = fs_vnode_create(qid->path + 1, type, v, out);
    if (err < 0) {
        free(v);
        put_fid(v9fs, io_fid);
        put_fid(v9fs, path_fid);
        return err;
    }

    return NO_ERROR;
}

status_t v9fs_vnode_open(struct fs_vnode *vn) {
    v9fs_t *v9fs = v9fs_of(vn);
    v9fs_vnode_t *v = v9fs_vnode_of(vn);

    mutex_acquire(&v9fs->lock);
    if (v->io_fid != V9FS_NO_FID) {
        mutex_release(&v9fs->lock);
        return NO_ERROR;
    }
    mutex_release(&v9fs->lock);

    // clone the walk fid rather than opening it: opening it would make the
    // object unusable as the origin of any later walk
    uint32_t io_fid;
    status_t err = v9fs_walk_fid(v9fs, v->path_fid, NULL, &io_fid, NULL);
    if (err < 0) {
        return err;
    }

    virtio_9p_msg_t tlopen = {
        .msg_type = P9_TLOPEN,
        .tag = P9_TAG_DEFAULT,
        .msg.tlopen = {
            .fid = io_fid,
            // a directory is only ever enumerated; everything else is opened
            // for both, since the layer has no notion of an access mode
            .flags = (vn->type == FS_VNODE_DIR) ? (O_DIRECTORY | O_RDONLY)
                                                : O_RDWR,
        }};
    virtio_9p_msg_t rlopen = {};

    err = v9fs_rpc(v9fs, &tlopen, &rlopen, P9_RLOPEN);
    if (err < 0) {
        virtio_9p_msg_destroy(&rlopen);
        put_fid(v9fs, io_fid);
        return err;
    }

    mutex_acquire(&v9fs->lock);
    if (v->io_fid == V9FS_NO_FID) {
        v->io_fid = io_fid;
        v->iounit = rlopen.msg.rlopen.iounit;
        io_fid = V9FS_NO_FID;
    }
    mutex_release(&v9fs->lock);

    virtio_9p_msg_destroy(&rlopen);

    // another thread got there first while the open was in flight
    put_fid(v9fs, io_fid);

    return NO_ERROR;
}

void v9fs_release(struct fs_vnode *vn) {
    v9fs_t *v9fs = v9fs_of(vn);
    v9fs_vnode_t *v = v9fs_vnode_of(vn);

    put_fid(v9fs, v->io_fid);
    put_fid(v9fs, v->path_fid);
    free(v);
}

status_t v9fs_mount(bdev_t *dev, enum fs_mount_options options,
                    fscookie **cookie, struct fs_vnode **root) {
    status_t ret;

    LTRACEF("bdev (%p) cookie (%p)\n", dev, cookie);

    if (!dev) {
        return ERR_INVALID_ARGS;
    }
    if (options != FS_MOUNT_OPTION_NONE) {
        return ERR_INVALID_ARGS;
    }

    v9fs_t *v9fs = calloc(1, sizeof(v9fs_t));
    if (!v9fs) {
        return ERR_NO_MEMORY;
    }

    v9fs->dev = virtio_9p_bdev_to_virtio_device(dev);
    v9fs->bdev = dev;
    mutex_init(&v9fs->lock);

    uint32_t root_fid = get_unused_fid(v9fs);
    if (root_fid == V9FS_NO_FID) {
        // unreachable on a fresh allocator, but the attach below would
        // otherwise be sent with P9_FID_NOFID as its fid
        ret = ERR_NO_RESOURCES;
        goto err;
    }

    LTRACEF("root fid: %u\n", root_fid);

    virtio_9p_msg_t tatt = {
        .msg_type = P9_TATTACH,
        .tag = P9_TAG_DEFAULT,
        .msg.tattach = {
            .fid = root_fid,
            .afid = P9_FID_NOFID,
            .uname = "root",
            .aname = V9P_MOUNT_ANAME,
            .n_uname = P9_UNAME_NONUNAME,
        }};
    virtio_9p_msg_t ratt = {};

    ret = v9fs_rpc(v9fs, &tatt, &ratt, P9_RATTACH);
    if (ret != NO_ERROR) {
        virtio_9p_msg_destroy(&ratt);
        free_fid(v9fs, root_fid);
        goto err;
    }

    virtio_9p_qid_t root_qid = ratt.msg.rattach.qid;
    virtio_9p_msg_destroy(&ratt);

    // the attach fid is the root's walk fid, and clunking it is what detaches
    // the mount. building the vnode is the last thing that can fail, so an
    // error below this point cannot leave one behind that nothing owns.
    ret = v9fs_vnode_create(v9fs, root_fid, V9FS_NO_FID, 0, &root_qid, root);
    if (ret != NO_ERROR) {
        goto err;
    }

    *cookie = (fscookie *)v9fs;
    return NO_ERROR;

err:
    LTRACEF("mount 9p dev (%s) failed: %d\n", dev->name, ret);
    free(v9fs);
    return ret;
}

status_t v9fs_unmount(fscookie *cookie) {
    v9fs_t *v9fs = (v9fs_t *)cookie;

    LTRACEF("v9fs (%p)\n", v9fs);

    // the layer releases the root vnode before calling this, and refuses to
    // unmount at all while a handle is open, so every fid this mount held --
    // the attach fid included -- has been clunked by v9fs_release() already
    free(v9fs);

    return NO_ERROR;
}

static const struct fs_api v9fs_api = {
    .format = NULL,
    .mount = v9fs_mount,
    .unmount = v9fs_unmount,
    // the transport has no Tstatfs handler
    .fs_stat = NULL,

    .lookup = v9fs_lookup,
    .create = v9fs_create,
    .mkdir = v9fs_mkdir,
    .unlink = v9fs_unlink,
    .rmdir = v9fs_rmdir,
    // no Trename or Treadlink in the transport either, so a symlink on the
    // host is reported as a plain file rather than followed
    .rename = NULL,
    .readlink = NULL,
    .release = v9fs_release,

    .read = v9fs_read,
    .write = v9fs_write,
    // truncating needs Tsetattr, which the transport does not implement
    .truncate = NULL,
    .stat = v9fs_stat,
    .ioctl = NULL,

    .opendir = v9fs_opendir,
    .readdir = v9fs_readdir,
    .closedir = v9fs_closedir,
};

STATIC_FS_IMPL(9p, &v9fs_api);
