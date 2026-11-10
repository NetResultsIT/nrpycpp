/**************************************************************************
 *  Copyright (C) 2026 by NetResults S.r.l. ( http://www.netresults.it )  *
 *  Author( s ) :                                                         *
 *         Francesco Lamonica      <f.lamonica@netresults.it>             *
 **************************************************************************/

#include "nrpycppmodule.h"

#include <QDebug>

/*!
 * \internal
 * \brief toQString converts a python str (replacing what cannot be encoded, e.g. lone surrogates)
 * \return false, with a python exception set, if the conversion fails
 */
static bool toQString(PyObject *text, QString *out)
{
    PyObject *utf8 = PyUnicode_AsEncodedString(text, "utf-8", "replace"); //new reference
    if (!utf8)
        return false;
    *out = QString::fromUtf8(PyBytes_AS_STRING(utf8), PyBytes_GET_SIZE(utf8));
    Py_DECREF(utf8);
    return true;
}


/*!
 * \internal
 * \brief resolveRunnerId finds the runner the calling code belongs to
 * \return the id of the runner running a call on this thread, or else the one recorded in the python
 * thread by the wrapped threading.Thread.start(), or an empty string
 * \note must be called with the GIL held, it never leaves an exception set
 */
static QString resolveRunnerId()
{
    QString id = PyRunner::currentRunnerId();
    if (!id.isEmpty())
        return id;

    PyObject *threading = PyImport_ImportModule("threading"); //new reference
    PyObject *thread = threading ? PyObject_CallMethod(threading, "current_thread", nullptr) : nullptr; //new reference
    PyObject *recorded = thread ? PyObject_GetAttrString(thread, "_nrpycpp_runner_id") : nullptr; //new reference
    if (recorded && PyUnicode_Check(recorded) && !toQString(recorded, &id))
        id.clear();
    Py_XDECREF(recorded);
    Py_XDECREF(thread);
    Py_XDECREF(threading);
    PyErr_Clear();
    return id;
}


static PyObject *nrpycpp_send_message(PyObject *, PyObject *text)
{
    if (!PyUnicode_Check(text)) {
        PyErr_Format(PyExc_TypeError, "send_message() argument must be str, not %.100s", Py_TYPE(text)->tp_name);
        return nullptr;
    }
    QString message;
    if (!toQString(text, &message))
        return nullptr;
    const QString id = resolveRunnerId();
    if (id.isEmpty()) {
        PyErr_SetString(PyExc_RuntimeError, "send_message(): no runner is associated with the current thread "
                                            "(threads started with _thread.start_new_thread are not tracked)");
        return nullptr;
    }
    if (!PyRunner::routeMessage(id, message)) {
        PyErr_SetString(PyExc_RuntimeError, "send_message(): the runner does not exist anymore");
        return nullptr;
    }
    Py_RETURN_NONE;
}


static PyObject *nrpycpp_runner_id(PyObject *, PyObject *)
{
    const QString id = resolveRunnerId();
    if (id.isEmpty())
        Py_RETURN_NONE;
    const QByteArray utf8 = id.toUtf8();
    return PyUnicode_FromStringAndSize(utf8.constData(), utf8.size());
}


//_write(stream, text): stream is 0 for stdout, 1 for stderr; output with no runner is dropped
static PyObject *nrpycpp_write(PyObject *, PyObject *args)
{
    int stream = 0;
    PyObject *text = nullptr;
    if (!PyArg_ParseTuple(args, "iU", &stream, &text))
        return nullptr;
    QString s;
    if (!toQString(text, &s))
        return nullptr;
    const QString id = resolveRunnerId();
    if (!id.isEmpty())
        PyRunner::routeOutput(id, stream, reinterpret_cast<quintptr>(PyThreadState_Get()), s);
    Py_RETURN_NONE;
}


static PyObject *nrpycpp_flush(PyObject *, PyObject *args)
{
    int stream = 0;
    if (!PyArg_ParseTuple(args, "i", &stream))
        return nullptr;
    const QString id = resolveRunnerId();
    if (!id.isEmpty())
        PyRunner::flushOutput(id, stream, reinterpret_cast<quintptr>(PyThreadState_Get()));
    Py_RETURN_NONE;
}


//_exception(runner_id, traceback) -> True if the runner got it
static PyObject *nrpycpp_exception(PyObject *, PyObject *args)
{
    PyObject *runnerId = nullptr;
    PyObject *traceback = nullptr;
    if (!PyArg_ParseTuple(args, "UU", &runnerId, &traceback))
        return nullptr;
    QString id, text;
    if (!toQString(runnerId, &id) || !toQString(traceback, &text))
        return nullptr;
    return PyBool_FromLong(PyRunner::routeException(id, text) ? 1 : 0);
}


