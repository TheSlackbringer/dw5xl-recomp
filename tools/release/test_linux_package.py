"""Linux release content guards use synthetic files only."""
from pathlib import Path
import tempfile
import unittest

import make_release


class LinuxContentTest(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        self.stage = Path(self.scratch.name)

    def test_compiled_game_is_refused_under_an_extensionless_linux_name(self):
        (self.stage / 'renamed-program').write_bytes(b'\x7fELF\x02' + make_release.GAME_MARKER)
        with self.assertRaisesRegex(SystemExit, 'compiled game'):
            make_release.scan(self.stage)

    def test_ordinary_native_tool_is_allowed(self):
        (self.stage / 'launcher').write_bytes(b'\x7fELF\x02synthetic launcher')
        make_release.scan(self.stage)

    def test_all_translated_program_directories_are_refused(self):
        for unit in ('vu0', 'vu1', 'iop'):
            with self.subTest(unit=unit):
                path = self.stage / 'recomp' / 'dw5xl' / unit / 'generated.cpp'
                path.parent.mkdir(parents=True)
                path.write_text('synthetic translated input')
                with self.assertRaises(SystemExit):
                    make_release.scan(self.stage)
                path.unlink()


if __name__ == '__main__':
    unittest.main()
