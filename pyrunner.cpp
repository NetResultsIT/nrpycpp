#include "pyrunner.h"

PyRunner::~PyRunner()
{
    /*
     * Thread will self destroy when the finished() signal arrives
    */

    //unloadCurrentModule();
    //this->moveToThread(m_py_thread);
    //emit(tearDownSignal());
    tearDown();
    Py_DecRef(m_module_dict);
}

PyRunner::PyRunner(QString scriptPath, QStringList dependecies)
{
    m_errorCode = PyRunnerError_OK;
    m_syntaxError = false;
    m_sourceFilePy = scriptPath;
    m_dependecies = dependecies;
    m_module_dict = NULL;
    qRegisterMetaType<PyQACCall>("PyQACCall");

    m_py_thread = new QThread();
    m_py_thread->setObjectName("PyQAC-"+QFileInfo(scriptPath).completeBaseName()+QUuid::createUuid().toString());
    m_py_thread->start();

    bool b;
    b = connect(m_py_thread, SIGNAL(finished()),
               m_py_thread, SLOT(deleteLater()));
    assert(b);
    b = connect(this, SIGNAL(startCallSignal(PyQACCall)),
             this, SLOT(startCallSlot(PyQACCall)));
    assert(b);
    b = connect(this, SIGNAL(setupSignal()),
                this, SLOT(setup()));
    assert(b);
    b = connect(this, SIGNAL(tearDownSignal()),
                this, SLOT(tearDown()));
    assert(b);

    this->moveToThread(m_py_thread);
    emit(setupSignal());
}

void PyRunner::setup()
{
    try {
        QFileInfo scriptFileInfo(m_sourceFilePy);
        if(!scriptFileInfo.exists())
        {
            return;
        }

        m_scriptFileName = scriptFileInfo.completeBaseName();
        m_scriptFilePath = scriptFileInfo.dir().path();

        //Py_Initialize();

    }catch(...)
    {
        PyErr_Print();
    }
}

void PyRunner::tearDown()
{
    //PyImport_Cleanup();
    //Py_Finalize();
    unloadCurrentModule();
    m_py_thread->exit();
}

void PyRunner::start()
{
    asyncCallFunction("start", QStringList());
}

void PyRunner::stop()
{
    syncCallFunction("stop", QStringList());
}

