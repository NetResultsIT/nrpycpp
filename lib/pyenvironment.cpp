#include "pyenvironment.h"

#include <QMutexLocker>

PyEnvironment::~PyEnvironment()
{
    qDebug() << "PyEnvironment dtor";
}


bool PyEnvironment::getSkipFinalize() const
{
    return m_skipFinalize;
}

void PyEnvironment::setSkipFinalize(bool skipFinalize)
{
    m_skipFinalize = skipFinalize;
}

PyEnvironment::PyEnvironment()
{
    m_skipFinalize = false;
    m_initialized = false;
}


PyEnvironment &PyEnvironment::getInstance()
{
    static PyEnvironment instance;
    return instance;
}


bool PyEnvironment::start()
{
    QMutexLocker locker(&m_muxCounter);
    if(!m_initialized)
    {
        Py_Initialize();
        //PyEval_InitThreads(); //deprecated in python3.9, embeeded in Py_Initialize
        //PyEval_ReleaseLock(); //Release lock was deprecated, better to use the save state and restore in the stop()
        m_pPyThreadState = PyEval_SaveThread();
        m_initialized = true;
    }
    m_connectedRunners++;
    return m_initialized;
}


bool PyEnvironment::stop()
{
    QMutexLocker locker(&m_muxCounter);
    m_connectedRunners--;
    if (m_connectedRunners == 0) {
        try {
            //We need to restore thread state otherwise we have a crash
            PyEval_RestoreThread(m_pPyThreadState);
            if(!m_skipFinalize)
                Py_Finalize();
            m_initialized = false;
        } catch (...) {
            qDebug() << "Got exception in " << Q_FUNC_INFO;
        }
    }

    return true;
}

