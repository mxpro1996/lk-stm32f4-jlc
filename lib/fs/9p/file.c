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

#include <lk/err.h>
#include <lk/trace.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v9fs_priv.h"

#define LOCAL_TRACE 0

// The server reports at open time the largest transfer it will handle in one
// message on this fid; zero means it has no preference beyond the negotiated
// msize, and PAGE_SIZE is what this driver asked for before it paid attention
// to the field. Transfers are chunked either way, so a small value only costs
// more round trips.
static size_t io_chunk(const v9fs_vnode_t *v) {
    return v->iounit ? v->iounit : PAGE_SIZE;
}

status_t v9fs_create(struct fs_vnode *dir, const char *name, uint64_t len,
                     struct fs_vnode **out) {
    v9fs_t *v9fs = v9fs_of(dir);
    v9fs_vnode_t *dv = v9fs_vnode_of(dir);
    status_t err;

    LTRACEF("dir (%p) name (%s) len (%llu)\n", dir, name, len);

    // Tlcreate turns the fid it is handed into the fid of the new file, so it
    // gets a clone of the directory's walk fid rather than the fid itself
    uint32_t io_fid;
    err = v9fs_walk_fid(v9fs, dv->path_fid, NULL, &io_fid, NULL);
    if (err < 0) {
        return err;
    }

    virtio_9p_msg_t tlcreate = {
        .msg_type = P9_TLCREATE,
        .tag = P9_TAG_DEFAULT,
        .msg.tlcreate = {
            .fid = io_fid,
            // exclusive, so an existing name is refused rather than truncated,
            // which is what every other filesystem here does. the requested
            // length is ignored: sizing the file up front would need Tsetattr,
            // which the transport does not implement
            .flags = O_RDWR | O_CREAT | O_EXCL,
            .mode = S_IRUSR | S_IWUSR |
                    S_IRGRP | S_IWGRP |
                    S_IROTH | S_IWOTH,
            .name = name,
        }};
    virtio_9p_msg_t rlcreate = {};

    err = v9fs_rpc(v9fs, &tlcreate, &rlcreate, P9_RLCREATE);
    if (err < 0) {
        virtio_9p_msg_destroy(&rlcreate);
        put_fid(v9fs, io_fid);
        return err;
    }

    virtio_9p_qid_t qid = rlcreate.msg.rlcreate.qid;
    uint32_t iounit = rlcreate.msg.rlcreate.iounit;
    virtio_9p_msg_destroy(&rlcreate);

    // the create fid is open, so it cannot double as the vnode's walk fid
    uint32_t path_fid;
    err = v9fs_walk_fid(v9fs, dv->path_fid, name, &path_fid, NULL);
    if (err < 0) {
        // the file exists on the server, but there is no way to hand it back
        put_fid(v9fs, io_fid);
        return err;
    }

    return v9fs_vnode_create(v9fs, path_fid, io_fid, iounit, &qid, out);
}

ssize_t v9fs_read(struct fs_vnode *vn, void *buf, off_t offset, size_t len) {
    v9fs_t *v9fs = v9fs_of(vn);
    ssize_t rlen = 0;
    status_t err;

    LTRACEF("vn (%p) buf (%p) offset (%lld) len (%zu)\n", vn, buf, offset, len);

    if (vn->type == FS_VNODE_DIR) {
        return ERR_NOT_FILE;
    }

    if ((err = v9fs_vnode_open(vn)) < 0) {
        return err;
    }

    const v9fs_vnode_t *v = v9fs_vnode_of(vn);
    uint32_t fid = v->io_fid;
    const size_t chunk = io_chunk(v);

    while (len > 0) {
        virtio_9p_msg_t tread = {
            .msg_type = P9_TREAD,
            .tag = P9_TAG_DEFAULT,
            .msg.tread = {
                .fid = fid, .offset = offset, .count = MIN(len, chunk)}};
        virtio_9p_msg_t rread = {};

        err = v9fs_rpc(v9fs, &tread, &rread, P9_RREAD);
        if (err < 0) {
            virtio_9p_msg_destroy(&rread);
            break;
        }

        uint32_t readcount = rread.msg.rread.count;
        memcpy(&((uint8_t *)buf)[rlen], rread.msg.rread.data, readcount);

        offset += readcount;
        rlen += readcount;
        len -= readcount;

        virtio_9p_msg_destroy(&rread);

        // short read means end of file
        if (readcount == 0) {
            break;
        }
    }

    return err == NO_ERROR ? rlen : err;
}

ssize_t v9fs_write(struct fs_vnode *vn, const void *buf, off_t offset,
                   size_t len) {
    v9fs_t *v9fs = v9fs_of(vn);
    const uint8_t *cpos = buf;
    ssize_t wlen = 0;
    status_t err;

    LTRACEF("vn (%p) buf (%p) offset (%lld) len (%zu)\n", vn, buf, offset, len);

    if (vn->type == FS_VNODE_DIR) {
        return ERR_NOT_FILE;
    }

    if ((err = v9fs_vnode_open(vn)) < 0) {
        return err;
    }

    const v9fs_vnode_t *v = v9fs_vnode_of(vn);
    uint32_t fid = v->io_fid;
    const size_t chunk = io_chunk(v);

    while (len > 0) {
        virtio_9p_msg_t twrite = {
            .msg_type = P9_TWRITE,
            .tag = P9_TAG_DEFAULT,
            .msg.twrite = {
                .fid = fid, .offset = offset, .count = MIN(len, chunk),
                .data = cpos}};
        virtio_9p_msg_t rwrite = {};

        err = v9fs_rpc(v9fs, &twrite, &rwrite, P9_RWRITE);
        if (err < 0) {
            virtio_9p_msg_destroy(&rwrite);
            break;
        }

        uint32_t writecount = rwrite.msg.rwrite.count;

        offset += writecount;
        cpos += writecount;
        wlen += writecount;
        len -= writecount;

        virtio_9p_msg_destroy(&rwrite);

        // a server that accepts nothing would otherwise spin
        if (writecount == 0) {
            break;
        }
    }

    return err == NO_ERROR ? wlen : err;
}

status_t v9fs_stat(struct fs_vnode *vn, struct file_stat *stat) {
    v9fs_t *v9fs = v9fs_of(vn);
    v9fs_vnode_t *v = v9fs_vnode_of(vn);
    status_t err;

    LTRACEF("vn (%p) stat (%p)\n", vn, stat);

    // the walk fid answers this without having to be opened, which keeps stat
    // working on something the caller has no right to open for writing
    virtio_9p_msg_t tgatt = {
        .msg_type = P9_TGETATTR,
        .tag = P9_TAG_DEFAULT,
        .msg.tgetattr = {
            .fid = v->path_fid,
            .request_mask = P9_GETATTR_BASIC,
        }};
    virtio_9p_msg_t rgatt = {};

    err = v9fs_rpc(v9fs, &tgatt, &rgatt, P9_RGETATTR);
    if (err == NO_ERROR) {
        stat->size = rgatt.msg.rgetattr.size;
        stat->capacity = rgatt.msg.rgetattr.blocks * 512;
        stat->is_dir = S_ISDIR(rgatt.msg.rgetattr.mode);
    }

    virtio_9p_msg_destroy(&rgatt);

    return err;
}
