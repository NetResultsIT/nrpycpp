/**************************************************************************
 *  Copyright (C) 2022 by NetResults S.r.l. ( http://www.netresults.it )  *
 *  Author( s ) :                                                         *
 *         Stefano Aru                  <s.aru@netresults.it>             *
 *         Francesco Lamonica      <f.lamonica@netresults.it>             *
 **************************************************************************/

#include "pyrunner.h"

#include <QDebug>
#include <QSharedPointer>
#include <QVariant>
#include <pyenvironment.h>

#include "sleep_header.h"

#define NRPYQT_DEBUG_REMOVETOENABLE

#ifdef NRPYQT_DEBUG
#define PRINT_THREAD_INFO qDebug() << Q_FUNC_INFO << QThread::currentThread();
#else
#define PRINT_THREAD_INFO
#endif

PyRunner::~PyRunner()
{
    PRINT_THREAD_INFO
    /*
     * Thread will self destroy when the finished() signal arrives
    */

//    delete m_pPythonThread;
    m_pCallbackHandler = nullptr;

    m_pPythonThread->exit();
    m_pPythonThread->wait();
    m_pPythonThread->deleteLater();
//    delete m_pPythonThread;
//    tearDown();
    Py_DecRef(m_module_dict);
    //qDebug()<<Py_REFCNT(m_module_dict);
    while(m_calls.count()>0)
    {
        PyFunctionCall * call =  m_calls.values().last();
        m_calls.remove(call->id());
        delete call;
    }
    PyEnvironment::getInstance().untrackRunner(m_runnerId);
    delete m_pStdOutputCallBackBuffer;
    qDebug() << "PyRunner dtor done";
}


PyRunner::PyRunner(QString scriptPath, QStringList dependecies)
{
    PRINT_THREAD_INFO
    m_pStdOutputCallBackBuffer = new QByteArray();
    m_syntaxError = false;
    m_sourceFilePy = scriptPath;
    m_dependencies = dependecies;
    m_module_dict = NULL;
    m_pCallbackHandler = nullptr;

    qRegisterMetaType<PyFunctionCall>("PyFunctionCall");
    qRegisterMetaType<PyRunner>("PyRunner");

    m_runnerId = "NrPyCpp-" + QFileInfo(scriptPath).completeBaseName() + QUuid::createUuid().toString();
    m_pPythonThread = new QThread();
    m_pPythonThread->setObjectName(m_runnerId);
    PyEnvironment::getInstance().trackRunner(m_runnerId, this);

//    connect(m_pPythonThread, &QThread::finished, m_pPythonThread, &QThread::deleteLater);
    connect(m_pPythonThread, &QThread::finished, this, &PyRunner::tearDown);
    connect(m_pPythonThread, &QThread::started, this, &PyRunner::loadStuff);
    connect(this, &PyRunner::startCallRequestedSignal, this, &PyRunner::onStartCallRequest);
//    connect(this, &PyRunner::tearDownSignal, this, &PyRunner::tearDown);

    m_pPythonThread->start();
    this->moveToThread(m_pPythonThread);

    PRINT_THREAD_INFO

    //The call to emit in the ctor makes no sense, especially with parenthesis that is not an emit but a function call
    //emit(setupSignal());
    //let's call stup directly
    setup();
}


//This method should probably inserted within the constructor, i don't get how an exception could be thrown...
void PyRunner::setup()
{
    PRINT_THREAD_INFO
    try {
        QFileInfo scriptFileInfo(m_sourceFilePy);
        if(!scriptFileInfo.exists())
        {
            qDebug() << QString("[ERROR] Cannot find script: %1")
                            .arg(m_sourceFilePy);
            return;
        }

        m_scriptFileName = scriptFileInfo.completeBaseName();
        m_scriptFilePath = scriptFileInfo.dir().path();

//        loadCurrentModule();

    }
    catch(...)
    {
        /*
         *  who is generating the exception?
         */
        qCritical() << "[ERROR] cannot setup pyrunner";
        PyErr_Print();
    }
}


void PyRunner::tearDown()
{
    PRINT_THREAD_INFO

//    m_pPythonThread->exit();
//    m_pPythonThread->wait();
//    m_pPythonThread->deleteLater();
    unloadCurrentModule();
    // PyEval_RestoreThread(m_pPyThreadState);
    // Py_Finalize();
}


