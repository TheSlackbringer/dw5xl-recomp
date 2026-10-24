# Compile code copied to another execution address

Some SDKs copy an executable block from the ELF into kernel RAM, then install callbacks or syscall
handlers at its new address. An existing native wrapper at the source address does not establish
correct dispatch or PC-relative behaviour at the destination.

Declare verified copies in the recompiler profile:

```toml
[[code_copies]]
name = "example_kernel"
source = 0x1000
destination = 0x80070000
size = 0x28
functions = [[0, 0x10], [0x20, 8]]
```

`source` and `size` select unchanged file-backed input bytes. `destination` is their actual execution
address in direct-mapped EE RAM. Each `functions` pair is a byte offset and byte length within the
copy. Supply qualified code ranges and entry points from original callers, tables and control flow;
do not mark surrounding data executable to fill a gap. Names must be unique C++ identifiers.
Ranges must be aligned, nonempty, nonoverlapping and contained in the copy. The destination cannot
overlap another mapped image, including an alias of the same physical memory.

The compiler decodes the copied code at its destination PCs, preserving architectural branches,
J/JAL high address bits, link addresses, delay slots and exception PCs. It writes ordinary native
C++ alongside the main/module output and emits a separate sparse copied-image descriptor. It does
not expand the main dense table up to a kernel virtual address. Runtime descriptors and original
instruction expectations are generated from the player's input, never shipped as game code.

A copied entry resolves only when all of that image's translated instruction words match loaded
RAM. Unloaded, partial and overwritten code is rejected. Non-code data in the same image may change.
Each image is independent of overlay activation; multiple verified copies may coexist. Resolution
uses the declared execution addresses, not blanket virtual/physical function aliases. A direct jump
to a copied entry also uses guarded dispatch. Normal main-table calls retain their existing fast path.

This adds no runtime instruction decoder or interpreter. Byte comparison establishes loaded-code
identity; ordinary compiled functions execute the instructions. The existing hardware models retain
their roles. Supporting a copied entry does not certify its privileged operations or every possible
callback value.

Use `tools/debug/validate_function_map.py` before generation and audit matching generated output
afterwards. It independently maps copy destinations back to the selected input and adds the declared
function ranges as roots. Its structural/native-output checks do not prove that a particular runtime
copy has occurred; the dispatcher and execution tests cover availability separately.

Fast public regressions are in `tools/tests/test_code_copies.py`; actual generated native copied-code
and registry checks are part of `ps2x_boundary_native_tests`.
