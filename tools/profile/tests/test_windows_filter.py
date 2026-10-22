#!/usr/bin/env python3
"""Fast test of the Windows export filter (windows/Dw5xlFilter.cs) on synthetic input, without admin rights or a
recording: compiles it with Windows PowerShell's Add-Type and feeds it a small export with our program and a
made-up other program ("secret.exe"), then checks that only our data comes out; the other program appears only
as "other" (no name, id or stack).
Also checks record-task.ps1's decision mode. Windows only."""
import json, os, pathlib, subprocess, tempfile, textwrap, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
WIN = os.path.join(os.path.dirname(HERE), 'windows')
POWERSHELL = os.path.join(os.environ.get('WINDIR', r'C:\Windows'), 'System32', 'WindowsPowerShell', 'v1.0', 'powershell.exe')

EXPORT = textwrap.dedent("""\
    BeginHeader
                    P-Start,  TimeStamp,     Process Name ( PID),  ParentPID,  SessionID,          UniqueKey, DirectoryTableBase, Flags, UserSid, Command Line, Package Full Name, Package Relative Application Id
                    CSwitch,  TimeStamp, New Process Name ( PID),    New TID, NPri, NQnt, TmSinceLast, WaitTime, Old Process Name ( PID),    Old TID, OPri, OQnt,        OldState,      Wait Reason, Swapable, InSwitchTime, CPU, IdealProc,  OldRemQnt, NewPriDecr, PrevCState, OldThrdBamQosLevel, NewThrdBamQosLevel
    EndHeader
                  P-DCStart,         10, dw5xl_profile_fixture.exe (  100),  1, 1, 0x0, 0x0, 0, S-1, "fixture.exe --out x", ,
                  P-DCStart,         11,        secret.exe (  200),  1, 1, 0x0, 0x0, 0, S-1, "secret.exe --open private-diary.txt", ,
                  T-DCStart,         12, dw5xl_profile_fixture.exe (  100),       1001, 0x0, 0x0, 0x0, 0x0, 0x0, 0, 8, 5, 2, 0x0, fixture.exe!main
                  T-DCStart,         13,        secret.exe (  200),       2001, 0x0, 0x0, 0x0, 0x0, 0x0, 0, 8, 5, 2, 0x0, secret.exe!main
                 ThreadName,         14, dw5xl_profile_fixture.exe (  100),       1001, fixture-hot
                 ThreadName,         15,        secret.exe (  200),       2001, secret-thread
                    I-Start,         16, dw5xl_profile_fixture.exe (  100), 0x00007ff700000000, 0x00007ff700100000, 0x0, 0x0, 0x00007ff700000000, "C:\\\\x\\\\dw5xl_profile_fixture.exe", 0, \\Device\\x
                    I-Start,         17,        secret.exe (  200), 0x00007ff800000000, 0x00007ff800100000, 0x0, 0x0, 0x00007ff800000000, "C:\\\\x\\\\secret.exe", 0, \\Device\\x
             SampledProfile,         20, dw5xl_profile_fixture.exe (  100),       1001, 0x00007ff700001000,   1, fixture.exe!0x1, fixture.exe!0x2,     1, Unbatched
                      Stack,         20,       1001,   1, 0x00007ff700001000, dw5xl_profile_fixture.exe!0x00007ff700001000
             SampledProfile,         21,        secret.exe (  200),       2001, 0x00007ff800001000,   2, secret.exe!0x1, secret.exe!0x2,     1, Unbatched
                      Stack,         21,       2001,   1, 0x00007ff800001000,        secret.exe!0x00007ff800001000
                    CSwitch,         30,        secret.exe (  200),       2001,    8,   -1,           3,        0, dw5xl_profile_fixture.exe (  100),       1001,    8,   -1,         Waiting,        UserRequest,  NonSwap,    122,   0,   0,          0,    0,    1,     Utility,   Important
                      Stack,         30,       2001,   1, 0x00007ff800002000,        secret.exe!0x00007ff800002000
                ReadyThread,         40,        secret.exe (  200),       2001, dw5xl_profile_fixture.exe (  100),       1001,       Unwait,               0,  28,
                      Stack,         40,       2001,   1, 0x00007ff800003000,        secret.exe!0x00007ff800003000
                    CSwitch,         41, dw5xl_profile_fixture.exe (  100),       1001,    8,   -1,           3,        0,        secret.exe (  200),       2001,    8,   -1,         Running,        Executive,  NonSwap,    122,   0,   0,          0,    0,    1,     Utility,   Important
                      Stack,         41,       1001,   1, 0x00007ff700002000, dw5xl_profile_fixture.exe!0x00007ff700002000
                      P-End,         50, dw5xl_profile_fixture.exe (  100),  1, 1, 0x0, 0x0, 0, S-1, "fixture.exe", ,
    """)


