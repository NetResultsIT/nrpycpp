# Building nrPyCpp and its Python dependency

Contents:
[1. Overview](#1-overview) ·
[2. The python dependency (deps/python)](#2-the-python-dependency-depspython) ·
[3. Building the library](#3-building-the-library) ·
[4. Tests](#4-tests) ·
[5. The generated wrapper](#5-the-generated-wrapper) ·
[6. CI](#6-ci) ·
[7. Upgrading Python](#7-upgrading-python)

---

## 1. Overview

```
python-build-standalone release ──tools/prepare_python_runtime.py──▶ artifactory deps/python/release/<ver>/<os>/
                                                                           │
                         NRPYCPP_PYTHON_ROOT (or depspath.pri) ◀───────────┘
                                         │
                       lib/nrPyCpp.pro ──┴──▶ lib/last_build/ (libnrPyCpp + headers + libpython)
                                                     │
                     tests/nrPyCppTests.pro ─────────┴──▶ tst_nrpycpp
```

Requirements:

| | macOS | Linux | Windows |
|---|---|---|---|
| Qt | 6.8 (`macos` kit, universal) | 6.8 `gcc_64` | 6.8 `msvc2022_64` |
| compiler | Xcode (26 tested) | gcc with C++17 | Visual Studio 2022 (or 2019) with the x64 C++ tools |
| other | python3 >= 3.8 to run the tools (the system one is fine) | same | same, only to prepare packages |

---

## 2. The python dependency (deps/python)

### Where it comes from

[python-build-standalone](https://github.com/astral-sh/python-build-standalone) (by Astral, the authors
of `uv`) publishes relocatable CPython builds for every platform, with headers and libraries, and a
`SHA256SUMS` file per release. We use the `install_only_stripped` archives, prune them and republish them
on artifactory, so that builds never depend on github or on a python installed on the machine.

| platform key | upstream archive(s) | artifactory path (repo `bblocks`) | size |
|---|---|---|---|
| `macos` | `aarch64-apple-darwin` + `x86_64-apple-darwin`, merged into universal binaries | `deps/python/release/<ver>/macos/python-<ver>-macos.tgz` | 17 MB (45 MB unpacked) |
| `linux64_Ubuntu_22.04` | `x86_64-unknown-linux-gnu` (glibc >= 2.17) | `deps/python/release/<ver>/linux64/python-<ver>-linux64_Ubuntu_22.04.tgz` | 13 MB (42 MB) |
| `win64_vs2019` | `x86_64-pc-windows-msvc` | `deps/python/release/<ver>/win64/python-<ver>-win64_vs2019.zip` | 10 MB (31 MB) |

Sizes are for 3.13.18. The Windows build is made with a newer MSVC than VS2019, which does not matter
for a C library using the `vcruntime140` runtime; the `vs2019` suffix is there because VDK's `nr_co_deps`
selects the Windows packages by it. There is no debug package: debug builds of nrPyCpp link the release
python (see the troubleshooting table in [USAGE.md](USAGE.md#9-troubleshooting)).

Package layout (one top folder):

```
python/
  NRPYCPP_PYTHON.json         version, upstream release, archives and their sha256
  include/python3.13/         (windows: include/)          headers, used to build nrPyCpp only
  lib/libpython3.13.dylib     (linux: lib/libpython3.13.so[.1.0]; windows: python313.dll, python3.dll,
                               vcruntime140*.dll, libs/python313.lib)
  lib/python3.13/             (windows: Lib/ and DLLs/)    the standard library = the python home
```

What is removed from upstream: tcl/tk and `tkinter`, `idlelib`, `turtledemo`, `ensurepip`, `pydoc_data`,
the stdlib tests, the `_test*` extension modules, the dbm backends, `pip` (site-packages is empty),
`bin/` and the executables, `share/`, `config-3.13-*`, the `__pycache__` folders.
On macOS the binaries are merged with `lipo`, re-signed ad hoc, checked for both architectures and for the
absence of `LC_RPATH`, and `pyconfig.h` becomes a small header including `pyconfig-arm64.h` or
`pyconfig-x86_64.h` (the only header that differs).

### Preparing the packages

```sh
# exact version (recommended, reproducible): python 3.13.18 from the 20261026 release
for p in macos linux64_Ubuntu_22.04 win64_vs2019; do
    python3 tools/prepare_python_runtime.py prepare --platform $p --python 3.13.18 --release 20261026 --out build/python
done
# or the newest 3.13.x of the latest release
python3 tools/prepare_python_runtime.py prepare --platform macos --out build/python
```

All three platforms can be prepared on a Mac (`macos` needs a Mac for `lipo` and `codesign`; the
others work anywhere). The script:

1. resolves the version through the github API (skipped when `--python X.Y.Z --release` are given);
2. downloads into `<out>/downloads/` (kept, a second run reuses them) and checks each archive against the
   release `SHA256SUMS`;
3. extracts safely (no absolute paths, `..`, links leaving the folder or special files) into
   `<out>/work/<platform>/`, then prunes, merges (macOS) and verifies;
4. writes the package into `<out>/upload/deps/python/release/<ver>/<os>/`, the same tree as on artifactory.

`--revision <n>` produces `python-<ver>-<platform>-<n>.<ext>`, for a rebuild of a version already
published (published files are never replaced, see below).

### Testing the packages before publishing

Unpack each package and run the test suite against it, on each platform ([section 4](#4-tests)):

```sh
mkdir -p build/check && tar xzf build/python/upload/deps/python/release/3.13.18/macos/python-3.13.18-macos.tgz -C build/check
NRPYCPP_PYTHON_ROOT=$PWD/build/check/python QMAKE=~/Qt/6.8.6/macos/bin/qmake tests/run_tests.sh
```

On a Mac also check the x86_64 half, building universal and running under Rosetta (`arch` drops
`DYLD_*` variables, pass them with `-e`):

```sh
qmake lib/nrPyCpp.pro "QMAKE_APPLE_DEVICE_ARCHS=x86_64 arm64" && make release      # in a build folder
qmake tests/nrPyCppTests.pro CONFIG+=release "QMAKE_APPLE_DEVICE_ARCHS=x86_64 arm64" && make
arch -x86_64 -e DYLD_LIBRARY_PATH=$PWD/lib/last_build -e DYLD_FRAMEWORK_PATH=$QT/lib ./tst_nrpycpp
```

### Publishing on artifactory

```sh
python3 tools/prepare_python_runtime.py upload --out build/python --dry-run   # checks the paths, uploads nothing
python3 tools/prepare_python_runtime.py upload --out build/python
```

- Credentials: `--dry-run` reads with `~/.nr/artifactory_api_key`; the upload needs the token with write
  rights in `~/.nr/arti_adm_token.txt` (sent as `Authorization: Bearer`).
- For every file under `<out>/upload` it first queries `api/storage`: a free path is uploaded with its
  `X-Checksum-Sha1` and verified afterwards (size and sha1); an identical file already there is skipped;
  **a different file already there stops the upload**. Prepare a `--revision` instead of overwriting:
  every clean `nr_co_deps` fetch picks up what is published.
- Nothing is ever deleted by the script.

### Using a package

`nr_co_deps` (see the JSON entry in [USAGE.md](USAGE.md#1-adding-it-to-an-application)) or by hand:

```sh
curl -H "X-JFrog-Art-Api: $(cat ~/.nr/artifactory_api_key)" -O \
  https://nrreleases.jfrog.io/artifactory/bblocks/deps/python/release/3.13.18/macos/python-3.13.18-macos.tgz
tar xzf python-3.13.18-macos.tgz            # creates python/
```

---

## 3. Building the library

### Telling qmake where python is

Either export the variables (they take precedence, handy for CI):

```sh
export NRPYCPP_PYTHON_ROOT=/path/to/python      # the python/ folder of the package
export NRPYCPP_PYTHON_VERSION=3.13              # optional, default 3.13
```

or copy `lib/depspath.pri.sample` to `lib/depspath.pri` (git-ignored) and set `PYTHON_ROOT` and
`PYTHON_VERSION` there. A legacy setup with `PYTHON_INCLUDE_PATH`, `PYTHON_LIB_PATH` and
`PYTHON_VERSION` (e.g. Xcode's `Python3.framework`) still works, but the python home must then be
provided at runtime.

`PYTHON_ROOT` is also written into the build (`nrpycpp_build_config.h` in the build folder) as the last
fallback for the python home, so development builds and the tests run without any further setup.
Deployed applications should not rely on it (see [USAGE.md](USAGE.md#7-python-home-and-virtual-environments)).

### Building

```sh
mkdir -p build/lib && cd build/lib
~/Qt/6.8.6/macos/bin/qmake ../../lib/nrPyCpp.pro
make release            # or debug, or both (debug_and_release)
```

Windows, in a "x64 Native Tools Command Prompt for VS 2022":

```bat
set NRPYCPP_PYTHON_ROOT=C:\deps\python
mkdir build\lib && cd build\lib
C:\Qt\6.8.6\msvc2022_64\bin\qmake ..\..\lib\nrPyCpp.pro
nmake release
```

The result is copied into `lib/last_build/`:

| | macOS | Linux | Windows |
|---|---|---|---|
| library | `libnrPyCpp.2.0.0.dylib` + links (`libnrPyCpp_debug.*` for debug) | `libnrPyCpp.so.2.0.0` + links (`_d`) | `nrPyCpp.dll/.lib/.pdb` (`nrPyCppd.*`) |
| python | `libpython3.13.dylib` | `libpython3.13.so*` | `python313.dll` |
| headers | `include/PyCall.h`, `include/PyRunnerQt.h`, `include/nrPyCppConfig.h` | same | same |

Notes:

- Debug builds on Windows link the **release** `python313.lib`: `pyrunner.h` hides `_DEBUG` while
  including `Python.h`, otherwise `pyconfig.h` would require `python313_d.lib` and a debug CPython.
  Define `NRPYCPP_PYTHON_DEBUG` to build against a debug CPython instead.
- With Xcode 26 and Qt 6.8, Qt's `qyieldcpu.h` does not compile (`implicitly declaring library function
  '__yield'`): the `.pro` files add `-Wno-error=implicit-function-declaration` on macOS.
- The macOS post-link step copies the python dylib as it is (it already has `@rpath/libpython3.13.dylib`
  as install name and a valid signature). With the legacy Xcode python it changes the install name and
  re-signs the copy; never skip the re-sign, an arm64 dylib with a broken signature is killed at load.
- The library's macOS install name is a bare `libnrPyCpp.2.dylib` (unchanged from the previous
  versions); consumers fix it when bundling.

---

## 4. Tests

| | what | how |
|---|---|---|
| `tests/tst_nrpycpp.cpp` | QtTest suite of the library: conversions in both directions, edge values, errors and tracebacks, recovery after errors, bundled stdlib (ssl, hashlib, ctypes...), sync latency, sync calls from 4 threads, the `nrpycpp` module (messages, per-runner output routing with two runners, threads, thread exceptions, `_thread` threads, `PyCallBackIface`, runner destroyed while a thread prints), virtual environment, broken and missing scripts, async calls, re-creating runners, missing python home and venv for another version (both in child processes) | `tests/run_tests.sh` / `tests\run_tests.ps1` |
| `tests/tools/test_prepare_python_runtime.py` | unit tests of the packaging tool: safe extraction, pruning of the unix and windows layouts, version resolution, package layout, upload never overwriting (network mocked) | `python3 -m unittest discover -s tests/tools -v` |
| `tester/` | the original interactive sample | build `tester/nrPyCppTester.pro`, run from its build folder |

The runner scripts build the library (into `lib/last_build`) and the tests, then run `tst_nrpycpp`.
Extra arguments go to the test executable (e.g. `-v2`, or a test function name).

```sh
NRPYCPP_PYTHON_ROOT=/path/to/python QMAKE=~/Qt/6.8.6/macos/bin/qmake [CONFIG=debug] tests/run_tests.sh [build dir] [test args]
```

```powershell
$env:NRPYCPP_PYTHON_ROOT = "C:\deps\python"; $env:QTDIR = "C:\Qt\6.8.6\msvc2022_64"
powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1 [-BuildDir C:\build\nrpycpp-tests] [-Config debug]
```

`run_tests.ps1` finds Visual Studio with `vswhere` (or uses `$env:VSDEVCMD`), sets up the x64 MSVC
environment itself and uses `jom` if it is in `PATH`, `nmake` otherwise.

Verified on 2026-11-10 with python 3.13.18 (20261026) and Qt 6.8.6:

| platform | result | sync call |
|---|---|---|
| macOS 26 arm64, universal package | 34/34 | 12-14 µs |
| macOS x86_64 (Rosetta), universal package | 34/34 | 13 µs |
| Windows 11 ARM64 VM, x64 emulated, MSVC 2022, release and debug | 34/34 | 63 µs |
| Linux | suite not run yet (no Qt image); the package was checked in `ubuntu:22.04` x86_64 (docker, emulated) with a C program embedding it isolated (stdlib imports incl. ssl/ctypes/sqlite3, no RPATH, only glibc dependencies) | |

Published on 2026-11-10: `deps/python/release/3.13.18/{macos,linux64,win64}/`, resolved by `nr_co_deps`
for the three platform keys.

---

## 5. The generated wrapper

`lib/PyRunnerQt.h/.cpp` (the public class) are generated by wrappergen from the methods between
`//START_WRAPPER_METHODS` and `//END_WRAPPER_METHODS` (and the signals between the `SIGNAL` markers) in
`lib/pyrunner.h`, with `lib/nrpycpp_wrapper_conf.json`. After changing those methods, from `lib/`:

```sh
python3 ~/prog/git-scripts/wrappergen2/wrappergen.py -c nrpycpp_wrapper_conf.json          # regenerate
python3 ~/prog/git-scripts/wrappergen2/wrappergen.py -c nrpycpp_wrapper_conf.json --check  # verify, CI
```

The doc comments of the wrapper are taken from `pyrunner.cpp`. `nrPyCppConfig.h` is hand written.

---

## 6. CI

- Fetch the `deps/python` package of the platform, unpack it and export `NRPYCPP_PYTHON_ROOT` (this
  overrides any `depspath.pri`, so `depspath.pri.teamcity` is not needed with it).
- Build and test with `tests/run_tests.sh` / `tests\run_tests.ps1`, then package `lib/last_build` as before.
- Run `wrappergen --check` and the tools unit tests.

---

## 7. Upgrading Python

1. Pick the new version and release on python-build-standalone (a new patch release: just a new
   version; a new minor, e.g. 3.14: also update `NRPYCPP_PYTHON_VERSION`/`PYTHON_VERSION` defaults in
   `lib/nrPyCpp.pro`, the docs, and tell the users to recreate their virtual environments).
2. Prepare the three packages, run the tests on the three platforms against them.
3. Publish them, then bump the `deps/python` version in the VDK deps JSON files and rebuild nrPyCpp
   (the python shared library name contains the minor version: an application must ship the libpython
   nrPyCpp was built against).
