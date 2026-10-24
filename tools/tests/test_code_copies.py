"""Independent synthetic copied-code generation/validation regressions; no game data."""
import copy
import json
import struct
import subprocess
import sys
import unittest
import test_function_boundaries as fixtures

class CodeCopies(unittest.TestCase):
    def setUp(self):
        self.fixture=fixtures.FunctionBoundaries()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)

    def descriptor(self):
        return dict(name='kernel_copy',source=fixtures.BASE,destination=0x80070000,
                    size=40,functions=[[0,16],[32,8]])

    def words(self):
        return [0x10200007,0x24840004,fixtures.RETURN,0,0,0,0,0,fixtures.RETURN,0]

    def test_relocated_branch_effects_use_execution_address(self):
        result,out=self.fixture.generate(self.words(),[('original',fixtures.BASE,fixtures.BASE+16)],
                                         code_copies=[self.descriptor()])
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertEqual(self.fixture.native_effect(out,0,entry=0x80070000),(0,104,0x80070020,0))
        self.assertEqual(self.fixture.native_effect(out,1,entry=0x80070000),(1,104,0x2220,0))
        table=(out/'register_functions.cpp').read_text()
        self.assertIn('RecompiledCodeImageRegistration',table)
        self.assertIn('RecompiledCodeWord',table)
        self.assertNotIn('{0x80070010u,',table,'unqualified data must not become a native entry')

    def test_invalid_copies_preserve_existing_output(self):
        cases=[dict(source=fixtures.BASE+4,size=40),dict(destination=0x80001000),
               dict(destination=0x82000000),dict(destination=0x40070000),dict(size=0),
               dict(functions=[]),dict(functions=[[0,41]]),dict(functions=[[0,16],[8,8]]),
               dict(functions=[[0,3]]),dict(source=-4),dict(destination=0x80070001),
               dict(name='bad-name')]
        for changes in cases:
            with self.subTest(changes=changes):
                descriptor=self.descriptor();descriptor.update(changes)
                self.fixture.must_preserve_output_on_failure(self.words(),[('original',fixtures.BASE,fixtures.BASE+16)],
                    r'(?i)(code.copy|invalid address)',code_copies=[descriptor])

    def test_unmapped_debug_bytes_cannot_supply_an_executable_copy(self):
        words=struct.pack('<10I',*self.words())
        image=bytearray(self.fixture.elf_sections([
            ('.text',fixtures.BASE,struct.pack('<II',fixtures.RETURN,0),6,1,8),
            ('.debug_info',0x5000,words,0,1,len(words))]))
        struct.pack_into('<I',image,52+32,0) # debug bytes have no PT_LOAD mapping
        descriptor=self.descriptor();descriptor['source']=0x5000
        self.fixture.must_preserve_output_on_failure([], [('original',fixtures.BASE,fixtures.BASE+8)],
            r'(?i)code.copy',image=bytes(image),input_format='elf',code_copies=[descriptor])

    def test_unmapped_debug_sections_do_not_reserve_guest_ram(self):
        words=struct.pack('<10I',*self.words())
        image=bytearray(self.fixture.elf_sections([
            ('.text',fixtures.BASE,struct.pack('<II',fixtures.RETURN,0),6,1,8),
            ('.data',fixtures.BASE+0x100,words,3,1,len(words)),
            ('.debug_info',0,bytes(0x80000),0,1,0x80000)]))
        struct.pack_into('<I',image,52+2*32,0)
        descriptor=self.descriptor();descriptor['source']=fixtures.BASE+0x100
        result,out=self.fixture.generate([], [('original',fixtures.BASE,fixtures.BASE+8)],
            image=bytes(image),input_format='elf',code_copies=[descriptor])
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertEqual(self.fixture.native_effect(out,1,entry=0x80070000),(1,104,0x2220,0))

    def test_copied_view_takes_precedence_over_unmapped_metadata(self):
        words=struct.pack('<10I',*self.words())
        image=bytearray(self.fixture.elf_sections([
            ('.text',fixtures.BASE,struct.pack('<II',fixtures.RETURN,0),6,1,8),
            ('.data',fixtures.BASE+0x100,words,3,1,len(words)),
            ('.debug_info',0x80070000,bytes(40),0,1,40)]))
        struct.pack_into('<I',image,52+2*32,0)
        descriptor=self.descriptor();descriptor['source']=fixtures.BASE+0x100
        result,out=self.fixture.generate([], [('original',fixtures.BASE,fixtures.BASE+8)],
            image=bytes(image),input_format='elf',code_copies=[descriptor])
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertEqual(self.fixture.native_effect(out,1,entry=0x80070000),(1,104,0x2220,0))

    def test_zero_only_code_cannot_match_uninitialized_ram(self):
        self.fixture.must_preserve_output_on_failure([0]*10,[('original',fixtures.BASE,fixtures.BASE+16)],
            r'(?i)code.copy.*nonzero',code_copies=[self.descriptor()])

    def test_duplicate_images_and_physical_aliases_are_rejected(self):
        for changes in ({},{'name':'other','destination':0xa0070000}):
            with self.subTest(changes=changes):
                one=self.descriptor();two=copy.deepcopy(one);two.update(changes)
                self.fixture.must_preserve_output_on_failure(self.words(),[('original',fixtures.BASE,fixtures.BASE+16)],
                    r'(?i)code.copy',code_copies=[one,two])

    def test_independent_diagnostic_checks_copied_binary_paths(self):
        result,out=self.fixture.generate(self.words(),[('original',fixtures.BASE,fixtures.BASE+16)],
                                         code_copies=[self.descriptor()])
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        f=self.fixture;report=f.root/'diagnostic'
        command=[sys.executable,str(fixtures.ROOT/'tools/debug/validate_function_map.py'),
            '--profile',str(f.root/'profile.toml'),'--binary',str(f.root/'input.bin'),
            '--csv',str(f.root/'functions.csv'),'--passes','scan,reachability','--out',str(report)]
        run=subprocess.run(command,capture_output=True,text=True,timeout=30)
        self.assertIn(run.returncode,[0,1],run.stdout+run.stderr)
        data=json.loads((report/'report.json').read_text())
        self.assertTrue(any(x['start']==0x80070000 for x in data['module']['mapping']))
        self.assertFalse(data['errors'],data['errors'])
        self.assertFalse(any(x['code'] in ('TARGET_UNMAPPED','ROW_UNMAPPED') and x.get('address',0)>=0x80070000
                             for x in data['findings']),data['findings'])

if __name__=='__main__':unittest.main()
