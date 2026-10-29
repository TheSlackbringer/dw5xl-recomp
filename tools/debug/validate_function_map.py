#!/usr/bin/env python3
"""Read-only EE function-map and generated-output diagnostics. Python 3.11+, stdlib only."""
from __future__ import annotations

import argparse
import bisect
import collections
import csv
from dataclasses import dataclass
import hashlib
import io
import json
import os
from pathlib import Path
import re
import struct
import sys
import tomllib

VERSION = '031-v3-4'
DEFAULT_LIMITS = dict(binary_bytes=64<<20, csv_bytes=16<<20, config_bytes=1<<20,
    rows=100_000, root_visits=65_536, module_visits=2_000_000, tail_bytes=4096,
    findings=10_000, report_bytes=32<<20, generated_bytes=512<<20, generated_files=100_000)


class Invalid(Exception):
    def __init__(self, code, message):
        self.code, self.message = code, message
        super().__init__(message)


def sha(blob):
    return hashlib.sha256(blob).hexdigest()


def snapshot(path, limit):
    try:
        with path.open('rb') as stream:
            # The allowance can be hundreds of MiB even for a tiny generated file.
            # Keep one handle and the same one-byte oversize check without asking
            # the host to allocate an allowance-sized read buffer for every file.
            chunks = []
            remaining = limit + 1
            while remaining:
                chunk = stream.read(min(64 << 10, remaining))
                if not chunk:
                    break
                chunks.append(chunk)
                remaining -= len(chunk)
            data = b''.join(chunks)
    except OSError as exc:
        raise Invalid('INPUT_READ', f'{path.name}: {exc.strerror}') from exc
    if len(data)>limit:
        raise Invalid('LIMIT_INPUT', f'{path.name}: input byte limit exceeded')
    return data


def text_snapshot(path, limit):
    data=snapshot(path,limit)
    try:
        return data,data.decode('utf-8-sig')
    except UnicodeError as exc:
        raise Invalid('INPUT_ENCODING',f'{path.name}: UTF-8 required') from exc


def config_json(path, limit):
    data,text=text_snapshot(path,limit)
    try:
        value=json.loads(text)
    except (ValueError,RecursionError) as exc:
        raise Invalid('JSON_FORMAT',f'{path.name}: invalid JSON') from exc
    if not isinstance(value,dict):
        raise Invalid('JSON_FORMAT',f'{path.name}: object required')
    return data,value


@dataclass(frozen=True)
class Row:
    name: str
    start: int
    end: int
    size: int
    line: int


def parse_rows(data,limit):
    try:
        text=data.decode('utf-8-sig')
        reader=csv.reader(io.StringIO(text,newline=''),strict=True)
        if next(reader,None)!=['Name','Start','End','Size']:
            raise ValueError('header')
        rows=[]
        for line,fields in enumerate(reader,2):
            if not fields:continue
            if len(fields)!=4 or not fields[0]:raise ValueError('row')
            name,start,end,size=fields
            if not re.fullmatch(r'0[xX][0-9a-fA-F]+',start.strip()) or not re.fullmatch(r'0[xX][0-9a-fA-F]+',end.strip()):
                raise ValueError('address')
            if not re.fullmatch(r'-?[0-9]+',size.strip()):raise ValueError('size')
            rows.append(Row(name,int(start,16),int(end,16),int(size),line))
            if len(rows)>limit:raise Invalid('LIMIT_ROWS','CSV row limit exceeded')
    except (UnicodeError,ValueError,csv.Error) as exc:
        raise Invalid('CSV_FORMAT','Name,Start,End,Size with hexadecimal addresses and decimal Size required') from exc
    if not rows:raise Invalid('CSV_FORMAT','CSV has no rows')
    return rows


class Image:
    def __init__(self,blob,general):
        self.blob=blob
        self.mappings=[]
        self.code=[]
        self.format=general.get('input_format','elf')
        if self.format=='raw':
            base=general.get('raw_base');prefix=general.get('raw_code_offset',0)
            if type(base)is not int or type(prefix)is not int or base<0 or prefix<0 or base%4 or prefix%4 or len(blob)%4 or prefix>=len(blob) or base+len(blob)>1<<32:
                raise Invalid('RAW_MAPPING','Invalid aligned raw base/prefix/length')
            if not isinstance(general.get('module_id'),str) or not general['module_id']:
                raise Invalid('MODULE_ID','Raw profile requires module_id')
            self.mappings=[(base,base+len(blob),0)]
            self.code=[(base+prefix,base+len(blob))]
        elif self.format=='elf':
            self.parse_elf()
        else:raise Invalid('INPUT_FORMAT','Only ELF32 little-endian MIPS and raw are supported')
        self.mappings.sort()
        for a,b in zip(self.mappings,self.mappings[1:]):
            if a[1]>b[0]:raise Invalid('ELF_MAPPING','Overlapping load mappings')
        if not self.code:raise Invalid('ELF_MAPPING','No supported file-backed EE executable mapping')

    def add_code_copies(self,copies):
        if not isinstance(copies,list):raise Invalid('CODE_COPY','code_copies must be an array')
        names=set();rows=[]
        for copy in copies:
            if not isinstance(copy,dict):raise Invalid('CODE_COPY','Each code copy must be a table')
            name=copy.get('name');source=copy.get('source');dest=copy.get('destination');size=copy.get('size')
            if not isinstance(name,str) or not re.fullmatch('[A-Za-z][A-Za-z0-9_]*',name) or name in names:
                raise Invalid('CODE_COPY','Unique code copy identifier required')
            names.add(name)
            if any(type(v)is not int or v<0 or v>0xffffffff or v%4 for v in (source,dest,size)) or not size or source+size>1<<32 or dest+size>1<<32:
                raise Invalid('CODE_COPY','Invalid aligned code copy range')
            physical=dest&0x1fffffff
            if physical+size>0x2000000 or not (dest<0x2000000 or 0x80000000<=dest<0xc0000000):
                raise Invalid('CODE_COPY','Code copy destination is outside direct-mapped EE RAM')
            if any(physical<(hi&0x1fffffff) and physical+size>(lo&0x1fffffff) for lo,hi,_ in self.mappings):
                raise Invalid('CODE_COPY','Code copy destination overlaps a mapped image')
            off=self.offset(source)
            if off is None or self.offset(source+size-4)!=off+size-4:
                raise Invalid('CODE_COPY','Code copy source must be contiguous file-backed bytes')
            ranges=copy.get('functions')
            if not isinstance(ranges,list) or not ranges:raise Invalid('CODE_COPY','Explicit function ranges required')
            checked=[]
            for pair in ranges:
                if not isinstance(pair,list) or len(pair)!=2 or any(type(v)is not int for v in pair):
                    raise Invalid('CODE_COPY','Function ranges require offset/length integer pairs')
                start,length=pair
                if start<0 or length<=0 or (start|length)%4 or start+length>size:
                    raise Invalid('CODE_COPY','Function range outside copied image')
                checked.append((start,length))
            checked.sort()
            if any(a+n>b for (a,n),(b,_) in zip(checked,checked[1:])):raise Invalid('CODE_COPY','Overlapping copied functions')
            self.mappings.append((dest,dest+size,off))
            for start,length in checked:
                self.code.append((dest+start,dest+start+length))
                rows.append(Row('copy_'+name+'_'+str(start),dest+start,dest+start+length,length,0))
        self.mappings.sort();self.code.sort()
        return rows

    def bounds(self,offset,size):
        if offset<0 or size<0 or offset+size>len(self.blob):
            raise Invalid('ELF_FORMAT','Truncated/out-of-file ELF table or payload')

    def parse_elf(self):
        b=self.blob
        self.bounds(0,52)
        if b[:7]!=b'\x7fELF\x01\x01\x01':raise Invalid('ELF_FORMAT','ELF32 little-endian version 1 required')
        fields=struct.unpack_from('<HHIIIIIHHHHHH',b,16)
        _,machine,version,_,phoff,shoff,_,ehsize,phsize,phnum,shsize,shnum,shstr=fields
        if machine!=8 or version!=1 or ehsize<52 or phsize<32 or not phnum:
            raise Invalid('ELF_FORMAT','Invalid MIPS ELF header/program table')
        self.bounds(phoff,phsize*phnum)
        for i in range(phnum):
            typ,offset,addr,_,filesz,memsz,flags,_=struct.unpack_from('<8I',b,phoff+i*phsize)
            if typ!=1:continue
            self.bounds(offset,filesz)
            if filesz>memsz or addr+memsz>1<<32:raise Invalid('ELF_MAPPING','Invalid ELF load extent')
            if filesz:
                self.mappings.append((addr,addr+filesz,offset))
        if shnum:
            if shsize<40 or shstr>=shnum:raise Invalid('ELF_FORMAT','Invalid section table')
            self.bounds(shoff,shsize*shnum)
            sections=[struct.unpack_from('<10I',b,shoff+i*shsize) for i in range(shnum)]
            names=sections[shstr]
            if names[1]!=3:raise Invalid('ELF_FORMAT','Section-name table must be a string table')
            self.bounds(names[4],names[5]);strings=b[names[4]:names[4]+names[5]]
            for name,typ,flags,addr,offset,size,*_ in sections:
                if typ!=8:self.bounds(offset,size)
                if name>=len(strings):raise Invalid('ELF_FORMAT','Invalid section name offset')
                name_end=strings.find(b'\0',name)
                if name_end<0:raise Invalid('ELF_FORMAT','Unterminated section name')
                label=strings[name:name_end].decode('ascii',errors='replace')
                if typ==8 or not flags&4 or not size:continue
                if label in ('.vutext','.DVP.overlay') or label.startswith(('.vutext.','.DVP.overlay.')):continue
                if addr+size>1<<32 or not any(lo<=addr and addr+size<=hi and off+addr-lo==offset for lo,hi,off in self.mappings):
                    raise Invalid('ELF_MAPPING','Executable section does not match file-backed mapping')
                self.code.append((addr,addr+size))
        else:
            raise Invalid('ELF_MAPPING','Executable section classification is required; load flags alone cannot exclude VU/data')

    def offset(self,address):
        for lo,hi,offset in self.mappings:
            if lo<=address and address+4<=hi:return offset+address-lo
        return None

    def word(self,address):
        off=self.offset(address)
        return struct.unpack_from('<I',self.blob,off)[0] if off is not None else None

    def is_code(self,start,end=None):
        end=start+4 if end is None else end
        return any(lo<=start and end<=hi for lo,hi in self.code)


