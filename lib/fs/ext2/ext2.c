/*
 * Copyright (c) 2007-2015 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

#include "ext2_priv.h"
#include <assert.h>
#include <kernel/mutex.h>
#include <lib/fs.h>
#include <lk/debug.h>
#include <lk/err.h>
#include <lk/init.h>
#include <lk/trace.h>
#include <stdlib.h>
#include <string.h>

#define LOCAL_TRACE 0

static void endian_swap_superblock(struct ext2_super_block *sb) {
    LE32SWAP(sb->s_inodes_count);
    LE32SWAP(sb->s_blocks_count);
    LE32SWAP(sb->s_r_blocks_count);
    LE32SWAP(sb->s_free_blocks_count);
    LE32SWAP(sb->s_free_inodes_count);
    LE32SWAP(sb->s_first_data_block);
    LE32SWAP(sb->s_log_block_size);
    LE32SWAP(sb->s_log_frag_size);
    LE32SWAP(sb->s_blocks_per_group);
    LE32SWAP(sb->s_frags_per_group);
    LE32SWAP(sb->s_inodes_per_group);
    LE32SWAP(sb->s_mtime);
    LE32SWAP(sb->s_wtime);
    LE16SWAP(sb->s_mnt_count);
    LE16SWAP(sb->s_max_mnt_count);
    LE16SWAP(sb->s_magic);
    LE16SWAP(sb->s_state);
    LE16SWAP(sb->s_errors);
    LE16SWAP(sb->s_minor_rev_level);
    LE32SWAP(sb->s_lastcheck);
    LE32SWAP(sb->s_checkinterval);
    LE32SWAP(sb->s_creator_os);
    LE32SWAP(sb->s_rev_level);
    LE16SWAP(sb->s_def_resuid);
    LE16SWAP(sb->s_def_resgid);
    LE32SWAP(sb->s_first_ino);
    LE16SWAP(sb->s_inode_size);
    LE16SWAP(sb->s_block_group_nr);
    LE32SWAP(sb->s_feature_compat);
    LE32SWAP(sb->s_feature_incompat);
    LE32SWAP(sb->s_feature_ro_compat);
    LE32SWAP(sb->s_algorithm_usage_bitmap);

    /* ext3 journal stuff */
    LE32SWAP(sb->s_journal_inum);
    LE32SWAP(sb->s_journal_dev);
    LE32SWAP(sb->s_last_orphan);
    LE32SWAP(sb->s_default_mount_opts);
    LE32SWAP(sb->s_first_meta_bg);
}

static void endian_swap_inode(struct ext2_inode *inode) {
    LE16SWAP(inode->i_mode);
    LE16SWAP(inode->i_uid_low);
    LE32SWAP(inode->i_size);
    LE32SWAP(inode->i_atime);
    LE32SWAP(inode->i_ctime);
    LE32SWAP(inode->i_mtime);
    LE32SWAP(inode->i_dtime);
    LE16SWAP(inode->i_gid_low);
    LE16SWAP(inode->i_links_count);
    LE32SWAP(inode->i_blocks);
    LE32SWAP(inode->i_flags);

    // leave block pointers/symlink data alone

    LE32SWAP(inode->i_generation);
    LE32SWAP(inode->i_file_acl);
    LE32SWAP(inode->i_dir_acl);
    LE32SWAP(inode->i_faddr);

    LE16SWAP(inode->i_uid_high);
    LE16SWAP(inode->i_gid_high);
}

static void endian_swap_group_desc(struct ext2_group_desc *gd) {
    LE32SWAP(gd->bg_block_bitmap);
    LE32SWAP(gd->bg_inode_bitmap);
    LE32SWAP(gd->bg_inode_table);
    LE16SWAP(gd->bg_free_blocks_count);
    LE16SWAP(gd->bg_free_inodes_count);
    LE16SWAP(gd->bg_used_dirs_count);
}

