#!/usr/bin/env python3
"""Find game functions the recompiler would not translate although the game can reach them (issue 026).

The recompiler translates the functions in the Ghidra function lists plus the `entry_points` of the profile. A
function the game only reaches through a pointer (a handler table, a vtable) can be missing from both; the game
then stops at the first call ("[guest-branch:missing-target]" in game.log) and the picture freezes.

Reads the player's own disc files under game/dw5xl (read only) and prints addresses only. Looks for:
  - aligned 32-bit words anywhere in the main ELF / overlays that point into code no function starts at, and
  - lui + addiu/ori pairs that build such an address in a register,
where the target looks like a function: it ends in `jr $ra` before the next known function and, if it lies inside
a known function's range, it starts right after that function's own `jr $ra` + delay slot (Ghidra merged it).
Verified false positives (instruction words that look like addresses) are listed in ALLOWED.

Usage: tools/find_missing_entries.py [--repo DIR]   exit code 1 if anything unexplained is found."""
import argparse, bisect, csv, os, re, struct, sys

JR_RA = 0x03E00008

# Checked by hand (issue 026, 2026-10-20): mid-function instruction words that happen to equal a code address.
ALLOWED = {
    ('main', 0x11A100): 'delay slot inside the 0x11a0d8 handler; the "pointer" is an instruction word at 0x154174',
    ('main', 0x12903C): 'syscall inside the 0x129038 stub; "pointers" are instruction words',
    ('main', 0x129040): 'jr ra inside the 0x129038 stub; "pointers" are instruction words',
}


def load_functions(path):
    functions = []
    with open(path) as f:
        for row in csv.reader(f):
            try:
                functions.append((int(row[1], 16), int(row[2], 16)))
            except (IndexError, ValueError):
                pass
    return sorted(functions)


def entry_points(toml_text):
    block = re.search(r'^entry_points\s*=\s*\[(.*?)\]', toml_text, re.S | re.M)
    if not block:
        return set()
    return {int(a, 16) for a in re.findall(r'0x[0-9A-Fa-f]+', block.group(1))}


def load_modules(repo):
    game = os.path.join(repo, 'game', 'dw5xl')
    recomp = os.path.join(repo, 'recomp', 'dw5xl')
    elf = open(os.path.join(game, 'SLES_538.60'), 'rb').read()
    phoff = struct.unpack_from('<I', elf, 0x1C)[0]
    _, offset, vaddr, _, filesz = struct.unpack_from('<5I', elf, phoff)
    toml = open(os.path.join(recomp, 'dw5xl.toml')).read()
    modules = {'main': (elf[offset:offset + filesz], vaddr,
                        load_functions(os.path.join(recomp, 'ghidra_functions.csv')), entry_points(toml))}
    for name in ('ovl00', 'ovl01', 'ovl02'):
        toml = open(os.path.join(recomp, 'overlays', name, f'{name}.toml')).read()
        base = int(re.search(r'raw_base = (0x\w+)', toml).group(1), 16)
        code = int(re.search(r'raw_code_offset = (0x\w+)', toml).group(1), 16)
        data = open(os.path.join(game, os.path.basename(re.search(r'input = "([^"]+)"', toml).group(1))),
                    'rb').read()[code:]
        modules[name] = (data, base, load_functions(os.path.join(recomp, 'overlays', name,
                                                                 'ghidra_functions.code.csv')), entry_points(toml))
    return modules


def scan(modules):
    def word(name, address):
        data, base = modules[name][0], modules[name][1]
        o = address - base
        return struct.unpack_from('<I', data, o)[0] if 0 <= o <= len(data) - 4 else None

    starts = {n: [a for a, _ in m[2]] for n, m in modules.items()}
    known = {n: set(starts[n]) | m[3] for n, m in modules.items()}

    def missing_function(name, target):
        if target in known[name] or word(name, target) in (None, 0):
            return False
        functions, s = modules[name][2], starts[name]
        i = bisect.bisect_right(s, target) - 1
        inside = i >= 0 and functions[i][0] <= target < functions[i][1]
        if inside:
            k = target - 4
            while word(name, k) == 0:
                k -= 4
            if word(name, k - 4) != JR_RA:
                return False  # a branch target inside a function, not a function
        end = s[i + 1] if i + 1 < len(s) else target + 0x800
        return any(word(name, a) == JR_RA for a in range(target, min(end, target + 0x800), 4))

    def owners(target, source):
        # Overlays share 0x61A800..; an overlay only points into itself or the main ELF.
        for name, (data, base, _, _) in modules.items():
            if base <= target < base + len(data) and (name == 'main' or source in (name, 'main')):
                yield name

    found = {}
    for source, (data, base, _, _) in modules.items():
        count = len(data) // 4
        for i in range(count):
            address = base + i * 4
            value = struct.unpack_from('<I', data, i * 4)[0]
            candidates = [('pointer', value)]
            if value >> 26 == 0x0F:  # lui rt, hi
                rt, high = (value >> 16) & 31, (value & 0xFFFF) << 16
                for k in range(1, 10):
                    if i + k >= count:
                        break
                    nxt = struct.unpack_from('<I', data, (i + k) * 4)[0]
                    if nxt >> 26 in (9, 13) and (nxt >> 21) & 31 == rt:  # addiu / ori from rt
                        low = nxt & 0xFFFF
                        if nxt >> 26 == 9:
                            low = low - 0x10000 if low & 0x8000 else low
                            candidates.append(('lui+addiu', (high + low) & 0xFFFFFFFF))
                        else:
                            candidates.append(('lui+ori', high | low))
                        break
            for kind, target in candidates:
                if target % 4 or target < 0x100000 or (kind == 'pointer' and abs(target - address) < 0x100):
                    continue
                for name in owners(target, source):
                    if missing_function(name, target):
                        found.setdefault((name, target), []).append((kind, source, address))
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--repo', default=os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))
    args = parser.parse_args()
    found = scan(load_modules(args.repo))
    unexplained = 0
    for (name, target), refs in sorted(found.items()):
        note = ALLOWED.get((name, target))
        if not note:
            unexplained += 1
        where = ', '.join(f'{k} at {s}:0x{a:x}' for k, s, a in refs[:3])
        print(f'{"allowed " if note else "MISSING "} {name} 0x{target:x}  ({where}){"  -- " + note if note else ""}')
    print(f'{unexplained} unexplained missing function(s), {len(found) - unexplained} allowed')
    return 1 if unexplained else 0


if __name__ == '__main__':
    sys.exit(main())
