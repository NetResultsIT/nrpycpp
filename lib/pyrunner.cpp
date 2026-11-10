/**************************************************************************
 *  Copyright (C) 2022 by NetResults S.r.l. ( http://www.netresults.it )  *
 *  Author( s ) :                                                         *
 *         Stefano Aru                  <s.aru@netresults.it>             *
 *         Francesco Lamonica      <f.lamonica@netresults.it>             *
 **************************************************************************/

#include "pyrunner.h"

#include <QDebug>
#include <QMutexLocker>
#include <QVariant>

#define NRPYQT_DEBUG_REMOVETOENABLE

/*
 * Routing of the output / messages / exceptions of the scripts to the runner they belong to.
 * The registry maps the runner ids to the runners; everything that is emitted to (or called on) a
 * runner is done with the registry mutex held, so a runner cannot be destroyed in the meanwhile.
 * It is recursive because a callback handler may well create or destroy a runner.
 * Rule: the registry mutex is never held while waiting for the GIL (it is taken with the GIL held).
 * The registry is leaked on purpose: python threads may still write something during process exit.
 */
namespace {
struct RunnerRegistry
{
    QRecursiveMutex mutex;
    QHash<QString, PyRunner *> runners;
};

RunnerRegistry &registry()
{
    static RunnerRegistry *instance = new RunnerRegistry;
    return *instance;
}

//the runner whose call is being processed by this thread
thread_local QString t_currentRunnerId;

class CurrentRunnerGuard
{
    QString m_previous;
public:
    explicit CurrentRunnerGuard(const QString &runnerId)
    {
        m_previous = t_currentRunnerId;
        t_currentRunnerId = runnerId;
    }
    ~CurrentRunnerGuard() { t_currentRunnerId = m_previous; }
};
}

#ifdef NRPYQT_DEBUG
#define PRINT_THREAD_INFO qDebug() << Q_FUNC_INFO << QThread::currentThread();
#else
#define PRINT_THREAD_INFO
#endif

PyRunner::~PyRunner()
{
    PRINT_THREAD_INFO
    //from now on nothing is routed to this runner anymore (this waits for what is being emitted),
    //it must be done before waiting for the GIL below
    {
        RunnerRegistry &reg = registry();
        QMutexLocker locker(&reg.mutex);
        if (reg.runners.value(m_runnerId) == this)
            reg.runners.remove(m_runnerId);
        m_pCallbackHandler = nullptr;
    }

    //stop the python thread first: a call that is running is allowed to finish, queued ones are dropped
    m_pPythonThread->quit();
    m_pPythonThread->wait();
    delete m_pPythonThread;

    if (Py_IsInitialized()) {
        PyGILState_STATE gstate = openCallContext();
        unloadCurrentModule();
        Py_XDECREF(m_module_dict);
        m_module_dict = nullptr;
        closeCallContext(gstate);
    }
}


PyRunner::PyRunner(QString scriptPath, QStringList dependecies)
{
    PRINT_THREAD_INFO
    m_sourceFilePy = scriptPath;
    m_dependencies = dependecies;
    m_module_dict = nullptr;
    qRegisterMetaType<PyFunctionCall>("PyFunctionCall");

    m_runnerId = "NrPyCpp-" + QFileInfo(scriptPath).completeBaseName() + QUuid::createUuid().toString();
    {
        RunnerRegistry &reg = registry();
        QMutexLocker locker(&reg.mutex);
        reg.runners.insert(m_runnerId, this);
    }

    m_pPythonThread = new QThread();
    m_pPythonThread->setObjectName(m_runnerId);
    m_pPythonThread->start();

    connect(this, &PyRunner::startCallRequestedSignal, this, &PyRunner::onStartCallRequest);

    this->moveToThread(m_pPythonThread);

    PRINT_THREAD_INFO

    setup();
}


/*!
 * \brief PyRunner::runnerId
 * \return the unique id of this runner, the nrpycpp python module uses it to route what the scripts send back
 */
QString PyRunner::runnerId()
{
    return m_runnerId;
}


/*!
 * \brief PyRunner::setCallbackHandler sets the handler that gets output, messages and exceptions of the scripts
 * \param pHandler the handler (not owned), or nullptr to remove it
 * \note the handler is called from the python threads with the GIL held; when this method returns
 * the previous handler is not in use anymore
 */
