/*
 * Copyright (c) 2007 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

#include "ext2_priv.h"
#include <kernel/mutex.h>
#include <lk/debug.h>
#include <lk/err.h>
#include <lk/trace.h>
#include <stdlib.h>
#include <string.h>

#define LOCAL_TRACE 0

off_t ext2_file_len(ext2_t *ext2, struct ext2_inode *inode) {
    /* calculate the file size */
    off_t len = inode->i_size;
    if ((ext2->sb.s_feature_ro_compat & EXT2_FEATURE_RO_COMPAT_LARGE_FILE) && (S_ISREG(inode->i_mode))) {
        /* can potentially be a large file */
        len |= (off_t)inode->i_size_high << 32;
    }

    return len;
}

ssize_t ext2_read(struct fs_vnode *vn, void *buf, off_t offset, size_t len) {
    ext2_vnode_t *v = vn->priv;

    /* only regular files have contents that can be read this way */
    if (!S_ISREG(v->inode.i_mode)) {
        return ERR_NOT_FILE;
    }

    /* negative offsets are invalid */
    if (offset < 0) {
        return ERR_INVALID_ARGS;
    }

    mutex_acquire(&v->ext2->lock);
    ssize_t ret = ext2_read_inode(v->ext2, &v->inode, buf, offset, len);
    mutex_release(&v->ext2->lock);

    return ret;
}

/* No lock: the inode is held by value and the filesystem is read-only, so
 * nothing here reaches the block cache. */
status_t ext2_stat(struct fs_vnode *vn, struct file_stat *stat) {
    ext2_vnode_t *v = vn->priv;

    stat->size = ext2_file_len(v->ext2, &v->inode);
    stat->is_dir = S_ISDIR(v->inode.i_mode);

    return NO_ERROR;
}

/* Read a symlink target. Short targets live inline in the inode's block
 * pointer array, longer ones in the file's data blocks -- and only that longer
 * case reaches the block cache, so it is the only one that takes the lock. */
ssize_t ext2_readlink(struct fs_vnode *vn, char *buf, size_t len) {
    ext2_vnode_t *v = vn->priv;

    LTRACEF("inode %u, buf %p, len %zu\n", v->inum, buf, len);

    if (!S_ISLNK(v->inode.i_mode)) {
        return ERR_NOT_FILE;
    }

    off_t linklen = ext2_file_len(v->ext2, &v->inode);
    if (linklen < 0) {
        return ERR_NOT_VALID;
    }
    if (linklen + 1 > (off_t)len) {
        return ERR_NOT_ENOUGH_BUFFER;
    }

    if (linklen > (off_t)sizeof(v->inode.i_block)) {
        mutex_acquire(&v->ext2->lock);
        ssize_t err = ext2_read_inode(v->ext2, &v->inode, buf, 0, linklen);
        mutex_release(&v->ext2->lock);
        if (err < 0) {
            return err;
        }
        if (err != linklen) {
            return ERR_IO;
        }
    } else {
        memcpy(buf, &v->inode.i_block[0], linklen);
    }
    buf[linklen] = 0;

    LTRACEF("read link '%s'\n", buf);

    return linklen;
}
