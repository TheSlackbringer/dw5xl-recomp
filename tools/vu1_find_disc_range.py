#!/usr/bin/env python3
"""Locate a VU1 program's reachable code in the player's own disc files (for tools/disc_vu_inputs.py).

  tools/vu1_find_disc_range.py <census image .bin> <entry pc> <name> [disc file ...]

The image comes from a local PS2X_VU1_CENSUS run (never commit it). Prints a RANGES entry (offsets, sizes and a
SHA256 only, no instruction bytes) and the IMAGE_REGISTRATIONS line for tools/disc_vu_inputs.py, and the target
line for tools/vu1_translator.py. Each contiguous reachable run is matched greedily (longest prefix found) in the
given disc files (default: game/dw5xl/SLES_538.60).
"""
import hashlib, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vu1_translator as vt

def fnv1a_64(data):
    h = 1469598103934665603
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h

def main():
    image_path, entry, name = sys.argv[1], int(sys.argv[2], 0), sys.argv[3]
    files = sys.argv[4:] or ['game/dw5xl/SLES_538.60']
    image = open(image_path, 'rb').read()
    sources = [(None if os.path.basename(f) == 'SLES_538.60' else os.path.basename(f), open(f, 'rb').read()) for f in files]
    reachable = vt.find_reachable(image, entry)
    runs, cur = [], [reachable[0]]
    for pc in reachable[1:]:
        if pc == cur[-1] + 8:
            cur.append(pc)
        else:
            runs.append((cur[0], cur[-1] + 8)); cur = [pc]
    runs.append((cur[0], cur[-1] + 8))
    chunks = []
    for a, b in runs:
        pc = a
        while pc < b:
            best = None
            for src, blob in sources:
                lo, hi = 8, b - pc
                while lo <= hi:  # longest prefix (multiple of 8) present in blob
                    mid = (lo + hi) // 16 * 8 or 8
                    if blob.find(image[pc:pc + mid]) >= 0:
                        if not best or mid > best[2]:
                            best = (src, blob.find(image[pc:pc + mid]), mid)
                        lo = mid + 8
                    else:
                        hi = mid - 8
            if not best:
                sys.exit(f'{name}: bytes at VU pc 0x{pc:04x} not found in {files}')
            src, off, size = best
            chunks.append((pc, off, size) + ((src,) if src else ()))
            pc += size
    rebuilt = bytearray(16384)
    for c in chunks:
        blob = dict(sources)[c[3] if len(c) > 3 else None]
        rebuilt[c[0]:c[0] + c[2]] = blob[c[1]:c[1] + c[2]]
    end = reachable[-1]
    image_hash = fnv1a_64(image)
    key = f'{image_hash:016x}'
    print('RANGES entry:')
    print(f" ('{key}',\n  {entry},\n  {end},\n  '{name}',\n  {chunks},\n  '{hashlib.sha256(bytes(rebuilt[entry:end + 8])).hexdigest()}'),")
    print('IMAGE_REGISTRATIONS:', f'({image_hash}, {entry}, {name!r})')
    print('translator target:', f"('{key}', 0x{entry:04x}, 0x{end:04x}, '{name}'),")

if __name__ == '__main__':
    main()
