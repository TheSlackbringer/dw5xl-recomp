import struct, sys

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
    tgt = (pc + 8 + imm * 8) & 0x3fff
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
            val = f"fmac<{fmac_map[base]}, {d}>(norm(vf[{fs}]), splat(cycle >= q_ready ? (q = pending_q) : q), acc, fl)"
            return blend_wrap(f"vf[{fd}]", val, d)
        if name == base + 'Aq':
            val = f"fmac<{fmac_map[base]}, {d}>(norm(vf[{fs}]), splat(cycle >= q_ready ? (q = pending_q) : q), acc, fl)"
            return blend_wrap("acc", val, d)
            
    for base in ('ADD', 'SUB', 'MUL', 'MADD', 'MSUB'):
        if name == base + 'i':
            val = f"fmac<{fmac_map[base]}, {d}>(norm(vf[{fs}]), splat(i_reg), acc, fl)"
            return blend_wrap(f"vf[{fd}]", val, d)
        if name == base + 'Ai':
            val = f"fmac<{fmac_map[base]}, {d}>(norm(vf[{fs}]), splat(i_reg), acc, fl)"
            return blend_wrap("acc", val, d)
            
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
        return f"clip = ((clip << 6) | (vl::clipFlags(vf[{fs}], bc<3>(vf[{ft}])) & 0x3F)) & 0xFFFFFF;"
        
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
            bw = blend_wrap(f"vf[{ft}]", f"vl::lq(mem, vi[{fs}])", dd)
            if bw: stmts.append(bw)
            stmts.append(f"if ({fs} != 0) vi[{fs}] = vl::i16(vi[{fs}] + 1);")
            return " ".join(stmts)
        if sp == 0x35: # SQI
            stmts = [f"vl::sq<0x{lm:x}, false>(c, mem, vi[{ft}], vf[{fs}], cycle);"]
            stmts.append(f"if ({ft} != 0) vi[{ft}] = vl::i16(vi[{ft}] + 1);")
            return " ".join(stmts)
        if sp == 0x36: # LQD
            stmts = [f"if ({fs} != 0) vi[{fs}] = vl::i16(vi[{fs}] - 1);"]
            bw = blend_wrap(f"vf[{ft}]", f"vl::lq(mem, vi[{fs}])", dd)
            if bw: stmts.append(bw)
            return " ".join(stmts)
        if sp == 0x37: # SQD
            stmts = [f"if ({ft} != 0) vi[{ft}] = vl::i16(vi[{ft}] - 1);"]
            stmts.append(f"vl::sq<0x{lm:x}, false>(c, mem, vi[{ft}], vf[{fs}], cycle);")
            return " ".join(stmts)
        if sp == 0x38: # DIV
            return f"if (cycle >= q_ready) q = pending_q; {{ uint32_t di = 0; pending_q = vl::div(vl::lane<{l['fsf']}>(vf[{fs}]), vl::lane<{l['ftf']}>(vf[{ft}]), di); q_ready = cycle + 6; }}"
        if sp == 0x3b: # WAITQ
            return f"if (cycle >= q_ready) q = pending_q;"
        if sp == 0x3c: # MTIR
            return f"if ({ft} != 0) vi[{ft}] = static_cast<int32_t>(static_cast<int16_t>(vl::bits<{l['fsf']}>(vf[{fs}]) & 0xFFFF));"
        if sp == 0x3d: # MFIR
            val = f"_mm_castsi128_ps(_mm_set1_epi32(static_cast<int32_t>(static_cast<int16_t>(vi[{fs}] & 0xFFFF))))"
            return blend_wrap(f"vf[{ft}]", val, dd)
        if sp == 0x3e: # ILWR
            comp = 0 if dd & 8 else (1 if dd & 4 else (2 if dd & 2 else 3))
            return f"if ({ft} != 0) vi[{ft}] = vl::ilw(mem, vi[{fs}], {comp});"
        if sp == 0x3f: # ISWR
            lm = dest_mask(dd)
            return f"vl::isw<0x{lm:x}, false>(c, mem, vi[{fs}], vi[{ft}], cycle);"
        if sp == 0x68: # XTOP
            return f"if ({ft} != 0) vi[{ft}] = static_cast<int32_t>(c.st.top & 0x3FFu);"
        if sp == 0x69: # XITOP
            return f"if ({ft} != 0) vi[{ft}] = static_cast<int32_t>(c.st.itop & 0x3FFu);"
        if sp == 0x6c: # XGKICK
            return f"vl::kickStart(c, vi[{fs}] & 0x3FF, cycle);"
        if sp == 0x7b: # WAITP
            return ""
            
    if t == 'NORM':
        op = l['op']
        dd = l['dd']
        lm = dest_mask(dd)
        ft, fs, imm, tgt = l['ft'], l['fs'], l['imm'], l['tgt']
        if op == 0x00: # LQ
            return blend_wrap(f"vf[{ft}]", f"vl::lq(mem, vi[{fs}] + ({imm}))", dd)
        if op == 0x01: # SQ
            return f"vl::sq<0x{lm:x}, false>(c, mem, vi[{ft}] + ({imm}), vf[{fs}], cycle);"
        if op == 0x04: # ILW
            comp = 0 if dd & 8 else (1 if dd & 4 else (2 if dd & 2 else 3))
            return f"if ({ft} != 0) vi[{ft}] = vl::ilw(mem, vi[{fs}] + ({imm}), {comp});"
        if op == 0x05: # ISW
            return f"vl::isw<0x{lm:x}, false>(c, mem, vi[{fs}] + ({imm}), vi[{ft}], cycle);"
        if op == 0x08: # IADDIU
            imm15 = (lo & 0x7FF) | ((lo >> 10) & 0x7800)
            return f"if ({ft} != 0) vi[{ft}] = vl::i16(vi[{fs}] + ({imm15}));"
        if op == 0x09: # ISUBIU
            imm15 = (lo & 0x7FF) | ((lo >> 10) & 0x7800)
            return f"if ({ft} != 0) vi[{ft}] = vl::i16(vi[{fs}] - ({imm15}));"
        if op == 0x11: # FCSET
            return f"clip = 0x{lo & 0xFFFFFF:06x}u;"
        if op == 0x12: # FCAND
            return f"vi[1] = ((clip & 0x{lo & 0xFFFFFF:06x}u) != 0) ? 1 : 0;"
        if op == 0x13: # FCOR
            imm24 = lo & 0xFFFFFF
            return f"vi[1] = ((clip | 0x{imm24:06x}u) == 0xFFFFFFu) ? 1 : 0;"
        if op == 0x1a: # FMAND
            return f"if ({ft} != 0) vi[{ft}] = fl.mac & (vi[{fs}] & 0xFFFF);"
            
    raise ValueError(f"Unknown lower op at pc {pc:04x}: lo={lo:08x}")

