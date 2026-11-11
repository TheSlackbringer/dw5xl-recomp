"""Where the tools' tests find the recompiler: one answer for all of them.

PS2X_TEST_RECOMPILER wins (CI sets it). Otherwise the first that exists of the folders it is built in:
external/PS2Recomp/build (README.md here) and build-recomp (CI's folder, also used on some PCs). Before 2026-11-11
three tests looked only in build-recomp and two only in external/PS2Recomp/build, so the same PC passed some and
failed others with "Build/set PS2X_TEST_RECOMPILER"."""
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NAME = 'ps2_recomp' + ('.exe' if os.name == 'nt' else '')
CANDIDATES = (ROOT / 'external/PS2Recomp/build/ps2xRecomp' / NAME,
              ROOT / 'build-recomp/ps2xRecomp' / NAME)


def recompiler():
    """The recompiler to test with. When none is built: the README's path, so the tests' own message names it."""
    chosen = os.environ.get('PS2X_TEST_RECOMPILER')
    if chosen:
        return Path(chosen)
    for path in CANDIDATES:
        if path.is_file():
            return path
    return CANDIDATES[0]
