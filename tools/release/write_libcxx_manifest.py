#!/usr/bin/env python3
"""Writes or checks /approved-debs/manifest.json (issue 041): package, version, file name, size and SHA-256 of
every libc++ .deb in the builder image.
  write_libcxx_manifest.py DIR          writes DIR/manifest.json (at image build time)
  write_libcxx_manifest.py --check DIR  the .debs in DIR are exactly the ones in its manifest (at release build time)
"""
import hashlib, json, subprocess, sys
from pathlib import Path

EXPECTED = {'libc++-23-dev', 'libc++1', 'libc++abi-23-dev', 'libc++abi1', 'libunwind-23-dev', 'llvm-libunwind1'}


def describe(path):
    field = lambda name: subprocess.run(['dpkg-deb', '-f', str(path), name], check=True, capture_output=True,
                                        text=True).stdout.strip()
    data = path.read_bytes()
    return {'Package': field('Package'), 'Version': field('Version'), 'Filename': path.name, 'Size': len(data),
            'SHA256': hashlib.sha256(data).hexdigest()}


def main():
    check = sys.argv[1:2] == ['--check']
    root = Path(sys.argv[-1])
    found = sorted((describe(p) for p in root.glob('*.deb')), key=lambda d: d['Package'])
    if {d['Package'] for d in found} != EXPECTED or len(found) != len(EXPECTED):
        raise SystemExit(f'libc++ packages in {root}: {[d["Package"] for d in found]}, expected {sorted(EXPECTED)}')
    if len({d['Version'] for d in found}) != 1:
        raise SystemExit(f'libc++ packages of different versions in {root}')
    manifest = root / 'manifest.json'
    if not check:
        manifest.write_text(json.dumps(found, indent=2) + '\n')
        return
    if json.loads(manifest.read_text()) != found:
        raise SystemExit(f'libc++ packages in {root} do not match {manifest}')
    print(f'libc++ packages match the image manifest: {found[0]["Version"]}')


main()
