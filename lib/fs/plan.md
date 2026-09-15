# lib/fs: moving the path walk into the fs layer

Status: complete, 2026-08-24. All seven phases are implemented on this branch:
the pre-fixes and test expansion of §6 Phase 0, the node tree / vnode
interface / walk of Phase 1, the memfs conversion plus the
FS_NODE_CACHE_SIZE LRU of Phase 2, the ext2 conversion of Phase 3, the
spifs conversion of Phase 4, the FAT conversion of Phase 5 and the 9p
conversion of Phase 6 and the cleanup of Phase 7. Every filesystem in the
tree is on the vnode interface and the legacy string-path interface is gone.
Sections below describe the design as proposed; deviations that emerged during
implementation:

- the dirhandle holds the directory's *vnode* (plus the fs cursor), not the
  fs_node; nothing pins node chains for open files or dirs, the vnode does
- unlink evicts a cached name node rather than detaching it live; the layer's
  ERR_BUSY check makes detach-while-open unreachable in the first version
- vnode-interface components attach as children of the mount point node, so
  a mount has no separate root node; `mounted->root` is a vnode
- the node cache is runtime-resizable (fs_set_node_cache_size) on top of the
  FS_NODE_CACHE_SIZE build default
- readdir does not report "." or "..": the layer flattens both lexically and
  no filesystem in the tree lists them, so this is the cross-filesystem `ls`
  contract, not an ext2 detail
- the symlink depth limit is the layer's FS_MAX_SYMLINK_DEPTH (8), replacing
  ext2's private limit of 4. A spliced path must fit FS_MAX_PATH_LEN (128),
  where the old ext2 walker allowed targets up to 512 bytes; over that the
  walk returns ERR_NOT_ENOUGH_BUFFER
- a path *through* a name that does not exist on a flat filesystem is now
  ERR_NOT_FOUND, not ERR_NOT_SUPPORTED: the walk fails on the missing
  component before the filesystem is asked anything. Only an op the
  filesystem does not implement at all (mkdir on spifs) still gives
  ERR_NOT_SUPPORTED
- a mount root can be opened with fs_open_file() and stat'd: it reports
  is_dir with a zero size rather than an error. memfs already behaved this
  way; spifs now matches, and docs/fs.md should state it as the contract
- namespace ops run under the global fs_lock, so a create on spifs now holds
  it across a flash erase and a ToC commit, as ext2's lookup holds it across
  bcache I/O. That is what §4's locking decision buys; revisit only if a real
  target shows contention

- a vnode id of 0 means "no identity" to the layer, so a filesystem whose
  natural identity can legitimately be zero has to shift it into a nonzero
  encoding. FAT identifies an object by the location of its short name entry,
  and 0:0 is the first entry of a FAT12/16 root directory, so it sets the high
  bit of the id. Without that the file in that one slot gets a vnode per open,
  which means no shared length and no working busy check
- FAT's readdir no longer reports "." and "..", which brings it in line with
  the listing contract above. They remain on disk, and remain resolvable as
  path components, because the layer flattens both lexically
- **§7 "vnode priv vs embedded" is resolved in favour of priv**: the layer
  allocates the vnode, so embedding would mean handing allocation to the
  filesystem for every object. FAT, the case the question was raised for, hangs
  its per-object state off priv and frees it in release() exactly as ext2 does
- **§5's "hash on dir_entry_location" for FAT is moot**: the layer's per-mount
  vnode list *is* the open object table, and it deduplicates by id, so FAT's
  own open file table and the linear lookup_file scan through it were deleted
  rather than made faster
- FAT unlink now matches a name the same way lookup does, through the
  reconstructed long name. Removing a file by the short name alias FAT
  generated for it no longer works, which is the same rule as opening it
- no filesystem implements rename yet. The op exists in fs_api, but there is no
  public fs_rename for anything to reach it through, so wiring one up would be
  unreachable code; FAT is the natural first implementation when the public
  call arrives

- 9p keeps **two fids per vnode**, not one. 9P2000 says a walk may not start
  from a fid that has been opened for I/O, so the fid an object was walked to
  is never opened and the I/O fid is cloned off it on first use. qemu turns
  out to permit the walk anyway -- a probe against it answers Rwalk, not
  Rlerror -- so this is conformance with the protocol rather than a
  workaround for the one server LK talks to, and it costs one clone and one
  clunk per object that is actually read or enumerated
- Tremove clunks the fid it is handed whether or not the removal succeeds, so
  9p removes through a *clone* of the child's walk fid. Using the child's own
  fid would work only when the removal succeeds; a refused rmdir of a
  non-empty directory would leave the surviving vnode holding a fid the
  server had already dropped, and everything through that directory would
  fail until the node cache aged it out