status_t ext2_mount(bdev_t *dev, enum fs_mount_options options, fscookie **cookie,
                    struct fs_vnode **root) {
    /* the filesystem is intrinsically read-only, so that option is always satisfied */
    if ((options & ~FS_MOUNT_OPTION_READ_ONLY) != 0) {
        return ERR_INVALID_ARGS;
    }
    int err;

    LTRACEF("dev %p\n", dev);

    if (!dev) {
        return ERR_NOT_FOUND;
    }

    ext2_t *ext2 = malloc(sizeof(ext2_t));
    if (!ext2) {
        return ERR_NO_MEMORY;
    }
    ext2->dev = dev;
    ext2->gd = NULL;
    ext2->cache = NULL;
    mutex_init(&ext2->lock);

    err = bio_read(dev, &ext2->sb, 1024, sizeof(struct ext2_super_block));
    if (err < 0) {
        goto err;
    }

    endian_swap_superblock(&ext2->sb);

    /* see if the superblock is good */
    if (ext2->sb.s_magic != EXT2_SUPER_MAGIC) {
        err = ERR_NOT_VALID;
        goto err;
    }

    /* calculate group count, rounded up */
    ext2->s_group_count = (ext2->sb.s_blocks_count + ext2->sb.s_blocks_per_group - 1) / ext2->sb.s_blocks_per_group;

    /* print some info */
    LTRACEF("rev level %d\n", ext2->sb.s_rev_level);
    LTRACEF("compat features 0x%x\n", ext2->sb.s_feature_compat);
    LTRACEF("incompat features 0x%x\n", ext2->sb.s_feature_incompat);
    LTRACEF("ro compat features 0x%x\n", ext2->sb.s_feature_ro_compat);
    LTRACEF("block size %d\n", EXT2_BLOCK_SIZE(ext2->sb));
    LTRACEF("inode size %d\n", EXT2_INODE_SIZE(ext2->sb));
    LTRACEF("block count %d\n", ext2->sb.s_blocks_count);
    LTRACEF("blocks per group %d\n", ext2->sb.s_blocks_per_group);
    LTRACEF("group count %d\n", ext2->s_group_count);
    LTRACEF("inodes per group %d\n", ext2->sb.s_inodes_per_group);

    /* we only support dynamic revs */
    if (ext2->sb.s_rev_level > EXT2_DYNAMIC_REV) {
        err = ERR_NOT_SUPPORTED;
        goto err;
    }

    /* make sure it doesn't have any ro features we don't support */
    if (ext2->sb.s_feature_ro_compat & ~(EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER | EXT2_FEATURE_RO_COMPAT_LARGE_FILE)) {
        err = ERR_NOT_SUPPORTED;
        goto err;
    }

    /* read in all the group descriptors */
    ext2->gd = malloc(sizeof(struct ext2_group_desc) * ext2->s_group_count);
    if (!ext2->gd) {
        err = ERR_NO_MEMORY;
        goto err;
    }
    err = bio_read(ext2->dev, (void *)ext2->gd,
                   (EXT2_BLOCK_SIZE(ext2->sb) == 4096) ? 4096 : 2048,
                   sizeof(struct ext2_group_desc) * ext2->s_group_count);
    if (err < 0) {
        goto err;
    }

    int i;
    for (i = 0; i < ext2->s_group_count; i++) {
        endian_swap_group_desc(&ext2->gd[i]);
        LTRACEF("group %d:\n", i);
        LTRACEF("\tblock bitmap %d\n", ext2->gd[i].bg_block_bitmap);
        LTRACEF("\tinode bitmap %d\n", ext2->gd[i].bg_inode_bitmap);
        LTRACEF("\tinode table %d\n", ext2->gd[i].bg_inode_table);
        LTRACEF("\tfree blocks %d\n", ext2->gd[i].bg_free_blocks_count);
        LTRACEF("\tfree inodes %d\n", ext2->gd[i].bg_free_inodes_count);
        LTRACEF("\tused dirs %d\n", ext2->gd[i].bg_used_dirs_count);
    }

    /* initialize the block cache */
    ext2->cache = bcache_create(ext2->dev, EXT2_BLOCK_SIZE(ext2->sb), 4);
    if (!ext2->cache) {
        err = ERR_NO_MEMORY;
        goto err;
    }

    /* Build the root vnode, which also validates that the root inode reads.
     * This is the only step of the mount that goes through the block cache --
     * the superblock and group descriptors above are read straight off the
     * device -- so it is the only one that needs the lock. Nothing else can
     * reach the mount yet; the lock is here to satisfy the block cache
     * routines, which assert on it. */
    mutex_acquire(&ext2->lock);
    err = ext2_create_vnode(ext2, EXT2_ROOT_INO, root);
    mutex_release(&ext2->lock);
    if (err < 0) {
        goto err;
    }

    //  TRACE("successfully mounted volume\n");

    *cookie = (fscookie *)ext2;

    return 0;

err:
    LTRACEF("exiting with err code %d\n", err);

    if (ext2->cache) {
        bcache_destroy(ext2->cache);
    }
    mutex_destroy(&ext2->lock);
    free(ext2->gd);
    free(ext2);
    return err;
}

