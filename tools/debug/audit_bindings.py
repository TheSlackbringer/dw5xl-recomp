#!/usr/bin/env python3
"""Offline, read-only selected binding evidence checks. Python 3.11+, stdlib only."""
from __future__ import annotations

import argparse
import collections
import hashlib
import json
import math
import os
from pathlib import Path
import re
import stat
import sys
import tomllib

from validate_function_map import Image, Invalid as ImageInvalid

VERSION = 'binding-audit-1'
KINDS = {'argument', 'return', 'memory_output', 'state_effect', 'callback', 'ownership',
         'clobber', 'stack', 'gp', 'continuation', 'mmio'}
DEFAULTS = {'manifest_bytes': 2 << 20, 'artifact_bytes': 64 << 20, 'total_bytes': 128 << 20,
            'records': 4096, 'report_bytes': 4 << 20}
IDENT = re.compile(r'[A-Za-z0-9_.-]{1,80}\Z')
SHA = re.compile(r'[0-9a-f]{64}\Z')
GPRS = {'zero','at','v0','v1','a0','a1','a2','a3','t0','t1','t2','t3','t4','t5','t6','t7',
        's0','s1','s2','s3','s4','s5','s6','s7','t8','t9','k0','k1','gp','sp','fp','s8','ra',
        *('r'+str(i) for i in range(32))}
GPR_NAMES = 'zero at v0 v1 a0 a1 a2 a3 t0 t1 t2 t3 t4 t5 t6 t7 s0 s1 s2 s3 s4 s5 s6 s7 t8 t9 k0 k1 gp sp fp ra'.split()
GPR_NUMBER = {name:i for i,name in enumerate(GPR_NAMES)}
GPR_NUMBER.update({'r'+str(i):i for i in range(32)})
GPR_NUMBER['s8'] = 30


class Invalid(Exception):
    def __init__(self, code, message):
        self.code, self.message = code, message
        super().__init__(message)


def require(test, code='SCHEMA', message='Invalid schema value'):
    if not test:
        raise Invalid(code, message)


def obj(value, required, optional=()):
    require(type(value) is dict, message='Object required')
    require(set(required) <= value.keys() and value.keys() <= set(required) | set(optional),
            message='Missing or unsupported object keys')
    return value


def string(value, limit=2048):
    require(type(value) is str and 0 < len(value) <= limit and '\x00' not in value,
            message='Bounded nonempty string required')
    return value


def ident(value):
    require(type(value) is str and IDENT.fullmatch(value), message='Invalid logical ID')
    return value


def member(value, collection):
    return type(value) is str and value in collection


def address(value):
    require(type(value) is str and re.fullmatch(r'0x[0-9a-fA-F]{1,8}', value), message='Hexadecimal 32-bit address required')
    return int(value, 16)


def number(value, low=0, high=0xffffffff):
    require(type(value) is int and low <= value <= high, message='Integer outside supported range')
    return value


def sequence(value, *, nonempty=False):
    require(type(value) is list and (not nonempty or value), message='Array required')
    return value


def refs(value, known):
    sequence(value, nonempty=True)
    require(all(type(x) is str and x in known for x in value) and len(set(value)) == len(value),
            message='Unique existing citation references required')


def location(value):
    string(value, 64)
    if value.startswith('gpr:'):
        require(value[4:] in GPRS, message='Unknown guest register')
    elif value.startswith('fpr:'):
        require(re.fullmatch(r'f(?:[0-9]|[12][0-9]|3[01])', value[4:]), message='Unknown float register')
    else:
        require(re.fullmatch(r'stack:[+-]0x[0-9a-fA-F]{1,8}', value), message='Unsupported location')


def locations(value, nonempty=True):
    sequence(value, nonempty=nonempty)
    require(len(value) <= 128, message='Too many locations')
    for item in value:
        location(item)
    require(len({canonical_location(v) for v in value}) == len(value), message='Repeated physical locations')