- 9p's per-handle 4KB write-back page buffer is deleted rather than moved
  onto the vnode. Shared per object it would have to be flushed from
  release(), which the layer defers behind its node cache, so a write could
  sit unsent indefinitely -- and a reopen would read it back through the
  buffer without the server ever seeing it. Writes are write-through now;
  bulk transfers are chunked at PAGE_SIZE either way, so only repeated
  sub-4KB access to one page pays for it
- 9p reports a host symlink as a plain file. The layer would follow a
  FS_VNODE_SYMLINK through readlink(), and the transport implements neither
  Treadlink nor Tsymlink, so claiming the type would break the walk
- **walk RPC amplification, measured against qemu**: a cold five component
  open costs six Twalks (one per component plus the I/O clone) where the old
  whole-path Twalk cost one; a warm one costs zero, because the node cache
  answers the path and the vnode still holds its opened fid. The optional
  multi-component lookup §6 leaves open is not worth an fs_api change that
  touches every filesystem for a cost only the first open pays
- 9p's fids come from a bitmap over a fixed range (512) rather than a
  counter, so they are recycled; a mount that exhausts them fails the lookup
  with ERR_NO_RESOURCES rather than reusing a live one. Recycling makes it
  matter *why* a request failed: a reply of any kind, Rlerror included,
  proves the server processed it and released the fid, but a transport
  timeout leaves that unknown, so such a number is abandoned rather than
  handed out again into a possible collision
- 9p sizes each read and write by the iounit the server reports when the fid
  is opened, falling back to PAGE_SIZE when it reports none. qemu answers
  about 124KB for a file and zero for a directory, so a large transfer is one
  round trip instead of one per page -- and on a 64K page target the chunk
  stops being larger than what the server asked for
