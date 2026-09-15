#!/usr/bin/env python3
"""Build the ext2 test disk images.

The in-guest driver is read-only, so unlike the FAT images these carry no
consumable content: the whole contract is that the guest can read every file
byte for byte and must not modify the image at all (run-ext2-tests.py verifies
the second half by hashing the image before and after the run).

Two images differing only in block size, because the indirect block fan-out --
and therefore which file offsets take the single/double indirect paths -- is a
function of the block size. The generated pattern files use the same position
dependent byte formula as the guest test, so the guest verifies content without
the image embedding any expectations.

Requires mke2fs and e2fsck (e2fsprogs 1.43+ for mke2fs -d).

  ./mkimage.py                  build every image that is out of date
  ./mkimage.py --type ext2-1k   just one
  ./mkimage.py --force          rebuild even if up to date
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

# block_size is the ext2 block size; size_kb the image size.
IMAGES = {
    'ext2-1k': dict(block_size=1024, size_kb=24 * 1024, label='EXT2TEST1K'),
    'ext2-4k': dict(block_size=4096, size_kb=24 * 1024, label='EXT2TEST4K'),
}

# Generated pattern payloads: name -> (seed, size). Keep in sync with the
# expectations in ext2_tests.c.
#
# pattern_large is sized to need double indirect blocks at either block size:
# with 1KB blocks the double indirect region starts at 12KB + 256KB, with 4KB
# blocks at 48KB + 4MB. 6MB is comfortably past both.
GENERATED = {
    'pattern_small.bin': (0x1111, 4000),
    'pattern_large.bin': (0x2222, 6 * 1024 * 1024),
}

HELLO_CONTENT = b'hello ext2\n'
DEEP_CONTENT = b'deep file\n'

# A directory whose name pushes the symlink target over 60 bytes, so the
# symlink is stored in data blocks instead of inline in the inode.
LONG_DIR = 'a_directory_with_a_name_long_enough_to_defeat_inline_symlinks'


def pattern_bytes(size, seed):
    """Position dependent byte pattern; must match pattern_byte() in
    ext2_tests.c (which reuses the formula from the FAT ram tests)."""
    out = bytearray(size)
    for i in range(size):
        x = (seed ^ ((i * 2654435761) & 0xFFFFFFFF)) & 0xFFFFFFFF
        x ^= x >> 15
        x = (x * 2246822519) & 0xFFFFFFFF
        x ^= x >> 13
        out[i] = x & 0xFF
    return bytes(out)


def require_tools():
    missing = [t for t in ('mke2fs', 'e2fsck') if shutil.which(t) is None]
    if missing:
        sys.exit(f"missing required tools: {', '.join(missing)}\n"
                 f"install e2fsprogs")


def run(cmd, quiet, **kwargs):
    if not quiet:
        print('  ' + ' '.join(str(c) for c in cmd))
    subprocess.run(cmd, check=True, capture_output=quiet, **kwargs)


def build_tree(root):
    """The content tree every image carries. Returns nothing; the guest test
    knows this layout."""
    def put(relpath, content):
        path = os.path.join(root, relpath)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'wb') as f:
            f.write(content)

    put('hello.txt', HELLO_CONTENT)
    put('dir1/dir2/dir3/deep.txt', DEEP_CONTENT)
    for name, (seed, size) in GENERATED.items():
        put(name, pattern_bytes(size, seed))
    put(f'{LONG_DIR}/target.txt', HELLO_CONTENT)

    links = os.path.join(root, 'links')
    os.makedirs(links)
    # relative, one level up
    os.symlink('../hello.txt', os.path.join(links, 'rel_link'))
    # relative, several components deep
    os.symlink('../dir1/dir2/dir3/deep.txt', os.path.join(links, 'rel_deep'))
    # absolute. The fs layer resolves these against the mount namespace root
    # rather than the filesystem's own root, so this one names nothing unless
    # the namespace happens to have a /dir1/dir2/dir3/deep.txt of its own --
    # which is exactly what the guest test arranges.
    os.symlink('/dir1/dir2/dir3/deep.txt', os.path.join(links, 'abs_link'))
    # a link to a directory, so it can be walked through as an intermediate
    # path component rather than resolved as the last one
    os.symlink('../dir1', os.path.join(links, 'dirlink'))
    # a chain that stays inside the layer's symlink depth limit
    os.symlink('chain2', os.path.join(links, 'chain1'))
    os.symlink('chain3', os.path.join(links, 'chain2'))
    os.symlink('../hello.txt', os.path.join(links, 'chain3'))
    # a loop, which must fail cleanly rather than hang
    os.symlink('loop2', os.path.join(links, 'loop1'))
    os.symlink('loop1', os.path.join(links, 'loop2'))
    # a target long enough (>60 bytes) to be stored in blocks rather than
    # inline in the inode. Relative, so it resolves inside the mount.
    target = f'../{LONG_DIR}/target.txt'
    assert len(target) > 60, target
    os.symlink(target, os.path.join(links, 'long_link'))


def image_is_current(img_path):
    return (os.path.exists(img_path) and
            os.path.getmtime(os.path.abspath(__file__)) <= os.path.getmtime(img_path))


def build_image(name, spec, out_dir, quiet, force):
    img_path = os.path.join(out_dir, f'blk.bin.{name}')

    if not force and image_is_current(img_path):
        if not quiet:
            print(f"{name}: up to date")
        return

    print(f"building {name} ({spec['size_kb']} KB, "
          f"{spec['block_size']} byte blocks)")

    if os.path.exists(img_path):
        os.unlink(img_path)

    with tempfile.TemporaryDirectory(prefix='lk-ext2-tree-') as tree:
        build_tree(tree)
        run(['mke2fs',
             '-q',
             '-t', 'ext2',
             '-b', str(spec['block_size']),
             '-L', spec['label'],
             '-d', tree,
             img_path,
             f"{spec['size_kb']}k"], quiet)

    # -n: verify only. A freshly built image that fsck complains about is a
    # tooling problem, better caught here than as guest test failures.
    run(['e2fsck', '-fn', img_path], quiet)


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--type', action='append', dest='types',
                   help='image type to build (default: all). May be repeated.')
    p.add_argument('--out-dir', default=HERE,
                   help='where to write images (default: this directory)')
    p.add_argument('--force', action='store_true', help='rebuild even if up to date')
    p.add_argument('--quiet', '-q', action='store_true')
    args = p.parse_args()

    require_tools()

    types = args.types or list(IMAGES)
    for t in types:
        if t not in IMAGES:
            sys.exit(f"unknown image type '{t}' (choose from {', '.join(IMAGES)})")

    os.makedirs(args.out_dir, exist_ok=True)
    for t in types:
        build_image(t, IMAGES[t], args.out_dir, args.quiet, args.force)

    return 0


if __name__ == '__main__':
    sys.exit(main())
