"""Strict read-only lookup for 042 generated batches; no game execution or relaxed audits."""
import hashlib
import json
from pathlib import Path
import re

class BatchError(ValueError):
    pass

def checksum(data):
    value=14695981039346656037
    for byte in data:
        value=((value^byte)*1099511628211)&0xffffffffffffffff
    return f'{value:016x}'

def _name(value):
    if not isinstance(value,str) or not re.fullmatch(r'[A-Za-z0-9_.$-]+\.cpp',value):
        raise BatchError('Invalid virtual/batch filename')
    return value

def _number(value):
    if not isinstance(value,int) or isinstance(value,bool) or value<0:
        raise BatchError('Invalid batch count/range')
    return value

def _read(path,limit):
    if path.is_symlink() or not path.is_file() or path.stat().st_size>limit:
        raise BatchError('Missing, linked or oversized batch input')
    return path.read_bytes()

def validate_batch_index(folder,limit=1024**3):
    folder=Path(folder)
    try:
        index_bytes=_read(folder/'ps2_generated_index.json',min(limit,64*1024**2))
        source_bytes=_read(folder/'ps2_generated_sources.json',min(limit,4*1024**2))
        index=json.loads(index_bytes);sources=json.loads(source_bytes)
        if index['schema']!=1 or sources['schema']!=1:
            raise BatchError('Unsupported batch schema')
        size=_number(index['batch_size'])
        if not 1<=size<=4096 or sources['batch_size']!=size:
            raise BatchError('Invalid batch size')
        entries=index['files'];batches=index['batches']
        if not isinstance(entries,list) or not isinstance(batches,list) or not entries or not batches:
            raise BatchError('Empty or malformed batch inventory')
        names=[_name(e['name']) for e in entries]
        files=[_name(b['file']) for b in batches]
        if names!=sorted(set(names)) or len(files)!=len(set(files)):
            raise BatchError('Duplicate/unordered batch inventory')
        if names.count('register_functions.cpp')!=1:
            raise BatchError('Registration file missing or duplicated')
        if sources['batches']!=batches or index['legacy_cpp_count']!=len(entries) or sources['legacy_cpp_count']!=len(entries):
            raise BatchError('Batch manifests disagree')
        actual={p.name for p in folder.glob('*.cpp')}
        if actual!=set(files):
            raise BatchError('Missing, extra or stale generated C++ files')
        consumed=0;total=len(index_bytes)+len(source_bytes)
        for number,batch in enumerate(batches):
            n=_number(batch['count']);length=_number(batch['bytes'])
            if n!=min(size,len(entries)-consumed):
                raise BatchError('Changed compilation-group boundaries')
            blob=_read(folder/files[number],limit-total);total+=len(blob)
            if length!=len(blob) or batch['fnv1a64']!=checksum(blob):
                raise BatchError('Batch byte count/checksum mismatch')
            previous=0
            for entry in entries[consumed:consumed+n]:
                offset=_number(entry['offset']);length=_number(entry['size'])
                if entry['file']!=files[number] or offset<previous or offset+length>len(blob):
                    raise BatchError('Invalid/overlapping virtual range')
                if entry['fnv1a64']!=checksum(blob[offset:offset+length]):
                    raise BatchError('Virtual source checksum mismatch')
                previous=offset+length
            consumed+=n
        if consumed!=len(entries):
            raise BatchError('Unconsumed virtual source entries')
        return index
    except (OSError,KeyError,TypeError,json.JSONDecodeError) as error:
        raise BatchError('Unreadable/malformed generated batch inventory') from error

def virtual_inventory(folder,index=None):
    folder=Path(folder)
    index=index or validate_batch_index(folder)
    result={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in folder.glob('*.h')}
    blob=None;current=None
    for row in index['files']:
        if current!=row['file']:
            current=row['file'];blob=(folder/current).read_bytes()
        result[row['name']]=hashlib.sha256(blob[row['offset']:row['offset']+row['size']]).hexdigest()
    return result