- qid.path is assumed unique across the share, which is what dedup keys on.
  It is not, for a share that spans devices (qemu's multidevs=), where two
  files on different devices would collapse into one vnode
- §5 costed the 9p conversion at +150 bytes; it measures −192 (5,255 → 5,063
  text, arm64 DEBUG=0), and a vnode is ~24 bytes of heap where an open file
  used to cost more than 4KB. The core is untouched by this phase: fs.c is
  byte-identical, so the §1 budget stood where Phase 3 left it until the
  cleanup below reclaimed part of it

- deleting the legacy adapter took find_mount with it: with no legacy mount to
  enter, a walk to a mounted path returns a node and find_mount could only
  return NULL, so mount()'s ERR_ALREADY_MOUNTED precheck was dead. Occupied
  paths are caught by the scaffold walk instead, which means the rejection now
  happens after bio_open and api->mount rather than before. That was already
  true for every vnode filesystem from Phase 1 onward; only the wording of the
  check changed
- unmount() is a required op rather than an optional one. It was always called
  without a NULL check on the teardown paths; only the root-type rejection in
  mount() pretended otherwise
- §7's FS_CAP_UNLINK_OPEN is **resolved as not built**: ERR_BUSY is uniform
  across all five filesystems and nothing in the tree wants POSIX
  unlink-while-open. The flag is the design if something ever does, and
  docs/fs.md says so. FS_NODE_CACHE_SIZE is the only build variable the layer
  grew
- §1 budgeted +3KB of thumb2 core. Deleting the adapter reclaims 636 bytes:
  fs.c.o measures 4,007 against the 2,293 baseline, so the layer costs
  **+1,714 bytes** for the node tree, the walk, mount stitching, refcounting
  and the node cache

## 1. Problem and constraints

Today `lib/fs` strips the mount prefix off a normalized path and hands the entire
remainder to the filesystem (`fs.c:89` `find_mount`, then `api->open(cookie, "/a/b/c")`).
Every filesystem therefore carries its own component splitter, its own notion of a
root, and its own "walk to the parent, then act on the last element" helper — FAT has
three copies of that helper, ext2 has a 104-line recursive walker with symlink
handling, and the flat filesystems (spifs, memfs) fake a root with
`strcmp("", trim_name(path))`. The fs layer has no object for a file or directory,
so there is nothing to refcount, nothing for two opens of one file to share, and no
place to hang a mount point: mount points are matched by string prefix, and "ls /"
works only because of the 120-line `rootfs_*` special case in `fs.c:460-578`.

Constraints that shape the design:

- **Embedded size.** The whole stack is built into the stm32f7xx family
  (`LK_EMBEDDED := 1`, 256KB RAM) via `project/virtual/fs.mk`, and `app/moot` boots
  from spifs on dartuinoP0. Baseline thumb2 `DEBUG=0` text sizes from
  `build-dartuinoP0-test-release`:

  | module            | text (bytes) | notes                                        |
  |-------------------|-------------:|----------------------------------------------|
  | `lib/fs` `fs.c.o` |        2,293 | all of the layer: mount list, walk, rootfs    |
  | `lib/fs` `shell.c.o` |     2,288 | always linked, has a cwd                     |
  | `lib/fs/memfs`    |        1,123 |                                              |
  | `lib/fs/ext2`     |        1,837 | read-only, no readdir                        |
  | `lib/fs/spifs`    |        3,112 |                                              |
  | `lib/fs/fat`      |       13,952 |                                              |
  | whole `lk.elf`    |      344,176 |                                              |

  (arm64 `DEBUG=0` for scale: fs 8.8K, spifs 5.8K, fat 24K, ext2 3.2K, memfs 2.1K, 9p 5.2K.)

  The budget proposed below is **+3KB thumb2 for the core**, with the filesystems
  roughly size-neutral, i.e. under 1% of that image. The design has to let an
  embedded build opt out of *caching* entirely (only objects on the path to an open
  handle stay alive) while still getting the shared walk.

- **Public API stability.** The `fs_*` functions in `lib/fs.h` have a very small
  consumer set and none of them care how the walk is done: `lib/libc/stdio.c`
  (open/create/read/write/stat/close), `lib/uefi/uefi.cpp` (open/read/close),
  `app/moot/fsboot.c` (open/stat/`FS_IOCTL_GET_FILE_ADDR`/close),
  `target/dartuinoP0/init.c` (mount), plus `lib/fs/shell.c` and `lib/fs/debug.c`.
  The refactor is therefore internal to `lib/fs` and its implementations; the
  `fs_*` entry points stay, `struct fs_api` is what changes.

- **Small filesystems must stay small.** A flat filesystem should be expressible as
  "one root directory vnode plus lookup-by-name in it", with no directory support
  and no extra bookkeeping beyond a refcount.

## 2. What each filesystem does today

The survey below is what determines the shape of the interface. File:line
references are to the current tree.

### spifs (`spifs.c`, 1,242 lines)

- Flat namespace, names stored inline in the 32-byte on-disk ToC entry
  (`toc_file_t`, 20 chars). Per-mount `list_node files` sorted by `page_idx`, with
  two sentinel pseudo-files `front-toc`/`back-toc` at head and tail that the
  allocator, readdir and ToC commit all depend on.
- Path code is about 15 lines: `trim_name` + `find_file` (`spifs.c:167`), a `/`
  check that only `create` performs (`spifs.c:723`), and the `strcmp("", name)`
  root test in `opendir` (`spifs.c:1076`).
- **No refcount.** `open` hands back the `spifs_file_t *` itself and `close` is a
  no-op; `remove` frees the object with no check for open handles
  (`spifs.c:872`). A read after remove is a use-after-free. Open *dir* cursors are
  fixed up (`spifs.c:861`), so the author knew about the class of problem.
- Stable identity: `page_idx` (no compaction, no rename). ToC slot index is not
  stable (`spifs_commit_toc` re-serializes the list).
- One mutex per mount, necessarily coarse: a single scratch page is shared by all I/O.
- Tests: 15 cases run under `ut all` on two device flavours. Not covered: same
  file opened twice, remove while open, any `/` in a name.

### memfs (`memfs.c`, 438 lines)

- Flat list of `{name, ptr, len}`, identity is the name string. `mkdir` is
  `#if 0`'d, `stat` hardcodes `is_dir = false`.
- Same ~12 lines of path code and the same missing refcount as spifs: `remove` frees
  with an `// XXX make sure there are no open file handles` (`memfs.c:214`), and the
  `dcookies` list exists but nothing walks it to fix dangling cursors.
- Only filesystem the generic `lib/fs/test` suite runs against — and
  `lib/fs/test/rules.mk` does not list `lib/fs/memfs` as a dep, so
  `project/armemu-test.mk` (fs + ext2 only) builds a suite that cannot mount.

### ext2 (825 lines of .c)

- Read-only. `ext2_api` (`ext2.c:251`) has six entries: mount, unmount, open, stat,
  read, close. **No opendir/readdir**, so `ls` on an ext2 mount is
  `ERR_NOT_SUPPORTED`, and the root directory is unreachable through any API (the
  walker reduces `"/"` to a zero-length lookup).
- `ext2_walk` (`dir.c:75-178`) is 104 lines: in-place splitting, two 128-byte
  inodes by value on the stack, 512-byte `path` and `link` stack buffers, symlink
  following with a nesting limit of 4 and a `goto nextcomponent` re-dispatch.
  `ext2_dir_lookup` (`dir.c:18-72`) already has the shape `(dir_inode, name) -> inum`.
- `ext2_file_t` is a by-value inode copy that does **not record its inode number**.
  No inode cache (only a 4-block unlocked bcache), no locking anywhere.
- Mount error paths leak (`ext2.c:118-158` `return err` instead of `goto err`);
  `options != 0` is rejected so a read-only fs refuses `FS_MOUNT_OPTION_READ_ONLY`.
- **No tests at all**, no image, no script.

### FAT (~4,500 lines of C++)

- `fat_dir_walk` (`dir.cpp:740-826`) plus `split_path`,
  `resolve_parent_cluster_and_last_element` and two inlined copies of it in
  `mkdir` (`dir.cpp:1082`) and `fat_dir_allocate` (`dir.cpp:1306`): ~150 lines of
  path handling on top of ~400 lines of directory-entry scanning (LFN assembly,
  short-name matching) that stays regardless. The parent-resolve copies use
  `char local_path[FS_MAX_FILE_LEN + 1]` (65 bytes) for a 128-byte path, so
  create/mkdir/remove/rmdir silently truncate long paths.
- **Already has a vnode cache over open objects**: `fat_file` carries `int ref_`,
  lives on `fat_fs::file_list_`, and `lookup_file(dir_entry_location)`
  (`fs.cpp:74`) dedups so two opens share one object. It is a linear list and only
  covers open files.
- Identity: `dir_entry_location {starting_dir_cluster, dir_offset}` (64 bits,
  names the SFN slot). First cluster is unusable (0 for every empty file). The
  root has no dirent and is represented three ways (FAT32 `root_cluster`, magic
  cluster 0 on FAT12/16, synthetic `{1,0}` in `opendir`), and `mkdir` must write
  `..` = 0 when the parent is root even on FAT32.
- Hazards a single vnode type removes: `opendir` `reinterpret_cast<fat_dir *>` on
  whatever `lookup_file` returns (`dir.cpp:1593`, `fat_file::open_file_priv`
  accepts directories); `create_file` skips `lookup_file`.
- Case-insensitive matching (`strnicmp`, `dir.cpp:549`). Unlink of an open file is
  `ERR_BUSY` (`check_entry_not_busy`); FAT has no link count so unlink-while-open
  cannot be implemented anyway.
- One mutex per mount held across every entry point; the walk nests so two scratch
  name buffers live on the mount.
- Tests: the RAM tier (`ut fat_ram`, 9 geometries, nested dirs, remount, truncate)
  runs on every arch in CI; `ut fat` against images via `scripts/run-fat-tests.py`;
  `ut fat_stress`. `test_fat_split_path` tests a function the refactor deletes.

### 9p (~1,090 lines)

- Hierarchical and stateless on the client: one persistent root fid, and every
  open/create/opendir/mkdir re-walks from it with one multi-component `TWALK`
  (`path_to_wname`, `v9fs.c:39`, up to 16 components per RPC).
- **There is no walk-by-qid in the protocol.** `TWALK` takes `fid + names`. A
  "materialize a vnode from an id with no context" call cannot be implemented
  without the fs keeping its own qid-to-path map.
- One fid per open handle, a never-recycling 32-bit fid counter (`v9fs.c:67`),
  `put_fid` asserts on clunk failure, `unmount` frees nothing (root fid leaked),
  `remove`/`rmdir` not registered although the transport already speaks `TREMOVE`.
- Bugs the survey found: root-level create/mkdir send `"/name"` to the server
  (`file.c:156`, `dir.c:158`); `v9fs_stat_file` returns with the file mutex held on
  RPC failure (`file.c:485`); the 4KB page buffer is bypassed without flush for
  accesses over 4KB (`file.c:392`). `v9fs->lock` guards only the fid counter; the
  handle lists are unlocked.
- Tests: two console commands, not unit tests, one open of one root file.

### fs layer (`fs.c`, 866 lines)

- `fs_normalize_path` (130 lines, well tested) does `.`/`..`/`//` lexically.
- `find_mount` prefix-matches a flat mount list. Nested mounts are rejected.
  Exact match returns the literal `"/"` with a `TODO: decide if this is necessary`.
- Intrinsic rootfs synthesizes dirents for mount points (`fs.c:460-578`) including
  live-iterator fixup on unmount.
- readdir EOF is `ERR_NOT_FOUND` in memfs/rootfs, `ERR_OUT_OF_RANGE` in 9p; the
  layer passes either through.
- `fs_load_file` has no callers; `debug.c:24` declares a nonexistent
  `fs_mount_type`; `debug.c:78` passes `(void **)&is_mapped` for a `bool`.

### Summary table

| fs    | namespace | path code | refcount        | stable id                   | can `get_vnode(id)`? | root object | tests            |
|-------|-----------|----------:|-----------------|-----------------------------|----------------------|-------------|------------------|
| spifs | flat      |   ~15 ln  | none (UAF)      | `page_idx`                  | yes (list scan)      | none        | ut, 15 cases     |
| memfs | flat      |   ~12 ln  | none (UAF)      | none today                  | trivially            | none        | generic suite    |
| ext2  | tree+links|  ~112 ln  | none            | inode number                | yes                  | `root_inode`| **none**         |
| FAT   | tree      |  ~150 ln  | yes, open only  | `dir_entry_location`        | yes (~30 ln)         | 3 encodings | ut ram/image/stress |
| 9p    | tree      |   ~60 ln  | none            | qid (identity only)         | **no**               | root fid    | console cmd only |

## 3. Which model

### A. BeOS/NewOS: vnode cache keyed by id, fs does `lookup(dir, name) -> id`

The layer keeps a hash of `(mount, vnode_id) -> vnode`. Walking calls
`lookup(dir_vnode, name, &id)` and then `get_vnode(id)` (cache hit or fs
materializes it). A name cache is an optional accelerator in front of that. Mounts
are stitched by `covered_by` on the mount-point vnode. NewOS `kernel/vfs.c` is
2,741 lines; Haiku's descendant `fs_interface.h` is ~100 ops, which is the
direction this grows if left alone.

What the survey says about it here:

- Requires every fs to implement `get_vnode(id)` with no parent context. 9p cannot
  (no walk-by-qid). FAT can, but the name is not recoverable from the id, and
  **rename changes the id** (it is a dirent location), so an id-keyed cache has to
  be told to rekey. spifs can only by linear scan.
- The mount point is a vnode with no name, so there is still no way to print a path
  or enumerate mount points without a second structure — the rootfs special case
  survives.
- On the plus side it is the smaller core: no name storage, no child lists.

### B. Linux-style: the fs layer owns a named directory tree, vnodes hang off it

Every node the layer has touched is a `dentry`-like object `{parent, name,
children, vnode, refcount, mounted}`. The walk is a child-list search per
component; a miss calls `lookup(dir_vnode, name) -> vnode` and inserts. Mounts are
a pointer from the mount-point node to the mounted root node, so mount points are
ordinary named nodes, "ls /" is a readdir of a node with no backing vnode, and
nested mounts fall out for free.

- Only `lookup(parent, name)` is ever needed from the fs — every filesystem in the
  tree can do that, and it is exactly `TWALK` with one name for 9p and
  `fat_find_file_in_dir` / `ext2_dir_lookup` for the others.
- Rename is a node move; the id is never used for lookup, only for dedup.
- The cost is per-node name storage and the child list. That is also where the
  embedded knob lives: on `LK_EMBEDDED` the tree is pruned the moment a node's
  refcount hits zero, so the resident tree is exactly the set of nodes on the path
  to something open — the same memory footprint the BeOS model would have with its
  name cache off.

### Recommendation: B, with LK-sized simplifications

Keep an optional per-vnode id purely for deduplication (so a case-mismatched FAT
lookup or an ext2 hard link does not produce two vnodes for one object), and do not
have a `get_vnode(id)` op at all. Skip negative entries, hard-link alias lists,
per-fs name-compare hooks and a dentry LRU in the first version; all of them can be
added later without changing the fs-facing ops.

## 4. Proposed design

### Objects

```c
// owned by lib/fs, one per name the layer has resolved
struct fs_node {
    struct list_node node;          // in parent->children
    struct fs_node *parent;         // NULL for the namespace root
    struct list_node children;
    char *name;                     // single component, heap, NULL for root
    int ref;                        // open handles + mounted + child nodes (+ cache)
    struct fs_vnode *vnode;         // NULL for a virtual directory (mount scaffold)
    struct fs_mount *mounted;       // non-NULL if a filesystem root is mounted here
};

// owned by lib/fs, allocated on behalf of the fs in lookup/create/mkdir
struct fs_vnode {
    struct fs_mount *mount;
    uint64_t id;                    // optional, 0 = no dedup
    int ref;                        // held by fs_node plus anything the fs layer
                                    // hands out (open filehandles)
    uint8_t type;                   // FS_VNODE_FILE / DIR / SYMLINK
    void *priv;                     // fs private
};
```

`fs_node` and `fs_vnode` are 1:1 in the first version (no aliases). A `filehandle`
holds a `fs_vnode *` (not an `fs_node *`), so unlink can detach the node while the
vnode lives on until the last close. A `dirhandle` holds the node, because readdir
on a mount scaffold directory enumerates children, and readdir on a real directory
needs a per-handle cursor from the fs.

### Filesystem-facing ops

```c
struct fs_api {
    // volume
    status_t (*format)(bdev_t *, const void *args);
    status_t (*mount)(bdev_t *, enum fs_mount_options, fscookie **, fs_vnode **root);
    status_t (*unmount)(fscookie *);
    status_t (*fs_stat)(fscookie *, struct fs_stat *);

    // namespace: always one component, always relative to a directory vnode
    status_t (*lookup)(fs_vnode *dir, const char *name, fs_vnode **out);
    status_t (*create)(fs_vnode *dir, const char *name, uint64_t len, fs_vnode **out);
    status_t (*mkdir)(fs_vnode *dir, const char *name, fs_vnode **out);
    status_t (*unlink)(fs_vnode *dir, const char *name, fs_vnode *child);
    status_t (*rmdir)(fs_vnode *dir, const char *name, fs_vnode *child);
    status_t (*rename)(fs_vnode *olddir, const char *oldname, fs_vnode *child,
                       fs_vnode *newdir, const char *newname);    // optional
    ssize_t  (*readlink)(fs_vnode *link, char *buf, size_t len);  // optional
    void     (*release)(fs_vnode *);                               // last ref dropped

    // file, on the vnode (LK read/write are positional, so no per-open state)
    ssize_t  (*read)(fs_vnode *, void *, off_t, size_t);
    ssize_t  (*write)(fs_vnode *, const void *, off_t, size_t);
    status_t (*truncate)(fs_vnode *, uint64_t);
    status_t (*stat)(fs_vnode *, struct file_stat *);
    status_t (*ioctl)(fs_vnode *, int, void *);

    // directory enumeration needs a cursor, so it keeps a cookie
    status_t (*opendir)(fs_vnode *, dircookie **);
    status_t (*readdir)(dircookie *, struct dirent *);
    status_t (*closedir)(dircookie *);
};
```

Points worth calling out:

- The fs allocates `priv` and fills `id`/`type` through a helper
  (`fs_vnode_create(mount, id, type, priv, &vn)`); the layer frees the `fs_vnode`
  after `release()`.
- `open`/`close` per handle disappear for files. FAT's `open_file`/`close_file`
  refcount becomes the vnode refcount; 9p's open fid becomes a lazily opened fid on
  the vnode (which also fixes the per-handle page-buffer incoherence).
- `unlink`/`rmdir` receive the child vnode so the fs can refuse (`ERR_BUSY`) or mark
  it orphaned. **The layer itself returns `ERR_BUSY` if the child has open handles**
  in the first version — that is FAT's semantics today, it makes spifs/memfs
  correct without any deferred-free machinery, and it keeps the embedded
  filesystems from needing an orphan state. POSIX unlink-while-open can be enabled
  per fs later with a capability flag.
- A flat filesystem implements `mount` (returning a root dir vnode), `lookup`
  (valid only when `dir` is the root), `create`, `unlink`, `release`, the file ops
  and the dir ops. No `mkdir`/`rmdir`/`rename`/`readlink`. That is the spifs and
  memfs surface.
- `readdir` EOF becomes one value (`ERR_NOT_FOUND`, matching `fs_read_dir`'s
  current documented behaviour in the rootfs and memfs) enforced by the layer.
- `struct dirent` could grow a `type` field while the struct is being touched
  (`ls` currently opens every entry to find out if it is a directory).

### The walk

`fs_walk(const char *path, struct fs_node **out, const char **last, int flags)`:

1. Copy and `fs_normalize_path()` as today — `..` stays lexical, which is what the
   shell's `cd` and every consumer already assume.
2. Start at the namespace root node; for each component, search `children` by
   `strcmp`; on a miss, if the node has no vnode (scaffold dir) fail with
   `ERR_NOT_FOUND`, otherwise call `lookup(node->vnode, comp)`, dedup by
   `(mount, id)` if the fs supplied an id, and insert a new child.
3. If the node has `mounted`, step to `mounted->root` before continuing.
4. With `FS_WALK_PARENT`, stop one component early and return the remaining
   component in `*last` — this is what create/mkdir/unlink/rmdir/rename use, and it
   replaces FAT's three parent-resolve helpers and 9p's two.
5. Symlinks (ext2 only): if `readlink` is present and the vnode is a symlink,
   splice the target and continue, depth-limited. Absolute targets restart at the
   namespace root, not the fs root — a semantic change from `ext2_walk`, and the
   right one.

`FS_MAX_PATH_LEN`/`FS_MAX_FILE_LEN` remain the only limits; the per-fs name limit
(spifs 19 chars) is enforced by the fs in `create`.

### Mounting

`fs_mount(path, ...)` walks to the target, creating scaffold nodes for missing
intermediate components (this is what lets `mount /mnt/foo` work without a
filesystem at `/` today, and it replaces the rootfs code entirely), calls the fs
`mount` to get a root vnode, and sets `node->mounted`. Unmount requires the mount's
vnode refcount to be exactly the root's, otherwise `ERR_BUSY`, and prunes any
scaffold nodes left childless. `fs_dump_mounts` and `df` walk the tree.

### Lifetime and caching

- `fs_node::ref` is held by each child, by an open `dirhandle`, and by `mounted`.
  `fs_vnode::ref` is held by its node and by each open `filehandle`.
- When a node's ref drops to zero it is either pruned immediately (calls
  `release()` on the vnode if it is the last ref) or kept on an LRU list capped at
  `FS_NODE_CACHE_SIZE`. Default: `0` when `LK_EMBEDDED`, something like 64
  otherwise. This is the single knob that decides whether the layer is a cache or
  just a walk.
