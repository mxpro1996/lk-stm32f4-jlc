/*
 * Copyright (c) 2015 Steve White
 * Copyright (c) 2022 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#include "dir.h"

#include <ctype.h>
#include <endian.h>
#include <lib/bcache/bcache_block_ref.h>
#include <lk/cpp.h>
#include <lk/err.h>
#include <lk/trace.h>
#include <lktl/auto_call.h>
#include <memory>
#include <new>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fat_fs.h"
#include "fat_priv.h"
#include "file_iterator.h"

#define LOCAL_TRACE FAT_GLOBAL_TRACE(0)

// An open dir handle: the directory being walked and how far into it we are.
// The layer holds a reference on the vnode for as long as the handle is open,
// which is what keeps the fat_vnode below alive.
struct fat_dir_cookie {
    fat_vnode *dir;

    // next directory offset to read, in bytes from the start of the directory
    uint32_t index;
};

// Convert a UTF-8 path element into UCS-2 code units used by FAT LFN entries.
// Rejects malformed/overlong UTF-8, surrogate code points, and non-BMP code points.
status_t fat_utf8_to_ucs2(const char *utf8, uint16_t *ucs2, size_t max_ucs2_len,
                          size_t *out_ucs2_len) {
    DEBUG_ASSERT(utf8 && ucs2);

    size_t out = 0;
    for (size_t i = 0; utf8[i] != '\0';) {
        uint32_t codepoint = 0;
        uint8_t b0 = static_cast<uint8_t>(utf8[i]);

        if (b0 < 0x80) {
            codepoint = b0;
            i += 1;
        } else if ((b0 & 0xe0) == 0xc0) {
            if (utf8[i + 1] == '\0') {
                return ERR_INVALID_ARGS;
            }
            uint8_t b1 = static_cast<uint8_t>(utf8[i + 1]);
            if ((b1 & 0xc0) != 0x80) {
                return ERR_INVALID_ARGS;
            }

            codepoint = ((b0 & 0x1f) << 6) | (b1 & 0x3f);
            if (codepoint < 0x80) {
                return ERR_INVALID_ARGS;
            }
            i += 2;
        } else if ((b0 & 0xf0) == 0xe0) {
            if (utf8[i + 1] == '\0' || utf8[i + 2] == '\0') {
                return ERR_INVALID_ARGS;
            }
            uint8_t b1 = static_cast<uint8_t>(utf8[i + 1]);
            uint8_t b2 = static_cast<uint8_t>(utf8[i + 2]);
            if ((b1 & 0xc0) != 0x80 || (b2 & 0xc0) != 0x80) {
                return ERR_INVALID_ARGS;
            }

            codepoint = ((b0 & 0x0f) << 12) | ((b1 & 0x3f) << 6) | (b2 & 0x3f);
            if (codepoint < 0x800) {
                return ERR_INVALID_ARGS;
            }
            i += 3;
        } else {
            // FAT LFN stores UCS-2 and cannot represent non-BMP code points.
            return ERR_INVALID_ARGS;
        }

        // reject UTF-8 that would encode surrogate code points
        if (codepoint >= 0xd800 && codepoint <= 0xdfff) {
            return ERR_INVALID_ARGS;
        }
        // reject non-BMP code points, since FAT LFN uses UCS-2 and cannot represent them
        if (codepoint > 0xffff) {
            return ERR_INVALID_ARGS;
        }

        if (out >= max_ucs2_len) {
            return ERR_TOO_BIG;
        }
        ucs2[out++] = static_cast<uint16_t>(codepoint);
    }

    if (out_ucs2_len) {
        *out_ucs2_len = out;
    }
    return NO_ERROR;
}

/* Convert a single UCS-2 code point to UTF-8, writing bytes in reverse order.
 * Returns the number of bytes written (1-3). */
inline size_t ucs2_char_to_utf8(uint16_t c, char out[3]) {

    // TODO: handle > U+FFFF code points if we ever want to support them in FAT LFN.
    if (c < 0x80) {
        out[0] = static_cast<char>(c);
        return 1;
    } else if (c < 0x800) {
        out[0] = static_cast<char>(0xc0 | (c >> 6));
        out[1] = static_cast<char>(0x80 | (c & 0x3f));
        return 2;
    } else {
        out[0] = static_cast<char>(0xe0 | (c >> 12));
        out[1] = static_cast<char>(0x80 | ((c >> 6) & 0x3f));
        out[2] = static_cast<char>(0x80 | (c & 0x3f));
        return 3;
    }
}

// Convert UCS-2 code units (from FAT LFN) to UTF-8.
// Returns NO_ERROR on success, ERR_TOO_BIG if the UTF-8 output buffer is too small.
status_t fat_ucs2_to_utf8(const uint16_t *ucs2, size_t ucs2_len, char *utf8,
                          size_t max_utf8_len, size_t *out_utf8_len) {
    DEBUG_ASSERT(utf8);

    if (ucs2_len == 0) {
        utf8[0] = '\0';
        if (out_utf8_len) {
            *out_utf8_len = 0;
        }
        return NO_ERROR;
    }

    DEBUG_ASSERT(ucs2);

    size_t out = 0;
    for (size_t i = 0; i < ucs2_len; i++) {
        uint16_t c = ucs2[i];
        char utf8_bytes[3];
        size_t nbytes = ucs2_char_to_utf8(c, utf8_bytes);

        if (out + nbytes > max_utf8_len) {
            return ERR_TOO_BIG;
        }
        // write forwards by temporarily offsetting the buffer
        for (size_t j = 0; j < nbytes; j++) {
            utf8[out + j] = utf8_bytes[j];
        }
        out += nbytes;
    }

    utf8[out] = '\0';
    if (out_utf8_len) {
        *out_utf8_len = out;
    }
    return NO_ERROR;
}

