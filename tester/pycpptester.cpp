#include "pycpptester.h"

#include <QDebug>
#include <QDir>
#include <QThread>
#include <QDateTime>

#include "PyRunnerQt.h"
#include "../lib/sleep_header.h"

PyCppTester::PyCppTester(QString i_pythonscriptfullpath, QObject *parent)
    : QObject{parent}
    , m_pyscriptPath((i_pythonscriptfullpath))
{

    QStringList dependencies;
//    dependencies.append("/home/aru/Projects/PyQAC/Dependency/");
//    dependencies.append("/home/aru/Projects/PyQAC/Dependency2/");
    //dependencies.apPypend("/home/aru/Projects/PyQAC/PyDependencyImporter/");

    qDebug() << "Executing Python script:" << m_pyscriptPath;
    m_pPyRunner = new PyRunnerQt(m_pyscriptPath, dependencies);
    connect(m_pPyRunner, &PyRunnerQt::callCompletedSignal, this, &PyCppTester::onCallFinished);
    connect(this, &PyCppTester::closeAppRequested, this, &PyCppTester::onCloseAppRequest);
}


PyCppTester::~PyCppTester()
{
    if (m_pPyRunner) {
        delete m_pPyRunner;
        m_pPyRunner = nullptr;
    }
}

#include <QTimer>
void PyCppTester::go()
{
    QTimer::singleShot(500, this, [this] {executeTestList();});
}

void PyCppTester::executeTestList()
{
    qDebug() << "Application thread " << QThread::currentThread();

    QVariantList paramlist;
    QString funcname;


    funcname = "upper";
    paramlist << "ciccio";
    execute(funcname, paramlist);

    funcname = "swapp";
    paramlist.clear();
    paramlist << "foo" << "bar";
    execute(funcname, paramlist);

    funcname = "shiftR3";
    paramlist.clear();
    paramlist << "foo" << "bar" << "zoidberg";
    execute(funcname, paramlist);

    funcname = "shiftR3";
    paramlist.clear();
    paramlist << 1 << false << 3.14;
    execute(funcname, paramlist);


    funcname = "lengthy_func";
    paramlist.clear();
    paramlist << 5;
    execute(funcname, paramlist, false);

    qDebug() << "Printing calls...";
    m_pPyRunner->printCalls();

    funcname = "sum";
    paramlist.clear();
    paramlist << 1.4 << 41;
    //execute(funcname, paramlist);

    funcname = "negate";
    paramlist.clear();
    paramlist << true;
    //execute(funcname, paramlist);

    funcname = "lengthy_func";
    paramlist.clear();
    paramlist << 2;
    //execute(funcname, paramlist);

    funcname = "countbytes";
    paramlist.clear();
    QByteArray ba(1000 ,'a');
    paramlist << ba;
    //execute(funcname, paramlist);

    funcname = "replbytes";
    paramlist.clear();
    QByteArray ba2(1000 ,'a');
    paramlist << ba2 << "a" << "x";
    //execute(funcname, paramlist);

    qDebug() << "sleeping for 2 secs";
    //sleep_for_millis(2000);

    qDebug() << "testlist finished at " << QDateTime::currentDateTime();
    ontestlistfinished();
}


void PyCppTester::execute(QString func, QVariantList params, bool sync)
{
    qDebug() << QDateTime::currentDateTime() << " - Calling" << (sync?"(SYNC)":"(ASYNC)") << func << " with params: " << params;
    if (sync) {
        PyFunctionCallResult pyres = m_pPyRunner->syncCallFunction(func, params);
        QVariant res;
        if (pyres.error) {
            res = pyres.errorMessage;
        } else {
            res = pyres.returnValue;
        }
        qDebug() << func << " result:" << res << "started at " << pyres.startTime << "finished at " << pyres.endTime;
    } else {
        qDebug() << func << " callID:" << m_pPyRunner->asyncCallFunction(func, params);
    }
}



void PyCppTester::onCallFinished(QString c)
{
    qDebug() << Q_FUNC_INFO << c << QDateTime::currentDateTime();
    //gather async results
    foreach(QString s, m_pPyRunner->getAsyncCallsList()) {
        PyFunctionCallResult r = m_pPyRunner->getAsyncCallResult(s);
        qDebug() << "result / start / end: " << r.returnValue << r.startTime << r.endTime;
    }

    quitIfThereAreNoMoreCallPending();
}


void PyCppTester::ontestlistfinished()
{
    qDebug() << "all testlist tests have been called";
    quitIfThereAreNoMoreCallPending();
}


void PyCppTester::quitIfThereAreNoMoreCallPending()
{
    int pendingCalls = m_pPyRunner->getAsyncCallsList().size();
    if ( pendingCalls == 0) {
        qDebug() << "all async calls are completed, calling quit.";
        emit closeAppRequested();
    } else {
        qDebug() << "Waiting for " << pendingCalls << " calls to terminate";
    }
}

#include <QCoreApplication>
void PyCppTester::onCloseAppRequest()
{
    //qDebug () << "deleting runner...";
    //delete m_pPyRunner;
    //m_pPyRunner = nullptr;
    qDebug() << "calling app quit...";
    qApp->quit();
}

