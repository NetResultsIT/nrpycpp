---
name: nrpycpp-python-deps
description: Prepare, test and publish the embedded CPython runtime of nrPyCpp (artifactory deps/python, from python-build-standalone, for macOS universal, Linux x86_64, Windows x64), build nrPyCpp against it and run its test suite on macOS, Linux and the Windows VM. Use when asked to update/upgrade/publish the python dependency, build or test nrPyCpp, or debug python embedding issues (python home, venv, signatures, debug builds).
---

# nrPyCpp python dependency, build and tests

Full documentation: `docs/BUILDING.md` (build, packages, publishing, tests) and `docs/USAGE.md` (API,
deployment, troubleshooting). Read the relevant section before changing anything.

## 1. Versions
- Python **3.13** (3.10 and older are EOL). Source: python-build-standalone (Astral), `install_only_stripped`.
- Pin both the version and the release (`--python 3.13.18 --release 20261026`): "latest" is only for
  looking up what exists. Published so far (2026-11-10): 3.13.18 from 20261026, all three platforms
  (`deps/python/release/3.13.18/{macos,linux64,win64}/`); `nr_co_deps` unpacks into `<rel_dest_dir>/<platform>/python/`.
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
- Output tree = artifactory tree: `<work>/upload/deps/python/release/<ver>/{macos,linux64,win64}/python-<ver>-<platform>.{tgz,zip}`, one top folder `python/` with `NRPYCPP_PYTHON.json`.
- Pruning list and package layout: `docs/BUILDING.md` section 2. If a check fails (e.g. a new extension
  module with an rpath), fix the script, do not hand-edit the package. Keep `tests/tools` passing:
  `python3 -m unittest discover -s tests/tools -v`.

## 3. Test before publishing (do not skip)
Unpack each package and run the suite against it: `NRPYCPP_PYTHON_ROOT=<dir>/python QMAKE=<qt>/bin/qmake tests/run_tests.sh` (34 tests, must all pass).
- **macOS**: also the x86_64 half: build lib and tests with `"QMAKE_APPLE_DEVICE_ARCHS=x86_64 arm64"`, run
  `arch -x86_64 -e DYLD_LIBRARY_PATH=<repo>/lib/last_build -e DYLD_FRAMEWORK_PATH=<qt>/lib ./tst_nrpycpp`
  (`arch` drops `DYLD_*` without `-e`). Qt kit: `~/Qt/6.8.6/macos`.
- **Windows** (Parallels VM "Windows 11", ARM64, x64 emulated; Qt `C:\Qt\6.8.6\msvc2022_64`, VS 2022
  Community with x64 tools): `tests\run_tests.ps1` (`-Config debug` too: it checks that debug builds link
  the release python). The VM is shared with the VDK builds:
  - lock first: `mkdir /Users/lamonica/prog/vdk/.claude/winvm.lock` (atomic; if it exists wait, never
    break it), write the owner into `winvm.lock/owner`, remove both when done;
  - `prlctl exec` hangs on long commands and mangles quoting: serve the files from the Mac with
    `python3 -m http.server <port> --bind 10.211.55.2` (own port, 8765 is taken), have the guest
    download a small launcher with `iwr ... -OutFile` and run it (`iex` of the content fails: it comes
    as bytes), start the real script with `Start-Process ... -WindowStyle Hidden`, write a `done.txt`
    with the exit code, poll it with short `prlctl exec ... cmd /c type`;
  - zip the working tree without `.git`, `lib/last_build` binaries, `lib/depspath.pri`, build folders.
  - A build + run takes about 1 minute emulated. Expected sync latency ~60 µs (12 µs native Mac).
- **Linux**: the Qt suite has not been run yet (the VDK image `vdk-deps-u2204` has no Qt; say so if
  asked). At least check the package: in `docker run --platform linux/amd64 ubuntu:22.04`, build a small
  C program with `PyConfig_InitIsolatedConfig` + `config.home` against `python/include/python3.13` and
  `-lpython3.13`, import `ssl ctypes sqlite3 zlib hashlib`, and `readelf -d` libpython (no RPATH/RUNPATH).

## 4. Publish (needs explicit user go-ahead for this upload)
```sh
python3 tools/prepare_python_runtime.py upload --out <work> --dry-run   # read key ~/.nr/artifactory_api_key
python3 tools/prepare_python_runtime.py upload --out <work>             # token ~/.nr/arti_adm_token.txt (Bearer)
```
- Repo `bblocks` at `https://nrreleases.jfrog.io/artifactory`. Never print the token.
- The script GETs `api/storage` for each path: free → PUT with `X-Checksum-Sha1` → verify size+sha1;
  identical → skip; different → stop. **Never overwrite a published path**: rebuild with `--revision <n>`
  (file `python-<ver>-<platform>-<n>`; `nr_co_deps` takes the newest upload in the version folder).
- On 401/403 stop and report where the packages are; do not look for other credentials.
- Deleting an upload only on the user's explicit request, by exact path.

## 5. Consumers
- VDK deps JSON entry (module `deps/python`, `branch_or_tag: release`, `unzip: true`, `ios`/`android_*`
  `none`): template in `docs/USAGE.md` section 1. Check with
  `BUILD_OS=<macos|linux64_Ubuntu_22.04|win64_vs2019> python3 ~/prog/git-scripts/nr_checkout_deps/nr_co_deps.py -o python <json>`
  in a scratch folder. Do not edit VDK's JSON files unless asked.
- nrPyCpp itself is published separately under `libs/nrpycpp` (not by this skill).

## 6. Building nrPyCpp
- `NRPYCPP_PYTHON_ROOT` (+ optional `NRPYCPP_PYTHON_VERSION`) overrides `lib/depspath.pri`; legacy
  `PYTHON_INCLUDE_PATH/PYTHON_LIB_PATH` still work. Output in `lib/last_build` (lib, headers, libpython).
- `PyRunnerQt.h/.cpp` are generated: `python3 ~/prog/git-scripts/wrappergen2/wrappergen.py -c nrpycpp_wrapper_conf.json` from `lib/`, `--check` to verify. Never edit them by hand.

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
