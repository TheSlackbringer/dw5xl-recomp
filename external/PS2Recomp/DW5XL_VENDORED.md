# Vendored PS2Recomp (modified)

This directory is a modified copy of [PS2Recomp](https://github.com/ran-j/PS2Recomp) (GPL-3.0, see `LICENSE`), vendored into the DW5XL port on 2026-10-13. It is not the unmodified upstream release.

- Upstream base: `ran-j/PS2Recomp` commit `c5a9d02573410a2085a4b4b831b0b68ba3515440` (2026-09-30).
- Applied on top: the PS2Recomp patch series kept in `../../vendor/runtime-patches/` (provenance in `../../THIRD_PARTY.md`), upstream pull requests #271 and #274, and the DW5XL runtime changes (VIF1 DIRECT/GIF PATH2 handling, libmpeg end-of-stream, PSS audio sub-stream filtering, memory card directory count, IOP SET_SREG forwarding, `sceCdBreak`, `GetSystemTimeLow`, PAL/NTSC cadence selection and others).
- Changes after vendoring are recorded in this repository's Git history.

Build directories (`build/`) are local and ignored.