namespace {

bool fat_short_name_taken(fat_fs *fat, uint32_t starting_cluster, const char short_name[11],
                          status_t *err_out);

constexpr size_t kFatMaxLfnChars = 255;
constexpr size_t kFatLfnCharsPerEntry = 13;

constexpr size_t kLfnNameOffsets[kFatLfnCharsPerEntry] = {
    1,
    3,
    5,
    7,
    9,
    14,
    16,
    18,
    20,
    22,
    24,
    28,
    30,
};

uint8_t fat_lfn_sfn_checksum(const uint8_t short_name[11]) {
    uint8_t checksum = 0;
    for (size_t i = 0; i < 11; i++) {
        checksum = static_cast<uint8_t>(((checksum & 1) ? 0x80 : 0) + (checksum >> 1) + short_name[i]);
    }
    return checksum;
}

inline char sanitize_sfn_char(uint8_t c) {
    if (isalnum(c)) {
        return static_cast<char>(toupper(c));
    }
    return '_';
}

void write_lfn_name_part(uint8_t *ent, const uint16_t *ucs2, size_t ucs2_len,
                         size_t sequence) {
    size_t start = (sequence - 1) * kFatLfnCharsPerEntry;
    for (size_t i = 0; i < kFatLfnCharsPerEntry; i++) {
        size_t idx = start + i;
        uint16_t v;
        if (idx < ucs2_len) {
            v = ucs2[idx];
        } else if (idx == ucs2_len) {
            v = 0x0000;
        } else {
            v = 0xffff;
        }
        fat_write16(ent, kLfnNameOffsets[i], v);
    }
}

void fill_lfn_dirent(uint8_t *ent, const uint16_t *ucs2, size_t ucs2_len,
                     uint8_t sequence, bool sequence_is_last, uint8_t checksum) {
    memset(ent, 0, DIR_ENTRY_LENGTH);
    ent[0] = sequence_is_last ? static_cast<uint8_t>(sequence | 0x40) : sequence;
    ent[11] = static_cast<uint8_t>(fat_attribute::lfn);
    ent[12] = 0;
    ent[13] = checksum;
    fat_write16(ent, 26, 0);
    write_lfn_name_part(ent, ucs2, ucs2_len, sequence);
}

void build_short_name_alias(const char *name, uint32_t ordinal, char sfn[12]) {
    memset(sfn, ' ', 11);
    sfn[11] = 0;

    const char *dot = strrchr(name, '.');
    const char *stem_end = dot ? dot : (name + strlen(name));

    char ext[3] = {' ', ' ', ' '};
    if (dot && dot[1] != 0) {
        size_t e = 0;
        for (const char *p = dot + 1; *p && e < sizeof(ext); p++) {
            if (*p == '.') {
                continue;
            }
            ext[e++] = sanitize_sfn_char(static_cast<uint8_t>(*p));
        }
    }

    char suffix[8];
    snprintf(suffix, sizeof(suffix), "~%" PRIu32, ordinal);
    size_t suffix_len = strlen(suffix);
    suffix_len = MIN(suffix_len, 7u);
    size_t prefix_len = 8 - suffix_len;

    size_t out = 0;
    for (const char *p = name; p < stem_end && out < prefix_len; p++) {
        if (*p == '.' || *p == ' ') {
            continue;
        }
        sfn[out++] = sanitize_sfn_char(static_cast<uint8_t>(*p));
    }
    while (out < prefix_len) {
        sfn[out++] = '_';
    }
    for (size_t i = 0; i < suffix_len; i++) {
        sfn[prefix_len + i] = suffix[i];
    }

    memcpy(&sfn[8], ext, sizeof(ext));
}

status_t generate_unique_short_name_for_lfn(fat_fs *fat, uint32_t starting_dir_cluster,
                                            const char *name, char sfn[12]) {
    for (uint32_t ord = 1; ord < 1000000; ord++) {
        build_short_name_alias(name, ord, sfn);

        status_t err;
        if (!fat_short_name_taken(fat, starting_dir_cluster, sfn, &err)) {
            if (err < 0) {
                return err;
            }
            // this short name is not taken, we can use it
            return NO_ERROR;
        }
    }

    return ERR_ALREADY_EXISTS;
}

// walk one entry into the dir, starting at byte offset into the directory block iterator.
// both dbi and offset will be modified during the call.
// filles out the entry and returns a pointer into the passed in buffer in out_filename.
// NOTE: *must* pass at least a MAX_FILE_NAME_LEN byte char pointer in the filename_buffer slot.
//
// out_record_start_entries, if passed, receives how many directory entries were
// stepped over before the start of the record just returned: the first of its long
// name entries, or the short name entry itself when it stands alone. Counting in
// entries rather than bytes keeps it independent of where in a sector the walk
// started; the caller adds it to the absolute offset it began the call at.
status_t fat_find_next_entry(fat_fs *fat, file_block_iterator &dbi, uint32_t &offset, dir_entry *entry,
                             char filename_buffer[MAX_FILE_NAME_LEN], char **out_filename,
                             uint32_t *out_record_start_entries = nullptr) {

    DEBUG_ASSERT(entry && filename_buffer && out_filename);

    // Note: offset is used as an ABSOLUTE directory offset (accounting for sector boundaries)
    // We track which sector we're in based on offset / bytes_per_sector

    // entries stepped over since the call started, which is the unit
    // out_record_start_entries is reported in
    uint32_t entries_scanned = 0;

    // lfn parsing state: build UTF-8 string backwards from the end of filename_buffer.
    struct lfn_parse_state {
        size_t utf8_pos = 0;          // next byte position to write (decrements)
        uint8_t max_sequence = 0;     // highest sequence (the first LFN entry with 0x40)
        uint8_t last_sequence = 0xff; // last sequence seen (for validation)
        uint8_t checksum = 0;
        uint32_t start_entry = 0; // index of the first LFN entry of the run
        bool start_valid = false;

        void reset() {
            utf8_pos = 0;
            max_sequence = 0;
            last_sequence = 0xff;
            start_valid = false;
        }
    } lfn_state;

    // step past the entry just examined
    auto next_entry = [&offset, &entries_scanned]() {
        offset += DIR_ENTRY_LENGTH;
        entries_scanned++;
    };

    for (;;) {
        if (LOCAL_TRACE >= 2) {
            LTRACEF("dir sector:\n");
            hexdump8_ex(dbi.get_bcache_ptr(0), fat->info().bytes_per_sector, 0);
        }

        // walk within a sector
        while (offset < fat->info().bytes_per_sector) {
            LTRACEF_LEVEL(2, "looking at offset %#x\n", offset);
            const uint8_t *ent = dbi.get_bcache_ptr(offset);
            if (ent[0] == 0) { // no more entries
                // we're completely done
                LTRACEF("completely done\n");
                return ERR_NOT_FOUND;
            } else if (ent[0] == 0xE5) { // deleted entry
                LTRACEF("deleted entry\n");
                lfn_state.reset();
                next_entry();
                continue;
            } else if (ent[0x0B] == (uint8_t)fat_attribute::volume_id) {
                // skip volume ids
                LTRACEF("skipping volume id\n");
                lfn_state.reset();
                next_entry();
                continue;
            } else if (ent[0x0B] == (uint8_t)fat_attribute::lfn) {
                // part of a LFN sequence
                uint8_t sequence = ent[0] & ~0x40;
                if (sequence == 0) {
                    // malformed LFN sequence, discard any accumulated state
                    LTRACEF("invalid LFN sequence 0\n");
                    lfn_state.reset();
                    next_entry();
                    continue;
                }
                // FAT stores LFN entries in reverse: highest sequence (with 0x40 flag)
                // first, then decreasing to sequence 1 immediately before the SFN.
                // We convert each UCS-2 code point to UTF-8 and write backwards
                // from the end of filename_buffer.
                if (ent[0] & 0x40) {
                    // end sequence (first LFN entry), start at end of buffer
                    lfn_state.utf8_pos = MAX_FILE_NAME_LEN;
                    lfn_state.max_sequence = sequence;
                    lfn_state.last_sequence = sequence;
                    lfn_state.checksum = ent[0x0d];
                    lfn_state.start_entry = entries_scanned;
                    lfn_state.start_valid = true;
                    LTRACEF_LEVEL(2, "start of new LFN entry, sequence %u\n", sequence);
                } else {
                    if (lfn_state.last_sequence != sequence + 1) {
                        // our entry is out of sequence? drop it and start over
                        LTRACEF("ent out of sequence %u (last sequence %u)\n", sequence, lfn_state.last_sequence);
                        lfn_state.reset();
                        next_entry();
                        continue;
                    }
                    if (lfn_state.checksum != ent[0x0d]) {
                        // all of the long sequences need to match the checksum
                        LTRACEF("ent mismatches previous checksum\n");
                        lfn_state.reset();
                        next_entry();
                        continue;
                    }
                    lfn_state.last_sequence = sequence;
                }

                // extract unicode characters and convert to UTF-8, writing backwards.
                constexpr size_t table[] = {30, 28, 24, 22, 20, 18, 16, 14, 9, 7, 5, 3, 1};
                for (size_t i = 0; i < kFatLfnCharsPerEntry; i++) {
                    uint16_t c = fat_read16(ent, table[i]);
                    if (c == 0xffff || c == 0x0) {
                        continue;
                    }

                    // Convert one UCS2 char to up to 3 utf-8 bytes in a temporary buffer
                    char utf8_bytes[3];
                    size_t nbytes = ucs2_char_to_utf8(c, utf8_bytes);
                    if (lfn_state.utf8_pos < nbytes) {
                        LTRACEF("LFN too long for filename buffer\n");
                        lfn_state.reset();
                        break;
                    }

                    // Copy them into the output buffer in reverse order
                    for (size_t j = 0; j < nbytes; j++) {
                        filename_buffer[lfn_state.utf8_pos - nbytes + j] = utf8_bytes[j];
                    }
                    lfn_state.utf8_pos -= nbytes;
                }

                if (lfn_state.max_sequence == 0) {
                    next_entry();
                    continue;
                }

                // iterate one more entry, since we need to at least need to find the corresponding SFN
                next_entry();
                continue;
            } else {
                // regular entry, extract the short file name
                char short_filename[8 + 1 + 3 + 1]; // max short name (8 . 3 NULL)
                size_t fname_pos = 0;

                // where this entry sits, in case it turns out to be a record of its own
                const uint32_t sfn_entry = entries_scanned;

                // Ignore trailing spaces in filename and/or extension
                int fn_len = 8, ext_len = 3;
                for (int i = 7; i >= 0; i--) {
                    if (ent[i] == 0x20) {
                        fn_len--;
                    } else {
                        break;
                    }
                }
                for (size_t i = 10; i >= 8; i--) {
                    if (ent[i] == 0x20) {
                        ext_len--;
                    } else {
                        break;
                    }
                }

                for (int i = 0; i < fn_len; i++) {
                    short_filename[fname_pos++] = ent[i];
                }
                if (ext_len > 0) {
                    short_filename[fname_pos++] = '.';
                    for (int i = 0; i < ext_len; i++) {
                        short_filename[fname_pos++] = ent[8 + i];
                    }
                }
                short_filename[fname_pos++] = 0;
                DEBUG_ASSERT(fname_pos <= sizeof(short_filename));

                // now we have the SFN, see if we just got finished parsing a corresponding LFN
                // in the previous entries
                uint32_t record_start_entry = sfn_entry;
                if (lfn_state.last_sequence == 1) {
                    uint8_t checksum = fat_lfn_sfn_checksum(ent);
                    if (checksum == lfn_state.checksum && lfn_state.utf8_pos < MAX_FILE_NAME_LEN) {
                        // move the backwards-built UTF-8 string to the start of the buffer
                        size_t utf8_len = MAX_FILE_NAME_LEN - lfn_state.utf8_pos;
                        memmove(filename_buffer, filename_buffer + lfn_state.utf8_pos, utf8_len);
                        filename_buffer[utf8_len] = '\0';
                        *out_filename = filename_buffer;

                        // the long name entries are part of this file's record
                        if (lfn_state.start_valid) {
                            record_start_entry = lfn_state.start_entry;
                        }
                    } else {
                        LTRACEF("LFN checksum mismatch, using SFN\n");
                        strlcpy(filename_buffer, short_filename, sizeof(short_filename));
                        *out_filename = filename_buffer;
                    }
                } else {
                    // copy the parsed short file name into the out buffer
                    strlcpy(filename_buffer, short_filename, sizeof(short_filename));
                    *out_filename = filename_buffer;
                }

                if (out_record_start_entries) {
                    *out_record_start_entries = record_start_entry;
                }

                lfn_state.reset();
                next_entry();

                // fall through, we've found a file entry
            }

            LTRACEF("found filename '%s'\n", *out_filename);

            // fill out the passed in dir entry and exit
            uint32_t target_cluster = fat_read16(ent, 0x1a);
            if (fat->info().fat_bits == 32) {
                target_cluster |= (uint32_t)fat_read16(ent, 0x14) << 16;
                target_cluster &= 0x0fffffff;
            }
            entry->length = fat_read32(ent, 0x1c);
            entry->attributes = (fat_attribute)ent[0x0B];
            entry->start_cluster = target_cluster;
            return NO_ERROR;
        }

        DEBUG_ASSERT(offset <= fat->info().bytes_per_sector);

        // move to the next sector
        status_t err = dbi.next_sector();
        if (err < 0) {
            break;
        }
        // start over at offset 0 in the new sector
        offset = 0;
    }

    // we're out of entries
    return ERR_NOT_FOUND;
}

// Find a named entry in a directory, reporting the extent of the on-disk record
// that names it: [entry_start_offset, entry_end_offset) in bytes from the start of
// the directory. The last entry of that span is the short name entry, which is what
// a dir_entry_location points at and what fat_dir_update_entry writes to; anything
// ahead of it is the long name run. Both offsets are optional.
status_t fat_find_file_in_dir(fat_fs *fat, uint32_t starting_cluster, const char *name,
                              dir_entry *entry, uint32_t *entry_start_offset,
                              uint32_t *entry_end_offset) {
    LTRACEF("start_cluster %u, name '%s', out entry %p\n", starting_cluster, name, entry);

    DEBUG_ASSERT(fat->lock.is_held());
    DEBUG_ASSERT(entry);

    // cache the length of the string we're matching against
    const size_t namelen = strlen(name);

    // kick start our directory sector iterator
    file_block_iterator dbi(fat, starting_cluster);
    status_t err = dbi.next_sectors(0);
    if (err < 0) {
        return err;
    }

    uint32_t offset = 0;
    uint32_t dir_offset_base = 0;
    char *filename_buffer = fat->name_scratch();
    for (;;) {
        char *filename;
        uint32_t record_start_entries;
        const uint32_t scan_start_offset = dir_offset_base + offset;

        // Reset the sector increment count before calling fat_find_next_entry,
        // which may call next_sector one or more times.
        dbi.reset_sector_inc_count();

        // step forward one entry and see if we got something
        err = fat_find_next_entry(fat, dbi, offset, entry, filename_buffer, &filename,
                                  &record_start_entries);
        if (err < 0) {
            return err;
        }

        // Account for any sector increments that happened in fat_find_next_entry to keep an
        // absolute offset into the directory
        dir_offset_base += dbi.get_sector_inc_count() * fat->info().bytes_per_sector;

        const size_t filenamelen = strlen(filename);

        // see if we've matched an entry
        if (filenamelen == namelen && !strnicmp(name, filename, filenamelen)) {
            // fat_find_next_entry leaves offset just past the short name entry it
            // matched and reports how many entries it stepped over to reach the
            // start of the record, which may include deleted slots it skipped.
            const uint32_t end_offset = dir_offset_base + offset;
            DEBUG_ASSERT(end_offset >= DIR_ENTRY_LENGTH);
            if (entry_start_offset) {
                *entry_start_offset = scan_start_offset + record_start_entries * DIR_ENTRY_LENGTH;
                DEBUG_ASSERT(*entry_start_offset < end_offset);
            }
            if (entry_end_offset) {
                *entry_end_offset = end_offset;
            }
            return NO_ERROR;
        }
    }
}

// Is this exact 8.3 name already used by an entry in the directory? Matching the
// raw name bytes rather than a reconstructed name is what makes it usable as the
// uniqueness test for a generated short name alias.
bool fat_short_name_taken(fat_fs *fat, uint32_t starting_cluster, const char short_name[11],
                          status_t *err_out) {
    LTRACEF("start_cluster %u, short_name '%.11s'\n", starting_cluster, short_name);

    DEBUG_ASSERT(fat->lock.is_held());
    DEBUG_ASSERT(err_out);

    *err_out = NO_ERROR;

    file_block_iterator dbi(fat, starting_cluster);
    status_t err = dbi.next_sectors(0);
    if (err < 0) {
        *err_out = err;
        return false;
    }

    uint32_t sector_offset = 0;
    for (;;) {
        while (sector_offset < fat->info().bytes_per_sector) {
            const uint8_t *ent = dbi.get_bcache_ptr(sector_offset);
            if (ent[0] == 0) {
                // end of the directory, so nothing past here is taken either
                return false;
            }

            if (ent[0] != 0xE5 &&
                ent[0x0B] != (uint8_t)fat_attribute::volume_id &&
                ent[0x0B] != (uint8_t)fat_attribute::lfn &&
                !memcmp(ent, short_name, 11)) {
                return true;
            }

            sector_offset += DIR_ENTRY_LENGTH;
        }

        err = dbi.next_sector();
        if (err < 0) {
            // running off the end of a fixed size root dir is not an error here
            if (err != ERR_OUT_OF_RANGE) {
                *err_out = err;
            }
            return false;
        }
        sector_offset = 0;
    }
}

} // anonymous namespace

