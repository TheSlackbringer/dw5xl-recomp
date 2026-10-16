"""Conservative constant-VI proof for native VU BAL/JR control flow.
No game instructions or bytes. Unknown JR inputs fail before code generation.
"""
import struct
from collections import deque
import vu1_timing

BRANCHES = {0x20,0x21,0x24,0x25,0x28,0x29,0x2c,0x2d,0x2e,0x2f}
MAX_VALUES = 64
UNKNOWN = None


def signed16(x):
    x &= 0xffff
    return x-0x10000 if x&0x8000 else x


def join_value(a,b):
    if a is None or b is None:return None
    u=a|b
    return u if len(u)<=MAX_VALUES else None


def unary(a,fn):
    return None if a is None else frozenset(fn(x) for x in a)


def binary(a,b,fn):
    if a is None or b is None or len(a)*len(b)>MAX_VALUES:return None
    return frozenset(fn(x,y) for x in a for y in b)


def analyze(code,entry,decode_lower):
    if entry<0 or entry%8 or entry+8>len(code):raise ValueError('invalid VU entry')
    initial=(frozenset({0}),)+(None,)*15
    states={entry:(initial,initial)}; queue=deque([entry]); seen=set(); jr_targets={}; unresolved=set()
    ebit_ends=set(); delay_slots=set(); branches={}
    def pair(pc):
        if pc<0 or pc%8 or pc+8>len(code):raise ValueError(f'VU flow outside image at {pc:04x}')
        lo,up=struct.unpack_from('<II',code,pc)
        l=decode_lower(lo,pc)
        op=l.get('op',-1) if not(up>>31) else -1
        return lo,up,l,op
    def enqueue(pc,state):
        pair(pc)
        old=states.get(pc)
        if old is None:states[pc]=state;queue.append(pc);return
        merged=tuple(tuple(join_value(a,b)for a,b in zip(x,y))for x,y in zip(old,state))
        if merged!=old:states[pc]=merged;queue.append(pc)
    def transfer(pc,state):
        vals,branch_vals=state; vals=list(vals); prior=list(vals)
        lo,up,l,op=pair(pc)
        if up>>31:return tuple(vals),tuple(vals)
        usage=vu1_timing.lower_usage(lo)
        if (usage.vi_read | usage.vi_write) >> 16:
            raise ValueError(f'noncanonical VI operand unsupported in native-call graph at {pc:04x}')
        for reg in range(1,16):
            if usage.vi_write&(1<<reg):vals[reg]=None
        fs=l.get('fs',0)&15;ft=l.get('ft',0)&15;fd=l.get('fd',0)&15
        if op==0x21:
            if ft:vals[ft]=frozenset({signed16((pc+16)//8)})
        elif op in (0x08,0x09):
            imm=(lo&0x7ff)|((lo>>10)&0x7800)
            if ft:vals[ft]=unary(prior[fs],lambda x:signed16(x+(imm if op==8 else -imm)))
        elif l['type']=='INT':
            name=l['name'];target=ft if name=='IADDI' else fd
            if target:
                if name=='IADDI':vals[target]=unary(prior[fs],lambda x:signed16(x+l['imm']))
                elif name=='IADD':vals[target]=binary(prior[fs],prior[ft],lambda x,y:signed16(x+y))
                elif name=='ISUB':vals[target]=binary(prior[fs],prior[ft],lambda x,y:signed16(x-y))
                elif name=='IAND':vals[target]=binary(prior[fs],prior[ft],lambda x,y:signed16(x&y))
                elif name=='IOR':vals[target]=binary(prior[fs],prior[ft],lambda x,y:signed16(x|y))
        vals[0]=frozenset({0}); next_branch=list(vals)
        timing=vu1_timing.pair_timing(lo,up)
        if timing['firstVi'] and timing['flags']&vu1_timing.FLAG_BRANCH_DELAY:
            next_branch[timing['firstVi']]=prior[timing['firstVi']]
        return tuple(vals),tuple(next_branch)
    while queue:
        pc=queue.popleft();state=states[pc];seen.add(pc)
        lo,up,l,op=pair(pc)
        if op==0x25:raise ValueError(f'JALR not supported at {pc:04x}')
        if op in BRANCHES or up>>30&1:
            dl,du,d,do=pair(pc+8)
            if do in BRANCHES or du>>30&1:raise ValueError(f'branch/E-bit in delay slot {pc+8:04x}')
            if op in BRANCHES and up>>30&1:raise ValueError(f'branch with E-bit at {pc:04x}')
            seen.add(pc+8);delay_slots.add(pc+8)
            out=transfer(pc+8,transfer(pc,state))
            if up>>30&1:ebit_ends.add(pc+8);continue
            branches[pc]=op
            if op in (0x20,0x21):enqueue(l['tgt'],out)
            elif op==0x24:
                values=state[1][l['fs']&15]
                if values is None:unresolved.add(pc);continue
                dests={(int(x)&0xffff)*8&0x3fff for x in values}
                jr_targets.setdefault(pc,set()).update(dests)
                for dst in dests:enqueue(dst,out)
            else:
                enqueue(l['tgt'],out);enqueue(pc+16,out)
        else:enqueue(pc+8,transfer(pc,state))
    # A state can become less precise later; never retain an earlier optimistic proof.
    for pc in list(jr_targets):
        _,_,l,_=pair(pc)
        if states[pc][1][l['fs']&15] is None:unresolved.add(pc)
    if unresolved:raise ValueError('Unproven JR target(s): '+','.join(f'{pc:04x}'for pc in sorted(unresolved)))
    if delay_slots & set(states):raise ValueError('independent delay-slot entry is not supported')
    return {'reachable':sorted(seen),'jr_targets':jr_targets,'branches':branches,'ebit_ends':ebit_ends}
