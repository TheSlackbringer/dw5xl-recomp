"""Independently authored 030 inputs. No game bytes, addresses or tool imports."""
from pathlib import Path
import hashlib
import json

KINDS = {
    'argument': {'locations': ['gpr:a0'], 'bits': 32, 'representation': 'pointer'},
    'return': {'locations': ['gpr:v0'], 'bits': 32, 'representation': 'unsigned'},
    'memory_output': {'target': 'out+4', 'access': 'write', 'bits': 32},
    'state_effect': {'target': 'context.error', 'effect': 'set:12'},
    'callback': {'arguments': [{'locations': ['gpr:a0'], 'bits': 32, 'representation': 'pointer'}],
                 'returns': {'locations': ['gpr:v0'], 'bits': 32, 'representation': 'signed'}, 'context': 'user'},
    'ownership': {'resource': 'heap', 'operations': {'allocate': 'runtime', 'free': 'runtime', 'reallocate': 'runtime'}},
    'clobber': {'locations': ['gpr:t0', 'gpr:t1']},
    'stack': {'alignment': 16, 'delta': 0},
    'gp': {'location': 'gpr:gp', 'policy': 'preserved'},
    'continuation': {'location': 'gpr:ra', 'policy': 'return'},
    'mmio': {'target': 'device.status', 'access': 'read', 'bits': 32},
}


def digest(blob):
    return hashlib.sha256(blob).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n', encoding='utf-8')


def fact(value, side):
    return {'value': value, 'evidence': [side], 'scope': 'reviewed_' + side,
            'rationale': 'Independent authored ' + side + ' contract; no executed behavior claim.'}


def field(kind, *, name=None):
    import copy
    return {'id': name or kind, 'kind': kind, 'required': True, 'condition': 'always',
            'original': fact(copy.deepcopy(KINDS[kind]), 'original'),
            'replacement': fact(copy.deepcopy(KINDS[kind]), 'replacement')}


def fixture(root):
    """Authored return instruction and delay slot, independent contract and handler text."""
    files = {'image': ('input.bin', bytes.fromhex('0800e00300000000'), 'binary'),
             'original': ('original.txt', b'input a0, output v0\nindependently specified contract\n', 'text'),
             'replacement': ('handler.cpp', b'// handler reads a0; writes v0\n// alias _sample -> sample\n', 'text'),
             'profile': ('profile.toml', b'[general]\nstubs = ["_sample@0x1000"]\n', 'profile')}
    artifacts = []
    for key, (name, blob, kind) in files.items():
        (root / name).write_bytes(blob)
        artifacts.append({'id': key, 'path': name, 'sha256': digest(blob), 'kind': kind})
    return {'schema_version': 1, 'audit_id': 'synthetic', 'source_revision': 'authored-fixture',
            'artifacts': artifacts,
            'modules': [{'id': 'test', 'serial': 'synthetic', 'artifact': 'image', 'format': 'raw',
                         'base_address': '0x1000', 'load_generation': 'fixture'}],
            'citations': [{'id': 'original', 'artifact': 'original', 'lines': [1, 2], 'contains': 'input a0'},
                          {'id': 'replacement', 'artifact': 'replacement', 'lines': [1, 2], 'contains': 'reads a0'},
                          {'id': 'profile', 'artifact': 'profile', 'lines': [1, 2], 'contains': '_sample@0x1000'},
                          {'id': 'instruction', 'module': 'test', 'instructions': [{'address': '0x1000', 'hex': '0800e003'}]}],
            'conditions': [{'id': 'always', 'state': 'established', 'value': True,
                            'evidence': ['original'], 'rationale': 'Authored comparable input domain.'}],
            'aliases': [{'name': '_sample', 'handler': 'sample', 'evidence': ['replacement']}],
            'registrations': [{'id': 'selected', 'module': 'test', 'address': '0x1000',
                               'name': '_sample', 'condition': 'always', 'evidence': ['profile'],
                               'profile': {'artifact': 'profile', 'entry': '_sample@0x1000'}}],
            'bindings': [{'id': 'sample', 'module': 'test', 'address': '0x1000', 'name': '_sample',
                          'handler': 'sample', 'selection_scope': 'declared',
                          'question': 'Do the independently specified selected contracts agree?',
                          'fields': [field('argument'), field('return')], 'observations': [],
                          'limitations': ['No execution or whole-program ABI equivalence established.']}]}


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    args.directory.mkdir(parents=True, exist_ok=False)
    write_json(args.directory / 'manifest.json', fixture(args.directory))
    print('Created authored binding example.')
