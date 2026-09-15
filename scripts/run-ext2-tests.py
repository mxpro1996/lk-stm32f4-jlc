#!/usr/bin/env python3
"""Run the ext2 filesystem tests against real disk images.

Builds the images with lib/fs/ext2/test/mkimage.py, boots the qemu test
project with one attached, and lets the in-guest ext2_tests suite verify the
content (the images carry position dependent patterns the guest can check
without any manifest). test.ext2.required=1 is passed so a misconfigured run
fails instead of silently skipping every device backed test.

The driver is read-only, so the host-side verification is total: the image
must be byte for byte identical after the run. Any difference at all is the
driver writing to a filesystem it has no business writing to.

  ./scripts/run-ext2-tests.py                   both images on arm64
  ./scripts/run-ext2-tests.py --type ext2-4k    just one
  ./scripts/run-ext2-tests.py --arch x86-64     a different architecture
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
LK_ROOT = os.path.dirname(SCRIPT_DIR)
EXT2_TEST_DIR = os.path.join(LK_ROOT, 'lib', 'fs', 'ext2', 'test')
MKIMAGE = os.path.join(EXT2_TEST_DIR, 'mkimage.py')
BOOT_TESTS = os.path.join(SCRIPT_DIR, 'run-qemu-boot-tests.py')

# What the first attached disk is called inside the guest; same table as
# run-fat-tests.py.
ARCH_DEVICE = {
    'arm': 'virtio0',
    'arm64': 'virtio0',
    'm68k': 'virtio0',
    'riscv32': 'virtio0',
    'riscv64': 'virtio0',
    'x86': 'ahci0.0',
    'x86-64': 'ahci0.0',
}


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def build_images(types, force, quiet):
    cmd = [sys.executable, MKIMAGE]
    for t in types:
        cmd += ['--type', t]
    if force:
        cmd.append('--force')
    if quiet:
        cmd.append('--quiet')
    subprocess.run(cmd, check=True)


def run_one(image_type, arch, timeout, quiet, scratch_dir):
    src = os.path.join(EXT2_TEST_DIR, f'blk.bin.{image_type}')
    if not os.path.exists(src):
        print(f"{image_type}: image {src} does not exist")
        return False

    # Work on a copy so a misbehaving driver cannot damage the pristine image.
    scratch = os.path.join(scratch_dir, f'{image_type}.img')
    shutil.copyfile(src, scratch)
    before = sha256_of(scratch)

    device = ARCH_DEVICE[arch]
    cmd = [BOOT_TESTS, '--arch', arch, '--timeout', str(timeout),
           '-A', f'test.ext2.device={device}',
           '-A', 'test.ext2.required=1',
           '-d', scratch]
    if quiet:
        cmd.append('--quiet')

    print(f"\n=== {image_type} on {arch} (device {device}) ===")
    boot_ok = subprocess.run(cmd).returncode == 0

    after = sha256_of(scratch)
    unchanged = before == after
    if not unchanged:
        print(f"  {image_type}: the image CHANGED during the run -- "
              f"a read-only driver wrote to the device")
    elif not quiet:
        print(f"  image: unmodified by the run, as a read-only driver must")

    ok = boot_ok and unchanged
    print(f"  {image_type}: {'PASS' if ok else 'FAIL'}"
          f"{'' if boot_ok else ' (guest tests failed)'}")
    return ok


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--type', action='append', dest='types',
                   choices=['ext2-1k', 'ext2-4k'],
                   help='image type to test (default: both). May be repeated.')
    p.add_argument('--arch', default='arm64', choices=sorted(ARCH_DEVICE),
                   help='architecture to run under (default: arm64)')
    p.add_argument('--timeout', type=int, default=120,
                   help='per-image QEMU timeout in seconds (default: 120)')
    p.add_argument('--force-images', action='store_true',
                   help='rebuild the disk images even if up to date')
    p.add_argument('--quiet', '-q', action='store_true')
    args = p.parse_args()

    types = args.types or ['ext2-1k', 'ext2-4k']

    print("=== building disk images ===")
    build_images(types, args.force_images, args.quiet)

    scratch_dir = tempfile.mkdtemp(prefix='lk-ext2-')
    results = {}
    try:
        for t in types:
            results[t] = run_one(t, args.arch, args.timeout, args.quiet,
                                 scratch_dir)
    finally:
        shutil.rmtree(scratch_dir, ignore_errors=True)

    print("\n" + "=" * 50)
    for t, ok in results.items():
        print(f"{'PASS' if ok else 'FAIL'}  {t}")
    print("=" * 50)

    failed = [t for t, ok in results.items() if not ok]
    if failed:
        print(f"FAILED: {', '.join(failed)}")
        return 1
    print("all ext2 image tests passed")
    return 0


if __name__ == '__main__':
    sys.exit(main())