static status_t fat_dir_is_empty(fat_fs *fat, uint32_t starting_cluster) {
    DEBUG_ASSERT(fat->lock.is_held());

    if (starting_cluster < 2 || starting_cluster >= fat->info().total_clusters) {
        return ERR_BAD_STATE;
    }

    file_block_iterator dbi(fat, starting_cluster);
    status_t err = dbi.next_sectors(0);
    if (err < 0) {
        return err;
    }

    uint32_t offset = 0;
    char *filename_buffer = fat->name_scratch();
    for (;;) {
        char *filename;
        dir_entry entry;

        err = fat_find_next_entry(fat, dbi, offset, &entry, filename_buffer, &filename);
        if (err == ERR_NOT_FOUND) {
            return NO_ERROR;
        }
        if (err < 0) {
            return err;
        }

        if (!strcmp(filename, ".") || !strcmp(filename, "..")) {
            continue;
        }

        return ERR_NOT_ALLOWED;
    }
}

// construct a short file name from the incoming name
// the sfn is padded out with spaces the same way a real FAT entry is
status_t name_to_short_file_name(char sfn[8 + 3 + 1], const char *name) {
    // zero length inputs don't fly
    if (name[0] == 0) {
        return ERR_INVALID_ARGS;
    }

    // start off with a spaced out sfn
    memset(sfn, ' ', 8 + 3);
    sfn[8 + 3] = 0;

    size_t input_pos = 0;
    size_t output_pos = 0;

    // pick out the 8 entry part
    for (auto i = 0; i < 8; i++) {
        char c = name[input_pos];
        if (c == 0) {
            break;
        } else if (c == '.') {
            output_pos = 8;
            break;
        } else {
            sfn[output_pos++] = toupper(c);
            input_pos++;
        }
    }

    // at this point input pos had better be looking at a . or a null
    if (name[input_pos] == 0) {
        return NO_ERROR;
    }
    if (name[input_pos] != '.') {
        return ERR_INVALID_ARGS;
    }
    input_pos++;

    for (auto i = 0; i < 3; i++) {
        char c = name[input_pos];
        if (c == 0) {
            break;
        } else if (c == '.') {
            // can only see '.' once
            return ERR_INVALID_ARGS;
        } else {
            sfn[output_pos++] = toupper(c);
            input_pos++;
        }
    }

    // at this point we should be looking at the end of the input string
    if (name[input_pos] != 0) {
        return ERR_INVALID_ARGS;
    }

    return NO_ERROR;
}