PyGILState_STATE PyRunner::openCallContext()
{
    PyGILState_STATE gstate = PyGILState_Ensure();
    return gstate;
}


void PyRunner::closeCallContext(PyGILState_STATE state)
{
    PyGILState_Release(state);

    //not needed https://stackoverflow.com/questions/8451334/why-is-pygilstate-release-throwing-fatal-python-errors
    //PyEval_ReleaseLock();
}


QString PyRunner::runnerId() const
{
    return m_runnerId;
}


void PyRunner::processCall(PyFunctionCall * call)
{
    PRINT_THREAD_INFO
     //TODO - I don't get this check, syntax_error is set only in the syncCallFunction
    // but after the process call is happening, so how can this enter the check? (2022-01-25 FL)
    if(m_syntaxError)
    {
        handleCompletedCall(call);
        return;
    }

    try {
        PyGILState_STATE gstate = openCallContext();



        PyObject * py_lib_mod_dict = getModuleDict(); //borrowed reference of global variable
        if ( !py_lib_mod_dict ) {
            //FIXME - log error and return (2022-01-26 FL)
            qCritical() << "CRITICAL - cannot get Python module dictionary...";
        }

        Py_IncRef(py_lib_mod_dict);

        if (!py_lib_mod_dict)
        {
            call->result().error = true;
            call->result().errorMessage.append("PyQAC ERROR: Cannot find module at \"" + m_sourceFilePy+"\"!");
            //FIXME - should not we return an error? (2022-01-26 FL)
            qCritical() << "CRITICAL - cannot use Python module dictionary...";
        }



        char * p = new char[call->functionName().length() + 1];
        QSharedPointer<char> function = QSharedPointer<char>(p);
        strcpy(function.data(), call->functionName().toUtf8().constData());

        //get function name
        PyObject * py_function_name = NULL;

        //PyString_FromString
        if (!call->result().error)
            py_function_name = PyUnicode_FromString(function.data()); //new reference

        if (!py_function_name)
            call->result().error = true;



        //get function object
        PyObject * py_func = NULL;
        if (!call->result().error) {
            py_func = PyDict_GetItem(py_lib_mod_dict, py_function_name); //borrowed reference
            Py_IncRef(py_func);//maybe we can remove this
        }

        if (!py_func) {
            call->result().error = true;
            call->result().errorMessage.append("ERROR: cannot find python function named \"" + call->functionName() +"\"!");
            qDebug() << call->result().errorMessage;
            //FIXME - shouldn't we return here? (2022-01-26 FL)
        }


        PyObject * py_args = getTupleParams(call->params());//New reference

        PyObject * py_ret = NULL;

        if (!call->result().error) {
            //this is the actual python execution
            py_ret = PyObject_CallObject(py_func, py_args);//New reference
        }



        if (!py_ret)
            call->result().error = true;

        if (!call->result().error) {
               call->result().returnValue = parseObject(py_ret);
            Py_DecRef(py_ret);
        } else {
            PyErr_Print();
            call->result().errorMessage.append("NrPyCpp ERROR retrieving result from function named \"" + call->functionName() + "\"!");
        }

        Py_DecRef(py_lib_mod_dict);
        Py_DecRef(py_function_name);
        Py_DecRef(py_func);
        Py_DecRef(py_args);

        closeCallContext(gstate);
        handleCompletedCall(call);
    } catch (...) {
        //qDebug() << e.what();
        PyErr_Print();
    }
}


/*!
 * \internal
 * \brief PyRunner::trackCall will insert or update the call info during its process
 * \param call
 */
void PyRunner::trackCall(PyFunctionCall * call)
{
    PRINT_THREAD_INFO
    m_callsMutex.lock();
    m_calls.insert(call->id(), call);
    m_callsMutex.unlock();
}


/*!
 * \brief PyRunner::printCalls utility function to print current calls
 */
void PyRunner::printCalls()
{
    PRINT_THREAD_INFO
    m_callsMutex.lock();
    foreach(PyFunctionCall *call, m_calls)
    {
        qDebug() << call->getInfo();
    }
    m_callsMutex.unlock();
}


/*!
 * \internal
 * \brief PyRunner::getCall returns a copy of the call specified by its callid
 * \param callID
 * \return
 */
PyFunctionCall * PyRunner::getCall(QUuid callID)
{
    PRINT_THREAD_INFO
    m_callsMutex.lock();
    PyFunctionCall * returnValue = m_calls.value(callID);
    m_callsMutex.unlock();
    return returnValue;
}


