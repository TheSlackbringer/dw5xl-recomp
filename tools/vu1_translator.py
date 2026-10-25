import os, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vu1_timing

# Target unit (issue 025): 'vu1' (16 KB micro memory, the default) or 'vu0' (4 KB micro memory, the microprograms
# the EE starts with VCALLMS). VU0 has no PATH1/XGKICK, no EFU, no XTOP/XITOP and no WAITP/MFP.
UNIT = 'vu1'

def set_unit(unit):
    global UNIT
    if unit not in ('vu0', 'vu1'):
        raise ValueError(f'unknown VU unit {unit!r}')
    UNIT = unit
    import native_call_analysis
    native_call_analysis.PC_MASK = pc_mask()

def pc_mask():
    return 0x0fff if UNIT == 'vu0' else 0x3fff

def mem_lq(addr):
    return f"vl::lq0(mem, {addr})" if UNIT == 'vu0' else f"vl::lq(mem, {addr})"

def mem_sq(lm, addr, val):
    if UNIT == 'vu0':
        return f"vl::sq0<0x{lm:x}>(c, mem, {addr}, {val});"
    return f"vl::sq<0x{lm:x}, false>(c, mem, {addr}, {val}, 0u);"

def mem_ilw(addr, comp):
    return f"vl::ilw0(mem, {addr}, {comp})" if UNIT == 'vu0' else f"vl::ilw(mem, {addr}, {comp})"

def mem_isw(lm, addr, val):
    if UNIT == 'vu0':
        return f"vl::isw0<0x{lm:x}>(c, mem, {addr}, {val});"
    return f"vl::isw<0x{lm:x}, false>(c, mem, {addr}, {val}, 0u);"

def vu1_only(name, pc):
    if UNIT == 'vu0':
        raise ValueError(f"{name} at pc {pc:04x} is not a VU0 instruction")

BC = ['x', 'y', 'z', 'w']
def dest_name(d): return ''.join(c for i, c in enumerate('xyzw') if d & (8 >> i))
def dest_mask(d):
    return ((d & 8) >> 3) | ((d & 4) >> 1) | ((d & 2) << 1) | ((d & 1) << 3)
def s11(x): return x - 0x800 if x & 0x400 else x
def f32(u): return struct.unpack('<f', struct.pack('<I', u & 0xffffffff))[0]

UP = {
    0x00: 'ADDx', 0x01: 'ADDy', 0x02: 'ADDz', 0x03: 'ADDw',
    0x04: 'SUBx', 0x05: 'SUBy', 0x06: 'SUBz', 0x07: 'SUBw',
    0x08: 'MADDx', 0x09: 'MADDy', 0x0a: 'MADDz', 0x0b: 'MADDw',
    0x0c: 'MSUBx', 0x0d: 'MSUBy', 0x0e: 'MSUBz', 0x0f: 'MSUBw',
    0x10: 'MAXx', 0x11: 'MAXy', 0x12: 'MAXz', 0x13: 'MAXw',
    0x14: 'MINIx', 0x15: 'MINIy', 0x16: 'MINIz', 0x17: 'MINIw',
    0x18: 'MULx', 0x19: 'MULy', 0x1a: 'MULz', 0x1b: 'MULw',
    0x1c: 'MULq', 0x1d: 'MAXi', 0x1e: 'MULi', 0x1f: 'MINIi',
    0x20: 'ADDq', 0x21: 'MADDq', 0x22: 'ADDi', 0x23: 'MADDi',
    0x24: 'SUBq', 0x25: 'MSUBq', 0x26: 'SUBi', 0x27: 'MSUBi',
    0x28: 'ADD',  0x29: 'MADD',  0x2a: 'MUL',  0x2b: 'MAX',
    0x2c: 'SUB',  0x2d: 'MSUB',  0x2e: 'OPMSUB', 0x2f: 'MINI'
}

UPS = {
    0x00: 'ADDAx', 0x01: 'ADDAy', 0x02: 'ADDAz', 0x03: 'ADDAw',
    0x04: 'SUBAx', 0x05: 'SUBAy', 0x06: 'SUBAz', 0x07: 'SUBAw',
    0x08: 'MADDAx', 0x09: 'MADDAy', 0x0a: 'MADDAz', 0x0b: 'MADDAw',
    0x0c: 'MSUBAx', 0x0d: 'MSUBAy', 0x0e: 'MSUBAz', 0x0f: 'MSUBAw',
    0x10: 'ITOF0', 0x11: 'ITOF4', 0x12: 'ITOF12', 0x13: 'ITOF15',
    0x14: 'FTOI0', 0x15: 'FTOI4', 0x16: 'FTOI12', 0x17: 'FTOI15',
    0x18: 'MULAx', 0x19: 'MULAy', 0x1a: 'MULAz', 0x1b: 'MULAw',
    0x1c: 'MULAq', 0x1d: 'ABS',   0x1e: 'MULAi',  0x1f: 'CLIP',
    0x20: 'ADDAq', 0x21: 'MADDAq', 0x22: 'ADDAi', 0x23: 'MADDAi',
    0x24: 'SUBAq', 0x25: 'MSUBAq', 0x26: 'SUBAi', 0x27: 'MSUBAi',
    0x28: 'ADDA',  0x29: 'MADDA',  0x2a: 'MULA',   0x2c: 'SUBA',
    0x2d: 'MSUBA', 0x2e: 'OPMULA', 0x2f: 'NOP'
}

def decode_upper(w):
    d = (w >> 21) & 15
    ft = (w >> 16) & 31
    fs = (w >> 11) & 31
    fd = (w >> 6) & 31
    op = w & 63
    if op < 0x3c:
        name = UP.get(op, f'?up{op:x}')
        return {'type': 'UP', 'name': name, 'op': op, 'd': d, 'ft': ft, 'fs': fs, 'fd': fd}
    sp = ((w >> 6) & 31) << 2 | (w & 3)
    name = UPS.get(sp, f'?ups{sp:x}')
    return {'type': 'UPS', 'name': name, 'sp': sp, 'd': d, 'ft': ft, 'fs': fs, 'fd': fd}

def decode_lower(w, pc):
    op = w >> 25
    dd = (w >> 21) & 15
    ft = (w >> 16) & 31
    fs = (w >> 11) & 31
    fd = (w >> 6) & 31
    imm = s11(w & 0x7ff)
    if op == 0x40:
        fn = w & 63
        if fn in (0x30, 0x31, 0x32, 0x34, 0x35):
            name = {0x30:'IADD', 0x31:'ISUB', 0x32:'IADDI', 0x34:'IAND', 0x35:'IOR'}[fn]
            i5 = (w >> 6) & 31
            i5 = i5 - 32 if i5 & 16 else i5
            return {'type': 'INT', 'name': name, 'fd': fd, 'fs': fs, 'ft': ft, 'imm': i5}
        if fn >= 0x3c:
            sp = ((w >> 6) & 31) << 2 | (w & 3)
            return {'type': 'SPEC', 'sp': sp, 'dd': dd, 'ft': ft, 'fs': fs, 'fd': fd,
                    'fsf': (w >> 21) & 3, 'ftf': (w >> 23) & 3}
    tgt = (pc + 8 + imm * 8) & pc_mask()
    return {'type': 'NORM', 'op': op, 'dd': dd, 'ft': ft, 'fs': fs, 'fd': fd, 'imm': imm, 'tgt': tgt}

def blend_wrap(reg, val, d):
    lm = dest_mask(d)
    if lm == 0xf:
        return f"{reg} = {val};"
    elif lm == 0:
        return ""
    else:
        return f"{reg} = blend<0x{lm:x}>({reg}, {val});"

