#!/usr/bin/env python3
"""Clean Ghidra's ExportPS2Functions CSV for ps2_recomp.

- drop rows at/after the end of code (data labels Ghidra marks executable)
- clamp functions whose End is far past Start+Size (non-contiguous bodies: Ghidra reports the
  max body address, and ps2_recomp would then emit one giant function that inlines everything
  in between, bypassing stubs, e.g. FUN_0010a380 0x10A380..0x11FD38 swallowing libmpeg)
usage: fix_ghidra_csv.py in.csv out.csv CODE_END_HEX
"""
import sys
src, dst, code_end = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
out, clamped = [], 0
for i, line in enumerate(open(src)):
    if i == 0:
        out.append(line); continue
    name, start, end, size = line.rstrip('\n').split(',')
    s, e, n = int(start, 16), int(end, 16), int(size)
    if s >= code_end:
        continue
    if e - s > n + 0x400:
        e = s + n; clamped += 1
        print(f"clamped {name} 0x{s:08X}..0x{int(end,16):08X} -> 0x{e:08X}", file=sys.stderr)
    out.append(f"{name},0x{s:08X},0x{e:08X},{n}\n")
open(dst, 'w').writelines(out)
print(f"{len(out)-1} rows, {clamped} clamped", file=sys.stderr)
