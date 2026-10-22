"""Shared helpers for the profiling tools (tools/profile, a debugging tool). See CONTRACT.md."""
import glob, json, os, re, shutil, struct

WINDOWS = os.name == 'nt'
EXE = '.exe' if WINDOWS else ''
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
# Program file names whose data the Windows filter lets through (fixed list, written to the protected config); on
# Linux the same names without .exe, kept by perf_samples.py. WINDOWS_PROGRAMS also reads Windows captures elsewhere.
WINDOWS_PROGRAMS = ['dw5xl.exe', 'dw5xl_profile_fixture.exe']
ALLOWED_PROGRAMS = WINDOWS_PROGRAMS if WINDOWS else ['dw5xl', 'dw5xl_profile_fixture']
RECORDING_LIMIT_S = 480
RATE_LIMIT_PER_HOUR = 30
MIN_FREE_GB = 20
RAW_KEEP = 0   # recordings are deleted after use
# Windows: protected folder (admin-writable only) and the user's folder for the switch, stop flag and capture list.
# Not under AppData: packaged apps (such as the Claude desktop app and the programs it starts) see a private copy of
# AppData, so files written there are invisible to the scheduled task.
PROTECTED = os.path.join(os.environ.get('ProgramData', r'C:\ProgramData'), 'DW5XL-Profiling')
USER_DIR = os.path.join(os.environ.get('ProgramData', r'C:\ProgramData'), 'DW5XL-Profiling-switch')


def toolchain_bin():
    """The llvm-mingw (Windows) or LLVM (Linux) bin folder used for symbols; None if not found."""
    root = os.environ.get('LLVM_MINGW_ROOT')
    if root and os.path.isdir(os.path.join(root, 'bin')):
        return os.path.join(root, 'bin')
    for cache in sorted(glob.glob(os.path.join(REPO, 'app', 'build*', 'CMakeCache.txt'))):
        try:
            text = open(cache, encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        m = re.search(r'^LLVM_MINGW_ROOT:PATH=(.+)$', text, re.M)
        if m and os.path.isdir(os.path.join(m.group(1).strip(), 'bin')):
            return os.path.join(m.group(1).strip(), 'bin')
    found = shutil.which('llvm-symbolizer' + EXE)
    return os.path.dirname(found) if found else None


def tool_path(name):
    b = toolchain_bin()
    p = os.path.join(b, name + EXE) if b else None
    return p if p and os.path.exists(p) else shutil.which(name + EXE)


def load_json(path, default=None):
    try:
        with open(path, encoding='utf-8') as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def write_json(path, data):
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(data, f, indent=1)
    os.replace(tmp, path)


def pe_image_base(path):
    """Preferred ImageBase of a PE file (None if not PE)."""
    try:
        with open(path, 'rb') as f:
            head = f.read(4096)
        off = struct.unpack_from('<I', head, 0x3C)[0]
        if head[off:off + 4] != b'PE\0\0':
            return None
        magic = struct.unpack_from('<H', head, off + 24)[0]
        return struct.unpack_from('<Q', head, off + 24 + 24)[0] if magic == 0x20B else \
            struct.unpack_from('<I', head, off + 24 + 28)[0]
    except (OSError, struct.error, IndexError):
        return None


# ---------- markers (runtime/ps2_profile_markers.h) ----------
MARKER_HEADER = struct.Struct('<8sIIQQ' + 'QQQ' + 'QQQ')
MARKER_RECORD = struct.Struct('<QQIIIHH')


def read_markers(path):
    """(header dict, [record dicts]) from a markers file; (None, []) if missing or invalid."""
    try:
        with open(path, 'rb') as f:
            data = f.read()
    except OSError:
        return None, []
    if len(data) < MARKER_HEADER.size:
        return None, []
    h = MARKER_HEADER.unpack_from(data, 0)
    if h[0] != b'DW5XLPM1' or h[1] != 1 or h[2] != MARKER_RECORD.size:
        return None, []
    header = {'count': h[3], 'dropped': h[4],
              'anchors': [{'steady_ns': h[5], 'os_ticks': h[6], 'os_freq': h[7]},
                          {'steady_ns': h[8], 'os_ticks': h[9], 'os_freq': h[10]}]}
    records = []
    for i in range(h[3]):
        o = MARKER_HEADER.size + i * MARKER_RECORD.size
        if o + MARKER_RECORD.size > len(data):
            break
        ns, seq, tid, program, cycles, event, kind = MARKER_RECORD.unpack_from(data, o)
        records.append({'t_ns': ns, 'seq': seq, 'tid': tid, 'program': program, 'vu1_cycles': cycles,
                        'event': event, 'kind': kind})
    return header, records


def steady_offset_ns(anchors):
    """steady_ns - os_ticks converted to ns, from marker anchors (0 if none): the clock offset to apply."""
    vals = [a['steady_ns'] - a['os_ticks'] * 1_000_000_000 // a['os_freq'] for a in anchors or [] if a.get('os_freq')]
    return sum(vals) // len(vals) if vals else 0