def translate_upper(u):
    name = u['name']
    d = u['d']
    fd = u['fd']
    fs = u['fs']
    ft = u['ft']
    
    if name == 'NOP':
        return ""
        
    fmac_map = {'ADD':'Add', 'SUB':'Sub', 'MUL':'Mul', 'MADD':'MAdd', 'MSUB':'MSub',
                'ADDA':'Add', 'SUBA':'Sub', 'MULA':'Mul', 'MADDA':'MAdd', 'MSUBA':'MSub'}
                
    for base in ('ADD', 'SUB', 'MUL', 'MADD', 'MSUB'):
        for i, c in enumerate(BC):
            if name == base + c:
                val = f"fmac<{fmac_map[base]}, {d}>(norm(vf[{fs}]), bc<{i}>(norm(vf[{ft}])), acc, fl)"
                return blend_wrap(f"vf[{fd}]", val, d)
            if name == base + 'A' + c:
                val = f"fmac<{fmac_map[base]}, {d}>(norm(vf[{fs}]), bc<{i}>(norm(vf[{ft}])), acc, fl)"
                return blend_wrap("acc", val, d)
                
    for base in ('ADD', 'SUB', 'MUL', 'MADD', 'MSUB'):
        if name == base:
            val = f"fmac<{fmac_map[base]}, {d}>(norm(vf[{fs}]), norm(vf[{ft}]), acc, fl)"
            return blend_wrap(f"vf[{fd}]", val, d)
        if name == base + 'A':
            val = f"fmac<{fmac_map[base]}, {d}>(norm(vf[{fs}]), norm(vf[{ft}]), acc, fl)"
            return blend_wrap("acc", val, d)
            
    for base in ('ADD', 'SUB', 'MUL', 'MADD', 'MSUB'):
        if name == base + 'q':
            val = f"fmac<{fmac_map[base]}, {d}>(norm(vf[{fs}]), splat(c.st.q), acc, fl)"
            return blend_wrap(f"vf[{fd}]", val, d)
        if name == base + 'Aq':
            val = f"fmac<{fmac_map[base]}, {d}>(norm(vf[{fs}]), splat(c.st.q), acc, fl)"
            return blend_wrap("acc", val, d)
            
    for base in ('ADD', 'SUB', 'MUL', 'MADD', 'MSUB'):
        if name == base + 'i':
            val = f"fmac<{fmac_map[base]}, {d}>(norm(vf[{fs}]), splat(i_reg), acc, fl)"
            return blend_wrap(f"vf[{fd}]", val, d)
        if name == base + 'Ai':
            val = f"fmac<{fmac_map[base]}, {d}>(norm(vf[{fs}]), splat(i_reg), acc, fl)"
            return blend_wrap("acc", val, d)
            
    # Outer product (interpreter OuterMul/OuterMSub): x = fs.y*ft.z, y = fs.z*ft.x,
    # z = fs.x*ft.y; the exact FMAC of crossed operands (w is outside dest xyz).
    if name in ('OPMULA', 'OPMSUB'):
        a = f"_mm_shuffle_ps(norm(vf[{fs}]), norm(vf[{fs}]), _MM_SHUFFLE(3, 0, 2, 1))"
        b = f"_mm_shuffle_ps(norm(vf[{ft}]), norm(vf[{ft}]), _MM_SHUFFLE(3, 1, 0, 2))"
        if name == 'OPMULA':
            return blend_wrap("acc", f"fmac<Mul, {d}>({a}, {b}, acc, fl)", d)
        return blend_wrap(f"vf[{fd}]", f"fmac<MSub, {d}>({a}, {b}, acc, fl)", d)

    if name == 'MAX':
        return blend_wrap(f"vf[{fd}]", f"vuMinMaxV(vf[{fs}], vf[{ft}], true)", d)
    if name == 'MAXi':
        return blend_wrap(f"vf[{fd}]", f"vuMinMaxV(vf[{fs}], splat(i_reg), true)", d)
    for i, c in enumerate(BC):
        if name == 'MAX' + c:
            return blend_wrap(f"vf[{fd}]", f"vuMinMaxV(vf[{fs}], bc<{i}>(vf[{ft}]), true)", d)
            
    if name == 'MINI':
        return blend_wrap(f"vf[{fd}]", f"vuMinMaxV(vf[{fs}], vf[{ft}], false)", d)
    if name == 'MINIi':
        return blend_wrap(f"vf[{fd}]", f"vuMinMaxV(vf[{fs}], splat(i_reg), false)", d)
    for i, c in enumerate(BC):
        if name == 'MINI' + c:
            return blend_wrap(f"vf[{fd}]", f"vuMinMaxV(vf[{fs}], bc<{i}>(vf[{ft}]), false)", d)
            
    if name == 'CLIP':
        return f"vl::LiftAccess::clipAt(c, cyc, vl::clipFlags(vf[{fs}], bc<3>(vf[{ft}])) & 0x3F);"
        
    if name == 'FTOI0':
        return blend_wrap(f"vf[{ft}]", f"vuFtoiV(norm(vf[{fs}]), 1.0f)", d)
    if name == 'FTOI4':
        return blend_wrap(f"vf[{ft}]", f"vuFtoiV(norm(vf[{fs}]), 16.0f)", d)
    if name == 'ITOF0':
        return blend_wrap(f"vf[{ft}]", f"vuItofV(vf[{fs}], 1.0f)", d)
    if name == 'ITOF4':
        return blend_wrap(f"vf[{ft}]", f"vuItofV(vf[{fs}], 16.0f)", d)
    if name == 'ABS':
        return blend_wrap(f"vf[{ft}]", f"vabs(vf[{fs}])", d)

    raise ValueError(f"Unknown upper op: {name}")

def translate_lower(l, lo, i_bit, pc):
    if i_bit:
        fval = f32(lo)
        return f"i_reg = vl::normf(f32(0x{lo:08x}u)); // {fval:g}"
        
    t = l['type']
    if t == 'INT':
        name = l['name']
        fd, fs, ft, imm = l['fd'], l['fs'], l['ft'], l['imm']
        if name == 'IADD':
            return f"if ({fd} != 0) vi[{fd}] = vl::i16(vi[{fs}] + vi[{ft}]);"
        if name == 'ISUB':
            return f"if ({fd} != 0) vi[{fd}] = vl::i16(vi[{fs}] - vi[{ft}]);"
        if name == 'IADDI':
            return f"if ({ft} != 0) vi[{ft}] = vl::i16(vi[{fs}] + ({imm}));"
        if name == 'IAND':
            return f"if ({fd} != 0) vi[{fd}] = vi[{fs}] & vi[{ft}];"
        if name == 'IOR':
            return f"if ({fd} != 0) vi[{fd}] = vi[{fs}] | vi[{ft}];"
            
    if t == 'SPEC':
        sp = l['sp']
        dd = l['dd']
        lm = dest_mask(dd)
        ft, fs = l['ft'], l['fs']
        if sp == 0x30: # MOVE
            return blend_wrap(f"vf[{ft}]", f"vf[{fs}]", dd)
        if sp == 0x31: # MR32
            return blend_wrap(f"vf[{ft}]", f"vl::mr32(vf[{fs}])", dd)
        if sp == 0x34: # LQI
            stmts = []
            bw = blend_wrap(f"vf[{ft}]", mem_lq(f"vi[{fs}]"), dd)
            if bw: stmts.append(bw)
            stmts.append(f"if ({fs} != 0) vi[{fs}] = vl::i16(vi[{fs}] + 1);")
            return " ".join(stmts)
        if sp == 0x35: # SQI
            stmts = [mem_sq(lm, f"vi[{ft}]", f"vf[{fs}]")]
            stmts.append(f"if ({ft} != 0) vi[{ft}] = vl::i16(vi[{ft}] + 1);")
            return " ".join(stmts)
        if sp == 0x36: # LQD
            stmts = [f"if ({fs} != 0) vi[{fs}] = vl::i16(vi[{fs}] - 1);"]
            bw = blend_wrap(f"vf[{ft}]", mem_lq(f"vi[{fs}]"), dd)
            if bw: stmts.append(bw)
            return " ".join(stmts)
        if sp == 0x37: # SQD
            stmts = [f"if ({ft} != 0) vi[{ft}] = vl::i16(vi[{ft}] - 1);"]
            stmts.append(mem_sq(lm, f"vi[{ft}]", f"vf[{fs}]"))
            return " ".join(stmts)
        if sp == 0x38: # DIV
            return f"{{ uint32_t di = 0; const float qv = vl::div(vl::lane<{l['fsf']}>(vf[{fs}]), vl::lane<{l['ftf']}>(vf[{ft}]), di); vl::LiftAccess::queueQAt(c, cyc, qv, 7u, di); }}"
        if sp == 0x3b: # WAITQ
            return ""  # WAITQ: the stall is applied by LiftAccess::begin
        if sp == 0x3c: # MTIR
            return f"if ({ft} != 0) vi[{ft}] = static_cast<int32_t>(static_cast<int16_t>(vl::bits<{l['fsf']}>(vf[{fs}]) & 0xFFFF));"
        if sp == 0x3d: # MFIR
            val = f"_mm_castsi128_ps(_mm_set1_epi32(static_cast<int32_t>(static_cast<int16_t>(vi[{fs}] & 0xFFFF))))"
            return blend_wrap(f"vf[{ft}]", val, dd)
        if sp == 0x3e: # ILWR
            comp = 0 if dd & 8 else (1 if dd & 4 else (2 if dd & 2 else 3))
            return f"if ({ft} != 0) vi[{ft}] = {mem_ilw(f'vi[{fs}]', comp)};"
        if sp == 0x3f: # ISWR
            lm = dest_mask(dd)
            return mem_isw(lm, f"vi[{fs}]", f"vi[{ft}]")
        if sp == 0x68: # XTOP
            vu1_only('XTOP', pc)
            return f"if ({ft} != 0) vi[{ft}] = static_cast<int32_t>(c.st.top & 0x3FFu);"
        if sp == 0x69: # XITOP
            vu1_only('XITOP', pc)
            return f"if ({ft} != 0) vi[{ft}] = static_cast<int32_t>(c.st.itop & 0x3FFu);"
        if sp == 0x6c: # XGKICK
            vu1_only('XGKICK', pc)
            return f"vl::LiftAccess::xgkickAt(c, cyc, vi[{fs}] & 0x3FF);"
        if sp == 0x7b: # WAITP
            vu1_only('WAITP', pc)
            return ""
            
    if t == 'NORM':
        op = l['op']
        dd = l['dd']
        lm = dest_mask(dd)
        ft, fs, imm, tgt = l['ft'], l['fs'], l['imm'], l['tgt']
        if op == 0x00: # LQ
            return blend_wrap(f"vf[{ft}]", mem_lq(f"vi[{fs}] + ({imm})"), dd)
        if op == 0x01: # SQ
            return mem_sq(lm, f"vi[{ft}] + ({imm})", f"vf[{fs}]")
        if op == 0x04: # ILW
            comp = 0 if dd & 8 else (1 if dd & 4 else (2 if dd & 2 else 3))
            return f"if ({ft} != 0) vi[{ft}] = {mem_ilw(f'vi[{fs}] + ({imm})', comp)};"
        if op == 0x05: # ISW
            return mem_isw(lm, f"vi[{fs}] + ({imm})", f"vi[{ft}]")
        if op == 0x08: # IADDIU
            imm15 = (lo & 0x7FF) | ((lo >> 10) & 0x7800)
            return f"if ({ft} != 0) vi[{ft}] = vl::i16(vi[{fs}] + ({imm15}));"
        if op == 0x09: # ISUBIU
            imm15 = (lo & 0x7FF) | ((lo >> 10) & 0x7800)
            return f"if ({ft} != 0) vi[{ft}] = vl::i16(vi[{fs}] - ({imm15}));"
        if op == 0x1c: # FCGET: committed CLIP pipeline, low twelve bits
            vi_t = ft & 15
            return f"if ({vi_t} != 0) vi[{vi_t}] = static_cast<int32_t>(c.st.clip & 0x0FFFu);"
        if op == 0x11: # FCSET
            return f"vl::LiftAccess::fcsetAt(c, cyc, 0x{lo & 0xFFFFFF:06x}u);"
        if op == 0x12: # FCAND
            return f"vi[1] = ((c.st.clip & 0x{lo & 0xFFFFFF:06x}u) != 0) ? 1 : 0;"
        if op == 0x13: # FCOR
            imm24 = lo & 0xFFFFFF
            return f"vi[1] = ((c.st.clip | 0x{imm24:06x}u) == 0xFFFFFFu) ? 1 : 0;"
        if op == 0x16: # FSAND: committed status flags (low twelve bits) AND imm12 (issue 025, VU0 program 0x0030)
            vi_t = ft & 15
            imm12 = (((lo >> 21) & 1) << 11) | (lo & 0x7FF)
            return f"vl::LiftAccess::flagsSync(c, fq, cyc); if ({vi_t} != 0) vi[{vi_t}] = static_cast<int32_t>((c.st.status & 0xFFFu) & 0x{imm12:03x}u);"
        if op == 0x1a: # FMAND
            return f"vl::LiftAccess::flagsSync(c, fq, cyc); if ({ft} != 0) vi[{ft}] = static_cast<int32_t>(c.st.mac & (vi[{fs}] & 0xFFFF));"
            
    raise ValueError(f"Unknown lower op at pc {pc:04x}: lo={lo:08x}")

