"""Known-byte and bounded-allocation contracts for diagnostic file snapshots."""
import hashlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import unittest

TOOL = Path(__file__).resolve().parents[1] / 'debug/validate_function_map.py'
spec = importlib.util.spec_from_file_location('snapshot_validator', TOOL)
validator = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = validator
spec.loader.exec_module(validator)


class SnapshotReads(unittest.TestCase):
    def test_small_file_large_allowance_never_requests_huge_buffer(self):
        payload = b'original snapshot\x00\xff' * 500
        requests = []

        class Stream(io.BytesIO):
            def read(self, count=-1):
                requests.append(count)
                if count < 0 or count > 1024 * 1024:
                    raise AssertionError('Small-file snapshot requests an oversized read buffer')
                return super().read(count)

        class Input:
            name = 'synthetic.bin'
            opens = 0

            def open(self, mode):
                self.opens += 1
                self.asserted_mode = mode
                self.stream = Stream(payload)
                return self.stream

        path = Input()
        self.assertEqual(validator.snapshot(path, 512 << 20), payload)
        self.assertEqual(path.opens, 1)
        self.assertEqual(path.asserted_mode, 'rb')
        self.assertTrue(path.stream.closed)
        self.assertTrue(requests)

    def test_empty_exact_limit_and_oversize_keep_known_bytes(self):
        with tempfile.TemporaryDirectory(prefix='snapshot-bytes-') as temp:
            path = Path(temp) / 'known.bin'
            for size in (0, 1, 65535, 65536, 65537, 131089):
                payload = (bytes(range(256)) * ((size + 255) // 256))[:size]
                path.write_bytes(payload)
                for limit in (size, size + 1):
                    with self.subTest(size=size, limit=limit):
                        actual = validator.snapshot(path, limit)
                        self.assertEqual(actual, payload)
                        self.assertEqual(hashlib.sha256(actual).digest(), hashlib.sha256(payload).digest())
                if size:
                    with self.subTest(size=size, oversize=True):
                        with self.assertRaises(validator.Invalid) as result:
                            validator.snapshot(path, size - 1)
                        self.assertEqual(result.exception.code, 'LIMIT_INPUT')
                self.assertEqual(path.read_bytes(), payload)

    def test_oversize_reads_stop_at_existing_one_byte_lookahead(self):
        class Stream(io.BytesIO):
            def close(self):
                self.consumed = self.tell()
                super().close()
        class Input:
            name = 'oversize.bin'
            def open(self, mode):
                self.stream = Stream(b'A' * 400000)
                return self.stream
        path = Input()
        with self.assertRaises(validator.Invalid) as result:
            validator.snapshot(path, 70000)
        self.assertEqual(result.exception.code, 'LIMIT_INPUT')
        self.assertEqual(path.stream.consumed, 70001)
        self.assertTrue(path.stream.closed)

    def test_missing_file_keeps_input_read_error(self):
        with tempfile.TemporaryDirectory(prefix='snapshot-missing-') as temp:
            with self.assertRaises(validator.Invalid) as result:
                validator.snapshot(Path(temp) / 'missing.bin', 1024)
            self.assertEqual(result.exception.code, 'INPUT_READ')


if __name__ == '__main__':
    unittest.main()