void PyRunner::setCallbackHandler(PyCallBackIface *pHandler)
{
    QMutexLocker locker(&registry().mutex);
    m_pCallbackHandler = pHandler;
}


QString PyRunner::currentRunnerId()
{
    return t_currentRunnerId;
}


/*!
 * \internal
 * \brief PyRunner::emitOutputLines emits the complete lines buffered for the stream
 * \param includePartialLine emit also the last line if it has no "\n" yet
 * \note must be called with the registry mutex held
 */
void PyRunner::emitOutputLines(int stream, quintptr threadKey, bool includePartialLine)
{
    QString buffer = m_outputBuffers[stream].take(threadKey);
    QStringList lines;
    int start = 0;
    int newLine;
    while ((newLine = buffer.indexOf(QLatin1Char('\n'), start)) >= 0) {
        lines << buffer.mid(start, newLine - start);
        start = newLine + 1;
    }
    buffer.remove(0, start);
    if (includePartialLine && !buffer.isEmpty()) {
        lines << buffer;
        buffer.clear();
    }
    if (!buffer.isEmpty())
        m_outputBuffers[stream].insert(threadKey, buffer);

    for (QString &line : lines) {
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        if (stream == StdErr)
            emit errorOutputReceived(line);
        else
            emit outputReceived(line);
        if (m_pCallbackHandler)
            m_pCallbackHandler->onStdOutput(QSharedPointer<QString>::create(line));
    }
}


bool PyRunner::routeMessage(const QString &runnerId, const QString &text)
{
    QMutexLocker locker(&registry().mutex);
    PyRunner *runner = registry().runners.value(runnerId, nullptr);
    if (!runner)
        return false;
    emit runner->messageReceived(text);
    if (runner->m_pCallbackHandler)
        runner->m_pCallbackHandler->onSendMessage(QSharedPointer<QString>::create(text));
    return true;
}


bool PyRunner::routeOutput(const QString &runnerId, int stream, quintptr threadKey, const QString &text)
{
    QMutexLocker locker(&registry().mutex);
    PyRunner *runner = registry().runners.value(runnerId, nullptr);
    if (!runner || (stream != StdOut && stream != StdErr))
        return false;
    runner->m_outputBuffers[stream][threadKey] += text;
    runner->emitOutputLines(stream, threadKey, false);
    return true;
}


bool PyRunner::flushOutput(const QString &runnerId, int stream, quintptr threadKey)
{
    QMutexLocker locker(&registry().mutex);
    PyRunner *runner = registry().runners.value(runnerId, nullptr);
    if (!runner || (stream != StdOut && stream != StdErr))
        return false;
    runner->emitOutputLines(stream, threadKey, true);
    return true;
}


bool PyRunner::routeException(const QString &runnerId, const QString &traceback)
{
    QMutexLocker locker(&registry().mutex);
    PyRunner *runner = registry().runners.value(runnerId, nullptr);
    if (!runner)
        return false;
    emit runner->unhandledException(traceback);
    if (runner->m_pCallbackHandler)
        runner->m_pCallbackHandler->onException(QSharedPointer<QString>::create(traceback));
    return true;
}


void PyRunner::setup()
{
    PRINT_THREAD_INFO
    QFileInfo scriptFileInfo(m_sourceFilePy);
    if(!scriptFileInfo.exists())
    {
        qWarning() << QString("[ERROR] Cannot find script: %1").arg(m_sourceFilePy);
        return;
    }

    m_scriptFileName = scriptFileInfo.completeBaseName();
    m_scriptFilePath = scriptFileInfo.absoluteDir().path();
}


PyGILState_STATE PyRunner::openCallContext()
{
    PyGILState_STATE gstate = PyGILState_Ensure();
    return gstate;
}


void PyRunner::closeCallContext(PyGILState_STATE state)
{
    PyGILState_Release(state);
}


/*!
 * \internal
 * \brief PyRunner::takePythonError fetches and clears the current python exception
 * \return the formatted traceback, or an empty string if no exception was set
 * \note must be called with the GIL held
 */
