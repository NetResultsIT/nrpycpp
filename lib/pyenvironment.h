#ifndef PYENVIRONMENT_H
#define PYENVIRONMENT_H

#include "pyrunner.h"
#include <QMutex>
#include <QMap>

/*struct PyEnvModule
{
    QString moduleName;
    QString modulePath;
    PyRunner * module;
};

Q_DECLARE_METATYPE(PyEnvModule)*/

class PyEnvironment
{
    bool m_initialized;
    bool m_skipFinalize;
    QMutex m_muxCounter;
    int m_connectedRunners = 0;
    PyThreadState *m_pPyThreadState = nullptr;

private:
    PyEnvironment();
    PyEnvironment(PyEnvironment const& copy);
    PyEnvironment & operator = (PyEnvironment const&copy);
    //QMap<QString, PyRunner*> m_modules;

public:
    static PyEnvironment &getInstance();
    bool start();
    bool stop();
    ~PyEnvironment();

    //PyRunner * getInstanceModule(QString modulePath, QStringList dependecies = QStringList());
    //void unloadModule(PyRunner* runner);
    bool getSkipFinalize() const;
    void setSkipFinalize(bool skipFinalize);
};

#endif // PYENVIRONMENT_H
