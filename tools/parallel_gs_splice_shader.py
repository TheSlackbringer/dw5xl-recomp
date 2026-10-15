#!/usr/bin/env python3
"""Replace one shader's SPIR-V in paraLLEl-GS's generated shaders/slangmosh.hpp.

slangmosh (Granite's shader packer) is not part of the build. When a patched
shader keeps its interface (bindings, push constants, specialization
constants) the stored reflection stays valid, so only its bytecode changes:
the new SPIR-V is appended to spirv_bank and the request_shader lines are
pointed at it, leaving the rest of the file untouched.

Compile each variant like slangmosh does, e.g.:
  glslc -fshader-stage=frag --target-env=vulkan1.1 -O -DPROMOTED=0 -I. -o v0.spv sample_circuit.frag
Compute shaders (request_program) work the same way:
  glslc -fshader-stage=comp --target-env=vulkan1.1 -O -I. -o ts.spv triangle_setup.comp

Usage: parallel_gs_splice_shader.py slangmosh.hpp NAME variant0.spv [variant1.spv ...]
"""
import re
import struct
import sys


def main():
    hpp, name, spvs = sys.argv[1], sys.argv[2], sys.argv[3:]
    src = open(hpp).read()
    bank = re.search(r'static const uint32_t spirv_bank\[\] =\s*\{(.*?)\};', src, re.S)
    base = len(re.findall(r'0x[0-9a-fA-F]+u', bank.group(1)))
    words, entries = [], []
    for path in spvs:
        data = open(path, 'rb').read()
        entries.append((base + len(words), len(data)))
        words += struct.unpack('<%dI' % (len(data) // 4), data)
    body = bank.group(1).rstrip()
    if not body.endswith(','):
        body += ','
    body += '\n\t// %s (patched)\n' % name
    body += ',\n'.join('\t' + ', '.join('0x%08xu' % w for w in words[i:i + 13])
                       for i in range(0, len(words), 13)) + ',\n'
    src = src[:bank.start(1)] + body + src[bank.end(1):]
    for variant, (offset, size) in enumerate(entries):
        target = r'this->%s\[%d\]' % (name, variant) if len(entries) > 1 else r'this->%s' % name
        src, count = re.subn(target + r' = device.request_(?:shader|program)\(spirv_bank \+ \d+, \d+,',
                             lambda m: m.group(0).split('(')[0] + '(spirv_bank + %d, %d,' % (offset, size), src)
        if count != 1:
            sys.exit('expected one request_shader/request_program line for %s variant %d, found %d' % (name, variant, count))
    open(hpp, 'w').write(src)


if __name__ == '__main__':
    main()
