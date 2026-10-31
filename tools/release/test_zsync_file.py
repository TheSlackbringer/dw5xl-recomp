"""The .zsync header checks (issue 044) on synthetic files; no zsyncmake needed."""
import hashlib
from pathlib import Path
import tempfile
import unittest

import zsync_file


def write_zsync(path, name, data, url=None):
    header = (f'zsync: 0.6.2\nFilename: {name}\nMTime: Sat, 31 Oct 2026 08:00:00 +0000\nBlocksize: 2048\n'
              f'Length: {len(data)}\nHash-Lengths: 2,2,4\nURL: {url or name}\nSHA-1: {hashlib.sha1(data).hexdigest()}\n\n')
    path.write_bytes(header.encode() + b'\x00\xffsynthetic block checksums\n\n')


class ZsyncFileTest(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.appimage = Path(scratch.name) / 'DW5XL-RECOMP-123abcd-x86_64.AppImage'
        self.appimage.write_bytes(b'synthetic AppImage')
        self.zsync = Path(str(self.appimage) + '.zsync')

    def test_header_is_read_up_to_the_checksums(self):
        write_zsync(self.zsync, self.appimage.name, self.appimage.read_bytes())
        header = zsync_file.read_header(self.zsync)
        self.assertEqual(header['URL'], self.appimage.name)
        self.assertEqual(header['Blocksize'], '2048')

    def test_matching_zsync_passes(self):
        write_zsync(self.zsync, self.appimage.name, self.appimage.read_bytes())
        zsync_file.check(self.zsync, self.appimage)

    def test_other_name_size_or_content_is_refused(self):
        data = self.appimage.read_bytes()
        for case, name, content, url in (('name', 'DW5XL-old-x86_64.AppImage', data, None),
                                         ('url', self.appimage.name, data, 'elsewhere.AppImage'),
                                         ('size', self.appimage.name, data + b'!', None),
                                         ('content', self.appimage.name, data.upper(), None)):
            with self.subTest(case=case):
                write_zsync(self.zsync, name, content, url)
                with self.assertRaises(ValueError):
                    zsync_file.check(self.zsync, self.appimage)


if __name__ == '__main__':
    unittest.main()