QString PyRunner::getResult(QString resultName)
{
    QString functionName = "getResult";

    QStringList params;
    params.append(resultName);
    QString result = syncCallFunction(functionName, params);
//    qDebug()<<result;
    return  result;
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

void PyRunner::processCall(PyQACCall call)
{
    if(m_syntaxError)
    {
        emit(callDidFinishedSlot(call));
        return;
    }

    try {
        qDebug()<<"resolving call"<<call.functionName;
        PyGILState_STATE gstate = openCallContext();

        PyObject * py_lib_mod_dict = getModuleDict();//borrowed reference of global variable

        Py_IncRef(py_lib_mod_dict);

//        printPyDict(py_lib_mod_dict);

        if(!py_lib_mod_dict)
        {
            call.error=true;
            call.errorMessage.append("PyQAC ERROR: Cannot find module at \""+m_sourceFilePy+"\"!");
        }

//        char * function =  call.functionName.toUtf8().data();

        char * function = new char(call.functionName.length() + 1);
        strcpy(function, call.functionName.toLatin1().constData());

        qDebug()<<"function:"<<function;

        //get function name
        PyObject * py_function_name = NULL;

        //PyString_FromString
        if(!call.error)
            py_function_name = PyUnicode_FromString(function);// this->PyStringFromString(call.functionName);// PyString_FromString(function);//new reference

        if(!py_function_name)
            call.error=true;

        //get function object
        PyObject * py_func = NULL;
        if(!call.error)
            py_func = PyDict_GetItem(py_lib_mod_dict, py_function_name);//borrowed reference

        //Py_IncRef(py_func);

        if(!py_func){
            call.error=true;
            call.errorMessage.append("PyQAC ERROR : cannot find function named \""+call.functionName+"\"!");
            qDebug()<<call.errorMessage;
        }


        PyObject * py_args = getTupleParams(call.params);//borrowed reference

        PyObject * py_ret = NULL;

        if(!call.error)
            py_ret = PyObject_CallObject(py_func, py_args);//new reference

        if(!py_ret)
            call.error=true;

        if(!call.error)
        {
            call.returnValue = parseObject(py_ret);
            Py_DecRef(py_ret);
        }else {
            call.errorMessage.append("PyQAC ERROR : retrieving result from function named \""+call.functionName+"\"!");

        }



        Py_DecRef(py_lib_mod_dict);
        Py_DecRef(py_function_name);
        //Py_DecRef(py_func);

        if(params.size())
            Py_DecRef(py_args);

        closeCallContext(gstate);
        emit(callDidFinishedSlot(call));
    } catch (...)
    {
        //qDebug() << e.what();
        PyErr_Print();
    }
}

QStringList PyRunner::getParams() const
{
    return m_params;
}

void PyRunner::setParams(const QStringList &params)
{
    m_params = params;

    QString stringParams;

    stringParams = "['"+params[0]+"'";

    for(int i=1; i<params.length(); i++)
    {
        stringParams += ",'"+params[i]+"'";
    }

    stringParams += "]";

    //qDebug()<<stringParams;

    syncCallFunction("setParams", params);
}

void PyRunner::setParam(QString paramName, QString paramValue)
{
    QString functionName = "setParam";

    QStringList params;
    params.append(paramName);
    params.append(paramValue);
    syncCallFunction(functionName, params);
}

void PyRunner::trackCall(PyQACCall call)
{
    m_callsMutex.lock();
    m_calls.insert(call.CallID, call);
    //printCalls();
    m_callsMutex.unlock();
}

void PyRunner::printCalls()
{
    foreach(PyQACCall call, m_calls.values())
    {
        qDebug()<< "Call Id: "<<call.CallID<<"; name: "<<call.functionName;
    }
}

PyQACCall PyRunner::getCall(QUuid callID)
{
    m_callsMutex.lock();
    PyQACCall returnValue = m_calls.value(callID);
    m_callsMutex.unlock();
    return returnValue;
}

void PyRunner::untrackCall(PyQACCall call)
{
    m_callsMutex.lock();
    m_calls.remove(call.CallID);
    //printCalls();
    m_callsMutex.unlock();
}

bool PyRunner::checkCall(QUuid callID)
{
    m_callsMutex.lock();
    bool returnValue = m_calls.contains(callID);
    m_callsMutex.unlock();
    return returnValue;
}

PyObject * PyRunner::getModuleDict()
{
    if(!m_module_dict)
    {
        loadCurrentModule();

        PyObject * scriptName = PyUnicode_FromString(m_scriptFileName.toUtf8().data());//new reference

        //script meta data
        PyObject * m_py_lib_mod = PyImport_Import(scriptName);//new reference

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

PyObject *PyRunner::getTupleParams(QStringList params)//borrowed reference
{
    if(params.size()==0)
        return NULL;

    QString formatString ="(";

    for(int i=0; i<params.size(); i++)
    {
        formatString +="s";
    }

    formatString +=")";
//    qDebug()<<"formatString"<<formatString;
//    const char * formatChars = formatString.toUtf8().data();

    char * formatChars = new char(formatString.length() + 1);
    strcpy(formatChars, formatString.toLatin1().constData());

    PyErr_Print();//if you remove this, python >3 stops working

    PyObject * args;

    if(params.count()==1)
    {
        qDebug()<<"1 arg";
        args = Py_BuildValue(formatChars, qPrintable(params[0]));//new reference
    }else if(params.count()==2)
    {
        qDebug()<<"2 args";
        args = Py_BuildValue(formatChars, qPrintable(params[0]), qPrintable(params[1]));//new reference
    }else
    {
        args = Py_BuildValue(formatChars);
        qDebug()<<"Problem parsing PYQAC args";
    }


    qDebug()<<"here";

//    PyObject * args = Py_BuildValue(formatChars, argList);//new reference

//    for(int i=0; i<params.size(); i++)
//    {
//        QString param = params[i];
//        PyObject * value = PyUnicode_FromFormat(param.toUtf8().data());//new reference
//        PyErr_Print();
//        PyTuple_SetItem(args, i, value);//steals the reference
//    }

    printPyTuple(args);

    return args;
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
//        PyObject* objectsRepresentation = PyObject_Repr(tupleValue);//New reference
        qDebug()<<"value"<<i<<parseObject(tupleValue);
//        Py_DecRef(objectsRepresentation);
    }
}

void PyRunner::printPyTuple(PyObject *tuple)
{
    int tupleSize = static_cast<int>(PyTuple_Size(tuple));
    qDebug()<< "tuple size "<<tupleSize;
    for (int i=0; i<tupleSize; i++)
    {
        PyObject * tupleValue = PyTuple_GetItem(tuple,i); //Borrowed reference
        PyObject* objectsRepresentation = PyObject_Repr(tupleValue);//New reference
        const char* s = PyByteArray_AsString(objectsRepresentation);
        Py_DecRef(objectsRepresentation);
    }
}

QString PyRunner::parseObject(PyObject *object)
{
    PyTypeObject* type = object->ob_type;
    const char* p = type->tp_name;

    QString returnValue = "";

    if(QString("str").compare(p)==0)
    {

        PyObject* objectsRepresentation = PyObject_Repr(object);//new reference

#if PY_MAJOR_VERSION == 2
        const char* s = PyString_AsString(objectsRepresentation);
#elif PY_MAJOR_VERSION == 3
        PyObject* str = PyUnicode_AsEncodedString(objectsRepresentation, "utf-8", "~E~");//new reference
        const char* s = PyBytes_AsString(str);
        Py_DecRef(str);
#endif
        PyErr_Print();
        returnValue = QString(s);
        returnValue = returnValue.mid(1,returnValue.length()-2);
        Py_DecRef(objectsRepresentation);
    }else if(QString("int").compare(p)==0)
    {
        PyObject * strObject = PyObject_Str(object);//new reference
        returnValue = parseObject(strObject);
        Py_DecRef(strObject);
    }else if(QString("float").compare(p)==0)
    {
        PyObject * strObject = PyObject_Str(object);//new reference
        returnValue = parseObject(strObject);
        Py_DecRef(strObject);
    }else if(QString("double").compare(p)==0)
    {
        PyObject * strObject = PyObject_Str(object);//new reference
        returnValue = parseObject(strObject);
        Py_DecRef(strObject);
    }else if(QString("NoneType").compare(p)==0)
    {
        //do nothing
    }else
    {
//        qDebug()<<"PyQAC Warning: attemp to parse unknown type"<<p;
        PyObject * strObject = PyObject_Str(object);//new reference
        returnValue = parseObject(strObject);
        Py_DecRef(strObject);
    }

    return returnValue;
}

void PyRunner::loadCurrentModule()
{
    //PyRun_SimpleString("import sys");
    //PyRun_SimpleString("print sys.modules.keys()");

    PyObject* sys = PyImport_ImportModule( "sys" );//new reference
    PyObject* sys_path = PyObject_GetAttrString( sys, "path" );//new reference
    PyObject* folder_path = PyUnicode_FromString( m_scriptFilePath.toUtf8().data() );//new reference

    //add script path to python search path
    PyList_Append( sys_path, folder_path );

    //add dependecies path to python search path
    foreach (QString dependecy, m_dependecies)
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

void PyRunner::unloadCurrentModule()
{
    PyGILState_STATE gstate = PyGILState_Ensure();
    //PyEval_InitThreads();

    //unload imported paths
    QString unloadPathCommand = "sys.path.remove(\""+m_scriptFilePath+"\")";
    QString unloadModuleCommand = "del sys.modules[\""+m_scriptFileName+"\"]";
    PyRun_SimpleString("import sys");
    PyRun_SimpleString(unloadPathCommand.toUtf8().data());
    PyRun_SimpleString(unloadModuleCommand.toUtf8().data());

    foreach (QString dependency, m_dependecies)
    {
        unloadPathCommand = "sys.path.remove(\""+dependency+"\")";
        PyRun_SimpleString(unloadPathCommand.toUtf8().data());
    }

    PyGILState_Release(gstate);
//    PyEval_ReleaseLock();
}

void PyRunner::getReturnValues()
{

}

QString PyRunner::syncCallFunction(QString functionName, QStringList params = QStringList())
{
    PyQACCall call;
    call.CallID = QUuid::createUuid();
    call.synch = true;
    call.functionName = functionName;
    call.params = params;
    call.error = false;

    this->moveToThread(m_py_thread);

    emit(startCallSignal(call));

    while (!checkCall(call.CallID))
    {
#ifdef WIN32
         _sleep(1000);
#elif
        usleep(1);
#endif
    }

    call = getCall(call.CallID);

    untrackCall(call);

    if(call.error)
    {
        m_syntaxError = true;
        m_errorCode = PyRunnerError_SYNTAX_ERROR;
        m_errorString = "Syntax error";
        m_errorMessage = call.errorMessage;
        return call.errorMessage;
    }

    return call.returnValue;
}

int PyRunner::getErrorCode()
{
    return m_errorCode;
}

QString PyRunner::getErrorString()
{
    return m_errorString;
}

QString PyRunner::getErrorMessage()
{
    return m_errorMessage;
}


void PyRunner::checkError()
{
    if(!m_syntaxError)
        m_errorCode = syncCallFunction("getErrorCode", QStringList()).toInt();

    if(!m_syntaxError)
        m_errorMessage = syncCallFunction("getErrorMsg", QStringList());

    if(m_errorCode!=0 && !m_syntaxError)
        m_errorString = m_scriptFileName+" reported error";
}

void PyRunner::asyncCallFunction(QString functionName, QStringList params)
{
    PyQACCall call;
    call.CallID = QUuid::createUuid();
    call.synch = false;
    call.functionName = functionName;
    call.params = params;
    call.error = false;

    this->moveToThread(m_py_thread);
    emit(startCallSignal(call));
}

void PyRunner::startCallSlot(PyQACCall call)
{
    try {
        processCall(call);
    } catch (...)
    {

    }

}

void PyRunner::callDidFinishedSlot(PyQACCall call)
{
    if(call.synch)
    {
        trackCall(call);
    }
}