def legacy_find_reachable(code, entry_pc):
    visited = set()
    worklist = [entry_pc]
    
    while worklist:
        pc = worklist.pop()
        while pc not in visited:
            visited.add(pc)
            lo, up = struct.unpack_from('<II', code, pc)
            e_bit = (up >> 30) & 1
            l = decode_lower(lo, pc)
            
            if not (up >> 31) and l['type'] == 'NORM' and l['op'] in (0x20, 0x28, 0x29, 0x2c, 0x2d, 0x2e, 0x2f):
                d_pc = pc + 8
                visited.add(d_pc)
                tgt = l['tgt']
                worklist.append(tgt)
                if l['op'] == 0x20: # Unconditional branch
                    break
                else: # Conditional branch: fallthrough to d_pc + 8
                    pc = d_pc + 8
                    continue
                    
            if e_bit:
                visited.add(pc + 8)
                break
                
            pc += 8
            
    return sorted(visited)

# Scratch proposal: prove every dynamic return before native emission.
_NATIVE_CALL_INFO = None
def find_reachable(code, entry_pc):
    global _NATIVE_CALL_INFO
    legacy = legacy_find_reachable(code, entry_pc)
    needs_calls = False
    for pc in legacy:
        lo, up = struct.unpack_from('<II', code, pc)
        l = decode_lower(lo, pc)
        if not (up >> 31) and l['type'] == 'NORM' and l['op'] in (0x21, 0x24, 0x25):
            needs_calls = True
    if not needs_calls:
        _NATIVE_CALL_INFO = None
        return legacy
    import native_call_analysis
    _NATIVE_CALL_INFO = native_call_analysis.analyze(code, entry_pc, decode_lower)
    return _NATIVE_CALL_INFO['reachable']

ACC_OPS = {'ADDA', 'SUBA', 'MULA', 'MADDA', 'MSUBA', 'OPMULA', 'OPMSUB', 'CLIP', 'NOP'}
def is_acc_op(name):
    for a in ACC_OPS:
        if name.startswith(a):
            return True
    return False

def upper_writes_vf(u):
    """VF register an upper instruction writes (None for ACC, CLIP and NOP)."""
    if u['type'] not in ('UP', 'UPS'):
        return None
    name = u['name']
    if is_acc_op(name) or u['d'] == 0:
        return None
    if u['type'] == 'UPS':
        # ITOF/FTOI/ABS write ft; the fd field is part of the opcode.
        if name.startswith(('ITOF', 'FTOI', 'ABS')):
            return u['ft'] or None
        return None
    return u['fd'] or None

def lower_reads_vf(l):
    if l['type'] == 'SPEC':
        sp = l['sp']
        if sp in (0x30, 0x31, 0x35, 0x37, 0x3c): # MOVE, MR32, SQI, SQD, MTIR
            return [l['fs']]
        if sp in (0x38, 0x3a): # DIV, RSQRT
            return [l['fs'], l['ft']]
        if sp == 0x39: # SQRT
            return [l['ft']]
    elif l['type'] == 'NORM':
        if l['op'] == 0x01: # SQ
            return [l['fs']]
    return []

def lower_writes_vf(l):
    if l['type'] == 'SPEC':
        if l['sp'] in (0x30, 0x31, 0x34, 0x36, 0x3d) and l['dd'] != 0: # MOVE, MR32, LQI, LQD, MFIR
            return l['ft'] or None
    elif l['type'] == 'NORM':
        if l['op'] == 0x00 and l['dd'] != 0: # LQ
            return l['ft'] or None
    return None

# MAC/status flags are only observable through these lower instructions:
# FSEQ FSSET FSAND FSOR FMEQ FMAND FMOR. A segment without them only needs the
# final committed flags, which it accumulates locally (same result as queueing
# every FMAC result: FMAC and FDIV commits touch disjoint status bits).
MAC_STATUS_READERS = (0x14, 0x15, 0x16, 0x17, 0x18, 0x1a, 0x1b)
QUEUE_FMAC_FLAGS = True

# Issue 025: FMAC flag results nothing can observe. A result lands kFmacLatency (4) cycles after its pair and stays
# visible until the next result lands. If the next flag-producing FMAC follows in straight-line code and no flag
# reader, branch or stop (E/D/T bit) comes from the pair after this one up to three pairs after that next FMAC,
# no instruction (and no later run) can see this result: it is not queued, and its sticky bits (cur | extra, all
# that commit ORs into status bits 6-11) ride on the next queued result, which commits no earlier than any reader
# that could have seen them.
DEAD_PUSHES = set()
_BRANCH_OPS = (0x20, 0x21, 0x24, 0x25, 0x28, 0x29, 0x2c, 0x2d, 0x2e, 0x2f)

# Issue 025 v10 V2: the same rule along every control-flow path. A branch's delay slot runs next, then its target
# and (conditional) the fall-through; JR goes to its proven targets; an E bit ends the program after its delay
# slot. A result is dead when on every path from the pair after it the next flag-producing FMAC comes before any
# flag reader, and no reader comes in the three pairs after that FMAC either; an end of program, a D/T bit or a
# jump without proven targets on the way keeps the push.
_END = -1

