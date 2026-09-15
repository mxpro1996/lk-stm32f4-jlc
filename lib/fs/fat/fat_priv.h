/*
 * Copyright (c) 2015 Steve White
 * Copyright (c) 2022 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

#include <lib/bio.h>
#include <lib/fs.h>
#include <lk/compiler.h>

#include "fat_fs.h"

// Individual files should
// #define LOCAL_TRACE FAT_GLOBAL_TRACE(0)
// can override here for all fat files
#define FAT_GLOBAL_TRACE(local) (local | 0)

/* file allocation table parsing */
uint32_t fat_next_cluster_in_chain(fat_fs *fat, uint32_t cluster);
uint32_t fat_find_last_cluster_in_chain(fat_fs *fat, uint32_t starting_cluster);
status_t fat_allocate_cluster_chain(fat_fs *fat, uint32_t start_cluster, uint32_t count,
                                    uint32_t *first_cluster, uint32_t *last_cluster,
                                    bool zero_new_blocks);
status_t fat_free_cluster_chain(fat_fs *fat, uint32_t start_cluster);
status_t fat_truncate_cluster_chain(fat_fs *fat, uint32_t keep_last_cluster);

/* general io routines */
uint32_t fat_sector_for_cluster(fat_fs *fat, uint32_t cluster);
ssize_t fat_read_cluster(fat_fs *fat, void *buf, uint32_t cluster);
ssize_t fat_zero_cluster(fat_fs *fat, uint32_t cluster);

// general directory apis outside of an object
struct dir_entry {
    fat_attribute attributes;
    uint32_t length;
    uint32_t start_cluster;
    // TODO time
};

// where an object's short name entry lives: the starting cluster of the directory
// holding it and the byte offset of the entry within that directory
struct dir_entry_location {
    uint32_t starting_dir_cluster;
    uint32_t dir_offset;
};

inline bool operator==(const dir_entry_location &a, const dir_entry_location &b) {
    return (a.starting_dir_cluster == b.starting_dir_cluster && a.dir_offset == b.dir_offset);
}

// The root directory has no entry naming it anywhere, so it needs a location no
// real object can occupy. Cluster 1 is reserved by the FAT specification and is
// never the starting cluster of a directory, which makes 1:0 unambiguous even on
// FAT12/16, where the root directory itself is addressed as the magic cluster 0
// and a file really can live at offset 0 of it.
constexpr uint32_t kRootDirCluster = 1;
constexpr dir_entry_location kRootDirLocation = {kRootDirCluster, 0};

// One filesystem object, file or directory, hanging off the priv pointer of a
// vnode the fs layer owns. It caches the directory entry describing the object
// along with the extent of the on-disk record that names it, so writing a new
// size or deleting the object costs no directory scan.
class fat_vnode {
  public:
    fat_vnode(fat_fs *fs, const dir_entry &entry, const dir_entry_location &loc,
              uint32_t record_start_offset)
        : fs_(fs), loc_(loc), record_start_offset_(record_start_offset),
          start_cluster_(entry.start_cluster),
          length_((entry.attributes == fat_attribute::directory) ? 0 : entry.length),
          attributes_(entry.attributes) {}

    // Identity the layer deduplicates on, so two lookups of one object share a
    // vnode. The high bit is set because a location of 0:0 is legal -- the first
    // entry of a FAT12/16 root directory -- while an id of 0 means "no identity"
    // to the layer and would leave that one file with a vnode per open.
    uint64_t id() const {
        return (1ULL << 63) | ((uint64_t)loc_.starting_dir_cluster << 32) | loc_.dir_offset;
    }

    fat_fs *fs() const { return fs_; }
    const dir_entry_location &loc() const { return loc_; }
    uint32_t record_start_offset() const { return record_start_offset_; }
    uint32_t start_cluster() const { return start_cluster_; }
    bool is_dir() const { return attributes_ == fat_attribute::directory; }
    bool is_root() const { return loc_.starting_dir_cluster == kRootDirCluster; }

    // file ops, called through the vnode interface
    ssize_t read(void *buf, off_t offset, size_t len);
    ssize_t write(const void *buf, off_t offset, size_t len);
    status_t truncate(uint64_t len);
    status_t stat(struct file_stat *out);

  private:
    status_t zero_range_locked(uint32_t offset, uint32_t len);

    fat_fs *const fs_;

    // our dir entry: where it lives, and the record that names it
    const dir_entry_location loc_;
    const uint32_t record_start_offset_;

    // our start cluster and length, mirroring the dir entry
    uint32_t start_cluster_ = 0;
    uint32_t length_ = 0;

    const fat_attribute attributes_;
};

// the fat_vnode a layer vnode carries
inline fat_vnode *vnode_of(struct fs_vnode *vn) {
    return (fat_vnode *)vn->priv;
}

// wrap a directory entry in a vnode for the layer, which owns it from then on
status_t fat_vnode_create(fat_fs *fat, const dir_entry &entry, const dir_entry_location &loc,
                          uint32_t record_start_offset, struct fs_vnode **out);

// allocate a new entry called name in the directory starting at parent_cluster,
// passing back the location of its short name entry and the start of its record
status_t fat_dir_allocate(fat_fs *fat, uint32_t parent_cluster, const char *name,
                          fat_attribute attr, uint32_t starting_cluster, uint32_t size,
                          dir_entry_location *loc, uint32_t *record_start_offset);

status_t fat_dir_update_entry(fat_fs *fat, const dir_entry_location &loc, uint32_t starting_cluster, uint32_t size);

// mark the whole record naming an object deleted, given the extent a vnode carries
status_t fat_dir_remove_entry(fat_fs *fat, const dir_entry_location &loc,
                              uint32_t record_start_offset);
