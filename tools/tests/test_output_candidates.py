"""042 approved output candidates: exact legacy bytes, ordered groups and failure/retry."""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import sys
import re

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/debug'))
from generated_sources import BatchError,validate_batch_index,virtual_inventory

RECOMP = Path(os.environ.get('PS2X_TEST_RECOMPILER', 'build-recomp/ps2xRecomp/ps2_recomp'))
PREFIX = '[setup-timing] '

class OutputCandidates(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='output-candidates-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.assertTrue(RECOMP.is_file(), 'Set PS2X_TEST_RECOMPILER to a built tool')

    def generate(self, count, name, *, workers=4, batch=0, bad=None):
        inp = self.root/'input.bin'
        inp.write_bytes(struct.pack('<2I', 0x03e00008, 0) * count)
        csv = self.root/'map.csv'
        csv.write_text('Name,Start,End,Size\n'+''.join(
            f'{"Z" if i%2 else "a"}_{i:04},0x{4096+8*i:x},0x{4104+8*i:x},8\n' for i in range(count)))
        out = self.root/name
        profile = self.root/'config.toml'
        profile.write_text('[general]\ninput_format="raw"\nmodule_id="output_test"\nraw_base=4096\n'+
            f'output_worker_threads={workers}\noutput_file_batch_size={batch}\n'+''.join(
                f'{key}={json.dumps(str(path))}\n' for key,path in [('input',inp),('ghidra_output',csv),('output',out)]))
        if bad:
            (out/bad).mkdir(parents=True)
        result = subprocess.run([str(RECOMP),str(profile)],capture_output=True,encoding='utf-8',timeout=60)
        return result,out

    def inventory(self, out):
        return {p.name:p.read_bytes() for p in out.iterdir() if p.is_file()}

    def test_header_write_failure_is_not_success_and_retry_is_complete(self):
        reference,out=self.generate(17,'header-reference')
        self.assertEqual(reference.returncode,0,reference.stdout+reference.stderr)
        expected=self.inventory(out)
        for batch in (0,64):
            for header in ('ps2_recompiled_functions.h','ps2_recompiled_stubs.h'):
                with self.subTest(batch=batch,header=header):
                    label=f'blocked-{batch}-{header}'
                    failed,folder=self.generate(17,label,batch=batch,bad=header)
                    self.assertNotEqual(failed.returncode,0,'a refused header write must not report completed output')
                    self.assertNotIn('Recompilation completed successfully',failed.stdout)
                    (folder/header).rmdir() # the fixture created this empty blocking directory
                    retry,_=self.generate(17,label,batch=batch)
                    self.assertEqual(retry.returncode,0,retry.stdout+retry.stderr)
                    actual=self.virtual(folder)[0] if batch else self.inventory(folder)
                    self.assertEqual(actual,expected,'retry must recover the entire exact output inventory')

    def virtual(self, out):
        index = validate_batch_index(out)
        self.assertEqual(index['schema'],1)
        self.assertEqual(index['batch_size'],64)
        blobs={b['file']:(out/b['file']).read_bytes() for b in index['batches']}
        files={p.name:p.read_bytes() for p in out.glob('*.h')}
        for row in index['files']:
            self.assertNotIn(row['name'],files,'duplicate virtual filename')
            data=blobs[row['file']][row['offset']:row['offset']+row['size']]
            files[row['name']]=data
        self.assertEqual([r['name'] for r in index['files']],sorted(r['name'] for r in index['files']))
        for b in index['batches']:
            self.assertLessEqual(b['count'],64)
        return files,index

    def test_corrupt_index_or_extra_output_is_rejected(self):
        result,out=self.generate(65,'tamper',batch=64)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        index=validate_batch_index(out)
        batch=out/index['batches'][0]['file'];original=batch.read_bytes()
        batch.write_bytes(original+b' ')
        with self.assertRaises(BatchError):validate_batch_index(out)
        batch.write_bytes(original)
        extra=out/'stale.cpp';extra.write_text('// stale')
        with self.assertRaises(BatchError):validate_batch_index(out)
        extra.unlink()
        metadata=out/'ps2_generated_index.json';old=metadata.read_bytes();bad=json.loads(old)
        bad['files'][1]['offset']=bad['files'][0]['offset']
        metadata.write_text(json.dumps(bad))
        with self.assertRaises(BatchError):validate_batch_index(out)
        metadata.write_bytes(old)
        self.assertEqual(len(virtual_inventory(out)),68)

    def test_cmake_preserves_unity_filenames_and_maintained_tail(self):
        for count in [61,62,63,64,65]:
            result,out=self.generate(count,f'cmake-input-{count}',batch=64)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            source=self.root/f'cmake-project-{count}';source.mkdir()
            build=self.root/f'cmake-build-{count}'
            extra=[source/'maintained main.cpp',source/'debug.cpp']
            for path in extra:path.write_text('// maintained')
            index=validate_batch_index(out)
            diagnostic=index['files'][0]['name']
            helper=(ROOT/'app/cmake/generated_sources.cmake').as_posix()
            (source/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.20)\nproject(BatchGroups NONE)\n'+
                f'include("{helper}")\n'+f'dw5xl_generated_sources(check "{out.as_posix()}" "{extra[0].as_posix()};{extra[1].as_posix()}" files batched)\n'+
                f'dw5xl_generated_function("{out.as_posix()}" "{diagnostic}" diagnostic)\n'+
                'if(NOT batched)\nmessage(FATAL_ERROR "batch mode not selected")\nendif()\n')
            cmake=os.environ.get('PS2X_TEST_CMAKE','cmake')
            run=subprocess.run([cmake,'-S',str(source),'-B',str(build),'-G','Ninja'],capture_output=True,encoding='utf-8',timeout=60)
            self.assertEqual(run.returncode,0,run.stdout+run.stderr)
            self.assertEqual((build/'diagnostic-generated'/diagnostic).read_bytes(),self.virtual(out)[0][diagnostic])
            wrappers=sorted((build/'CMakeFiles/check.dir/Unity').glob('unity_*_cxx.cxx'))
            expected=(count+1+2+63)//64
            self.assertEqual(len(wrappers),expected)
            content=''.join(p.read_text() for p in wrappers)
            for path in extra:self.assertEqual(content.count(path.as_posix()),1)
            for i,path in enumerate(wrappers):self.assertEqual(path.name,f'unity_{i}_cxx.cxx')
            index=validate_batch_index(out)
            for batch in index['batches']:self.assertEqual(content.count(batch['file']),1)
            virtual=[row['name'] for row in index['files']]+[p.name for p in extra]
            groups=[]
            for wrapper in wrappers:
                expanded=[]
                for include in re.findall(r'#include "([^"]+)"',wrapper.read_text()):
                    name=Path(include).name
                    if name.startswith('ps2_batch_'):
                        expanded.extend(row['name'] for row in index['files'] if row['file']==name)
                    else:expanded.append(name)
                groups.append(expanded)
            self.assertEqual(groups,[virtual[i:i+64] for i in range(0,len(virtual),64)])

    def test_parallel_writes_preserve_every_byte(self):
        a,serial=self.generate(193,'serial',workers=1)
        b,parallel=self.generate(193,'parallel')
        self.assertEqual(a.returncode,0,a.stdout+a.stderr)
        self.assertEqual(b.returncode,0,b.stdout+b.stderr)
        self.assertEqual(self.inventory(serial),self.inventory(parallel))
        events=[json.loads(s[len(PREFIX):]) for s in b.stdout.splitlines() if s.startswith(PREFIX)]
        output=next(e for e in events if e['event']=='end' and e['stage']=='output')
        self.assertEqual(output['counts']['parallel_function_writers'],4)
        self.assertEqual(output['counts']['function_files'],193)

    def test_batched_bytes_and_group_boundaries_match_legacy(self):
        for count in [1,62,63,64,65,127,128,193]:
            with self.subTest(count=count):
                a,serial=self.generate(count,f'serial-{count}',workers=1)
                b,batch=self.generate(count,f'batch-{count}',batch=64)
                self.assertEqual(a.returncode,0,a.stdout+a.stderr)
                self.assertEqual(b.returncode,0,b.stdout+b.stderr)
                files,index=self.virtual(batch)
                self.assertEqual(files,self.inventory(serial))
                cpp=sorted(n for n in files if n.endswith('.cpp'))
                actual=[[r['name'] for r in index['files'] if r['file']==b['file']] for b in index['batches']]
                self.assertEqual(actual,[cpp[i:i+64] for i in range(0,len(cpp),64)])
                self.assertEqual(sum(b['count'] for b in index['batches']),count+1,'registration included once')
                repeated,repeat=self.generate(count,f'repeated-{count}',batch=64)
                self.assertEqual(repeated.returncode,0,repeated.stdout+repeated.stderr)
                self.assertEqual(self.inventory(batch),self.inventory(repeat),'batch bytes independent of output root')

    def test_write_error_stops_and_retry_produces_complete_inventory(self):
        baseline,serial=self.generate(193,'reference',workers=1)
        self.assertEqual(baseline.returncode,0,baseline.stdout+baseline.stderr)
        bad=next(p.name for p in serial.glob('*_0x1008.cpp'))
        failed,out=self.generate(193,'failure',bad=bad)
        self.assertNotEqual(failed.returncode,0)
        self.assertNotIn('Recompilation completed successfully',failed.stdout)
        (out/bad).rmdir()
        again,out=self.generate(193,'failure')
        self.assertEqual(again.returncode,0,again.stdout+again.stderr)
        self.assertEqual(self.inventory(out),self.inventory(serial))

if __name__=='__main__':unittest.main()