def pair_successors(code, reachable):
    reach = set(reachable)
    succ = {}
    # A jump straight into a delay slot has no branch pending: the pair after the slot runs next.
    jumped_to = set()
    for pc in reachable:
        lo, up = struct.unpack_from('<II', code, pc)
        l = decode_lower(lo, pc)
        if not (up >> 31) & 1 and l['type'] == 'NORM' and l['op'] in _BRANCH_OPS:
            if l['op'] == 0x24:
                jumped_to.update((_NATIVE_CALL_INFO['jr_targets'].get(pc) or ()) if _NATIVE_CALL_INFO else ())
            elif l['op'] != 0x25:
                jumped_to.add(l['tgt'])
    for pc in reachable:
        lo, up = struct.unpack_from('<II', code, pc)
        if pc not in succ:
            succ[pc] = {pc + 8}
        l = decode_lower(lo, pc)
        d_pc = pc + 8
        if (up >> 30) & 1:
            succ[pc] = {d_pc}
            succ[d_pc] = {_END}
        elif not (up >> 31) & 1 and l['type'] == 'NORM' and l['op'] in _BRANCH_OPS:
            succ[pc] = {d_pc}
            if l['op'] == 0x24:
                jr = _NATIVE_CALL_INFO['jr_targets'].get(pc) if _NATIVE_CALL_INFO else None
                succ[d_pc] = set(jr) if jr else {None}
            elif l['op'] == 0x25:
                succ[d_pc] = {None}
            elif l['op'] in (0x20, 0x21):
                succ[d_pc] = {l['tgt']}
            else:
                succ[d_pc] = {l['tgt'], pc + 16}
            if d_pc in jumped_to:
                succ[d_pc].add(pc + 16)
    for pc, nxt in succ.items():
        succ[pc] = {q if q in (_END, None) or q in reach else None for q in nxt}
    return succ

def find_dead_pushes(code, reachable):
    info = {}
    for pc in reachable:
        lo, up = struct.unpack_from('<II', code, pc)
        i_bit = (up >> 31) & 1
        l = decode_lower(lo, pc)
        m = re.search(r'fmac<\w+, (\d+)>', translate_upper(decode_upper(up & 0x07ffffff)))
        lower_norm = not i_bit and l['type'] == 'NORM'
        info[pc] = (m is not None and int(m.group(1)) != 0,
                    lower_norm and l['op'] in MAC_STATUS_READERS,
                    bool((up >> 27) & 0x3))
    succ = pair_successors(code, reachable)
    dead = set()
    for pc in reachable:
        push, _, stop = info[pc]
        if not push or stop:
            continue
        # States: (pair, n) with n = 0 before the next push, n = 1..3 the pairs still checked after it.
        todo = [(q, 0) for q in succ[pc]]
        seen = set()
        ok = True
        while todo and ok:
            q, n = todo.pop()
            if (q, n) in seen:
                continue
            seen.add((q, n))
            if q is None or q == _END or info[q][1] or info[q][2]:
                ok = False
                break
            if n == 0:
                nxt = 1 if info[q][0] else 0
            else:
                nxt = n + 1 if n < 3 else None
            if nxt is not None:
                todo.extend((r, nxt) for r in succ[q])
        if ok:
            dead.add(pc)
    return dead

def fmac_flag_code(pc=None):
    if QUEUE_FMAC_FLAGS:
        if DEAD_PUSHES and pc in DEAD_PUSHES:
            return "    if (fl.pending) { fl.pending = false; stk |= fl.cur | fl.extra; }"
        if DEAD_PUSHES:
            return "    if (fl.pending) { fl.pending = false; vl::LiftAccess::flagsPush(c, fq, cyc, fl.mac, fl.cur, fl.extra | stk); stk = 0; }"
        return "    if (fl.pending) { fl.pending = false; vl::LiftAccess::flagsPush(c, fq, cyc, fl.mac, fl.cur, fl.extra); }"
    return "    if (fl.pending) { fl.pending = false; lastMac = fl.mac; lastCur = fl.cur; sticky6 |= fl.cur | fl.extra; anyFmac = true; }"

def emit_pair(u, l, lo, i_bit, pc, out):
    shadow = None
    w = upper_writes_vf(u)
    # Like the interpreter's upperVfShadowReg: when the lower instruction reads
    # or writes the register the upper writes, it sees the old value and the
    # upper result wins.
    if w is not None and (w in lower_reads_vf(l) or w == lower_writes_vf(l)):
        shadow = w
        out.append(f"    shadow_vf = vf[{shadow}];")
    up_code = translate_upper(u)
    lo_code = translate_lower(l, lo, i_bit, pc)
    if shadow is not None:
        lo_code = lo_code.replace(f"vf[{shadow}]", "shadow_vf")
    if up_code:
        out.append(f"    {up_code}")
        if 'fmac<' in up_code:
            out.append(fmac_flag_code(pc))
    if lo_code: out.append(f"    {lo_code}")

def timing_begin(code, pc, timings):
    """Stall checks before a pair, with the pair's read slots as constants."""
    lo, up = struct.unpack_from('<II', code, pc)
    t = vu1_timing.pair_timing(lo, up)
    timings.append((pc, t))
    lines = []
    if t['reads']:
        checks = ' '.join(f"r_ = rdy[{slot}] > r_ ? rdy[{slot}] : r_;" for slot in t['reads'])
        lines.append(f"    {{ uint64_t r_ = cyc; {checks} if (r_ > cyc) vl::LiftAccess::stall(c, cyc, r_); }}")
    if t['flags'] & (vu1_timing.FLAG_FDIV | vu1_timing.FLAG_WAITQ):
        lines.append("    vl::LiftAccess::fdivWait(c, cyc);")
    if t['flags'] & (vu1_timing.FLAG_EFU | vu1_timing.FLAG_WAITP):
        raise ValueError(f'EFU timing at {pc:04x} is not supported by the lifter')
    if t['flags'] & vu1_timing.FLAG_XGKICK:
        lines.append("    vl::LiftAccess::kickWait(c, cyc);")
    if t['firstVi'] and t['flags'] & vu1_timing.FLAG_BRANCH_DELAY:
        lines.append(f"    ov = vi[{t['firstVi']}];")
    return lines

def timing_end(code, pc):
    """Write latencies, branch VI delay and the cycle step after a pair."""
    lo, up = struct.unpack_from('<II', code, pc)
    t = vu1_timing.pair_timing(lo, up)
    lines = []
    if t['writes']:
        lines.append("    " + " ".join(f"rdy[{slot}] = cyc + {lat};" for slot, lat in t['writes']))
        lines.append(f"    if (cyc + {t['maxLat']} > hz) hz = cyc + {t['maxLat']};")
    if t['firstVi'] and t['flags'] & vu1_timing.FLAG_BRANCH_DELAY:
        lines.append(f"    bbValid = true; bbReg = {t['firstVi']}; bbVal = ov;")
    else:
        lines.append("    bbValid = false;")
    lines.append("    vl::LiftAccess::tick(c, cyc);")
    return lines

# ---- Static timing (issue 001) ---------------------------------------------
# A region is a run of straight-line pairs (no branch, delay slot, E-bit,
# FDIV/EFU/XGKICK wait, label after its first pair). Its stalls are computed
# here, once; at run time a guard checks that no register produced before the
# region delays it (the interpreter would not stall on an external operand),
# then the pairs run at constant cycle offsets without per-pair bookkeeping.
# If the guard fails, the unchanged per-pair code below runs instead.
STATIC_SPECIAL_FLAGS = (vu1_timing.FLAG_FDIV | vu1_timing.FLAG_EFU | vu1_timing.FLAG_WAITQ |
                        vu1_timing.FLAG_WAITP | vu1_timing.FLAG_XGKICK)
STATIC_OBSERVERS = ('c.st.q', 'c.st.p', 'c.st.clip', 'c.st.mac', 'c.st.status', 'LiftAccess::', 'vl::sq',
                    'vl::isw', 'flagsPush', 'flagsSync')

def static_regions(code, blocks, targets, branch_info, branch_delay_slots):
    special = set(branch_info) | set(branch_delay_slots)
    for block in blocks:
        for i, pc in enumerate(block):
            lo, up = struct.unpack_from('<II', code, pc)
            if (up >> 30) & 1:
                special.add(pc)
                if i + 1 < len(block):
                    special.add(block[i + 1])
            if vu1_timing.pair_timing(lo, up)['flags'] & STATIC_SPECIAL_FLAGS:
                special.add(pc)
    regions = []
    for block in blocks:
        cur = []
        for pc in block:
            if pc in special or (pc in targets and cur):
                if len(cur) >= 2:
                    regions.append(cur)
                cur = []
            if pc not in special:
                cur.append(pc)
        if len(cur) >= 2:
            regions.append(cur)
    return regions

