"""030 known-answer contract/evidence tests; entirely synthetic and offline."""
import copy
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import binding_fixtures as fixtures

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / 'tools/debug/audit_bindings.py'


class BindingAudit(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='binding-audit-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.manifest = fixtures.fixture(self.root)
        self.counter = 0

    def run_audit(self, data=None, *, output=None, extra=(), raw=None):
        self.assertTrue(TOOL.is_file(), '030 tool absent: implement only after this test-first failure')
        path = self.root / 'manifest.json'
        if raw is None:
            fixtures.write_json(path, self.manifest if data is None else data)
        else:
            path.write_text(raw, encoding='utf-8')
        self.counter += 1
        out = output or self.root / ('out-' + str(self.counter))
        proc = subprocess.run([sys.executable, '-B', str(TOOL), '--manifest', str(path), '--out', str(out), *extra],
                              capture_output=True, text=True, timeout=15)
        report = json.loads((out / 'report.json').read_text()) if out.is_dir() and (out / 'report.json').is_file() else None
        return proc, report, out

    def test_compatible_alias_and_reports(self):
        p, report, out = self.run_audit()
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertEqual(report['status'], 'complete')
        self.assertEqual(report['bindings'][0]['selection']['handler'], 'sample')
        self.assertEqual([f['status'] for f in report['bindings'][0]['fields']], ['compatible', 'compatible'])
        md = (out / 'report.md').read_text()
        self.assertIn('sample', md)
        self.assertIn('compatible', md)
        self.assertIn('No execution', md)
        self.assertNotIn(str(self.root), md)

    def test_every_supported_kind_positive_negative_unknown(self):
        mutations = {
            'argument': ('bits', 64), 'return': ('bits', 128),
            'memory_output': ('access', 'none'), 'state_effect': ('effect', 'unchanged'),
            'callback': ('context', 'different'),
            'ownership': ('operations', {'allocate': 'runtime', 'free': 'original', 'reallocate': 'runtime'}),
            'clobber': ('locations', ['gpr:t0']), 'stack': ('delta', 16),
            'gp': ('policy', 'caller_supplied'), 'continuation': ('policy', 'resume'), 'mmio': ('access', 'write')}
        for kind in fixtures.KINDS:
            with self.subTest(kind=kind):
                data = copy.deepcopy(self.manifest)
                f = fixtures.field(kind)
                data['bindings'][0]['fields'] = [f]
                p, r, _ = self.run_audit(data)
                self.assertEqual((p.returncode, r['bindings'][0]['fields'][0]['status']), (0, 'compatible'))
                key, value = mutations[kind]
                f['replacement']['value'][key] = value
                p, r, _ = self.run_audit(data)
                self.assertEqual((p.returncode, r['bindings'][0]['fields'][0]['status']), (1, 'contradiction'))
                data['conditions'][0]['state'] = 'unknown'
                data['conditions'][0].pop('value')
                p, r, _ = self.run_audit(data)
                self.assertEqual((p.returncode, r['bindings'][0]['fields'][0]['status']), (2, 'unknown'))

    def test_extra_stack_arguments_float_and_wide_returns(self):
        for kind, expected, observed in [
            ('argument', {'locations':['gpr:t0'],'bits':32,'representation':'signed'}, {'locations':['gpr:t1'],'bits':32,'representation':'signed'}),
            ('argument', {'locations':['stack:+0x20'],'bits':64,'representation':'software_double'}, {'locations':['fpr:f12'],'bits':32,'representation':'float'}),
            ('return', {'locations':['gpr:v0'],'bits':128,'representation':'raw'}, {'locations':['gpr:v0'],'bits':32,'representation':'unsigned'})]:
            data = copy.deepcopy(self.manifest)
            f = fixtures.field(kind);f['original']['value'] = expected;f['replacement']['value'] = observed
            data['bindings'][0]['fields'] = [f]
            p, r, _ = self.run_audit(data)
            self.assertEqual(p.returncode, 1)
            self.assertEqual(r['bindings'][0]['fields'][0]['status'], 'contradiction')

    def test_unusual_float_locations_and_control_flag(self):
        fields = []
        for i, location in enumerate(['fpr:f12','fpr:f13','gpr:a0']):
            f = fixtures.field('argument', name='arg'+str(i))
            value = {'locations':[location], 'bits':32, 'representation':'float' if i<2 else 'signed'}
            f['original']['value'] = value;f['replacement']['value'] = copy.deepcopy(value);fields.append(f)
        self.manifest['bindings'][0]['fields'] = fields
        p, r, _ = self.run_audit()
        self.assertEqual(p.returncode, 0)
        self.assertTrue(all(f['status']=='compatible' for f in r['bindings'][0]['fields']))

    def test_physical_register_and_stack_aliases_are_compatible(self):
        for kind,original,observed in [
            ('argument',{'locations':['gpr:a0'],'bits':32,'representation':'unsigned'},
                        {'locations':['gpr:r4'],'bits':32,'representation':'unsigned'}),
            ('return',{'locations':['stack:+0x020'],'bits':64,'representation':'raw'},
                      {'locations':['stack:+0x20'],'bits':64,'representation':'raw'}),
            ('gp',{'location':'gpr:fp','policy':'preserved'},{'location':'gpr:s8','policy':'preserved'}),
            ('clobber',{'locations':['gpr:t0','gpr:ra']},{'locations':['gpr:r31','gpr:r8']})]:
            data=copy.deepcopy(self.manifest);f=fixtures.field(kind)
            f['original']['value']=original;f['replacement']['value']=observed
            data['bindings'][0]['fields']=[f]
            p,r,_=self.run_audit(data);self.assertEqual(p.returncode,0)
            self.assertEqual(r['bindings'][0]['fields'][0]['status'],'compatible')

    def test_observations_are_not_behavioral_verdicts(self):
        self.manifest['bindings'][0]['observations'] = [{'id':'tail-policy','required':True,
            'original':fixtures.fact('computed correction','original'),
            'replacement':fixtures.fact('constant zero','replacement')}]
        f = fixtures.field('state_effect')
        f['replacement']['value']['effect'] = 'unchanged'
        self.manifest['bindings'][0]['fields'].append(f)
        self.manifest['conditions'].append({'id':'failure','state':'unknown','evidence':['original'],
                                             'rationale':'Corresponding exhaustion not established.'})
        f['condition'] = 'failure'
        p, r, out = self.run_audit()
        self.assertEqual(p.returncode, 2)
        b = r['bindings'][0]
        self.assertEqual(b['observations'][0]['status'], 'verified')
        self.assertEqual(b['observations'][0]['comparison'], 'different_descriptions')
        self.assertEqual(b['fields'][-1]['status'], 'unknown')
        self.assertIn('computed correction',(out/'report.md').read_text())

    def test_incomplete_dominates_without_hiding_contradictions(self):
        self.manifest['bindings'][0]['fields'][0]['replacement']['value']['locations'] = ['gpr:a1']
        self.manifest['bindings'][0]['fields'][1]['replacement'] = None
        p, r, _ = self.run_audit()
        self.assertEqual(p.returncode, 2)
        self.assertEqual(r['counts']['contradiction'], 1)
        self.assertEqual(r['counts']['unknown'], 1)

    def test_unsupported_required_and_optional(self):
        f = fixtures.field('argument');f['kind'] = 'future_semantics'
        self.manifest['bindings'][0]['fields'] = [f]
        p, r, _ = self.run_audit();self.assertEqual(p.returncode, 2)
        self.assertIn('UNSUPPORTED_FACT', r['bindings'][0]['fields'][0]['reasons'])
        f['required'] = False
        p, r, _ = self.run_audit();self.assertEqual(p.returncode, 0)
        self.assertEqual(r['bindings'][0]['fields'][0]['status'], 'unknown')

    def test_conditional_selection_and_ambiguity(self):
        other = copy.deepcopy(self.manifest['registrations'][0]);other['id']='other';other['condition']='optional'
        self.manifest['registrations'].append(other)
        self.manifest['conditions'].append({'id':'optional','state':'established','value':False,
                                             'evidence':['original'],'rationale':'Inactive authored alternative.'})
        p, r, _ = self.run_audit();self.assertEqual(p.returncode, 0)
        self.manifest['conditions'][-1]['state']='unknown';self.manifest['conditions'][-1].pop('value')
        p, r, _ = self.run_audit();self.assertEqual(p.returncode, 2)
        self.assertEqual(r['bindings'][0]['selection']['status'], 'unknown')
        self.manifest['conditions'][-1].update(state='established',value=True)
        p, r, _ = self.run_audit();self.assertEqual(p.returncode, 2)
        self.assertIn('AMBIGUOUS_SELECTION',r['bindings'][0]['selection']['reasons'])

    def test_false_and_incompatible_comparison_conditions_remain_unknown(self):
        # An unequal annotation is not a behavioral contradiction unless the
        # original/replacement comparison conditions have been established.
        for state, value, reason in [('established', False, 'CONDITION_FALSE'),
                                     ('incompatible', None, 'CONDITION_INCOMPATIBLE')]:
            for unequal in (False, True):
                with self.subTest(state=state, unequal=unequal):
                    data = copy.deepcopy(self.manifest)
                    condition = {'id': 'comparison', 'state': state, 'evidence': ['original'],
                                 'rationale': 'This comparison is not established as applicable.'}
                    if value is not None:
                        condition['value'] = value
                    data['conditions'].append(condition)
                    field = data['bindings'][0]['fields'][0]
                    field['condition'] = 'comparison'
                    if unequal:
                        field['replacement']['value']['locations'] = ['gpr:a1']
                    p, report, _ = self.run_audit(data)
                    self.assertEqual(p.returncode, 2)
                    self.assertEqual(report['bindings'][0]['selection']['status'], 'compatible')
                    checked = report['bindings'][0]['fields'][0]
                    self.assertEqual(checked['status'], 'unknown')
                    self.assertIn(reason, checked['reasons'])
                    self.assertEqual(report['counts']['contradiction'], 0)

    def test_wrong_handler_alias_and_missing_registration(self):
        self.manifest['bindings'][0]['handler']='wrong'
        p, r, _ = self.run_audit();self.assertEqual(p.returncode, 1)
        self.assertEqual(r['bindings'][0]['selection']['status'], 'contradiction')
        self.manifest['aliases'].append({'name':'_sample','handler':'another','evidence':['replacement']})
        p, r, _ = self.run_audit();self.assertEqual(p.returncode, 2)
        self.manifest['registrations']=[]
        p, r, _ = self.run_audit();self.assertEqual(p.returncode, 2)

    def test_overlay_addresses_remain_separate(self):
        module = copy.deepcopy(self.manifest['modules'][0]);module['id']='overlay';module['load_generation']='second'
        self.manifest['modules'].append(module)
        reg=copy.deepcopy(self.manifest['registrations'][0]);reg.update(id='overlay_reg',module='overlay')
        self.manifest['registrations'].append(reg)
        b=copy.deepcopy(self.manifest['bindings'][0]);b.update(id='overlay_binding',module='overlay')
        self.manifest['bindings'].append(b)
        p,r,_=self.run_audit();self.assertEqual(p.returncode,0)
        self.assertEqual([b['module'] for b in r['bindings']],['test','overlay'])

    def test_stale_missing_and_range_evidence(self):
        for change in ['hash','binary_hash','profile_hash','missing','line','contains','bytes','profile']:
            with self.subTest(change=change):
                data=copy.deepcopy(self.manifest)
                if change=='hash':data['artifacts'][1]['sha256']='0'*64
                elif change=='binary_hash':data['artifacts'][0]['sha256']='0'*64
                elif change=='profile_hash':data['artifacts'][3]['sha256']='0'*64
                elif change=='missing':data['artifacts'][1]['path']='missing.txt'
                elif change=='line':data['citations'][0]['lines']=[1,999]
                elif change=='contains':data['citations'][0]['contains']='never present'
                elif change=='bytes':data['citations'][-1]['instructions'][0]['hex']='00000000'
                else:data['registrations'][0]['profile']['entry']='other@0x1000'
                p,r,_=self.run_audit(data)
                self.assertEqual(p.returncode,2)
                self.assertEqual(r['status'],'incomplete')

    def test_ghidra_instruction_correspondence_and_limits(self):
        image_hash=self.manifest['artifacts'][0]['sha256']
        blob=('PROGRAM\tauthored\t'+image_hash+'\tr5900:LE:32:default\nINS\t00001000\t0800e003\treturn\n').encode()
        path=self.root/'export.tsv';path.write_bytes(blob)
        self.manifest['artifacts'].append({'id':'export','path':path.name,'sha256':fixtures.digest(blob),'kind':'ghidra','module':'test'})
        p,r,_=self.run_audit();self.assertEqual(p.returncode,0)
        self.assertEqual(r['artifacts'][-1]['instructions_checked'],1)
        self.manifest['artifacts'][-1]['sha256']='0'*64
        p,r,_=self.run_audit();self.assertEqual(p.returncode,2)
        self.assertTrue(any(i['code']=='STALE_ARTIFACT' and i['subject']=='export' for i in r['issues']))
        self.manifest['artifacts'][-1]['sha256']=fixtures.digest(blob)
        wrong_identity=blob.replace(image_hash.encode(),b'0'*64)
        path.write_bytes(wrong_identity);self.manifest['artifacts'][-1]['sha256']=fixtures.digest(wrong_identity)
        p,r,_=self.run_audit();self.assertEqual(p.returncode,2)
        self.assertTrue(any(i['code']=='GHIDRA_IDENTITY' for i in r['issues']))
        bad=blob.replace(b'0800e003',b'00000000');path.write_bytes(bad);self.manifest['artifacts'][-1]['sha256']=fixtures.digest(bad)
        p,r,_=self.run_audit();self.assertEqual(p.returncode,2)
        self.assertTrue(any(i['code']=='ORIGINAL_BYTES' for i in r['issues']))

    def test_conflicting_facts_and_malformed_inputs(self):
        for change in ['duplicate_id','bad_type','unknown_condition','bad_width','executed_scope']:
            data=copy.deepcopy(self.manifest)
            if change=='duplicate_id':data['bindings'][0]['fields'].append(copy.deepcopy(data['bindings'][0]['fields'][0]))
            elif change=='bad_type':data['bindings'][0]['fields'][0]['required']=1
            elif change=='unknown_condition':data['bindings'][0]['fields'][0]['condition']='absent'
            elif change=='bad_width':data['bindings'][0]['fields'][0]['original']['value']['bits']=True
            else:data['bindings'][0]['fields'][0]['original']['scope']='executed_original'
            p,r,out=self.run_audit(data);self.assertEqual(p.returncode,2);self.assertIsNone(r);self.assertFalse(out.exists())
        for raw in ['[]','{"schema_version":1,"schema_version":1}', '['*2000+']'*2000, '{"schema_version":NaN}']:
            p,r,out=self.run_audit(raw=raw);self.assertEqual(p.returncode,2);self.assertFalse(out.exists())

    def test_existing_output_and_aliases_are_preserved(self):
        out=self.root/'existing';out.mkdir();keep=out/'keep';keep.write_bytes(b'unchanged')
        p,r,_=self.run_audit(output=out);self.assertEqual(p.returncode,2);self.assertEqual(keep.read_bytes(),b'unchanged')
        original=self.root/'original.txt';before=original.read_bytes()
        p,r,_=self.run_audit(output=original);self.assertEqual(p.returncode,2);self.assertEqual(original.read_bytes(),before)
        hard=self.root/'hard';os.link(original,hard)
        p,r,_=self.run_audit(output=hard);self.assertEqual(p.returncode,2);self.assertEqual(original.read_bytes(),before)

    def test_symlink_output_is_preserved(self):
        out=self.root/'existing';out.mkdir();keep=out/'keep';keep.write_bytes(b'unchanged')
        link=self.root/'link'
        try:link.symlink_to(out,target_is_directory=True)
        except OSError as exc:self.skipTest('Symlink privilege unavailable: '+str(exc.errno))
        p,r,_=self.run_audit(output=link);self.assertEqual(p.returncode,2);self.assertEqual(keep.read_bytes(),b'unchanged')
        dangling=self.root/'dangling';dangling.symlink_to(self.root/'absent-target')
        p,r,_=self.run_audit(output=dangling);self.assertEqual(p.returncode,2)
        self.assertTrue(dangling.is_symlink());self.assertFalse((self.root/'absent-target').exists())

    @unittest.skipUnless(os.name=='nt', 'Windows junction check requires Windows')
    def test_windows_junction_output_is_preserved(self):
        target=self.root/'target';target.mkdir();keep=target/'keep';keep.write_bytes(b'unchanged')
        link=self.root/'junction'
        result=subprocess.run(['cmd','/c','mklink','/J',str(link),str(target)],capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.addCleanup(lambda: os.rmdir(link) if link.exists() else None)
        p,r,_=self.run_audit(output=link);self.assertEqual(p.returncode,2);self.assertEqual(keep.read_bytes(),b'unchanged')

    def test_conflicting_alternatives_keep_other_findings(self):
        first=self.manifest['bindings'][0]['fields'][0]
        other=copy.deepcopy(first['original']);other['value']['locations']=['gpr:a1']
        first['original']=[first['original'],other]
        self.manifest['bindings'][0]['fields'][1]['replacement']['value']['bits']=64
        p,r,_=self.run_audit();self.assertEqual(p.returncode,2)
        self.assertIn('CONFLICTING_ANNOTATIONS',r['bindings'][0]['fields'][0]['reasons'])
        self.assertEqual(r['bindings'][0]['fields'][1]['status'],'contradiction')

    def test_invalid_relationship_types_are_bounded_diagnostics(self):
        for group,key in [('modules','artifact'),('modules','format'),('citations','artifact'),
                          ('registrations','module'),('registrations','condition'),('bindings','module')]:
            data=copy.deepcopy(self.manifest);data[group][0][key]=[]
            p,r,out=self.run_audit(data)
            self.assertEqual(p.returncode,2,p.stderr);self.assertNotIn('Traceback',p.stderr);self.assertFalse(out.exists())
            self.assertEqual(json.loads(p.stdout)['status'],'invalid')

    def test_output_write_failure_cleans_only_owned_artifacts(self):
        fixtures.write_json(self.root/'manifest.json',self.manifest)
        with mock.patch.object(sys,'path',[str(ROOT/'tools/debug')]+sys.path):
            spec=importlib.util.spec_from_file_location('binding_audit_output_test',TOOL)
            checker=importlib.util.module_from_spec(spec);spec.loader.exec_module(checker)
        original_open=Path.open
        def fail_report(path,*args,**kwargs):
            if path.name=='report.md':raise PermissionError('simulated output failure')
            return original_open(path,*args,**kwargs)
        out=self.root/'failed-output'
        with mock.patch.object(Path,'open',fail_report),contextlib.redirect_stdout(io.StringIO()) as stdout:
            result=checker.main(['--manifest',str(self.root/'manifest.json'),'--out',str(out)])
        self.assertEqual(result,2);self.assertEqual(json.loads(stdout.getvalue())['code'],'OUTPUT_WRITE')
        self.assertFalse(out.exists());self.assertEqual((self.root/'original.txt').read_bytes(),b'input a0, output v0\nindependently specified contract\n')

    def test_unaligned_original_word_cannot_pass(self):
        self.manifest['citations'][-1]['instructions']=[{'address':'0x1001','hex':'00e00300'}]
        p,r,_=self.run_audit();self.assertEqual(p.returncode,2)

    def test_ghidra_explicit_incomplete_export_is_retained(self):
        image_hash=self.manifest['artifacts'][0]['sha256']
        blob=('PROGRAM\tauthored\t'+image_hash+'\tr5900:LE:32:default\nINS\t00001000\t0800e003\treturn\nINCOMPLETE\tbounded caller export\n').encode()
        (self.root/'limited.tsv').write_bytes(blob)
        self.manifest['artifacts'].append({'id':'limited','path':'limited.tsv','sha256':fixtures.digest(blob),'kind':'ghidra','module':'test'})
        p,r,_=self.run_audit();self.assertEqual(p.returncode,2)
        self.assertEqual(r['artifacts'][-1]['instructions_checked'],1)
        self.assertTrue(r['artifacts'][-1]['coverage_limits'])

    def test_numeric_overflow_is_not_silently_emitted_as_infinity(self):
        data=copy.deepcopy(self.manifest)
        f=data['bindings'][0]['fields'][0];f['kind']='future';f['original']['value']='overflow'
        raw=json.dumps(data).replace('"overflow"','1e999')
        p,r,out=self.run_audit(raw=raw)
        self.assertEqual(p.returncode,2);self.assertIsNone(r);self.assertFalse(out.exists())

    @unittest.skipUnless(hasattr(os,'mkfifo'), 'POSIX nonregular-input check requires mkfifo')
    def test_fifo_input_is_refused_without_blocking(self):
        pipe=self.root/'pipe';os.mkfifo(pipe)
        self.manifest['artifacts'][1]['path']=pipe.name
        p,r,_=self.run_audit();self.assertEqual(p.returncode,2)
        self.assertTrue(any(i['code']=='MISSING_ARTIFACT' for i in r['issues']))

    def test_limits_fail_without_mutation(self):
        before={p.name:p.read_bytes() for p in self.root.iterdir() if p.is_file()}
        for flag in ['--max-manifest-bytes','--max-artifact-bytes','--max-total-bytes','--max-records','--max-report-bytes']:
            p,r,out=self.run_audit(extra=[flag,'1']);self.assertEqual(p.returncode,2);self.assertFalse(out.exists())
        for name,blob in before.items():self.assertEqual((self.root/name).read_bytes(),blob)

    def test_repeat_and_changed_evidence_require_review(self):
        p,r,_=self.run_audit();self.assertEqual(p.returncode,0)
        p,again,_=self.run_audit();self.assertEqual(p.returncode,0);self.assertEqual(r,again)
        path=self.root/'handler.cpp';path.write_bytes(path.read_bytes()+b'// changed\n')
        p,r,_=self.run_audit();self.assertEqual(p.returncode,2)
        self.assertTrue(any(i['code']=='STALE_ARTIFACT' for i in r['issues']))


if __name__ == '__main__':
    unittest.main()