@dataclass(frozen=True)
class Control:
    kind: str='ordinary'
    target: int|None=None
    delayed: bool=False
    likely: bool=False
    unconditional: bool=False


def decode(pc,w):
    op,rs,rt,rd,sa,fn=w>>26,(w>>21)&31,(w>>16)&31,(w>>11)&31,(w>>6)&31,w&63
    target=(pc+4+struct.unpack('<h',struct.pack('<H',w&0xffff))[0]*4)&0xffffffff
    if op in (4,5,6,7,20,21,22,23):
        if op in (6,7,22,23) and rt:return Control('unsupported')
        return Control('branch',target,True,op>=20,op in (4,20) and rs==rt)
    if op==1 and rt in (0,1,2,3,16,17,18,19):
        return Control('call' if rt>=16 else 'branch',target,True,rt in (2,3,18,19),False)
    if op in (16,17,18) and rs==8:
        return Control('unsupported') if rt>3 else Control('branch',target,True,rt in (2,3))
    if op in (2,3):return Control('jump' if op==2 else 'call',(((pc+4)&0xf0000000)|((w&0x3ffffff)<<2))&0xffffffff,True,False,op==2)
    if op==0 and fn in (8,9):
        if rt or sa or (fn==8 and rd):return Control('unsupported')
        return Control('return' if fn==8 and rs==31 else 'indirect' if fn==8 or rd==0 else 'call',None,True)
    if (op==0 and fn in (12,13)) or w==0x42000018:return Control('exception')
    return Control()


def nop(word):
    return word in (0,0x24000000)


def cpp_code(text):
    """Blank comments/literals, preserving offsets and newlines for evidence locations."""
    token=re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')
    return token.sub(lambda m:re.sub(r'[^\n]',' ',m[0]),text)


def braced(code,start):
    if start>=len(code) or code[start]!='{':raise Invalid('GENERATED_FORMAT','Expected C++ brace')
    depth=0
    for i in range(start,len(code)):
        if code[i]=='{':depth+=1
        elif code[i]=='}':
            depth-=1
            if depth==0:return i+1
    raise Invalid('GENERATED_FORMAT','Unbalanced supported generated wrapper/table')


def registrations(code):
    """Only the two current emitter table layouts; comments never supply addresses."""
    records={};recognized=False
    newlines=[m.start() for m in re.finditer('\n',code)]
    def line_at(offset):return bisect.bisect_left(newlines,offset)+1
    base=re.search(r'\bg_ps2RecompiledFunctionTableBase\s*=\s*(0x[\da-fA-F]+)[uU]?\s*;',code)
    if base:
        recognized=True;base=int(base[1],16)
        if base%4:raise Invalid('GENERATED_FORMAT','Unaligned main table base')
        assignments=list(re.finditer(r'\bg_ps2RecompiledFunctionTable\s*\[[^\]]*\]\s*=\s*([^;]*);',code))
        understood=0
        for match in re.finditer(r'\bg_ps2RecompiledFunctionTable\s*\[\s*(\d+)[uU]?\s*\]\s*=\s*(\w+)\s*;',code):
            understood+=1
            addr=base+int(match[1])*4
            if addr>0xffffffff:raise Invalid('GENERATED_FORMAT','Registration overflow')
            if not re.fullmatch(r'[A-Za-z_]\w*',match[2]) or match[2] in ('nullptr','NULL'):
                raise Invalid('GENERATED_FORMAT','A registration requires a function symbol')
            if addr in records:raise Invalid('GENERATED_FORMAT','Duplicate registration')
            records[addr]=dict(symbol=match[2],line=line_at(match.start()))
        if len([m for m in assignments if m[1].strip()!='{}'])!=understood:
            raise Invalid('GENERATED_FORMAT','Unsupported indexed registration assignment')
    for match in re.finditer(r'\bRecompiledModuleEntry\s+\w+\s*\[\s*\]\s*=\s*\{',code):
        recognized=True;end=braced(code,match.end()-1)
        body=code[match.end():end-1]
        pattern=r'\{\s*(0x[\da-fA-F]+)[uU]?\s*,\s*(\w+)\s*\}\s*,?'
        if re.sub(pattern,'',body).strip():raise Invalid('GENERATED_FORMAT','Unsupported module entry layout')
        for entry in re.finditer(pattern,body):
            addr=int(entry[1],16)
            if addr>0xffffffff or addr%4 or addr in records:raise Invalid('GENERATED_FORMAT','Invalid/duplicate module registration')
            if not re.fullmatch(r'[A-Za-z_]\w*',entry[2]) or entry[2] in ('nullptr','NULL'):
                raise Invalid('GENERATED_FORMAT','A registration requires a function symbol')
            records[addr]=dict(symbol=entry[2],line=line_at(match.end()+entry.start()))
    return recognized,records


