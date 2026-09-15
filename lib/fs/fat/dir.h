/*
 * Copyright (c) 2022 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

#include <inttypes.h>
#include <lib/fs.h>
#include <lk/list.h>

#include "file.h"

class fat_fs;
struct dir_entry;
struct dir_entry_location;

// Convert UTF-8 to UCS-2 for FAT LFN handling.
// Returns NO_ERROR on success, ERR_INVALID_ARGS for malformed/unrepresentable UTF-8,
// and ERR_TOO_BIG if max_ucs2_len is insufficient.
status_t fat_utf8_to_ucs2(const char *utf8, uint16_t *ucs2, size_t max_ucs2_len,
                          size_t *out_ucs2_len);

// Convert UCS-2 to UTF-8 for FAT LFN read path.
// Returns NO_ERROR on success, ERR_TOO_BIG if the UTF-8 output buffer is too small.
status_t fat_ucs2_to_utf8(const uint16_t *ucs2, size_t ucs2_len, char *utf8,
                          size_t max_utf8_len, size_t *out_utf8_len);

// Convert a user name to a canonical 8.3 short name (space padded, null terminated).
status_t name_to_short_file_name(char sfn[8 + 3 + 1], const char *name);

// directory side of the vnode interface
status_t fat_lookup(struct fs_vnode *dir, const char *name, struct fs_vnode **out);
status_t fat_mkdir(struct fs_vnode *dir, const char *name, struct fs_vnode **out);
status_t fat_unlink(struct fs_vnode *dir, const char *name, struct fs_vnode *child);
status_t fat_rmdir(struct fs_vnode *dir, const char *name, struct fs_vnode *child);
status_t fat_opendir(struct fs_vnode *vn, dircookie **dcookie);
status_t fat_readdir(dircookie *dcookie, struct dirent *ent);
status_t fat_closedir(dircookie *dcookie);
