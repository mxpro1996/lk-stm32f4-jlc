# FAT Work - To Do

This file tracks the remaining tasks and improvements for the FAT filesystem implementation.

## Functional Gaps

- **Timestamps** (`fat_priv.h`, `dir.cpp`): Implement proper creation, access, and modification time handling. Currently, these are written as zero and ignored during `stat`.
- **Attribute Validation** (`file.cpp`): Tighten `open_file_priv` to reject entries with `volume_id` or other special attributes that shouldn't be opened as regular files.
- **Non-zero length file creation** (`file.cpp`): `create_file` returns `ERR_NOT_IMPLEMENTED` for any non-zero initial length, so `mkfile <path> <len>` from the shell does not work.

## Performance Improvements

- **FSInfo Free Cluster Hint** (`fat.cpp`): Use the `fsinfo_next_free` hint during cluster allocation to avoid linear scans from the beginning of the FAT on large volumes. `fat_fs::fs_stat` already prefers the FSInfo free count when it is valid, but allocation still scans from cluster 2.

## Code Quality & Refactoring

- **Cluster Extension Consolidation**: Share the cluster allocation logic between directory growth (`fat_dir_allocate`) and file growth/truncate (`file.cpp`).

## Testing & Validation

- **Edge Case Tests** still wanted:
  - Invalid name handling: reserved DOS names, trailing dots and spaces, characters
    illegal in a short name, paths at and beyond `FS_MAX_PATH_LEN`/`FS_MAX_FILE_LEN`.
  - Verification of `.` and `..` *content* (the cluster numbers they point at) via
    direct directory reads. A listing no longer shows them at all, so this is now
    the only way to check them.
  - Concurrency: several threads on one mount, and deliberate contention on a
    single file against the refcounted open file table.

## Test Infrastructure

The tests come in two tiers:

- `lib/fs/fat/test/ram_tests.cpp` and `stress.cpp` format their own volumes with
  `fs_format_device()` in a heap buffer, so they need no disk image and no host
  tooling and run on every architecture in CI. `ram_tests.cpp` sweeps a geometry
  matrix pinned to exact cluster counts either side of both FAT type thresholds;
  `stress.cpp` is gated behind `test.fat.stress=1`.
- `lib/fs/fat/test/test.cpp` runs against a real image built by `mkimage.py` and
  driven by `scripts/run-fat-tests.py`, which verifies the result from the host
  with `fsck.fat` and `mtools`.

Every test cleans up after itself so `ut fat` is re-runnable in place. The one
exception is `test_fat_witness`, which leaves a documented tree for the host
verifier; keep it in step with `WITNESS_FILES` in `run-fat-tests.py`.

## Recently Completed

- Converted to the fs layer's vnode interface (`lib/fs/plan.md` Phase 5): one
  `fat_vnode` type replaces `fat_file`/`fat_dir`, the layer owns object lifetime
  and identity, and `fat_dir_walk`, `split_path`, the three parent-resolve copies
  and the open file table are gone. Each vnode caches the extent of its own
  directory record, so unlink no longer rescans the parent. Non-root
  mkdir/remove and the FAT12/16 root-first-entry identity case are covered by
  new `ram_tests.cpp` cases.
- `fs_unmount` with files or directories still open: the layer refuses it, so
  the driver no longer has an open file list to assert on.
- Mount-time device size validation, plus a check that the metadata fits inside
  `total_sectors` (`fs.cpp`).
- FAT type selection compared `total_clusters` (= data clusters + 2) against the
  spec's thresholds, so volumes with 4083-4084 or 65523-65524 data clusters got
  the wrong FAT width (`fs.cpp`).
- The path walk returned a `dir_entry_location` one entry past the short name
  entry, so writing to any file opened by path updated the wrong directory slot,
  lost the size and leaked clusters (`dir.cpp`).
- `format` and `fs_stat` vtable hooks (`format.cpp`, `fs.cpp`).
- LFN record boundary conditions and reuse of deleted slots
  (`test_fat_stress_dir_slot_reuse`).