def canonical_location(value):
    if value.startswith('gpr:'):
        return 'gpr:r'+str(GPR_NUMBER[value[4:]])
    if value.startswith('stack:'):
        return 'stack:'+str(int(value[6:],16))
    return value


def canonical_value(kind,value):
    if kind in ('argument','return','clobber'):
        result={**value,'locations':[canonical_location(v) for v in value['locations']]}
        if kind=='clobber':result['locations'].sort()
        return result
    if kind in ('gp','continuation'):
        return {**value,'location':canonical_location(value['location'])}
    if kind=='callback':
        return {**value,'arguments':[canonical_value('argument',v) for v in value['arguments']],
                'returns':canonical_value('return',value['returns']) if value['returns'] is not None else None}
    return value


def slot(value):
    obj(value, ['locations','bits','representation'])
    locations(value['locations'])
    require(type(value['bits']) is int and value['bits'] in (8,16,32,64,128), message='Unsupported width')
    require(value['representation'] in ('signed','unsigned','pointer','float','software_double','raw'),
            message='Unsupported representation')
    if value['representation'] == 'float':
        require(value['bits'] in (32,64), message='Float width must be 32 or 64')
    if value['representation'] == 'software_double':
        require(value['bits'] == 64, message='Software-double width must be 64')


def typed_value(kind, value):
    if kind not in KINDS:
        return  # Unknown kinds remain visible; they are never compared as supported facts.
    if kind in ('argument','return'):
        slot(value)
    elif kind in ('memory_output','mmio'):
        obj(value, ['target','access','bits'])
        string(value['target'],256)
        require(value['access'] in ('read','write','read_write','none'), message='Unsupported memory access')
        require(type(value['bits']) is int and value['bits'] in (8,16,32,64,128), message='Unsupported width')
    elif kind == 'state_effect':
        obj(value, ['target','effect'])
        string(value['target'],256);string(value['effect'])
    elif kind == 'callback':
        obj(value, ['arguments','returns','context'])
        sequence(value['arguments']);require(len(value['arguments']) <= 32, message='Callback argument limit')
        for item in value['arguments']:
            slot(item)
        if value['returns'] is not None:
            slot(value['returns'])
        string(value['context'],256)
    elif kind == 'ownership':
        obj(value, ['resource','operations'])
        string(value['resource'],256)
        require(type(value['operations']) is dict and 0 < len(value['operations']) <= 64, message='Operation map required')
        for key, item in value['operations'].items():
            string(key,128);string(item,256)
    elif kind == 'clobber':
        obj(value, ['locations']);locations(value['locations'],False)
    elif kind == 'stack':
        obj(value, ['alignment','delta'])
        n = number(value['alignment'],1,65536)
        require(n & (n-1) == 0, message='Stack alignment must be a power of two')
        number(value['delta'],-(1<<31),(1<<31)-1)
    else:
        obj(value, ['location','policy']);location(value['location']);string(value['policy'],256)


def snapshot(path, limit):
    try:
        # Opening nonblocking and checking the opened handle excludes FIFOs/devices.
        # The same handle supplies size and bytes; no stat/open replacement window.
        fd = os.open(path, os.O_RDONLY | getattr(os,'O_NONBLOCK',0) | getattr(os,'O_BINARY',0))
        with os.fdopen(fd,'rb') as stream:
            info = os.fstat(stream.fileno())
            require(stat.S_ISREG(info.st_mode),'INPUT_READ','Regular snapshot file required')
            require(info.st_size <= limit,'LIMIT_INPUT','Input byte limit exceeded')
            chunks, remaining = [], limit + 1
            while remaining:
                part = stream.read(min(65536, remaining))
                if not part:
                    break
                chunks.append(part);remaining -= len(part)
            blob = b''.join(chunks)
    except OSError:
        raise Invalid('INPUT_READ', 'Input snapshot unavailable') from None
    require(len(blob) <= limit, 'LIMIT_INPUT', 'Input byte limit exceeded')
    return blob


def digest(blob):
    return hashlib.sha256(blob).hexdigest()


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'JSON_DUPLICATE', 'Duplicate JSON key')
        result[key] = value
    return result


