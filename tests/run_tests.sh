#!/bin/bash
# Build nrPyCpp and its test suite against a python runtime, then run the tests (macOS, Linux).
#
#   NRPYCPP_PYTHON_ROOT=/path/to/python QMAKE=~/Qt/6.8.6/macos/bin/qmake tests/run_tests.sh [build dir]
#
# NRPYCPP_PYTHON_ROOT: the "python" folder of a deps/python package (see docs/BUILDING.md)
# QMAKE: the qmake to use (default: qmake in PATH); JOBS: parallel jobs (default: number of cpus)
# CONFIG: release (default) or debug
# The library is installed into lib/last_build as by a normal build.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(dirname "$HERE")"
BUILD="${1:-$REPO/build-tests}"
QMAKE="${QMAKE:-qmake}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN)}"
CONFIG="${CONFIG:-release}"
: "${NRPYCPP_PYTHON_ROOT:?set NRPYCPP_PYTHON_ROOT to the python runtime folder}"
export NRPYCPP_PYTHON_ROOT

mkdir -p "$BUILD/lib" "$BUILD/tests"
(cd "$BUILD/lib" && "$QMAKE" "$REPO/lib/nrPyCpp.pro" && make -j"$JOBS" "$CONFIG")
(cd "$BUILD/tests" && "$QMAKE" "$REPO/tests/nrPyCppTests.pro" CONFIG+="$CONFIG" && make -j"$JOBS")

QT_LIBS="$("$QMAKE" -query QT_INSTALL_LIBS)"
# the library has a bare install name (libnrPyCpp.1.dylib) on macOS, so it is found through the library path
export DYLD_LIBRARY_PATH="$REPO/lib/last_build${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
export DYLD_FRAMEWORK_PATH="$QT_LIBS${DYLD_FRAMEWORK_PATH:+:$DYLD_FRAMEWORK_PATH}"
export LD_LIBRARY_PATH="$REPO/lib/last_build:$QT_LIBS${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# the suite configures the interpreter itself, do not let the caller's settings leak in
unset NRPYCPP_PYTHONHOME NRPYCPP_VENV
"$BUILD/tests/tst_nrpycpp" "${@:2}"
