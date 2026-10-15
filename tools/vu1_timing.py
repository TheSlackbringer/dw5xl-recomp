"""Per-pair VU1 timing, mirroring the runtime interpreter's decode.

A transcription of VU1Interpreter::decodeUpperUsage / decodeLowerUsage /
decodeInstructionPair and makeLiftTiming (ps2xRuntime/src/lib/vu/ps2_vu1_core.cpp).
The lifted code embeds the resulting table; at runtime each segment checks it
against the interpreter's own decode and is interpreted instead on a mismatch,
so a divergence here costs speed, never correctness.
"""

FMAC_LATENCY = 4
ACC_FORWARD_LATENCY = 1
READY_VI_BASE = 128
READY_ACC_BASE = 144

PIPE_NONE, PIPE_FMAC, PIPE_LSU, PIPE_FDIV, PIPE_EFU, PIPE_IALU, PIPE_BRANCH, PIPE_XGKICK = range(8)

FLAG_FDIV, FLAG_EFU, FLAG_WAITQ, FLAG_WAITP, FLAG_XGKICK, FLAG_BRANCH_DELAY = 1, 2, 4, 8, 16, 32


def lane_for_component(component):
    return 1 << (3 - component)


class Usage:
    def __init__(self):
        self.vf_read = []          # [reg, lanes], at most 2
        self.vf_write = [0, 0]     # reg, lanes
        self.vi_read = 0
        self.vi_write = 0
        self.acc_read = 0
        self.acc_write = 0
        self.latency = 0
        self.vf_latency = 0
        self.vi_latency = 0
        self.pipeline = PIPE_NONE
        self.wait_q = False
        self.wait_p = False
        self.delays_branch = False

    def add_vf_read(self, reg, lanes):
        if lanes == 0:
            return
        for entry in self.vf_read:
            if entry[0] == reg:
                entry[1] |= lanes
                return
        if len(self.vf_read) < 2:
            self.vf_read.append([reg, lanes])

    def add_vf_write(self, reg, lanes):
        if reg == 0 or lanes == 0:
            return
        if self.vf_write[0] == 0:
            self.vf_write = [reg, lanes]
        elif self.vf_write[0] == reg:
            self.vf_write[1] |= lanes

    def read_vi(self, reg):
        if reg != 0:
            self.vi_read |= 1 << reg

    def write_vi(self, reg):
        if reg != 0:
            self.vi_write |= 1 << reg


def dest(w): return (w >> 21) & 0xF
def ft(w): return (w >> 16) & 0x1F
def fs(w): return (w >> 11) & 0x1F
def fd(w): return (w >> 6) & 0x1F


def upper_usage(upper):
    u = Usage()
    u.pipeline = PIPE_FMAC
    u.latency = FMAC_LATENCY
    op = upper & 0x3F
    d, s, t, f = dest(upper), fs(upper), ft(upper), fd(upper)
    if op <= 0x2F:
        u.add_vf_read(s, d)
        u.add_vf_write(f, d)
        if op <= 0x1B:
            u.add_vf_read(t, lane_for_component(op & 3))
        elif op >= 0x28:
            u.add_vf_read(t, 0xE if op == 0x2E else d)
        if op in (0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x21, 0x23, 0x25, 0x27, 0x29, 0x2D, 0x2E):
            u.acc_read = d
        return u
    if op >= 0x3C:
        special = (upper & 3) | ((upper >> 4) & 0x7C)
        writes_acc = (special <= 0x0F or 0x18 <= special <= 0x1C or special == 0x1E or
                      0x20 <= special <= 0x2A or 0x2C <= special <= 0x2E)
        if writes_acc:
            u.add_vf_read(s, d)
            if special <= 0x1B:
                u.add_vf_read(t, lane_for_component(special & 3))
            elif 0x28 <= special <= 0x2E:
                u.add_vf_read(t, 0xE if special == 0x2E else d)
            u.acc_write = d
            if (0x08 <= special <= 0x0F) or special in (0x21, 0x23, 0x25, 0x27, 0x29, 0x2D):
                u.acc_read = d
        elif 0x10 <= special <= 0x17 or special == 0x1D:
            u.add_vf_read(s, d)
            u.add_vf_write(t, d)
        elif special == 0x1F:
            u.add_vf_read(s, 0xE)
            u.add_vf_read(t, 0x1)
        elif special not in (0x2F, 0x30):
            raise ValueError(f'reserved upper instruction {upper:08x}')
        return u
    raise ValueError(f'reserved upper instruction {upper:08x}')


