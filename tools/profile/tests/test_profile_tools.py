#!/usr/bin/env python3
"""Fast unit tests for the profiling tools (no recording, no admin rights): marker files, names, the Windows converter
and the report, on made-up data (tests/synthetic.py). Not part of CI (a debugging tool); run when changing the tools:
python3 -m unittest discover -s tools/profile/tests"""
import json, os, struct, sys, tempfile, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
import common, etw_samples, report, synthetic

FIXTURE_EXE = synthetic.FIXTURE + '.exe'


class Markers(unittest.TestCase):
    def test_roundtrip(self):
        header = common.MARKER_HEADER.pack(b'DW5XLPM1', 1, 32, 2, 5, 100, 1000, 10_000_000, 900, 9000, 10_000_000)
        recs = common.MARKER_RECORD.pack(10, 1, 7, 0x10, 99, 4, 0) + common.MARKER_RECORD.pack(20, 2, 7, 0, 0, 100, 3)
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, 'm.bin')
            with open(path, 'wb') as f:
                f.write(header + recs)
            h, r = common.read_markers(path)
        self.assertEqual(h['count'], 2)
        self.assertEqual(h['dropped'], 5)
        self.assertEqual(r[0], {'t_ns': 10, 'seq': 1, 'tid': 7, 'program': 0x10, 'vu1_cycles': 99, 'event': 4, 'kind': 0})
        self.assertEqual(r[1]['event'], 100)
        self.assertEqual(common.MARKER_HEADER.size, 80)
        self.assertEqual(common.MARKER_RECORD.size, 32)

    def test_bad_magic(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, 'm.bin')
            with open(path, 'wb') as f:
                f.write(b'X' * 80)
            self.assertEqual(common.read_markers(path), (None, []))

    def test_offset(self):
        anchors = [{'steady_ns': 1_000_000_500, 'os_ticks': 10_000_000, 'os_freq': 10_000_000}]
        self.assertEqual(common.steady_offset_ns(anchors), 500)


class Names(unittest.TestCase):
    def test_normalize(self):
        self.assertEqual(report.normalize("`anonymous namespace'::fixture_hot_spin"), 'fixture_hot_spin')
        self.assertEqual(report.normalize('(anonymous namespace)::f(int, char)'), 'f')
        self.assertEqual(report.normalize('vl::fmacOutOfLine<(vl::FmacOp)2, 15>(float vector[4])'),
                         'vl::fmacOutOfLine<(vl::FmacOp)2, 15>')
        self.assertEqual(report.normalize('lift_da28_0010(vl::Run&)'), 'lift_da28_0010')

    def test_image_base(self):
        # a minimal PE32+ header: "MZ", e_lfanew -> "PE\0\0", optional header magic 0x20B, ImageBase
        head = bytearray(512)
        head[0:2] = b'MZ'
        struct.pack_into('<I', head, 0x3C, 0x80)
        head[0x80:0x84] = b'PE\0\0'
        struct.pack_into('<H', head, 0x80 + 24, 0x20B)
        struct.pack_into('<Q', head, 0x80 + 24 + 24, 0x140000000)
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, 'a.exe')
            with open(path, 'wb') as f:
                f.write(head)
            self.assertEqual(common.pe_image_base(path), 0x140000000)
            with open(path, 'wb') as f:
                f.write(b'\x7fELF' + bytes(60))
            self.assertIsNone(common.pe_image_base(path))

    def test_windows_module_path(self):
        # a Windows capture read on another OS: the module's file name still comes from a C:\ path
        sym = report.Symbols([{'pid': 1, 'base': 0x1000, 'end': 0x2000, 'path': 'C:\\x\\game.exe'}])
        sym.resolve_all({(1, 0x1800)})
        self.assertEqual(sym.label(1, 0x1800), '<game.exe>')


class Convert(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        events, cls.anchors, cls.truth, markers = synthetic.etw_text()
        ev_path = os.path.join(cls.tmp.name, 'events.txt')
        cls.markers = os.path.join(cls.tmp.name, 'markers.bin')
        with open(ev_path, 'w', encoding='utf-8') as f:
            f.write(events)
        with open(cls.markers, 'wb') as f:
            f.write(markers)
        h, _ = common.read_markers(cls.markers)
        cls.samples = os.path.join(cls.tmp.name, 'samples.jsonl')
        cls.counts = etw_samples.convert(ev_path, cls.anchors, cls.samples, synthetic.QPC_FREQ,
                                         common.steady_offset_ns(h['anchors']))
        with open(cls.samples, encoding='utf-8') as f:
            cls.records = [json.loads(l) for l in f]

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_anchor_origin(self):
        self.assertEqual(etw_samples.qpc_origin(self.anchors, synthetic.QPC_FREQ), synthetic.QPC_ORIGIN)
        self.assertEqual(etw_samples.estimate_freq(self.anchors), synthetic.QPC_FREQ)

    def test_counts(self):
        self.assertEqual(self.counts.get('sample'), 3 * (20 + 10))
        self.assertEqual(self.counts.get('switch'), 6)
        self.assertEqual(self.counts.get('ready'), 3)
        self.assertEqual(self.counts.get('module'), 1)

    def test_thread_names(self):
        names = {r['name'] for r in self.records if r['type'] == 'thread' and r['name']}
        self.assertEqual(names, {'fixture-hot', 'fixture-waiter', 'fixture-waker'})

    def test_report(self):
        rep = report.build(self.records, [self.markers], {FIXTURE_EXE})
        interval = rep['sample_interval_ns']
        self.assertEqual(interval, 1_000_000)
        self.assertEqual(rep['processes_with_samples'], [FIXTURE_EXE])
        hot = [t for t in rep['threads'] if t['name'] == 'fixture-hot']
        self.assertEqual(len(hot), 3)
        # no symbol files here: frames in the module are named after it, kernel addresses "[kernel]"
        self.assertTrue(all(t['self'][0][0] == f'<{FIXTURE_EXE}>' for t in hot))
        waits = [w for t in rep['threads'] if t['name'] == 'fixture-waiter' for w in t['waits']]
        marks = {m['seq']: m['t_ns'] for m in rep['markers'] if m['event'] == 100}
        for i, cy in enumerate(self.truth['cycles']):
            w = [w for w in waits if w['start_ns'] <= cy['signal_ns'] <= w['end_ns']][0]
            self.assertLessEqual(abs((w['end_ns'] - w['start_ns']) - (cy['woken_ns'] - cy['wait_start_ns'])), interval)
            self.assertLessEqual(abs(w['start_ns'] - marks[10 * (i + 1) + 3]), interval)
            self.assertLessEqual(abs(w['end_ns'] - marks[10 * (i + 1) + 5]), interval)
            self.assertEqual(w['waker_thread'], 'fixture-waker')
            self.assertEqual(w['waker_stack'], ['[kernel]', f'<{FIXTURE_EXE}>'])
        spans = [s for s in rep['spans'] if s['tid'] in {t['tid'] for t in hot}]
        self.assertEqual(len(spans), 3)


if __name__ == '__main__':
    unittest.main()