/*!
 * \internal
 * \brief PyRunner::untrackCall remove the specified call from the tracking map
 * \param call
 * this is called when the function ha completed its processing
 */
void PyRunner::untrackCall(PyFunctionCall * call)
{
    m_callsMutex.lock();
    m_calls.remove(call->id());
    m_callsMutex.unlock();
    delete call;
}


QString PyRunner::getCallInfo(QString id)
{
    QUuid uid(id);
    PyFunctionCall * c = getCall(uid);
    return c->getInfo();
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
    PyFunctionCall * c = getCall(uid);
    PyFunctionCallResult result = c->result();
    if (!result.endTime.isValid()) {
        qDebug() << "The call with id " << id << " seems not to have finished yet...";
    } else {
        qDebug() << "Call with id " << id << " seems to have completed processing, removing from async call list";
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
    m_callsMutex.lock();
    foreach (PyFunctionCall * c, m_calls) {
        ls << c->id().toString();
    }
    m_callsMutex.unlock();
    return ls;
}


/*!
 * \internal
 * \brief PyRunner::checkCall verifies if the returning call has the callID requested
 * \param callID
 * \return true if the result arrived, false otherwise
 * \note this is executed on the application thread (it is called from the syncCallFunction())
 */
bool PyRunner::checkCall(QUuid callID)
{
    PRINT_THREAD_INFO
    PyFunctionCall * c = getCall(callID);

    if (!c->isValid()) {
        qDebug() << "Call with id " << callID << " has not yet been started";
        return false;
    }

    //qDebug() << "Checking call with id " << callID << " for completion...";

    if (!c->result().endTime.isValid()) {
        qDebug() << "Call with id " << callID << " has not yet completed";
        return false;
    }

    return true;
}

void PyRunner::loadRedirectOutput()
{
    QString redirectClass = R"(
import sys
import threading
import ctypes
import logging
from io import StringIO
from typing import List, Optional

class NrPyCpp:
    @staticmethod
    def runnerId():
        return "[RUNNER_ID]"

    @staticmethod
    def send_message(message):
        callbackHandler = _NrPyCpp()
        callbackHandler.send_message(message)

class _NrPyCpp:

    def __init__(self):
        self.runnerId = NrPyCpp.runnerId()
        self.lib = ctypes.CDLL("[LIBRARY_PLACE_HOLDER]")
        self._nrpycpp_write_callback = self.lib._nrpycpp_write_callback
        self._nrpycpp_write_callback.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        self._nrpycpp_write_callback.restype = None

        self._nrpycpp_flush_callback = self.lib._nrpycpp_flush_callback
        self._nrpycpp_flush_callback.argtypes = [ctypes.c_char_p]
        self._nrpycpp_flush_callback.restype = None

        self._nrpycpp_exception_callback = self.lib._nrpycpp_exception_callback
        self._nrpycpp_exception_callback.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        self._nrpycpp_exception_callback.restype = None

        self._nrpycpp_send_message_callback = self.lib._nrpycpp_send_message_callback
        self._nrpycpp_send_message_callback.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        self._nrpycpp_send_message_callback.restype = None

        sys.excepthook = self.handle_exception
        threading.excepthook = self.handle_thread_exception

    def write(self, s):
        self._nrpycpp_write_callback(s.encode('utf-8'), self.runnerId.encode('utf-8'))

    def flush(self):
        self._nrpycpp_flush_callback(self.runnerId.encode('utf-8'))

    def send_message(self, message):
        self._nrpycpp_send_message_callback(message.encode('utf-8'), self.runnerId.encode('utf-8'))

    def handle_exception(self, exc_type, exc_value, exc_traceback):
        message = f"{exc_type.__name__}: {exc_value}"
        self._nrpycpp_exception_callback(message.encode('utf-8'), self.runnerId.encode('utf-8'))

    def handle_thread_exception(self, args):
        message = f"[Thread {args.thread.name}] {args.exc_type.__name__}: {args.exc_value}"
        self._nrpycpp_exception_callback(message.encode('utf-8'), self.runnerId.encode('utf-8'))


class _InterceptedStream:
    """
    Wrapper per sys.stdout/sys.stderr.
    Tutto ciò che viene scritto qui viene intercettato e inoltrato a ConsoleInterceptor.
    """
    def __init__(self, name: str, original_stream, interceptor):
        self.name = name
        self._original = original_stream
        self._interceptor = interceptor

    def write(self, text):
        if not text:
            return
        # registra la stampa nel buffer dell'interceptor
        self._interceptor._capture_print(self.name, text)
        # inoltra al flusso originale se richiesto
        if self._interceptor.forward_prints:
            self._original.write(text)

    def flush(self):
        try:
            self._original.flush()
        except Exception:
            pass

    def __getattr__(self, item):
        # delega altri attributi (es. encoding, fileno, ecc.)
        return getattr(self._original, item)


class ConsoleInterceptor(logging.Handler):
    _instance_lock = threading.Lock()
    _instance = None

    def __new__(cls, *args, **kwargs):
        with cls._instance_lock:
            if cls._instance is not None:
                raise RuntimeError("ConsoleInterceptor è già stato istanziato.")
            instance = super().__new__(cls)
            cls._instance = instance
            return instance

    def __init__(self, forward_to_original: bool = True, forward_prints: bool = False, fmt: Optional[str] = None):
        """
        forward_to_original: True -> inoltra anche i log logging agli handler originali
        forward_prints: True -> inoltra le print() alla console originale
        fmt: formato messaggi logging
        """
        super().__init__(level=logging.NOTSET)
        self._lock = threading.RLock()
        self.forward_logs = forward_to_original
        self.forward_prints = forward_prints

        self._root = logging.getLogger()
        self._original_stdout = sys.stdout
        self._original_stderr = sys.stderr
        self._original_handlers = []
        self._stopped = False

        if fmt:
            self.setFormatter(logging.Formatter(fmt))

        self._replace_logging_handlers()
        self._replace_print_streams()

        # Imposta root logger per non perdere messaggi
        if self._root.level != logging.NOTSET:
            self._root.setLevel(logging.NOTSET)

        if self not in self._root.handlers:
            self._root.addHandler(self)

    # --- gestione logging ---
    def _replace_logging_handlers(self):
        with self._lock:
            new_handlers = []
            for h in self._root.handlers:
                if isinstance(h, logging.StreamHandler) and getattr(h, "stream", None) in (sys.stdout, sys.stderr):
                    self._original_handlers.append(h)
                else:
                    new_handlers.append(h)
            self._root.handlers = new_handlers

    def emit(self, record: logging.LogRecord) -> None:
        try:
            with self._lock:

                msg = self.format(record)
                callbackHandler = _NrPyCpp()
                callbackHandler.write(msg)


                if self.forward_logs:
                    for orig in self._original_handlers:
                        try:
                            stream = getattr(orig, "stream", None)
                            if stream:
                                stream.write(msg + "\n")
                                stream.flush()
                        except Exception:
                            continue
        except Exception:
            self.handleError(record)

    # --- gestione print() ---
    def _replace_print_streams(self):
        sys.stdout = _InterceptedStream("stdout", sys.stdout, self)
        sys.stderr = _InterceptedStream("stderr", sys.stderr, self)

    def _capture_print(self, stream_name: str, text: str):
        with self._lock:
            # puoi anche aggiungere tag tipo "[STDOUT]" o "[STDERR]"

            callbackHandler = _NrPyCpp()
            callbackHandler.write(text)

    def stop(self):
        """Ripristina sys.stdout/stderr e gli handler logging originali."""
        with self._lock:
            if self._stopped:
                return

            # ripristina i flussi originali
            sys.stdout = self._original_stdout
            sys.stderr = self._original_stderr

            # ripristina gli handler logging
            for h in self._original_handlers:
                if h not in self._root.handlers:
                    self._root.addHandler(h)

            try:
                self._root.removeHandler(self)
            except Exception:
                pass

            self._stopped = True


__interceptor = ConsoleInterceptor(fmt="%(levelname)s: %(message)s")
)";

#ifdef QT_DEBUG
    redirectClass.replace("[LIBRARY_PLACE_HOLDER]", "libnrPyCpp_d.so.1");
#else
    redirectClass.replace("[LIBRARY_PLACE_HOLDER]", "libnrPyCpp.so.1");
#endif

    redirectClass.replace("[RUNNER_ID]", m_runnerId);
    //qDebug()<<qPrintable(redirectClass);
    PyObject* result = PyRun_String(
        qPrintable(redirectClass),
        Py_file_input,
        static_cast<PyObject*>(m_module_dict),  // globals
        static_cast<PyObject*>(m_module_dict)   // locals (usa lo stesso dizionario)
        );

    if (!result) {
        PyErr_Print();
        qDebug() << "Errore nell'esecuzione del codice Python.\n";
    } else {
        Py_DECREF(result);  // Pulizia
    }
}