def simulate_region(code, region):
    """Issue offsets, external read guards, final ready offsets and horizon of a region."""
    local, guards, issue, hz = {}, {}, [], None
    t = -1
    for pc in region:
        lo, up = struct.unpack_from('<II', code, pc)
        tm = vu1_timing.pair_timing(lo, up)
        k = t + 1
        for slot in tm['reads']:
            if slot in local:
                k = max(k, local[slot])
        for slot in tm['reads']:
            if slot not in local and slot not in guards:
                guards[slot] = k
        for slot, lat in tm['writes']:
            local[slot] = k + lat
        if tm['writes']:
            hz = max(hz or 0, k + tm['maxLat'])
        issue.append(k)
        t = k
    return issue, guards, local, hz, t + 1

STATIC_QUIET_OBSERVERS = ('vl::sq', 'vl::isw', 'clipAt', 'fcsetAt', 'c.st.clip', 'c.st.q', 'c.st.p', 'c.st.status')

def emit_static_region(code, region, emit_one, out):
    start = region[0]
    issue, guards, final, hz, length = simulate_region(code, region)
    bodies = []
    for pc in region:
        body = []
        emit_one(pc, body)
        bodies.append(body)
    text = '\n'.join(line for body in bodies for line in body)
    # Quiet: by the guard no result queued before the region commits before it
    # ends, the region's FMAC flags stay in the segment's local queue, and the
    # only results it queues in the interpreter's pipeline are clip flags. Then
    # Q, P, MAC and status cannot change underneath it; only the clip pipeline
    # (caught up before each CLIP/FCSET and each clip read) and PATH1 (caught up
    # before stores and at the end) move.
    calls = set(re.findall(r'LiftAccess::(\w+)', text))
    quiet = calls <= {'flagsPush', 'flagsSync', 'clipAt', 'fcsetAt'}
    # Most FMAC results in flight at once in the local flag queue: pushes in
    # any window of kFmacLatency cycles (older ones are drained first).
    push_at = [k for k, body in zip(issue, bodies) if any('flagsPush(' in line for line in body)]
    window = max((sum(1 for q in push_at if k - 4 < q <= k) for k in push_at), default=0)
    conds = [f"rdy[{slot}] <= t0 + {k}" for slot, k in sorted(guards.items(), key=lambda x: (x[1], x[0]))]
    if quiet:
        conds.append(f"vl::LiftAccess::quietUntil(c, t0 + {length})")
        if window:
            conds.append(f"vl::LiftAccess::flagRoom(fq, {window}u)")
    out.append(f"    // static region {start:04x}-{region[-1]:04x}: {len(region)} pairs, {length} cycles"
               f"{', quiet' if quiet else ''}")
    out.append("    { const uint64_t t0 = cyc;")
    out.append(f"    if ({' && '.join(conds) or 'true'}) {{")
    out.append("    uint64_t synced = t0;")
    for pc, k, body in zip(region, issue, bodies):
        lo, up = struct.unpack_from('<II', code, pc)
        t = vu1_timing.pair_timing(lo, up)
        observers = STATIC_QUIET_OBSERVERS if quiet else STATIC_OBSERVERS
        out.append(f"    // {pc:04x} @+{k}")
        out.append(f"    cyc = t0 + {k};")
        if any(o in line for line in body for o in observers):
            out.append("    vl::LiftAccess::catchUp(c, synced, cyc);")
        if t['firstVi'] and t['flags'] & vu1_timing.FLAG_BRANCH_DELAY:
            out.append(f"    ov = vi[{t['firstVi']}];")
        out.extend(body)
        if t['firstVi'] and t['flags'] & vu1_timing.FLAG_BRANCH_DELAY:
            out.append(f"    bbValid = true; bbReg = {t['firstVi']}; bbVal = ov;")
        else:
            out.append("    bbValid = false;")
    out.append(f"    cyc = t0 + {length};")
    if final:
        out.append("    " + " ".join(f"rdy[{slot}] = t0 + {k};" for slot, k in sorted(final.items())))
    if hz is not None:
        out.append(f"    if (t0 + {hz} > hz) hz = t0 + {hz};")
    out.append("    vl::LiftAccess::catchUp(c, synced, cyc);")
    out.append(f"    goto rend_{start:04x};")
    out.append("    } }")