namespace {

void fill_short_dirent(uint8_t *ent, const char short_name[11], fat_attribute attr,
                       uint32_t starting_cluster, uint32_t size) {
    memcpy(&ent[0], short_name, 11);              // name
    ent[11] = (uint8_t)attr;                      // attribute
    ent[12] = 0;                                  // reserved
    ent[13] = 0;                                  // creation time tenth of second
    fat_write16(ent, 14, 0);                      // creation time seconds / 2
    fat_write16(ent, 16, 0);                      // creation date
    fat_write16(ent, 18, 0);                      // last accessed date
    fat_write16(ent, 20, starting_cluster >> 16); // fat cluster high
    fat_write16(ent, 22, 0);                      // modification time
    fat_write16(ent, 24, 0);                      // modification date
    fat_write16(ent, 26, starting_cluster);       // fat cluster low
    fat_write32(ent, 28, size);                   // file size
}

// given a dir entry location, open the corresponding sector and pass back a open pointer
// into the block cache.
// this code encapsulates the logic that takes into account that cluster 0 is magic in
// fat 12 and fat 16 for the root dir.
bcache_block_ref open_dirent_block(fat_fs *fat, const dir_entry_location &loc) {
    LTRACEF("fat %p, loc %u:%u\n", fat, loc.starting_dir_cluster, loc.dir_offset);

    uint32_t cluster = loc.starting_dir_cluster;
    uint32_t offset = loc.dir_offset;
    uint32_t sector;

    if (cluster == 0) {
        DEBUG_ASSERT(fat->info().fat_bits == 12 || fat->info().fat_bits == 16);
        // Special case on FAT12/16 to represent the root dir.
        DEBUG_ASSERT(offset < fat->info().root_entries * DIR_ENTRY_LENGTH);
        sector = fat->info().root_start_sector + offset / fat->info().bytes_per_sector;
    } else {
        // Walk the cluster chain if the offset exceeds the current cluster.
        const uint32_t bytes_per_cluster = fat->info().bytes_per_sector * fat->info().sectors_per_cluster;
        while (offset >= bytes_per_cluster) {
            if (is_eof_cluster(cluster)) {
                return bcache_block_ref(fat->bcache());
            }
            cluster = fat_next_cluster_in_chain(fat, cluster);
            if (is_eof_cluster(cluster)) {
                return bcache_block_ref(fat->bcache());
            }
            offset -= bytes_per_cluster;
        }
        uint32_t cluster_sector = fat_sector_for_cluster(fat, cluster);
        if (cluster_sector == 0xffffffff) {
            return bcache_block_ref(fat->bcache());
        }
        sector = cluster_sector + offset / fat->info().bytes_per_sector;
    }

    bcache_block_ref bref(fat->bcache());
    bref.get_block(sector);

    return bref;
}

status_t mark_entry_record_deleted(fat_fs *fat, uint32_t parent_cluster,
                                   uint32_t entry_start_offset,
                                   uint32_t entry_end_offset) {
    LTRACEF("cluster=%u, start=%u, end=%u\n",
            parent_cluster, entry_start_offset, entry_end_offset);

    if (entry_start_offset >= entry_end_offset ||
        (entry_start_offset % DIR_ENTRY_LENGTH) != 0 ||
        (entry_end_offset % DIR_ENTRY_LENGTH) != 0) {
        return ERR_BAD_STATE;
    }

    for (uint32_t offset = entry_start_offset; offset < entry_end_offset; offset += DIR_ENTRY_LENGTH) {
        LTRACEF("Deleting entry at offset %u\n", offset);
        dir_entry_location loc = {
            .starting_dir_cluster = parent_cluster,
            .dir_offset = offset,
        };
        bcache_block_ref bref = open_dirent_block(fat, loc);
        if (!bref.is_valid()) {
            LTRACEF("ERROR: open_dirent_block failed!\n");
            return ERR_IO;
        }

        uint8_t *ent = (uint8_t *)bref.ptr();
        ent += loc.dir_offset % fat->info().bytes_per_sector;

        // A record is a run of long name entries followed by the short name entry.
        // Deleting the wrong span would quietly destroy a neighbouring file, so
        // check the shape of what we are about to mark rather than trusting the
        // offsets we were handed.
        const bool last = (offset + DIR_ENTRY_LENGTH == entry_end_offset);
        DEBUG_ASSERT(ent[0] != 0x00 && ent[0] != 0xE5);
        DEBUG_ASSERT(last == (ent[0x0B] != (uint8_t)fat_attribute::lfn));

        ent[0] = 0xE5;
        bref.mark_dirty();
    }

    return NO_ERROR;
}

} // namespace