void PyRunner::onStdOutputWriteCallBack(const char *s)
{
    m_pStdOutputCallBackBuffer->append(s);
}

void PyRunner::onStdOutputFlushCallback()
{
    // const QString * log = new QString(*m_pStdOutputCallBackBuffer);

    const QSharedPointer<QString> log =  QSharedPointer<QString>(new QString(*m_pStdOutputCallBackBuffer));
    //qDebug()<<m_pStdOutputCallBackBuffer->toStdString();
    if (m_pCallbackHandler)
    {
        m_pCallbackHandler->onStdOutput(log);
    }
    m_pStdOutputCallBackBuffer->clear();

}

void PyRunner::onExceptionCallback(const char *exceptionMsg)
{
    const QSharedPointer<QString> log =  QSharedPointer<QString>(new QString(QString::fromUtf8(exceptionMsg)));
    if (m_pCallbackHandler)
    {
        m_pCallbackHandler->onException(log);
    }
    m_syntaxError = true;
}

void PyRunner::onSendMessage(const char *s)
{
    const QSharedPointer<QString> msg =  QSharedPointer<QString>(new QString(QString::fromUtf8(s)));
    if (m_pCallbackHandler)
    {
        m_pCallbackHandler->onSendMessage(msg);
    }
}

PyObject * PyRunner::getModuleDict()
{
    if(!m_module_dict)
    {
        //loadCurrentModule();

        PyObject * scriptName = PyUnicode_FromString(qPrintable(m_scriptFileName)); //new reference

        //script meta data
        PyObject * m_py_lib_mod = PyImport_Import(scriptName); //new reference

        if(!m_py_lib_mod)
        {
            //qDebug()<<"problem";
            PyErr_Print();
            m_module_dict = NULL;
            return NULL;
        }

        m_module_dict = PyModule_GetDict(m_py_lib_mod);//borrowed reference

        //this one leaks
        Py_IncRef(m_module_dict);

        if(!m_module_dict)
        {
            //qDebug()<<"problem";
            PyErr_Print();
            m_module_dict = NULL;
            return NULL;
        }
//        printPyDict(m_module_dict);

        loadRedirectOutput();

        Py_DecRef(m_py_lib_mod);
        Py_DecRef(scriptName);
    }

    return  m_module_dict;
}


