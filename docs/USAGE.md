# Using nrPyCpp

nrPyCpp embeds a CPython interpreter in a Qt application and lets C++ call the functions of a Python
script, synchronously or asynchronously, converting the arguments and the return value between `QVariant`
and Python objects. The script can talk back: what it prints, the messages it sends and the exceptions of
its threads reach the runner they belong to (section 7).

Contents:
[1. Adding it to an application](#1-adding-it-to-an-application) ·
[2. Calling Python](#2-calling-python) ·
[3. Types](#3-types) ·
[4. Errors](#4-errors) ·
[5. Writing the scripts](#5-writing-the-scripts) ·
[6. Threads](#6-threads) ·
[7. Talking back to C++](#7-talking-back-to-c) ·
[8. Python home and virtual environments](#8-python-home-and-virtual-environments) ·
[9. Deploying an application](#9-deploying-an-application) ·
[10. Troubleshooting](#10-troubleshooting)

---

## 1. Adding it to an application

You need two packages, both on artifactory (repo `bblocks`):

| package | module | contents |
|---|---|---|
| nrPyCpp | `libs/nrpycpp` | `libnrPyCpp` and its headers |
| Python runtime | `deps/python` | one `python/` folder: CPython 3.13 shared library, standard library, headers |

With `nr_co_deps` (VDK `deps_qt6.json` / `deps_release_qt6.json`), the python runtime is an entry like:

```json
{
  "name": "python",
  "rel_dest_dir": "deps/python-bin",
  "proto": "artifact",
  "url": "https://nrreleases.jfrog.io/nrreleases/bblocks",
  "module": "deps/python",
  "branch_or_tag": "release",
  "version": {
    "default": "3.13.18",
    "ios": "none",
    "android_arm64": "none",
    "android_x86_64": "none",
    "android_arm7": "none"
  },
  "runtime_dep": "true",
  "unzip": "true"
}
```

`nr_co_deps` unpacks it into `deps/python-bin/<platform>/python/` (verified for `macos`,
`linux64_Ubuntu_22.04` and `win64_vs2019`).

The application links nrPyCpp only (nrPyCpp links python). In the application `.pro`:

```qmake
NRPYCPP_DIR = $$PWD/deps/libnrpycpp-bin         # lib/last_build of a local build, or the artifactory package
INCLUDEPATH += $$NRPYCPP_DIR/include
LIBS += -L$$NRPYCPP_DIR -lnrPyCpp               # debug builds: nrPyCppd (windows), nrPyCpp_debug (macOS), nrPyCpp_d (linux)
```

The public headers are `PyRunnerQt.h` (the runner), `PyCall.h` (the result type, included by the former)
and `nrPyCppConfig.h` (interpreter configuration). Python's own headers are not needed by the application.

---

## 2. Calling Python

```cpp
#include "PyRunnerQt.h"

// the script, and optional folders where the script finds the modules it imports
PyRunnerQt *runner = new PyRunnerQt("/path/to/rtp_tools.py", QStringList() << "/path/to/helpers");
```

Creating the first runner starts the interpreter (see [section 8](#8-python-home-and-virtual-environments)).
The script is imported on the first call, not by the constructor.

`runner->runnerId()` is the unique id of the runner, `NrPyCpp-<script name>{<uuid>}` (also the name of its
thread). What the script sends back (messages, output, thread exceptions) is described in
[section 7](#7-talking-back-to-c); `setCallbackHandler()` is the interface flavour of the same.

### Synchronous calls

```cpp
PyFunctionCallResult r = runner->syncCallFunction("parse_header", QVariantList() << packet);
if (r.error) {
    qWarning().noquote() << r.errorMessage;
} else {
    QVariantMap header = r.returnValue.toMap();
}
```

`syncCallFunction()` blocks the calling thread until the function returns. The overhead is about 12 µs per
call on an M-series Mac, measured by the test suite (`syncCallLatency`).

### Asynchronous calls

```cpp
connect(runner, &PyRunnerQt::callCompletedSignal, this, [runner](QString callId) {
    PyFunctionCallResult r = runner->getAsyncCallResult(callId);
    ...
});
QString callId = runner->asyncCallFunction("long_analysis", QVariantList() << capture);
```

- `asyncCallFunction()` returns immediately with the call id (a UUID string).
- `callCompletedSignal(callId)` is emitted when the call ends, successfully or not. It is queued to the
  thread `runner` belongs to, so the slot runs in that thread's event loop.
- `getAsyncCallResult(callId)` returns the result **once**: a finished call is then forgotten. Before the
  call ends (or for an unknown id) it returns a result with an invalid `endTime`.
- `getAsyncCallsList()` lists the async calls whose result has not been retrieved yet (running or finished).
- `getCallInfo(callId)` and `printCalls()` are debugging helpers.

### The result

```cpp
struct PyFunctionCallResult {
    QDateTime startTime;     // when python started the call
    QDateTime endTime;       // when it ended; invalid while the call is running
    QVariant  returnValue;   // the converted return value (invalid for None or on error)
    bool      error;
    QString   errorMessage;  // python traceback or nrPyCpp error, see section 4
};
```

### Destroying a runner

`delete runner` waits for the call that is running (Python code cannot be interrupted safely), drops the
calls still queued (their `callCompletedSignal` is never emitted) and unloads the script. Do not destroy a
runner while another thread is blocked in its `syncCallFunction()`.

---

## 3. Types

C++ to Python (parameters):

| `QVariant` type | Python |
|---|---|
| invalid `QVariant()` | `None` |
| `bool` | `bool` |
| `char`, `short`, `int`, `long`, `qlonglong` | `int` |
| unsigned variants up to `qulonglong` | `int` (full 64 bit range) |
| `float`, `double` | `float` |
| `QString` | `str` |
| `QByteArray` | `bytes` |
| `QVariantList`, `QStringList` | `list` (recursively) |
| `QVariantMap`, `QVariantHash` | `dict` with `str` keys (recursively) |
| anything else (`QPoint`, `QDateTime`, custom types...) | error `unsupported parameter type "<type>"`, the function is not called |

Python to C++ (return value):

| Python | `QVariant` type |
|---|---|
| `None` | invalid `QVariant()` |
| `bool` | `bool` |
| `int` | `int` if it fits, else `qlonglong`, else `qulonglong`; beyond 64 bits it is an error |
| `float` | `double` |
| `str` | `QString` |
| `bytes`, `bytearray` | `QByteArray` |
| `list`, `tuple` | `QVariantList` (recursively) |
| `dict` | `QVariantMap`, keys converted with `str()` (recursively) |
| anything else | error `unsupported return type "<type>"` |

To return several values, return a tuple or a list; to return a structure, return a dict. Pass binary
data as `QByteArray`: a `QString` goes through UTF-8.

> **Watch out:** `QVariantList() << otherList` appends the *elements* of `otherList`. To pass a list as
> one parameter write `QVariantList() << QVariant(otherList)`.

---

## 4. Errors

Every failure is reported in the result (`error == true`), the library never throws and never exits.
`errorMessage` is one of:

| cause | message |
|---|---|
| exception raised by the function | the python traceback, e.g. `Traceback (most recent call last): ... ZeroDivisionError: division by zero` |
| the script does not import (syntax error, failing import...) | `NrPyCpp ERROR: cannot load module "<script>"` followed by the traceback |
| the script file does not exist | `NrPyCpp ERROR: cannot find script "<script>"` |
| no such function | `NrPyCpp ERROR: cannot find python function named "<name>"` |
| a parameter cannot be converted | `NrPyCpp ERROR calling "<name>": parameter <n>: unsupported parameter type "<type>"` |
| the return value cannot be converted | `NrPyCpp ERROR retrieving result from "<name>": ...` |
| the interpreter could not be started | `NrPyCpp ERROR: the python interpreter is not initialized (see the log for details)` |

A failed import is retried on the next call, so a broken script can be fixed while the application keeps
running. Library messages (interpreter start, venv, missing home) go to the Qt log (`qDebug`, `qWarning`,
`qCritical`) with the `NrPyCpp:` prefix.

---

## 5. Writing the scripts

```python
# rtp_tools.py
import struct

def parse_header(packet):
    v_p_x_cc, m_pt, seq, ts, ssrc = struct.unpack("!BBHII", packet[:12])
    return {"version": v_p_x_cc >> 6, "marker": bool(m_pt & 0x80), "pt": m_pt & 0x7F,
            "seq": seq, "timestamp": ts, "ssrc": ssrc}
```

- **Module name.** The script is imported as a module named after its file name (`rtp_tools.py` →
  `rtp_tools`). Its folder and the dependency folders are *appended* to `sys.path`, so a standard library
  module with the same name wins: do not call a script `codecs.py`, `json.py`, `struct.py`, `select.py`...
  Two scripts with the same file name in different folders cannot be used at the same time.
- **State.** Module-level variables keep their value between calls of the same runner. Runners on the
  same script share the module, hence the state. Destroying all the runners of a script unloads it, so a
  new runner imports the current version of the file (handy to reload an edited script).
- **Imports.** The bundled standard library is available (`struct`, `zlib`, `hashlib`, `ssl`, `socket`,
  `ctypes`, `json`, `sqlite3`, `asyncio`...), except tkinter, idle, turtle, ensurepip and the dbm
  backends. Third party packages come from the virtual environment (section 8).
- **Output.** `print()` goes to the process stdout/stderr, unbuffered, so it is interleaved with the Qt
  log in the console.
- **Threads in scripts** are allowed. Make them daemon threads: at exit the interpreter waits for the
  non-daemon ones.
- **Signals.** The interpreter does not install signal handlers (Ctrl+C is handled by the application, a
  long loop in Python cannot be interrupted with `KeyboardInterrupt`).

---

## 6. Threads

- Each runner owns a thread. Its calls, sync and async, run one at a time in that thread, in the order
  they were requested.
- `syncCallFunction()` can be called from any thread, also from many threads at the same time (the test
  suite does it); it blocks only the caller. Do not call slow functions synchronously from the GUI thread.
  Calling it from a slot that runs in the runner's own thread is allowed (it runs right away instead of
  waiting).
- All the runners share **one interpreter and its GIL**: Python code of different runners does not run in
  parallel. C code that releases the GIL (`zlib`, `hashlib` on large buffers, `time.sleep`, socket I/O)
  does. The GIL is released whenever no Python code is running, so idle runners cost nothing.
- The interpreter starts with the first runner (or `NrPyCpp::isPythonAvailable()`) and stays alive until
  the process exits, even when all the runners are destroyed: re-initializing CPython is not supported by
  many extension modules.

---

## 7. Talking back to C++

Besides returning a value, a script can send text to the C++ side. Everything is routed **per runner**:
any number of runners share the interpreter and each one gets only what its own scripts produce.

### The `nrpycpp` module

The interpreter provides a built-in module `nrpycpp` (nothing to install, it is not a file):

```python
import nrpycpp

nrpycpp.send_message("42% done")    # -> PyRunnerQt::messageReceived("42% done")
nrpycpp.runner_id()                 # -> the id of the runner (PyRunnerQt::runnerId()), or None
print("some text")                  # -> PyRunnerQt::outputReceived("some text")
print("problem", file=sys.stderr)   # -> PyRunnerQt::errorOutputReceived("problem")
```

- `send_message(text)` raises `TypeError` if `text` is not a `str`, and `RuntimeError` if the calling code
  does not belong to any runner (see the routing rules).
- `runner_id()` returns the same string as `PyRunnerQt::runnerId()`: `NrPyCpp-<script name>{<uuid>}`.
- Names starting with `_` are internal.
- The module is only available when nrPyCpp started the interpreter itself. If the host application
  initializes Python before the first runner, the module does not exist and the output is not routed
  (the library logs a warning).

### Signals

```cpp
connect(runner, &PyRunnerQt::messageReceived,     this, [](QString text)      { ... });
connect(runner, &PyRunnerQt::outputReceived,      this, [](QString line)      { ... }); // stdout
connect(runner, &PyRunnerQt::errorOutputReceived, this, [](QString line)      { ... }); // stderr
connect(runner, &PyRunnerQt::unhandledException,  this, [](QString traceback) { ... });
QString id = runner->runnerId();
```

- `outputReceived` / `errorOutputReceived` are emitted once per **line**, without the line end. Output is
  line buffered per runner, stream and Python thread: a partial line (`print("x", end="")`) is delivered
  when the thread calls `sys.stdout.flush()` and, for the thread running the call, at the latest when the
  call ends. A partial line written by another thread waits for its end (or its flush).
- `unhandledException` carries the traceback of an exception that escaped a Python thread started by the
  script. Exceptions raised inside a call are not signalled: they are in the `PyFunctionCallResult`. The
  traceback is not repeated in `errorOutputReceived`, but it is still printed on the console.
- The signals are emitted from the Python thread and queued to the thread the `PyRunnerQt` lives in, like
  `callCompletedSignal`. Output still goes to the console too, as it always did.

### Which runner does the code belong to

1. Code running inside a call of the runner (the function and what it calls).
2. A thread created with `threading.Thread` (also `ThreadPoolExecutor` workers, `Timer`...) belongs to the
   runner of the code that started it. Threads started by those threads belong to the same runner.
3. Anything else belongs to **no** runner: its output is only written to the console and `send_message()`
   raises `RuntimeError`. This is the case of threads created with `_thread.start_new_thread` (they bypass
   `threading`) or by extension modules, and of code run after the runner has been destroyed.

A thread outlives the call that started it, and the runner it belongs to may be destroyed before it ends: from
then on its output is discarded and `send_message()` raises `RuntimeError`. No crash, and the other runners
are not affected.

### Callback interface

For code that prefers an interface to signals (and for the users of the previous release) there is
`PyCallBackIface`:

```cpp
#include "PyCallBackIface.h"

class MyHandler : public PyCallBackIface {
public:
    void onStdOutput(const QSharedPointer<QString> &line) override;       // stdout and stderr lines
    void onException(const QSharedPointer<QString> &traceback) override;  // unhandled thread exceptions
    void onSendMessage(const QSharedPointer<QString> &message) override;  // nrpycpp.send_message()
};

runner->setCallbackHandler(&handler);   // not owned; nullptr removes it
```

The handler is called **synchronously from the Python thread that produced the event**, with the GIL held
and while the runner's routing lock is held: keep it short, do not wait for other threads in it, and make it
thread safe (several Python threads can call it). It may be used together with the signals. When
`setCallbackHandler()` returns, the previous handler is not in use anymore, so it can be deleted. Remove
the handler (or make sure it outlives the runner) before destroying the runner: the destructor of the
runner also detaches it.

### Moving from the previous release

Scripts used `callbackHandler.send_message(...)` (with a ctypes or PySide module and the runner id passed
by the C++ code): use `nrpycpp.send_message("...")` instead, there is no id to pass around. The C++ side,
`PyCallBackIface` and `setCallbackHandler()`, is unchanged; `runnerId()` replaces `getRunnerId()`.
Calling C++ (VDK) objects from Python, which the PySide/shiboken work aimed at, is not supported and
not planned: scripts talk back only with text (`send_message()`, output, exceptions).

### Example

```python
# analyzer.py
import nrpycpp

def analyze(capture):
    for i, packet in enumerate(capture):
        if i % 1000 == 0:
            nrpycpp.send_message("packet %d of %d" % (i, len(capture)))
        ...
    print("done")
    return summary
```

```cpp
connect(runner, &PyRunnerQt::messageReceived, progressLabel, &QLabel::setText);
connect(runner, &PyRunnerQt::outputReceived, logView, &QPlainTextEdit::appendPlainText);
runner->asyncCallFunction("analyze", QVariantList() << capture);
```

---

## 8. Python home and virtual environments

### Python home

The interpreter needs its standard library (the *python home*: the `python/` folder of the package,
holding `lib/python3.13/` on macOS/Linux, `Lib\` and `DLLs\` on Windows). It is looked for in this order:

1. `NrPyCpp::setPythonHome(path)`, called before the first runner is created;
2. the `NRPYCPP_PYTHONHOME` environment variable;
3. next to the executable: `<app dir>/python`; on macOS also `Contents/Frameworks/python` and
   `Contents/Resources/python`; on Linux also `<app dir>/../lib/python`;
4. development builds only: the runtime the library was built against (`NRPYCPP_PYTHON_ROOT` at build
   time, recorded in the build).

The interpreter is **isolated**: `PYTHONPATH`, `PYTHONHOME` and the other `PYTHON*` variables, the user
site-packages and any python installed on the machine are ignored.

```cpp
#include "nrPyCppConfig.h"

NrPyCpp::setPythonHome(QCoreApplication::applicationDirPath() + "/../Resources/python");
if (!NrPyCpp::isPythonAvailable())      // starts the interpreter: false if it cannot (see the log)
    disablePythonFeatures();
```

### Virtual environment (third party packages)

Packages are installed by the user in a virtual environment created with **the same Python version
(3.13)**, then given to nrPyCpp:

```sh
uv venv --python 3.13 ~/vdk-python              # or: python3.13 -m venv ~/vdk-python
uv pip install --python ~/vdk-python scapy construct crcmod
```

```cpp
NrPyCpp::setVirtualEnv(QDir::homePath() + "/vdk-python");    // before the first runner
```

or `NRPYCPP_VENV=~/vdk-python`. Its `site-packages` is added with `site.addsitedir()`, so `.pth` files
work. The version is read from the venv's `pyvenv.cfg`: a venv made for another `X.Y` is ignored with a
warning, because its compiled packages would not load. Packages with C extensions need wheels for
`cp313` (not the free-threaded `cp313t`). Any CPython 3.13 can create the venv: it does not have to be
the bundled one.

---

## 9. Deploying an application

Ship the shared library and the standard library from the `deps/python` package; leave out `include/`,
`libs/` and `NRPYCPP_PYTHON.json` (it only records the origin of the package). Keep the license files.

**macOS** (app bundle):

```
MyApp.app/Contents/Frameworks/libnrPyCpp.2.dylib
MyApp.app/Contents/Frameworks/libpython3.13.dylib       <- python/lib/libpython3.13.dylib
MyApp.app/Contents/Resources/python/lib/python3.13/     <- python/lib/python3.13
```

`libpython3.13.dylib` has the install name `@rpath/libpython3.13.dylib`, so the app needs an rpath to
`@executable_path/../Frameworks` (qmake: `QMAKE_RPATHDIR += @executable_path/../Frameworks`). The
library's own install name is a bare `libnrPyCpp.2.dylib`: fix the reference in the executable while
bundling (`install_name_tool -change libnrPyCpp.2.dylib @rpath/libnrPyCpp.2.dylib`). Sign everything,
dylibs first, with the application identity (after any `install_name_tool` the old signature is broken
and an arm64 binary is killed at load). Keep the stdlib under `Resources`, not `Frameworks`: codesign
expects only code in `Frameworks`.

**Windows**:

```
MyApp\MyApp.exe
MyApp\nrPyCpp.dll
MyApp\python313.dll          <- python\python313.dll (python3.dll is not needed)
MyApp\vcruntime140.dll, vcruntime140_1.dll   (if the app does not install the VC++ runtime already)
MyApp\python\Lib\            <- python\Lib
MyApp\python\DLLs\           <- python\DLLs (extension modules and openssl, libffi, sqlite DLLs)
```

**Linux**:

```
<prefix>/bin/myapp                                 (rpath $ORIGIN/../lib)
<prefix>/lib/libnrPyCpp.so.2
<prefix>/lib/libpython3.13.so.1.0                  <- python/lib/libpython3.13.so.1.0
<prefix>/lib/python/lib/python3.13/                <- python/lib/python3.13 (home = <prefix>/lib/python)
```

The Linux package runs on glibc 2.17 and newer.

---

## 10. Troubleshooting

| symptom | cause and fix |
|---|---|
| macOS: the process dies at start with no message (`Killed: 9`) | a dylib with a broken signature, usually after `install_name_tool`: re-sign it (`codesign -f -s - <dylib>` for development) |
| `Library not loaded: libnrPyCpp.2.dylib` | the bare install name: set `DYLD_LIBRARY_PATH` in development, fix the reference in the bundle (section 9) |
| `cannot initialize python ... Failed to import encodings module` | the python home is wrong or incomplete: check the path in the log, it must contain `lib/python3.13/encodings` (`Lib\encodings` on Windows) |
| `cannot find a python home` | none of the locations of section 8 has a runtime: call `setPythonHome()` or set `NRPYCPP_PYTHONHOME` |
| `virtual environment ... was created with python 3.12 ... ignoring it` | recreate the venv with Python 3.13 |
| `ModuleNotFoundError` for a third party package | no venv configured, or the venv was ignored: look for `NrPyCpp:` lines in the log |
| `ModuleNotFoundError` for a module next to the script | the module is in another folder: pass it in the `dependencies` of the runner |
| a stdlib module is imported instead of my script | the script has a stdlib name (section 5) |
| `setPythonHome()/setVirtualEnv() has no effect` | they were called after the interpreter was started: call them before the first runner |
| Windows debug build fails to link `python313_d.lib` | `pyconfig.h` asks for a debug python; nrPyCpp hides `_DEBUG` around `Python.h` unless `NRPYCPP_PYTHON_DEBUG` is defined, so this only happens if Python.h is included elsewhere in a debug build |
| Xcode 26: `implicitly declaring library function '__yield'` in `qyieldcpu.h` | a Qt 6.8 header issue with recent clang: add `QMAKE_CXXFLAGS += -Wno-error=implicit-function-declaration` (nrPyCpp and its tests do) |
| `send_message()` raises `RuntimeError` | the code does not belong to a runner: a thread created with `_thread` or by a C extension, or the runner was destroyed (section 7) |
| the output of a script is not delivered by the signals | python was started by the host application before the first runner (the `nrpycpp` module is not available), or the line is not complete yet: call `sys.stdout.flush()` |
| macOS: `Killed: 9` after rebuilding in place | `cp` over a dylib that was already loaded keeps a stale signature in the kernel cache: remove `lib/last_build/*.dylib` before building again |
| a call never returns | the Python function does not return (loop, blocking read): sync calls wait forever, use an async call or a timeout in the script |
