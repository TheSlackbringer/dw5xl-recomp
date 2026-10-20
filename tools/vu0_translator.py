#!/usr/bin/env python3
"""Translate the game's VU0 microprograms (started by VCALLMS on the EE) to native C++ (issue 025).

Same translator and cycle-exact static schedule as tools/vu1_translator.py, with the VU0 target: 4 KB micro
memory and data, no XGKICK/EFU/XTOP/XITOP/WAITP. Inputs are rebuilt from the player's own executable
(tools/disc_vu_inputs.py VU0_RANGES: offsets, sizes and SHA256 only). Each program is registered under the hash
of the whole 4 KB VU0 micro memory the game runs it from (the census hash), like VU1's image registrations.

  DW5XL_VU1_ELF=<SLES_538.60> (default game/dw5xl/SLES_538.60)
  DW5XL_VU0_LIFTED_OUT=<file>  (default recomp/dw5xl/vu0/dw5xl_vu0_lifted.cpp; game-derived, never in Git)
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vu1_translator as vt
from disc_vu_inputs import load_vu0_bins, VU0_RANGES


def main():
    vt.set_unit('vu0')
    bins, covered = load_vu0_bins(os.environ.get('DW5XL_VU1_ELF', 'game/dw5xl/SLES_538.60'))
    out = [vt.LIFT_PREAMBLE]
    regs = []
    for image_hash, entry, end, name, _chunks, _digest in VU0_RANGES:
        code = bins[image_hash]
        reachable = vt.find_reachable(code, entry)
        missing = [p for p in reachable if len(covered[image_hash][p:p + 8]) != 8 or not all(covered[image_hash][p:p + 8])]
        if missing:
            raise ValueError(f'Unextracted reachable VU0 instructions in {name}: {missing}')
        if reachable[-1] != end:
            raise ValueError(f'{name}: last reachable pair 0x{reachable[-1]:04x}, VU0_RANGES says 0x{end:04x}')
        print(f'Translating {name} (VU0 pc=0x{entry:04x}, {len(reachable)} pairs)...')
        out.append(vt.translate_segment(code, entry, name, static_timing=vt.static_enabled(name)))
        regs.append(f'    {{ 0x{image_hash}ull, 0x{entry:04x}, {name}, 0u, ~0u, true }},')
    out.append('static const vl::Registration s_vu0_registrations[] = {\n' + '\n'.join(regs) + '\n};\n' +
               'static vl::Registrar s_vu0_registrar(s_vu0_registrations, '
               'sizeof(s_vu0_registrations) / sizeof(s_vu0_registrations[0]));\n')
    # Output is translated game code: keep it in the ignored recomp/ tree, never in Git.
    out_path = os.environ.get('DW5XL_VU0_LIFTED_OUT', 'recomp/dw5xl/vu0/dw5xl_vu0_lifted.cpp')
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, 'w', newline='\n') as f:  # LF on every OS, as the C++ port writes it
        f.write('\n\n'.join(out))
    print(f'Wrote {out_path}')


if __name__ == '__main__':
    main()