def translate_segment(code, entry_pc, fn_name, static_timing=False):
    global QUEUE_FMAC_FLAGS, DEAD_PUSHES
    reachable = find_reachable(code, entry_pc)
    QUEUE_FMAC_FLAGS = False
    for rpc in reachable:
        rlo, rup = struct.unpack_from('<II', code, rpc)
        if (rup >> 31) & 1:
            continue
        rl = decode_lower(rlo, rpc)
        if rl['type'] == 'NORM' and rl['op'] in MAC_STATUS_READERS:
            QUEUE_FMAC_FLAGS = True
    
    branch_info = {}
    call_info = {}
    branch_delay_slots = set()
    targets = set()
    
    for pc in reachable:
        lo, up = struct.unpack_from('<II', code, pc)
        l = decode_lower(lo, pc)
        if not (up >> 31) and l['type'] == 'NORM' and l['op'] in (0x20, 0x21, 0x24, 0x28, 0x29, 0x2c, 0x2d, 0x2e, 0x2f):
            op = l['op']
            tgt = l['tgt']
            ft, fs = l['ft'], l['fs']
            if op in (0x20, 0x21, 0x24): cond = "true"
            elif op == 0x28: cond = f"(brvi({ft}) == brvi({fs}))"
            elif op == 0x29: cond = f"(brvi({ft}) != brvi({fs}))"
            elif op == 0x2c: cond = f"(brvi({fs}) < 0)"
            elif op == 0x2d: cond = f"(brvi({fs}) > 0)"
            elif op == 0x2e: cond = f"(brvi({fs}) <= 0)"
            elif op == 0x2f: cond = f"(brvi({fs}) >= 0)"
            branch_info[pc] = (tgt, cond)
            branch_delay_slots.add(pc + 8)
            if op == 0x24:
                if not _NATIVE_CALL_INFO or pc not in _NATIVE_CALL_INFO['jr_targets']:
                    raise ValueError(f'unproven JR at {pc:04x}')
                targets.update(_NATIVE_CALL_INFO['jr_targets'][pc])
            else:
                targets.add(tgt)
            if op in (0x21, 0x24):
                call_info[pc] = (op, ft & 15, fs & 15)
            
    if call_info or entry_pc != reachable[0]:
        targets.add(entry_pc)

    blocks = []
    curr_block = []
    for pc in reachable:
        if not curr_block or pc == curr_block[-1] + 8:
            curr_block.append(pc)
        else:
            blocks.append(curr_block)
            curr_block = [pc]
    if curr_block:
        blocks.append(curr_block)
        
    out = []
    timings = []
    out.append(f"void {fn_name}(vl::Run &c)")
    out.append("{")
    out.append("    using namespace vl;")
    out.append("@@TIMING_TABLE@@")
    out.append("    vl::checkReset(c);")
    out.append("    alignas(16) V vf[32];")
    out.append("    for (int i = 0; i < 32; ++i) vf[i] = _mm_loadu_ps(c.st.vf[i]);")
    out.append("    int32_t vi[16];")
    out.append("    for (int i = 0; i < 16; ++i) vi[i] = c.st.vi[i];")
    out.append("    V acc = _mm_loadu_ps(c.st.acc);")
    out.append("    V shadow_vf = _mm_setzero_ps();")
    out.append("    float i_reg = c.st.i;")
    out.append("    Flags fl{ c.st.status & 0xFu, c.st.mac, (c.st.status >> 6) & 0x3Fu };")
    if not QUEUE_FMAC_FLAGS:
        out.append("    uint32_t lastMac = 0, lastCur = 0, sticky6 = 0; bool anyFmac = false;")
    out.append("    uint8_t *mem = c.mem;")
    out.append("    uint64_t *rdy = vl::LiftAccess::readyTable(c);")
    out.append("    uint64_t cyc = vl::LiftAccess::cycle(c), hz = 0;")
    DEAD_PUSHES = find_dead_pushes(code, reachable) if QUEUE_FMAC_FLAGS else set()
    if QUEUE_FMAC_FLAGS:
        out.append("    vl::LiftAccess::FlagQueue fq; vl::LiftAccess::flagsEnter(c, fq);")
    if DEAD_PUSHES:
        out.append("    uint32_t stk = 0; // sticky bits of FMAC results nothing can observe (issue 025)")
    out.append("    bool bbValid = false; uint8_t bbReg = 0; int32_t bbVal = 0, ov = 0;")
    out.append("    auto brvi = [&](int r) -> int32_t { return r == 0 ? 0 : ((bbValid && bbReg == r) ? bbVal : vi[r]); };")
    for bpc in sorted(branch_info.keys()):
        out.append(f"    bool taken_{bpc:04x} = false;")
        if bpc in call_info and call_info[bpc][0] == 0x24:
            out.append(f"    uint32_t jump_{bpc:04x} = 0;")
    out.append("    auto checkpoint = [&] { if (!vl::resetRequested()) return;")
    out.append("        for (int n = 1; n < 32; ++n) _mm_storeu_ps(c.st.vf[n], vf[n]);")
    out.append("        for (int n = 1; n < 16; ++n) c.st.vi[n] = vi[n];")
    out.append("        _mm_storeu_ps(c.st.acc, acc);")
    if not QUEUE_FMAC_FLAGS:
        out.append("        if (anyFmac) { c.st.mac = lastMac; c.st.status = (c.st.status & 0xFF0u) | lastCur | (sticky6 << 6); }")
    if QUEUE_FMAC_FLAGS:
        out.append("        vl::LiftAccess::flagsLeave(c, fq, cyc);")
    out.append("        c.st.i = i_reg; vl::LiftAccess::leave(c, cyc, hz, bbValid, bbReg, bbVal); throw vl::ResetCanceled{}; };")
    out.append("    bool resetCanceled = false;")
    out.append("    try {")
    # Execution starts at the entry, which is not the first emitted block when the
    # segment also reaches shared code below it.
    if call_info or entry_pc != reachable[0]:
        out.append(f"    goto loc_{entry_pc:04x};")
    out.append("")

    region_start, region_end = {}, {}
    if static_timing:
        for r in static_regions(code, blocks, targets, branch_info, branch_delay_slots):
            region_start[r[0]] = r
            region_end[r[-1]] = r[0]

    def emit_one(rpc, body):
        rlo, rup = struct.unpack_from('<II', code, rpc)
        emit_pair(decode_upper(rup & 0x07ffffff), decode_lower(rlo, rpc), rlo, (rup >> 31) & 1, rpc, body)

    for block in blocks:
        i = 0
        while i < len(block):
            pc = block[i]
            lo, up = struct.unpack_from('<II', code, pc)
            e_bit = (up >> 30) & 1
            i_bit = (up >> 31) & 1
            u = decode_upper(up & 0x07ffffff)
            l = decode_lower(lo, pc)
            
            if pc in targets:
                out.append(f"loc_{pc:04x}:;")
                out.append("    checkpoint();")

            if pc in region_start:
                emit_static_region(code, region_start[pc], emit_one, out)
                
            if pc in branch_info:
                tgt, cond = branch_info[pc]
                out.append(f"    // {pc:04x}: branch to {tgt:04x}")
                out.extend(timing_begin(code, pc, timings))
                up_code = translate_upper(u)
                if up_code:
                    out.append(f"    {up_code}")
                    if 'fmac<' in up_code:
                        out.append(fmac_flag_code(pc))
                out.append(f"    taken_{pc:04x} = {cond};")
                if pc in call_info:
                    op, ft, fs = call_info[pc]
                    if op == 0x24:
                        out.append(f"    jump_{pc:04x} = (static_cast<uint32_t>(static_cast<uint16_t>(brvi({fs}))) * 8u) & 0x{pc_mask():04X}u;")
                    elif ft:
                        out.append(f"    vi[{ft}] = {(pc + 16) // 8};")
                out.extend(timing_end(code, pc))
                
                i += 1
                d_pc = block[i]
                d_lo, d_up = struct.unpack_from('<II', code, d_pc)
                d_i_bit = (d_up >> 31) & 1
                d_u = decode_upper(d_up & 0x07ffffff)
                d_l = decode_lower(d_lo, d_pc)
                
                if d_pc in targets:
                    out.append(f"loc_{d_pc:04x}:;")
                    out.append("    checkpoint();")
                out.append(f"    // {d_pc:04x}: delay slot for {pc:04x}")
                out.extend(timing_begin(code, d_pc, timings))
                emit_pair(d_u, d_l, d_lo, d_i_bit, d_pc, out)
                out.extend(timing_end(code, d_pc))
                if pc in call_info and call_info[pc][0] == 0x24:
                    out.append(f"    switch (jump_{pc:04x}) {{")
                    for target in sorted(_NATIVE_CALL_INFO['jr_targets'][pc]):
                        out.append(f"    case 0x{target:04x}u: goto loc_{target:04x};")
                    out.append('    default: throw std::logic_error("verified VU JR target proof violated");')
                    out.append('    }')
                else:
                    out.append(f"    if (taken_{pc:04x}) {{ taken_{pc:04x} = false; goto loc_{tgt:04x}; }}")
                i += 1
                continue
                
            out.append(f"    // {pc:04x}")
            out.extend(timing_begin(code, pc, timings))
            emit_pair(u, l, lo, i_bit, pc, out)
            out.extend(timing_end(code, pc))
            if pc in region_end:
                out.append(f"rend_{region_end[pc]:04x}:;")
            
            if e_bit:
                i += 1
                d_pc = block[i]
                d_lo, d_up = struct.unpack_from('<II', code, d_pc)
                d_i_bit = (d_up >> 31) & 1
                d_u = decode_upper(d_up & 0x07ffffff)
                d_l = decode_lower(d_lo, d_pc)
                if d_pc in targets:
                    out.append(f"loc_{d_pc:04x}:;")
                    out.append("    checkpoint();")
                out.append(f"    // {d_pc:04x}: delay slot for e-bit at {pc:04x}")
                out.extend(timing_begin(code, d_pc, timings))
                emit_pair(d_u, d_l, d_lo, d_i_bit, d_pc, out)
                out.extend(timing_end(code, d_pc))
                final_pc = d_pc + 8
                out.append(f"    c.st.pc = 0x{final_pc:04x};")
                out.append("    goto done;")
                i += 1
                continue
                
            i += 1

    out.append("done:;")
    out.append("    } catch (const vl::ResetCanceled &) { resetCanceled = true; }")
    if QUEUE_FMAC_FLAGS:
        out.append("    vl::LiftAccess::flagsLeave(c, fq, cyc);")
    out.append("    vl::LiftAccess::leave(c, cyc, hz, bbValid, bbReg, bbVal);")
    out.append("    for (int i = 1; i < 32; ++i) _mm_storeu_ps(c.st.vf[i], vf[i]);")
    out.append("    c.st.vf[0][0] = 0.0f; c.st.vf[0][1] = 0.0f; c.st.vf[0][2] = 0.0f; c.st.vf[0][3] = 1.0f;")
    out.append("    for (int i = 1; i < 16; ++i) c.st.vi[i] = vi[i];")
    out.append("    c.st.vi[0] = 0;")
    out.append("    _mm_storeu_ps(c.st.acc, acc);")
    out.append("    c.st.i = i_reg;")
    if not QUEUE_FMAC_FLAGS:
        out.append("    if (anyFmac) { c.st.mac = lastMac; c.st.status = (c.st.status & 0xFF0u) | lastCur | (sticky6 << 6); }")
    out.append("    if (resetCanceled) throw vl::ResetCanceled{};")
    out.append("}")
    # The embedded timing must equal the interpreter's decode, else the
    # program is interpreted (vl::LiftAccess::validate).
    table = [f"    static const uint16_t kPcs[] = {{{', '.join(f'0x{p:04x}' for p, _ in timings)}}};",
             "    static const VuLiftTiming kTiming[] = {",
             ",\n".join(f"        {vu1_timing.timing_initializer(t)}" for _, t in timings),
             "    };",
             "    static std::atomic<int> s_timingState{0};",
             f"    if (!vl::LiftAccess::validate(c, kTiming, kPcs, {len(timings)}u, s_timingState, \"{fn_name}\")) {{ c.declined = true; return; }}"]
    i = out.index("@@TIMING_TABLE@@")
    out[i:i + 1] = table
    # Every program uses the inlined per-lane fast path (vl::fmac, always inline). Issue 001 M12 had the very large
    # programs call it out of line (inlined ~37 % slower then); issue 025 v10 V4b measured the opposite on today's
    # code: -19.5 % on the real runs of the 0x0800 programs, exact.
    return "\n".join(out)

def static_enabled(name):
    # DW5XL_VU1_STATIC: 'all' (default), 'none', or a comma list of segments.
    sel = os.environ.get('DW5XL_VU1_STATIC', 'all')
    return sel == 'all' or name in sel.split(',')