- `unlink`: walk to parent, `ERR_BUSY` if the child vnode has handle refs, call the
  fs, then detach the node and drop its ref. `rename`: detach, rename the node,
  attach under the new parent.
- Cache validity across a remove-then-create on the same id (FAT slot reuse, spifs
  page reuse) is handled by detaching on unlink, so a stale cached vnode can never
  be found by name; dedup by id only applies to nodes still attached.

### Locking

One global `fs_lock` mutex protecting the tree, both refcounts and the mount
list, replacing `mount_lock`. It is held across `lookup()` calls in the first
version — every in-tree fs already serializes on a per-mount mutex, and the 9p
transport allows one outstanding RPC, so this costs nothing today. It is **not**
held across read/write/truncate/stat/readdir, which go straight to the vnode.
The NewOS `busy` flag trick (mark the node, drop the lock around the fs call,
wake waiters) is the upgrade path if walking ever needs to overlap I/O.

### Legacy adapter (what makes this incremental)

Keep the current `struct fs_api` as `struct fs_legacy_api`. A legacy mount's root
node is a leaf as far as the walk is concerned: when the walk reaches it with
components remaining, the layer reassembles the remainder as a string and calls the
old `open/create/remove/mkdir/opendir` exactly as `fs.c` does now. `filehandle`
gains a union for the legacy cookie. This means the core can land and be tested
against memfs before any other filesystem changes, each filesystem converts in its
own commit, and the adapter plus `trim_name` are deleted at the end.

