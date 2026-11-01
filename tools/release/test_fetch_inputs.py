#!/usr/bin/env python3
"""fetch_inputs.py (issue 027 M11): pinned inputs are fetched and checked by size and SHA-256 (offline: file://
URLs), a mismatch or a missing file is refused, and both pin lists are well formed."""
import hashlib, json, subprocess, sys, tempfile, unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE / 'fetch_inputs.py'


def run(*args):
    return subprocess.run([sys.executable, str(SCRIPT), *map(str, args)], capture_output=True, text=True)


class FetchInputs(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.source = self.tmp / 'source.bin'
        self.source.write_bytes(b'pinned input\n' * 100)
        data = self.source.read_bytes()
        self.item = {'filename': 'sub/input.bin', 'url': self.source.as_uri(), 'size': len(data),
                     'sha256': hashlib.sha256(data).hexdigest()}

    def pins(self, **change):
        path = self.tmp / 'pins.json'
        path.write_text(json.dumps([dict(self.item, **change)]))
        return path

    def test_fetch_then_check(self):
        out = self.tmp / 'inputs'
        self.assertEqual(run(self.pins(), out).returncode, 0)
        self.assertEqual((out / 'sub/input.bin').read_bytes(), self.source.read_bytes())
        self.assertEqual(run('--check', self.pins(), out).returncode, 0)

    def test_wrong_size_refused(self):
        result = run(self.pins(size=self.item['size'] + 1), self.tmp / 'inputs')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('bytes', result.stderr)

    def test_wrong_checksum_refused(self):
        result = run(self.pins(sha256='0' * 64), self.tmp / 'inputs')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('SHA-256', result.stderr)

    def test_check_finds_a_missing_file(self):
        result = run('--check', self.pins(), self.tmp / 'empty')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('missing', result.stderr)

    def test_pin_lists_are_well_formed(self):
        for name in ('linux-tools.json', 'windows-tools.json'):
            items = json.loads((HERE / name).read_text())
            self.assertTrue(items, name)
            for item in items:
                self.assertEqual(set(item), {'filename', 'url', 'size', 'sha256'}, name)
                self.assertTrue(item['url'].startswith('https://'), item['url'])
                self.assertGreater(item['size'], 0)
                self.assertRegex(item['sha256'], r'^[0-9a-f]{64}$')
                self.assertFalse(item['filename'].startswith('/') or '..' in item['filename'])


if __name__ == '__main__':
    unittest.main()