/*!
 * \internal
 * \brief PyRunner::getTupleParams transforms the c++ parameters into python types
 * \param params
 * \return
 */
PyObject *PyRunner::getTupleParams(QVariantList params)//New reference
{
    PRINT_THREAD_INFO
    if(params.size()==0) {
        return NULL;
    }

    PyErr_Print();//if you remove this, python >3 stops working

    PyObject *tup = PyTuple_New(params.size());//New Reference
    for (int i = 0; i < params.size(); i++) {
        if (params[i].typeId() == QVariant::String)
        {
            PyObject * stringValue = PyUnicode_FromString(qPrintable(params[i].toString())); //new reference
            PyTuple_SET_ITEM(tup, i, stringValue);
            // Note that PyTuple_SET_ITEM steals the reference we get from PyLong_FromLong.
        } else if (params[i].typeId() == QVariant::Int || params[i].type() == QVariant::LongLong)
        {
            PyObject * longValue = PyLong_FromLong(params[i].toLongLong()); //new reference
            PyTuple_SET_ITEM(tup, i, longValue);
            // Note that PyTuple_SET_ITEM steals the reference we get from PyLong_FromLong.
        } else if (params[i].typeId() == QVariant::UInt || params[i].type() == QVariant::ULongLong)
        {
            PyObject * longValue = PyLong_FromLong(params[i].toLongLong()); //new reference
            PyTuple_SET_ITEM(tup, i, longValue);
            // Note that PyTuple_SET_ITEM steals the reference we get from PyLong_FromLong.
        } else if (params[i].typeId() == QVariant::Double)
        {
            PyObject * floatValue = PyFloat_FromDouble(params[i].toDouble()); //new reference
            PyTuple_SET_ITEM(tup, i, floatValue);
            // Note that PyTuple_SET_ITEM steals the reference we get from PyLong_FromLong.
        } else if (params[i].typeId() == QVariant::Bool)
        {
            PyObject * boolValue = PyBool_FromLong(params[i].toInt()); //new reference
            PyTuple_SET_ITEM(tup, i, boolValue);
            // Note that PyTuple_SET_ITEM steals the reference we get from PyLong_FromLong.
            // PyTuple_SET_ITEM(tup, i, PyBool_FromLong(params[i].toInt())); //FIXME - this conversion is flaky
        } else if(params[i].typeId()==QVariant::ByteArray)
        {
            //PyObject * bytesParam = Py_BuildValue("y#", params[i].toByteArray().constData(),params[i].toByteArray().size() -1 );
            PyObject * bytesParam = PyBytes_FromStringAndSize(params[i].toByteArray().constData(), params[i].toByteArray().size());
            PyTuple_SET_ITEM(tup, i, bytesParam);
            // Note that PyTuple_SET_ITEM steals the reference we get from PyLong_FromLong.
        }else
        {
            // PyObject * myrunner = PyObject_New(QObject, NULL);
            qDebug()<<"CANNOT PARSE TYPE"<<params[i].typeId();
            qDebug()<<"CANNOT PARSE TYPE"<<params[i].metaType();
            qDebug()<<"CANNOT PARSE TYPE"<<params[i].typeName();
        }
        // Note that PyTuple_SET_ITEM steals the reference we get from PyLong_FromLong.
    }
    return tup;
}


