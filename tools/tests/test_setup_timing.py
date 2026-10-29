"""042: Release translator timing must describe real phases on synthetic input."""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
RECOMP = Path(os.environ.get('PS2X_TEST_RECOMPILER', str(ROOT / 'build-recomp/ps2xRecomp/ps2_recomp')))
PREFIX = '[setup-timing] '


class SetupTiming(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix='setup-timing-')
        self.addCleanup(self.scratch.cleanup)
        self.root = Path(self.scratch.name)
        self.assertTrue(RECOMP.is_file(), 'Build/set PS2X_TEST_RECOMPILER; no automatic download')
        (self.root / 'input.bin').write_bytes(struct.pack('<4I', 0x03e00008, 0, 0x03e00008, 0))
        (self.root / 'functions.csv').write_text('Name,Start,End,Size\na,0x1000,0x1008,8\nb,0x1008,0x1010,8\n')
        self.profile = self.root / 'profile.toml'
        self.profile.write_text('[general]\ninput_format="raw"\nmodule_id="timing_fixture"\nraw_base=4096\n' +
            ''.join(f'{key}={json.dumps(str(self.root / name))}\n' for key, name in
                    [('input', 'input.bin'), ('ghidra_output', 'functions.csv'), ('output', 'generated')]))

    def run_translator(self, *options):
        return subprocess.run([str(RECOMP), str(self.profile), *options], capture_output=True,
                              encoding='utf-8', timeout=60)

    def events(self, result):
        return [json.loads(line[len(PREFIX):]) for line in result.stdout.splitlines() if line.startswith(PREFIX)]

    def test_completed_release_breaks_down_translation(self):
        result = self.run_translator('--timing-run', 'fixture-run', '--timing-profile', 'main')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        events = self.events(result)
        self.assertTrue(events, 'Release translator provides no stage measurements')
        begins = {e['id']: e for e in events if e['event'] == 'begin'}
        ends = {e['id']: e for e in events if e['event'] == 'end'}
        self.assertEqual(begins.keys(), ends.keys())
        required = {'translator', 'initialize', 'config', 'input_map', 'analysis',
                    'entry_discovery', 'continuation_discovery', 'output'}
        self.assertTrue(required <= {e['stage'] for e in ends.values()}, str(ends))
        for identity, end in ends.items():
            begin = begins[identity]
            self.assertEqual(end['result'], 'success')
            self.assertEqual(end['run'], 'fixture-run')
            self.assertEqual(end['profile'], 'main')
            self.assertEqual(end['schema'], 1)
            self.assertEqual(end['clock'], 'process-steady')
            self.assertEqual(end['duration_us'], end['at_us'] - begin['at_us'])
            self.assertGreaterEqual(end['duration_us'], 0)
            if end['parent']:
                self.assertLessEqual(begins[end['parent']]['at_us'], begin['at_us'])
                self.assertGreaterEqual(ends[end['parent']]['at_us'], end['at_us'])
        output = next(e for e in ends.values() if e['stage'] == 'output')
        self.assertEqual(output['counts']['functions'], 2)
        self.assertEqual(output['counts']['function_files'], 2)
        self.assertGreater(output['counts']['function_bytes'], 0)

    def test_timing_does_not_change_generated_bytes(self):
        result = self.run_translator('--no-timing')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(self.events(result))
        def inventory():
            return {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in (self.root / 'generated').glob('*') if p.is_file()}
        baseline = inventory()
        result = self.run_translator('--timing-run', 'second-run')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(inventory(), baseline)

    def test_failure_is_not_a_completed_interval(self):
        (self.root / 'input.bin').unlink()
        result = self.run_translator('--timing-run', 'failed-run')
        self.assertNotEqual(result.returncode, 0)
        events = self.events(result)
        self.assertTrue(events, 'Failure has no timing record')
        terminal = [e for e in events if e['event'] == 'end' and e['stage'] == 'translator']
        self.assertEqual(len(terminal), 1)
        self.assertEqual(terminal[0]['result'], 'failed')
        self.assertNotIn('output', {e['stage'] for e in events})


if __name__ == '__main__':
    unittest.main()
