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
#include <Python.h>
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
    void trackCall(PyFunctionCall call);
    PyFunctionCall getCall(QUuid callID);
    void untrackCall(PyFunctionCall call);
    bool checkCall(QUuid callID);

    PyThreadState * m_interpreterState;
    void loadInterpreter();
    void unloadInterpreter();

    PyObject * getModuleDict();
    PyObject * m_module_dict;
    void loadCurrentModule();

    void setup();

    QVariant parseObject(PyObject * object);
    PyObject * getTupleParams(QVariantList paramList);

    /*debug utils*/
    void printPyTuple(PyObject* tuple);
    void printPyList(PyObject *list);
    void printPyDict(PyObject *list);

    //errors
    bool m_syntaxError;

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
    void tearDownSignal(); //FIXME - WTF? this is never emitted (2022-02-01 FL)
    void startCallRequestedSignal(PyFunctionCall call);
    //START_SIGNAL_METHODS
    void callCompletedSignal(QString);
    //END_SIGNAL_METHODS

private slots:
    void tearDown();
    void onStartCallRequest(PyFunctionCall call);
};

#endif // PYRUNNER_H