## 5. Per-filesystem conversion cost

| fs    | delete                                                     | add                                                                                   | net  | risk |
|-------|------------------------------------------------------------|---------------------------------------------------------------------------------------|-----:|------|
| memfs | all path code, `dcookies` fixup                            | real directory nodes (child lists), `mkdir`/`rmdir`, ids, refcount via `release`      | ~0   | low; it is the reference implementation and the test vehicle |
| spifs | `trim_name`, `/` checks, `opendir` root test (~15 ln)       | root dir vnode; `lookup` = `find_file` minus sentinels; `release`; ensure sentinels are never returned by lookup | +30 | low; the list order and sentinel invariants are untouched |
| ext2  | `ext2_walk`, `ext2_lookup`, the 1KB of stack buffers (~112 ln) | vnode with inode number + inode; `lookup` from `ext2_dir_lookup`; `readlink` from `ext2_read_link`; **`readdir` (new, ~100 ln)**; fix mount leaks | +100 | low; read-only, but there are no tests, so write them first |
| FAT   | `fat_dir_walk`, `split_path`, three parent-resolve copies, `element_scratch_`, `opendir` root special case, `reinterpret_cast` (~200 ln) | single vnode type replacing `fat_file`/`fat_dir`, hash on `dir_entry_location`, root vnode `{1,0}` with `is_root`, cache the dirent extent `[start,end)` so unlink stops rescanning | −50 | medium; largest, but has the best tests and already has the refcounted table |
| 9p    | `path_to_wname`, both parent/leaf splits, dead code (~60 ln) | fid-per-vnode with a recycling fid allocator, non-asserting clunk, real locking, unmount teardown, optional multi-component `lookup` fast path | +150 | medium; correctness is easy, the risk is RPC amplification on a cold tree and cache staleness against a live host directory |
| core  | `find_mount` prefix matching, `rootfs_*` (~200 ln)          | node tree, walk, mount stitching, refcounts, LRU, legacy adapter (~500-600 ln)        | +400 | the `+3KB` thumb2 budget is measured here |