def resume_labels(code):
    labels={}
    for match in re.finditer(r'case\s+(0x[\da-fA-F]+)[uU]?\s*:\s*goto\s+((?:label_|L_0x)([\da-fA-F]+))\s*;',code):
        address=int(match[1],16)
        if address!=int(match[3],16) or not re.search(r'\b'+re.escape(match[2])+r'\s*:',code):
            raise Invalid('GENERATED_RESUME','Resume case lacks matching address label')
        labels[address]=dict(label=match[2],line=code.count('\n',0,match.start())+1)
    return labels


class Analysis:
    def __init__(self,args):
        self.args=args
        self.limits={k:getattr(args,'limit_'+k) for k in DEFAULT_LIMITS}
        self.report=dict(schema_version=1,tool=VERSION,status='INCOMPLETE',exit_code=2,
            selected_passes=[],module={},inputs={},limits=self.limits,findings=[],errors=[],
            unknowns=[],summary={},reachability={'edges':[]},audit=None,comparison=None)
        self.rows=[];self.valid=[];self.entries=set();self.cache={};self.slots=[]
        self.finding_keys=set();self.unknown_keys=set();self.error_keys=set()

    def error(self,code,message,**facts):
        key=(code,message,json.dumps(facts,sort_keys=True))
        if key in self.error_keys:return
        if len(self.error_keys)>=self.limits['findings'] and code!='LIMIT_ERRORS':
            raise Invalid('LIMIT_ERRORS','Error record limit exceeded')
        self.error_keys.add(key)
        self.report['errors'].append(dict(code=code,message=message,**facts))

    def finding(self,code,row,pc,message,**facts):
        key=(code,row.line,pc,facts.get('target'))
        if key in self.finding_keys:return
        if len(self.finding_keys)>=self.limits['findings']:
            raise Invalid('LIMIT_FINDINGS','Finding limit exceeded')
        self.finding_keys.add(key)
        self.report['findings'].append(dict(code=code,row=row.line,entry=row.start,address=pc,
            file_offset=self.image.offset(pc),message=message,**facts))

    def unknown(self,code,pc,**facts):
        key=(code,pc,json.dumps(facts,sort_keys=True))
        if key in self.unknown_keys:return
        if len(self.unknown_keys)>=self.limits['findings']:
            raise Invalid('LIMIT_COVERAGE','Coverage record limit exceeded')
        self.unknown_keys.add(key);self.report['unknowns'].append(dict(code=code,address=pc,**facts))

    def get_control(self,pc):
        if pc not in self.cache:
            if len(self.cache)>=self.limits['module_visits']:
                raise Invalid('LIMIT_DECODE','Decoded instruction fact limit exceeded')
            word=self.image.word(pc)
            self.cache[pc]=decode(pc,word) if word is not None else Control('unmapped')
        return self.cache[pc]

    def load(self):
        a=self.args;record=None
        if a.manifest:
            _,manifest=config_json(a.manifest,self.limits['config_bytes'])
            if manifest.get('schema_version')!=1 or not isinstance(manifest.get('modules'),list):
                raise Invalid('MANIFEST_FORMAT','Manifest schema_version 1 and modules list required')
            matches=[m for m in manifest['modules'] if isinstance(m,dict) and m.get('id')==a.module]
            if len(matches)!=1:raise Invalid('MODULE_ID','Select one unique manifest module')
            record=matches[0];base=a.manifest.parent
            try:
                refs={'profile':record['profile'],'binary':record['binary'],'csv':record['maps'][a.map]}
                if any(not isinstance(v,dict) or not isinstance(v.get('path'),str) for v in refs.values()):
                    raise Invalid('MANIFEST_FORMAT','Each input reference needs a path and expected hash')
                paths={k:base/v['path'] for k,v in refs.items()}
            except (KeyError,TypeError) as exc:raise Invalid('MANIFEST_FORMAT','Requested paths/map missing') from exc
            self.report['input_mode']='manifest'
            for field in ('generated_dir','generation_provenance','baseline_csv'):
                if record.get(field):
                    if not isinstance(record[field],str):raise Invalid('MANIFEST_FORMAT',f'{field} requires a path string')
                    if not getattr(a,field):setattr(a,field,base/record[field])
            if record.get('baseline_map') and not a.baseline_csv:
                try:
                    baseline=record['maps'][record['baseline_map']]
                    a.baseline_csv=base/baseline['path']
                    self.baseline_expected=baseline['sha256']
                except (KeyError,TypeError) as exc:raise Invalid('MANIFEST_FORMAT','Invalid baseline map reference') from exc
        else:
            paths=dict(profile=a.profile,binary=a.binary,csv=a.csv);self.report['input_mode']='direct'
        self.paths=paths
        blobs={k:snapshot(p,self.limits['binary_bytes' if k=='binary' else 'csv_bytes' if k=='csv' else 'config_bytes']) for k,p in paths.items()}
        self.hashes={k:sha(v) for k,v in blobs.items()}
        if record:
            for k,ref in refs.items():
                if not isinstance(ref.get('sha256'),str) or ref['sha256']!=self.hashes[k]:
                    raise Invalid('HASH_MISMATCH',f'{k}: expected hash missing/mismatch')
        self.report['inputs']={k:dict(label=p.name,sha256=self.hashes[k]) for k,p in paths.items()}
        try:self.profile=tomllib.loads(blobs['profile'].decode('utf-8-sig'))
        except (ValueError,UnicodeError) as exc:raise Invalid('PROFILE_FORMAT','Invalid TOML profile') from exc
        general=self.profile.get('general',{})
        if not isinstance(general,dict):raise Invalid('PROFILE_FORMAT','general table required')
        self.rows=parse_rows(blobs['csv'],self.limits['rows'])
        self.image=Image(blobs['binary'],general)
        copied_rows=self.image.add_code_copies(self.profile.get('code_copies',[]))
        self.rows.extend(copied_rows)
        self.entries.update(row.start for row in copied_rows)
        module_id=general.get('module_id','main')
        if record:
            if general.get('module_id') and module_id!=record['id']:raise Invalid('MODULE_ID','Profile/manifest module mismatch')
            for field,actual in [('format',self.image.format),('raw_base',general.get('raw_base')),('raw_code_offset',general.get('raw_code_offset',0))]:
                if field in record and record[field]!=actual:raise Invalid('PROFILE_MAPPING','Manifest/profile mapping disagreement')
            module_id=record['id']
        self.report['module']=dict(id=module_id,format=self.image.format,serial=record.get('serial') if record else None,
            mapping=[dict(start=lo,end=hi,file_offset=off) for lo,hi,off in self.image.mappings],
            code_regions=[list(region) for region in self.image.code])
        if not isinstance(module_id,str) or (record and record.get('serial') is not None and not isinstance(record['serial'],str)):
            raise Invalid('MODULE_ID','Module and serial labels must be strings')
        for table in (general,self.profile):
            for key in ('entry_points','untracked_stubs'):
                selectors=table.get(key,[])
                if not isinstance(selectors,list) or not all(isinstance(v,str) for v in selectors):
                    raise Invalid('PROFILE_FORMAT',f'{key} must be a string array')
                for selector in selectors:
                    literal=selector.rsplit('@',1)[-1].strip()
                    if re.fullmatch(r'(?:0[xX][0-9a-fA-F]+|[0-9]+)',literal):
                        value=int(literal,16 if literal.lower().startswith('0x') else 10)
                        if value<0 or value>0xffffffff or value%4:raise Invalid('PROFILE_ENTRY','Invalid aligned 32-bit entry')
                        self.entries.add(value)
                    else:
                        matches={r.start for r in self.rows if r.name==selector.strip()}
                        if len(matches)==1:self.entries.update(matches)
                        else:self.unknown('SELECTOR_UNRESOLVED',None,selector=selector)
        self.report['selected_passes']=['scan','reachability']+(['audit'] if a.generated_dir else [])
        if a.passes:
            passes=a.passes.split(',')
            if not set(passes)<=set(('scan','reachability','audit')) or 'scan' not in passes:
                raise Invalid('PASSES','Select scan with optional reachability,audit')
            if 'audit' in passes and not a.generated_dir:raise Invalid('PASSES','Audit requires generated-dir')
            self.report['selected_passes']=[p for p in ('scan','reachability','audit') if p in passes]
        self.report['summary']=dict(rows=len(self.rows),profile_entries=len(self.entries),alias_groups=0,
            decoded_words=0,slot_candidates=0)

    def contains(self,target):
        i=bisect.bisect_right(self.starts,target)-1;found=[]
        while i>=0 and self.prefix_ends[i]>target:
            row=self.ordered[i]
            if row.start<=target<row.end:found.append(row)
            i-=1
        return found

    def classify(self,row,target):
        if row.start<=target<row.end:return 'in_row'
        if target in self.row_starts:return 'map_entry'
        if target in self.entries:return 'profile_entry'
        if self.contains(target):return 'other_row_interior'
        return 'unmapped_in_module' if self.image.offset(target)is not None else 'outside_module'

    def scan(self):
        aliases=collections.defaultdict(list)
        for row in self.rows:
            aliases[row.start].append(row)
            if row.start<0 or row.end>1<<32 or row.end<=row.start:
                self.finding('ROW_RANGE',row,row.start,'Invalid 32-bit exclusive range');continue
            if row.start%4 or row.end%4:
                self.finding('ROW_ALIGNMENT',row,row.start,'Instruction range is not four-byte aligned');continue
            if not self.image.is_code(row.start,row.end):
                self.finding('ROW_UNMAPPED',row,row.start,'Range is not within file-backed EE mapping');continue
            if row.size<=0:self.finding('ROW_SIZE',row,row.start,'Positive body byte count required')
            if row.size!=row.end-row.start:self.unknown('BODY_BLOCKS_UNAVAILABLE',row.start,row=row.line)
            self.valid.append(row)
        self.report['summary']['alias_groups']=sum(len(v)>1 for v in aliases.values())
        for start,group in aliases.items():
            if len({r.end for r in group})>1:
                self.finding('ALIAS_CONFLICT',group[0],start,'Same entry has conflicting ends')
        self.ordered=sorted(self.valid,key=lambda r:(r.start,r.end,r.line))
        self.starts=[r.start for r in self.ordered];self.prefix_ends=[];end=0
        for r in self.ordered:end=max(end,r.end);self.prefix_ends.append(end)
        self.row_starts=set(self.starts)
        targets=collections.Counter();sites=collections.defaultdict(set)
        for row in self.valid:
            for pc in range(row.start,row.end,4):
                c=self.get_control(pc)
                if c.kind=='unsupported':self.error('UNSUPPORTED_CONTROL','Reserved/unsupported control encoding',address=pc)
                if c.delayed and pc+4>=row.end:
                    slot=self.image.word((pc+4)&0xffffffff)
                    self.slots.append((row,pc,c,slot))
                    self.finding('SLOT_SPLIT',row,pc,'Delay slot is outside selected row',slot_address=(pc+4)&0xffffffff,
                                 slot_word=slot,slot_nop=nop(slot),slot_rows=[r.line for r in self.contains((pc+4)&0xffffffff)])
                if c.target is not None:
                    category=('call:' if c.kind=='call' else 'transfer:')+self.classify(row,c.target)
                    targets[category]+=1;sites[category].add(pc)
                if c.target is None or c.kind=='call':continue
                classification=self.classify(row,c.target)
                if classification in ('other_row_interior','unmapped_in_module'):
                    self.finding('TARGET_INTERIOR' if classification=='other_row_interior' else 'TARGET_UNMAPPED',
                        row,pc,'Unexplained target; registration/ownership requires review',target=c.target,classification=classification)
                elif classification=='outside_module':self.unknown('EXTERNAL_MODULE',pc,target=c.target)
        self.report['summary']['slot_candidates']=len(self.slots)
        self.report['summary']['direct_targets']={k:dict(occurrences=n,unique_sites=len(sites[k])) for k,n in sorted(targets.items())}
        if self.args.baseline_csv:
            data=snapshot(self.args.baseline_csv,self.limits['csv_bytes'])
            if hasattr(self,'baseline_expected') and sha(data)!=self.baseline_expected:
                raise Invalid('HASH_MISMATCH','Baseline CSV expected hash mismatch')
            baseline=parse_rows(data,self.limits['rows'])
            def groups(rows):
                result=collections.defaultdict(list)
                for r in rows:result[r.start].append(dict(name=r.name,end=r.end,size=r.size))
                return {k:sorted(v,key=lambda x:(x['name'],x['end'],x['size'])) for k,v in result.items()}
            old,new=groups(baseline),groups(self.rows)
            self.report['comparison']=dict(baseline_sha256=sha(data),
                changed=[dict(entry=k,before=old[k],after=new[k]) for k in sorted(old.keys()&new.keys()) if old[k]!=new[k]],
                dropped=[dict(entry=k,rows=old[k]) for k in sorted(old.keys()-new.keys())],
                added=[dict(entry=k,rows=new[k]) for k in sorted(new.keys()-old.keys())])

    def reachability(self):
        graph=self.report['reachability']
        graph.update(roots=0,visits=0,stops={},edge_counts={},
            edge_detail_scope='Cross-row, tail and branch-likely edges; ordinary internal edges counted only')
        stops=collections.Counter();counts=collections.Counter();edge_keys=set();path_sites=set()
        known=self.row_starts|self.entries
        groups=collections.defaultdict(list)
        for row in self.valid:groups[row.start,row.end].append(row)
        roots=[(row.start,row,aliases) for aliases in groups.values() for row in aliases[:1]]
        for entry in sorted(self.entries-self.row_starts):
            owners=self.contains(entry)
            if owners:
                for row in owners:roots.append((entry,row,[row]))
            elif self.image.is_code(entry):
                roots.append((entry,Row('<profile>',entry,entry+4,4,0),[]))
                self.unknown('PROFILE_BLOCKS_UNAVAILABLE',entry)
            else:self.unknown('PROFILE_OUTSIDE_CODE',entry)
        for root,row,aliases in roots:
            graph['roots']+=1
            queue=collections.deque([root]);pred={root:None};adj=collections.defaultdict(list)
            visited=set();slot_visited=set();candidates=[];outside=set()
            def path_to(pc):
                path=[]
                while pc is not None:
                    path.append(pc);pc=pred[pc]
                return path[::-1]
            def visit(pc,slot=False):
                nonlocal visited,slot_visited
                target=slot_visited if slot else visited
                if pc in target:return
                target.add(pc);graph['visits']+=1
                if len(visited)+len(slot_visited)>self.limits['root_visits']:
                    raise Invalid('LIMIT_ROOT_VISITS','Per-root visited word limit exceeded')
                if graph['visits']>self.limits['module_visits']:
                    raise Invalid('LIMIT_MODULE_VISITS','Module visited word limit exceeded')
            while queue:
                pc=queue.popleft()
                if pc in visited:continue
                visit(pc)
                for alias in aliases:path_sites.add((alias.line,pc))
                c=self.get_control(pc)
                if c.kind in ('unsupported','unmapped'):
                    self.error('UNSUPPORTED_CONTROL','Unsupported/unmapped reached instruction',address=pc)
                    stops[c.kind]+=1;continue
                slot_exception=False
                if c.delayed:
                    slot=(pc+4)&0xffffffff;visit(slot,True)
                    if not self.image.is_code(slot):
                        self.error('SLOT_UNREADABLE','Reached delay slot lacks EE bytes',address=slot);continue
                    slot_kind=self.get_control(slot).kind
                    if slot_kind=='exception':
                        slot_exception=True;stops['exception_in_slot']+=1
                    elif slot_kind!='ordinary':
                        self.error('DELAY_CONTROL','Control transfer inside a delay slot is unsupported',address=slot);continue
                if slot_exception and not c.likely:continue
                if c.kind in ('return','indirect','exception'):
                    stops[c.kind]+=1;continue
                edges=[]
                if c.target is not None and c.kind!='call' and not slot_exception:edges.append((c.target,'taken'))
                if c.kind=='ordinary':edges.append(((pc+4)&0xffffffff,'fallthrough'))
                elif c.kind=='call' or (c.kind=='branch' and not c.unconditional):
                    edges.append(((pc+8)&0xffffffff,'continuation' if c.kind=='call' else 'not_taken'))
                if c.kind=='call':stops['callee_not_entered']+=1
                for target,kind in edges:
                    counts[kind]+=1
                    inside=row.start<=target<row.end
                    classification=self.classify(row,target)
                    if not inside or c.likely or pc in outside:
                        key=(root,row.line,pc,target,kind)
                        if key not in edge_keys:
                            edge_keys.add(key)
                            graph['edges'].append(dict(root=root,row=row.line,rows=[r.line for r in aliases],
                                address=pc,target=target,kind=kind,classification=classification,
                                slot_execution='taken_only' if c.likely else 'always' if c.delayed else 'none'))
                    if target in known and target not in (row.start,root):
                        stops['known_entry']+=1;continue
                    if not self.image.is_code(target):
                        stops['outside_ee_code']+=1;continue
                    if not inside:
                        outside.add(target)
                        if len(outside)*4>self.limits['tail_bytes']:
                            raise Invalid('LIMIT_TAIL','Per-root out-of-row traversal byte limit exceeded')
                        if c.kind!='ordinary':candidates.append((pc,target))
                    adj[pc].append(target)
                    if target not in pred:pred[target]=pc;queue.append(target)
            # Reverse reachability gives a real way back, including chained/cyclic tails.
            reverse=collections.defaultdict(list)
            for source,targets in adj.items():
                for target in targets:reverse[target].append(source)
            back={pc for pc in visited if row.start<=pc<row.end};todo=sorted(back);next_back={pc:None for pc in back}
            while todo:
                destination=todo.pop()
                for source in reverse[destination]:
                    if source not in back:
                        back.add(source);todo.append(source);next_back[source]=destination
            for source,target in sorted(set(candidates)):
                if not row.line:continue  # A profile root has no declared body end to diagnose.
                path=path_to(source)+[target]
                return_path=[];cursor=target
                if target in back:
                    while cursor is not None:return_path.append(cursor);cursor=next_back[cursor]
                self.finding('SEPARATED_BLOCK',row,source,'Path-supported out-of-row block; ownership is unproved',
                    target=target,root=root,rows=[r.line for r in aliases],path=path,way_back=target in back,
                    return_path=return_path,
                    intervening_entries=sorted(e for e in self.row_starts if min(row.start,target)<e<max(row.start,target)),
                    related_rows=[r.line for r in self.contains(target)])
        graph['stops']=dict(sorted(stops.items()));graph['edge_counts']=dict(sorted(counts.items()))
        tails=collections.defaultdict(set)
        for f in self.report['findings']:
            if f['code']=='SEPARATED_BLOCK':tails[f['target']].add(f['entry'])
        for f in self.report['findings']:
            if f['code']=='SEPARATED_BLOCK':f['shared_candidate_entries']=sorted(tails[f['target']])
        # Evidence attaches to the linear findings without treating no path as proof of dead code.
        for f in self.report['findings']:
            if f['code'] in ('TARGET_INTERIOR','TARGET_UNMAPPED','SLOT_SPLIT'):
                f['path_evidence']='recorded' if (f['row'],f['address']) in path_sites else 'linear_candidate_only'

    def audit(self):
        a=self.args
        audit=dict(correspondence='unverified',files={},registration_count=0,exit_occurrences=0,
            unique_exit_sites=0,unregistered_exits=0,native_stub_wrappers=0,slot_checks=[],configured_entries=[],external_targets=[])
        self.report['audit']=audit
        provenance=None
        if a.generation_provenance:
            blob,provenance=config_json(a.generation_provenance,self.limits['generated_bytes'])
            audit['provenance_sha256']=sha(blob)
            generation=provenance.get('generation',{})
            if (provenance.get('schema_version')!=1 or provenance.get('inputs')!=self.hashes or
                not isinstance(provenance.get('outputs'),dict) or not isinstance(generation,dict) or
                generation.get('exit_code')!=0 or not isinstance(generation.get('command'),list) or
                not generation['command'] or not all(isinstance(v,str) for v in generation['command']) or
                not re.fullmatch(r'[0-9a-f]{64}',str(generation.get('recompiler_sha256',''))) or
                not isinstance(generation.get('source_revision'),str) or not generation['source_revision']):
                self.error('GENERATION_MISMATCH','Invalid generation identity or selected input hashes')
                provenance=None
        else:self.error('GENERATION_UNVERIFIED','Generation correspondence was not supplied')
        folder=a.generated_dir
        if not folder.is_dir():raise Invalid('GENERATED_READ','Generated directory missing')
        batch_metadata={}
        if (folder/'ps2_generated_index.json').exists() or (folder/'ps2_generated_sources.json').exists():
            try:
                try:from generated_sources import validate_batch_index,BatchError
                except ImportError:from tools.debug.generated_sources import validate_batch_index,BatchError
                index=validate_batch_index(folder,self.limits['generated_bytes'])
                audit['batch_layout']=dict(batch_size=index['batch_size'],virtual_cpp_files=len(index['files']),
                                          physical_cpp_files=len(index['batches']),checksums='validated FNV-1a; provenance remains SHA-256')
                for name in ('ps2_generated_index.json','ps2_generated_sources.json'):
                    blob=(folder/name).read_bytes();batch_metadata[name]=blob
            except BatchError as error:raise Invalid('GENERATED_FORMAT',str(error))
        paths=[];count=0
        def walk_error(exc):raise Invalid('GENERATED_READ','Cannot enumerate generated inputs completely')
        for root,dirs,files in os.walk(folder,followlinks=False,onerror=walk_error):
            dirs.sort();files.sort()
            if any((Path(root)/name).is_symlink() for name in dirs):
                raise Invalid('GENERATED_READ','Generated directory symlinks are unsupported')
            for name in files:
                count+=1
                if count>self.limits['generated_files']:raise Invalid('LIMIT_GENERATED_FILES','Generated file limit exceeded')
                path=Path(root)/name
                if path.is_symlink():raise Invalid('GENERATED_READ','Generated file symlinks are unsupported')
                if path.suffix in ('.cpp','.h','.hpp'):paths.append(path)
        table={};functions={};exits=[];implicit_candidates=[];calls=collections.Counter();total=0;table_files=[]
        wanted={row.start for row,_,_,_ in self.slots}|self.entries
        for name,blob in batch_metadata.items():
            audit['files'][name]=sha(blob);total+=len(blob)
        # Only relevant function bodies are retained; all generated inputs are consumed once and hashed.
        for path in sorted(paths,key=lambda p:p.relative_to(folder).as_posix()):
            blob,text=text_snapshot(path,self.limits['generated_bytes']-total)
            total+=len(blob);label=path.relative_to(folder).as_posix()
            audit['files'][label]=sha(blob)
            code=cpp_code(text)
            recognized,entries=registrations(code)
            if recognized:
                table_files.append(label)
                for addr,entry in entries.items():
                    if addr in table:raise Invalid('GENERATED_FORMAT','Conflicting registration tables')
                    table[addr]=dict(file=label,**entry)
            definitions=list(re.finditer(r'\bvoid\s+(\w+_0x[\da-fA-F]+)\s*\(\s*uint8_t\s*\*\s*rdram\s*,\s*R5900Context\s*\*\s*ctx\s*,\s*PS2Runtime\s*\*\s*runtime\s*\)\s*\{',code))
            if path.suffix=='.cpp' and 'ctx->pc' in code and not definitions:
                self.error('GENERATED_FORMAT','Unrecognized wrapper format',file=label)
            for match in definitions:
                end=braced(code,match.end()-1);body=code[match.end():end-1]
                symbol=match[1];line=code.count('\n',0,match.end())+1
                initial_pc=re.search(r'ctx\s*->\s*pc\s*=\s*(0x[\da-fA-F]+)[uU]?\s*;',body)
                if not initial_pc:
                    stub=re.search(r'ctx\s*->\s*pc\s*=\s*getRegU32\s*\(\s*ctx\s*,\s*31\s*\)\s*;\s*(ps2_stubs|ps2_syscalls)::(\w+)\s*\(\s*rdram\s*,\s*ctx\s*,\s*runtime\s*\)\s*;\s*$',body)
                    if stub:
                        audit['native_stub_wrappers']+=1
                        functions[symbol]=dict(entry=None,file=label,line=line,code=body,labels={},native_stub=stub[1]+'::'+stub[2])
                        continue
                    self.error('GENERATED_FORMAT','Wrapper lacks literal initial PC',file=label);continue
                entry=int(initial_pc[1],16)
                # Literal external returns are the supported invariant, not arbitrary C++ evaluation.
                for exit_match in re.finditer(r'ctx\s*->\s*pc\s*=\s*(0x[\da-fA-F]+)[uU]?\s*;\s*return\s*;',body):
                    exits.append((entry,int(exit_match[1],16),label,line+body.count('\n',0,exit_match.start())))
                for call in re.finditer(r'runtime\s*->\s*dispatchGuestBranch\s*\(\s*rdram\s*,\s*ctx\s*,\s*(0x[\da-fA-F]+)[uU]?\s*,\s*0x[\da-fA-F]+[uU]?\s*,\s*0x[\da-fA-F]+[uU]?\s*,\s*PS2Runtime::GuestBranchKind::DirectCall\s*,',body):
                    calls[int(call[1],16)]+=1
                labels=resume_labels(body)
                tail=re.search(r'ctx\s*->\s*pc\s*=\s*(0x[\da-fA-F]+)[uU]?\s*;\s*$',body)
                if tail:
                    implicit_candidates.append(dict(entry=entry,end=int(tail[1],16),symbol=symbol,
                        labels=set(labels),file=label,line=line+body.count('\n',0,tail.start())))
                if entry in wanted or labels.keys()&self.entries:
                    if symbol in functions:raise Invalid('GENERATED_FORMAT','Duplicate supported wrapper')
                    functions[symbol]=dict(entry=entry,file=label,line=line,code=body,labels=labels)
        audit['generated_bytes']=total;audit['table_files']=table_files
        if not table_files or not table:self.error('GENERATED_FORMAT','No supported nonempty registration table')
        audit['registration_count']=len(table)
        for f in self.report['findings']:
            if f['code'] in ('TARGET_INTERIOR','TARGET_UNMAPPED') and f['target'] in table:
                f['generated_registration']=table[f['target']]
        if provenance:
            if provenance['outputs']!=audit['files']:
                self.error('GENERATION_MISMATCH','Generated output file set/hash differs from provenance')
            else:audit['correspondence']='matching_supplied_provenance'
        # A main table may explain external addresses; it must be explicitly supplied and hash-pinned.
        main={}
        if a.main_registration:
            blob,text=text_snapshot(a.main_registration,self.limits['generated_bytes'])
            if sha(blob)!=a.main_registration_sha256:raise Invalid('HASH_MISMATCH','Main registration hash mismatch')
            clean=cpp_code(text)
            if 'g_ps2RecompiledFunctionTableBase' not in clean:raise Invalid('GENERATED_FORMAT','Main context must be the main indexed table')
            _,main=registrations(clean)
            audit['main_context']=dict(label=a.main_registration.name,sha256=sha(blob),registrations=len(main),
                limitation='Declared generated main table; runtime module availability is not established')
        external=collections.Counter();unique=set()
        audit['direct_dispatch_calls']=dict(occurrences=sum(calls.values()),
            registered_current_module=sum(n for target,n in calls.items() if target in table),
            unresolved_current_module=[dict(target=t,occurrences=n) for t,n in sorted(calls.items()) if t not in table and self.image.offset(t) is not None],
            limitation='Calls can resolve through runtime stubs/overrides; no ownership or availability is inferred')
        for target,n in calls.items():
            if self.image.offset(target) is None:external[target]+=n
        rows_by_start={r.start:r for r in self.valid}
        for entry,target,file,line in exits:
            audit['exit_occurrences']+=1;unique.add((file,line,target))
            if target in table:continue
            if self.image.offset(target) is None:
                external[target]+=1;continue
            row=rows_by_start.get(entry,Row('<generated>',entry,entry+4,4,0))
            self.finding('GENERATED_TARGET_UNREGISTERED',row,entry,'Literal generated exit lacks current-module registration',
                target=target,generated_file=file,generated_line=line,address_kind='wrapper_entry')
            audit['unregistered_exits']+=1
        # An emitted final PC assignment is not necessarily reachable: JR/J can
        # leave a dead epilogue behind. Follow independent binary control flow from
        # the actual registered wrapper/resume entries before reporting that exit.
        audit['implicit_exits']=[]
        audit['unregistered_implicit_exits']=0
        audit['implicit_exit_scope']='Literal trailing PC; bounded possible binary paths from declared/configured entries and corroborated resume entries. Uncorroborated registrations are recorded separately. No general C++ equivalence or indirect-destination proof.'
        registered_by_symbol=collections.defaultdict(set)
        for address,record in table.items():registered_by_symbol[record['symbol']].add(address)
        # Registration alone can be a speculative indirect-fallback label. Build
        # independent possible paths from declared/configured entries, never seed
        # this search with every emitted resume label. Calls supply both their
        # direct target and possible return continuation, without ownership claims.
        corroborated=set(self.row_starts|self.entries);pending=list(corroborated);reached=set()
        while pending:
            pc=pending.pop()
            if pc in reached or not self.image.is_code(pc):continue
            reached.add(pc)
            if len(reached)>self.limits['module_visits']:
                raise Invalid('LIMIT_IMPLICIT_ROOTS','Implicit-entry evidence visit limit exceeded')
            c=self.get_control(pc)
            if c.kind in ('unsupported','unmapped','return','indirect','exception'):continue
            slot_exception=False
            if c.delayed:
                slotpc=(pc+4)&0xffffffff;slot=self.get_control(slotpc)
                if not self.image.is_code(slotpc) or slot.kind in ('unsupported','unmapped'):continue
                slot_exception=slot.kind=='exception'
            if slot_exception:
                if c.likely:pending.append((pc+8)&0xffffffff)
            elif c.kind=='ordinary':pending.append((pc+4)&0xffffffff)
            else:
                if c.target is not None:
                    corroborated.add(c.target);pending.append(c.target)
                if c.kind=='call' or (c.kind=='branch' and not c.unconditional):
                    continuation=(pc+8)&0xffffffff
                    if c.kind=='call':corroborated.add(continuation)
                    pending.append(continuation)
        # Merely passing through a PC inside one wrapper does not make it an
        # external entry into a different overlapping wrapper. Conditional
        # untaken paths and ordinary fall-through remain local paths.
        audit['implicit_entry_evidence_visits']=len(reached)
        audit['corroborated_implicit_entries']=len(corroborated)
        audit['uncorroborated_resume_entries']=[]
        implicit_visits=0
        for candidate in implicit_candidates:
            entry,end=candidate['entry'],candidate['end']
            if end<=entry or end>0xffffffff or end%4:continue
            registered_roots=registered_by_symbol[candidate['symbol']]&({entry}|candidate['labels'])
            roots=registered_roots&corroborated
            unknown=registered_roots-corroborated
            if unknown:
                audit['uncorroborated_resume_entries'].append(dict(entry=entry,entries=sorted(unknown),
                    generated_file=candidate['file'],limitation='Registration alone does not prove an indirect entry is reached'))
            pending=[(root,root,'root') for root in sorted(roots)]
            visited=set();witness=None
            while pending:
                pc,root,edge=pending.pop()
                if pc==end:
                    if edge=='fallthrough':witness=root;break
                    continue
                if pc<entry or pc>=end or pc in visited:continue
                visited.add(pc);implicit_visits+=1
                if len(visited)>self.limits['root_visits'] or implicit_visits>self.limits['module_visits']:
                    raise Invalid('LIMIT_IMPLICIT_VISITS','Implicit-exit path visit limit exceeded')
                c=self.get_control(pc)
                if c.kind in ('unsupported','unmapped') or not self.image.is_code(pc):
                    self.error('IMPLICIT_EXIT_UNSUPPORTED','Unsupported reached instruction in implicit-exit audit',address=pc)
                    continue
                slot_exception=False
                if c.delayed:
                    slot=self.get_control((pc+4)&0xffffffff)
                    if not self.image.is_code((pc+4)&0xffffffff) or slot.kind in ('unsupported','unmapped'):
                        self.error('IMPLICIT_EXIT_UNSUPPORTED','Unavailable/unsupported reached slot in implicit-exit audit',address=pc)
                        continue
                    slot_exception=slot.kind=='exception'
                if slot_exception:
                    if c.likely:pending.append(((pc+8)&0xffffffff,root,'fallthrough'))
                elif c.kind=='ordinary':pending.append(((pc+4)&0xffffffff,root,'fallthrough'))
                elif c.kind=='jump':
                    if c.target is not None:pending.append((c.target,root,'explicit'))
                elif c.kind=='call':pending.append(((pc+8)&0xffffffff,root,'fallthrough'))
                elif c.kind=='branch':
                    if c.target is not None:pending.append((c.target,root,'explicit'))
                    if not c.unconditional:pending.append(((pc+8)&0xffffffff,root,'fallthrough'))
            if witness is None:continue
            record=dict(entry=entry,root=witness,target=end,registered=end in table,
                generated_file=candidate['file'],generated_line=candidate['line'])
            audit['implicit_exits'].append(record)
            if end not in table and self.image.is_code(end):
                row=rows_by_start.get(entry,Row('<generated>',entry,end,end-entry,0))
                self.finding('GENERATED_FALLTHROUGH_UNREGISTERED',row,witness,
                    'Reachable trailing generated PC lacks current-module registration',
                    root=witness,target=end,generated_file=candidate['file'],generated_line=candidate['line'])
                audit['unregistered_implicit_exits']+=1
        audit['implicit_exit_visits']=implicit_visits
        audit['unique_exit_sites']=len(unique)
        audit['external_targets']=[dict(target=target,occurrences=n,
            evidence='supplied_main_registration' if target in main else 'unknown_external_module',
            registration=main.get(target)) for target,n in sorted(external.items())]
        context_files={};context_bytes=0
        for external_target in audit['external_targets']:
            registration=external_target['registration']
            if not registration:continue
            symbol=registration['symbol'];path=a.main_registration.parent/(symbol+'.cpp')
            if not path.is_file():
                external_target['resume_evidence']='wrapper_unavailable';continue
            if symbol not in context_files:
                blob,text=text_snapshot(path,self.limits['generated_bytes']-context_bytes)
                context_bytes+=len(blob);context_files[symbol]=(sha(blob),cpp_code(text))
            fingerprint,code=context_files[symbol]
            target=external_target['target']
            labels=resume_labels(code);resume=labels.get(target)
            external_target['resume_evidence']=dict(wrapper_file=path.name,sha256=fingerprint,
                declared_wrapper_entry=int(initial_pc[1],16) if (initial_pc:=re.search(r'ctx\s*->\s*pc\s*=\s*(0x[\da-fA-F]+)[uU]?\s*;',code)) else None,
                resume_label=resume['label'] if resume else None,line=resume['line'] if resume else None,
                limitation='Observed context wrapper; its generation correspondence is not independently audited here')
        for entry in sorted(self.entries):
            registered=table.get(entry);function=functions.get(registered['symbol']) if registered else None
            audit['configured_entries'].append(dict(entry=entry,registration=registered,
                wrapper_entry=function['entry'] if function else None,
                native_stub=function.get('native_stub') if function else None,
                resume_label=entry in function['labels'] if function else None))
            if registered and (not function or (not function.get('native_stub') and entry!=function['entry'] and entry not in function['labels'])):
                self.error('GENERATED_RESUME','Configured interior registration lacks recognized resume evidence',address=entry)
        for row,pc,c,word in self.slots:
            registered=table.get(row.start);function=functions.get(registered['symbol']) if registered else None
            evidence=dict(row=row.line,address=pc,slot_address=(pc+4)&0xffffffff,
                generated_file=function['file'] if function else None,registration=registered)
            audit['slot_checks'].append(evidence)
            if not function:
                evidence['result']='unmatched';self.error('GENERATED_WRAPPER','Slot candidate wrapper is unmatched',address=pc);continue
            body=function['code']
            matches=list(re.finditer(r'ctx\s*->\s*pc\s*=\s*(0x[\da-fA-F]+)[uU]?\s*;',body))
            locations=[m for m in matches if int(m[1],16)==pc and body[m.end():].lstrip().startswith('{')]
            if len(locations)!=1:
                evidence['result']='unsupported';self.error('SLOT_EMISSION_UNSUPPORTED','Unmatched supported branch block',address=pc);continue
            location=locations[0];start=location.end()
            while body[start].isspace():start+=1
            block=body[start:braced(body,start)]
            evidence['generated_line']=function['line']+body.count('\n',0,location.start())
            if nop(word):evidence['result']='nop_elision_allowed';continue
            # Independently derive ADDIU arithmetic from instruction bits. No source comment counts as evidence.
            if word is None or word>>26!=9 or c.kind!='branch' or c.likely:
                evidence['result']='unsupported';self.error('SLOT_EMISSION_UNSUPPORTED','Unsupported non-NOP slot or transfer pattern',address=pc);continue
            rs=(word>>21)&31;rt=(word>>16)&31;imm=struct.unpack('<h',struct.pack('<H',word&0xffff))[0]
            compact=re.sub(r'\s+','',block)
            operations=[f'SET_GPR_S32(ctx,{rt},(int32_t)ADD32(GPR_U32(ctx,{rs}),{literal}));' for literal in sorted({imm,imm&0xffffffff})]
            expected=[f'ctx->pc=0x{pc+4:x}u;ctx->in_delay_slot=true;{branch_pc}{operation}ctx->in_delay_slot=false;'
                for branch_pc in ('',f'ctx->branch_pc=0x{pc:x}u;') for operation in operations]
            # Address hex case may differ; do not change identifiers or operation literals.
            normalized=re.sub(r'\b0x([\da-fA-F]+)[uU]?',lambda m:'0x'+m[1].lower()+'u',compact)
            condition=re.search(r'constboolbranch_taken_0x[\da-fA-F]+=[^;]+;',normalized)
            actual=normalized[condition.end():] if condition else ''
            if not condition:
                evidence['result']='unsupported';self.error('SLOT_EMISSION_UNSUPPORTED','Conditional emitter pattern not recognized',address=pc)
            elif any(actual.startswith(pattern) for pattern in expected):evidence['result']='operation_present'
            elif 'in_delay_slot' in actual or 'SET_GPR_' in actual:
                evidence['result']='unsupported';self.error('SLOT_EMISSION_UNSUPPORTED','Transformed/mismatching slot operation',address=pc)
            else:
                evidence['result']='missing_operation'
                self.finding('SLOT_EMISSION_MISSING',row,pc,'Supported branch wrapper omits non-NOP ADDIU slot',
                    slot_address=pc+4,slot_word=word,generated_file=function['file'],generated_line=evidence['generated_line'])

    def run(self):
        try:
            self.load();self.scan()
            if 'reachability' in self.report['selected_passes']:self.reachability()
            if 'audit' in self.report['selected_passes']:self.audit()
        except Invalid as exc:self.error(exc.code,exc.message)
        except (TypeError,ValueError,KeyError,struct.error,RecursionError):
            self.error('INPUT_FORMAT','Unsupported input shape or encoding')
        self.report['summary']['decoded_words']=len(self.cache)
        for key in ('findings','errors','unknowns'):
            self.report[key].sort(key=lambda f:(f.get('row',0),f.get('address') or 0,f['code'],json.dumps(f,sort_keys=True)))
        code=2 if self.report['errors'] else 1 if self.report['findings'] else 0
        self.report.update(exit_code=code,status={0:'CHECKED',1:'REVIEW',2:'INCOMPLETE'}[code])
        return self.report


