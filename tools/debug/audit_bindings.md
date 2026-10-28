# Review selected replacement contracts

`audit_bindings.py` is a read-only, offline evidence checker for Python 3.11+ (standard
library only). Use it when investigating a suspect SDK replacement, argument/result
locations, missing memory output, callback/state ownership or a proposed binding change.
It checks explicit reviewed contracts and their evidence; it does not recover an ABI
from arbitrary machine code or prove numerical/gameplay equivalence. It adds no runtime
hooks. Ordinary play, FPS profiling and unrelated edits do not need a real-input run.

```sh
python tools/debug/audit_bindings.py --manifest scratch/selected.json --out scratch/review
python -m unittest discover -s tools/tests -p test_audit_bindings.py -v
```

The output directory must not exist. The tool creates `report.json` and `report.md` only
after request validation and report-size checks. It refuses existing paths, including
links, rather than replacing inputs/previous reports. An I/O failure cleans up files it
created where possible. Inputs are bounded snapshots and are never rewritten. Keep all
game-derived evidence and real reports outside Git. No game, compiler, Ghidra install,
GPU, network or admin access is needed to run the authored synthetic examples/tests.

## Schema version 1

Unknown JSON keys and duplicate keys are errors. Objects are explicit rather than a
source-regex guess at effective runtime registrations. Paths are relative to the manifest;
absolute paths work but reports use logical IDs, not those paths. All lists below have
unique IDs unless otherwise specified. IDs are ASCII letters/digits, `_`, `.`, `-`.
Addresses are `0x` hexadecimal unsigned 32-bit strings. SHA-256 values are lowercase hex.

The top-level object has these required keys:

| Key | Content |
|---|---|
| `schema_version` | Integer `1`. |
| `audit_id`, `source_revision` | Nonempty review label and source revision label (not a build proof). |
| `artifacts` | `{id,path,sha256,kind}` snapshots; kind `binary`, `text`, `profile` (TOML), or `ghidra`. A Ghidra artifact also has `module`. |
| `modules` | `{id,serial,artifact,format,load_generation}`. `artifact` is a binary ID. Format `elf` uses file-backed ELF32 LE MIPS mappings; `raw` additionally needs `base_address`, optionally `code_offset` (default 0). |
| `citations` | Exact text ranges or original instruction bytes, below. |
| `conditions` | `{id,state,evidence,rationale}`, with `state` `established`, `unknown` or `incompatible`. Established conditions also have Boolean `value`. |
| `aliases` | `{name,handler,evidence}` records. Aliases are explicit reviewed mappings; the checker does not strip underscores or infer them from names. |
| `registrations` | `{id,module,address,name,condition,evidence}`, optionally `profile:{artifact,entry}`. |
| `bindings` | Selected questions/contracts, below. |

Registration `profile.entry` supports an explicit `name@0xaddress` token in
`[general].stubs`. Both the actual parsed TOML membership and the token's name/address
must agree. Other profile selection forms need a reviewed declaration and citations;
this adapter does not claim to parse arbitrary app registration code. Known-inactive
registrations are excluded. Unknown conditions/aliases/evidence or multiple active
registrations prevent clean selection. A different established handler is a contradiction.
Equal addresses in separately identified modules/load contexts remain separate.

### Citations and original bytes

Text citations have `{id,artifact,lines:[first,last]}`, with positive inclusive line
numbers and optional `contains` literal text that must occur in that range. References
to a nonexistent range or stale artifact are incomplete evidence. Snapshot hashes prove
which bytes were reviewed, not that a reviewer interpreted them correctly.

Instruction citations have `{id,module,instructions:[{address,hex},...]}`. Every supplied
word must match the mapped original binary. `hex` describes exactly four bytes, in file
byte order. This is correspondence checking, not a new disassembler or execution oracle.

A `ghidra` artifact accepts an existing tab-separated export with a
`PROGRAM\tname\t<elf-sha256>\tr5900:LE:32:default` header and
`INS\t<hex-address>\t<8-hex-file-bytes>\t<description>` records. Program identity and every
INS record are checked against that module's verified snapshot. Descriptions/decompiler
prototypes are not automatically interpreted. `INCOMPLETE` export records and failed
`DECOMP` records remain visible and prevent a clean report. Use bounded selected exports;
do not infer exhaustive callers or live execution from an export.

### Bindings, facts and conditions

A binding is `{id,module,address,name,handler,selection_scope,question,fields,observations,
limitations}`. `selection_scope` is `declared` or `reviewed_effective`; the latter records
a human's scoped review and still is not proof of live selection. `limitations` is an
array of nonempty strings. Selection always reports this scope. The fields array is
nonempty; each is `{id,kind,required,condition,original,replacement}`.

