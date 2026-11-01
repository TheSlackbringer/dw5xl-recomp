#!/usr/bin/env python3
"""Downloads or checks the pinned build inputs of a release builder image (issue 027 M11): every file of a pin list
(linux-tools.json, windows-tools.json: file name, URL, size, SHA-256) is fetched once, while the image is built, and
checked; release builds then take them from the image and download nothing.
  fetch_inputs.py LIST DIR          downloads every file of LIST into DIR and checks it (at image build time)
  fetch_inputs.py --check LIST DIR  the files in DIR are exactly the ones LIST pins (no network)
AppImages are made executable."""
import hashlib, json, sys, urllib.request
from pathlib import Path


def mismatch(path, item):
    """Why `path` is not the file `item` pins, or None."""
    if not path.is_file():
        return f'{path} is missing'
    data = path.read_bytes()
    if len(data) != item['size']:
        return f'{path} has {len(data)} bytes, the pin says {item["size"]}'
    if hashlib.sha256(data).hexdigest() != item['sha256']:
        return f'{path} has another SHA-256 than the pin ({item["sha256"]})'
    return None


def main(argv):
    check = argv[:1] == ['--check']
    if check:
        argv = argv[1:]
    if len(argv) != 2:
        sys.exit(__doc__)
    items, root = json.loads(Path(argv[0]).read_text()), Path(argv[1])
    for item in items:
        path = root / item['filename']
        if not check:
            path.parent.mkdir(parents=True, exist_ok=True)
            urllib.request.urlretrieve(item['url'], path)
        problem = mismatch(path, item)
        if problem:
            sys.exit(f'input check failed: {problem}')
        if path.suffix == '.AppImage':
            path.chmod(0o755)
        print(f'{"checked" if check else "fetched"} {item["filename"]} ({item["size"]} bytes, {item["sha256"][:12]})')


if __name__ == '__main__':
    main(sys.argv[1:])
