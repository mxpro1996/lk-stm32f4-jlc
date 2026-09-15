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
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define LOCAL_TRACE 0

/* The size of a directory entry with an empty name: everything up to the name
 * itself, which the struct declares at its maximum length. */
#define EXT2_DIRENT_HEADER_LEN offsetof(struct ext2_dir_entry_2, name)

/* Sanity bound on a directory scan, in blocks. */
#define EXT2_MAX_DIR_BLOCKS 1024

/* Map one block of a directory into the block cache, so entries can be read
 * without a bounce buffer: a block sized one does not fit on the stack (a
 * whole page at 4K block size) and the scan is too hot to allocate on. The
 * caller must ext2_put_block() what it gets back.
 *
 * Returns ERR_NOT_FOUND at the end of the directory. Directories are never
 * sparse, so a hole counts as the end.
 */
static status_t dir_get_block(ext2_t *ext2, struct ext2_inode *inode, uint fileblock,
                              void **ptr, blocknum_t *phys) {
    uint32_t block_size = EXT2_BLOCK_SIZE(ext2->sb);

    if ((off_t)((uint64_t)fileblock * block_size) >= ext2_file_len(ext2, inode)) {
        return ERR_NOT_FOUND;
    }

    blocknum_t bnum = ext2_file_block_to_fs_block(ext2, inode, fileblock);
    if (bnum == 0) {
        return ERR_NOT_FOUND;
    }

    int err = ext2_get_block(ext2, ptr, bnum);
    if (err < 0) {
        return err;
    }

    *phys = bnum;
    return NO_ERROR;
}

/* Step over one directory entry, returning its length. Zero means the rest of
 * the block is unusable, which for a well formed filesystem only happens on a
 * corrupt volume. */
static uint dirent_len(const struct ext2_dir_entry_2 *ent, uint pos, uint32_t block_size) {
    /* round up so the next entry stays aligned even if the volume does not
     * keep rec_len a multiple of four */
    uint len = ROUNDUP(LE16(ent->rec_len), 4);

    if (len < EXT2_DIRENT_HEADER_LEN || pos + len > block_size) {
        return 0;
    }
    return len;
}

/* Find one name in a directory, one component of a path resolved by the fs
 * layer's walk. */
static status_t ext2_lookup_locked(struct fs_vnode *dirvn, const char *name, struct fs_vnode **out) {
    ext2_vnode_t *dir = dirvn->priv;
    ext2_t *ext2 = dir->ext2;
    uint32_t block_size = EXT2_BLOCK_SIZE(ext2->sb);
    size_t namelen = strlen(name);

    LTRACEF("dir inode %u, name '%s'\n", dir->inum, name);

    if (!S_ISDIR(dir->inode.i_mode)) {
        return ERR_NOT_DIR;
    }
    if (namelen == 0 || namelen > EXT2_NAME_LEN) {
        return ERR_NOT_FOUND;
    }

    for (uint fileblock = 0; fileblock < EXT2_MAX_DIR_BLOCKS; fileblock++) {
        void *ptr;
        blocknum_t phys;
        status_t err = dir_get_block(ext2, &dir->inode, fileblock, &ptr, &phys);
        if (err < 0) {
            /* ERR_NOT_FOUND here is the end of the directory, which is also
             * the answer for a name that is not in it */
            return err;
        }

        inodenum_t inum = 0;
        uint pos = 0;
        while (pos + EXT2_DIRENT_HEADER_LEN <= block_size) {
            const struct ext2_dir_entry_2 *ent = (const void *)((const uint8_t *)ptr + pos);

            uint len = dirent_len(ent, pos, block_size);
            if (len == 0) {
                break;
            }

            if (ent->inode != 0 && ent->name_len == namelen &&
                pos + EXT2_DIRENT_HEADER_LEN + namelen <= block_size &&
                memcmp(name, ent->name, namelen) == 0) {
                inum = LE32(ent->inode);
                break;
            }

            pos += len;
        }

        ext2_put_block(ext2, phys);

        if (inum != 0) {
            LTRACEF("match: inode %u\n", inum);
            return ext2_create_vnode(ext2, inum, out);
        }
    }

    /* a directory this large is not one we are willing to scan */
    return ERR_TOO_BIG;
}