void PyRunner::printPyDict(PyObject *dict)
{
    PyObject * keys = PyDict_Keys(dict); //New Reference
    int size = static_cast<int>(PyList_Size(keys));
    qDebug()<< "Dict size "<<size;
    for (int i=0; i<size; i++)
    {
        PyObject * key = PyList_GetItem(keys, i); //Borrowed reference
        PyObject * value = PyDict_GetItem(dict, key); //Borrowed reference

        QString parsedKey = parseObject(key).toString();
        QVariant parsedValue = parseObject(value);

        qDebug()<<i <<"key"<<parsedKey<< " : value"<<parsedValue;
    }
    Py_DecRef(keys);
}


void PyRunner::printPyList(PyObject *list)
{
    int size = static_cast<int>(PyList_Size(list));
    qDebug()<< "list size "<<size;
    for (int i=0; i<size; i++)
    {
        PyObject * tupleValue = PyList_GetItem(list,i); //Borrowed reference
        qDebug()<<"value"<<i<<parseObject(tupleValue);
    }
}


void PyRunner::printPyTuple(PyObject *tuple)
{
    int tupleSize = static_cast<int>(PyTuple_Size(tuple));
    qDebug()<< "tuple size "<<tupleSize;
    for (int i=0; i<tupleSize; i++)
    {
        PyObject * tupleValue = PyTuple_GetItem(tuple,i); //Borrowed reference
        qDebug()<<"value"<<i<<parseObject(tupleValue);
    }
}

/*!
 * \internal
 * \brief PyRunner::parseObject parse the return value from the python call
 * \param object
 * \return
 */
