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
    if [ "${REBUILD_IMAGE:-0}" = "1" ]; then
        docker build -f "$REPO/bindings/Dockerfile" -t "$IMAGE" "$REPO"
    fi
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
  /work/src/view/CodeEditor.hpp /work/src/view/CodeEditorOptions.hpp \
  /work/bindings/typesystem_qmn.xml
echo "=== generated files ==="
find /tmp/gen -type f | sort
echo "=== rejected functions ==="
cat /tmp/gen/mjb_rejected_functions.log 2>/dev/null || true
'
    exit 0
fi

if [ "${1:-}" = "--wheel" ]; then
    # scikit-build-core path: wheel from pyproject.toml, auditwheel-repaired
    # (Phase F), installed into a scratch location, then smoke-tested with the
    # repo off sys.path entirely.
    if [ "${REBUILD_IMAGE:-0}" = "1" ]; then
        docker build -f "$REPO/bindings/Dockerfile" -t "$IMAGE" "$REPO"
    fi
    run '
mkdir -p /work/dist && rm -f /work/dist/qmn_native-*.whl
/opt/venv/bin/pip wheel --no-build-isolation --no-deps -w /work/dist /work
RAW=$(ls /work/dist/qmn_native-*.whl)
echo "raw wheel:     $RAW"
# Tripwire: the abi3 promise (wheel.py-api = "cp310" in pyproject.toml).
case "$RAW" in
    *-cp310-abi3-*) ;;
    *) echo "FATAL: wheel is not cp310-abi3: $RAW" >&2; exit 1 ;;
esac
# Guard: a leftover from an earlier partial install would make the smoke pass
# for the wrong reason - qmn must be unimportable until the wheel is installed.
if /opt/venv/bin/python -c "import qmn" 2>/dev/null; then
    echo "FATAL: qmn already importable from the base venv; refusing to test" >&2
    exit 1
fi

echo "=== auditwheel show (pre-repair verdict) ==="
auditwheel show "$RAW" || true
# Repair pinned to manylinux_2_34: PySide6 6.12 own tag (the Qt_6.12 versioned
# refs of the wrappers only resolve against the Qt it bundles, and that Qt
# needs glibc >= 2.34; the extension ELF itself only references up to
# GLIBC_2.14 - measured - so the floor is the dependency, not our code).
# Excludes: Qt + its ICU subtree (pruned), the pyside/shiboken 6.12 SONAMEs -
# all provided by the pinned PySide6 at runtime, grafting copies would load a
# second Qt and violate the one-Qt-at-runtime rule - plus libGLX/libOpenGL,
# host-provided libglvnd exactly like PySide6 needs it. Keep in sync with
# [tool.cibuildwheel.linux] repair_command in pyproject.toml.
mkdir -p /work/dist/repaired && rm -f /work/dist/repaired/*.whl
auditwheel repair --plat manylinux_2_34_x86_64 -w /work/dist/repaired \
    --exclude libQt6Core.so.6 --exclude libQt6Gui.so.6 \
    --exclude libQt6Widgets.so.6 --exclude libQt6DBus.so.6 \
    --exclude libpyside6.abi3.so.6.12 --exclude libshiboken6.abi3.so.6.12 \
    --exclude libGLX.so.0 --exclude libOpenGL.so.0 \
    "$RAW"
WHL=$(ls /work/dist/repaired/*.whl)
echo "repaired wheel: $WHL"
# Tripwire: abi3 python tag + the pinned manylinux baseline in the name.
case "$WHL" in
    *-cp310-abi3-*manylinux_2_34_x86_64.whl) ;;
    *) echo "FATAL: repaired wheel has unexpected tag: $WHL" >&2; exit 1 ;;
esac
echo "=== repaired wheel contents ==="
/opt/venv/bin/python -m zipfile -l "$WHL"
/opt/venv/bin/pip install --target /tmp/pylibs --force-reinstall --no-deps "$WHL"
echo "=== smoke test against repaired wheel (cwd=/tmp) ==="
cd /tmp
PYTHONPATH=/tmp/pylibs /opt/venv/bin/python /work/bindings/tests/smoke_wheel.py
echo "=== qtmonaco parity test against repaired wheel (cwd=/tmp) ==="
PYTHONPATH=/tmp/pylibs /opt/venv/bin/python /work/bindings/tests/parity_monaco.py
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

echo "=== qtmonaco parity test (build tree) ==="
PYTHONPATH=/work/bindings/python:/work/build-py/bindings \
  /opt/venv/bin/python /work/bindings/tests/parity_monaco.py
'
