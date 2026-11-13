---
name: nrpycpp-dev-workflow
description: Procedure for any non trivial change to nrPyCpp (feature, bug fix, refactoring) with a lead model and a worker, the design rules of this library (one thread per runner, one shared interpreter and GIL, the generated PyRunnerQt wrapper, type conversions), how to build and test it, and where new knowledge is written. Use before planning, implementing or reviewing a change in this repository.
---

# nrPyCpp development workflow

The rules that are always loaded are in `AGENTS.md`. The python runtime package (prepare, test, publish) is in
`nrpycpp-python-deps`, the Qt checklist in `qt-cpp`, versioning in `semver-abi`.

## 1. Roles
The lead is the most capable model available. It reads the code, designs the tests, writes the plan, stops at the
gate, briefs the worker and reviews the result. The worker is a subagent on a lighter, faster model: it implements
the plan, builds, runs the tests and reports; it does not decide what the plan leaves open. A change of a few lines
that the lead can verify alone does not need a worker.

## 2. Phase 1, plan (lead)
1. Check who else is working: `git worktree list`, `git status`, running builds. Use a worktree of your own.
2. Read the code involved: `lib/pyrunner.h/.cpp` (`PyRunner`: one thread per script, calls, conversions),
   `lib/pyenvironment.h/.cpp` (`PyEnvironment`: the interpreter, started once per process), `lib/nrpycppmodule.*`
   (the `nrpycpp` module scripts import), `lib/nrPyCppConfig.h` (python home and venv), `lib/PyCall.h` and
   `lib/PyCallBackIface.h` (public types).
3. Design the tests first. The library suite is `tests/tst_nrpycpp.cpp` with its scripts in `tests/scripts`; the
   cases: normal, boundary values of each conversion, wrong arguments and wrong state (broken or missing script,
   unknown function) with the expected `error` and `errorMessage`, calls from several threads, a runner destroyed
   while busy, cleanup. A scenario that needs another interpreter configuration (no python home, a venv for
   another version) runs as a child process of the test (`--child <scenario>`). Changes to the packaging tool are
   tested in `tests/tools`.
4. Check the design rules of section 6.
5. Write the plan: files and functions, tests, the public API effect, the documents to update.

## 3. Phase 2, gate (lead)
The list is in `AGENTS.md` section 2. When the plan touches it, present what changes, why, the consumers affected
(VDK; the applications that link the library), the version you would set and the tests, and wait. For a third-party
component also state its name, version, license and where its license text will live; the user confirms the
license fits the company policy.

## 4. Phase 3, implementation (worker)
The brief contains: the directory (a worktree) and branch; the files and lines; the tests to write first; the
production changes; the line endings (LF); the generated files not to touch (`PyRunnerQt.h/.cpp`); the exact
build and test commands of section 7 and the expected baseline; what to report (files changed, deviations, the
`Totals:` line of the test program, what could not be built or run, what was learned); "do not commit".

## 5. Phase 4, review (lead)
Read the whole diff. Check:
- public API untouched, or approved and classified as patch, minor or major with `NRPYCPP_VERSION` raised
  accordingly (skill `semver-abi`);
- `PyRunnerQt.h/.cpp` regenerated with the `--check` of wrappergen, not edited; the doc comments of the wrapper
  are the ones of `pyrunner.cpp`;
- every Python C-API call is made with the GIL held; reference counts of new objects are balanced;
- error paths tested; no new public header or exported class without approval;
- `docs/USAGE.md` and `docs/BUILDING.md` updated; no internal host, URL or personal path added (public repository);
- thread rules of `qt-cpp`.
Rerun the tests yourself. Report what was verified and what was not (a platform, a debug build, a mobile target).

## 6. Design rules of this repository
- `lib/pyrunner.cpp` (about 930 lines) is the largest file. Put new conversion or environment logic in a small
  class or in `PyEnvironment` / `nrpycppmodule` instead of growing it.
- Public methods of `PyRunnerQt` come from the methods between the `START_WRAPPER_METHODS` and
  `END_WRAPPER_METHODS` markers of `lib/pyrunner.h`; add them there and regenerate, never in the generated files.
- Python's headers must not reach an installed header: `pyrunner.h` is internal, the installed ones are listed in
  `DIST_INCS` of `lib/nrPyCpp.pro`.
- The interpreter is initialized once per process and never re-initialized: do not add a path that finalizes it
  while the process runs.
- Errors are returned in `PyFunctionCallResult`, not thrown or only logged.

## 7. Build and test
Details: `docs/BUILDING.md` sections 3 and 4 (do not copy them here); the python runtime and the pitfalls of the
build: `nrpycpp-python-deps`.
- Needs Qt 6.8 and a python runtime package: `NRPYCPP_PYTHON_ROOT` (the `python` folder of the package) or
  `lib/depspath.pri` made from `lib/depspath.pri.sample`.
- Wrapper first: `PyRunnerQt.h/.cpp` are not in the repository, generate them (`docs/BUILDING.md` section 5) before
  the first build (not verified for a fresh clone).
- Library and suite on macOS and Linux: `NRPYCPP_PYTHON_ROOT=<python folder> QMAKE=<qt>/bin/qmake tests/run_tests.sh
  [build dir] [test args]`; on Windows `tests\run_tests.ps1`. The scripts build the library into `lib/last_build`,
  build the tests and run `tst_nrpycpp`. Extra arguments go to the test program: a test function name runs one
  test, `-v2` is verbose. `CONFIG=debug` selects a debug build.
- Tools: `python3 -m unittest discover -s tests/tools -v` (needs no Qt and no network).
- `tester/nrPyCppTester.pro` is an interactive sample, not a test.
- Baseline: the suite passes completely (the count is in `.agents/skills/nrpycpp-python-deps/baselines.md`).

## 8. Where new knowledge goes
Write down what you learn while working (a missing build step, a trap, a wrong statement in these files) in the
same change that produced it, in one place:

| What was learned | Where it is written |
|---|---|
| A rule that applies to every change here | `AGENTS.md` section 4, one line with a pointer |
| A procedure, or a step that was missing or wrong (build, tests, dependencies) | the `SKILL.md` of that skill, corrected in place |
| A dated result, a baseline, what one machine or VM did | a reference file of the skill (`baselines.md` of `nrpycpp-python-deps`) |
| A Qt trap, or anything else that belongs in a shared skill | the `agent-skills` repository, not the copy here (see below) |
| A change of API, type conversion, error or enum value | `docs/USAGE.md`, in the same change as the code, with the version raised |
| A change of build, package layout or python version | `docs/BUILDING.md` (and `README.md` if its quick start changes) |
| Why the code is the way it is (design, a race analysis) | a comment at the code when it is local, else `docs/USAGE.md` section 6 or `docs/BUILDING.md` |
| An open problem, something not verified | an issue of the public repository, or the pull request description; not a skill |
| Something about one person's machine or preferences | not in the repository (the agent's own memory or local settings) |

- One place per fact: everything else points to it.
- Correct a wrong statement where it stands; do not append a note that contradicts it.
- A worker reports what it learned; the lead decides where it goes and reviews the edit like code.
- The shared skills (`qt-cpp`, `semver-abi`) and `sync_skills.py` are copies from the `agent-skills` repository,
  pinned in `.agents/shared-skills.lock`; the skills check fails if they are edited here. To change one, change it
  in `agent-skills`, or describe the change in your pull request here and a maintainer carries it over and updates
  the copies. Nothing in this repository needs a checkout of another project.

## 9. After the merge
Suggest, never do unasked: removing the worktree, the local branch and the scratch build folders you created.