# Shared preamble of every generated lift file (VU1 and, issue 025, VU0): includes and helpers.
LIFT_PREAMBLE = """#include "runtime/ps2_vu1_lift.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace {

using vl::V;

inline float vuMinMax(float left, float right, bool maximum)
{
    uint32_t a, b;
    std::memcpy(&a, &left, sizeof(a));
    std::memcpy(&b, &right, sizeof(b));
    const auto key = [](uint32_t bits) {
        return (bits & 0x80000000u) ? ~bits : (bits ^ 0x80000000u);
    };
    const bool leftFirst = key(a) < key(b);
    const uint32_t selected = maximum ? (leftFirst ? b : a) : (leftFirst ? a : b);
    float result;
    std::memcpy(&result, &selected, sizeof(result));
    return result;
}

inline V vuMinMaxV(V a, V b, bool maximum)
{
    alignas(16) float fa[4], fb[4], fr[4];
    _mm_store_ps(fa, a);
    _mm_store_ps(fb, b);
    for (int i = 0; i < 4; ++i)
        fr[i] = vuMinMax(fa[i], fb[i], maximum);
    return _mm_load_ps(fr);
}

inline int32_t vuFloatToInt(float value, float scale)
{
    const double scaled = static_cast<double>(value) * static_cast<double>(scale);
    if (scaled >= static_cast<double>(std::numeric_limits<int32_t>::max()))
        return std::numeric_limits<int32_t>::max();
    if (scaled <= static_cast<double>(std::numeric_limits<int32_t>::min()))
        return std::numeric_limits<int32_t>::min();
    return static_cast<int32_t>(scaled);
}

inline V vuFtoiV(V a, float scale)
{
    alignas(16) float fa[4];
    alignas(16) uint32_t ir[4];
    _mm_store_ps(fa, a);
    for (int i = 0; i < 4; ++i)
        ir[i] = static_cast<uint32_t>(vuFloatToInt(fa[i], scale));
    return _mm_castsi128_ps(_mm_load_si128(reinterpret_cast<const __m128i *>(ir)));
}

inline V vuItofV(V a, float scale)
{
    alignas(16) uint32_t ir[4];
    alignas(16) float fr[4];
    _mm_store_si128(reinterpret_cast<__m128i *>(ir), _mm_castps_si128(a));
    for (int i = 0; i < 4; ++i)
        fr[i] = static_cast<float>(static_cast<int32_t>(ir[i])) / scale;
    return _mm_load_ps(fr);
}

} // namespace
"""