// Mark the record naming an object deleted: the run of long name entries plus the
// short name entry at loc. Every vnode carries the extent of its own record, so
// nothing has to scan the directory to find it again.
status_t fat_dir_remove_entry(fat_fs *fat, const dir_entry_location &loc,
                              uint32_t record_start_offset) {
    return mark_entry_record_deleted(fat, loc.starting_dir_cluster, record_start_offset,
                                     loc.dir_offset + DIR_ENTRY_LENGTH);
}

status_t fat_lookup(struct fs_vnode *dir, const char *name, struct fs_vnode **out) {
    fat_vnode *dirv = vnode_of(dir);
    fat_fs *fat = dirv->fs();

    LTRACEF("dir %p name '%s'\n", dirv, name);

    DEBUG_ASSERT(dirv->is_dir());

    AutoLock guard(fat->lock);

    dir_entry entry = {};
    uint32_t record_start_offset = 0;
    uint32_t entry_end_offset = 0;
    status_t err = fat_find_file_in_dir(fat, dirv->start_cluster(), name, &entry,
                                        &record_start_offset, &entry_end_offset);
    if (err < 0) {
        // a directory that runs out before the name turns up, however it ran out,
        // is a name that is not there
        return ERR_NOT_FOUND;
    }

    const dir_entry_location loc = {
        .starting_dir_cluster = dirv->start_cluster(),
        .dir_offset = entry_end_offset - DIR_ENTRY_LENGTH,
    };

    return fat_vnode_create(fat, entry, loc, record_start_offset, out);
}