Each side is null (missing evidence), a reviewed fact, or a list of alternative reviewed
facts. A fact is `{value,evidence,scope,rationale}`; evidence is a nonempty citation-ID
array. Scope is `reviewed_original` on the original side and `reviewed_replacement` on the
replacement side. Rationale is required. Execution/gameplay scopes are not accepted as
machine-verified evidence. Conflicting valid alternatives produce an unknown field,
retaining their observations. Compatible duplicate alternatives do not invent a conflict.

Supported fact kinds and exact value shapes:

| Kind | Value |
|---|---|
| `argument`, `return` | `{locations:[...],bits,representation}`. Ordered, unique locations; bits 8/16/32/64/128; representation `signed`, `unsigned`, `pointer`, `float`, `software_double`, `raw`. |
| `memory_output`, `mmio` | `{target,access,bits}`; symbolic target, access `read`, `write`, `read_write`, `none`, same bit widths. |
| `state_effect` | `{target,effect}`; independently reviewed symbolic target/postcondition. |
| `callback` | `{arguments:[slot,...],returns:slot-or-null,context}`; slots have the argument shape above. |
| `ownership` | `{resource,operations:{operation:owner,...}}`; nonempty explicit operation/owner mapping. |
| `clobber` | `{locations:[...]}`; treated as a set. |
| `stack` | `{alignment,delta}`; positive power-of-two alignment, signed 32-bit delta. |
| `gp`, `continuation` | `{location,policy}`; explicit reviewed location and policy. |

Locations use `gpr:a0`, `gpr:t0`, `gpr:ra`, `fpr:f13`, `stack:+0x20`, etc.; register
names/numbers must be valid. Symbols/policies identify reviewed facts; no expression is
evaluated. Physical register aliases (`a0`/`r4`, `fp`/`s8`) and equal signed hexadecimal
stack offsets are normalized for comparison; input observations remain unchanged. Symbolic
targets/postconditions must use the same reviewed labels for equivalent meanings; arbitrary
expression equivalence is outside the checker. Unknown fact kinds remain visible as unsupported. Unknown required kinds
prevent clean completion; optional unknowns remain coverage limits.

A field compares its exact typed values only under an established true condition with
valid evidence. Missing, conflicting, unsupported or incomparable facts produce
`unknown`. Established unequal contract facts produce `contradiction`; equal facts
produce `compatible` for the checked field. A false comparison condition means that case
was not checked, so a required field remains unknown. False registration conditions,
in contrast, exclude that registration. Input revision labels are not evidence of a build.

### Structural observations are separate

An observation is `{id,required,original,replacement}`, with string-valued reviewed facts.
Valid cited observations are `verified` as reviewed descriptions and their strings are
reported as `same_description` or `different_descriptions`. This never creates a semantic
contradiction by itself. For example, “computed correction” and “constant zero” establish
a source-backed policy difference; they do not prove different numerical results for a
particular input. Record numerical equivalence as an unsupported/unresolved required
field if the selected question requires it. The same applies to allocation failure state
when equivalent exhaustion conditions have not been established.

Preserve original expectations independently of replacement observations and of this
tool's output. For a real review, retain the question, manifests, evidence snapshots,
expected report assertions and limitations. Repeat against the same snapshots; then use
an authored evidence/selection variant to check that stale/incomplete/conflicting inputs
require review again. Never mutate original game files to make a variant. Replacing a
hash alone is not semantic re-review; the checker cannot verify arbitrary annotations.

## Reports and limits

JSON is canonical/deterministic for identical inputs; Markdown displays the same issues,
selection results, checked fields, observations and limitations. Reports cite logical
artifact IDs and exact line/instruction references and hash the bytes actually consumed.
No output claims full ABI, numerical, whole-program or gameplay equivalence.

Exit statuses: **0** all required checks complete, no contradictions; **1** established
contradictions with complete required evidence; **2** invalid/incomplete required evidence.
Incomplete evidence takes precedence while valid contradictions remain in the report.
Invalid schema/limits/output requests leave existing outputs untouched and print a bounded
JSON diagnostic. A complete report with an unknown required numerical field legitimately
exits 2; producing that honest result can still qualify the evidence tool.

Defaults: manifest 2 MiB, individual artifact 64 MiB, all artifact snapshots 128 MiB,
4,096 combined records, 4 MiB per output report, JSON nesting 24 and 100,000 JSON nodes.
CLI overrides: `--max-manifest-bytes`, `--max-artifact-bytes`, `--max-total-bytes`,
`--max-records`, `--max-report-bytes` (positive integers, hard-capped). Reads use bounded
chunks, not allowance-sized allocations. Reports never run subprocesses, fetch dependencies,
evaluate input code or modify Ghidra projects, configs, maps or generated output.

To create a complete authored example without game data:

```sh
python tools/tests/binding_fixtures.py scratch/binding-example
python tools/debug/audit_bindings.py --manifest scratch/binding-example/manifest.json --out scratch/binding-example/report
```