def parse_manifest(blob):
    def constant(_):
        raise Invalid('JSON_FORMAT', 'Non-finite JSON number')
    try:
        data = json.loads(blob.decode('utf-8-sig'), object_pairs_hook=unique_object, parse_constant=constant)
    except (ValueError,UnicodeError,RecursionError):
        raise Invalid('JSON_FORMAT', 'Bounded UTF-8 JSON object required') from None
    pending, nodes = [(data,0)], 0
    while pending:
        item, depth = pending.pop();nodes += 1
        require(depth <= 24 and nodes <= 100000, 'LIMIT_JSON', 'JSON depth/node limit exceeded')
        require(type(item) is not float or math.isfinite(item),'JSON_FORMAT','Finite JSON numbers required')
        if type(item) is dict:
            pending.extend((v,depth+1) for v in item.values())
        elif type(item) is list:
            pending.extend((v,depth+1) for v in item)
    return data


class Audit:
    def __init__(self, data, path, blob, limits):
        self.data, self.path, self.manifest_hash, self.limits = data, path, digest(blob), limits
        self.issues, self.snapshots, self.artifact_reports, self.images, self.citations = [], {}, {}, {}, {}
        self.record_count = 0
        self.validate()

    def count(self, amount=1):
        self.record_count += amount
        require(self.record_count <= self.limits['records'], 'LIMIT_RECORDS', 'Record limit exceeded')

    def index(self, name):
        result = {}
        for item in sequence(self.data[name]):
            require(type(item) is dict and 'id' in item, message='Identified object required')
            key = ident(item['id'])
            require(key not in result, message='Duplicate logical ID')
            result[key] = item;self.count()
        return result

    def fact_schema(self, item, side, kind=None, observation=False):
        if item is None:
            return
        if type(item) is list and not observation:
            sequence(item,nonempty=True);self.count(len(item))
            for alternative in item:
                require(type(alternative) is dict, message='Fact alternatives must be objects')
                self.fact_schema(alternative,side,kind)
            return
        obj(item, ['value','evidence','scope','rationale'])
        refs(item['evidence'], self.citation_defs)
        require(item['scope'] == 'reviewed_' + side, message='Only side-specific reviewed evidence scopes supported')
        string(item['rationale'])
        if observation:
            string(item['value'])
        else:
            typed_value(kind, item['value'])

    def validate(self):
        obj(self.data, ['schema_version','audit_id','source_revision','artifacts','modules','citations',
                        'conditions','aliases','registrations','bindings'])
        require(type(self.data['schema_version']) is int and self.data['schema_version'] == 1,
                message='Unsupported schema version')
        ident(self.data['audit_id']);string(self.data['source_revision'],128)
        self.artifacts = self.index('artifacts');self.modules = self.index('modules')
        self.citation_defs = self.index('citations');self.conditions = self.index('conditions')
        self.registrations = self.index('registrations');self.bindings = self.index('bindings')
        require(self.bindings and self.modules and self.artifacts, message='Selected bindings/modules/artifacts required')
        for item in self.artifacts.values():
            obj(item,['id','path','sha256','kind'],['module'])
            string(item['path'],4096)
            require(type(item['sha256']) is str and SHA.fullmatch(item['sha256']), message='Expected SHA-256 required')
            require(item['kind'] in ('binary','text','profile','ghidra'), message='Unsupported artifact kind')
            if item['kind'] == 'ghidra':
                require(member(item.get('module'),self.modules), message='Ghidra module identity required')
            else:
                require('module' not in item, message='Module key reserved for Ghidra artifacts')
        for item in self.modules.values():
            obj(item,['id','serial','artifact','format','load_generation'],['base_address','code_offset'])
            string(item['serial'],128);string(item['load_generation'],128)
            require(member(item['artifact'],self.artifacts) and self.artifacts[item['artifact']]['kind']=='binary',
                    message='Module binary artifact required')
            require(item['format'] in ('elf','raw'),message='Unsupported module format')
            if item['format']=='raw':
                require('base_address' in item, message='Raw base address required');address(item['base_address'])
                number(item.get('code_offset',0))
            else:
                require('base_address' not in item and 'code_offset' not in item, message='ELF uses its own load mappings')
        for item in self.citation_defs.values():
            if 'instructions' in item:
                obj(item,['id','module','instructions'])
                require(member(item['module'],self.modules),message='Unknown instruction module')
                for instruction in sequence(item['instructions'],nonempty=True):
                    self.count();obj(instruction,['address','hex']);address(instruction['address'])
                    require(type(instruction['hex']) is str and re.fullmatch(r'[0-9a-fA-F]{8}',instruction['hex']),
                            message='Exactly one instruction word required')
            else:
                obj(item,['id','artifact','lines'],['contains'])
                require(member(item['artifact'],self.artifacts) and self.artifacts[item['artifact']]['kind']!='binary',
                        message='Text artifact required')
                sequence(item['lines']);require(len(item['lines'])==2,message='Inclusive line range required')
                a,b=item['lines'];number(a,1);number(b,a)
                if 'contains' in item:string(item['contains'])
        for item in self.conditions.values():
            obj(item,['id','state','evidence','rationale'],['value'])
            require(item['state'] in ('established','unknown','incompatible'),message='Unsupported condition state')
            refs(item['evidence'],self.citation_defs);string(item['rationale'])
            require(('value' in item and type(item['value']) is bool) if item['state']=='established'
                    else 'value' not in item,message='Established conditions alone have Boolean values')
        self.aliases=collections.defaultdict(list)
        for item in sequence(self.data['aliases']):
            self.count();obj(item,['name','handler','evidence']);string(item['name'],128);string(item['handler'],128)
            refs(item['evidence'],self.citation_defs);self.aliases[item['name']].append(item)
        for item in self.registrations.values():
            obj(item,['id','module','address','name','condition','evidence'],['profile'])
            require(member(item['module'],self.modules) and member(item['condition'],self.conditions),message='Registration module/condition missing')
            address(item['address']);string(item['name'],128);refs(item['evidence'],self.citation_defs)
            if 'profile' in item:
                p=obj(item['profile'],['artifact','entry']);string(p['entry'],256)
                require(member(p['artifact'],self.artifacts) and self.artifacts[p['artifact']]['kind']=='profile',message='TOML profile artifact required')
        for item in self.bindings.values():
            obj(item,['id','module','address','name','handler','selection_scope','question','fields','observations','limitations'])
            require(member(item['module'],self.modules),message='Binding module required');address(item['address'])
            string(item['name'],128);string(item['handler'],128);string(item['question'])
            require(item['selection_scope'] in ('declared','reviewed_effective'),message='Explicit selection scope required')
            for limit in sequence(item['limitations']):string(limit)
            ids=set()
            for field in sequence(item['fields'],nonempty=True):
                self.count();obj(field,['id','kind','required','condition','original','replacement'])
                key=ident(field['id']);require(key not in ids,message='Conflicting field IDs');ids.add(key)
                ident(field['kind']);require(type(field['required']) is bool,message='Required flag must be Boolean')
                require(member(field['condition'],self.conditions),message='Unknown field condition')
                for side in ('original','replacement'):self.fact_schema(field[side],side,field['kind'])
            ids=set()
            for observation in sequence(item['observations']):
                self.count();obj(observation,['id','required','original','replacement'])
                key=ident(observation['id']);require(key not in ids,message='Duplicate observation ID');ids.add(key)
                require(type(observation['required']) is bool,message='Required flag must be Boolean')
                for side in ('original','replacement'):self.fact_schema(observation[side],side,observation=True)

    def issue(self, code, subject, message):
        self.issues.append(dict(code=code,subject=subject,message=message))

    def load(self):
        total=0
        for key,item in self.artifacts.items():
            report=dict(id=key,kind=item['kind'],expected_sha256=item['sha256'],status='invalid')
            self.artifact_reports[key]=report
            try:
                blob=snapshot(self.path.parent/item['path'],min(self.limits['artifact_bytes'],self.limits['total_bytes']-total))
            except Invalid as exc:
                if exc.code.startswith('LIMIT_'):raise
                self.issue('MISSING_ARTIFACT',key,'Snapshot unavailable');continue
            total+=len(blob);require(total<=self.limits['total_bytes'],'LIMIT_INPUT','Total snapshot limit exceeded')
            report.update(sha256=digest(blob),bytes=len(blob))
            if report['sha256']!=item['sha256']:
                self.issue('STALE_ARTIFACT',key,'Expected snapshot hash differs');continue
            if item['kind']!='binary':
                try:
                    blob.decode('utf-8-sig')
                except UnicodeError:
                    self.issue('TEXT_ENCODING',key,'UTF-8 evidence required');continue
            self.snapshots[key]=blob;report['status']='verified'
        for key,module in self.modules.items():
            blob=self.snapshots.get(module['artifact'])
            if blob is None:continue
            general={'input_format':module['format']}
            if module['format']=='raw':
                general.update(raw_base=address(module['base_address']),raw_code_offset=module.get('code_offset',0),module_id=key)
            try:
                self.images[key]=Image(blob,general)
            except ImageInvalid:
                self.issue('MODULE_MAPPING',key,'Unsupported or invalid file-backed module mapping')
        for key,item in self.artifacts.items():
            if item['kind']=='ghidra' and key in self.snapshots:self.ghidra(key,item)
        for key,item in self.citation_defs.items():
            reasons=[]
            if 'instructions' in item:
                image=self.images.get(item['module'])
                if image is None:reasons.append('MODULE_UNAVAILABLE')
                else:
                    for ins in item['instructions']:
                        pc=address(ins['address']);offset=image.offset(pc)
                        if pc%4 or offset is None or not image.is_code(pc,pc+4) or image.blob[offset:offset+4]!=bytes.fromhex(ins['hex']):
                            reasons.append('ORIGINAL_BYTES');break
            else:
                blob=self.snapshots.get(item['artifact'])
                if blob is None or self.artifact_reports[item['artifact']]['status']!='verified':reasons.append('UNVERIFIED_ARTIFACT')
                else:
                    lines=blob.decode('utf-8-sig').splitlines();a,b=item['lines']
                    if b>len(lines):reasons.append('CITATION_RANGE')
                    elif 'contains' in item and item['contains'] not in '\n'.join(lines[a-1:b]):reasons.append('CITATION_TEXT')
            self.citations[key]=dict(**item,status='verified' if not reasons else 'invalid',reasons=reasons)
            for reason in reasons:self.issue(reason,key,'Citation does not match the current snapshot')

    def ghidra(self,key,item):
        module=self.modules[item['module']];image=self.images.get(item['module'])
        lines=self.snapshots[key].decode('utf-8-sig').splitlines();report=self.artifact_reports[key]
        header=lines[0].split('\t') if lines else []
        expected=self.artifact_reports[module['artifact']].get('sha256')
        if len(header)!=4 or header[0]!='PROGRAM' or header[2]!=expected or header[3]!='r5900:LE:32:default' or image is None:
            report['status']='invalid';self.issue('GHIDRA_IDENTITY',key,'Export program/module identity differs');return
        checked=0;limits=[]
        for line in lines[1:]:
            if line.startswith('INS\t'):
                self.count()
                parts=line.split('\t',3)
                if len(parts)!=4 or not re.fullmatch('[0-9a-fA-F]{1,8}',parts[1]) or not re.fullmatch('[0-9a-fA-F]{8}',parts[2]):
                    report['status']='invalid';self.issue('GHIDRA_FORMAT',key,'Malformed instruction record');continue
                pc=int(parts[1],16);offset=image.offset(pc)
                if pc%4 or offset is None or not image.is_code(pc,pc+4) or image.blob[offset:offset+4]!=bytes.fromhex(parts[2]):
                    report['status']='invalid';self.issue('ORIGINAL_BYTES',key,'Export instruction differs from the original snapshot')
                else:checked+=1
            elif line.startswith('INCOMPLETE\t') or (line.startswith('DECOMP\t') and line.endswith('\tfalse')):
                limits.append(line[:512])
        report['instructions_checked']=checked;report['coverage_limits']=limits
        if limits:self.issue('EXPORT_INCOMPLETE',key,'Export records an incomplete scope; see coverage_limits')
        if not checked:self.issue('GHIDRA_EMPTY',key,'No corresponding original instructions checked')

    def evidence_ok(self, values):
        return all(self.citations[key]['status']=='verified' for key in values)

    def condition(self,key):
        item=self.conditions[key]
        if not self.evidence_ok(item['evidence']):return 'unknown'
        if item['state']!='established':return item['state']
        return 'true' if item['value'] else 'false'

    def registration_ok(self,reg):
        reasons=[]
        if not self.evidence_ok(reg['evidence']):reasons.append('UNVERIFIED_SELECTION_EVIDENCE')
        if 'profile' in reg:
            p=reg['profile'];blob=self.snapshots.get(p['artifact'])
            if blob is None:reasons.append('PROFILE_UNAVAILABLE')
            else:
                try:
                    profile=tomllib.loads(blob.decode('utf-8-sig'))
                    tokens=profile.get('general',{}).get('stubs',[])
                    match=re.fullmatch(r'([^@]+)@(0x[0-9a-fA-F]{1,8})',p['entry'])
                    if not isinstance(tokens,list) or p['entry'] not in tokens or not match or match[1]!=reg['name'] or int(match[2],16)!=address(reg['address']):
                        reasons.append('PROFILE_SELECTION')
                except (ValueError,TypeError,AttributeError,UnicodeError):reasons.append('PROFILE_FORMAT')
        return reasons

    def selection(self,binding):
        active=[];uncertain=[];reasons=[]
        image=self.images.get(binding['module']);pc=address(binding['address'])
        if pc%4 or image is None or not image.is_code(pc,pc+4):reasons.append('BINDING_MAPPING')
        for reg in self.registrations.values():
            if reg['module']!=binding['module'] or address(reg['address'])!=pc:continue
            state=self.condition(reg['condition'])
            if state=='false':continue
            errors=self.registration_ok(reg)
            if state!='true' or errors:
                uncertain.append(reg['id']);reasons.extend(errors or ['UNKNOWN_SELECTION_CONDITION'])
            else:active.append(reg)
        result=dict(status='unknown',scope=binding['selection_scope'],active=[x['id'] for x in active],
                    uncertain=uncertain,reasons=reasons,handler=None)
        if len(active)>1:reasons.append('AMBIGUOUS_SELECTION')
        if uncertain:reasons.append('UNCERTAIN_SELECTION')
        if not active:reasons.append('MISSING_SELECTION')
        if reasons:return result
        selected=active[0];aliases=self.aliases.get(selected['name'],[])
        if aliases:
            if not all(self.evidence_ok(a['evidence']) for a in aliases):reasons.append('UNVERIFIED_ALIAS')
            targets={a['handler'] for a in aliases}
            if len(targets)!=1:reasons.append('AMBIGUOUS_ALIAS')
            if reasons:return result
            handler=next(iter(targets))
        elif selected['name']==binding['handler']:
            handler=selected['name']
        else:
            reasons.append('MISSING_ALIAS');return result
        result['handler']=handler
        if selected['name']!=binding['name']:
            reasons.append('BINDING_NAME');result['status']='contradiction'
        elif handler!=binding['handler']:
            reasons.append('HANDLER_MISMATCH');result['status']='contradiction'
        else:result['status']='compatible'
        return result

    def reviewed(self,item,kind):
        if item is None:return None,['MISSING_FACT']
        items=item if type(item) is list else [item]
        if not all(self.evidence_ok(f['evidence']) for f in items):return None,['UNVERIFIED_FACT_EVIDENCE']
        values=[]
        for fact in items:
            value=canonical_value(kind,fact['value'])
            values.append(value)
        if any(value!=values[0] for value in values[1:]):return None,['CONFLICTING_ANNOTATIONS']
        return values[0],[]

    def compare(self,field):
        reasons=[];kind=field['kind']
        if kind not in KINDS:reasons.append('UNSUPPORTED_FACT')
        state=self.condition(field['condition'])
        if state!='true':reasons.append('CONDITION_'+state.upper())
        original,errors=self.reviewed(field['original'],kind);reasons.extend(errors)
        replacement,errors=self.reviewed(field['replacement'],kind);reasons.extend(errors)
        status='unknown' if reasons else ('compatible' if original==replacement else 'contradiction')
        return dict(**field,status=status,reasons=reasons)

    def observe(self,item):
        _,a=self.reviewed(item['original'],None);_,b=self.reviewed(item['replacement'],None)
        valid=not a and not b
        comparison=None
        if valid:
            comparison='same_description' if item['original']['value']==item['replacement']['value'] else 'different_descriptions'
        return dict(**item,status='verified' if valid else 'unverified',comparison=comparison,reasons=a+b)

    def run(self):
        self.load();results=[];counts=collections.Counter(compatible=0,contradiction=0,unknown=0)
        incomplete=bool(self.issues);contradiction=False
        for binding in self.bindings.values():
            selection=self.selection(binding);counts[selection['status']]+=1
            incomplete |= selection['status']=='unknown';contradiction |= selection['status']=='contradiction'
            fields=[self.compare(f) for f in binding['fields']]
            observations=[self.observe(o) for o in binding['observations']]
            for field in fields:
                counts[field['status']]+=1
                incomplete |= field['required'] and field['status']=='unknown'
                contradiction |= field['status']=='contradiction'
            incomplete |= any(o['required'] and o['status']!='verified' for o in observations)
            results.append({**{k:v for k,v in binding.items() if k not in ('fields','observations')},
                            'selection':selection,'fields':fields,'observations':observations})
        code=2 if incomplete else 1 if contradiction else 0
        report=dict(schema_version=1,tool_version=VERSION,audit_id=self.data['audit_id'],
                    source_revision=self.data['source_revision'],manifest_sha256=self.manifest_hash,
                    status={0:'complete',1:'findings',2:'incomplete'}[code],exit_code=code,
                    scope='Reviewed contract facts and snapshot correspondence only; no execution or full semantic/ABI proof.',
                    counts=dict(counts),issues=self.issues,artifacts=list(self.artifact_reports.values()),
                    modules=list(self.modules.values()),citations=list(self.citations.values()),
                    conditions=list(self.conditions.values()),bindings=results)
        return report