status_t fat_mkdir(struct fs_vnode *dir, const char *name, struct fs_vnode **out) {
    fat_vnode *dirv = vnode_of(dir);
    fat_fs *fat = dirv->fs();

    LTRACEF("dir %p name '%s'\n", dirv, name);

    DEBUG_ASSERT(dirv->is_dir());

    if (fat->is_read_only()) {
        return ERR_NOT_ALLOWED;
    }

    AutoLock guard(fat->lock);

    const uint32_t parent_cluster = dirv->start_cluster();

    // ".." names the root directory as cluster 0 whatever cluster it starts at
    const uint32_t parent_cluster_for_dotdot = dirv->is_root() ? 0 : parent_cluster;

    uint32_t first_cluster = 0;
    uint32_t last_cluster = 0;
    status_t err = fat_allocate_cluster_chain(fat, 0, 1, &first_cluster, &last_cluster, true);
    if (err != NO_ERROR) {
        return err;
    }

    dir_entry_location loc = {};
    uint32_t record_start_offset = 0;
    err = fat_dir_allocate(fat, parent_cluster, name, fat_attribute::directory, first_cluster, 0,
                           &loc, &record_start_offset);
    if (err != NO_ERROR) {
        fat_free_cluster_chain(fat, first_cluster);
        return err;
    }

    // from here on an error has an entry to undo as well as the cluster. Declared
    // ahead of the block reference below so it runs after that reference is dropped.
    auto unwind = lk::make_auto_call([&]() {
        fat_dir_remove_entry(fat, loc, record_start_offset);
        fat_free_cluster_chain(fat, first_cluster);
    });

    bcache_block_ref bref(fat->bcache());
    uint32_t sector = fat_sector_for_cluster(fat, first_cluster);
    if (sector == 0xffffffff) {
        return ERR_INVALID_ARGS;
    }
    err = bref.get_block(sector);
    if (err < 0) {
        return err;
    }

    char dot_name[11];
    memset(dot_name, ' ', sizeof(dot_name));
    dot_name[0] = '.';

    char dotdot_name[11];
    memset(dotdot_name, ' ', sizeof(dotdot_name));
    dotdot_name[0] = '.';
    dotdot_name[1] = '.';

    uint8_t *block = (uint8_t *)bref.ptr();
    fill_short_dirent(block + 0 * DIR_ENTRY_LENGTH, dot_name, fat_attribute::directory,
                      first_cluster, 0);
    fill_short_dirent(block + 1 * DIR_ENTRY_LENGTH, dotdot_name, fat_attribute::directory,
                      parent_cluster_for_dotdot, 0);
    bref.mark_dirty();

    bcache_flush(fat->bcache());

    const dir_entry entry = {
        .attributes = fat_attribute::directory,
        .length = 0,
        .start_cluster = first_cluster,
    };
    err = fat_vnode_create(fat, entry, loc, record_start_offset, out);
    if (err < 0) {
        return err;
    }

    unwind.cancel();

    return NO_ERROR;
}

