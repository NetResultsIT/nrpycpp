---
name: nrpycpp-python-deps
description: Prepare, test and publish the embedded CPython runtime of nrPyCpp (package deps/python, from python-build-standalone, for macOS universal, Linux x86_64, Windows x64), build nrPyCpp against it and run its test suite on macOS, Linux and Windows. Use when asked to update, upgrade or publish the python dependency, build or test nrPyCpp, or debug python embedding issues (python home, venv, signatures, debug builds).
---

# nrPyCpp python dependency, build and tests

Full documentation: `docs/BUILDING.md` (build, packages, publishing, tests) and `docs/USAGE.md` (API,
deployment, troubleshooting). Read the relevant section before changing anything.

## Local environment
Values of the maintainer's machine are the defaults; set your own.
- `QT_ROOT`: Qt 6.8 install (default `~/Qt/6.8.6`); the macOS qmake is `$QT_ROOT/macos/bin/qmake`.
- `NR_SCRIPTS`: checkout of the NetResults scripts, with `wrappergen2/wrappergen.py` and
  `nr_checkout_deps/nr_co_deps.py` (no default; ask the maintainer).
- Credentials of the package server: where they are is in `docs/BUILDING.md` section 2, "Publishing"; never print them.

## 1. Versions
- Python **3.13** (3.10 and older are EOL). Source: python-build-standalone (Astral), `install_only_stripped`.
- Pin both the version and the release (`--python 3.13.18 --release 20261026`): "latest" is only for
  looking up what exists. What is published, and when: `baselines.md`. `nr_co_deps` unpacks into
  `<rel_dest_dir>/<platform>/python/`.