def markdown(report):
    def safe(value):
        return str(value).replace('&','&amp;').replace('<','&lt;').replace('>','&gt;').replace('|','\\|').replace('`','\\`').replace('\n',' ')
    lines=['# Selected binding evidence', '', f"Status: **{report['status']}**; exit {report['exit_code']}.",
           '',safe(report['scope']),'',f"Manifest SHA-256: `{report['manifest_sha256']}`",'',
           'Review: '+safe(report['audit_id'])+'; source label: '+safe(report['source_revision']),
           'Counts: '+safe(json.dumps(report['counts'],sort_keys=True)),'', '## Evidence issues','']
    lines.extend('- '+safe(i['code'])+' / '+safe(i['subject'])+': '+safe(i['message']) for i in report['issues'])
    if not report['issues']:lines.append('None.')
    for binding in report['bindings']:
        selection=binding['selection']
        lines.extend(['','## '+safe(binding['id']),'',safe(binding['question']),'',
                      'Selection: '+safe(selection['status'])+'; scope '+safe(selection['scope'])+'; handler '+safe(selection['handler'])+'.',
                      'Reasons: '+safe(', '.join(selection['reasons'])),'',
                      '| Field | Kind / required | Status / conditions | Reasons |','|---|---|---|---|'])
        for field in binding['fields']:
            lines.append('| '+safe(field['id'])+' | '+safe(field['kind'])+' / '+str(field['required'])+' | '+safe(field['status'])+' / '+safe(field['condition'])+' | '+safe(', '.join(field['reasons']))+' |')
        for field in binding['fields']:
            lines.extend(['','### Field '+safe(field['id']),''])
            for side in ('original','replacement'):
                lines.append('- '+side+': '+safe(json.dumps(field[side],sort_keys=True)))
        lines.extend(['','### Reviewed structural observations',''])
        for observation in binding['observations']:
            lines.append('- '+safe(observation['id'])+': '+safe(observation['status'])+' / '+safe(observation['comparison']))
            for side in ('original','replacement'):
                lines.append('  - '+side+': '+safe(json.dumps(observation[side],sort_keys=True)))
        lines.extend(['','### Limits',''])
        lines.extend('- '+safe(v) for v in binding['limitations'])
    lines.extend(['','## Modules and load contexts',''])
    lines.extend('- '+safe(json.dumps(module,sort_keys=True)) for module in report['modules'])
    lines.extend(['','## Evidence inventory',''])
    for artifact in report['artifacts']:lines.append('- '+safe(json.dumps(artifact,sort_keys=True)))
    lines.extend(['','## Citations and conditions',''])
    lines.extend('- '+safe(json.dumps(c,sort_keys=True)) for c in report['citations'])
    lines.extend('- '+safe(json.dumps(c,sort_keys=True)) for c in report['conditions'])
    return '\n'.join(lines)+'\n'


