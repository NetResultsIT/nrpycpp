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

#include "sleep_header.h"

#define NRPYQT_DEBUG_REMOVETOENABLE

#ifdef NRPYQT_DEBUG
#define PRINT_THREAD_INFO qDebug() << Q_FUNC_INFO << QThread::currentThread();
#else
#define PRINT_THREAD_INFO
#endif

PyRunner::~PyRunner()
{
    /*
     * Thread will self destroy when the finished() signal arrives
    */

    PRINT_THREAD_INFO

    emit tearDownSignal();
    m_pPythonThread->exit();
    //block and wait for teardown to be completed
    Py_DecRef(m_module_dict);
}


PyRunner::PyRunner(QString scriptPath, QStringList dependecies)
{
    PRINT_THREAD_INFO
    m_syntaxError = false;
    m_sourceFilePy = scriptPath;
    m_dependencies = dependecies;
    m_module_dict = NULL;
    m_interpreterState = NULL;
    qRegisterMetaType<PyFunctionCall>("PyFunctionCall");

    m_pPythonThread = new QThread();
    m_pPythonThread->setObjectName("NrPyCpp-" + QFileInfo(scriptPath).completeBaseName() + QUuid::createUuid().toString());
    m_pPythonThread->start();

//    connect(m_pPythonThread, &QThread::finished, this, &PyRunner::tearDown, Qt::BlockingQueuedConnection);

    connect(this, &PyRunner::startCallRequestedSignal, this, &PyRunner::onStartCallRequest);
    connect(this, &PyRunner::tearDownSignal, this, &PyRunner::tearDown, Qt::BlockingQueuedConnection);

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
    unloadInterpreter();
}


void PyRunner::processCall(PyFunctionCall call)
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
        loadInterpreter();
        loadCurrentModule();
        PyObject * py_lib_mod_dict = getModuleDict(); //borrowed reference of global variable
        if ( !py_lib_mod_dict ) {
            //FIXME - log error and return (2022-01-26 FL)
            qCritical() << "CRITICAL - cannot get Python module dictionary...";
        }
        Py_IncRef(py_lib_mod_dict);

        if (!py_lib_mod_dict)
        {
            call.result().error = true;
            call.result().errorMessage.append("PyQAC ERROR: Cannot find module at \"" + m_sourceFilePy+"\"!");
            //FIXME - should not we return an error? (2022-01-26 FL)
            qCritical() << "CRITICAL - cannot use Python module dictionary...";
        }

        char * p = new char[call.functionName().length() + 1];
        QSharedPointer<char> function = QSharedPointer<char>(p);
        strcpy(function.data(), call.functionName().toUtf8().constData());

        //get function name
        PyObject * py_function_name = NULL;

        //PyString_FromString
        if (!call.result().error)
            py_function_name = PyUnicode_FromString(function.data()); //new reference

        if (!py_function_name)
            call.result().error = true;

        //get function object
        PyObject * py_func = NULL;
        if (!call.result().error) {
            py_func = PyDict_GetItem(py_lib_mod_dict, py_function_name); //borrowed reference
            Py_IncRef(py_func);
        }

        if (!py_func) {
            call.result().error = true;
            call.result().errorMessage.append("ERROR: cannot find python function named \"" + call.functionName() +"\"!");
            qDebug() << call.result().errorMessage;
            //FIXME - shouldn't we return here? (2022-01-26 FL)
        }


        PyObject * py_args = getTupleParams(call.params());//borrowed reference

        PyObject * py_ret = NULL;

        if (!call.result().error) {
            //this is the actual python execution
            py_ret = PyObject_CallObject(py_func, py_args);//new reference
        }

        if (!py_ret)
            call.result().error = true;

        if (!call.result().error) {
               call.result().returnValue = parseObject(py_ret);
            Py_DecRef(py_ret);
        } else {
            call.result().errorMessage.append("NrPyCpp ERROR retrieving result from function named \"" + call.functionName() + "\"!");
        }

        Py_DecRef(py_lib_mod_dict);
        Py_DecRef(py_function_name);
        Py_DecRef(py_func);
        Py_DecRef(py_args);

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
void PyRunner::trackCall(PyFunctionCall call)
{
    PRINT_THREAD_INFO
    m_callsMutex.lock();
    m_calls.insert(call.id(), call);
    m_callsMutex.unlock();
}


/*!
 * \brief PyRunner::printCalls utility function to print current calls
 */
void PyRunner::printCalls()
{
    PRINT_THREAD_INFO
    m_callsMutex.lock();
    foreach(PyFunctionCall call, m_calls)
    {
        qDebug() << call.getInfo();
    }
    m_callsMutex.unlock();
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
    m_callsMutex.lock();
    PyFunctionCall returnValue = m_calls.value(callID);
    m_callsMutex.unlock();
    return returnValue;
}


/*!
 * \internal
 * \brief PyRunner::untrackCall remove the specified call from the tracking map
 * \param call
 * this is called when the function ha completed its processing
 */
void PyRunner::untrackCall(PyFunctionCall call)
{
    m_callsMutex.lock();
    m_calls.remove(call.id());
    m_callsMutex.unlock();
}