def markdown(report):
    lines=[f"# Function-map report: {report['status']}", '', f"Module: {report['module'].get('id','unknown')}",
           f"Passes: {', '.join(report['selected_passes'])}",'',
           'The checks do not certify all entries, ownership, generated semantics or runtime module availability.','']
    lines+=['## Inputs and coverage','', 'Tool: '+report['tool'], '',
        'Input fingerprints: `'+json.dumps(report['inputs'],sort_keys=True)+'`','',
        'Counts: `'+json.dumps(report['summary'],sort_keys=True)+'`','',
        'Bounds: `'+json.dumps(report['limits'],sort_keys=True)+'`','']
    graph={k:v for k,v in report['reachability'].items() if k!='edges'}
    lines+=['Reachability: `'+json.dumps(graph,sort_keys=True)+'`','']
    if report['audit'] is not None:
        audit={k:v for k,v in report['audit'].items() if k not in ('files','configured_entries')}
        lines+=['Generated audit: `'+json.dumps(audit,sort_keys=True)+'`','',
            'Per-file hashes, all configured entries and detailed edges are in report.json.','']
    for key in ('findings','errors','unknowns'):
        lines += [f'## {key.title()}','']
        for f in report[key]:lines.append('- '+json.dumps(f,ensure_ascii=True,sort_keys=True))
        if not report[key]:lines.append('None.')
        lines.append('')
    return '\n'.join(lines)