def find_reachable(code, entry_pc):
    visited = set()
    worklist = [entry_pc]
    
    while worklist:
        pc = worklist.pop()
        while pc not in visited:
            visited.add(pc)
            lo, up = struct.unpack_from('<II', code, pc)
            e_bit = (up >> 30) & 1
            l = decode_lower(lo, pc)
            
            if l['type'] == 'NORM' and l['op'] in (0x20, 0x28, 0x29, 0x2c, 0x2d, 0x2e, 0x2f):
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

ACC_OPS = {'ADDA', 'SUBA', 'MULA', 'MADDA', 'MSUBA', 'OPMULA', 'OPMSUB', 'CLIP', 'NOP'}
def is_acc_op(name):
    for a in ACC_OPS:
        if name.startswith(a):
            return True
    return False

def upper_writes_vf(u):
    if u['type'] not in ('UP', 'UPS'):
        return None
    name = u['name']
    if is_acc_op(name):
        return None
    if u['d'] == 0 or u['fd'] == 0:
        return None
    return u['fd']

def lower_reads_vf(l):
    if l['type'] == 'SPEC':
        sp = l['sp']
        if sp in (0x30, 0x31, 0x35, 0x3c): # MOVE, MR32, SQI, MTIR
            return [l['fs']]
        if sp == 0x38: # DIV
            return [l['fs'], l['ft']]
    elif l['type'] == 'NORM':
        if l['op'] == 0x01: # SQ
            return [l['fs']]
    return []