@unittest.skipUnless(os.name == 'nt' and os.path.exists(POWERSHELL), 'Windows PowerShell only')
class WindowsFilter(unittest.TestCase):
    def run_ps(self, script):
        p = subprocess.run([POWERSHELL, '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-Command', script],
                           capture_output=True, text=True, timeout=180)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        return p.stdout

    def test_filter(self):
        with tempfile.TemporaryDirectory() as d:
            src, dst = os.path.join(d, 'in.txt'), os.path.join(d, 'out.txt')
            pathlib.Path(src).write_text(EXPORT, encoding='utf-8')
            cs = os.path.join(WIN, 'Dw5xlFilter.cs')
            out = self.run_ps(f"Add-Type -TypeDefinition (Get-Content -Raw -Path '{cs}') -Language CSharp; "
                              f"$r = New-Object System.IO.StreamReader('{src}'); $w = New-Object System.IO.StreamWriter('{dst}'); "
                              f"$n = [Dw5xlFilter]::Filter($r, $w, [string[]]@('dw5xl_profile_fixture.exe'), 0); "
                              f"$r.Close(); $w.Close(); $n")
            text = pathlib.Path(dst).read_text(encoding='utf-8')
        body = text.split('EndHeader', 1)[1]
        self.assertGreater(int(out.strip().splitlines()[-1]), 0)
        self.assertNotIn('private-diary.txt', text)
        self.assertNotIn('secret-thread', body)
        self.assertNotIn('secret.exe', body)               # not even its name
        self.assertNotIn('secret.exe!', body)
        self.assertNotIn('2001', body)              # no thread id or stack of the other program
        self.assertIn('other (0)', body)             # only "other", where it meets ours
        self.assertIn('fixture-hot', body)
        self.assertIn('0x00007ff700001000', body)  # our sample's stack
        self.assertIn('0x00007ff700002000', body)  # our switched-in thread's stack
        self.assertEqual(body.count('SampledProfile'), 1)
        self.assertEqual(body.count('CSwitch'), 2)
        self.assertEqual(body.count('ReadyThread'), 1)

    def test_cap(self):
        with tempfile.TemporaryDirectory() as d:
            src, dst = os.path.join(d, 'in.txt'), os.path.join(d, 'out.txt')
            pathlib.Path(src).write_text(EXPORT, encoding='utf-8')
            cs = os.path.join(WIN, 'Dw5xlFilter.cs')
            out = self.run_ps(f"Add-Type -TypeDefinition (Get-Content -Raw -Path '{cs}') -Language CSharp; "
                              f"$r = New-Object System.IO.StreamReader('{src}'); $w = New-Object System.IO.StreamWriter('{dst}'); "
                              f"$n = [Dw5xlFilter]::Filter($r, $w, [string[]]@('dw5xl_profile_fixture.exe'), 600); "
                              f"$r.Close(); $w.Close(); $n")
            self.assertLess(int(out.strip().splitlines()[-1]), 0)   # negative: cut short by the cap
            self.assertLessEqual(os.path.getsize(dst), 700)

    def test_decide(self):
        with tempfile.TemporaryDirectory() as d:
            bindir = os.path.join(d, 'bin')
            os.makedirs(bindir)
            pathlib.Path(bindir, 'record-task.ps1').write_text(
                pathlib.Path(WIN, 'record-task.ps1').read_text(encoding='utf-8'), encoding='utf-8')
            pathlib.Path(bindir, 'config.json').write_text(
                json.dumps({'rate_limit_per_hour': 15, 'min_free_gb': 20, 'user_dir': d}), encoding='utf-8')
            script = os.path.join(bindir, 'record-task.ps1')
            low = json.loads(self.run_ps(f"& '{script}' -DecideOnly -SimulatedFreeGb 19.9").strip().splitlines()[-1])
            ok = json.loads(self.run_ps(f"& '{script}' -DecideOnly -SimulatedFreeGb 20.0").strip().splitlines()[-1])
        self.assertEqual(low, {'accepted': False, 'reason': 'low disk'})
        self.assertEqual(ok, {'accepted': True, 'reason': 'ok'})

    def test_rate_count(self):
        """15 accepted recordings in the last hour block the next one; older and refused entries do not count."""
        import datetime
        now = datetime.datetime.now(datetime.timezone.utc)

        def line(minutes_ago, accepted):
            t = (now - datetime.timedelta(minutes=minutes_ago)).strftime('%Y-%m-%dT%H:%M:%SZ')
            return json.dumps({'time': t, 'accepted': accepted, 'reason': 'ok' if accepted else 'off'})
        with tempfile.TemporaryDirectory() as d:
            bindir = os.path.join(d, 'bin')
            os.makedirs(bindir)
            pathlib.Path(bindir, 'record-task.ps1').write_text(
                pathlib.Path(WIN, 'record-task.ps1').read_text(encoding='utf-8'), encoding='utf-8')
            pathlib.Path(bindir, 'config.json').write_text(
                json.dumps({'rate_limit_per_hour': 15, 'min_free_gb': 20, 'user_dir': d}), encoding='utf-8')
            script = os.path.join(bindir, 'record-task.ps1')
            results = {}
            for n in (14, 15):
                audit = os.path.join(d, f'audit{n}.jsonl')
                lines = [line(5 + i, True) for i in range(n)] + [line(90, True), line(1, False), line(2, False)]
                pathlib.Path(audit).write_text('\n'.join(lines) + '\n', encoding='utf-8')
                results[n] = json.loads(self.run_ps(
                    f"& '{script}' -DecideOnly -SimulatedFreeGb 100 -AuditPath '{audit}'").strip().splitlines()[-1])
        self.assertEqual(results[14], {'accepted': True, 'reason': 'ok'})
        self.assertEqual(results[15], {'accepted': False, 'reason': 'rate limit'})


if __name__ == '__main__':
    unittest.main()
