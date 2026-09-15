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

#include "fat_fs.h"
#include "fat_priv.h"

// file side of the vnode interface; the object itself is fat_vnode in fat_priv.h
status_t fat_create(struct fs_vnode *dir, const char *name, uint64_t len, struct fs_vnode **out);
ssize_t fat_read(struct fs_vnode *vn, void *buf, off_t offset, size_t len);
ssize_t fat_write(struct fs_vnode *vn, const void *buf, off_t offset, size_t len);
status_t fat_truncate(struct fs_vnode *vn, uint64_t len);
status_t fat_stat(struct fs_vnode *vn, struct file_stat *stat);