Bugs the survey found that are worth fixing on master ahead of, and independent
of, the refactor (each is a one-commit change and several are crashes):

- spifs and memfs: `remove` while a file handle is open frees the object under the
  handle. Short-term fix: refuse with `ERR_BUSY` via an open count, which is also
  the semantics the new layer will enforce.
- 9p: `"/name"` sent on root-level create/mkdir; `v9fs_stat_file` returns with the
  mutex held; `unmount` leaks the root fid and every handle.
- ext2: mount error paths leak; `FS_MOUNT_OPTION_READ_ONLY` rejected on a
  read-only fs; `stat` leaves `capacity` uninitialized.
- FAT: 65-byte path buffers truncate paths over 64 chars in
  mkdir/remove/rmdir/create.
- `lib/fs/debug.c:78` `(void **)&is_mapped` on a `bool`; stale `fs_mount_type`
  extern; dead `fs_load_file`.
- `lib/fs/test/rules.mk` needs `lib/fs/memfs`.

## 6. Phases

Each phase is a separately landable commit series, and each ends with
`scripts/buildall -q -e -d`, `run-qemu-boot-tests.py --arch arm64` (and x86-64,
riscv64), `run-fat-tests.py`, and a thumb2 size check against the baseline table in
§1 on `dartuinoP0-test DEBUG=0`.

