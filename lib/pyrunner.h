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
#include "PyCallBackIface.h"

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
    QString m_runnerId;
    PyCallBackIface * m_pCallbackHandler = nullptr; //guarded by the routing mutex (see pyrunner.cpp)
    //pending partial line of stdout / stderr for each python thread (a thread that is in the middle of
    //a print() must not get its line cut by another one), guarded by the routing mutex
    QHash<quintptr, QString> m_outputBuffers[2];
    void emitOutputLines(int stream, quintptr threadKey, bool includePartialLine); //with the routing mutex held
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

    //not wrapped automatically, PyRunnerQt::setCallbackHandler() is hand written (see the wrapper conf)
    void setCallbackHandler(PyCallBackIface * pHandler);

    /* Routing of what the scripts send back (nrpycpp python module, see nrpycppmodule.cpp).
     * The runner is identified by its id, they return false if it does not exist (anymore). */
    enum OutputStream { StdOut = 0, StdErr = 1 };
    static QString currentRunnerId(); //the runner whose call is being processed on this thread, if any
    static bool routeMessage(const QString &runnerId, const QString &text);
    static bool routeOutput(const QString &runnerId, int stream, quintptr threadKey, const QString &text);
    static bool flushOutput(const QString &runnerId, int stream, quintptr threadKey); //threadKey: the writing python thread
    static bool routeException(const QString &runnerId, const QString &traceback);

    //START_WRAPPER_METHODS

    //identification
    QString runnerId();

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
    void messageReceived(QString message);
    void outputReceived(QString text);
    void errorOutputReceived(QString text);
    void unhandledException(QString traceback);
    //END_SIGNAL_METHODS

private slots:
    void onStartCallRequest(PyFunctionCall call);
};

#endif // PYRUNNER_H