- A new minor (3.14) also means: defaults in `lib/nrPyCpp.pro` (`PYTHON_VERSION = 3.13`), docs, users
  recreate their venvs, rebuild nrPyCpp (libpython's name has the minor version).

## 2. Prepare the packages (`tools/prepare_python_runtime.py`, stdlib only)
```sh
for p in macos linux64_Ubuntu_22.04 win64_vs2019; do
  python3 tools/prepare_python_runtime.py prepare --platform $p --python 3.13.18 --release 20261026 --out <work>
done
```
- `macos` merges `aarch64-apple-darwin` + `x86_64-apple-darwin` (lipo, ad hoc re-sign, dispatching
  `pyconfig.h`, checks both archs and no `LC_RPATH`): needs a Mac. The other two run anywhere.
- Every archive is checked against the release `SHA256SUMS`; downloads are cached in `<work>/downloads`.
- Output tree = package server tree: `<work>/upload/deps/python/release/<ver>/{macos,linux64,win64}/python-<ver>-<platform>.{tgz,zip}`, one top folder `python/` with `NRPYCPP_PYTHON.json`.
- Pruning list and package layout: `docs/BUILDING.md` section 2. If a check fails (e.g. a new extension
  module with an rpath), fix the script, do not hand-edit the package. Keep `tests/tools` passing:
  `python3 -m unittest discover -s tests/tools -v`.

## 3. Test before publishing (do not skip)
Unpack each package and run the suite against it: `NRPYCPP_PYTHON_ROOT=<dir>/python QMAKE=<qt>/bin/qmake tests/run_tests.sh` (all tests must pass; the count is in `baselines.md`).
- **macOS**: also the x86_64 half: build lib and tests with `"QMAKE_APPLE_DEVICE_ARCHS=x86_64 arm64"`, run
  `arch -x86_64 -e DYLD_LIBRARY_PATH=<repo>/lib/last_build -e DYLD_FRAMEWORK_PATH=<qt>/lib ./tst_nrpycpp`
  (`arch` drops `DYLD_*` without `-e`). Qt kit: `$QT_ROOT/macos`.
- **Windows** (x64 MSVC; Qt `C:\Qt\6.8.6\msvc2022_64`, Visual Studio 2022 with the x64 tools):
  `tests\run_tests.ps1` (`-Config debug` too: it checks that debug builds link the release python).
  Several people may share one Windows machine or VM: agree on a lock with the others before using it, and never
  break someone else's. For a VM whose remote execution hangs on long commands, start the real script detached,
  make it write its exit code to a file, and poll the file with short commands.
  Zip the working tree without `.git`, the binaries in `lib/last_build`, `lib/depspath.pri` and build folders.
- **Linux**: the Qt suite may not have been run (see `baselines.md`). At least check the package: in
  `docker run --platform linux/amd64 ubuntu:22.04`, build a small C program with `PyConfig_InitIsolatedConfig` +
  `config.home` against `python/include/python3.13` and `-lpython3.13`, import `ssl ctypes sqlite3 zlib hashlib`,
  and `readelf -d` libpython (no RPATH/RUNPATH).

## 4. Publish (needs explicit user go-ahead for this upload)
```sh
python3 tools/prepare_python_runtime.py upload --out <work> --dry-run   # reads the key, uploads nothing
python3 tools/prepare_python_runtime.py upload --out <work>             # needs the token with write rights
```
- Repo `bblocks` of the package server; key and token files are named in `docs/BUILDING.md` section 2. Never print the token.
- The script GETs `api/storage` for each path: free: PUT with `X-Checksum-Sha1`, then verify size and sha1;
  identical: skip; different: stop. **Never overwrite a published path**: rebuild with `--revision <n>`
  (file `python-<ver>-<platform>-<n>`; `nr_co_deps` takes the newest upload in the version folder).
- On 401/403 stop and report where the packages are; do not look for other credentials.
- Deleting an upload only on the user's explicit request, by exact path.

## 5. Consumers
- VDK deps JSON entry (module `deps/python`, `branch_or_tag: release`, `unzip: true`, `ios`/`android_*`
  `none`): template in `docs/USAGE.md` section 1. Check with
  `BUILD_OS=<macos|linux64_Ubuntu_22.04|win64_vs2019> python3 $NR_SCRIPTS/nr_checkout_deps/nr_co_deps.py -o python <json>`
  in a scratch folder. Do not edit VDK's JSON files unless asked.
- nrPyCpp itself is published separately under `libs/nrpycpp` (not by this skill).

## 6. Building nrPyCpp
- `NRPYCPP_PYTHON_ROOT` (+ optional `NRPYCPP_PYTHON_VERSION`) overrides `lib/depspath.pri`; legacy
  `PYTHON_INCLUDE_PATH/PYTHON_LIB_PATH` still work. Output in `lib/last_build` (lib, headers, libpython).
- `PyRunnerQt.h/.cpp` are generated: `python3 $NR_SCRIPTS/wrappergen2/wrappergen.py -c nrpycpp_wrapper_conf.json` from `lib/`, `--check` to verify. Never edit them by hand.

## Pitfalls seen
- An arm64 dylib whose signature was broken by `install_name_tool` is SIGKILLed at load, silently
  (`Killed: 9`, no dyld message): re-sign (`codesign -f -s -`). This is what broke the old Xcode-python setup.
- `libnrPyCpp`'s macOS install name is bare (`libnrPyCpp.2.dylib`): tests and samples need `DYLD_LIBRARY_PATH`.
- Xcode 26 + Qt 6.8: `qyieldcpu.h` `__yield` error, worked around with `-Wno-error=implicit-function-declaration`.
- Linux archives have `share/terminfo` names differing only by case: unextractable on macOS/Windows,
  the script skips `share/` (not needed).
- `_dbm` was the only extension outside libpython on macOS/Linux and has an `LC_RPATH @loader_path/../..`: pruned.
- MSVC debug builds: `pyconfig.h` turns `_DEBUG` into `Py_DEBUG` + `python313_d.lib`; `pyrunner.h`
  hides `_DEBUG` around `Python.h` (opt out with `NRPYCPP_PYTHON_DEBUG`).
- Test code: `QVariantList() << aList` concatenates; wrap in `QVariant(aList)` to pass a list.
- The interpreter starts once per process and is never re-initialized: scenarios needing another
  configuration run as child processes of `tst_nrpycpp` (`--child <scenario>`).
