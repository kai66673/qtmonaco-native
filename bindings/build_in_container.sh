#!/usr/bin/env bash
# Build the Shiboken6/PySide6 extension inside the container environment.
#
# Why a container: Shiboken codegen cannot run on the Arch host (the pip
# generator needs libxml2.so.2, see AGENTS.md), and PySide6 wheels carry their
# own Qt, so the extension is compiled against that Qt (bindings/Dockerfile
# pins PySide6 6.12.0 + the matching Qt 6.12.0 from the mirror archives).
#
#   ./bindings/build_in_container.sh              # image + configure + build + smoke
#   ./bindings/build_in_container.sh --codegen    # generator only, inspect output
set -euo pipefail

IMAGE=${IMAGE:-qmn-binding:0}
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

# --user keeps build artifacts in the bind mount owned by the invoking user.
run() {
    docker run --rm \
        -v "$REPO":/work -w /work \
        --user "$(id -u):$(id -g)" \
        -e HOME=/tmp \
        "$IMAGE" bash -euo pipefail -c "$1"
}

if [ "${1:-}" = "--codegen" ]; then
    docker build -f "$REPO/bindings/Dockerfile" -t "$IMAGE" "$REPO"
    # Exactly the CLI the Shiboken6 CMake macro runs (Shiboken6ToolsMacros.
    # cmake), reproduced by hand so the generated names and the rejected-API
    # log can be inspected before any compiling happens.
    run '
SP=$(/opt/venv/bin/python -c "import sysconfig;print(sysconfig.get_paths()[\"purelib\"])")
mkdir -p /tmp/gen
"$SP/shiboken6_generator/shiboken6" \
  --generator-set=shiboken \
  --enable-parent-ctor-heuristic \
  --enable-return-value-heuristic \
  --use-isnull-as-nb_nonzero \
  --avoid-protected-hack \
  --enable-pyside-extensions \
  -I/work/bindings -T/work/bindings \
  -I"$QTDIR/include" -I"$QTDIR/include/QtWidgets" \
  -I"$QTDIR/include/QtGui" -I"$QTDIR/include/QtCore" \
  -I/work/src \
  -T"$SP/PySide6/typesystems" \
  --output-directory=/tmp/gen \
  /work/src/view/CodeEditor.hpp /work/bindings/typesystem_qmn.xml
echo "=== generated files ==="
find /tmp/gen -type f | sort
echo "=== rejected functions ==="
cat /tmp/gen/mjb_rejected_functions.log 2>/dev/null || true
'
    exit 0
fi

if [ "${1:-}" = "--wheel" ]; then
    # scikit-build-core path: full wheel from pyproject.toml, installed into a
    # scratch location, then smoke-tested with the repo off sys.path entirely.
    if [ "${REBUILD_IMAGE:-0}" = "1" ]; then
        docker build -f "$REPO/bindings/Dockerfile" -t "$IMAGE" "$REPO"
    fi
    run '
mkdir -p /work/dist && rm -f /work/dist/qmn_native-*.whl
/opt/venv/bin/pip wheel --no-build-isolation --no-deps -w /work/dist /work
WHL=$(ls /work/dist/qmn_native-*.whl)
# Guard: a leftover from an earlier partial install would make the smoke pass
# for the wrong reason - qmn must be unimportable until the wheel is installed.
if /opt/venv/bin/python -c "import qmn" 2>/dev/null; then
    echo "FATAL: qmn already importable from the base venv; refusing to test" >&2
    exit 1
fi
echo "=== wheel contents ==="
/opt/venv/bin/python -m zipfile -l "$WHL"
/opt/venv/bin/pip install --target /tmp/pylibs --force-reinstall --no-deps "$WHL"
echo "=== smoke test against installed wheel (cwd=/tmp) ==="
cd /tmp
PYTHONPATH=/tmp/pylibs /opt/venv/bin/python /work/bindings/tests/smoke_wheel.py
'
    exit 0
fi

# Rebuild the image only when told to (it is slow: Qt download + py7zr).
if [ "${REBUILD_IMAGE:-0}" = "1" ]; then
    docker build -f "$REPO/bindings/Dockerfile" -t "$IMAGE" "$REPO"
fi

run '
cmake -S /work -B /work/build-py -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DQMN_BUILD_PYTHON=ON \
  -DPython_EXECUTABLE=/opt/venv/bin/python
cmake --build /work/build-py

echo "=== extension ==="
find /work/build-py/bindings -name "*.so" -printf "%p (%s bytes)\n"

echo "=== smoke test ==="
PYTHONPATH=/work/bindings/python /opt/venv/bin/python /work/bindings/tests/smoke_qmn.py
'