def lower_usage(lower):
    u = Usage()
    if lower == 0 or lower == 0x8000033C:
        return u
    op_hi = (lower >> 25) & 0x7F
    d, vf_t, vf_s = dest(lower), ft(lower), fs(lower)
    vi_t, vi_s, vi_d = ft(lower), fs(lower), fd(lower)
    if op_hi == 0x00:
        u.pipeline, u.latency = PIPE_LSU, 4; u.read_vi(vi_s); u.add_vf_write(vf_t, d); return u
    if op_hi == 0x01:
        u.pipeline, u.latency = PIPE_LSU, 1; u.read_vi(vi_t); u.add_vf_read(vf_s, d); return u
    if op_hi == 0x04:
        u.pipeline, u.latency = PIPE_LSU, 4; u.read_vi(vi_s); u.write_vi(vi_t); return u
    if op_hi == 0x05:
        u.pipeline, u.latency = PIPE_LSU, 1; u.read_vi(vi_s); u.read_vi(vi_t); return u
    if op_hi in (0x08, 0x09):
        u.pipeline, u.latency, u.delays_branch = PIPE_IALU, 1, True; u.read_vi(vi_s); u.write_vi(vi_t); return u
    if op_hi in (0x10, 0x12, 0x13):
        u.pipeline, u.latency = PIPE_IALU, 1; u.write_vi(1); return u
    if op_hi == 0x11:
        u.pipeline, u.latency = PIPE_FMAC, FMAC_LATENCY; return u
    if op_hi in (0x14, 0x16, 0x17):
        u.pipeline, u.latency = PIPE_IALU, 1; u.write_vi(vi_t); return u
    if op_hi == 0x15:
        u.pipeline, u.latency = PIPE_FMAC, FMAC_LATENCY; return u
    if op_hi in (0x18, 0x1A, 0x1B):
        u.pipeline, u.latency = PIPE_IALU, 1; u.read_vi(vi_s); u.write_vi(vi_t); return u
    if op_hi == 0x1C:
        u.pipeline, u.latency = PIPE_IALU, 1; u.write_vi(vi_t); return u
    if op_hi == 0x20:
        u.pipeline = PIPE_BRANCH; return u
    if op_hi == 0x21:
        u.pipeline, u.latency = PIPE_BRANCH, 1; u.write_vi(vi_t); return u
    if op_hi == 0x24:
        u.pipeline = PIPE_BRANCH; u.read_vi(vi_s); return u
    if op_hi == 0x25:
        u.pipeline, u.latency = PIPE_BRANCH, 1; u.read_vi(vi_s); u.write_vi(vi_t); return u
    if op_hi in (0x28, 0x29):
        u.pipeline = PIPE_BRANCH; u.read_vi(vi_s); u.read_vi(vi_t); return u
    if op_hi in (0x2C, 0x2D, 0x2E, 0x2F):
        u.pipeline = PIPE_BRANCH; u.read_vi(vi_s); return u
    if op_hi != 0x40:
        raise ValueError(f'reserved lower instruction {lower:08x}')

    direct = lower & 0x3F
    if direct in (0x30, 0x31, 0x34, 0x35):
        u.pipeline, u.latency, u.delays_branch = PIPE_IALU, 1, True
        u.read_vi(vi_s); u.read_vi(vi_t); u.write_vi(vi_d); return u
    if direct == 0x32:
        u.pipeline, u.latency, u.delays_branch = PIPE_IALU, 1, True
        u.read_vi(vi_s); u.write_vi(vi_t); return u
    if direct < 0x3C:
        raise ValueError(f'reserved lower instruction {lower:08x}')

    special = (lower & 3) | ((lower >> 4) & 0x7C)
    fsf_lane = lane_for_component((lower >> 21) & 3)
    ftf_lane = lane_for_component((lower >> 23) & 3)
    if special in (0x30, 0x31):
        u.pipeline, u.latency = PIPE_FMAC, 4
        u.add_vf_read(vf_s, 0xF if special == 0x31 else d); u.add_vf_write(vf_t, d)
    elif special in (0x34, 0x36):
        u.pipeline, u.latency, u.vi_latency, u.delays_branch = PIPE_LSU, 4, 1, True
        u.read_vi(vi_s); u.write_vi(vi_s); u.add_vf_write(vf_t, d)
    elif special in (0x35, 0x37):
        u.pipeline, u.latency, u.delays_branch = PIPE_LSU, 1, True
        u.read_vi(vi_t); u.write_vi(vi_t); u.add_vf_read(vf_s, d)
    elif special == 0x38:
        u.pipeline, u.latency = PIPE_FDIV, 7
        u.add_vf_read(vf_s, fsf_lane); u.add_vf_read(vf_t, ftf_lane)
    elif special == 0x39:
        u.pipeline, u.latency = PIPE_FDIV, 7
        u.add_vf_read(vf_t, ftf_lane)
    elif special == 0x3A:
        u.pipeline, u.latency = PIPE_FDIV, 13
        u.add_vf_read(vf_s, fsf_lane); u.add_vf_read(vf_t, ftf_lane)
    elif special == 0x3B:
        u.pipeline, u.wait_q = PIPE_FDIV, True
    elif special == 0x3C:
        u.pipeline, u.latency, u.delays_branch = PIPE_IALU, 1, True
        u.add_vf_read(vf_s, fsf_lane); u.write_vi(vi_t)
    elif special == 0x3D:
        u.pipeline, u.latency = PIPE_FMAC, 4
        u.read_vi(vi_s); u.add_vf_write(vf_t, d)
    elif special == 0x3E:
        u.pipeline, u.latency = PIPE_LSU, 4
        u.read_vi(vi_s); u.write_vi(vi_t)
    elif special == 0x3F:
        u.pipeline, u.latency = PIPE_LSU, 1
        u.read_vi(vi_s); u.read_vi(vi_t)
    elif special in (0x40, 0x41):
        u.pipeline, u.latency = PIPE_FMAC, 4
        u.add_vf_write(vf_t, d)
    elif special in (0x42, 0x43):
        u.pipeline, u.latency = PIPE_IALU, 1
        u.add_vf_read(vf_s, fsf_lane)
    elif special == 0x64:
        u.pipeline, u.latency = PIPE_FMAC, 4
        u.add_vf_write(vf_t, d)
    elif special in (0x68, 0x69):
        u.pipeline, u.latency = PIPE_IALU, 1
        u.write_vi(vi_t)
    elif special == 0x6C:
        u.pipeline, u.latency = PIPE_XGKICK, 2
        u.read_vi(vi_s)
    elif special in (0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x78, 0x79, 0x7A, 0x7C, 0x7D, 0x7E):
        u.pipeline = PIPE_EFU
        u.latency = {0x70: 11, 0x71: 18, 0x72: 18, 0x79: 18, 0x73: 24, 0x74: 54, 0x75: 54, 0x7D: 54,
                     0x76: 12, 0x78: 12, 0x7A: 12, 0x7C: 29, 0x7E: 44}[special]
        if 0x70 <= special <= 0x73:
            u.add_vf_read(vf_s, 0xE)
        elif special == 0x74:
            u.add_vf_read(vf_s, 0xC)
        elif special == 0x75:
            u.add_vf_read(vf_s, 0xA)
        elif special == 0x76:
            u.add_vf_read(vf_s, 0xF)
        else:
            u.add_vf_read(vf_s, fsf_lane)
    elif special == 0x7B:
        u.pipeline, u.wait_p = PIPE_EFU, True
    else:
        raise ValueError(f'reserved lower instruction {lower:08x}')
    return u