status_t ext2_lookup(struct fs_vnode *dirvn, const char *name, struct fs_vnode **out) {
    ext2_vnode_t *dir = dirvn->priv;

    mutex_acquire(&dir->ext2->lock);
    status_t err = ext2_lookup_locked(dirvn, name, out);
    mutex_release(&dir->ext2->lock);

    return err;
}

/* An open directory is just a byte offset into it; the entries are read
 * straight out of the block cache. */
typedef struct {
    ext2_vnode_t *dir;  // the layer holds a vnode reference for as long as this lives
    off_t offset;
} ext2_dircookie_t;

/* No lock: the inode is held by value, so nothing here reaches the block
 * cache. */
status_t ext2_opendir(struct fs_vnode *dirvn, dircookie **cookie) {
    ext2_vnode_t *dir = dirvn->priv;

    if (!S_ISDIR(dir->inode.i_mode)) {
        return ERR_NOT_DIR;
    }

    ext2_dircookie_t *dcookie = malloc(sizeof(ext2_dircookie_t));
    if (!dcookie) {
        return ERR_NO_MEMORY;
    }

    dcookie->dir = dir;
    dcookie->offset = 0;

    *cookie = (dircookie *)dcookie;
    return NO_ERROR;
}

/* "." and ".." are not reported: the layer resolves both lexically, and no
 * other filesystem in the tree lists them. */
static bool is_dot_entry(const char *name, uint namelen) {
    if (namelen == 1 && name[0] == '.') {
        return true;
    }
    if (namelen == 2 && name[0] == '.' && name[1] == '.') {
        return true;
    }
    return false;
}

static status_t ext2_readdir_locked(dircookie *cookie, struct dirent *ent_out) {
    ext2_dircookie_t *dcookie = (ext2_dircookie_t *)cookie;
    ext2_t *ext2 = dcookie->dir->ext2;
    uint32_t block_size = EXT2_BLOCK_SIZE(ext2->sb);

    for (;;) {
        uint fileblock = dcookie->offset / block_size;
        uint pos = dcookie->offset % block_size;

        void *ptr;
        blocknum_t phys;
        status_t err = dir_get_block(ext2, &dcookie->dir->inode, fileblock, &ptr, &phys);
        if (err < 0) {
            /* ERR_NOT_FOUND is the end of the directory, which is how the
             * layer expects readdir to report exhaustion */
            return err;
        }

        bool found = false;
        while (pos + EXT2_DIRENT_HEADER_LEN <= block_size) {
            const struct ext2_dir_entry_2 *ent = (const void *)((const uint8_t *)ptr + pos);

            uint len = dirent_len(ent, pos, block_size);
            if (len == 0) {
                break;
            }

            uint namelen = ent->name_len;
            if (pos + EXT2_DIRENT_HEADER_LEN + namelen > block_size) {
                break;
            }

            bool usable = (LE32(ent->inode) != 0) && !is_dot_entry(ent->name, namelen);
            if (usable) {
                /* copy the name out while the cache block is still held */
                size_t tocopy = MIN(namelen, sizeof(ent_out->name) - 1);
                memcpy(ent_out->name, ent->name, tocopy);
                ent_out->name[tocopy] = 0;
            }

            pos += len;

            if (usable) {
                found = true;
                break;
            }
        }

        if (!found) {
            /* nothing left in this block, resume at the next one */
            pos = block_size;
        }
        dcookie->offset = (off_t)((uint64_t)fileblock * block_size + pos);

        ext2_put_block(ext2, phys);

        if (found) {
            return NO_ERROR;
        }
    }
}

/* Holding the lock across the whole scan also makes the cursor advance
 * atomically, so two threads sharing one directory handle each see a distinct
 * entry rather than both reading from the same offset. */
status_t ext2_readdir(dircookie *cookie, struct dirent *ent_out) {
    ext2_dircookie_t *dcookie = (ext2_dircookie_t *)cookie;

    mutex_acquire(&dcookie->dir->ext2->lock);
    status_t err = ext2_readdir_locked(cookie, ent_out);
    mutex_release(&dcookie->dir->ext2->lock);

    return err;
}

status_t ext2_closedir(dircookie *cookie) {
    free(cookie);
    return NO_ERROR;
}