QString PyRunner::takePythonError()
{
#if PY_VERSION_HEX >= 0x030C0000
    PyObject *exc = PyErr_GetRaisedException(); //new reference
    if (!exc)
        return QString();
    PyObject *excType = Py_NewRef(reinterpret_cast<PyObject *>(Py_TYPE(exc)));
    PyObject *excValue = exc;
    PyObject *excTraceback = PyException_GetTraceback(exc); //new reference or NULL
#else
    PyObject *excType = nullptr;
    PyObject *excValue = nullptr;
    PyObject *excTraceback = nullptr;
    PyErr_Fetch(&excType, &excValue, &excTraceback);
    if (!excType)
        return QString();
    PyErr_NormalizeException(&excType, &excValue, &excTraceback);
#endif

    QString message;
    PyObject *tracebackModule = PyImport_ImportModule("traceback"); //new reference
    if (tracebackModule) {
        PyObject *lines = PyObject_CallMethod(tracebackModule, "format_exception", "OOO",
                                              excType,
                                              excValue ? excValue : Py_None,
                                              excTraceback ? excTraceback : Py_None); //new reference
        if (lines) {
            PyObject *separator = PyUnicode_FromString(""); //new reference
            PyObject *joined = separator ? PyUnicode_Join(separator, lines) : nullptr; //new reference
            if (joined) {
                const char *s = PyUnicode_AsUTF8(joined);
                if (s)
                    message = QString::fromUtf8(s).trimmed();
            }
            Py_XDECREF(joined);
            Py_XDECREF(separator);
            Py_DECREF(lines);
        }
        Py_DECREF(tracebackModule);
    }

    //fallback in case the traceback module is not usable
    if (message.isEmpty() && excValue) {
        PyErr_Clear();
        PyObject *str = PyObject_Str(excValue); //new reference
        const char *s = str ? PyUnicode_AsUTF8(str) : nullptr;
        message = QString("%1: %2").arg(Py_TYPE(excValue)->tp_name, s ? QString::fromUtf8(s) : QString());
        Py_XDECREF(str);
    }

    PyErr_Clear();
    Py_XDECREF(excType);
    Py_XDECREF(excValue);
    Py_XDECREF(excTraceback);
    return message;
}


void PyRunner::processCall(PyFunctionCall call)
{
    PRINT_THREAD_INFO
    PyFunctionCallResult &result = call.result();

    if (!Py_IsInitialized()) {
        result.error = true;
        result.errorMessage = "NrPyCpp ERROR: the python interpreter is not initialized (see the log for details)";
        handleCompletedCall(call);
        return;
    }

    if (m_scriptFileName.isEmpty()) {
        result.error = true;
        result.errorMessage = "NrPyCpp ERROR: cannot find script \"" + m_sourceFilePy + "\"";
        handleCompletedCall(call);
        return;
    }

    //whatever the script (and the threads it starts) prints or sends meanwhile belongs to this runner
    CurrentRunnerGuard currentRunner(m_runnerId);
    PyGILState_STATE gstate = openCallContext();

    QString errorMessage;
    PyObject * py_lib_mod_dict = getModuleDict(&errorMessage); //owned by this runner
    if (!py_lib_mod_dict) {
        result.error = true;
        result.errorMessage = "NrPyCpp ERROR: cannot load module \"" + m_sourceFilePy + "\"\n" + errorMessage;
    }

    PyObject * py_func = nullptr;
    if (!result.error) {
        py_func = PyDict_GetItemString(py_lib_mod_dict, call.functionName().toUtf8().constData()); //borrowed reference
        if (!py_func || !PyCallable_Check(py_func)) {
            result.error = true;
            result.errorMessage = "NrPyCpp ERROR: cannot find python function named \"" + call.functionName() + "\"";
        }
    }

    PyObject * py_args = nullptr;
    if (!result.error) {
        py_args = getTupleParams(call.params(), &errorMessage); //new reference
        if (!py_args) {
            result.error = true;
            result.errorMessage = "NrPyCpp ERROR calling \"" + call.functionName() + "\": " + errorMessage;
        }
    }

    if (!result.error) {
        //this is the actual python execution
        PyObject * py_ret = PyObject_CallObject(py_func, py_args); //new reference
        if (!py_ret) {
            result.error = true;
            result.errorMessage = takePythonError();
        } else {
            result.returnValue = parseObject(py_ret, &errorMessage);
            if (!errorMessage.isEmpty()) {
                result.error = true;
                result.errorMessage = "NrPyCpp ERROR retrieving result from \"" + call.functionName() + "\": " + errorMessage;
            }
            Py_DECREF(py_ret);
        }
    }

    Py_XDECREF(py_args);
    //never leave a pending exception behind, it would make the next call fail in odd ways
    if (PyErr_Occurred())
        PyErr_Clear();

    //a print(x, end="") has no line end: deliver it now, before the caller is told the call completed
    //(only what this thread wrote: the threads started by the call may be in the middle of a line)
    const quintptr threadKey = reinterpret_cast<quintptr>(PyThreadState_Get());
    flushOutput(m_runnerId, StdOut, threadKey);
    flushOutput(m_runnerId, StdErr, threadKey);

    closeCallContext(gstate);
    handleCompletedCall(call);
}