def emit_pair(u, l, lo, i_bit, pc, out):
    shadow = None
    w = upper_writes_vf(u)
    if w is not None and w in lower_reads_vf(l):
        shadow = w
        out.append(f"    shadow_vf = vf[{shadow}];")
    up_code = translate_upper(u)
    lo_code = translate_lower(l, lo, i_bit, pc)
    if shadow is not None:
        lo_code = lo_code.replace(f"vf[{shadow}]", "shadow_vf")
    if up_code: out.append(f"    {up_code}")
    if lo_code: out.append(f"    {lo_code}")

def translate_segment(code, entry_pc, fn_name):
    reachable = find_reachable(code, entry_pc)
    
    branch_info = {}
    branch_delay_slots = set()
    targets = set()
    
    for pc in reachable:
        lo, up = struct.unpack_from('<II', code, pc)
        l = decode_lower(lo, pc)
        if l['type'] == 'NORM' and l['op'] in (0x20, 0x28, 0x29, 0x2c, 0x2d, 0x2e, 0x2f):
            op = l['op']
            tgt = l['tgt']
            ft, fs = l['ft'], l['fs']
            if op == 0x20: cond = "true"
            elif op == 0x28: cond = f"(vi[{ft}] == vi[{fs}])"
            elif op == 0x29: cond = f"(vi[{ft}] != vi[{fs}])"
            elif op == 0x2c: cond = f"(vi[{fs}] < 0)"
            elif op == 0x2d: cond = f"(vi[{fs}] > 0)"
            elif op == 0x2e: cond = f"(vi[{fs}] <= 0)"
            elif op == 0x2f: cond = f"(vi[{fs}] >= 0)"
            branch_info[pc] = (tgt, cond)
            branch_delay_slots.add(pc + 8)
            targets.add(tgt)
            
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
    out.append(f"void {fn_name}(vl::Run &c)")
    out.append("{")
    out.append("    using namespace vl;")
    out.append("    vl::checkReset(c);")
    out.append("    alignas(16) V vf[32];")
    out.append("    for (int i = 0; i < 32; ++i) vf[i] = _mm_loadu_ps(c.st.vf[i]);")
    out.append("    int32_t vi[16];")
    out.append("    for (int i = 0; i < 16; ++i) vi[i] = c.st.vi[i];")
    out.append("    V acc = _mm_loadu_ps(c.st.acc);")
    out.append("    V shadow_vf = _mm_setzero_ps();")
    out.append("    float q = c.st.q;")
    out.append("    float pending_q = q;")
    out.append("    uint64_t q_ready = 0;")
    out.append("    float i_reg = c.st.i;")
    out.append("    uint32_t clip = c.st.clip;")
    out.append("    Flags fl{ c.st.status & 0xFu, c.st.mac, (c.st.status >> 6) & 0x3Fu };")
    out.append("    uint8_t *mem = c.mem;")
    out.append("    uint64_t cycle = c.cycle;")
    for bpc in sorted(branch_info.keys()):
        out.append(f"    bool taken_{bpc:04x} = false;")
    out.append("    auto checkpoint = [&] { if (!vl::resetRequested()) return;")
    out.append("        for (int n = 1; n < 32; ++n) _mm_storeu_ps(c.st.vf[n], vf[n]);")
    out.append("        for (int n = 1; n < 16; ++n) c.st.vi[n] = vi[n];")
    out.append("        _mm_storeu_ps(c.st.acc, acc); c.st.q = cycle >= q_ready ? pending_q : q;")
    out.append("        c.st.i = i_reg; c.st.clip = clip; c.st.mac = fl.mac;")
    out.append("        c.st.status = (c.st.status & 0xFF0u) | (fl.cur & 0xFu) | ((fl.sticky & 0x3Fu) << 6);")
    out.append("        c.cycle = cycle; throw vl::ResetCanceled{}; };")
    out.append("    bool resetCanceled = false;")
    out.append("    try {")
    out.append("")

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
                
            if pc in branch_info:
                tgt, cond = branch_info[pc]
                out.append(f"    // {pc:04x}: branch to {tgt:04x}")
                up_code = translate_upper(u)
                if up_code:
                    out.append(f"    {up_code}")
                out.append(f"    taken_{pc:04x} = {cond};")
                
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
                emit_pair(d_u, d_l, d_lo, d_i_bit, d_pc, out)
                out.append("    cycle += 2;")
                out.append(f"    if (taken_{pc:04x}) goto loc_{tgt:04x};")
                i += 1
                continue
                
            out.append(f"    // {pc:04x}")
            emit_pair(u, l, lo, i_bit, pc, out)
            out.append("    ++cycle;")
            
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
                emit_pair(d_u, d_l, d_lo, d_i_bit, d_pc, out)
                out.append("    ++cycle;")
                final_pc = d_pc + 8
                out.append(f"    c.st.pc = 0x{final_pc:04x};")
                out.append("    goto done;")
                i += 1
                break
                
            i += 1

    out.append("done:;")
    out.append("    } catch (const vl::ResetCanceled &) { resetCanceled = true; }")
    out.append("    for (int i = 1; i < 32; ++i) _mm_storeu_ps(c.st.vf[i], vf[i]);")
    out.append("    c.st.vf[0][0] = 0.0f; c.st.vf[0][1] = 0.0f; c.st.vf[0][2] = 0.0f; c.st.vf[0][3] = 1.0f;")
    out.append("    for (int i = 1; i < 16; ++i) c.st.vi[i] = vi[i];")
    out.append("    c.st.vi[0] = 0;")
    out.append("    _mm_storeu_ps(c.st.acc, acc);")
    out.append("    if (cycle >= q_ready) q = pending_q;")
    out.append("    c.st.q = q;")
    out.append("    c.st.i = i_reg;")
    out.append("    c.st.clip = clip;")
    out.append("    c.st.mac = fl.mac;")
    out.append("    c.st.status = (c.st.status & 0xFF0u) | (fl.cur & 0xFu) | ((fl.sticky & 0x3Fu) << 6);")
    out.append("    c.endCycle = cycle;")
    out.append("    c.cycle = cycle;")
    out.append("    if (resetCanceled) throw vl::ResetCanceled{};")
    out.append("}")
    return "\n".join(out)

