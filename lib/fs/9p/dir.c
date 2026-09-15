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

status_t v9fs_lookup(struct fs_vnode *dir, const char *name,
                     struct fs_vnode **out) {
    v9fs_t *v9fs = v9fs_of(dir);
    v9fs_vnode_t *dv = v9fs_vnode_of(dir);

    LTRACEF("dir (%p) name (%s)\n", dir, name);

    uint32_t fid;
    virtio_9p_qid_t qid;
    status_t err = v9fs_walk_fid(v9fs, dv->path_fid, name, &fid, &qid);
    if (err < 0) {
        return err;
    }

    // the fid is not opened here: most walks are through a directory on the
    // way to something else, and those never need one
    return v9fs_vnode_create(v9fs, fid, V9FS_NO_FID, 0, &qid, out);
}

status_t v9fs_mkdir(struct fs_vnode *dir, const char *name,
                    struct fs_vnode **out) {
    v9fs_t *v9fs = v9fs_of(dir);
    v9fs_vnode_t *dv = v9fs_vnode_of(dir);

    LTRACEF("dir (%p) name (%s)\n", dir, name);

    virtio_9p_msg_t tmkdir = {
        .msg_type = P9_TMKDIR,
        .tag = P9_TAG_DEFAULT,
        .msg.tmkdir = {
            .dfid = dv->path_fid,
            .name = name,
            .mode = S_IFDIR | S_IRWXU |
                    S_IRGRP | S_IWGRP |
                    S_IROTH | S_IWOTH,
        }};
    virtio_9p_msg_t rmkdir = {};

    status_t err = v9fs_rpc(v9fs, &tmkdir, &rmkdir, P9_RMKDIR);
    virtio_9p_msg_destroy(&rmkdir);
    if (err < 0) {
        return err;
    }

    // Tmkdir leaves the directory fid alone and reports only a qid, so the new
    // directory still has to be walked to before it can be handed back
    return v9fs_lookup(dir, name, out);
}

// Tremove both removes the file and clunks the fid it is given, whether or not
// the removal succeeds. It therefore gets a clone: a refusal (a directory that
// is not empty, say) has to leave the child's own fids intact, because the
// vnode outlives the attempt.
static status_t remove_entry(struct fs_vnode *dir, struct fs_vnode *child) {
    v9fs_t *v9fs = v9fs_of(dir);
    v9fs_vnode_t *cv = v9fs_vnode_of(child);

    uint32_t fid;
    status_t err = v9fs_walk_fid(v9fs, cv->path_fid, NULL, &fid, NULL);
    if (err < 0) {
        return err;
    }

    virtio_9p_msg_t tremove = {
        .msg_type = P9_TREMOVE,
        .tag = P9_TAG_DEFAULT,
        .msg.tremove = {
            .fid = fid,
        }};
    virtio_9p_msg_t rremove = {};

    err = v9fs_rpc(v9fs, &tremove, &rremove, P9_RREMOVE);

    const bool replied = v9fs_server_replied(&rremove);
    virtio_9p_msg_destroy(&rremove);

    if (replied) {
        // a server that answered has clunked the fid whether or not it removed
        // anything, so only the number comes back
        free_fid(v9fs, fid);
    }

    return err;
}

status_t v9fs_unlink(struct fs_vnode *dir, const char *name,
                     struct fs_vnode *child) {
    LTRACEF("dir (%p) name (%s)\n", dir, name);
    return remove_entry(dir, child);
}

status_t v9fs_rmdir(struct fs_vnode *dir, const char *name,
                    struct fs_vnode *child) {
    LTRACEF("dir (%p) name (%s)\n", dir, name);
    return remove_entry(dir, child);
}

status_t v9fs_opendir(struct fs_vnode *vn, dircookie **dcookie) {
    LTRACEF("vn (%p)\n", vn);

    if (vn->type != FS_VNODE_DIR) {
        return ERR_NOT_DIR;
    }

    status_t err = v9fs_vnode_open(vn);
    if (err < 0) {
        return err;
    }

    v9fs_dircookie_t *dir = calloc(1, sizeof(v9fs_dircookie_t));
    if (!dir) {
        return ERR_NO_MEMORY;
    }

    dir->v9fs = v9fs_of(vn);
    // Treaddir carries its own offset, so every cursor on this directory can
    // share the one opened fid the vnode holds
    dir->fid = v9fs_vnode_of(vn)->io_fid;

    *dcookie = (dircookie *)dir;

    return NO_ERROR;
}

status_t v9fs_readdir(dircookie *dcookie, struct dirent *ent) {
    v9fs_dircookie_t *dir = (v9fs_dircookie_t *)dcookie;
    p9_dirent_t p9_dent;
    status_t err;

    LTRACEF("dir (%p) ent (%p)\n", dir, ent);

    while (true) {
        // refill from the server when the buffer runs dry
        if (dir->head == dir->tail) {
            virtio_9p_msg_t treaddir = {
                .msg_type = P9_TREADDIR,
                .tag = P9_TAG_DEFAULT,
                .msg.treaddir = {
                    .fid = dir->fid,
                    .offset = dir->offset,
                    .count = PAGE_SIZE,
                }};
            virtio_9p_msg_t rreaddir = {};

            err = v9fs_rpc(dir->v9fs, &treaddir, &rreaddir, P9_RREADDIR);
            if (err < 0) {
                virtio_9p_msg_destroy(&rreaddir);
                return err;
            }

            dir->head = 0;
            dir->tail = MIN(rreaddir.msg.rreaddir.count, PAGE_SIZE);
            memcpy(dir->data, rreaddir.msg.rreaddir.data, dir->tail);
            LTRACEF("head (%u) tail (%u)\n", dir->head, dir->tail);

            virtio_9p_msg_destroy(&rreaddir);

            if (dir->tail == 0) {
                return ERR_NOT_FOUND; // end of directory
            }
        }

        while (dir->head < dir->tail) {
            ssize_t sread = p9_dirent_read(dir->data + dir->head,
                                           dir->tail - dir->head, &p9_dent);
            if (sread < NO_ERROR) {
                return sread; // malformed entry
            }

            LTRACEF("head (%u) tail (%u) sread (%zd) offset (%llu) name (%s)\n",
                    dir->head, dir->tail, sread, p9_dent.offset, p9_dent.name);

            dir->head += sread;
            // the cursor advances even for an entry that is not reported, or a
            // refill would start over from before it
            dir->offset = p9_dent.offset;

            // the fs layer flattens "." and ".." lexically and no filesystem
            // here lists them, so the host's are dropped
            bool dot = !strcmp(p9_dent.name, ".") || !strcmp(p9_dent.name, "..");
            if (!dot) {
                strlcpy(ent->name, p9_dent.name, FS_MAX_FILE_LEN);
            }

            p9_dirent_destroy(&p9_dent);

            if (!dot) {
                return NO_ERROR;
            }
        }
    }
}

status_t v9fs_closedir(dircookie *dcookie) {
    v9fs_dircookie_t *dir = (v9fs_dircookie_t *)dcookie;

    LTRACEF("dir (%p)\n", dir);

    // the fid belongs to the directory's vnode, which the layer holds for as
    // long as this handle is open
    free(dir);

    return NO_ERROR;
}
