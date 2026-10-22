#!/usr/bin/env python3
"""Fast unit tests for the Linux side of the profiling tools (perf_samples.py, report.py with name frames), on made-up
`perf script` text (tests/synthetic.py). No recording, no perf needed. Not part of CI (a debugging tool); run when
changing the tools: python3 -m unittest discover -s tools/profile/tests"""
import json, os, sys, tempfile, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
import common, perf_samples, report, synthetic

FIXTURE = synthetic.FIXTURE


def convert(lines, markers=None, programs=(FIXTURE,), cap=None):
    tmp = tempfile.TemporaryDirectory()
    path = os.path.join(tmp.name, 'samples.jsonl')
    offset = 0
    if markers:
        mpath = os.path.join(tmp.name, 'markers.bin')
        with open(mpath, 'wb') as f:
            f.write(markers)
        h, _ = common.read_markers(mpath)
        offset = common.steady_offset_ns(h['anchors'])
    counts = perf_samples.convert(lines, path, set(programs), offset, cap)
    with open(path, encoding='utf-8') as f:
        records = [json.loads(l) for l in f if l.strip()]
    return tmp, path, counts, records


class FrameNames(unittest.TestCase):
    def test_names(self):
        self.assertEqual(perf_samples.frame_name('fixture_hot_spin+0x14', '/x/a'), 'fixture_hot_spin')
        self.assertEqual(perf_samples.frame_name('[unknown]', '[unknown]'), '[unknown]')
        self.assertEqual(perf_samples.frame_name('[unknown]', '/usr/lib/libz.so.1'), '<libz.so.1>')
        self.assertEqual(perf_samples.frame_name('schedule', '[kernel.kallsyms]'), '[kernel]')


class Convert(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lines, cls.truth, cls.markers = synthetic.perf_text()
        cls.tmp, cls.path, cls.counts, cls.records = convert(cls.lines, cls.markers)
        cls.mpath = os.path.join(cls.tmp.name, 'markers.bin')

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_counts(self):
        self.assertEqual(self.counts.get('sample'), 3 * (20 + 10))
        self.assertEqual(self.counts.get('switch'), 6)
        self.assertEqual(self.counts.get('ready'), 3)
        self.assertEqual(self.counts.get('lost', 0), 0)

    def test_process_name_from_exec_mapping(self):
        # perf's comm is cut to 15 characters ("dw5xl_profile_f"); the full name comes from the executable's mapping
        procs = [r for r in self.records if r['type'] == 'process']
        self.assertEqual([(r['pid'], r['process']) for r in procs], [(synthetic.PID, FIXTURE)])
        self.assertTrue(any(r['type'] == 'module' and r['path'] == '/x/' + FIXTURE for r in self.records))

    def test_thread_names(self):
        names = {r['name'] for r in self.records if r['type'] == 'thread' and r['name']}
        self.assertTrue({'fixture-hot', 'fixture-waiter', 'fixture-waker'} <= names)

    def test_only_allowed_programs(self):
        self.assertEqual({r['process'] for r in self.records if r['type'] == 'sample'}, {FIXTURE})
        self.assertFalse(any(r.get('process') == 'otherprog' for r in self.records))
        tmp, _, _, records = convert(self.lines, programs=('dw5xl',))
        tmp.cleanup()
        self.assertEqual(records, [])

    def test_switches_one_sided(self):
        sw = [r for r in self.records if r['type'] == 'switch']
        outs = [r for r in sw if r['old_pid'] is not None]
        ins = [r for r in sw if r['new_pid'] is not None]
        self.assertTrue(outs and ins and len(outs) + len(ins) == len(sw))
        self.assertTrue(all(r['new_pid'] is None and r['old_state'] == 'Waiting' for r in outs))
        self.assertTrue(all(r['old_pid'] is None for r in ins))

    def test_cap(self):
        tmp, path, counts, _ = convert(self.lines, cap=4_000)
        size = os.path.getsize(path)
        tmp.cleanup()
        self.assertTrue(counts.get('truncated'))
        self.assertLessEqual(size, 4_000)

    def test_report(self):
        rep = report.build(self.records, [self.mpath], {FIXTURE})
        interval = rep['sample_interval_ns']
        self.assertEqual(interval, 1_000_000)
        pid = self.truth['pid']
        hot = [t for t in rep['threads'] if t['pid'] == pid and t['name'] == 'fixture-hot']
        self.assertEqual(len(hot), len(self.truth['cycles']))
        for t in hot:
            self.assertEqual(t['self'][0][0], 'fixture_hot_spin')
            self.assertIn('fixture_hot_outer', [n for n, _ in t['total']])
        waits = [w for t in rep['threads'] if t['pid'] == pid and t['name'] == 'fixture-waiter' for w in t['waits']]
        marks = {m['seq']: m['t_ns'] for m in rep['markers'] if m['event'] == 100}
        for i, cy in enumerate(self.truth['cycles']):
            w = [w for w in waits if w['start_ns'] <= cy['signal_ns'] <= w['end_ns']][0]
            self.assertLessEqual(abs((w['end_ns'] - w['start_ns']) - (cy['woken_ns'] - cy['wait_start_ns'])), interval)
            # the recorder's exact switch moments vs the fixture's markers for the same moments
            self.assertLessEqual(abs(w['start_ns'] - marks[10 * (i + 1) + 3]), interval)
            self.assertLessEqual(abs(w['end_ns'] - marks[10 * (i + 1) + 5]), interval)
            self.assertEqual(w['waker_thread'], 'fixture-waker')
            self.assertEqual(w['waker_stack'], ['pthread_cond_signal', 'std::condition_variable::notify_one',
                                                'fixture_wake_signal'])
        self.assertEqual(rep['processes_with_samples'], [FIXTURE])

    def test_without_wakeups(self):
        # paranoid 2: no sched_waking events, so waits have no waker (durations still measured)
        lines, truth, markers = synthetic.perf_text(wakeups=False)
        tmp, _, counts, records = convert(lines, markers)
        tmp.cleanup()
        self.assertEqual(counts.get('ready', 0), 0)
        rep = report.build(records, [], {FIXTURE})
        waits = [w for t in rep['threads'] if t['name'] == 'fixture-waiter' for w in t['waits']]
        self.assertEqual(len(waits), len(truth['cycles']))
        self.assertTrue(all(w['waker_thread'] is None for w in waits))


if __name__ == '__main__':
    unittest.main()
