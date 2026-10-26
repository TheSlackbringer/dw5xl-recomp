#!/usr/bin/env python3
"""038: execute independently authored synthetic inputs through the real native generator.

No game/private data. Uses an existing recompiler and host C++ compiler. Simple fixtures never
call runtime methods; their unused runtime argument is null, not a replacement implementation.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
RECOMP = Path(os.environ.get('PS2X_TEST_RECOMPILER', str(ROOT / (
    'external/PS2Recomp/build/ps2xRecomp/ps2_recomp' + ('.exe' if os.name == 'nt' else '')))))
CXX = shlex.split(os.environ.get('CXX', 'clang++'))
BASE = 0x1000
RETURN = 0x03e00008


class FunctionBoundaries(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='native-boundary-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.compiled = {}
        self.assertTrue(RECOMP.is_file(), 'Build the recompiler or set PS2X_TEST_RECOMPILER')
        self.assertTrue(CXX and shutil.which(CXX[0]), 'A host C++ compiler is required; no automatic install')

    def generate(self, words, rows, *, prefix=0, entry_points=(), image=None, base=BASE, input_format='raw', code_copies=()):
        binary = self.root / 'input.bin'
        binary.write_bytes(image if image is not None else b'\0' * prefix + struct.pack('<' + 'I' * len(words), *words))
        csv = self.root / 'functions.csv'
        csv.write_text('Name,Start,End,Size\n' + ''.join(
            f'{name},0x{start:x},0x{end:x},{end-start}\n' for name, start, end in rows))
        out = self.root / 'generated'
        profile = self.root / 'profile.toml'
        profile.write_text('[general]\ninput_format='+json.dumps(input_format)+'\n' +
            (f'module_id="boundary_test"\nraw_base={base-prefix}\nraw_code_offset={prefix}\n' if input_format=='raw' else '') +
            'entry_points=' + json.dumps(list(entry_points)) + '\n' + ''.join(
                f'{k}={json.dumps(str(v))}\n' for k, v in [('input', binary), ('ghidra_output', csv), ('output', out)]) + ''.join(
                '\n[[code_copies]]\n'+''.join(f'{key}={json.dumps(value)}\n' for key,value in copy.items()) for copy in code_copies))
        result = subprocess.run([str(RECOMP), str(profile)], capture_output=True, text=True, timeout=60)
        (self.root / 'generation.log').write_text(result.stdout + result.stderr)
        return result, out

    @staticmethod
    def elf_sections(sections):
        # Authored ELF32 MIPS with independent section and load flags. Section
        # tuples: name, address, actual bytes, flags, type, declared byte count.
        names=b'\0'+b''.join(name.encode()+b'\0' for name,*_ in sections)+b'.shstrtab\0'
        count=len(sections)+2;phnum=len(sections)
        shoff=(52+32*phnum+15)&~15;names_at=shoff+count*40
        payload=(names_at+len(names)+15)&~15
        blob=bytearray(payload);blob[names_at:names_at+len(names)]=names
        cursor=payload;name_at=1;base=min(x[1] for x in sections)
        for index,(name,address,data,flags,kind,size) in enumerate(sections,1):
            struct.pack_into('<10I',blob,shoff+index*40,name_at,kind,flags,address,cursor,size,0,0,4,0)
            # A segment per section preserves real guest gaps and matches every
            # section's file offset; executable load flags alone still cannot
            # turn data/VU/NOBITS sections into file-backed EE code.
            struct.pack_into('<8I',blob,52+(index-1)*32,1,cursor,address,address,0 if kind==8 else size,size,7,4)
            if kind!=8:blob.extend(data);cursor+=len(data)
            name_at+=len(name)+1
        struct.pack_into('<10I',blob,shoff+(count-1)*40,name_at,3,0,0,names_at,len(names),0,0,1,0)
        struct.pack_into('<16sHHIIIIIHHHHHH',blob,0,b'\x7fELF\x01\x01\x01'+b'\0'*9,
            2,8,1,base,52,shoff,0,52,32,phnum,40,count,count-1)
        return bytes(blob)

    def must_preserve_output_on_failure(self, words, rows, diagnostic, **kwargs):
        out=self.root/'generated';out.mkdir(exist_ok=True)
        sentinel=out/'keep.cpp';sentinel.write_text('// previous working output\n')
        result,_=self.generate(words,rows,**kwargs)
        self.assertNotEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertRegex(result.stdout+result.stderr,diagnostic)
        self.assertEqual(sentinel.read_text(),'// previous working output\n')
        self.assertEqual(sorted(p.name for p in out.iterdir()),['keep.cpp'])

    def test_elf_slot_requires_file_backed_ee_code(self):
        branch=struct.pack('<I',0x10200007);slot=struct.pack('<I',0x24840004)
        cases=[('.data',BASE+4,slot,3,1,4),('.vutext',BASE+4,slot,6,1,4),
            ('.vutext.program',BASE+4,slot,6,1,4),('.DVP.overlay',BASE+4,slot,6,1,4),
            ('.DVP.overlay.program',BASE+4,slot,6,1,4),('.bss',BASE+4,b'',6,8,4),
            ('.text.gap',BASE+8,slot,6,1,4)]
        for section in cases:
            for end in (BASE+4,BASE+8):
                with self.subTest(section=section[0],end=end):
                    image=self.elf_sections([('.text',BASE,branch,6,1,4),section])
                    self.must_preserve_output_on_failure([], [('owner',BASE,end)],r'(?i)(delay.slot|slot)',
                        image=image,input_format='elf')

    def test_elf_adjacent_executable_section_supplies_real_slot(self):
        pack=lambda *w:struct.pack('<'+'I'*len(w),*w)
        image=self.elf_sections([('.text.branch',BASE,pack(0x10200007),6,1,4),
            ('.text.slot',BASE+4,pack(0x24840004,RETURN,0),6,1,12),
            ('.text.target',BASE+32,pack(RETURN,0),6,1,8)])
        result,out=self.generate([], [('owner',BASE,BASE+4),('slot',BASE+4,BASE+16),
            ('target',BASE+32,BASE+40)],image=image,input_format='elf')
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertEqual(self.native_effect(out,0),(0,104,BASE+32,0))
        self.assertEqual(self.native_effect(out,2),(2,104,BASE+8,0))

    def test_truncated_elf_section_cannot_invent_required_slot(self):
        image=self.elf_sections([('.text',BASE,struct.pack('<I',0x10200007),6,1,8)])
        self.must_preserve_output_on_failure([], [('owner',BASE,BASE+8)],r'(?i)(elf|slot|decode|failed)',
            image=image,input_format='elf')

    def test_delayed_transfer_inside_required_slot_is_rejected(self):
        self.must_preserve_output_on_failure([0x10200007,RETURN,0], [('owner',BASE,BASE+4)],r'(?i)delay.slot')

    def test_raw_bounds_and_wrong_module_preserve_working_output(self):
        for length in (1,2,3):
            with self.subTest(partial_bytes=length):
                image=struct.pack('<I',0x10200007)+b'\0'*length
                self.must_preserve_output_on_failure([], [('owner',BASE,BASE+4)],r'Invalid raw module size/base',image=image)
        with self.subTest(wraparound=True):
            self.must_preserve_output_on_failure([0x10200007,0], [('owner',0xfffffffc,0x100000000)],
                r'Invalid raw module size/base',base=0xfffffffc)
        with self.subTest(unaligned_prefix=True):
            self.must_preserve_output_on_failure([0x10200007,0], [('owner',BASE,BASE+4)],
                r'raw_code_offset must be an aligned guest-sized offset',prefix=2)
        with self.subTest(wrong_module=True):
            other=self.root/'other_module.bin';other.write_bytes(struct.pack('<II',0x24840004,RETURN))
            before=other.read_bytes()
            self.must_preserve_output_on_failure([0x10200007], [('owner',BASE,BASE+4)],r'(?i)delay.slot')
            self.assertEqual(other.read_bytes(),before)

    def native_effect(self, out, initial, *, entry=BASE, fcr31=0, continuation=None):
        table = (out / 'register_functions.cpp').read_text()
        def symbol_at(address):
            entry_arrays = re.findall(r'\bRecompiledModuleEntry\s+\w+\s*\[\s*\]\s*=\s*\{(.*?)\n\};', table, re.S)
            found = [name for body in entry_arrays for name in re.findall(
                r'\{\s*0x' + f'{address:x}' + r'[uU]?\s*,\s*([A-Za-z_]\w*)\s*\}', body, re.I)]
            base=re.search(r'g_ps2RecompiledFunctionTableBase\s*=\s*(0x[\da-fA-F]+)',table)
            if base:
                for index,name,comment in re.findall(r'g_ps2RecompiledFunctionTable\[(\d+)\]\s*=\s*(\w+)\s*;\s*//\s*(0x[\da-fA-F]+)',table):
                    actual=int(base[1],16)+int(index)*4
                    self.assertEqual(actual,int(comment,16),'Indexed table address must agree with its slot')
                    if actual==address:found.append(name)
            self.assertLessEqual(len(found), 1)
            return found[0] if found else None
        symbol = symbol_at(entry)
        self.assertIsNotNone(symbol, 'Must execute the actual registered wrapper')
        symbols = [symbol]
        extra_call = ''
        if continuation is not None:
            extra = symbol_at(continuation)
            if extra:
                symbols.append(extra)
                extra_call = f'if(ctx.pc=={continuation}u) {extra}(nullptr,&ctx,nullptr);'
        sources = []
        for name in symbols:
            found = [p for p in out.glob('*.cpp') if re.search(
                r'\bvoid\s+' + re.escape(name) + r'\s*\(', p.read_text())]
            self.assertEqual(len(found), 1, 'Compile emitted code, never a replacement function')
            if found[0] not in sources:
                sources.append(found[0])
        declarations = '\n'.join(f'extern void {s}(uint8_t*,R5900Context*,PS2Runtime*);' for s in set(symbols))
        harness = f'''#include "ps2_runtime.h"
#include <cstdio>
#include <cstdlib>
{declarations}
int main(int argc,char**argv) {{
    if(argc!=3)return 3;
    R5900Context ctx;
    ctx.pc={entry}u;
    ctx.fcr31=std::strtoul(argv[2],nullptr,0);
    ctx.r[1]=_mm_set_epi64x(0,std::strtoll(argv[1],nullptr,10));
    ctx.r[4]=_mm_set_epi64x(0,100);
    ctx.r[31]=_mm_set_epi64x(0,0x2220);
    {symbol}(nullptr,&ctx,nullptr);
    {extra_call}
    std::printf("%lld %lld %u %u\\n",(long long)_mm_cvtsi128_si64(ctx.r[1]),
        (long long)_mm_cvtsi128_si64(ctx.r[4]),ctx.pc,(unsigned)ctx.in_delay_slot);
}}
'''
        key = hashlib.sha256((harness + ''.join(p.read_text() for p in sources)).encode()).hexdigest()
        if key not in self.compiled:
            cpp = self.root / ('native-' + key + '.cpp')
            cpp.write_text(harness)
            exe = self.root / ('native-' + key + ('.exe' if os.name == 'nt' else ''))
            inc = ROOT / 'external/PS2Recomp'
            command = CXX + ['-std=c++20', '-O2', '-msse4.1'] + [
                '-I' + str(p) for p in [inc/'ps2xRuntime/include', inc/'ps2xRuntime/src/lib/Kernel',
                                       inc/'ps2xIOP/include', out]] + [str(cpp)] + [str(p) for p in sources] + ['-o', str(exe)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, 'Compile before checking effects:\n' + result.stdout + result.stderr)
            self.compiled[key] = exe
        run = subprocess.run([str(self.compiled[key]), str(initial), str(fcr31)],
                             capture_output=True, text=True, timeout=10)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        return tuple(int(v) for v in run.stdout.split())

    def split(self, word, slot=0x24840004, **kwargs):
        result, out = self.generate([word, slot, RETURN, 0, 0, 0, 0, 0, RETURN, 0],
            [('branch', BASE, BASE+4), ('slot_entry', BASE+4, BASE+16), ('target', BASE+32, BASE+40)], **kwargs)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return out

    def test_split_beq_preserves_decrement_and_original_condition(self):
        out = self.split(0x10200007, 0x2421ffff)
        for initial, expected in [(0, (-1,100,BASE+32,0)), (3, (2,100,BASE+8,0))]:
            with self.subTest(initial=initial):
                self.assertEqual(self.native_effect(out, initial), expected)

    def test_split_bne_preserves_pointer_increment_on_both_paths(self):
        out = self.split(0x14200007, prefix=16)
        for initial, pc in [(0, BASE+8), (2, BASE+32)]:
            with self.subTest(initial=initial):
                self.assertEqual(self.native_effect(out, initial), (initial,104,pc,0))

    def test_real_nop_slot_is_valid(self):
        self.assertEqual(self.native_effect(self.split(0x10200007, 0), 0), (0,100,BASE+32,0))

    def test_required_slot_at_file_end_is_not_invented(self):
        result, _ = self.generate([0x10200007], [('missing_slot', BASE, BASE+4)])
        self.assertNotEqual(result.returncode, 0, 'A required unavailable slot must fail')
        self.assertRegex(result.stdout + result.stderr, r'(?i)(slot|delay)')

    def test_unsupported_slot_fails_before_replacing_working_output(self):
        out = self.root / 'generated'; out.mkdir()
        sentinel = out / 'keep.cpp'; sentinel.write_text('// previous working output\n')
        result, _ = self.generate([0x10200007,0x74000000], [('bad_slot', BASE, BASE+4)])
        self.assertNotEqual(result.returncode, 0)
        self.assertRegex(result.stdout + result.stderr, r'(?i)(slot|delay)')
        self.assertEqual(sentinel.read_text(), '// previous working output\n')
        self.assertEqual(sorted(p.name for p in out.iterdir()), ['keep.cpp'])

    def test_reserved_slot_encoding_is_rejected(self):
        # SLL reserves rs=0. The encoder-shaped word must not silently become SLL.
        result, _ = self.generate([0x10200007,0x00210800], [('reserved_slot',BASE,BASE+4)])
        self.assertNotEqual(result.returncode,0)
        self.assertRegex(result.stdout+result.stderr,r'(?i)(slot|delay)')

    def test_in_body_slot_is_not_executed_twice(self):
        result, out = self.generate([0x10200007,0x24840004,RETURN,0,0,0,0,0,RETURN,0],
            [('normal_body', BASE, BASE+16), ('target', BASE+32, BASE+40)])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.native_effect(out,0), (0,104,BASE+32,0))

    def test_slot_entry_keeps_its_independent_native_effect(self):
        out = self.split(0x10200007,0x2421ffff)
        self.assertEqual(self.native_effect(out,8,entry=BASE+4), (7,100,0x2220,0))

    def test_configured_interior_entry_continues_after_clipped_end(self):
        result, out = self.generate([0x2421000a,0x24210001,0x24840004,RETURN,0,0,0,0,RETURN,0],
            [('clipped_owner', BASE, BASE+8), ('next_function', BASE+32, BASE+40)],
            entry_points=['configured@0x1004'])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.native_effect(out,3,entry=BASE+4,continuation=BASE+8), (4,104,0x2220,0))

    def test_unreferenced_indirect_fallback_slot_does_not_discover_padding(self):
        # JR a1 makes existing analysis register fallback PCs, including a return's
        # slot. That speculative label must not justify translating adjacent data.
        result,out=self.generate([0x00a00008,0,RETURN,0x24840004,0x74000000,0,0,0,RETURN,0],
            [('indirect_owner',BASE,BASE+16),('next_function',BASE+32,BASE+40)])
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        table=(out/'register_functions.cpp').read_text()
        self.assertNotRegex(table,r'\{\s*0x1010[uU]?\s*,')

    def test_exception_slot_does_not_discover_dead_bytes(self):
        result,out=self.generate([0x10200003,0x0000000d,0x74000000,0,0x74000000,0],
            [('exception_owner',BASE,BASE+4)])
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        table=(out/'register_functions.cpp').read_text()
        self.assertNotRegex(table,r'\{\s*0x10(?:08|10)[uU]?\s*,',
            'An unconditional BREAK slot terminates both branch paths')

    def test_conditional_family_boundary_slots(self):
        cases=[('blez',0x18200007,-1,2,False),('bgtz',0x1c200007,2,-1,False),
            ('beql',0x50200007,0,2,True),('bnel',0x54200007,2,0,True),
            ('blezl',0x58200007,-1,2,True),('bgtzl',0x5c200007,2,-1,True),
            ('bltz',0x04200007,-1,2,False),('bgez',0x04210007,2,-1,False),
            ('bltzl',0x04220007,-1,2,True),('bgezl',0x04230007,2,-1,True),
            ('bltzal',0x04300007,-1,2,False),('bgezal',0x04310007,2,-1,False),
            ('bltzall',0x04320007,-1,2,True),('bgezall',0x04330007,2,-1,True)]
        for name, word, taken, untaken, likely in cases:
            out = self.split(word)
            for initial, is_taken in [(taken,True), (untaken,False)]:
                with self.subTest(branch=name,taken=is_taken):
                    self.assertEqual(self.native_effect(out,initial),
                        (initial,104 if is_taken or not likely else 100,BASE+32 if is_taken else BASE+8,0))

if __name__ == '__main__':
    unittest.main()