/*!
 * \internal
 * \brief PyRunner::trackCall will insert or update the call info during its process
 * \param call
 */
void PyRunner::trackCall(PyFunctionCall call)
{
    PRINT_THREAD_INFO
    QMutexLocker locker(&m_callsMutex);
    m_calls.insert(call.id(), call);
}


/*!
 * \brief PyRunner::printCalls utility function to print current calls
 */
void PyRunner::printCalls()
{
    PRINT_THREAD_INFO
    QMutexLocker locker(&m_callsMutex);
    for (const PyFunctionCall &call : std::as_const(m_calls))
    {
        qDebug() << call.getInfo();
    }
}


/*!
 * \internal
 * \brief PyRunner::getCall returns a copy of the call specified by its callid
 * \param callID
 * \return
 */
PyFunctionCall PyRunner::getCall(QUuid callID)
{
    PRINT_THREAD_INFO
    QMutexLocker locker(&m_callsMutex);
    return m_calls.value(callID);
}


/*!
 * \internal
 * \brief PyRunner::untrackCall remove the specified call from the tracking map
 * \param call
 * this is called when the function ha completed its processing
 */
void PyRunner::untrackCall(PyFunctionCall call)
{
    QMutexLocker locker(&m_callsMutex);
    m_calls.remove(call.id());
}


QString PyRunner::getCallInfo(QString id)
{
    QUuid uid(id);
    PyFunctionCall c = getCall(uid);
    return c.getInfo();
}


/*!
 * \brief PyRunner::getAsyncCallResult return the result info for the specified call
 * \param id
 * \return a PyFunctionCallResult containing all information about the finished call
 * \note if the result is valid (i.e. the call has finished processing) then the call is removed from
 * the tracking map so any following request for result of the same call will provide an invalid empty result
 */
PyFunctionCallResult PyRunner::getAsyncCallResult(QString id)
{
    QUuid uid(id);
    PyFunctionCall c = getCall(uid);
    PyFunctionCallResult result = c.result();
    if (!result.endTime.isValid()) {
        qDebug() << "The call with id " << id << " seems not to have finished yet...";
    } else {
        untrackCall(c);
    }
    return result;
}


/*!
 * \brief PyRunner::getAsyncCallsList
 * \return a list of the id of the calls that have not been requested their results
 * either because the processing did not end, or because the result has not been requested (for finished calls)
 */
QStringList PyRunner::getAsyncCallsList()
{
    QStringList ls;
    QMutexLocker locker(&m_callsMutex);
    for (const PyFunctionCall &c : std::as_const(m_calls)) {
        if (!c.isSync())
            ls << c.id().toString();
    }
    return ls;
}


/*!
 * \internal
 * \brief PyRunner::getModuleDict imports the script module (on first use) and returns its dictionary
 * \param errorMessage filled with the python traceback if the import fails
 * \return the module dictionary (owned by the runner) or NULL
 * \note if the import fails it will be retried on the next call, so a fixed script can be picked up
 */
PyObject * PyRunner::getModuleDict(QString *errorMessage)
{
    if(!m_module_dict)
    {
        loadCurrentModule();

        PyObject * py_lib_mod = PyImport_ImportModule(m_scriptFileName.toUtf8().constData()); //new reference
        if(!py_lib_mod)
        {
            *errorMessage = takePythonError();
            return nullptr;
        }

        m_module_dict = PyModule_GetDict(py_lib_mod); //borrowed reference
        Py_XINCREF(m_module_dict);
        Py_DECREF(py_lib_mod);
    }

    return m_module_dict;
}


/*!
 * \internal
 * \brief PyRunner::toPyObject transforms a c++ value into the corresponding python type
 * \param value
 * \param errorMessage filled in if the value type is not supported
 * \return a new reference, or NULL on error
 */
