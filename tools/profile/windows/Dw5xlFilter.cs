// Export filter (tools/profile), compiled by record-task.ps1 (Add-Type, C# 5). Runs as administrator inside
// the protected folder: reads xperf's text export of a recording straight from xperf's output (no temporary file)
// and writes only what concerns the allowed programs (fixed names from the protected config):
//   their processes, threads, thread names, modules and CPU samples with stacks; thread switches and wake-ups where
//   one side is theirs, the other side reduced to "other (0)", thread 0, no stack: not even its name.
// Everything else stays inside the admin-only raw recording.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;

public static class Dw5xlFilter
{
    static readonly Regex Proc = new Regex(@"^(.*?)\s*\(\s*(\d+)\)$", RegexOptions.Compiled);

    static Process StartXperf(string xperf, string args)
    {
        var psi = new ProcessStartInfo(xperf, args);
        psi.UseShellExecute = false;
        psi.RedirectStandardOutput = true;
        psi.RedirectStandardError = true;
        psi.CreateNoWindow = true;
        var p = Process.Start(psi);
        p.ErrorDataReceived += (s, e) => { };
        p.BeginErrorReadLine();
        return p;
    }

    static string[] Fields(string line)
    {
        var f = line.Split(',');
        for (int i = 0; i < f.Length; i++)
            f[i] = f[i].Trim();
        return f;
    }

    static bool ParseProc(string field, out string name, out int pid)
    {
        var m = Proc.Match(field);
        name = m.Success ? m.Groups[1].Value.Trim() : field;
        pid = -1;
        return m.Success && int.TryParse(m.Groups[2].Value, out pid);
    }

    static bool TryInt(string s, out int v) { return int.TryParse(s, out v); }

    // Exports a recording; returns the number of lines written, negative if the output cap cut it short.
    public static long Run(string xperf, string etl, string outPath, string[] allowed, long cap)
    {
        var p = StartXperf(xperf, "-i \"" + etl + "\" -quiet -tle -tti -a dumper -stacktimeshifting");
        long n;
        using (var w = new StreamWriter(outPath, false, new UTF8Encoding(false), 1 << 20))
            n = Filter(p.StandardOutput, w, allowed, cap);
        p.WaitForExit();
        return n;
    }

    // The filter itself, on any text export (unit-tested with synthetic input).
    public static long Filter(TextReader input, TextWriter w, string[] allowed, long cap)
    {
        var allow = new HashSet<string>(allowed, StringComparer.OrdinalIgnoreCase);
        var pids = new HashSet<int>();
        var tids = new HashSet<int>();
        long kept = 0, bytes = 0;
        bool truncated = false, header = true;
        {
            string line;
            while ((line = input.ReadLine()) != null)
            {
                if (truncated)
                    continue; // keep reading so xperf can finish
                string emit = null;
                if (header)
                {
                    emit = line;
                    if (line.Trim() == "EndHeader")
                        header = false;
                }
                else
                {
                    int c = line.IndexOf(',');
                    if (c < 0)
                        continue;
                    string kind = line.Substring(0, c).Trim();
                    string[] f;
                    string name, other;
                    int pid, otherPid, tid;
                    switch (kind)
                    {
                    case "Stack":
                        f = Fields(line);
                        if (f.Length >= 5 && TryInt(f[2], out tid) && tids.Contains(tid))
                            emit = "Stack, " + f[1] + ", " + f[2] + ", " + f[3] + ", " + f[4];
                        break;
                    case "P-Start":
                    case "P-DCStart":
                        f = Fields(line);
                        if (f.Length > 2 && ParseProc(f[2], out name, out pid) && allow.Contains(name))
                        {
                            pids.Add(pid);
                            emit = line;
                        }
                        break;
                    case "P-End":
                    case "P-DCEnd":
                        f = Fields(line);
                        if (f.Length > 2 && ParseProc(f[2], out name, out pid))
                            pids.Remove(pid);
                        break;
                    case "T-Start":
                    case "T-DCStart":
                    case "ThreadName":
                    case "SampledProfile":
                        f = Fields(line);
                        if (f.Length > 3 && ParseProc(f[2], out name, out pid) && (pids.Contains(pid) || allow.Contains(name)))
                        {
                            pids.Add(pid);
                            if (TryInt(f[3], out tid))
                                tids.Add(tid);
                            emit = line;
                        }
                        break;
                    case "T-End":
                    case "T-DCEnd":
                        f = Fields(line);
                        if (f.Length > 3 && ParseProc(f[2], out name, out pid) && pids.Contains(pid) && TryInt(f[3], out tid))
                            tids.Remove(tid);
                        break;
                    case "I-Start":
                    case "I-DCStart":
                        f = Fields(line);
                        if (f.Length > 2 && ParseProc(f[2], out name, out pid) && pids.Contains(pid))
                            emit = line;
                        break;
                    case "CSwitch":
                        f = Fields(line);
                        if (f.Length > 13 && ParseProc(f[2], out name, out pid) && ParseProc(f[8], out other, out otherPid))
                        {
                            bool newOurs = pids.Contains(pid), oldOurs = pids.Contains(otherPid);
                            if (newOurs || oldOurs)
                            {
                                if (!newOurs) { f[2] = "other (0)"; f[3] = "0"; }
                                if (!oldOurs) { f[8] = "other (0)"; f[9] = "0"; }
                                emit = string.Join(", ", f);
                            }
                        }
                        break;
                    case "ReadyThread":
                        f = Fields(line);
                        if (f.Length > 6 && ParseProc(f[2], out name, out pid) && ParseProc(f[4], out other, out otherPid))
                        {
                            bool wakerOurs = pids.Contains(pid), wokenOurs = pids.Contains(otherPid);
                            if (wakerOurs || wokenOurs)
                            {
                                if (!wakerOurs) { f[2] = "other (0)"; f[3] = "0"; }
                                if (!wokenOurs) { f[4] = "other (0)"; f[5] = "0"; }
                                emit = string.Join(", ", f);
                            }
                        }
                        break;
                    }
                }
                if (emit != null)
                {
                    bytes += emit.Length + 1;
                    if (cap > 0 && bytes > cap)
                    {
                        truncated = true;
                        continue;
                    }
                    w.WriteLine(emit);
                    kept++;
                }
            }
        }
        return truncated ? -kept : kept;
    }

    // The '# cpu # 0xQPC # TimeStamp[ns] # key' lines of the first events: they tie trace time to QPC ticks.
    public static int Anchors(string xperf, string etl, string outPath, int maxLines)
    {
        var p = StartXperf(xperf, "-i \"" + etl + "\" -quiet -tle -tti -a dumper -range 0 50000 -add_rawdata");
        int n = 0;
        using (var w = new StreamWriter(outPath, false, new UTF8Encoding(false)))
        {
            string line;
            while ((line = p.StandardOutput.ReadLine()) != null)
            {
                if (n < maxLines && line.StartsWith("#"))
                {
                    w.WriteLine(line);
                    n++;
                }
            }
        }
        p.WaitForExit();
        return n;
    }
}