**Phase 0 — tests and pre-fixes (no interface change).**
Fix the bugs above. Extend `lib/fs/test`: open-twice, remove-while-open (expect
`ERR_BUSY`), nested `mkdir`/`opendir`/`readdir`, `stat` on a directory,
`ERR_NOT_FOUND` vs `ERR_NOT_SUPPORTED` on flat filesystems, unmount with open
handles. Add an ext2 image test modelled on `run-fat-tests.py` (`mke2fs -d` a tree
with nested dirs, a file large enough for double-indirect blocks, and a symlink
chain pinning the depth semantics). Convert the 9p console command to a `ut` case
gated on the device being present, as the FAT image tests do.

**Phase 1 — core.** Implement `fs_node`/`fs_vnode`, the walk, mount stitching and
the legacy adapter in `fs.c`, with every existing filesystem running through the
adapter. Delete `rootfs_*`. Nothing in any fs changes; the rootfs tests and
`test_rootfs_live_iter` become tests of the scaffold directories. Measure size.

**Phase 2 — memfs.** Rewrite as a tree-backed RAM fs on the new api. Becomes the
vehicle for the rest of the generic suite (nested dirs, rename, readdir on
directories with many entries, node cache eviction with `FS_NODE_CACHE_SIZE`
forced to 0 and to a small number).