PyObject *PyRunner::toPyObject(const QVariant &value, QString *errorMessage)
{
    if (!value.isValid())
        Py_RETURN_NONE;

    switch (value.userType()) {
    case QMetaType::Bool:
        return PyBool_FromLong(value.toBool() ? 1 : 0);
    case QMetaType::Char:
    case QMetaType::SChar:
    case QMetaType::Short:
    case QMetaType::Int:
    case QMetaType::Long:
    case QMetaType::LongLong:
        return PyLong_FromLongLong(value.toLongLong());
    case QMetaType::UChar:
    case QMetaType::UShort:
    case QMetaType::UInt:
    case QMetaType::ULong:
    case QMetaType::ULongLong:
        return PyLong_FromUnsignedLongLong(value.toULongLong());
    case QMetaType::Float:
    case QMetaType::Double:
        return PyFloat_FromDouble(value.toDouble());
    case QMetaType::QString: {
        const QByteArray utf8 = value.toString().toUtf8();
        return PyUnicode_FromStringAndSize(utf8.constData(), utf8.size());
    }
    case QMetaType::QByteArray: {
        const QByteArray ba = value.toByteArray();
        return PyBytes_FromStringAndSize(ba.constData(), ba.size());
    }
    case QMetaType::QStringList:
    case QMetaType::QVariantList: {
        const QVariantList list = value.toList();
        PyObject *pyList = PyList_New(list.size());
        if (!pyList)
            return nullptr;
        for (int i = 0; i < list.size(); i++) {
            PyObject *item = toPyObject(list.at(i), errorMessage);
            if (!item) {
                Py_DECREF(pyList);
                return nullptr;
            }
            PyList_SET_ITEM(pyList, i, item); //steals the reference
        }
        return pyList;
    }
    case QMetaType::QVariantMap:
    case QMetaType::QVariantHash: {
        const QVariantMap map = value.toMap();
        PyObject *pyDict = PyDict_New();
        if (!pyDict)
            return nullptr;
        for (auto it = map.cbegin(); it != map.cend(); ++it) {
            PyObject *item = toPyObject(it.value(), errorMessage);
            if (!item || PyDict_SetItemString(pyDict, it.key().toUtf8().constData(), item) < 0) {
                Py_XDECREF(item);
                Py_DECREF(pyDict);
                return nullptr;
            }
            Py_DECREF(item);
        }
        return pyDict;
    }
    default:
        *errorMessage = QString("unsupported parameter type \"%1\"").arg(value.typeName());
        return nullptr;
    }
}


/*!
 * \internal
 * \brief PyRunner::getTupleParams transforms the c++ parameters into python types
 * \param params
 * \param errorMessage filled in if a parameter cannot be converted
 * \return a new reference to the arguments tuple, or NULL on error
 */
PyObject *PyRunner::getTupleParams(const QVariantList &params, QString *errorMessage)
{
    PRINT_THREAD_INFO
    PyObject *tup = PyTuple_New(params.size());
    if (!tup) {
        *errorMessage = takePythonError();
        return nullptr;
    }
    for (int i = 0; i < params.size(); i++) {
        PyObject *item = toPyObject(params.at(i), errorMessage);
        if (!item) {
            if (errorMessage->isEmpty())
                *errorMessage = takePythonError();
            *errorMessage = QString("parameter %1: %2").arg(i).arg(*errorMessage);
            Py_DECREF(tup);
            return nullptr;
        }
        PyTuple_SET_ITEM(tup, i, item); //steals the reference
    }
    return tup;
}


void PyRunner::printPyDict(PyObject *dict)
{
    QString err;
    qDebug() << "Dict size" << PyDict_Size(dict) << parseObject(dict, &err);
}


void PyRunner::printPyList(PyObject *list)
{
    QString err;
    qDebug() << "list size" << PyList_Size(list) << parseObject(list, &err);
}


void PyRunner::printPyTuple(PyObject *tuple)
{
    QString err;
    qDebug() << "tuple size" << PyTuple_Size(tuple) << parseObject(tuple, &err);
}

/*!
 * \internal
 * \brief PyRunner::parseObject parse the return value from the python call
 * \param object
 * \param errorMessage filled in if the object (or one of its items) cannot be converted
 * \return
 */