if __name__ == '__main__':
    def load(path):
        with open(path, 'rb') as f:
            return f.read()

    def fnv1a_64(data):
        h = 1469598103934665603
        for b in data:
            h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
        return h

    bins = {}
    import glob
    for p in glob.glob('notes/evidence/battle_real_census.tsv.*.bin'):
        h = p.split('.')[-2]
        bins[h] = load(p)

    targets = [
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
    ]

    out = []
    out.append("""#include "runtime/ps2_vu1_lift.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

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
""")

    regs = []
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
    regs.append("    { 0x3249a2515e770fccull, 0x0800, lift_battle_actor_skinning_0800, 0u },")
    regs.append("    { 0xc93b67a8d567d56full, 0x0800, lift_battle_actor_skinning_0800, 0u },")
    regs.append("    { 0xadc8836e4f4da918ull, 0x0800, lift_battle_actor_skinning_0800, 0u },")
    regs.append("    { 0xbaa8e61743cea7a2ull, 0x0800, lift_battle_mesh_0800, 0u },")
    regs.append("    { 0xbaa8e61743cea7a2ull, 0x2400, lift_battle_fx_2400, 0u },")
    regs.append("    { 0xbaa8e61743cea7a2ull, 0x0040, lift_battle_init_0040, 0u },")
    regs.append("    { 0xbaa8e61743cea7a2ull, 0x00f0, lift_battle_stream_00f0, 0u },")
    regs.append("    { 0x84a67c1d4789e87bull, 0x2400, lift_battle_fx_2400, 0u },")
    regs.append("    { 0x84a67c1d4789e87bull, 0x0800, lift_battle_mesh_0800, 0u },")
    regs.append("    { 0x84a67c1d4789e87bull, 0x0040, lift_battle_init_0040, 0u },")
    regs.append("    { 0x84a67c1d4789e87bull, 0x0230, lift_battle_clip_0230, 0u },")
    regs.append("    { 0xb525f25c16e3205full, 0x0800, lift_battle_mesh_0800, 0u },")
    regs.append("    { 0xb525f25c16e3205full, 0x2400, lift_battle_fx_2400, 0u },")
    regs.append("    { 0xb525f25c16e3205full, 0x0040, lift_battle_init_0040, 0u },")
    regs.append("    { 0x64127817c106b85aull, 0x2400, lift_battle_particles_2400, 0u },")
    regs.append("    { 0x64127817c106b85aull, 0x0040, lift_battle_init_0040, 0u },")
    regs.append("    { 0x46d6ccb25f6b8d32ull, 0x2400, lift_battle_terrain_2400, 0u },")
    regs.append("    { 0x46d6ccb25f6b8d32ull, 0x0800, lift_battle_water_0800, 0u },")
    regs.append("    { 0x49f60042ec1ea159ull, 0x2400, lift_battle_particles_2400, 0u },")
    regs.append("    { 0x49f60042ec1ea159ull, 0x0040, lift_battle_init_0040, 0u },")
    regs.append("    { 0xe7d7cb4fef6b9efaull, 0x2400, lift_battle_terrain_2400, 0u },")
    regs.append("    { 0xe7d7cb4fef6b9efaull, 0x0800, lift_battle_water_0800, 0u },")
    regs.append("    { 0x563f13c8dedc09e4ull, 0x2400, lift_battle_fx_2400, 0u },")
    regs.append("    { 0x563f13c8dedc09e4ull, 0x0800, lift_battle_mesh_0800, 0u },")
    regs.append("    { 0x563f13c8dedc09e4ull, 0x0040, lift_battle_init_0040, 0u },")
    regs.append("    { 0x60b13c6c3365e06aull, 0x2400, lift_battle_shadows_2400, 0u },")
    regs.append("    { 0x0085daedcc3e27a7ull, 0x2400, lift_battle_shadows_2400, 0u },")
    regs.append("    { 0x6e15f776d6fcfae3ull, 0x2400, lift_battle_fx_2400, 0u },")
    regs.append("    { 0x8475f45d07f11326ull, 0x2400, lift_battle_particles_2400, 0u },")
    regs.append("    { 0xd2b8165963d8189full, 0x2400, lift_battle_shadows_2400, 0u },")
    regs.append("")
    regs.append("    // Subroutine-level footprint registrations (matches across all dynamic images):")

    for h, pc, end_pc, name in targets:
        code = bins[h]
        sub_code = code[pc:end_pc+8]
        sub_size = len(sub_code)
        sub_hash = fnv1a_64(sub_code)
        print(f"Translating {name} (PC=0x{pc:04x}, len={sub_size}B)...")
        out.append(translate_segment(code, pc, name))
        regs.append(f"    {{ 0x{sub_hash:016x}ull, 0x{pc:04x}, {name}, {sub_size}u }},")

    out.append("""
void lift_flush_0220(vl::Run &c)
{
    vl::checkReset(c);
    c.cycle += 2;
    c.endCycle = c.cycle;
    c.st.pc = 0x0230;
}

void lift_flush_0360(vl::Run &c)
{
    vl::checkReset(c);
    c.cycle += 2;
    c.endCycle = c.cycle;
    c.st.pc = 0x0370;
}
""")
    regs.append("    { 0x5344f2e1f3bd36fbull, 0x0220, lift_flush_0220, 16u },")
    regs.append("    { 0x5344f2e1f3bd36fbull, 0x0360, lift_flush_0360, 16u },")

    out.append("static const vl::Registration s_battle_registrations[] = {\n" + "\n".join(regs) + "\n};\n" +
               "static vl::Registrar s_battle_registrar(s_battle_registrations, sizeof(s_battle_registrations) / sizeof(s_battle_registrations[0]));\n")

    with open('app/src/dw5xl_vu1_lifted.cpp', 'w') as f:
        f.write("\n\n".join(out))
    print("Wrote app/src/dw5xl_vu1_lifted.cpp successfully!")