def vf_read_lanes(usage, reg):
    for r, lanes in usage.vf_read:
        if r == reg:
            return lanes
    return 0


def pair_timing(lower, upper):
    """VuLiftTiming fields for one pair (dict), as makeLiftTiming builds them."""
    up = upper_usage(upper)
    lo = Usage() if upper & 0x80000000 else lower_usage(lower)
    suppressed = 0
    w = up.vf_write[0]
    if w != 0 and (vf_read_lanes(lo, w) != 0 or lo.vf_write[0] == w):
        if lo.vf_write[0] == w:
            suppressed = w

    groups = [(r, l) for usage in (up, lo) for r, l in usage.vf_read if r != 0 and l != 0]
    vf_reg = [g[0] for g in groups] + [0] * (4 - len(groups))
    vf_lanes = [g[1] for g in groups] + [0] * (4 - len(groups))

    writes = []  # (slot, latency) in decodeInstructionPair's order
    w_reg, w_lanes, w_lat = [0, 0], [0, 0], [0, 0]
    if lo.vf_write[0] != 0 and suppressed != lo.vf_write[0]:
        lat = lo.vf_latency or lo.latency
        w_reg[0], w_lanes[0], w_lat[0] = lo.vf_write[0], lo.vf_write[1], lat
        writes += [(lo.vf_write[0] * 4 + c, lat) for c in range(4) if lo.vf_write[1] & lane_for_component(c)]
    if up.vf_write[0] != 0:
        lat = up.vf_latency or up.latency
        w_reg[1], w_lanes[1], w_lat[1] = up.vf_write[0], up.vf_write[1], lat
        writes += [(up.vf_write[0] * 4 + c, lat) for c in range(4) if up.vf_write[1] & lane_for_component(c)]
    vi_lat = lo.vi_latency or lo.latency
    vi_write = lo.vi_write & 0xFFFE
    writes += [(READY_VI_BASE + i, vi_lat) for i in range(16) if vi_write & (1 << i)]
    writes += [(READY_ACC_BASE + c, ACC_FORWARD_LATENCY) for c in range(4) if up.acc_write & lane_for_component(c)]
    max_lat = max((lat for _, lat in writes), default=0)
    first_vi = (vi_write & -vi_write).bit_length() - 1 if vi_write else 0

    flags = ((FLAG_FDIV if lo.pipeline == PIPE_FDIV else 0) | (FLAG_EFU if lo.pipeline == PIPE_EFU else 0) |
             (FLAG_WAITQ if lo.wait_q else 0) | (FLAG_WAITP if lo.wait_p else 0) |
             (FLAG_XGKICK if lo.pipeline == PIPE_XGKICK else 0) | (FLAG_BRANCH_DELAY if lo.delays_branch else 0))

    reads = []
    for usage in (up, lo):
        for r, l in usage.vf_read:
            if r != 0:
                reads += [r * 4 + c for c in range(4) if l & lane_for_component(c)]
        reads += [READY_VI_BASE + i for i in range(1, 16) if usage.vi_read & (1 << i)]
        reads += [READY_ACC_BASE + c for c in range(4) if usage.acc_read & lane_for_component(c)]

    return {
        'vfReg': vf_reg, 'vfLanes': vf_lanes,
        'viRead': (up.vi_read | lo.vi_read) & 0xFFFE, 'viWrite': vi_write,
        'accRead': up.acc_read | lo.acc_read, 'accWrite': up.acc_write,
        'wReg': w_reg, 'wLanes': w_lanes, 'wLat': w_lat,
        'viLat': vi_lat, 'maxLat': max_lat, 'firstVi': first_vi, 'flags': flags,
        # derived, for the emitted code
        'reads': sorted(set(reads)), 'writes': writes,
    }


def timing_initializer(t):
    """C++ aggregate initializer for a VuLiftTiming."""
    a = lambda xs: '{' + ', '.join(str(x) for x in xs) + '}'
    return (f"{{{a(t['vfReg'])}, {a(t['vfLanes'])}, 0x{t['viRead']:04x}, 0x{t['viWrite']:04x}, "
            f"{t['accRead']}, {t['accWrite']}, {a(t['wReg'])}, {a(t['wLanes'])}, {a(t['wLat'])}, "
            f"{t['viLat']}, {t['maxLat']}, {t['firstVi']}, {t['flags']}}}")
