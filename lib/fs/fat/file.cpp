/*
 * Copyright (c) 2015 Steve White
 * Copyright (c) 2022 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#include "file.h"

#include <lib/bio.h>
#include <lib/fs.h>
#include <lk/debug.h>
#include <lk/err.h>
#include <lk/trace.h>
#include <stdlib.h>
#include <string.h>

#include "fat_fs.h"
#include "fat_priv.h"

#include "file_iterator.h"

#define LOCAL_TRACE FAT_GLOBAL_TRACE(0)

status_t fat_vnode::zero_range_locked(uint32_t offset, uint32_t len) {
    DEBUG_ASSERT(fs_->lock.is_held());

    if (len == 0) {
        return NO_ERROR;
    }

    if (start_cluster_ < 2 || start_cluster_ >= fs_->info().total_clusters) {
        return ERR_IO;
    }

    uint32_t logical_cluster = offset / fs_->info().bytes_per_cluster;
    uint32_t sector_within_cluster =
        (offset % fs_->info().bytes_per_cluster) / fs_->info().bytes_per_sector;
    uint32_t offset_within_sector = offset % fs_->info().bytes_per_sector;

    file_block_iterator fbi(fs_, start_cluster_);
    status_t err = fbi.next_sectors(logical_cluster * fs_->info().sectors_per_cluster +
                                    sector_within_cluster);
    if (err < 0) {
        return err;
    }

    uint32_t remaining = len;
    while (remaining > 0) {
        uint32_t to_zero =
            MIN(fs_->info().bytes_per_sector - offset_within_sector, remaining);

        uint8_t *ptr = fbi.get_bcache_ptr(offset_within_sector);
        if (!ptr) {
            return ERR_IO;
        }

        memset(ptr, 0, to_zero);
        err = fbi.mark_bcache_dirty();
        if (err < 0) {
            return err;
        }

        remaining -= to_zero;
        offset_within_sector += to_zero;
        if (offset_within_sector == fs_->info().bytes_per_sector && remaining > 0) {
            offset_within_sector = 0;
            err = fbi.next_sector();
            if (err < 0) {
                return err;
            }
        }
    }

    return NO_ERROR;
}

ssize_t fat_vnode::read(void *_buf, const off_t offset, size_t len) {
    uint8_t *buf = (uint8_t *)_buf;

    LTRACEF("file %p buf %p offset %lld len %zu\n", this, _buf, offset, len);

    if (is_dir()) {
        return ERR_NOT_FILE;
    }

    // negative offsets are invalid
    if (offset < 0) {
        return ERR_INVALID_ARGS;
    }

    AutoLock guard(fs_->lock);

    // trim the read to the file
    if (offset >= length_) {
        return 0;
    }

    // above test should ensure offset < 32bit because file->length is 32bit unsigned
    DEBUG_ASSERT(offset <= UINT32_MAX);

    if (offset + len > length_) {
        len = length_ - offset;
    }

    LTRACEF("trimmed offset %lld len %zu\n", offset, len);

    // create a file block iterator and push it forward to the starting point
    uint32_t logical_cluster = offset / fs_->info().bytes_per_cluster;
    uint32_t sector_within_cluster = (offset % fs_->info().bytes_per_cluster) / fs_->info().bytes_per_sector;
    uint32_t offset_within_sector = offset % fs_->info().bytes_per_sector;

    LTRACEF("starting off logical cluster %u, sector within %u, offset within %u\n",
            logical_cluster, sector_within_cluster, offset_within_sector);

    file_block_iterator fbi(fs_, start_cluster_);

    // move it forward to our index point
    // also loads the buffer
    status_t err = fbi.next_sectors(logical_cluster * fs_->info().sectors_per_cluster + sector_within_cluster);
    if (err < 0) {
        LTRACEF("error moving up to starting point!\n");
        return err;
    }

    ssize_t amount_read = 0;
    size_t buf_offset = 0; // offset into the output buffer
    while (buf_offset < len) {
        size_t to_read = MIN(fs_->info().bytes_per_sector - offset_within_sector, len - buf_offset);

        LTRACEF("top of loop: sector offset %u, buf offset %zu, remaining len %zu, to_read %zu\n",
                offset_within_sector, buf_offset, len, to_read);

        // grab a pointer directly to the block cache
        const uint8_t *ptr;
        ptr = fbi.get_bcache_ptr(offset_within_sector);
        if (!ptr) {
            LTRACEF("error getting pointer to file in cache\n");
            return ERR_IO;
        }

        // make sure we dont do something silly
        DEBUG_ASSERT(buf_offset + to_read <= len);
        DEBUG_ASSERT(offset_within_sector + to_read <= fs_->info().bytes_per_sector);

        // copy out to the buffer
        memcpy(buf + buf_offset, ptr, to_read);

        // next iteration of the loop
        amount_read += to_read;
        buf_offset += to_read;

        // go to the next sector
        offset_within_sector += to_read;
        DEBUG_ASSERT(offset_within_sector <= fs_->info().bytes_per_sector);
        if (offset_within_sector == fs_->info().bytes_per_sector) {
            offset_within_sector = 0;
            // push the iterator forward to next sector
            if (buf_offset < len) {
                err = fbi.next_sector();
                if (err < 0) {
                    return err;
                }
            }
        }
    }

    return amount_read;
}

ssize_t fat_read(struct fs_vnode *vn, void *buf, off_t offset, size_t len) {
    return vnode_of(vn)->read(buf, offset, len);
}

status_t fat_vnode::stat(struct file_stat *out) {
    AutoLock guard(fs_->lock);

    LTRACEF("vnode %p stat %p\n", this, out);

    // directories, the root included, have no length of their own
    out->size = length_;
    out->is_dir = is_dir();
    return NO_ERROR;
}

status_t fat_stat(struct fs_vnode *vn, struct file_stat *stat) {
    return vnode_of(vn)->stat(stat);
}

status_t fat_create(struct fs_vnode *dir, const char *name, uint64_t len,
                    struct fs_vnode **out) {
    fat_vnode *dirv = vnode_of(dir);
    fat_fs *fs = dirv->fs();

    LTRACEF("dir %p name '%s' len %" PRIu64 "\n", dirv, name, len);

    DEBUG_ASSERT(dirv->is_dir());

    // currently only support zero length files
    if (len != 0) {
        return ERR_NOT_IMPLEMENTED;
    }

    if (fs->is_read_only()) {
        return ERR_NOT_ALLOWED;
    }

    AutoLock guard(fs->lock);

    // tell the dir code to find us a spot
    dir_entry_location loc = {};
    uint32_t record_start_offset = 0;
    status_t err = fat_dir_allocate(fs, dirv->start_cluster(), name, fat_attribute::file, 0, 0,
                                    &loc, &record_start_offset);
    if (err < 0) {
        return err;
    }

    const dir_entry entry = {
        .attributes = fat_attribute::file,
        .length = 0,
        .start_cluster = 0,
    };

    err = fat_vnode_create(fs, entry, loc, record_start_offset, out);
    if (err < 0) {
        // undo the entry we just wrote rather than leave a file nothing can open
        fat_dir_remove_entry(fs, loc, record_start_offset);
        return err;
    }

    return NO_ERROR;
}

status_t fat_vnode::truncate(uint64_t _len) {
    LTRACEF("file %p, len %" PRIu64 " \n", this, _len);

    // a directory can be opened as a file handle, and the root has no entry to
    // write a length back to at all
    if (is_dir()) {
        return ERR_NOT_FILE;
    }

    if (_len == length_) {
        return NO_ERROR;
    }

    if (fs_->is_read_only()) {
        return ERR_NOT_ALLOWED;
    }

    // test some boundary conditions
    // ULL, not UL: on a 32 bit target UL is 32 bits and a limit expressed that
    // way is one factor away from silently overflowing to zero
    if (_len >= 2ULL * 1024 * 1024 * 1024) {
        // 2GB limit on the fs
        return ERR_TOO_BIG;
    }

    AutoLock guard(fs_->lock);

    // from now on out use this as our length variable
    const uint32_t len32 = _len;

    // TODO: test for max size of fs

    if (len32 == length_) {
        return NO_ERROR;
    } else {
        // are we expanding/shrinking within a cluster that's already allocated?
        const uint32_t bpc = fs_->info().bytes_per_cluster;
        const uint32_t current_cluster_count = (length_ + bpc - 1) / bpc;
        const uint32_t new_cluster_count = (len32 + bpc - 1) / bpc;
        LTRACEF("existing len %u, clusters %u: newlen %u, clusters %u\n", length_, current_cluster_count, len32, new_cluster_count);
        if (new_cluster_count == current_cluster_count) {
            // new length doesn't change the cluster count
            // update the dir entry and move on
            status_t err = fat_dir_update_entry(fs_, loc_, start_cluster_, len32);
            if (err != NO_ERROR) {
                return err;
            }

            // remember our new length
            length_ = len32;
        } else if (len32 > length_) {
            // expanding the file
            LTRACEF("expanding the file: start_cluster_ %u\n", start_cluster_);

            const uint32_t old_length = length_;

            // TODO: compartmentalize this cluster extension/shrinking so DIR code can reuse it

            // walk to the end of the existing cluster chain
            const uint32_t existing_chain_end = fat_find_last_cluster_in_chain(fs_, start_cluster_);

            uint32_t first_cluster;
            uint32_t last_cluster;
            status_t err = fat_allocate_cluster_chain(fs_, existing_chain_end, new_cluster_count - current_cluster_count,
                                                      &first_cluster, &last_cluster, true);

            LTRACEF("fat_allocate_cluster_chain returns %d, first_cluster %u, last_cluster %u\n", err, first_cluster, last_cluster);
            if (err != NO_ERROR) {
                return err;
            }

            // update the dir entry, linking the first cluster in our chain if it's the first one
            err = fat_dir_update_entry(fs_, loc_, start_cluster_ ? start_cluster_ : first_cluster, len32);
            if (err != NO_ERROR) {
                return err;
            }

            // remember our new length
            length_ = len32;

            // if we just created the first cluster, remember it here
            if (start_cluster_ == 0) {
                start_cluster_ = first_cluster;
            }

            // Zero-fill any newly exposed bytes in the old tail cluster.
            if (old_length < len32) {
                const uint32_t old_tail = old_length % bpc;
                if (old_tail != 0) {
                    const uint32_t bytes_to_cluster_end = bpc - old_tail;
                    const uint32_t bytes_to_zero = MIN(bytes_to_cluster_end, len32 - old_length);
                    err = zero_range_locked(old_length, bytes_to_zero);
                    if (err != NO_ERROR) {
                        return err;
                    }
                }
            }
        } else {
            // shrinking the file
            uint32_t new_start_cluster = start_cluster_;

            if (new_cluster_count == 0) {
                // File shrunk to zero length: free the entire chain and clear start cluster.
                status_t err = fat_free_cluster_chain(fs_, start_cluster_);
                if (err != NO_ERROR) {
                    return err;
                }
                new_start_cluster = 0;
            } else {
                if (start_cluster_ < 2 || start_cluster_ >= fs_->info().total_clusters) {
                    return ERR_IO;
                }

                // Find the last cluster that remains part of the truncated file.
                uint32_t keep_last = start_cluster_;
                for (uint32_t i = 1; i < new_cluster_count; i++) {
                    uint32_t next = fat_next_cluster_in_chain(fs_, keep_last);
                    if (is_eof_cluster(next) || next < 2 || next >= fs_->info().total_clusters) {
                        return ERR_IO;
                    }
                    keep_last = next;
                }

                uint32_t first_free = fat_next_cluster_in_chain(fs_, keep_last);
                if (!is_eof_cluster(first_free)) {
                    status_t err = fat_truncate_cluster_chain(fs_, keep_last);
                    if (err != NO_ERROR) {
                        return err;
                    }
                }
            }

            status_t err = fat_dir_update_entry(fs_, loc_, new_start_cluster, len32);
            if (err != NO_ERROR) {
                return err;
            }

            start_cluster_ = new_start_cluster;
            length_ = len32;
        }
    }

    bcache_flush(fs_->bcache());

    return NO_ERROR;
}

ssize_t fat_vnode::write(const void *_buf, const off_t offset, size_t len) {
    const uint8_t *buf = (const uint8_t *)_buf;

    LTRACEF("file %p buf %p offset %lld len %zu\n", this, _buf, offset, len);

    if (is_dir()) {
        return ERR_NOT_FILE;
    }

    if (fs_->is_read_only()) {
        return ERR_NOT_ALLOWED;
    }

    if (offset < 0) {
        return ERR_INVALID_ARGS;
    }

    if (len == 0) {
        return 0;
    }

    const uint64_t end = (uint64_t)offset + len;
    if (end >= 2ULL * 1024 * 1024 * 1024) {
        return ERR_TOO_BIG;
    }

    if (end > length_) {
        status_t err = truncate(end);
        if (err != NO_ERROR) {
            return err;
        }
    }

    AutoLock guard(fs_->lock);

    uint32_t logical_cluster = offset / fs_->info().bytes_per_cluster;
    uint32_t sector_within_cluster =
        (offset % fs_->info().bytes_per_cluster) / fs_->info().bytes_per_sector;
    uint32_t offset_within_sector = offset % fs_->info().bytes_per_sector;

    file_block_iterator fbi(fs_, start_cluster_);
    status_t err = fbi.next_sectors(logical_cluster * fs_->info().sectors_per_cluster +
                                    sector_within_cluster);
    if (err < 0) {
        return err;
    }

    size_t written = 0;
    while (written < len) {
        size_t to_write = MIN(fs_->info().bytes_per_sector - offset_within_sector, len - written);

        uint8_t *ptr = fbi.get_bcache_ptr(offset_within_sector);
        if (!ptr) {
            return ERR_IO;
        }

        memcpy(ptr, buf + written, to_write);
        err = fbi.mark_bcache_dirty();
        if (err < 0) {
            return err;
        }

        written += to_write;
        offset_within_sector += to_write;
        if (offset_within_sector == fs_->info().bytes_per_sector && written < len) {
            offset_within_sector = 0;
            err = fbi.next_sector();
            if (err < 0) {
                return err;
            }
        }
    }

    bcache_flush(fs_->bcache());

    return written;
}

ssize_t fat_write(struct fs_vnode *vn, const void *buf, off_t offset, size_t len) {
    return vnode_of(vn)->write(buf, offset, len);
}

status_t fat_truncate(struct fs_vnode *vn, uint64_t len) {
    return vnode_of(vn)->truncate(len);
}