static PyMethodDef nrpycppMethods[] = {
    {"send_message", nrpycpp_send_message, METH_O,
     "send_message(text: str) -> None\n\nSends a text to the C++ side (PyRunnerQt::messageReceived)."},
    {"runner_id", nrpycpp_runner_id, METH_NOARGS,
     "runner_id() -> str | None\n\nThe id of the runner the calling code belongs to (PyRunnerQt::runnerId)."},
    {"_write", nrpycpp_write, METH_VARARGS, "internal: routes text written to stdout / stderr"},
    {"_flush", nrpycpp_flush, METH_VARARGS, "internal: delivers the pending partial line"},
    {"_exception", nrpycpp_exception, METH_VARARGS, "internal: routes an exception of a python thread"},
    {nullptr, nullptr, 0, nullptr}
};

static struct PyModuleDef nrpycppModuleDef = {
    PyModuleDef_HEAD_INIT,
    "nrpycpp",
    "Built-in module of the nrPyCpp library: how a script talks back to the C++ side.",
    -1,
    nrpycppMethods,
    nullptr, nullptr, nullptr, nullptr
};

PyObject *nrpycppModuleInit()
{
    return PyModule_Create(&nrpycppModuleDef);
}


/*
 * Executed once in a private namespace, never in the one of the user's modules.
 */
static const char *bootstrapCode = R"PYCODE(
import io, sys, threading, traceback
import nrpycpp

_tls = threading.local()  # quiet: do not route what is written (used while reporting an exception)


class _RoutedStream(io.TextIOBase):
    """Forwards what is written to the runner of the caller, and to the original stream."""

    def __init__(self, index, original):
        self._index = index
        self._original = original

    def write(self, s):
        if not isinstance(s, str):
            raise TypeError("write() argument must be str, not " + type(s).__name__)
        if not getattr(_tls, "quiet", False):
            nrpycpp._write(self._index, s)
        if self._original is not None:
            self._original.write(s)
        return len(s)

    def flush(self):
        if not getattr(_tls, "quiet", False):
            nrpycpp._flush(self._index)
        if self._original is not None:
            self._original.flush()

    def writable(self):
        return True

    def isatty(self):
        return self._original is not None and self._original.isatty()

    def fileno(self):
        if self._original is None:
            raise io.UnsupportedOperation("fileno")
        return self._original.fileno()

    @property
    def encoding(self):
        return getattr(self._original, "encoding", "utf-8")

    @property
    def errors(self):
        return getattr(self._original, "errors", "strict")

    @property
    def buffer(self):
        if self._original is None:
            raise AttributeError("buffer")
        return self._original.buffer

    def __getattr__(self, name):
        # reconfigure(), line_buffering, name, ... of the original stream
        original = self.__dict__.get("_original")
        if original is None:
            raise AttributeError(name)
        return getattr(original, name)


sys.stdout = _RoutedStream(0, sys.stdout)
sys.stderr = _RoutedStream(1, sys.stderr)

# a thread belongs to the runner that started it: this routes output, messages and exceptions of
# the threads started by a script (also the workers of a ThreadPoolExecutor)
_original_start = threading.Thread.start


def _start(self):
    if getattr(self, "_nrpycpp_runner_id", None) is None:
        self._nrpycpp_runner_id = nrpycpp.runner_id()
    if self._nrpycpp_runner_id is not None:
        # deliver the last partial line when the thread ends (the buffers are per thread state)
        run = self.run

        def _run():
            try:
                run()
            finally:
                nrpycpp._flush(0)
                nrpycpp._flush(1)

        self.run = _run
    return _original_start(self)


_start.__doc__ = _original_start.__doc__
threading.Thread.start = _start

_original_excepthook = threading.excepthook


def _excepthook(args):
    delivered = False
    try:
        runner_id = getattr(args.thread, "_nrpycpp_runner_id", None)
        if runner_id and args.exc_type is not SystemExit:
            text = "".join(traceback.format_exception(args.exc_type, args.exc_value, args.exc_traceback)).strip()
            delivered = nrpycpp._exception(runner_id, text)
    except Exception:
        pass
    # the original hook prints the traceback on the console; when the runner got it already,
    # it must not get it again as stderr output
    _tls.quiet = delivered
    try:
        _original_excepthook(args)
    finally:
        _tls.quiet = False


threading.excepthook = _excepthook
)PYCODE";


bool nrpycppInstallBootstrap()
{
    PyObject *globals = PyDict_New(); //new reference
    if (!globals)
        return false;
    PyObject *ret = nullptr;
    if (PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins()) == 0)
        ret = PyRun_String(bootstrapCode, Py_file_input, globals, globals); //new reference
    Py_DECREF(globals); //the functions defined by the code keep it alive
    if (!ret) {
        qWarning() << "NrPyCpp: cannot install the nrpycpp python module support, the output of the scripts will not be routed";
        PyErr_Print();
        return false;
    }
    Py_DECREF(ret);
    return true;
}
