"""Phase B definition of done (see PLAN.md), run against the build tree.

Proves the binding end to end at the level of state, not pixels: the module
imports, a CodeEditor is constructible offscreen, text round-trips, the bound
API behaves, and a Qt signal reaches a Python callable. It also asserts the
invariant this whole spike exists to measure: exactly ONE QtCore in the
process (smoke test details in _smoke_checks.py).

Run from the container:  bindings/build_in_container.sh
"""

import sys
from pathlib import Path

_here = Path(__file__).resolve().parent
_repo = _here.parent.parent
sys.path.insert(0, str(_here))                             # _smoke_checks
sys.path.insert(0, str(_repo / "bindings" / "python"))     # the qmn/ shim package
_ext_dir = _repo / "build-py" / "bindings"                 # where _qmn.abi3.so lands
if _ext_dir.is_dir():
    sys.path.insert(0, str(_ext_dir))

from _smoke_checks import run_checks  # noqa: E402

if __name__ == "__main__":
    sys.exit(run_checks())
