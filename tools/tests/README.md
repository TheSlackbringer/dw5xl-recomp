# Synthetic recompiler checks

These public fixtures use authored MIPS instructions, not game files. The native-boundary
checks run the actual CLI, compile its registered generated C++ and check independent
register, memory, PC, link and exception-state expectations. The C++ target also links the
production runtime and exercises calls, overlap and shared/disjoint continuations.

Build the upstream project with `PS2X_BUILD_RECOMP`, `PS2X_BUILD_RUNTIME`,
`PS2X_BUILD_ANALYZER` and `PS2X_BUILD_TEST` enabled. Reuse an existing approved toolchain
and dependency cache. Synthetic tests need no game, GPU, admin rights or profiler.

```sh
cmake --build external/PS2Recomp/build --target ps2_recomp ps2x_tests ps2x_boundary_native_tests
python3 -m unittest discover -s tools/tests -p 'test_*.py' -v
external/PS2Recomp/build/ps2xTest/ps2x_boundary_native_tests
(cd external/PS2Recomp && build/ps2xTest/ps2x_tests)
```

For another build directory set `PS2X_TEST_RECOMPILER` to its native `ps2_recomp`
executable. `CXX` selects a GCC/Clang-style native compiler command (default `clang++`);
on Windows use the existing native Clang/MinGW toolchain, not clang-cl flags. The new
C++ CTest is `native_function_boundaries` in the upstream build's `ps2xTest` directory.
The upstream unit executable needs the working directory above for an existing fixture.

Run these checks after boundary/entry/branch emission changes and as fast regression
checks. Real-disc map qualification is a separate targeted investigation: see
`tools/debug/validate_function_map.md`. Successful synthetic cases do not establish
whole-game equivalence, arbitrary indirect-target completeness or FPS improvements.
