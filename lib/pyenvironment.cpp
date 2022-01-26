#include "pyenvironment.h"

PyEnvironment::~PyEnvironment()
{

}

PyRunner * PyEnvironment::getInstanceModule(QString modulePath,QStringList dependecies)
{
    PyRunner* moduleRunner = new PyRunner(modulePath, dependecies);
    return moduleRunner;
}

PyEnvironment::PyEnvironment()
{
    m_initialized = false;
}

PyEnvironment &PyEnvironment::getInstance()
{
    static PyEnvironment instance;
    return instance;
}

bool PyEnvironment::start()
{
    if(!m_initialized)
    {
        Py_Initialize();
        PyEval_InitThreads();
        PyEval_ReleaseLock();
        m_initialized = true;
    }
    return m_initialized;
}

bool PyEnvironment::stop()
{

    try {

//        PyGILState_STATE gstate = PyGILState_Ensure();
        Py_Finalize();
        m_initialized = false;
//        PyGILState_Release(gstate);
    } catch (...) {

    }

    return true;
}
