# Validate EE function maps and generated output

`validate_function_map.py` is an explicit, read-only debugging tool for Python 3.11+
(standard library only). It checks a function map against its binary, follows bounded
instruction paths, and optionally audits corresponding PS2Recomp output. It never repairs
maps, executes game instructions, starts the game or improves FPS by itself.

Use it when changing an export/map or boundary, investigating missing targets or omitted
instructions, and before regenerating edited boundaries. Add the generated audit after
corresponding output exists. Record the selected passes, report and limitations in the
investigation. Ordinary play, packaging, FPS profiling and unrelated changes do not need
a real-map run. Synthetic tests are fast checks; real inputs never enter CI.

## Commands

From the repository root, supply all three inputs explicitly:

```sh
python tools/debug/validate_function_map.py --profile profile.toml --binary input.elf --csv functions.csv --out scratch/map-check
```

Default passes are `scan,reachability`. Select `--passes scan` for a deliberately limited
linear inspection. Adding `--generated-dir` selects `scan,reachability,audit` by default:

```sh
python tools/debug/validate_function_map.py --profile profile.toml --binary input.elf --csv functions.csv --generated-dir scratch/generated --generation-provenance scratch/generation.json --out scratch/audit
```

`--passes scan,audit` is also available. Reports always identify which passes ran.
`--baseline-csv raw.csv` lists changed/dropped/added entry groups, preserving aliases.
The comparison is informational; a changed End or Size is not automatically an error.

For repeatability, use `--manifest local.json --module main --map cleaned` instead of the
three direct input flags. Example schema (replace every hash with the real SHA-256):

```json
{
  "schema_version": 1,
  "modules": [{
    "id": "main",
    "serial": "local-disc-label",
    "format": "elf",
    "profile": {"path": "profile.toml", "sha256": "<64 lowercase hex digits>"},
    "binary": {"path": "input.elf", "sha256": "<64 lowercase hex digits>"},
    "maps": {
      "cleaned": {"path": "functions.csv", "sha256": "<64 lowercase hex digits>"},
      "raw": {"path": "raw.csv", "sha256": "<64 lowercase hex digits>"}
    },
    "baseline_map": "raw",
    "generated_dir": "generated",
    "generation_provenance": "generation.json"
  }]
}
```

Paths are relative to the manifest; absolute paths are accepted but reports use logical
labels/basenames. Raw modules must match the profile's `module_id`. Optional manifest
`format`, `raw_base` and `raw_code_offset` must agree with the profile. Every referenced
primary/baseline map has an expected hash. Omit optional fields that do not apply; an
overlay need not have a raw export. A requested missing map fails rather than selecting
another map. Identical addresses in separate overlay modules stay separate.

## Input interpretation

CSV has exactly `Name,Start,End,Size`, hexadecimal Start/End, decimal Size and exclusive
End. Same-range aliases are retained. Conflicting ends for one entry are findings.
Size is Ghidra body byte count, not necessarily End minus Start. A positive Size/span
mismatch records unavailable block information; it does not prove corruption or ownership.

The TOML profile is authoritative. Both `[general]` and root `entry_points` and legacy
`untracked_stubs` are read. Address literals, `name@address` and uniquely resolved map
names are supported. Comments do not contribute addresses. Unresolved names stay unknown.

ELF must be ELF32 little-endian MIPS with valid file-backed load mappings and executable
sections. BSS and `.vutext` / `.DVP.overlay` section families are excluded. Load flags alone
are insufficient to classify EE code. Raw input requires aligned `raw_base`,
`raw_code_offset` and `module_id` in `[general]`. Its payload after the prefix is mixed
code/data addressability, not proof that every byte is an instruction. Declared entries
are analysis roots, not evidence that the game executes them.

Inputs are bounded snapshots; reports hash the bytes actually consumed. Expected hashes
check identity, not historical correctness of a mapping. No input is rewritten.

## The three passes

