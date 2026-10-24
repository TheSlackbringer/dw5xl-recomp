#!/usr/bin/env python3
"""Independent synthetic CLI contracts for 031; no game data or private notes."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

TOOL = Path(__file__).resolve().parents[1] / 'debug/validate_function_map.py'
BASE = 0x1000
NOP, RETURN = 0, 0x03e00008


def branch(op, at, target, rs=1, rt=0):
    return op << 26 | rs << 21 | rt << 16 | (((target-at-4)//4) & 0xffff)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def elf(words, base=BASE, section_name='.text'):
    """Authored valid ELF32 MIPS, one load segment plus a named executable section."""
    payload = struct.pack('<'+'I'*len(words), *words)
    names = b'\0'+section_name.encode()+b'\0.shstrtab\0'
    shoff = (0x100+len(payload)+len(names)+3)&~3
    result = bytearray(shoff+120)
    struct.pack_into('<16sHHIIIIIHHHHHH', result, 0,
        b'\x7fELF\x01\x01\x01'+b'\0'*9, 2, 8, 1, base, 52, shoff, 0, 52, 32, 1, 40, 3, 2)
    struct.pack_into('<8I', result, 52, 1, 0x100, base, base, len(payload), len(payload), 5, 4)
    result[0x100:0x100+len(payload)] = payload
    result[0x100+len(payload):0x100+len(payload)+len(names)] = names
    struct.pack_into('<10I', result, shoff+40, 1, 1, 6, base, 0x100, len(payload), 0, 0, 4, 0)
    struct.pack_into('<10I', result, shoff+80, len(section_name)+2, 3, 0, 0,
                     0x100+len(payload), len(names), 0, 0, 1, 0)
    return bytes(result)


class FunctionMap(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='function-map-fixtures-')
        self.root = Path(self.temporary.name)
        self.binary = self.root/'input.bin'
        self.csv = self.root/'map.csv'
        self.profile = self.root/'profile.toml'
        self.counter = 0
        self.configure([RETURN, NOP], [('ret', BASE, BASE+8)])

    def tearDown(self):
        self.temporary.cleanup()

    def configure(self, words, rows, entries=(), *, raw=True, base=BASE, prefix=0, name='fixture'):
        self.binary.write_bytes(b'\0'*prefix+struct.pack('<'+'I'*len(words), *words) if raw else elf(words, base))
        self.csv.write_text('Name,Start,End,Size\n'+''.join(
            f'{label},0x{start:08x},0x{end:08x},{end-start}\n' for label,start,end in rows))
        self.profile.write_text('[general]\n'+f'module_id = "{name}"\n'+
            (f'input_format = "raw"\nraw_base = {base}\nraw_code_offset = {prefix}\n' if raw else 'input_format = "elf"\n')+
            'entry_points = '+json.dumps(list(entries))+'\n')

    def invoke(self, *extra, code=None, manifest=None, generated=None):
        self.assertTrue(TOOL.is_file(), '031 tool is missing: intended detector expectations have not run')
        self.counter += 1
        out = self.root/f'report-{self.counter}'
        args = [sys.executable, str(TOOL)]
        if manifest:
            args += ['--manifest', str(manifest), '--module', 'fixture', '--map', 'cleaned']
        else:
            args += ['--profile', str(self.profile), '--binary', str(self.binary), '--csv', str(self.csv)]
        args += ['--out', str(out)]
        if generated:
            args += ['--generated-dir', str(generated)]
        result = subprocess.run(args+list(extra), capture_output=True, text=True, timeout=30)
        if code is not None:
            self.assertEqual(result.returncode, code, result.stdout+result.stderr)
        report = json.loads((out/'report.json').read_text()) if (out/'report.json').exists() else None
        if report:
            self.assertEqual(report['exit_code'], result.returncode)
            self.assertTrue((out/'report.md').is_file())
        return result, report, out

    def codes(self, report):
        return {item['code'] for item in report['findings']}

    def test_clean_raw_and_elf(self):
        for raw in (True, False):
            with self.subTest(raw=raw):
                self.configure([RETURN, NOP], [('ret', BASE, BASE+8)], raw=raw)
                _, report, _ = self.invoke(code=0)
                self.assertEqual(report['status'], 'CHECKED')
                self.assertEqual(report['findings'], [])
                self.assertEqual(report['selected_passes'], ['scan', 'reachability'])

    def test_omitted_non_nop_slot_names_exact_word_and_row(self):
        self.configure([branch(4, BASE, BASE+8), 0x2421ffff, RETURN, NOP],
                       [('short', BASE, BASE+4), ('next', BASE+4, BASE+16)])
        _, report, _ = self.invoke(code=1)
        self.assertIn('SLOT_SPLIT',self.codes(report))
        finding = next(f for f in report['findings'] if f['code']=='SLOT_SPLIT')
        self.assertEqual((finding['row'], finding['address'], finding['slot_address']), (2, BASE, BASE+4))
        self.assertEqual(finding['slot_word'], 0x2421ffff)
        self.assertFalse(finding['slot_nop'])

    def test_all_delayed_families_and_nop_slot_annotation(self):
        words = [op<<26 | 1<<21 for op in (4,5,20,21)]
        words += [op<<26 | 1<<21 for op in (6,7,22,23)]
        words += [1<<26 | 1<<21 | rt<<16 for rt in (0,1,2,3,16,17,18,19)]
        words += [op<<26 | 8<<21 | rt<<16 for op in (16,17,18) for rt in (0,1,2,3)]
        words += [2<<26 | ((BASE+8)>>2), 3<<26 | ((BASE+8)>>2), RETURN, 1<<21|31<<11|9, 1<<21|9]
        for w in words:
            with self.subTest(word=hex(w)):
                self.configure([w,NOP,RETURN,NOP], [('split',BASE,BASE+4),('following',BASE+4,BASE+16)])
                _, report, _ = self.invoke(code=1)
                self.assertIn('SLOT_SPLIT', self.codes(report))
                f=next(f for f in report['findings'] if f['code']=='SLOT_SPLIT')
                self.assertTrue(f['slot_nop'])

    def test_reserved_cop_form_is_not_positive_evidence(self):
        self.configure([0x45040000,NOP,RETURN,NOP], [('bad',BASE,BASE+16)])
        _, report, _ = self.invoke(code=2)
        self.assertIn('UNSUPPORTED_CONTROL', {f['code'] for f in report['errors']})

    def test_profile_interior_target_controls_warning(self):
        words = [branch(4,BASE,BASE+12,0,0),NOP,NOP,RETURN,NOP]
        self.configure(words,[('thunk',BASE,BASE+8),('body',BASE+8,BASE+20)])
        _, report, _ = self.invoke(code=1)
        self.assertIn('TARGET_INTERIOR',self.codes(report))
        self.profile.write_text(self.profile.read_text().replace('entry_points = []',
            'entry_points = ["tail@0x100c"] # 0x9999 is only a comment'))
        _, report, _ = self.invoke(code=0)
        self.assertEqual(report['summary']['profile_entries'],1)

    def test_top_level_legacy_and_name_only_selectors(self):
        self.profile.write_text('entry_points=["0x1000"]\nuntracked_stubs=["ret"]\n'+self.profile.read_text())
        _, report, _ = self.invoke(code=0)
        self.assertEqual(report['summary']['profile_entries'],1)

    def test_aliases_overlapping_rows_and_size_span_not_corruption(self):
        self.csv.write_text('Name,Start,End,Size\na,0x1000,0x1008,4\nb,0x1000,0x1008,12\n')
        _, report, _ = self.invoke(code=0)
        self.assertEqual(report['summary']['alias_groups'],1)

    def test_structural_ranges_conflicting_ends(self):
        for text, expected in [
            ('bad,0x1000,0x1000,0\n','ROW_RANGE'),
            ('bad,0x1001,0x1008,7\n','ROW_ALIGNMENT'),
            ('bad,0x1000,0x1010,16\n','ROW_UNMAPPED'),
            ('a,0x1000,0x1008,8\nb,0x1000,0x1004,4\n','ALIAS_CONFLICT'),
            ('bad,0x1000,0x1008,-1\n','ROW_SIZE')]:
            with self.subTest(expected=expected):
                self.csv.write_text('Name,Start,End,Size\n'+text)
                _, report, _ = self.invoke(code=1)
                self.assertIn(expected,self.codes(report))

    def test_separated_tail_two_way_evidence_and_cycle_termination(self):
        # F enters a hole beyond G; hole returns to F. No repaired interval is requested.
        self.configure([branch(4,BASE,BASE+24),NOP,RETURN,NOP,RETURN,NOP,
                        branch(4,BASE+24,BASE+8,0,0),NOP],
                       [('F',BASE,BASE+16),('G',BASE+16,BASE+24)])
        _, report, _ = self.invoke(code=1)
        f=next(f for f in report['findings'] if f['code']=='SEPARATED_BLOCK')
        self.assertEqual(f['entry'],BASE)
        self.assertEqual(f['target'],BASE+24)
        self.assertTrue(f['way_back'])
        self.assertEqual(f['return_path'],[BASE+24,BASE+8])
        self.assertIn(BASE+16,f['intervening_entries'])

    def test_chained_tail_has_recorded_path(self):
        self.configure([branch(4,BASE,BASE+24),NOP,RETURN,NOP,RETURN,NOP,
                        branch(4,BASE+24,BASE+32),NOP,branch(4,BASE+32,BASE+8,0,0),NOP],
                       [('F',BASE,BASE+16),('G',BASE+16,BASE+24)])
        _, report, _ = self.invoke(code=1)
        paths=[f.get('path',[]) for f in report['findings'] if f['code']=='SEPARATED_BLOCK']
        self.assertTrue(any(BASE+24 in p and BASE+32 in p for p in paths))

    def test_calls_and_unreachable_data_do_not_create_tails(self):
        for call in (3<<26|((BASE+24)>>2),branch(1,BASE,BASE+24,0,17),1<<21|31<<11|9):
            self.configure([call,NOP,RETURN,NOP,RETURN,NOP,branch(4,BASE+24,BASE+8,0,0),NOP],
                           [('F',BASE,BASE+16),('G',BASE+16,BASE+24)])
            _, report, _ = self.invoke(code=0)
            self.assertNotIn('SEPARATED_BLOCK',self.codes(report))

    def test_likely_graph_slot_edges_are_conditional(self):
        # Both return instructions have real NOP slots; a return in another return's slot is invalid.
        self.configure([branch(20,BASE,BASE+16),0x24210001,RETURN,NOP,RETURN,NOP],
                       [('F',BASE,BASE+24)])
        _, report, _ = self.invoke(code=0)
        self.assertTrue(any(e.get('slot_execution')=='taken_only' for e in report['reachability']['edges']))

    def test_module_namespaces_and_prefix(self):
        for name in ('module_a','module_b'):
            self.configure([RETURN,NOP],[('F',BASE+16,BASE+24)],prefix=16,name=name)
            _, report, _ = self.invoke(code=0)
            self.assertEqual(report['module']['id'],name)

    def test_excluded_vu_section_and_bss_are_not_ee_bytes(self):
        for section in ('.vutext','.DVP.overlay.fake'):
            self.binary.write_bytes(elf([RETURN,NOP],section_name=section))
            self.profile.write_text('[general]\ninput_format="elf"\n')
            _, report, _ = self.invoke(code=2)
            self.assertTrue(report['errors'])

    def test_manifest_equivalent_inputs_and_hash_refusal(self):
        manifest = {'schema_version':1,'modules':[{'id':'fixture','serial':'SYNTHETIC',
            'profile':{'path':self.profile.name,'sha256':digest(self.profile)},
            'binary':{'path':self.binary.name,'sha256':digest(self.binary)},
            'maps':{'cleaned':{'path':self.csv.name,'sha256':digest(self.csv)}}}]}
        path=self.root/'manifest.json';path.write_text(json.dumps(manifest))
        _, direct, _ = self.invoke(code=0)
        _, via, _ = self.invoke(code=0,manifest=path)
        self.assertEqual(direct['findings'],via['findings'])
        manifest['modules'][0]['binary']['sha256']='0'*64;path.write_text(json.dumps(manifest))
        _, report, _ = self.invoke(code=2,manifest=path)
        self.assertIn('HASH_MISMATCH',{e['code'] for e in report['errors']})

    def test_raw_cleaned_diff_is_information(self):
        baseline=self.root/'raw.csv'
        baseline.write_text('Name,Start,End,Size\na,0x1000,0x1004,4\ndropped,0x2000,0x2004,4\n')
        _, report, _ = self.invoke('--baseline-csv',str(baseline),code=0)
        self.assertEqual(report['comparison']['changed'][0]['entry'],BASE)
        self.assertEqual(report['comparison']['dropped'][0]['entry'],0x2000)

    def generated(self, *, emitted=False, registered=True, slot_nop=False, comment_only=False):
        self.configure([branch(4,BASE,BASE+8),NOP if slot_nop else 0x24210001,RETURN,NOP],
                       [('short',BASE,BASE+4),('next',BASE+4,BASE+16)])
        folder=self.root/'generated';folder.mkdir(exist_ok=True)
        op='SET_GPR_S32(ctx, 1, (int32_t)ADD32(GPR_U32(ctx, 1), 1));' if emitted else ''
        slot=('ctx->pc = 0x1004u; ctx->in_delay_slot = true; '+op+' ctx->in_delay_slot = false;') if emitted else ''
        if comment_only:slot='// 0x1004: addiu (Delay Slot)\n'
        body=('void short_0x1000(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) {\n'
              'ctx->pc = 0x1000u; { const bool branch_taken_0x1000 = true;\n'+slot+
              '\nif (branch_taken_0x1000) { ctx->pc = 0x1008u; return; } } ctx->pc = 0x1008u; }\n')
        (folder/'short.cpp').write_text(body)
        (folder/'register_functions.cpp').write_text('const RecompiledModuleEntry entries[] = {\n'
             '{0x1000u, short_0x1000},\n'+('{0x1008u, next_0x1008},\n' if registered else '')+'};\n')
        provenance={'schema_version':1,'generation':{'command':['synthetic-recompiler','fixture'],
            'exit_code':0,'recompiler_sha256':'a'*64,'source_revision':'synthetic'},
            'inputs':{k:digest(p) for k,p in [('profile',self.profile),('binary',self.binary),('csv',self.csv)]},
            'outputs':{p.name:digest(p) for p in folder.glob('*.cpp')}}
        prov=self.root/'generation.json';prov.write_text(json.dumps(provenance))
        return folder,prov

    def implicit_fixture(self, interior=True, continuation=False):
        self.configure([RETURN,NOP,0x24840004,RETURN,NOP], [('owner',BASE,BASE+12)],
                       entries=['0x1008'] if interior else [])
        folder=self.root/'implicit';folder.mkdir()
        label='case 0x1008u: goto label_1008;' if interior else ''
        (folder/'owner.cpp').write_text('void owner_0x1000(uint8_t* rdram,R5900Context* ctx,PS2Runtime* runtime) {\n'
            'switch(ctx->pc) {'+label+'}\nctx->pc=0x1000u;\nctx->pc=GPR_U32(ctx,31);return;\n'
            'label_1008: ctx->pc=0x1008u; SET_GPR_S32(ctx,4,(int32_t)ADD32(GPR_U32(ctx,4),4));\n'
            'ctx->pc=0x100cu;\n}\n')
        entries='{0x1000u,owner_0x1000},'+('{0x1008u,owner_0x1000},' if interior else '')
        if continuation:
            entries+='{0x100cu,tail_0x100c},'
            (folder/'tail.cpp').write_text('void tail_0x100c(uint8_t* rdram,R5900Context* ctx,PS2Runtime* runtime) {'
                'ctx->pc=0x100cu;ctx->pc=GPR_U32(ctx,31);return;}')
        (folder/'register_functions.cpp').write_text('RecompiledModuleEntry entries[]={'+entries+'};')
        prov=self.root/'implicit-provenance.json'
        prov.write_text(json.dumps(dict(schema_version=1,
            inputs={k:digest(v) for k,v in [('binary',self.binary),('csv',self.csv),('profile',self.profile)]},
            outputs={p.name:digest(p) for p in folder.iterdir()},
            generation=dict(exit_code=0,command=['independent fixture'],recompiler_sha256='a'*64,source_revision='synthetic'))))
        return folder,prov

    def test_generated_implicit_interior_exit_is_not_hidden_by_registration(self):
        folder,prov=self.implicit_fixture()
        _,r,_=self.invoke('--generation-provenance',str(prov),generated=folder)
        missing=[f for f in r['findings'] if f['code']=='GENERATED_FALLTHROUGH_UNREGISTERED']
        self.assertEqual(len(missing),1)
        self.assertEqual(missing[0]['target'],BASE+12)
        self.assertEqual(missing[0]['root'],BASE+8)

    def test_generated_dead_epilogue_does_not_invent_an_exit(self):
        folder,prov=self.implicit_fixture(interior=False)
        _,r,_=self.invoke('--generation-provenance',str(prov),generated=folder)
        self.assertNotIn('GENERATED_FALLTHROUGH_UNREGISTERED',self.codes(r))

    def test_generated_registered_implicit_continuation_is_reported(self):
        folder,prov=self.implicit_fixture(continuation=True)
        _,r,_=self.invoke('--generation-provenance',str(prov),generated=folder)
        self.assertNotIn('GENERATED_FALLTHROUGH_UNREGISTERED',self.codes(r))
        checked=r['audit'].get('implicit_exits',[])
        self.assertTrue(any(x['target']==BASE+12 and x['registered'] for x in checked))

    def test_speculative_resume_label_is_not_a_proved_implicit_exit(self):
        folder,prov=self.implicit_fixture()
        # Keep the actual generated resume registration, remove its declared
        # profile justification. The only real entry returns before this label.
        self.profile.write_text(self.profile.read_text().replace('["0x1008"]','[]'))
        provenance=json.loads(prov.read_text());provenance['inputs']['profile']=digest(self.profile)
        prov.write_text(json.dumps(provenance))
        _,r,_=self.invoke('--generation-provenance',str(prov),generated=folder)
        self.assertNotIn('GENERATED_FALLTHROUGH_UNREGISTERED',self.codes(r))
        scope=r['audit'].get('uncorroborated_resume_entries',[])
        self.assertTrue(any(BASE+8 in x['entries'] for x in scope))

    def test_generated_registration_does_not_mask_missing_operation(self):
        folder,prov=self.generated()
        _, report, _ = self.invoke('--generation-provenance',str(prov),generated=folder,code=1)
        self.assertIn('SLOT_EMISSION_MISSING',self.codes(report))
        self.assertNotIn('GENERATED_TARGET_UNREGISTERED',self.codes(report))

    def test_generated_comment_alone_is_not_an_operation(self):
        folder,prov=self.generated(comment_only=True)
        _, report, _ = self.invoke('--generation-provenance',str(prov),generated=folder,code=1)
        self.assertIn('SLOT_EMISSION_MISSING',self.codes(report))

    def test_generated_valid_slot_and_nop_elision(self):
        for kwargs in ({'emitted':True},{'slot_nop':True}):
            folder,prov=self.generated(**kwargs)
            _, report, _ = self.invoke('--generation-provenance',str(prov),generated=folder,code=1)
            self.assertNotIn('SLOT_EMISSION_MISSING',self.codes(report))
            self.assertEqual(report['audit']['correspondence'],'matching_supplied_provenance')

    def test_generated_unregistered_exit_and_stale_provenance(self):
        folder,prov=self.generated(registered=False)
        _, report, _ = self.invoke('--generation-provenance',str(prov),generated=folder,code=1)
        self.assertIn('GENERATED_TARGET_UNREGISTERED',self.codes(report))
        (folder/'short.cpp').write_text((folder/'short.cpp').read_text()+'// later replacement\n')
        _, report, _ = self.invoke('--generation-provenance',str(prov),generated=folder,code=2)
        self.assertIn('GENERATION_MISMATCH',{e['code'] for e in report['errors']})

    def test_requested_audit_without_provenance_is_incomplete(self):
        folder,_=self.generated()
        _, report, _ = self.invoke(generated=folder,code=2)
        self.assertEqual(report['audit']['correspondence'],'unverified')

    def test_bounds_and_malformed_inputs_return_two(self):
        for flag in ('--limit-binary-bytes','--limit-csv-bytes','--limit-root-visits','--limit-module-visits','--limit-report-bytes'):
            with self.subTest(flag=flag):
                result, report, _=self.invoke(flag,'1',code=2)
                if report:self.assertTrue(report['errors'])
        self.csv.write_text('wrong,header\n')
        _, report, _=self.invoke(code=2)
        self.assertIn('CSV_FORMAT',{e['code'] for e in report['errors']})

    def test_output_and_input_preservation_determinism(self):
        snapshots={p:p.read_bytes() for p in (self.profile,self.csv,self.binary)}
        _, first, out=self.invoke(code=0)
        _, second, other=self.invoke(code=0)
        self.assertEqual(first,second)
        self.assertEqual((out/'report.md').read_bytes(),(other/'report.md').read_bytes())
        for p,before in snapshots.items():self.assertEqual(p.read_bytes(),before)
        kept=(out/'report.json').read_bytes()
        result=subprocess.run([sys.executable,str(TOOL),'--profile',str(self.profile),
             '--binary',str(self.binary),'--csv',str(self.csv),'--out',str(out)],capture_output=True)
        self.assertEqual(result.returncode,2)
        self.assertEqual((out/'report.json').read_bytes(),kept)

    def test_signed_branch_jump_high_bits_and_address_wrap(self):
        # Independent encoded targets exercise signed16 and PC+4's high nibble.
        self.configure([RETURN,NOP,0x1000fffd,NOP],[('all',BASE,BASE+16)])
        _,r,_=self.invoke(code=0)
        self.assertNotIn('TARGET_UNMAPPED',self.codes(r))
        self.configure([0x08000004,NOP],[('jump',0x0ffffffc,0x10000004)],base=0x0ffffffc)
        _,r,_=self.invoke(code=0)
        self.assertTrue(any(e['target']==0x10000010 for e in r['reachability']['edges']))
        self.configure([0x10000000],[('last',0xfffffffc,0x100000000)],base=0xfffffffc)
        _,r,_=self.invoke(code=2)
        f=next(f for f in r['findings'] if f['code']=='SLOT_SPLIT')
        self.assertEqual(f['slot_address'],0)
        self.assertIsNone(f['slot_word'])

    def test_shared_tail_context_cycles_and_unreachable_linear_candidates(self):
        self.configure([branch(4,BASE,BASE+32),NOP,RETURN,NOP,
                        branch(4,BASE+16,BASE+32),NOP,RETURN,NOP,
                        branch(4,BASE+32,BASE+32,0,0),NOP],
                       [('F',BASE,BASE+16),('G',BASE+16,BASE+32)])
        _,r,_=self.invoke(code=1)
        tails=[f for f in r['findings'] if f['code']=='SEPARATED_BLOCK' and f['target']==BASE+32]
        self.assertEqual({f['entry'] for f in tails},{BASE,BASE+16})
        self.assertTrue(all(not f['way_back'] for f in tails))
        self.configure([RETURN,NOP,branch(4,BASE+8,BASE+28),NOP,RETURN,NOP,NOP,RETURN,NOP],
                       [('F',BASE,BASE+16),('G',BASE+16,BASE+36)])
        _,r,_=self.invoke(code=1)
        f=next(f for f in r['findings'] if f['code']=='TARGET_INTERIOR')
        self.assertEqual(f['path_evidence'],'linear_candidate_only')
        self.assertNotIn('SEPARATED_BLOCK',self.codes(r))

    def test_likely_exception_slot_only_ends_taken_path(self):
        self.configure([branch(20,BASE,BASE+8),0x0000000d,RETURN,NOP],[('F',BASE,BASE+16)])
        _,r,_=self.invoke(code=0)
        self.assertEqual(r['reachability']['stops']['exception_in_slot'],1)
        self.assertEqual(r['reachability']['stops']['return'],1)

    def test_profile_root_without_body_does_not_invent_tail_findings(self):
        self.configure([RETURN,NOP,NOP,branch(4,BASE+12,BASE+24),NOP,RETURN,RETURN,NOP],
                       [('F',BASE,BASE+8)],entries=['0x1008'])
        _,r,_=self.invoke(code=2) # Reached return-in-slot remains unsupported, not a green omission.
        self.assertNotIn('SEPARATED_BLOCK',self.codes(r))
        self.assertIn('PROFILE_BLOCKS_UNAVAILABLE',{u['code'] for u in r['unknowns']})

    def test_elf_truncation_bad_machine_missing_sections_and_bss(self):
        for mutation in ('truncated','machine','load_only','bss'):
            with self.subTest(mutation=mutation):
                blob=bytearray(elf([RETURN,NOP]))
                if mutation=='truncated':blob=blob[:64]
                if mutation=='machine':struct.pack_into('<H',blob,18,62)
                if mutation=='load_only':struct.pack_into('<H',blob,48,0)
                if mutation=='bss':
                    shoff=struct.unpack_from('<I',blob,32)[0]
                    struct.pack_into('<I',blob,shoff+44,8)
                self.binary.write_bytes(blob);self.profile.write_text('[general]\ninput_format="elf"\n')
                self.invoke(code=2)

    def test_malformed_manifest_shapes_and_mapping_identity(self):
        path=self.root/'manifest.json'
        for doc in ({'schema_version':1,'modules':[]},
                    {'schema_version':1,'modules':[{'id':'fixture','profile':'oops','binary':{},'maps':{'cleaned':{}}}]}):
            path.write_text(json.dumps(doc));result,r,_=self.invoke(manifest=path,code=2)
            self.assertNotIn('Traceback',result.stderr)
            self.assertTrue(r['errors'])

    def test_unresolved_ambiguous_selector_is_explicit(self):
        self.configure([RETURN,NOP,RETURN,NOP],[('same',BASE,BASE+8),('same',BASE+8,BASE+16)],entries=['same'])
        _,r,_=self.invoke(code=0)
        self.assertEqual(r['summary']['profile_entries'],0)
        self.assertIn('SELECTOR_UNRESOLVED',{u['code'] for u in r['unknowns']})

    def test_generated_unsupported_arithmetic_and_format_never_green(self):
        folder,prov=self.generated(emitted=True)
        body=(folder/'short.cpp').read_text().replace('ADD32(GPR_U32(ctx, 1), 1)','ADD32(GPR_U32(ctx, 1), 2)')
        (folder/'short.cpp').write_text(body)
        data=json.loads(prov.read_text());data['outputs']['short.cpp']=digest(folder/'short.cpp');prov.write_text(json.dumps(data))
        _,r,_=self.invoke('--generation-provenance',str(prov),generated=folder,code=2)
        self.assertIn('SLOT_EMISSION_UNSUPPORTED',{e['code'] for e in r['errors']})
        (folder/'register_functions.cpp').write_text('void unknown_table();')
        data['outputs']['register_functions.cpp']=digest(folder/'register_functions.cpp');prov.write_text(json.dumps(data))
        _,r,_=self.invoke('--generation-provenance',str(prov),generated=folder,code=2)
        self.assertIn('GENERATED_FORMAT',{e['code'] for e in r['errors']})

    def test_generated_indexed_table_uses_values_not_comments(self):
        folder,prov=self.generated()
        (folder/'register_functions.cpp').write_text('''extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x1000u;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[3u] = {};
g_ps2RecompiledFunctionTable[0] = short_0x1000; // 0x9999
g_ps2RecompiledFunctionTable[2] = next_0x1008; // 0x8888
''')
        data=json.loads(prov.read_text());data['outputs']['register_functions.cpp']=digest(folder/'register_functions.cpp');prov.write_text(json.dumps(data))
        _,r,_=self.invoke('--generation-provenance',str(prov),generated=folder,code=1)
        self.assertEqual(r['audit']['registration_count'],2)
        self.assertNotIn('GENERATED_TARGET_UNREGISTERED',self.codes(r))

    def test_generated_limits_and_output_failure(self):
        folder,prov=self.generated()
        for option in ('--limit-generated-bytes','--limit-generated-files'):
            self.invoke(option,'1','--generation-provenance',str(prov),generated=folder,code=2)
        # A parent file cannot become an output directory; no JSON completion marker can exist.
        parent=self.root/'parent';parent.write_bytes(b'preserve')
        result,r,out=self.invoke('--out',str(parent/'report'),code=2)
        self.assertEqual(parent.read_bytes(),b'preserve');self.assertIsNone(r)

    def test_generated_profile_resume_requires_matching_executable_label(self):
        folder,prov=self.generated()
        self.profile.write_text(self.profile.read_text().replace('entry_points = []','entry_points = ["0x1008"]'))
        table=folder/'register_functions.cpp';table.write_text(table.read_text().replace('next_0x1008','short_0x1000'))
        wrapper=folder/'short.cpp'
        body=wrapper.read_text().replace('ctx->pc = 0x1000u;',
            'switch(ctx->pc) { case 0x1008u: goto label_1008; default: break; } ctx->pc = 0x1000u;',1)
        body=body.replace('} ctx->pc = 0x1008u;','} label_1008: ctx->pc = 0x1008u;')
        wrapper.write_text(body)
        data=json.loads(prov.read_text());data['inputs']['profile']=digest(self.profile)
        data['outputs']={p.name:digest(p) for p in folder.glob('*.cpp')};prov.write_text(json.dumps(data))
        _,r,_=self.invoke('--generation-provenance',str(prov),generated=folder,code=1)
        self.assertTrue(r['audit']['configured_entries'][0]['resume_label'])
        wrapper.write_text(body.replace('label_1008:','label_100c:'))
        data['outputs']['short.cpp']=digest(wrapper);prov.write_text(json.dumps(data))
        _,r,_=self.invoke('--generation-provenance',str(prov),generated=folder,code=2)
        self.assertIn('GENERATED_RESUME',{e['code'] for e in r['errors']})

    def test_explicit_main_context_does_not_use_another_overlay(self):
        folder,prov=self.generated()
        wrapper=folder/'short.cpp'
        wrapper.write_text(wrapper.read_text().replace('const bool',
            'runtime->dispatchGuestBranch(rdram,ctx,0x2008u,0x1000u,0x1008u,PS2Runtime::GuestBranchKind::DirectCall,"JAL"); const bool'))
        data=json.loads(prov.read_text());data['outputs']['short.cpp']=digest(wrapper);prov.write_text(json.dumps(data))
        context=self.root/'main';context.mkdir()
        table=context/'register_functions.cpp'
        table.write_text('extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x2000u; g_ps2RecompiledFunctionTable[2] = main_0x2000;')
        (context/'main_0x2000.cpp').write_text('void main_0x2000() { switch(ctx->pc) {case 0x2008u: goto label_2008;} label_2008: return;}')
        _,r,_=self.invoke('--generation-provenance',str(prov),'--main-registration',str(table),
            '--main-registration-sha256',digest(table),generated=folder,code=1)
        e=r['audit']['external_targets'][0]
        self.assertEqual(e['evidence'],'supplied_main_registration')
        self.assertEqual(e['resume_evidence']['resume_label'],'label_2008')
        table.write_text('const RecompiledModuleEntry entries[] = {{0x2008u, other_0x2008}};')
        self.invoke('--generation-provenance',str(prov),'--main-registration',str(table),
            '--main-registration-sha256',digest(table),generated=folder,code=2)

    def test_emitter_entry_is_initial_pc_not_symbol_suffix_and_unsigned_slot(self):
        folder,prov=self.generated(emitted=True)
        self.binary.write_bytes(struct.pack('<IIII',branch(4,BASE,BASE+8),0x2421ffff,RETURN,NOP))
        wrapper=folder/'short.cpp'
        wrapper.write_text(wrapper.read_text().replace('short_0x1000','short_0x1010')
            .replace('ctx->in_delay_slot = true;','ctx->in_delay_slot = true; ctx->branch_pc = 0x1000u;')
            .replace('ADD32(GPR_U32(ctx, 1), 1)','ADD32(GPR_U32(ctx, 1), 4294967295)'))
        table=folder/'register_functions.cpp';table.write_text(table.read_text().replace('short_0x1000','short_0x1010'))
        data=json.loads(prov.read_text());data['inputs']['binary']=digest(self.binary)
        data['outputs']={p.name:digest(p) for p in folder.glob('*.cpp')};prov.write_text(json.dumps(data))
        _,r,_=self.invoke('--generation-provenance',str(prov),generated=folder,code=1)
        self.assertEqual(r['audit']['slot_checks'][0]['result'],'operation_present')

    def test_hash_is_the_snapshot_decoded_even_after_external_replacement(self):
        before=digest(self.binary);self.counter+=1;out=self.root/f'snapshot-{self.counter}'
        harness='''import importlib.util,sys
from pathlib import Path
spec=importlib.util.spec_from_file_location('subject',sys.argv[1]);m=importlib.util.module_from_spec(spec);sys.modules['subject']=m;spec.loader.exec_module(m)
original=m.snapshot
def snapshot(path,limit):
    blob=original(path,limit)
    if path.name=='input.bin':path.write_bytes(b'external replacement')
    return blob
m.snapshot=snapshot
sys.exit(m.main(sys.argv[2:]))
'''
        result=subprocess.run([sys.executable,'-B','-c',harness,str(TOOL),'--profile',str(self.profile),
            '--binary',str(self.binary),'--csv',str(self.csv),'--out',str(out)],capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stderr)
        r=json.loads((out/'report.json').read_text())
        self.assertEqual(r['inputs']['binary']['sha256'],before)
        self.assertEqual(self.binary.read_bytes(),b'external replacement')

    def test_failed_json_publication_has_no_completion_marker(self):
        out=self.root/'failed-publish'
        harness='''import importlib.util,sys
spec=importlib.util.spec_from_file_location('subject',sys.argv[1]);m=importlib.util.module_from_spec(spec);sys.modules['subject']=m;spec.loader.exec_module(m)
def fail(*a,**k):raise OSError('synthetic publication failure')
m.os.link=fail
sys.exit(m.main(sys.argv[2:]))
'''
        result=subprocess.run([sys.executable,'-B','-c',harness,str(TOOL),'--profile',str(self.profile),
            '--binary',str(self.binary),'--csv',str(self.csv),'--out',str(out)],capture_output=True,text=True)
        self.assertEqual(result.returncode,2,result.stderr)
        self.assertTrue((out/'report.md').is_file())
        self.assertFalse((out/'report.json').exists())

    def test_generated_native_stub_is_explicit_not_a_missing_guest_body(self):
        self.profile.write_text(self.profile.read_text().replace('entry_points = []','entry_points = ["0x1000"]'))
        folder=self.root/'native';folder.mkdir()
        (folder/'native.cpp').write_text('void native_0x1010(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) { ctx->pc = getRegU32(ctx,31); ps2_stubs::fixture(rdram,ctx,runtime); }')
        (folder/'register_functions.cpp').write_text('const RecompiledModuleEntry entries[] = {{0x1000u,native_0x1010}};')
        prov=self.root/'native-provenance.json'
        prov.write_text(json.dumps(dict(schema_version=1,generation=dict(command=['fixture'],exit_code=0,
            recompiler_sha256='a'*64,source_revision='synthetic'),
            inputs={k:digest(p) for k,p in [('profile',self.profile),('binary',self.binary),('csv',self.csv)]},
            outputs={p.name:digest(p) for p in folder.glob('*.cpp')})))
        _,r,_=self.invoke('--generation-provenance',str(prov),generated=folder,code=0)
        self.assertEqual(r['audit']['native_stub_wrappers'],1)
        self.assertEqual(r['audit']['configured_entries'][0]['native_stub'],'ps2_stubs::fixture')
        (folder/'native.cpp').write_text((folder/'native.cpp').read_text().replace('ps2_stubs::','ps2_syscalls::'))
        data=json.loads(prov.read_text());data['outputs']['native.cpp']=digest(folder/'native.cpp');prov.write_text(json.dumps(data))
        _,r,_=self.invoke('--generation-provenance',str(prov),generated=folder,code=0)
        self.assertEqual(r['audit']['configured_entries'][0]['native_stub'],'ps2_syscalls::fixture')

    def test_existing_output_link_and_input_alias_refused(self):
        for path in (self.binary,self.root):
            result,_,_=self.invoke('--out',str(path),code=2)
            self.assertNotIn('Traceback',result.stderr)
        link=self.root/'linked-output'
        try:link.symlink_to(self.binary)
        except OSError as exc:self.skipTest('symlink creation unavailable: '+str(exc.errno))
        self.invoke('--out',str(link),code=2)


if __name__=='__main__':
    unittest.main()