if __name__ == '__main__':
    def load(path):
        with open(path, 'rb') as f:
            return f.read()

    def fnv1a_64(data):
        h = 1469598103934665603
        for b in data:
            h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
        return h

    input_mode = os.environ.get('DW5XL_VU1_INPUT', 'disc')
    if input_mode not in ('disc', 'capture'):
        raise ValueError('DW5XL_VU1_INPUT must be disc or capture')
    disc_mode = input_mode == 'disc'
    if disc_mode:
        from disc_vu_inputs import load_disc_bins, IMAGE_REGISTRATIONS
        bins, covered = load_disc_bins(os.environ.get('DW5XL_VU1_ELF', 'game/dw5xl/SLES_538.60'))
    else:
        bins = {}
        import glob
        for p in glob.glob(os.environ.get('DW5XL_VU1_CENSUS_GLOB', 'notes/evidence/battle_real_census.tsv.*.bin')):
            h = p.split('.')[-2]
            bins[h] = load(p)
        if not bins:
            raise ValueError('No captured VU1 microcode found')

    targets = [('dd3d1361f34c85a3', 672, 3120, 'lift_opening_dd3d_02a0'), ('4dedc9b811a016b1', 680, 2480, 'lift_opening_4ded_02a8'), ('4a6dbc6bbd3f1fbb', 784, 1352, 'lift_opening_4a6d_0310'), ('4a6dbc6bbd3f1fbb', 256, 1352, 'lift_opening_4a6d_0100'), ('4a6dbc6bbd3f1fbb', 0, 248, 'lift_opening_4a6d_0000'), ('881c2abb30cea5ce', 2352, 5944, 'lift_opening_881c_0930'), ('881c2abb30cea5ce', 0, 104, 'lift_opening_881c_0000'), ] + [('99119dfac138459b', 2048, 5200, 'lift_battle_calls_99119dfa_0800'), ('99119dfac138459b', 9216, 12976, 'lift_battle_calls_99119dfa_2400'), ('eb1d3af053afc5c2', 2048, 5128, 'lift_battle_calls_eb1d3af0_0800'), ('eb1d3af053afc5c2', 9216, 12904, 'lift_battle_calls_eb1d3af0_2400')] + [
        ('1ad6eefff0c2e7e5', 0x0800, 0x0a78, 'lift_1ad6_0800'),
        ('1ad6eefff0c2e7e5', 0x1000, 0x12b0, 'lift_1ad6_1000'),
        ('da28291ffe0b3334', 0x0000, 0x0448, 'lift_da28_0000'),
        ('da28291ffe0b3334', 0x0010, 0x0950, 'lift_da28_0010'),
        ('da28291ffe0b3334', 0x0050, 0x1070, 'lift_da28_0050'),
        ('1b91674603e323f2', 0x0800, 0x0c48, 'lift_1b91_0800'),
        ('3249a2515e770fcc', 0x0800, 0x0c48, 'lift_battle_actor_skinning_0800'),
        ('baa8e61743cea7a2', 0x0800, 0x0b18, 'lift_battle_mesh_0800'),
        ('baa8e61743cea7a2', 0x2400, 0x27e0, 'lift_battle_fx_2400'),
        ('64127817c106b85a', 0x2400, 0x2750, 'lift_battle_particles_2400'),
        ('46d6ccb25f6b8d32', 0x2400, 0x29b8, 'lift_battle_terrain_2400'),
        ('46d6ccb25f6b8d32', 0x0800, 0x0e18, 'lift_battle_water_0800'),
        ('60b13c6c3365e06a', 0x2400, 0x2878, 'lift_battle_shadows_2400'),
        ('baa8e61743cea7a2', 0x0040, 0x00f0, 'lift_battle_init_0040'),
        ('baa8e61743cea7a2', 0x00f0, 0x0230, 'lift_battle_stream_00f0'),
        ('84a67c1d4789e87b', 0x0230, 0x0370, 'lift_battle_clip_0230'),
        ('60b13c6c3365e06a', 0x0800, 0x0b88, 'lift_battle_shadows_0800'),
        ('84a67c1d4789e87b', 0x0000, 0x0028, 'lift_battle_stream_0000'),
        ('3a4f0cd1c30a3aef', 0x0370, 0x04b0, 'lift_battle_clip_0370'),
        ('3a4f0cd1c30a3aef', 0x04b0, 0x05f0, 'lift_battle_clip_04b0'),
        ('da28291ffe0b3334', 0x0060, 0x00f0, 'lift_battle_init_0060'),
        ('87e0189b1bc04e8e', 0x0028, 0x00f0, 'lift_battle_init_0028'),
        ('3a4f0cd1c30a3aef', 0x0800, 0x1410, 'lift_battle_3a4f_0800'),
        ('3a4f0cd1c30a3aef', 0x2400, 0x3080, 'lift_battle_3a4f_2400'),
        # Issue 025 (spec v3): Si Shui Gate fight programs that ran interpreted.
        ('d29a38db75ab7cca_0000', 0x0000, 0x02b0, 'lift_sishui_d29a_0000'),
        ('1ba40975a14161fc_0000', 0x0000, 0x0360, 'lift_sishui_1ba4_0000'),
        ('49f60042ec1ea159_0800', 0x0800, 0x0ab0, 'lift_sishui_49f6_0800'),
        ('49f60042ec1ea159_05f0', 0x05f0, 0x0ab0, 'lift_sishui_49f6_05f0'),
        ('563f13c8dedc09e4_05f0', 0x05f0, 0x0b10, 'lift_sishui_563f_05f0'),
        ('49f60042ec1ea159_0600', 0x0600, 0x2748, 'lift_sishui_49f6_0600'),
        ('6621289d99458305_0000', 0x0000, 0x0270, 'lift_sishui_6621_0000'),
        ('fae9165130cc72ff_0000', 0x0000, 0x02c8, 'lift_sishui_fae9_0000'),
        ('8d905f87f3bcba9d_0040', 0x0040, 0x1070, 'lift_sishui_8d90_0040'),
        ('5e225d7fd5bf037b_0000', 0x0000, 0x04f0, 'lift_sishui_5e22_0000'),
        ('336695fe491687e6_0800', 0x0800, 0x1410, 'lift_sishui_3366_0800'),
        ('3cb47dd9f41da6ae_0020', 0x0020, 0x0bd8, 'lift_sishui_3cb4_0020'),
        ('1ba40975a14161fc_0368', 0x0368, 0x0370, 'lift_sishui_1ba4_0368'),
        ('eb1d3af053afc5c2_04a0', 0x04a0, 0x04a8, 'lift_sishui_eb1d_04a0'),
        ('3a4f0cd1c30a3aef_05f0', 0x05f0, 0x1410, 'lift_sishui_3a4f_05f0'),
        ('dd3d1361f34c85a3_0000', 0x0000, 0x0008, 'lift_opening_dd3d_0000'),
        ('881c2abb30cea5ce_0070', 0x0070, 0x1738, 'lift_opening_881c_0070'),
        ('4dedc9b811a016b1_0000', 0x0000, 0x00b0, 'lift_opening_4ded_0000'),
        ('dd3d1361f34c85a3_0010', 0x0010, 0x0c30, 'lift_opening_dd3d_0010'),
        ('4dedc9b811a016b1_00b8', 0x00b8, 0x09b0, 'lift_opening_4ded_00b8'),
        ('baa8e61743cea7a2_0600', 0x0600, 0x27d8, 'lift_battle_baa8_0600'),
        ('64c44bbb44892dc9_0800', 0x0800, 0x0bc8, 'lift_battle_64c4_0800'),
    ]

    out = []
    out.append(LIFT_PREAMBLE)

    regs = []
    if disc_mode:
        regs.append("    // Preserved observed full-image registrations (metadata only):")
        for image_hash, pc, name in IMAGE_REGISTRATIONS:
            regs.append(f"    {{ 0x{image_hash:016x}ull, 0x{pc:04x}, {name}, 0u }},")
    else:
        # Full-image registrations (preparation / menus / unit tests):
        regs.append("    // Full-image registrations (preparation / menus / unit tests):")
        regs.append("    { 0x1ad6eefff0c2e7e5ull, 0x0800, lift_1ad6_0800, 0u },")
        regs.append("    { 0x1ad6eefff0c2e7e5ull, 0x1000, lift_1ad6_1000, 0u },")
        regs.append("    { 0x1b91674603e323f2ull, 0x0800, lift_1b91_0800, 0u },")
        regs.append("    { 0xda28291ffe0b3334ull, 0x0000, lift_da28_0000, 0u },")
        regs.append("    { 0xda28291ffe0b3334ull, 0x0010, lift_da28_0010, 0u },")
        regs.append("    { 0xda28291ffe0b3334ull, 0x0050, lift_da28_0050, 0u },")
        regs.append("    { 0x8d905f87f3bcba9dull, 0x0000, lift_da28_0000, 0u },")
        regs.append("    { 0x8d905f87f3bcba9dull, 0x0010, lift_da28_0010, 0u },")
        regs.append("    { 0x8d905f87f3bcba9dull, 0x0050, lift_da28_0050, 0u },")
        regs.append("    { 0x215297790907252aull, 0x0000, lift_da28_0000, 0u },")
        regs.append("    { 0x215297790907252aull, 0x0010, lift_da28_0010, 0u },")
        regs.append("    { 0x215297790907252aull, 0x0050, lift_da28_0050, 0u },")
        regs.append("    { 0x635ca7be952ce796ull, 0x0000, lift_da28_0000, 0u },")
        regs.append("    { 0x635ca7be952ce796ull, 0x0010, lift_da28_0010, 0u },")
        regs.append("    { 0x635ca7be952ce796ull, 0x0050, lift_da28_0050, 0u },")
        regs.append("    { 0xfb6ec66cb41b245full, 0x0000, lift_da28_0000, 0u },")
        regs.append("    { 0xfb6ec66cb41b245full, 0x0010, lift_da28_0010, 0u },")
        regs.append("    { 0xfb6ec66cb41b245full, 0x0050, lift_da28_0050, 0u },")
        regs.append("")
        regs.append("    // Image-specific registrations for dynamic battle images:")

    # Map subroutines to names for full-image matching
    sub_map = {}
    for h, pc, end_pc, name in targets:
        if h not in bins:
            print(f"Skipping {name}: no captured microcode for {h}")
            continue
        code = bins[h]
        sub_code = code[pc:end_pc+8]
        sub_size = len(sub_code)
        sub_hash = fnv1a_64(sub_code)
        sub_map[(sub_hash, pc, sub_size)] = name

    flush_0220_code = bins['baa8e61743cea7a2'][0x0220:0x0230]
    flush_0360_code = bins['baa8e61743cea7a2'][0x0360:0x0370]
    sub_map[(fnv1a_64(flush_0220_code), 0x0220, 16)] = 'lift_flush_0220'
    sub_map[(fnv1a_64(flush_0360_code), 0x0360, 16)] = 'lift_flush_0360'

    # Match all dynamic battle images from real census
    census_regs = {}
    import os
    if not disc_mode and os.path.exists('notes/evidence/battle_real_census.tsv'):
        with open('notes/evidence/battle_real_census.tsv') as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith('#'): continue
                parts = line.split()
                h_str = parts[0]
                pc = int(parts[1], 16)
                if h_str not in bins: continue
                code = bins[h_str]
                for (sub_h, sub_pc, sz), name in sub_map.items():
                    if sub_pc == pc:
                        chunk = code[pc:pc+sz]
                        if len(chunk) == sz and fnv1a_64(chunk) == sub_h:
                            ih = int(h_str, 16)
                            census_regs[(ih, pc)] = name
                            break

    existing_set = set(regs)
    for (ih, pc), name in sorted(census_regs.items()):
        reg_str = f"    {{ 0x{ih:016x}ull, 0x{pc:04x}, {name}, 0u }},"
        if reg_str not in existing_set:
            regs.append(reg_str)
            existing_set.add(reg_str)

    regs.append("")
    regs.append("    // Subroutine-level footprint registrations (matches across all dynamic images):")

    for h, pc, end_pc, name in targets:
        if h not in bins:
            print(f"Skipping {name}: no captured microcode for {h}")
            continue
        code = bins[h]
        # The registration hash must cover every reachable pair, including shared code
        # below the entry pc, or another microcode image could match with different code.
        sub_start = min(min(find_reachable(code, pc)), pc)
        sub_code = code[sub_start:end_pc+8]
        sub_size = len(sub_code)
        sub_hash = fnv1a_64(sub_code)
        print(f"Translating {name} (PC=0x{pc:04x}, len={sub_size}B)...")
        if disc_mode:
            missing = [p for p in find_reachable(code, pc) if not all(covered[h][p:p+8]) or len(covered[h][p:p+8]) != 8]
            if missing:
                raise ValueError(f'Unextracted reachable instructions in {name}: {missing}')
        reach = find_reachable(code, pc)
        out.append(translate_segment(code, pc, name, static_timing=static_enabled(name)))
        start_field = f", 0x{sub_start:04x}u" if sub_start != pc else ""
        regs.append(f"    {{ 0x{sub_hash:016x}ull, 0x{pc:04x}, {name}, {sub_size}u{start_field} }},")
        # Issue 025: when the footprint has gaps (other programs may sit there), also register the reachable code
        # alone, so every microcode image holding this program uses the translation.
        runs = []
        for rpc in reach:
            if runs and runs[-1][0] + runs[-1][1] == rpc:
                runs[-1][1] += 8
            else:
                runs.append([rpc, 8])
        if sum(size for _, size in runs) != sub_size:
            run_hash = fnv1a_64(b''.join(code[start:start + size] for start, size in runs))
            out.append(f"static const uint32_t kRuns_{name}[] = {{ " +
                       ", ".join(f"0x{start:04x}u, {size}u" for start, size in runs) + " };")
            regs.append(f"    {{ 0x{run_hash:016x}ull, 0x{pc:04x}, {name}, 0u, ~0u, false, kRuns_{name}, {len(runs)}u }},")

    out.append("""
void lift_flush_0220(vl::Run &c)
{
    vl::checkReset(c);
    vl::LiftAccess::begin(c, 0x0220, c.st.vi);
    vl::LiftAccess::end(c, 0x0220);
    vl::LiftAccess::begin(c, 0x0228, c.st.vi);
    vl::LiftAccess::end(c, 0x0228);
    c.st.pc = 0x0230;
}

void lift_flush_0360(vl::Run &c)
{
    vl::checkReset(c);
    vl::LiftAccess::begin(c, 0x0360, c.st.vi);
    vl::LiftAccess::end(c, 0x0360);
    vl::LiftAccess::begin(c, 0x0368, c.st.vi);
    vl::LiftAccess::end(c, 0x0368);
    c.st.pc = 0x0370;
}
""")
    regs.append("    { 0x5344f2e1f3bd36fbull, 0x0220, lift_flush_0220, 16u },")
    regs.append("    { 0x5344f2e1f3bd36fbull, 0x0360, lift_flush_0360, 16u },")

    out.append("static const vl::Registration s_battle_registrations[] = {\n" + "\n".join(regs) + "\n};\n" +
               "static vl::Registrar s_battle_registrar(s_battle_registrations, sizeof(s_battle_registrations) / sizeof(s_battle_registrations[0]));\n")

    # Output is translated game code: keep it in the ignored recomp/ tree, never in Git.
    out_path = os.environ.get('DW5XL_VU1_LIFTED_OUT', 'recomp/dw5xl/vu1/dw5xl_vu1_lifted.cpp')
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, 'w') as f:
        f.write("\n\n".join(out))
    print(f"Wrote {out_path}")

