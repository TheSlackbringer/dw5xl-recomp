# Exact saved-frame comparison

Run from the repository root with Python 3 and Pillow **13.0.0** available:

```text
python tools/debug/compare_frames.py reference.png candidate.png --out scratch/frame-diff --heatmap
```

Dependencies are declared in `requirements-frame-compare.txt`. Nothing installs automatically.
The utility reads completed saved files. It never starts, hooks or records the game and has no normal-play overhead.
It is written independently; no upstream comparator code or fixtures are included.

| Exit | Result |
|---|---|
| 0 | IDENTICAL: every decoded red, green, blue and alpha byte matches |
| 1 | DIFFERENT: at least one pixel has a changed channel |
| 2 | ERROR: unsupported/damaged input, invalid context, or incomplete output |

An exact DIFFERENT report can be a successful debugging result. It does not diagnose the cause.
Different game moments, machines or drivers may produce different pixels. For regression evidence, hold
input/frame, output kind, backend, weave, machine/GPU/driver constant; compare the intended before/after builds.
Performance checks still use the project's existing Vulkan 1x/SSAA 1 criteria. Never measure game timing with PNG dumps enabled.

## Files and results

Supported inputs: static **8-bit RGB/RGBA PNG** (including Adam7 and RGB transparent-colour entries) and
**binary P6 PPM with maxval 255**, as written by the replay renderer. Inputs must have equal dimensions.
Palette/grey/16-bit PNG, APNG (including one frame), other PNM modes, truncated/invalid files and trailing
P6 images/data are errors. Each input is bounded to 64 MiB encoded and 64 MiB decoded RGBA; these are
per-input guards, not a promise about total process memory.

RGBA means red, green, blue and opacity. RGB gains alpha 255 unless PNG transparency supplies another value.
Alpha-only differences and stored colours under alpha zero count. PPM discards source alpha, so matching PPMs
cannot prove equality of discarded alpha. No resize, alignment, rotation or colour-profile/gamma transform is applied.

`--out` must name a **new directory**. Existing directories, files and aliases are refused. Parent directories
are created as needed. Inputs are never overwritten. `report.json` is published atomically last and marks
completion. A failed write returns 2; any partial files without the completed report are incomplete output.

The JSON report includes file and decoded-pixel SHA-256 hashes, source format/mode/alpha origin, dimensions,
tool/Python/Pillow identity, exact changed count/fraction, first changed coordinate, inclusive bounds and
maximum absolute deltas for all four channels. Coordinates start at top-left `(0,0)` in row order.
Identical images have null changed coordinates/bounds and zero changed count/deltas.

`--heatmap` adds `difference.png`: changed pixels opaque magenta, unchanged pixels opaque black.
The visualization never changes the exact result. There are no tolerance, ignored-alpha, resizing or overwrite options.

## Optional capture context

`--context context.json` checks declared capture correspondence. Without it, a raw pixel comparison is allowed
but explicitly labelled **unverified**. Even matching declarations cannot prove equivalent game state.
Supplied stale, malformed or incompatible context returns 2 rather than silently switching to a raw comparison.

Example structure (replace the hashes and capture details with actual recorded values):

```json
{
  "reference": {
    "file_sha256": "0000000000000000000000000000000000000000000000000000000000000000",
    "build_id": "before", "scene_id": "saved-replay", "frame_index": 25,
    "scale": 1, "ssaa": 1, "capture_kind": "presented",
    "capture_options": {"backend": "vulkan", "weave": true, "fb": null, "stop_at": null, "skip": []}
  },
  "candidate": {
    "file_sha256": "1111111111111111111111111111111111111111111111111111111111111111",
    "build_id": "after", "scene_id": "saved-replay", "frame_index": 25,
    "scale": 1, "ssaa": 1, "capture_kind": "presented",
    "capture_options": {"backend": "vulkan", "weave": true, "fb": null, "stop_at": null, "skip": []}
  }
}
```

File hashes are checked against each input's consumed snapshot; they need not match each other.
Build IDs may differ. Scene/frame/scale/SSAA/output kind/options must match. Types are checked; booleans
are not accepted as integer frame/scale values. Backend names are `vulkan` or `cpu`; reading a backend label runs no renderer.

For **presented** output, `weave` declares the producer's FRAME-mode policy: `PS2X_NO_WEAVE` unset, empty or
starting with `0` means true; otherwise false. This does not prove that a particular frame was woven.
For **framebuffer** output, `weave` must be false, meaning not applicable regardless of the producer setting.
The utility never infers these settings from its own environment or the image filename.

Replay options use JSON keys `fb` (`null` or `[fbp, fbw, width, height]`), `stop_at` (`null` or a nonnegative
record index), and `skip` (inclusive `[start,end]` ranges, empty if none). Supply all three together with
actual choices/defaults. Framebuffer output requires the actual `fb` selection. Unknown settings are errors.
Optional `replay_input_sha256` goes in both records and must match; supplying it also requires the complete
replay-option set. No structured machine/GPU/driver labels are added to this schema.