QVariant PyRunner::parseObject(PyObject *object, QString *errorMessage)
{
    PRINT_THREAD_INFO
    if (object == Py_None)
        return QVariant();

    //bool must come before int, since in python bool is a subclass of int
    if (PyBool_Check(object))
        return QVariant(object == Py_True);

    if (PyLong_Check(object)) {
        int overflow = 0;
        long long value = PyLong_AsLongLongAndOverflow(object, &overflow);
        if (!overflow) {
            if (value >= INT_MIN && value <= INT_MAX)
                return QVariant(static_cast<int>(value));
            return QVariant(static_cast<qlonglong>(value));
        }
        if (overflow > 0) {
            unsigned long long uvalue = PyLong_AsUnsignedLongLong(object);
            if (!PyErr_Occurred())
                return QVariant(static_cast<qulonglong>(uvalue));
            PyErr_Clear();
        }
        *errorMessage = "integer value does not fit in 64 bits";
        return QVariant();
    }

    if (PyFloat_Check(object))
        return QVariant(PyFloat_AsDouble(object));

    if (PyUnicode_Check(object)) {
        Py_ssize_t size = 0;
        const char *s = PyUnicode_AsUTF8AndSize(object, &size);
        if (!s) {
            *errorMessage = takePythonError();
            return QVariant();
        }
        return QVariant(QString::fromUtf8(s, size));
    }

    if (PyBytes_Check(object))
        return QVariant(QByteArray(PyBytes_AS_STRING(object), PyBytes_GET_SIZE(object)));

    if (PyByteArray_Check(object))
        return QVariant(QByteArray(PyByteArray_AS_STRING(object), PyByteArray_GET_SIZE(object)));

    if (PyList_Check(object) || PyTuple_Check(object)) {
        QVariantList list;
        const Py_ssize_t size = PySequence_Fast_GET_SIZE(object);
        PyObject **items = PySequence_Fast_ITEMS(object); //borrowed references
        for (Py_ssize_t i = 0; i < size; i++) {
            list.append(parseObject(items[i], errorMessage));
            if (!errorMessage->isEmpty())
                return QVariant();
        }
        return QVariant(list);
    }

    if (PyDict_Check(object)) {
        QVariantMap map;
        PyObject *key = nullptr;
        PyObject *value = nullptr;
        Py_ssize_t pos = 0;
        while (PyDict_Next(object, &pos, &key, &value)) { //borrowed references
            PyObject *keyStr = PyObject_Str(key); //new reference
            const char *k = keyStr ? PyUnicode_AsUTF8(keyStr) : nullptr;
            if (!k) {
                Py_XDECREF(keyStr);
                *errorMessage = takePythonError();
                return QVariant();
            }
            map.insert(QString::fromUtf8(k), parseObject(value, errorMessage));
            Py_DECREF(keyStr);
            if (!errorMessage->isEmpty())
                return QVariant();
        }
        return QVariant(map);
    }

    *errorMessage = QString("unsupported return type \"%1\"").arg(Py_TYPE(object)->tp_name);
    return QVariant();
}


/*!
 * \internal
 * \brief appendPathOnce appends path to the python list if it is not there yet
 * \return true if the path was added
 */
static bool appendPathOnce(PyObject *list, const QString &path)
{
    PyObject *item = PyUnicode_FromString(path.toUtf8().constData()); //new reference
    if (!item)
        return false;
    bool added = false;
    if (PySequence_Contains(list, item) == 0)
        added = PyList_Append(list, item) == 0;
    Py_DECREF(item);
    PyErr_Clear();
    return added;
}


/*!
 * \internal
 * \brief removePath removes the first occurrence of path from the python list (if any)
 */
static void removePath(PyObject *list, const QString &path)
{
    PyObject *item = PyUnicode_FromString(path.toUtf8().constData()); //new reference
    if (!item)
        return;
    Py_ssize_t index = PySequence_Index(list, item);
    if (index >= 0)
        PySequence_DelItem(list, index);
    Py_DECREF(item);
    PyErr_Clear();
}


void PyRunner::loadCurrentModule()
{
    if (!m_addedSearchPaths.isEmpty())
        return;

    PyObject* sys_path = PySys_GetObject("path"); //borrowed reference
    if (!sys_path || !PyList_Check(sys_path)) {
        qWarning() << "NrPyCpp: cannot access sys.path";
        return;
    }

    //add script path and dependencies to python search path
    //(we only track what we added, so paths that were already there are left alone on unload)
    if (appendPathOnce(sys_path, m_scriptFilePath))
        m_addedSearchPaths << m_scriptFilePath;
    for (const QString &dependency : std::as_const(m_dependencies))
    {
        if (appendPathOnce(sys_path, dependency))
            m_addedSearchPaths << dependency;
    }
}