**Scan:** structural ranges, alignment, Size, file-backed mapping and conflicting aliases;
boundary delay slots; and direct-target classification. It covers ordinary/likely
BEQ/BNE/BLEZ/BGTZ, the eight BLTZ/BGEZ/link forms, J/JAL/JR/JALR and BC0/BC1/BC2
false/true/likely forms. Signed offsets and 32-bit jump arithmetic are used. Reserved
control encodings are never positive branch evidence. Calls do not imply ownership.

Targets are `in_row`, `map_entry`, `profile_entry`, `other_row_interior`,
`unmapped_in_module` or `outside_module`. An unexplained interior/hole target is a review
candidate, not proof that a function is missing. Generated resume registrations can
explain it. A slot outside End is reported even if another row starts at that slot.
`slot_nop` distinguishes the supported no-op encodings (zero and ADDIU zero,zero,0)
from useful arithmetic or unreadable bytes.

**Reachability:** each declared range/root retains its row context, including overlaps and
aliases. Configured interior entries are additional roots. The graph follows ordinary
fall-through and direct non-call edges, stops at another known entry, and conservatively
follows call continuations. Branch-likely slots execute only on the taken path; a BREAK
or syscall in such a slot ends that path. Returns, indirect jumps and exceptions are
counted coverage stops. Unsupported reached transfers are incomplete. A visited set
terminates cycles; separate word/root/module/tail budgets bound the work.

`SEPARATED_BLOCK` records a path into an out-of-row block, reverse reachability back,
intervening entries, related rows and other candidate entries sharing that target.
It does not authorize extending one contiguous interval. Profile roots without a declared
body cannot establish a misplaced boundary: they record `PROFILE_BLOCKS_UNAVAILABLE`.
Unreachable branch-looking data is not path evidence. Linear candidates remain visible
even without a discovered path; that absence does not establish dead code. Detailed
edges cover cross-row, tail and branch-likely edges; ordinary internal edges are counted.

**Audit:** recognizes the current main indexed registration table, raw module entry table,
and generated wrapper signature. Hashes every consumed `.cpp`/`.h`/`.hpp` file. Literal
`ctx->pc = <address>; return;` exits are checked against current-module registrations.
This check deliberately covers that emitted invariant; dynamic exits and general C++
equivalence are outside its claim. External module availability remains unknown.
Configured-entry records distinguish wrapper entries from emitted resume case/goto labels.
Literal trailing PC assignments also receive a bounded binary-path check, including
configured interior entries and possible call returns. Only resume entries corroborated
by paths from declared/configured roots can prove a missing fall-through registration.
Speculative indirect-fallback labels are listed under `uncorroborated_resume_entries`;
their presence alone cannot prove execution into adjacent padding/data. Neither this
check nor a clean result proves all indirect targets or general C++ equivalence.

Initial emitted PC assignments determine wrapper entries; function-name suffixes are not
assumed to be entry addresses. Current `getRegU32(ctx,31)` / `ps2_stubs::<name>` or `ps2_syscalls::<name>` adapters
are counted as native stub wrappers, with the stub name shown for configured registrations.
Their native implementations are outside this audit's guest-instruction claim.

Literal current-format `dispatchGuestBranch` direct calls are counted separately. External
calls can use the explicitly supplied main context; unresolved local calls are listed with
their runtime-stub/override limitation rather than treated as body-ownership evidence.

Boundary-slot auditing is separate from registration. It recognizes the current ordinary
conditional-branch block and independently derives ADDIU register/immediate arithmetic
from the binary. Correct slot markers, instruction PC and actual arithmetic must appear
in the expected execution position. A comment/address token is insufficient. An absent
operation is `SLOT_EMISSION_MISSING` even with all destinations registered. Supported NOP
elision is legitimate. Other slot instructions, transformed expressions, unsupported
branch patterns and unmatched wrappers return incomplete; they are not certified correct.
The current optional `branch_pc` assignment and signed/unsigned decimal representations
of the same 32-bit ADDIU immediate are recognized explicitly.

For an explicit external main context, provide `--main-registration register_functions.cpp`
and `--main-registration-sha256 <hash>`. Only the main indexed format is accepted, so an
unrelated overlay cannot resolve addresses by accident. Relevant adjacent wrapper files
are snapshotted/hashed to show resume evidence. This is declared context, with its own
correspondence limitation; it does not prove runtime availability or independently audit
that main generation.

