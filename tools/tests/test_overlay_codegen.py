#!/usr/bin/env python3
"""Game-independent CLI regression checks for separately generated raw modules."""
import os
import pathlib
import struct
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
RECOMP = pathlib.Path(os.environ.get('PS2X_TEST_RECOMPILER', str(ROOT / ('external/PS2Recomp/build/ps2xRecomp/ps2_recomp' + ('.exe' if os.name == 'nt' else '')))))

class RawModules(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix='dw5xl-raw-module-test-')
        self.work = pathlib.Path(self.scratch.name)
        # Synthetic MIPS return, not bytes copied from a game.
        (self.work / 'input.bin').write_bytes(struct.pack('<II', 0x03E00008, 0))
        (self.work / 'functions.csv').write_text('Name,Start,End,Size\nreturn_test,0x1000,0x1008,8\n')

    def tearDown(self):
        self.scratch.cleanup()

    def generate(self, identity, offset=0, end=0x1008, function_map=None):
        (self.work / 'functions.csv').write_text(function_map or f'Name,Start,End,Size\nreturn_test,0x1000,{end:#x},{end-0x1000}\n')
        output = self.work / identity
        config = self.work / 'input.toml'
        config.write_text(f'''[general]
input = "{self.work.as_posix()}/input.bin"
input_format = "raw"
module_id = "{identity}"
raw_base = 0x1000
raw_code_offset = {offset}
ghidra_output = "{self.work.as_posix()}/functions.csv"
output = "{output.as_posix()}"
''')
        result = subprocess.run([str(RECOMP), str(config)], capture_output=True, text=True)
        return result, output

    def test_same_address_modules_have_distinct_tables_and_no_bootstrap(self):
        for identity in ('module_a', 'module_b'):
            result, output = self.generate(identity)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            table = (output / 'register_functions.cpp').read_text()
            self.assertIn(f'g_ps2Module_{identity}', table)
            self.assertIn(f'{identity}_return_test_', table)
            self.assertNotIn('g_ps2RecompiledFunctionTable', table)
            self.assertFalse(any('startup' in p.name for p in output.iterdir()))

    def test_explicit_raw_entries_survive_containing_named_body(self):
        (self.work / 'input.bin').write_bytes(struct.pack('<IIII', 0x03E00008, 0, 0x03E00008, 0))
        result, output = self.generate('nested_entries', function_map=
            'Name,Start,End,Size\nnamed_outer,0x1000,0x1010,16\nFUN_00001008,0x1008,0x1010,8\n')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        table = (output / 'register_functions.cpp').read_text()
        self.assertIn('{0x1008u, nested_entries_FUN_00001008_', table)

    def test_invalid_offset_is_rejected(self):
        result, _ = self.generate('bad_offset', offset=12)
        self.assertNotEqual(result.returncode, 0)

    def test_out_of_file_body_is_rejected(self):
        result, _ = self.generate('bad_body', end=0x100C)
        self.assertNotEqual(result.returncode, 0)

    def test_invalid_identifier_is_rejected(self):
        result, _ = self.generate('bad-name')
        self.assertNotEqual(result.returncode, 0)

if __name__ == '__main__':
    unittest.main()