// shared by unlink and rmdir. The layer has already resolved and type checked the
// child and refused if anything still holds it open, so all that is left is to
// free the clusters and mark the record deleted.
static status_t remove_entry(struct fs_vnode *dir, struct fs_vnode *child) {
    fat_vnode *dirv = vnode_of(dir);
    fat_vnode *childv = vnode_of(child);
    fat_fs *fat = dirv->fs();

    if (fat->is_read_only()) {
        return ERR_NOT_ALLOWED;
    }

    AutoLock guard(fat->lock);

    const dir_entry_location &loc = childv->loc();
    DEBUG_ASSERT(loc.starting_dir_cluster == dirv->start_cluster());

    const uint32_t start_cluster = childv->start_cluster();

    if (childv->is_dir()) {
        status_t err = fat_dir_is_empty(fat, start_cluster);
        if (err < 0) {
            return err;
        }
    }

    if (start_cluster != 0) {
        if (start_cluster < 2 || start_cluster >= fat->info().total_clusters) {
            return ERR_BAD_STATE;
        }

        status_t err = fat_free_cluster_chain(fat, start_cluster);
        if (err != NO_ERROR) {
            return err;
        }
    }

    status_t err = fat_dir_remove_entry(fat, loc, childv->record_start_offset());
    if (err < 0) {
        LTRACEF("fat_dir_remove_entry failed: %d\n", err);
        return err;
    }

    bcache_flush(fat->bcache());

    return NO_ERROR;
}

status_t fat_unlink(struct fs_vnode *dir, const char *name, struct fs_vnode *child) {
    LTRACEF("dir %p name '%s'\n", vnode_of(dir), name);
    return remove_entry(dir, child);
}

status_t fat_rmdir(struct fs_vnode *dir, const char *name, struct fs_vnode *child) {
    LTRACEF("dir %p name '%s'\n", vnode_of(dir), name);
    return remove_entry(dir, child);
}

status_t fat_dir_allocate(fat_fs *fat, const uint32_t parent_cluster, const char *name,
                          const fat_attribute attr, const uint32_t starting_cluster,
                          const uint32_t size, dir_entry_location *loc,
                          uint32_t *record_start_offset) {
    LTRACEF("parent cluster %u, name '%s'\n", parent_cluster, name);

    if (fat->is_read_only()) {
        return ERR_NOT_ALLOWED;
    }

    DEBUG_ASSERT(fat->lock.is_held());

    // cluster 0 is the magic value for the fixed root dir on fat 12/16
    LTRACEF("starting dir cluster of parent dir %u\n", parent_cluster);

    // verify the file doesn't already exist
    dir_entry entry;
    status_t err = fat_find_file_in_dir(fat, parent_cluster, name, &entry, nullptr,
                                        nullptr);
    if (err >= 0) {
        // we found it, cant create a new file in its place
        return ERR_ALREADY_EXISTS;
    }

    char sfn[8 + 3 + 1];
    std::unique_ptr<uint16_t[]> lfn_ucs2;
    size_t lfn_ucs2_len = 0;
    bool needs_lfn = false;

    err = name_to_short_file_name(sfn, name);
    if (err < 0) {
        lfn_ucs2.reset(new (std::nothrow) uint16_t[kFatMaxLfnChars]);
        if (!lfn_ucs2) {
            return ERR_NO_MEMORY;
        }

        status_t lfn_err = fat_utf8_to_ucs2(name, lfn_ucs2.get(), kFatMaxLfnChars, &lfn_ucs2_len);
        if (lfn_err < 0) {
            return lfn_err;
        }

        if (lfn_ucs2_len == 0) {
            return ERR_INVALID_ARGS;
        }

        err = generate_unique_short_name_for_lfn(fat, parent_cluster, name, sfn);
        if (err < 0) {
            return err;
        }
        needs_lfn = true;
    }

    LTRACEF("short file name '%s'\n", sfn);

    size_t lfn_entry_count = needs_lfn ? ((lfn_ucs2_len + kFatLfnCharsPerEntry - 1) / kFatLfnCharsPerEntry) : 0;
    size_t total_entry_count = lfn_entry_count + 1;

    // now we have a starting cluster for the containing directory and proof that it doesn't already exist.
    // start walking to find enough contiguous free slots for [LFN entries..., SFN entry].
    uint32_t run_start_offset = 0;
    uint32_t run_len = 0;
    bool found_run = false;

    for (;;) {
        file_block_iterator dbi(fat, parent_cluster);
        err = dbi.next_sectors(0);
        if (err < 0) {
            return err;
        }

        uint32_t dir_offset = 0;
        uint32_t sector_offset = 0;
        run_len = 0;
        found_run = false;

        for (;;) {
            if (LOCAL_TRACE >= 2) {
                LTRACEF("dir sector:\n");
                hexdump8_ex(dbi.get_bcache_ptr(0), fat->info().bytes_per_sector, 0);
            }

            while (sector_offset < fat->info().bytes_per_sector) {
                LTRACEF_LEVEL(2, "looking at offset %#x\n", sector_offset);
                uint8_t *ent = dbi.get_bcache_ptr(sector_offset);

                if (ent[0] == 0xe5 || ent[0] == 0) {
                    if (run_len == 0) {
                        run_start_offset = dir_offset;
                    }
                    run_len++;
                    if (run_len >= total_entry_count) {
                        found_run = true;
                        break;
                    }
                } else {
                    run_len = 0;
                }

                dir_offset += DIR_ENTRY_LENGTH;
                sector_offset += DIR_ENTRY_LENGTH;
            }

            if (found_run) {
                break;
            }

            err = dbi.next_sector();
            if (err < 0) {
                if (err == ERR_OUT_OF_RANGE) {
                    break;
                }
                return err;
            }
            sector_offset = 0;
        }

        if (found_run) {
            break;
        }

        // Need more room.
        if (parent_cluster == 0) {
            // Root directory on FAT12/16 is fixed size and cannot grow.
            return ERR_NO_MEMORY;
        }

        uint32_t last_cluster = fat_find_last_cluster_in_chain(fat, parent_cluster);
        uint32_t new_cluster;
        uint32_t last_allocated;
        err = fat_allocate_cluster_chain(fat, last_cluster, 1, &new_cluster, &last_allocated, true);
        if (err != NO_ERROR) {
            return err;
        }
    }

    uint8_t checksum = fat_lfn_sfn_checksum(reinterpret_cast<const uint8_t *>(sfn));
    for (size_t i = 0; i < total_entry_count; i++) {
        dir_entry_location entry_loc = {
            .starting_dir_cluster = parent_cluster,
            .dir_offset = run_start_offset + static_cast<uint32_t>(i * DIR_ENTRY_LENGTH),
        };
        bcache_block_ref bref = open_dirent_block(fat, entry_loc);
        if (!bref.is_valid()) {
            return ERR_IO;
        }

        uint8_t *ent = static_cast<uint8_t *>(bref.ptr());
        ent += entry_loc.dir_offset % fat->info().bytes_per_sector;

        if (i < lfn_entry_count) {
            size_t reverse = lfn_entry_count - i;
            fill_lfn_dirent(ent, lfn_ucs2.get(), lfn_ucs2_len,
                            static_cast<uint8_t>(reverse),
                            (reverse == lfn_entry_count), checksum);
        } else {
            fill_short_dirent(ent, sfn, attr, starting_cluster, size);
        }
        bref.mark_dirty();
    }

    bcache_flush(fat->bcache());

    if (loc) {
        loc->starting_dir_cluster = parent_cluster;
        loc->dir_offset = run_start_offset + static_cast<uint32_t>(lfn_entry_count * DIR_ENTRY_LENGTH);
    }
    if (record_start_offset) {
        // the record starts at the first long name entry, or at the short name
        // entry itself when there are none
        *record_start_offset = run_start_offset;
    }

    return NO_ERROR;
}