QVariant PyRunner::parseObject(PyObject *object)
{
    PRINT_THREAD_INFO
    PyTypeObject* type = object->ob_type;
    const char* p = type->tp_name;

    QVariant returnValue;

    if(QString("str").compare(p)==0)
    {
        PyObject* objectsRepresentation = PyObject_Repr(object);//new reference

        /* Python 3.9 ONLY code - we dismissed Python 2.7 */
        PyObject* str = PyUnicode_AsEncodedString(objectsRepresentation, "utf-8", "~E~");//new reference
        if(str)
        {
            const char* s = PyBytes_AsString(str);
            QString valueString = QString(s);
            valueString = valueString.mid(1,valueString.length()-2);
            returnValue.setValue(valueString);
            Py_DecRef(str);
            PyErr_Print();
        }else
        {
            returnValue.setValue(QString(""));
        }
        Py_DecRef(objectsRepresentation);

    } else if(PyLong_CheckExact(object) ) {
        int value = PyLong_AsLong(object);
        returnValue.setValue(value);
    } else if(PyBool_Check(object) ) {
        int intbool = PyObject_IsTrue(object);
        bool value = (intbool==1?true:false);
        returnValue.setValue(value);
    } else if(PyFloat_Check(object)) {
        float value = PyFloat_AsDouble(object);
        returnValue.setValue(value);
    } else if(QString("NoneType").compare(p)==0) {
        //do nothing
    } else if(PyBytes_Check(object)) {
        Py_ssize_t size = PyBytes_Size(object);
        char * b = PyBytes_AsString(object);
        if(b)
        {
            QByteArray ba = QByteArray(b, static_cast<int>(size));
            returnValue.setValue(ba);
        }
    } else if(PyByteArray_Check(object)) {
        Py_ssize_t size = PyByteArray_Size(object);
        char * b = PyByteArray_AsString(object);
        if(b)
        {
            QByteArray ba = QByteArray(b, static_cast<int>(size));
            returnValue.setValue(ba);
        }
    } else if(PyList_Check(object)) {
        QVariantList list;
        int size = PyList_Size(object);
        for(int i=0;i<size;i++)
        {
             PyObject*iObject = PyList_GetItem(object, i); //Borrowed reference
             QVariant iVariant = parseObject(iObject);
             list.append(iVariant);
        }
        returnValue.setValue(list);
    } else if(PyDict_Check(object)) {
        QVariantMap dict;

        PyObject * keys = PyDict_Keys(object); //New Reference
        int size = static_cast<int>(PyList_Size(keys));
        for(int i=0;i<size;i++)
        {
            PyObject * key = PyList_GetItem(keys, i); //Borrowed reference
            PyObject * value = PyDict_GetItem(object, key); //Borrowed reference
            QVariant parsedKey = parseObject(key);
            QVariant parsedValue = parseObject(value);
            dict.insert(parsedKey.toString(), parsedValue);
        }
        Py_DecRef(keys);
        returnValue.setValue(dict);
    }else if(PyModule_Check(object)) {
        QString moduleName = PyModule_GetName(object);
//        PyObject * moduleFileName_obj = PyModule_GetFilenameObject(object);//new reference is valid

        QString moduleFileName = "";
//        if(moduleFileName_obj){
//            moduleFileName = parseObject(moduleFileName_obj).toString();
//            Py_DecRef(moduleFileName_obj);
//        }
        QVariantMap moduleDict;
        moduleDict.insert("name", moduleName);
        moduleDict.insert("filename", moduleFileName);
        returnValue.setValue(moduleDict);
    }else if(PyType_Check(object)) {
        returnValue.setValue(QVariant( "Type"));
    }else {
        qWarning() << "Warning: attempt to parse unknown type" << p;
    }

    PyErr_Print();
    return returnValue;
}


void PyRunner::loadCurrentModule()
{
    // Py_Initialize();
    // m_pPyThreadState = PyEval_SaveThread();
    PyGILState_STATE gstate = openCallContext();
    //loadRedirectOutput();
    PyObject* sys = PyImport_ImportModule( "sys" ); //new reference
    PyObject* sys_path = PyObject_GetAttrString( sys, "path" ); //new reference
    PyObject* folder_path = PyUnicode_FromString( qPrintable(m_scriptFilePath) ); //new reference
    PyObject* sys_modules = PyObject_GetAttrString( sys, "modules" ); //new reference
//    printPyDict(sys_modules);
//    printPyList(sys_path);
    QVariantMap modules = parseObject(sys_modules).toMap();
    m_defaultLoadedModules = modules.keys();
    m_defaultModulePathsCount = static_cast<int>(PyList_Size(sys_path));
    m_defaultModulesCount = static_cast<int>(PyDict_Size(sys_modules));
    //add script path to python search path
    PyList_Append( sys_path, folder_path );

    //add dependecies path to python search path
    foreach (QString dependecy, m_dependencies)
    {
        PyObject* dependency_path = PyUnicode_FromString(qPrintable(dependecy));//new reference
        PyList_Append( sys_path, dependency_path);
        Py_DecRef(dependency_path);
    }

    Py_DecRef(sys);
    Py_DecRef(sys_path);
    Py_DecRef(folder_path);
    Py_DecRef(sys_modules);
    closeCallContext(gstate);
}


