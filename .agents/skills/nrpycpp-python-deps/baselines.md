# Baselines of nrPyCpp and its python packages

Dated results. Update this file when a new package is published or the suite is run again.

## Published packages
- 2026-11-10: python 3.13.18 from the release 20261026, all three platforms:
  `deps/python/release/3.13.18/{macos,linux64,win64}/`.

## Test suite (`tst_nrpycpp`)
- The suite had 34 tests when the packages above were published (the number is from `docs/BUILDING.md`
  section 4, not counted again).
- macOS arm64 and macOS x86_64 under Rosetta: all passed; sync call latency about 12 to 14 microseconds.
- Windows 11 ARM64 VM, x64 emulated, MSVC 2022, release and debug: all passed; sync call latency about 60
  microseconds. A build and a run took about 1 minute emulated.
- Linux: the Qt suite had not been run (no image with Qt was available). The package was only checked with a C
  program embedding it (see `SKILL.md` section 3).