// update the starting cluster and/or size pointer in a directory entry
status_t fat_dir_update_entry(fat_fs *fat, const dir_entry_location &loc, uint32_t starting_cluster, uint32_t size) {
    LTRACEF("fat %p, loc %u:%u, cluster %u, size %u\n", fat, loc.starting_dir_cluster, loc.dir_offset, starting_cluster, size);

    if (fat->is_read_only()) {
        return ERR_NOT_ALLOWED;
    }

    bcache_block_ref bref = open_dirent_block(fat, loc);

    DEBUG_ASSERT(bref.is_valid());

    uint8_t *ent = (uint8_t *)bref.ptr();
    ent += loc.dir_offset % fat->info().bytes_per_sector;

    fat_write32(ent, 28, size);                   // file size
    fat_write16(ent, 20, starting_cluster >> 16); // fat cluster high
    fat_write16(ent, 26, starting_cluster);       // fat cluster low

    bref.mark_dirty();

    return NO_ERROR;
}

status_t fat_opendir(struct fs_vnode *vn, dircookie **dcookie) {
    fat_vnode *dirv = vnode_of(vn);

    LTRACEF("vnode %p dircookie %p\n", dirv, dcookie);

    if (!dirv->is_dir()) {
        return ERR_NOT_DIR;
    }

    auto *cookie = new (std::nothrow) fat_dir_cookie;
    if (!cookie) {
        return ERR_NO_MEMORY;
    }

    // the layer holds a reference on the vnode for as long as this handle lives
    cookie->dir = dirv;
    cookie->index = 0;

    *dcookie = (dircookie *)cookie;

    return NO_ERROR;
}

status_t fat_readdir(dircookie *dcookie, struct dirent *ent) {
    auto *cookie = (fat_dir_cookie *)dcookie;
    fat_vnode *dirv = cookie->dir;
    fat_fs *fat = dirv->fs();

    LTRACEF("dircookie %p ent %p, current index %u\n", cookie, ent, cookie->index);

    if (!ent) {
        return ERR_INVALID_ARGS;
    }

    // make sure the cookie makes sense
    DEBUG_ASSERT((cookie->index % DIR_ENTRY_LENGTH) == 0);

    AutoLock guard(fat->lock);

    for (;;) {
        dir_entry entry;
        char *filename_buffer = fat->name_scratch();
        char *filename;

        // kick start our directory sector iterator
        LTRACEF("start cluster %u\n", dirv->start_cluster());
        file_block_iterator dbi(fat, dirv->start_cluster());

        // move it forward to our index point
        // also loads the buffer
        status_t err = dbi.next_sectors(cookie->index / fat->info().bytes_per_sector);
        if (err < 0) {
            return err;
        }

        // reset how many sectors the dbi has pushed forward so we can account properly for index shifts later
        dbi.reset_sector_inc_count();

        // pass the index in units of sector offset
        uint32_t offset = cookie->index % fat->info().bytes_per_sector;
        err = fat_find_next_entry(fat, dbi, offset, &entry, filename_buffer, &filename);
        if (err < 0) {
            return err;
        }

        // bump the index forward by extracting how much the sector iterator pushed things forward
        uint32_t index_inc = offset - (cookie->index % fat->info().bytes_per_sector);
        index_inc += dbi.get_sector_inc_count() * fat->info().bytes_per_sector;
        LTRACEF("calculated index increment %u (old index %u, offset %u, sector_inc_count %u)\n",
                index_inc, cookie->index, offset, dbi.get_sector_inc_count());
        cookie->index += index_inc;

        // "." and ".." are how a FAT subdirectory records its own and its parent's
        // starting cluster on disk. The layer flattens both lexically and no
        // filesystem in the tree reports them, so they are not part of a listing.
        if (!strcmp(filename, ".") || !strcmp(filename, "..")) {
            continue;
        }

        // copy the info into the fs layer's entry while the lock still guards the
        // scratch buffer filename points into
        strlcpy(ent->name, filename, MIN(sizeof(ent->name), MAX_FILE_NAME_LEN));

        return NO_ERROR;
    }
}

status_t fat_closedir(dircookie *dcookie) {
    auto *cookie = (fat_dir_cookie *)dcookie;

    LTRACEF("dircookie %p\n", cookie);

    delete cookie;

    return NO_ERROR;
}