QString PyRunner::getCallInfo(QString id)
{
    PyFunctionCall c = getCall(id);
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
    foreach (PyFunctionCall c, m_calls) {
        ls << c.id().toString();
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
    PyFunctionCall c = getCall(callID);

    if (!c.isValid()) {
//        qDebug() << "Call has not yet been started";
        return false;
    }

    if (!c.result().endTime.isValid()) {
//        qDebug() << "Call has not yet completed";
        return false;
    }

    return true;
}


PyObject * PyRunner::getModuleDict()
{
    if(!m_module_dict)
    {
        loadCurrentModule();

        PyObject * scriptName = PyUnicode_FromString(m_scriptFileName.toUtf8().data()); //new reference

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

        Py_IncRef(m_module_dict);

        if(!m_module_dict)
        {
            //qDebug()<<"problem";
            PyErr_Print();
            m_module_dict = NULL;
            return NULL;
        }
//        printPyDict(m_module_dict);
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
PyObject *PyRunner::getTupleParams(QVariantList params)//borrowed reference (WHICH REF? FL)
{
    PRINT_THREAD_INFO
    if(params.size()==0) {
        return NULL;
    }

    PyErr_Print();//if you remove this, python >3 stops working

    PyObject *tup = PyTuple_New(params.size());
    for (int i = 0; i < params.size(); i++) {
        if (params[i].type() == QVariant::String) {
            PyTuple_SET_ITEM(tup, i, PyUnicode_FromString(params[i].toString().toStdString().c_str()));
        } else if (params[i].type() == QVariant::Int || params[i].type() == QVariant::LongLong) {
           PyTuple_SET_ITEM(tup, i, PyLong_FromLong(params[i].toLongLong()));
        } else if (params[i].type() == QVariant::UInt || params[i].type() == QVariant::ULongLong) {
            PyTuple_SET_ITEM(tup, i, PyLong_FromLong(params[i].toULongLong()));
        } else if (params[i].type() == QVariant::Double) {
            PyTuple_SET_ITEM(tup, i, PyFloat_FromDouble(params[i].toDouble()));
        } else if (params[i].type() == QVariant::Bool) {
            PyTuple_SET_ITEM(tup, i, PyBool_FromLong(params[i].toInt())); //FIXME - this conversion is flaky
        } else if(params[i].type()==QVariant::ByteArray) {
            //PyObject * bytesParam = Py_BuildValue("y#", params[i].toByteArray().constData(),params[i].toByteArray().size() -1 );
            PyObject * bytesParam = PyBytes_FromStringAndSize(params[i].toByteArray().constData(), params[i].toByteArray().size());
            PyTuple_SET_ITEM(tup, i, bytesParam);
        }
        // Note that PyTuple_SET_ITEM steals the reference we get from PyLong_FromLong.
    }
    return tup;
}


void PyRunner::printPyDict(PyObject *dict)
{
    PyObject * keys = PyDict_Keys(dict);
    int size = static_cast<int>(PyList_Size(keys));
    qDebug()<< "Dict size "<<size;
    for (int i=0; i<size; i++)
    {
        PyObject * key = PyList_GetItem(keys, i);
        PyObject * value = PyDict_GetItem(dict, key); //Borrowed reference
        qDebug()<<"value"<<i<<parseObject(value);
    }
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
        const char* s = PyBytes_AsString(str);
#if(0)
        Py_DecRef(str);
        PyErr_Print();
#endif
        QString valueString = QString(s);
        valueString = valueString.mid(1,valueString.length()-2);
        returnValue.setValue(valueString);
        Py_DecRef(str);
        PyErr_Print();
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
             PyObject*iObject = PyList_GetItem(object, i);
             QVariant iVariant = parseObject(iObject);
             list.append(iVariant);
        }
        returnValue.setValue(list);
    } else {
        qWarning() << "Warning: attempt to parse unknown type" << p;
    }

    PyErr_Print();
    return returnValue;
}


void PyRunner::loadCurrentModule()
{
    PyObject* sys = PyImport_ImportModule( "sys" ); //new reference
    PyObject* sys_path = PyObject_GetAttrString( sys, "path" ); //new reference
    PyObject* folder_path = PyUnicode_FromString( m_scriptFilePath.toUtf8().data() ); //new reference

    //add script path to python search path
    PyList_Append( sys_path, folder_path );

    //add dependecies path to python search path
    foreach (QString dependecy, m_dependencies)
    {
        PyObject* dependency_path = PyUnicode_FromString(dependecy.toUtf8().data());//new reference
        PyList_Append( sys_path, dependency_path);
        Py_DecRef(dependency_path);
    }

    //printPyList(sys_path);

    Py_DecRef(sys);
    Py_DecRef(sys_path);
    Py_DecRef(folder_path);
}


void PyRunner::loadInterpreter()
{
    if(m_interpreterState != NULL)
        return;

    m_interpreterState = Py_NewInterpreter();

    if(!m_interpreterState) {
        qDebug()<<"CANNOT CREATE SUB PYTHON INTEPRETER";
        PyErr_Print();
        return;
    }
}


void PyRunner::unloadInterpreter()
{
    if(m_interpreterState == NULL)
        return;

    //DEINIT STARTS
    PyThreadState_Swap(m_interpreterState);
    Py_EndInterpreter(m_interpreterState);
    m_interpreterState = NULL;
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

    //The following signal will start the execution of the call in the python thread
    //while we wait for its completion here on the app thread
    emit startCallRequestedSignal(call);

    while (!checkCall(call.id())) //TODO - this should be replaces with a waitcondition
    {
        sleep_for_millis(100);
    }

    //update with the result from the map
    call = getCall(call.id());

    untrackCall(call);
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
        // log errors
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
    trackCall(call);
    if (!call.isSync()) {
        emit callCompletedSignal(call.id().toString());
    }
}