def write_reports(out, files):
    # Refuse existing destinations; never replace an input, existing directory, or link.
    require(not os.path.lexists(out),'OUTPUT_EXISTS','Output path already exists')
    created=[];owned=False
    try:
        out.mkdir();owned=True
        for name,blob in files.items():
            path=out/name
            with path.open('xb') as stream:
                created.append(path);stream.write(blob)
    except OSError:
        for path in created:
            try:path.unlink()
            except OSError:pass
        if owned:
            try:out.rmdir()
            except OSError:pass
        raise Invalid('OUTPUT_WRITE','Unable to create new report files') from None


def allowance(value):
    try:result=int(value)
    except ValueError:raise argparse.ArgumentTypeError('Positive bounded integer required') from None
    if not 1<=result<=1<<30:raise argparse.ArgumentTypeError('Limit must be between 1 and 1073741824')
    return result


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest',required=True,type=Path)
    parser.add_argument('--out',required=True,type=Path)
    for name,value in DEFAULTS.items():
        parser.add_argument('--max-'+name.replace('_','-'),type=allowance,default=value)
    args=parser.parse_args(argv)
    limits={name:getattr(args,'max_'+name) for name in DEFAULTS}
    try:
        require(not os.path.lexists(args.out),'OUTPUT_EXISTS','Output path already exists')
        blob=snapshot(args.manifest,limits['manifest_bytes'])
        audit=Audit(parse_manifest(blob),args.manifest,blob,limits)
        report=audit.run()
        files={'report.json':(json.dumps(report,indent=2,sort_keys=True,ensure_ascii=True)+'\n').encode(),
               'report.md':markdown(report).encode('utf-8')}
        require(all(len(blob)<=limits['report_bytes'] for blob in files.values()),'LIMIT_REPORT','Report byte limit exceeded')
        write_reports(args.out,files)
        print(json.dumps({'status':report['status'],'exit_code':report['exit_code'],'counts':report['counts']},sort_keys=True))
        return report['exit_code']
    except Invalid as exc:
        print(json.dumps({'status':'invalid','exit_code':2,'code':exc.code,'message':exc.message[:512]},sort_keys=True))
        return 2
    except (RecursionError,UnicodeError,OverflowError):
        print(json.dumps({'status':'invalid','exit_code':2,'code':'INPUT_FORMAT','message':'Unsupported bounded input representation'}))
        return 2


if __name__=='__main__':
    sys.exit(main())