void PyRunner::unloadCurrentModule()
{

//    return;
    PyGILState_STATE gstate = openCallContext();
    //PyEval_InitThreads();
    PyObject* sys = PyImport_ImportModule( "sys" ); //new reference
    PyObject* sys_path = PyObject_GetAttrString( sys, "path" ); //new reference
    PyObject* sys_modules = PyObject_GetAttrString( sys, "modules" ); //new reference

//    printPyList(sys_path);
//    printPyDict(sys_modules);
    //unload imported paths
    QString unloadPathCommand = "sys.path.remove(\""+m_scriptFilePath+"\")";
    QString unloadModuleCommand = "del sys.modules[\""+m_scriptFileName+"\"]";
    PyRun_SimpleString("import sys");
    PyObject* scriptFileName = PyUnicode_FromString(qPrintable(m_scriptFileName));//new reference
    if (PyDict_Contains(sys_modules, scriptFileName))
    {
        PyRun_SimpleString(qPrintable(unloadPathCommand));

        PyErr_Print();
        //PyRun_SimpleString(unloadModuleCommand.toUtf8().data());
        PyDict_DelItem(sys_modules,scriptFileName);
        PyErr_Print();
    }

    foreach (QString dependency, m_dependencies)
    {
        unloadPathCommand = "sys.path.remove(\""+dependency+"\")";
        PyRun_SimpleString(qPrintable(unloadPathCommand));
        PyErr_Print();
    }

    QVariantMap modules = parseObject(sys_modules).toMap();
    //for(int i=m_defaultModulesCount; i< modules.keys().size(); i++)
    foreach (QString module, modules.keys()) {
        if( m_defaultLoadedModules.contains(module) )
            continue;
        PyObject * moduleName = PyUnicode_FromString(qPrintable(module));//new reference
        if ( PyDict_Contains(sys_modules, moduleName) )
        {
            PyDict_DelItem(sys_modules, moduleName);
            PyErr_Print();
        }
        Py_DecRef(moduleName);
    }
    QVariantList modulePaths = parseObject(sys_path).toList();
    for(int i=m_defaultModulePathsCount; i< modulePaths.length(); i++)
    {
        unloadPathCommand = "sys.path.remove(\""+ modulePaths.at(i).toString()  +"\")";
        PyRun_SimpleString(unloadPathCommand.toUtf8().data());
        PyErr_Print();
    }

    // PyRun_SimpleString("import gc");
    // PyRun_SimpleString("gc.collect()");

    Py_DecRef(scriptFileName);
    Py_DecRef(sys);
    Py_DecRef(sys_path);
    Py_DecRef(sys_modules);
//    PyGILState_Release(gstate);
    closeCallContext(gstate);
//    PyEval_ReleaseLock();
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
    PyFunctionCall * call= new PyFunctionCall(functionName, params, true);

    call->syncCallLock()->lock();

    //The following signal will start the execution of the call in the python thread
    //while we wait for its completion here on the app thread
    emit startCallRequestedSignal(call);

    call->syncCallLock()->lock();

//    while (!checkCall(call.id())) //TODO - this should be replaces with a waitcondition
//    {
//        sleep_for_millis(100);
//    }

    //update with the result from the map
    call = getCall(call->id());
    call->syncCallLock()->unlock();
    PyFunctionCallResult result = call->result();
    untrackCall(call);
    return result;
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
    PyFunctionCall * call = new PyFunctionCall(functionName, params, false);

    //this will signal to start execution of the specified call on the python thread
    emit startCallRequestedSignal(call);
    return call->id().toString();
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
void PyRunner::onStartCallRequest(PyFunctionCall * call)
{
    PRINT_THREAD_INFO
    try {
        call->result().startTime = QDateTime::currentDateTime();
        trackCall(call);
        processCall(call);
        if(call->isSync())
            call->syncCallLock()->unlock();
    } catch (...)
    {
        // log errors
    }

}


/*!
 * \internal
 * \brief PyRunner::handleCompletedCall will track the end time and then signal if it is an async call that result is avalaible
 * \param call the call that has just completed
 * \note this is only called within processCall() so maybe it could be refactored in to that method
 */
void PyRunner::handleCompletedCall(PyFunctionCall * call)
{
    PRINT_THREAD_INFO
    call->result().endTime = QDateTime::currentDateTime();
    trackCall(call);
    if (!call->isSync()) {
        emit callCompletedSignal(call->id().toString());
    }
}

void PyRunner::loadStuff()
{
    loadCurrentModule();
}

/*!
 * \brief PyRunner::sendMessage is a message sent by the python side, the responsible class to catch the message is PyCallback
 * \param msg a string message, can be a json or whatever, this is just a channell actual syntax must be discussed between the involved parts
 * \note in order to work the Pytohn side must know the pyrunnerid, see the class PyCallback for more info
 */

void PyRunner::sendMessage(QString msg)
{
    //qDebug()<<"received message"<<msg;
    emit messageReceived(msg);
}
