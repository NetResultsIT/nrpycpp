#ifndef PYENVIRONMENT_H
#define PYENVIRONMENT_H

#include "pyrunner.h"
#include <QMutex>
#include <thread>

class PyEnvironment
{
    bool m_initialized = false;
    bool m_initFailed = false;
    bool m_ownsInterpreter = false; //false if the host application started python itself
    std::thread::id m_initThread;
    QMutex m_muxCounter;
    int m_connectedRunners = 0;
    PyThreadState *m_pPyThreadState = nullptr;
    QString m_pythonHome;
    QString m_virtualEnv;

private:
    PyEnvironment();
    PyEnvironment(PyEnvironment const& copy);
    PyEnvironment & operator = (PyEnvironment const&copy);

    QString resolvePythonHome() const;
    bool initializeInterpreter();
    void activateVirtualEnv(const QString &venvPath);

public:
    static PyEnvironment &getInstance();
    bool start();
    bool stop();
    bool isAvailable();
    void setPythonHome(const QString &path);
    void setVirtualEnv(const QString &path);
    ~PyEnvironment();
};

#endif // PYENVIRONMENT_H