## Generation correspondence

The optional provenance file is schema 1:

```json
{
  "schema_version": 1,
  "inputs": {"profile": "<sha256>", "binary": "<sha256>", "csv": "<sha256>"},
  "generation": {
    "command": ["<recompiler>", "<effective-profile>"],
    "exit_code": 0,
    "recompiler_sha256": "<sha256>",
    "source_revision": "<revision>"
  },
  "outputs": {"register_functions.cpp": "<sha256>", "example.cpp": "<sha256>"}
}
```

The output map must contain the exact relative file set and hashes consumed by the audit.
Missing provenance gives `unverified` / exit 2, while still allowing investigation.
Mismatched inputs/outputs give `GENERATION_MISMATCH` / exit 2. Matching metadata is named
`matching_supplied_provenance`; it is a consistency check on a supplied declaration, not
proof that a command was actually run. Do not invent metadata around old output and call
it fresh qualification.

For corresponding-output qualification, observe the existing recompiler command generating
into a new scratch directory. Record executable/source identity, before/after input hashes,
command result and output hashes. If path fields are rewritten for isolation, verify the
effective TOML differs only in input/map paths referring to the same files and the new
output path; record its hash as additional generation metadata. Preserve the original
profile and current generated directory. No game build or gameplay is needed for this tool.

## Reports and limits

The output directory must be new and outside generated inputs. Existing files/directories
and aliases are refused. `report.md` is readable evidence; `report.json` is schema 1, with
identities, limits, selected passes, summaries, findings, coverage, paths and generated
evidence. JSON is published only after both reports have been fully written. A failed write
returns 2; unfinished output has no completed JSON marker. Reports contain no timestamps
and have deterministic ordering for identical snapshots, options and tool revision.

| Exit | Meaning |
| --- | --- |
| 0 `CHECKED` | Requested supported checks completed without findings; declared exclusions remain |
| 1 `REVIEW` | Completed with findings; this is often a successful diagnostic result |
| 2 `INCOMPLETE` | Invalid input/identity, unsupported requested audit coverage, limit or I/O failure |

Incomplete overrides review. No result certifies all code, unique body ownership or gameplay.
Main finding codes are `ROW_RANGE`, `ROW_ALIGNMENT`, `ROW_UNMAPPED`, `ROW_SIZE`,
`ALIAS_CONFLICT`, `SLOT_SPLIT`, `TARGET_INTERIOR`, `TARGET_UNMAPPED`, `SEPARATED_BLOCK`,
`GENERATED_TARGET_UNREGISTERED` and `SLOT_EMISSION_MISSING`. Errors and scope records are
separate arrays. Counts distinguish repeated row occurrences from unique instruction sites.

Defaults are binary 64 MiB; CSV 16 MiB; profile/manifest 1 MiB each; 100,000 rows;
65,536 visited words per root; 2,000,000 per module; 4 KiB of distinct out-of-row words per
root; 10,000 findings/coverage records; 32 MiB combined reports; and 512 MiB / 100,000 files
per generated module. `--help` lists `--limit-*` overrides. Exhaustion is incomplete,
with no silent truncation. These are tool resource bounds, not game acceptance thresholds.

## Public tests

```sh
python -m unittest discover -s tools/tests -p test_validate_function_map.py -v
```

Fixtures independently author tiny ELF/raw inputs, branch words, maps and emitted snippets.
No disc, Ghidra, private notes, GPU or installed recompiler is required. Tests cover both
input modes, branch families, graph paths, generated omissions and registration, hashes,
unsupported patterns, limits, determinism and preservation. A platform lacking symlink
creation can skip only that explicit alias-creation case; ordinary use does not need admin.

The implementation is original project code. Related design reference:
[DQ8-Recompiled boundary analysis and tests at 3e74da68](https://github.com/Sinan-Karakaya/DQ8-Recompiled/tree/3e74da68d7633db9996457dca4a9388ece0847c0/tools).
No upstream code, maps, generated game code or repairs were imported.
