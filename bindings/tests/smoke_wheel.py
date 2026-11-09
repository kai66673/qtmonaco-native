"""Phase B checks (see _smoke_checks.py) against the INSTALLED wheel: nothing
here touches the repository layout, so a green run proves the packaging works.

Run from the container:  bindings/build_in_container.sh --wheel
"""

import sys

from _smoke_checks import run_checks

if __name__ == "__main__":
    sys.exit(run_checks())