**Phase 3 — ext2.** Convert, add `readdir`, move symlink resolution up. Validated
by the Phase 0 image test.

**Phase 4 — spifs.** Convert. The existing 15 `ut` cases plus the new
double-open/remove-while-open cases cover it; check `app/moot` still boots on
dartuinoP0 (the `FS_IOCTL_GET_FILE_ADDR` path) and re-measure thumb2 size — this
is the configuration the budget is for.

**Phase 5 — FAT.** Convert; unify `fat_file`/`fat_dir`; replace the linear
`lookup_file` with a hash; merge the two `fat_find_file_in_dir*` scanners and cache
the dirent extent. Drop `test_fat_split_path`. Add the non-root mkdir/remove cases
`fat/plan.md` already wants. Consider adding `rename` here since the layer now
supports it and FAT is the fs people actually want it on.

**Phase 6 — 9p.** Convert with fid-per-vnode and a recycling fid allocator; wire
up `unlink`/`rmdir` via `TREMOVE`. Measure walk RPC counts on a cold and warm tree;
if amplification matters, add the optional multi-component `lookup` the walk can
use on a run of cache misses.

**Phase 7 — cleanup.** Delete the legacy adapter and `trim_name` (`fs_load_file`
went in Phase 0). Make `FS_NODE_CACHE_SIZE` and any per-fs capability flags
documented build variables. Write `docs/fs.md` (there is currently no fs
documentation in `docs/`).

## 7. Open questions

- **Should the shell's cwd become a node reference rather than a string?** A held
  `fs_node *` would pin the directory and make `cd` validate its target, but it
  also keeps a mount busy. Keeping the string is simpler and matches today.
- **Negative entries.** Cheap to add and they speed up repeated failed opens
  (stdio `fopen` probing), but they need invalidation on create/rename and are
  wrong for 9p against a live host directory. Leave out until there is a measured
  need.
- **Per-fs name compare.** Without it a FAT file looked up as `FOO` and later as
  `foo` misses the node cache on the second name (dedup by id still guarantees one
  vnode). Acceptable; a `compare` op can be added to `fs_api` later.
- **`fs_vnode` embedded vs pointed-to `priv`.** `priv` keeps the C++ FAT code and
  the C filesystems symmetric and lets the layer own allocation; embedding
  (`containerof`) saves one allocation per node. The FAT conversion is the place
  to decide, since it already has a class hierarchy to fit in.
- **Unlink-while-open.** `ERR_BUSY` everywhere is the proposal, and it is what
  shipped. If POSIX semantics are wanted for memfs/ext2-rw later, a
  `FS_CAP_UNLINK_OPEN` flag on the api lets those filesystems opt in without
  changing spifs or FAT. Not built: nothing in the tree wants it yet.