/* The layer calls this only once the mount's last reference has gone and every
 * vnode of it has been released, so no thread can be inside a filesystem op or
 * blocked on the lock, and tearing both the lock and the cache down here is
 * safe. */
status_t ext2_unmount(fscookie *cookie) {
    // free it up
    ext2_t *ext2 = (ext2_t *)cookie;

    bcache_destroy(ext2->cache);
    mutex_destroy(&ext2->lock);
    free(ext2->gd);
    free(ext2);

    return 0;
}

static void get_inode_addr(ext2_t *ext2, inodenum_t num, blocknum_t *block, size_t *block_offset) {
    num--;

    uint32_t group = num / ext2->sb.s_inodes_per_group;

    // calculate the start of the inode table for the group it's in
    *block = ext2->gd[group].bg_inode_table;

    // add the offset of the inode within the group
    size_t offset = (size_t)(num % EXT2_INODES_PER_GROUP(ext2->sb)) * EXT2_INODE_SIZE(ext2->sb);
    *block_offset = offset % EXT2_BLOCK_SIZE(ext2->sb);
    *block += offset / EXT2_BLOCK_SIZE(ext2->sb);
}

int ext2_load_inode(ext2_t *ext2, inodenum_t num, struct ext2_inode *inode) {
    int err;

    DEBUG_ASSERT(is_mutex_held(&ext2->lock));

    LTRACEF("num %d, inode %p\n", num, inode);

    blocknum_t bnum;
    size_t block_offset;
    get_inode_addr(ext2, num, &bnum, &block_offset);

    LTRACEF("bnum %u, offset %zd\n", bnum, block_offset);

    /* get a pointer to the cache block */
    void *cache_ptr;
    err = bcache_get_block(ext2->cache, &cache_ptr, bnum);
    if (err < 0) {
        return err;
    }

    /* copy the inode out */
    memcpy(inode, (uint8_t *)cache_ptr + block_offset, sizeof(struct ext2_inode));

    /* put the cache block */
    bcache_put_block(ext2->cache, bnum);

    /* endian swap it */
    endian_swap_inode(inode);

    LTRACEF("read inode: mode 0x%x, size %d\n", inode->i_mode, inode->i_size);

    return 0;
}

/* Wrap an inode number in a vnode for the layer. The filesystem hands back a
 * fresh one every time; the layer deduplicates by the inode number it carries
 * as the vnode id, so hard links to one object share a single vnode. */
status_t ext2_create_vnode(ext2_t *ext2, inodenum_t inum, struct fs_vnode **out) {
    ext2_vnode_t *v = malloc(sizeof(ext2_vnode_t));
    if (!v) {
        return ERR_NO_MEMORY;
    }

    v->ext2 = ext2;
    v->inum = inum;

    int err = ext2_load_inode(ext2, inum, &v->inode);
    if (err < 0) {
        free(v);
        return err;
    }

    enum fs_vnode_type type;
    if (S_ISDIR(v->inode.i_mode)) {
        type = FS_VNODE_DIR;
    } else if (S_ISLNK(v->inode.i_mode)) {
        type = FS_VNODE_SYMLINK;
    } else {
        /* anything else -- regular files, and the special files the driver
         * cannot do anything with either way */
        type = FS_VNODE_FILE;
    }

    status_t status = fs_vnode_create(inum, type, v, out);
    if (status < 0) {
        free(v);
        return status;
    }

    return NO_ERROR;
}

void ext2_release(struct fs_vnode *vn) {
    free(vn->priv);
}

static const struct fs_api ext2_api = {
    .mount = ext2_mount,
    .unmount = ext2_unmount,
    .lookup = ext2_lookup,
    .readlink = ext2_readlink,
    .release = ext2_release,
    .read = ext2_read,
    .stat = ext2_stat,
    .opendir = ext2_opendir,
    .readdir = ext2_readdir,
    .closedir = ext2_closedir,
};

STATIC_FS_IMPL(ext2, &ext2_api);