void PyRunner::unloadCurrentModule()
{
    //unload imported paths
    PyObject* sys_path = PySys_GetObject("path"); //borrowed reference
    if (sys_path && PyList_Check(sys_path)) {
        for (const QString &path : std::as_const(m_addedSearchPaths))
            removePath(sys_path, path);
    }
    m_addedSearchPaths.clear();

    //and the module itself, so that a new runner on the same script will load it again
    if (m_scriptFileName.isEmpty())
        return;
    PyObject* modules = PyImport_GetModuleDict(); //borrowed reference
    if (modules && PyDict_GetItemString(modules, m_scriptFileName.toUtf8().constData()))
        PyDict_DelItemString(modules, m_scriptFileName.toUtf8().constData());
    PyErr_Clear();
}

/*!
 * \brief PyRunner::syncCallFunction calls the specified function synchronously i.e. will not return until the processing has finished
 * \param functionName the function to be called
 * \param params a list of params for the specified function
 * \return a PyFunctionCallResult struct with data about the function result: start and endtime, errors, etc.
 */
PyFunctionCallResult PyRunner::syncCallFunction(QString functionName, QVariantList params)
{
    PRINT_THREAD_INFO
    PyFunctionCall call(functionName, params, true);

    if (QThread::currentThread() == m_pPythonThread) {
        //called from the python thread itself (e.g. from a slot directly connected to callCompletedSignal):
        //waiting for the queued request would deadlock, so run it right away
        onStartCallRequest(call);
    } else {
        //The following signal will start the execution of the call in the python thread
        //while we wait for its completion here on the calling thread
        emit startCallRequestedSignal(call);
    }

    QMutexLocker locker(&m_callsMutex);
    while (!m_calls.value(call.id()).result().endTime.isValid())
        m_callsCondition.wait(&m_callsMutex);

    //update with the result from the map
    call = m_calls.take(call.id());
    return call.result();
}


/*!
 * \brief PyRunner::asyncCallFunction calls the specified function in an async manner (i.e. will return immediately and notify user when it completed)
 * \param functionName the name of the function to be executed asynchronously
 * \param params a list with the function params
 * \return the id of the call so that its result can be queried when ready
 */
QString PyRunner::asyncCallFunction(QString functionName, QVariantList params)
{
    PRINT_THREAD_INFO
    PyFunctionCall call(functionName, params, false);

    //track it right away, so that it is listed in getAsyncCallsList() until its result is retrieved
    trackCall(call);

    //this will signal to start execution of the specified call on the python thread
    emit startCallRequestedSignal(call);
    return call.id().toString();
}


/*!
 * \internal
 * \brief PyRunner::onStartCallRequest executes the specified call in python
 * \param call a copy of the call data structure that should be executed
 *
 * This is a slot and meant to be called only via signal so that it will be executed
 * on the correct thread (the python one).
 * The function will start by setting the startTime and then upserting the stored
 * value in the tracking map with trackCall() then will start the processing with processCall().
 */
void PyRunner::onStartCallRequest(PyFunctionCall call)
{
    PRINT_THREAD_INFO
    try {
        call.result().startTime = QDateTime::currentDateTime();
        trackCall(call);
        processCall(call);
    } catch (...)
    {
        //never leave a sync caller waiting forever
        call.result().error = true;
        call.result().errorMessage = "NrPyCpp ERROR: unexpected exception while processing the call";
        handleCompletedCall(call);
    }

}


/*!
 * \internal
 * \brief PyRunner::handleCompletedCall will track the end time and then signal if it is an async call that result is avalaible
 * \param call the call that has just completed
 * \note this is only called within processCall() so maybe it could be refactored in to that method
 */
void PyRunner::handleCompletedCall(PyFunctionCall call)
{
    PRINT_THREAD_INFO
    call.result().endTime = QDateTime::currentDateTime();
    {
        QMutexLocker locker(&m_callsMutex);
        m_calls.insert(call.id(), call);
        m_callsCondition.wakeAll();
    }
    if (!call.isSync()) {
        emit callCompletedSignal(call.id().toString());
    }
}
