/**************************************************************************
 *  Copyright (C) 2022 by NetResults S.r.l. ( http://www.netresults.it )  *
 *  Author( s ) :                                                         *
 *         Stefano Aru                  <s.aru@netresults.it>             *
 *         Francesco Lamonica      <f.lamonica@netresults.it>             *
 **************************************************************************/

#ifndef PYRUNNER_H
#define PYRUNNER_H
#include <QObject>
#include <QString>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QUuid>
#include <QMap>
#include <QVariantList>
#include <QVariant>
#include <QDebug>
#include <QFile>
#include <QDir>
#include <QTimer>

#pragma push_macro("slots")
#undef slots
#if defined(_MSC_VER) && defined(_DEBUG) && !defined(NRPYCPP_PYTHON_DEBUG)
/* pyconfig.h turns _DEBUG into Py_DEBUG and auto-links pythonXY_d.lib, which only exists
 * in a debug build of CPython. Unless NRPYCPP_PYTHON_DEBUG is defined we build our debug
 * binaries against the release interpreter: include the CRT headers first, then hide _DEBUG */
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#undef _DEBUG
#include <Python.h>
#define _DEBUG 1
#else
#include <Python.h>
#endif
#pragma pop_macro("slots")

#include "PyCall.h"

enum  PyRunnerError //TODO move this into an nrcpptypes.h and include it in the wrapper
{
    PyRunnerError_OK = 0,//no error
    PyRunnerError_SYNTAX_ERROR = 3001,
    PyRunnerError_SEMANTIC_ERROR = 3002
};

class PyRunner : public QObject
{
    Q_OBJECT

    QStringList m_dependencies;
    QString m_sourceFilePy;
    QString m_scriptFileName;
    QString m_scriptFilePath;
    QThread * m_pPythonThread;
    void processCall(PyFunctionCall call);

    QHash<QUuid, PyFunctionCall> m_calls;
    QMutex m_callsMutex;
    QWaitCondition m_callsCondition; //signalled (with m_callsMutex) whenever a call completes
    void trackCall(PyFunctionCall call);
    PyFunctionCall getCall(QUuid callID);
    void untrackCall(PyFunctionCall call);

    /* the following methods must be called with the GIL held */
    void loadCurrentModule();
    void unloadCurrentModule();
    PyObject * getModuleDict(QString *errorMessage);
    PyObject * m_module_dict;
    QStringList m_addedSearchPaths; //the entries this runner appended to sys.path

    void setup();

    /* Function context call */
    PyGILState_STATE openCallContext();
    void closeCallContext(PyGILState_STATE state);

    /* conversions, with the GIL held: on failure they return an invalid value and fill errorMessage */
    QVariant parseObject(PyObject * object, QString *errorMessage);
    PyObject * toPyObject(const QVariant &value, QString *errorMessage);
    PyObject * getTupleParams(const QVariantList &paramList, QString *errorMessage);
    static QString takePythonError();

    /*debug utils*/
    void printPyTuple(PyObject* tuple);
    void printPyList(PyObject *list);
    void printPyDict(PyObject *list);

public:
    PyRunner(QString scriptPath, QStringList dependencies=QStringList());
    ~PyRunner();

    //START_WRAPPER_METHODS

    //utils
    void printCalls();

    //calling custom functions
    QString asyncCallFunction(QString functionName, QVariantList params = QVariantList());
    PyFunctionCallResult syncCallFunction(QString functionName, QVariantList params = QVariantList());
    PyFunctionCallResult getAsyncCallResult(QString id);
    QString getCallInfo(QString id);
    QStringList getAsyncCallsList();
    //END_WRAPPER_METHODS

private:
    void handleCompletedCall(PyFunctionCall call);

signals:
    void startCallRequestedSignal(PyFunctionCall call);
    //START_SIGNAL_METHODS
    void callCompletedSignal(QString);
    //END_SIGNAL_METHODS

private slots:
    void onStartCallRequest(PyFunctionCall call);
};

#endif // PYRUNNER_H
