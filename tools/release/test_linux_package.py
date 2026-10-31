"""Linux release content guards use synthetic files only."""
import configparser
import fnmatch
from pathlib import Path
import tempfile
import unittest

import make_release
from test_zsync_file import write_zsync
import update_draft


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


class LinuxNamesTest(unittest.TestCase):
    """Issue 044: the AppImage carries the project's name and no "linux"; the menu entry names the project."""

    def test_package_name(self):
        self.assertEqual(make_release.linux_package_name('v0.1.0-beta.6'), 'DW5XL-RECOMP-v0.1.0-beta.6-x86_64')

    def test_draft_rename_keeps_the_build_name_form(self):
        with tempfile.TemporaryDirectory() as scratch:
            built = Path(scratch) / (make_release.linux_package_name('123abcd') + '.AppImage')
            built.write_bytes(b'synthetic Linux builder')
            write_zsync(Path(str(built) + '.zsync'), built.name, built.read_bytes())
            target = update_draft.package_for_version(built, 'v0.1.0-beta.6')[0]
            self.assertEqual(target.name, make_release.linux_package_name('v0.1.0-beta.6') + '.AppImage')

    def test_update_information_finds_the_published_zsync(self):
        # AppImageUpdate matches '*' + the last field against the release's asset names.
        kind, user, repo, release, pattern = make_release.UPDATE_INFORMATION.split('|')
        self.assertEqual((kind, user, repo, release), ('gh-releases-zsync', 'TheSlackbringer', 'dw5xl-recomp', 'latest'))
        published = make_release.linux_package_name('v0.1.0-beta.6') + '.AppImage.zsync'
        self.assertTrue(fnmatch.fnmatchcase(published, '*' + pattern))
        for other in ('DW5XL-v0.1.0-beta.6-windows-x64.zip', published[:-len('.zsync')], published + '.sha256'):
            self.assertFalse(fnmatch.fnmatchcase(other, '*' + pattern), other)

    def test_desktop_entry_names_the_project(self):
        parser = configparser.ConfigParser(interpolation=None)
        parser.optionxform = str
        parser.read(Path(__file__).with_name('dw5xl.desktop'), encoding='utf-8')
        entry = parser['Desktop Entry']
        self.assertEqual(entry['Name'], 'DW5XL Recomp')
        self.assertIn('Dynasty Warriors 5 Xtreme Legends', entry['Comment'])
        self.assertEqual((entry['Exec'], entry['Icon'], entry['Categories']),
                         ('dw5xl_setup', 'dw5xl', 'Game;ActionGame;'))


if __name__ == '__main__':
    unittest.main()