def arguments(argv):
    p=argparse.ArgumentParser(description=__doc__)
    for flag in ('profile','binary','csv','out','manifest','generated-dir','generation-provenance','baseline-csv','main-registration'):
        p.add_argument('--'+flag,type=Path,required=flag=='out')
    p.add_argument('--module');p.add_argument('--map',default='cleaned');p.add_argument('--passes')
    p.add_argument('--main-registration-sha256')
    for key,value in DEFAULT_LIMITS.items():p.add_argument('--limit-'+key.replace('_','-'),type=int,default=value)
    a=p.parse_args(argv)
    if a.manifest:
        if not a.module or any((a.profile,a.binary,a.csv)):p.error('manifest requires module and excludes direct input flags')
    elif not all((a.profile,a.binary,a.csv)) or a.module:p.error('direct mode requires profile,binary,csv; module belongs to manifest mode')
    if any(getattr(a,'limit_'+key)<=0 for key in DEFAULT_LIMITS):p.error('limits must be positive')
    if bool(a.main_registration)!=bool(a.main_registration_sha256):p.error('main registration requires its expected hash')
    return a


def main(argv=None):
    a=arguments(argv)
    if a.out.exists() or a.out.is_symlink():
        print('ERROR: output must be a new directory',file=sys.stderr);return 2
    if a.generated_dir and a.out.resolve().is_relative_to(a.generated_dir.resolve()):
        print('ERROR: output cannot be inside generated inputs',file=sys.stderr);return 2
    analysis=Analysis(a)
    try:
        report=analysis.run()
        if a.generated_dir and a.out.resolve().is_relative_to(a.generated_dir.resolve()):
            raise Invalid('OUTPUT_ALIAS','Output cannot be inside generated inputs')
        machine=(json.dumps(report,sort_keys=True,indent=2,ensure_ascii=True)+'\n').encode()
        human=markdown(report).encode()
        if len(machine)+len(human)>a.limit_report_bytes:raise Invalid('LIMIT_REPORT','Combined report byte limit exceeded')
        a.out.mkdir(parents=True,exist_ok=False)
        (a.out/'report.md').write_bytes(human)
        # Write exclusively after the readable report; never replace a completed output.
        pending=a.out/'.report.json.pending'
        with pending.open('xb') as stream:stream.write(machine)
        os.link(pending,a.out/'report.json')
        pending.unlink()
        print(f"{report['status']}: {len(report['findings'])} findings, {len(report['errors'])} errors, {len(report['unknowns'])} scope records")
        for f in (report['errors']+report['findings'])[:5]:
            pc=f.get('address');where=f' at 0x{pc:08X}' if pc is not None else ''
            print(f"  {f['code']}{where}: {f['message']}")
        return report['exit_code']
    except (Invalid,OSError,ValueError,struct.error,RecursionError) as exc:
        message=exc.message if isinstance(exc,Invalid) else 'Input/output cannot be processed completely'
        print('ERROR: '+message,file=sys.stderr);return 2


if __name__=='__main__':
    sys.exit(main())
