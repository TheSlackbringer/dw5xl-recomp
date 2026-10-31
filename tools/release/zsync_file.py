"""The .zsync file next to the AppImage (issue 044): AppImage updaters read it to fetch the AppImage it names.

Its text header ends at the first blank line; the block checksums after it describe the AppImage's bytes. Also used
by the release build, which runs the builder image's Python: keep this to the standard library of Python 3.9.
"""
import hashlib


def read_header(path):
    head = path.read_bytes().split(b'\n\n', 1)[0].decode('ascii')
    return dict(line.split(': ', 1) for line in head.splitlines())


def check(zsync, target):
    """Raises ValueError unless the .zsync names target (file name and relative URL) and matches its size and SHA-1."""
    header = read_header(zsync)
    digest = hashlib.sha1()
    with target.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b''):
            digest.update(chunk)
    expected = {'Filename': target.name, 'URL': target.name, 'Length': str(target.stat().st_size),
                'SHA-1': digest.hexdigest()}
    wrong = {key: header.get(key) for key, value in expected.items() if header.get(key) != value}
    if wrong:
        raise ValueError(f'{zsync.name} does not describe {target.name}: {wrong}')


def rename(zsync, target):
    """Moves the .zsync next to target, a renamed copy of the AppImage it describes, and names target in it.

    Only the Filename and URL lines change: the size, SHA-1 and block checksums describe the bytes, which a rename
    keeps. Returns the new path; raises ValueError if the result does not describe target."""
    head, blank, checksums = zsync.read_bytes().partition(b'\n\n')
    lines = []
    for line in head.decode('ascii').splitlines():
        key = line.split(': ', 1)[0]
        lines.append(f'{key}: {target.name}' if key in ('Filename', 'URL') else line)
    renamed = target.with_name(target.name + '.zsync')
    renamed.write_bytes('\n'.join(lines).encode('ascii') + blank + checksums)
    if renamed != zsync:
        zsync.unlink()
    check(renamed, target)
    return renamed
