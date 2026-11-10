/**************************************************************************
 *  Copyright (C) 2026 by NetResults S.r.l. ( http://www.netresults.it )  *
 *  Author( s ) :                                                         *
 *         Francesco Lamonica      <f.lamonica@netresults.it>             *
 **************************************************************************/

/*
 * nrPyCpp test suite: it goes through the public API only (PyRunnerQt.h, nrPyCppConfig.h).
 *
 * The interpreter is started once per process, so the scenarios that need a different configuration
 * (no python home, a venv for another python version) run this same executable again as a child
 * process: "tst_nrpycpp --child <scenario>".
 */

#include <QtTest>
#include <QPoint>
#include <QMutex>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>

#include "PyCallBackIface.h"
#include "PyRunnerQt.h"
#include "nrPyCppConfig.h"

static QString testsDir()
{
    return QStringLiteral(NRPYCPP_TESTS_DIR);
}

static QString codecScript()
{
    return testsDir() + "/scripts/codec.py";
}

static QString talkbackScript()
{
    return testsDir() + "/scripts/talkback.py";
}

/* a PyCallBackIface that just records what it gets (called from the python threads) */
class RecordingHandler : public PyCallBackIface
{
public:
    QMutex mutex;
    QStringList output, exceptions, messages;
    void onStdOutput(const QSharedPointer<QString> &log) override { QMutexLocker l(&mutex); output << *log; }
    void onException(const QSharedPointer<QString> &msg) override { QMutexLocker l(&mutex); exceptions << *msg; }
    void onSendMessage(const QSharedPointer<QString> &msg) override { QMutexLocker l(&mutex); messages << *msg; }
    int outputCount() { QMutexLocker l(&mutex); return output.size(); }
    int exceptionCount() { QMutexLocker l(&mutex); return exceptions.size(); }
    int messageCount() { QMutexLocker l(&mutex); return messages.size(); }
};

static QStringList spyStrings(const QSignalSpy &spy)
{
    QStringList l;
    for (const QList<QVariant> &args : spy)
        l << args.at(0).toString();
    return l;
}

static QStringList codecDependencies()
{
    return QStringList() << testsDir() + "/scripts/dep";
}

/* a minimal virtual environment: what NrPyCpp::setVirtualEnv() looks at, pyvenv.cfg and site-packages */
static bool createFakeVenv(const QString &dir, const QString &pythonVersion)
{
    QFile cfg(dir + "/pyvenv.cfg");
    if (!cfg.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    cfg.write(QString("home = /nowhere\ninclude-system-site-packages = false\nversion = %1\n").arg(pythonVersion).toUtf8());
    cfg.close();
#ifdef Q_OS_WIN
    const QString sitePackages = dir + "/Lib/site-packages";
#else
    const QString sitePackages = dir + "/lib/python" + pythonVersion.section('.', 0, 1) + "/site-packages";
#endif
    if (!QDir().mkpath(sitePackages + "/tinypkg"))
        return false;
    QFile pkg(sitePackages + "/tinypkg/__init__.py");
    if (!pkg.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    pkg.write("VALUE = \"from-venv\"\n");
    return true;
}

static int runChild(const QString &scenario, const QProcessEnvironment &env, QByteArray *output)
{
    QProcess child;
    child.setProcessEnvironment(env);
    child.setProcessChannelMode(QProcess::MergedChannels);
    child.start(QCoreApplication::applicationFilePath(), QStringList() << "--child" << scenario);
    if (!child.waitForFinished(120000)) {
        child.kill();
        child.waitForFinished();
        *output = child.readAll() + "\n(timeout)";
        return -1;
    }
    *output = child.readAll();
    return child.exitStatus() == QProcess::NormalExit ? child.exitCode() : -1;
}


class TestNrPyCpp : public QObject
{
    Q_OBJECT

    QTemporaryDir m_venvDir;
    QString m_pythonVersion;
    PyRunnerQt *m_runner = nullptr;

    PyFunctionCallResult call(const QString &function, const QVariantList &params = QVariantList())
    {
        return m_runner->syncCallFunction(function, params);
    }

private slots:
    void initTestCase()
    {
        //ask a child which python we embed, to create a venv for that version before starting ours
        QByteArray out;
        QCOMPARE(runChild("version", QProcessEnvironment::systemEnvironment(), &out), 0);
        m_pythonVersion = QString::fromUtf8(out).section("VERSION=", 1).section('\n', 0, 0).trimmed();
        QVERIFY2(m_pythonVersion.count('.') == 2, out.constData());
        qInfo() << "embedded python" << m_pythonVersion;

        QVERIFY(m_venvDir.isValid());
        QVERIFY(createFakeVenv(m_venvDir.path(), m_pythonVersion));
        NrPyCpp::setVirtualEnv(m_venvDir.path());
        QVERIFY(NrPyCpp::isPythonAvailable());
        m_runner = new PyRunnerQt(codecScript(), codecDependencies());
    }

    void cleanupTestCase()
    {
        delete m_runner;
        m_runner = nullptr;
    }

    void parameterRoundTrip()
    {
        QVariantMap map;
        map["k"] = QVariantList() << 1 << "x";
        const QVariantList params = QVariantList()
                << QString::fromUtf8("\xc3\xa9\"'\n") << qlonglong(1) << (qlonglong(1) << 40)
                << qulonglong(18446744073709551615ULL) << 0.30000000000000004 << true
                << QByteArray("\x00\xff", 2) << map << QVariant() << QStringList({"a", "b"})
                << -7 << 3u << 2.5f;
        PyFunctionCallResult r = call("echo", params);
        QVERIFY2(!r.error, qPrintable(r.errorMessage));
        const QVariantList back = r.returnValue.toList();
        QCOMPARE(back.size(), params.size());
        QCOMPARE(back.at(0).toString(), QString::fromUtf8("\xc3\xa9\"'\n"));
        QCOMPARE(back.at(1).toLongLong(), 1);
        QCOMPARE(back.at(2).toLongLong(), qlonglong(1) << 40);
        QCOMPARE(back.at(3).toULongLong(), 18446744073709551615ULL);
        QCOMPARE(back.at(4).toDouble(), 0.30000000000000004);
        QCOMPARE(back.at(5).userType(), int(QMetaType::Bool));
        QCOMPARE(back.at(5).toBool(), true);
        QCOMPARE(back.at(6).toByteArray(), QByteArray("\x00\xff", 2));
        QCOMPARE(back.at(7).toMap().value("k").toList().value(1).toString(), QString("x"));
        QVERIFY(!back.at(8).isValid());
        QCOMPARE(back.at(9).toStringList(), QStringList({"a", "b"}));
        QCOMPARE(back.at(10).toInt(), -7);
        QCOMPARE(back.at(11).toUInt(), 3u);
        QCOMPARE(back.at(12).toDouble(), 2.5);
    }

    void pythonSeesTheRightTypes()
    {
        PyFunctionCallResult r = call("types", QVariantList() << 1 << true << 2.5 << QString("s") << QByteArray("b")
                                      << QVariant() << QVariant(QVariantList()) << QVariant(QVariantMap()));
        QVERIFY2(!r.error, qPrintable(r.errorMessage));
        QCOMPARE(r.returnValue.toStringList(),
                 QStringList({"int", "bool", "float", "str", "bytes", "NoneType", "list", "dict"}));
    }

    void returnValues()
    {
        PyFunctionCallResult r = call("edge_values");
        QVERIFY2(!r.error, qPrintable(r.errorMessage));
        const QVariantList b = r.returnValue.toList();
        QCOMPARE(b.size(), 9);
        QCOMPARE(b.at(0).toLongLong(), 1099511627776LL);
        QCOMPARE(b.at(1).toLongLong(), -1099511627776LL);
        QCOMPARE(b.at(2).toULongLong(), 18446744073709551615ULL);
        QCOMPARE(b.at(3).userType(), int(QMetaType::Double));
        QCOMPARE(b.at(3).toDouble(), 0.1 + 0.2);
        QCOMPARE(b.at(4).toString(), QString::fromUtf8("quote ' and \" \n and ünïcödé"));
        QCOMPARE(b.at(5).toList(), QVariantList({1, 2}));
        QCOMPARE(b.at(6).toMap().value("a").toInt(), 1);
        QCOMPARE(b.at(6).toMap().value("b").toList().value(0).toByteArray(), QByteArray("\x00\x01", 2));
        QVERIFY(!b.at(7).isValid());
        QCOMPARE(b.at(8).toBool(), true);
        //small integers keep coming back as int, as with the previous versions
        QCOMPARE(call("echo", QVariantList() << 47).returnValue.toList().value(0).userType(), int(QMetaType::Int));
    }

    void integerTooBigIsAnError()
    {
        PyFunctionCallResult r = call("too_big");
        QVERIFY(r.error);
        QVERIFY2(r.errorMessage.contains("64 bits"), qPrintable(r.errorMessage));
    }

    void pythonExceptionHasTraceback()
    {
        PyFunctionCallResult r = call("boom", QVariantList() << 1);
        QVERIFY(r.error);
        QVERIFY2(r.errorMessage.contains("ZeroDivisionError"), qPrintable(r.errorMessage));
        QVERIFY2(r.errorMessage.contains("helper.py"), qPrintable(r.errorMessage));
        QVERIFY2(r.errorMessage.contains("Traceback"), qPrintable(r.errorMessage));
        QVERIFY(r.startTime.isValid() && r.endTime.isValid());
    }

    void missingFunction()
    {
        PyFunctionCallResult r = call("does_not_exist");
        QVERIFY(r.error);
        QVERIFY2(r.errorMessage.contains("cannot find python function"), qPrintable(r.errorMessage));
    }

    void unsupportedParameter()
    {
        PyFunctionCallResult r = call("echo", QVariantList() << 1 << QVariant::fromValue(QPoint(1, 2)));
        QVERIFY(r.error);
        QVERIFY2(r.errorMessage.contains("parameter 1") && r.errorMessage.contains("QPoint"), qPrintable(r.errorMessage));
    }

    void unsupportedReturn()
    {
        PyFunctionCallResult r = call("unsupported_return");
        QVERIFY(r.error);
        QVERIFY2(r.errorMessage.contains("unsupported return type"), qPrintable(r.errorMessage));
    }

    void worksAfterErrors()
    {
        call("boom", QVariantList() << 1);
        call("too_big");
        PyFunctionCallResult r = call("crc", QVariantList() << QByteArray("123456789"));
        QVERIFY2(!r.error, qPrintable(r.errorMessage));
        QCOMPARE(r.returnValue.toULongLong(), 0xCBF43926ULL);
    }

    void bundledStdlib()
    {
        PyFunctionCallResult r = call("stdlib_check");
        QVERIFY2(!r.error, qPrintable(r.errorMessage));
        const QVariantList l = r.returnValue.toList();
        QCOMPARE(l.value(0).toString(), QString("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
        QCOMPARE(l.value(1).toByteArray(), QByteArray("\x01\x00\x02\x00\x00\x00", 6));
        QCOMPARE(l.value(2).toString(), QString("OpenSSL"));
    }

    void printDoesNotBreakCalls()
    {
        PyFunctionCallResult r = call("say", QVariantList() << QString("hello from python"));
        QVERIFY2(!r.error, qPrintable(r.errorMessage));
        QCOMPARE(r.returnValue.toInt(), 17);
    }

    void syncCallLatency()
    {
        const int n = 1000;
        QElapsedTimer t;
        t.start();
        for (int i = 0; i < n; i++)
            QVERIFY(!call("crc", QVariantList() << QByteArray(64, char(i))).error);
        const double usPerCall = t.nsecsElapsed() / 1000.0 / n;
        qInfo() << "sync call latency:" << usPerCall << "us";
        //the old implementation polled every 100 ms; even an emulated VM must stay far below that
        QVERIFY2(usPerCall < 5000, qPrintable(QString::number(usPerCall)));
    }

    void syncCallsFromManyThreads()
    {
        const int threads = 4;
        const int callsPerThread = 200;
        QAtomicInt failures = 0;
        QList<QThread *> workers;
        for (int t = 0; t < threads; t++) {
            workers << QThread::create([this, t, &failures]() {
                for (int i = 0; i < callsPerThread; i++) {
                    const QString s = QString("t%1-%2").arg(t).arg(i);
                    PyFunctionCallResult r = m_runner->syncCallFunction("echo", QVariantList() << s);
                    if (r.error || r.returnValue.toList().value(0).toString() != s)
                        failures.ref();
                }
            });
            workers.last()->start();
        }
        for (QThread *w : std::as_const(workers)) {
            QVERIFY(w->wait(60000));
            delete w;
        }
        QCOMPARE(failures.loadRelaxed(), 0);
    }

    void talkbackMessage()
    {
        PyRunnerQt runner(talkbackScript(), QStringList());
        QSignalSpy spy(&runner, &PyRunnerQt::messageReceived);
        PyFunctionCallResult r = runner.syncCallFunction("send", QVariantList() << QString::fromUtf8("hi \xc3\xa8"));
        QVERIFY2(!r.error, qPrintable(r.errorMessage));
        QCOMPARE(r.returnValue.toString(), runner.runnerId());
        QVERIFY(runner.runnerId().startsWith("NrPyCpp-talkback{"));
        QTRY_COMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toString(), QString::fromUtf8("hi \xc3\xa8"));
        //another runner has another id
        PyRunnerQt other(talkbackScript(), QStringList());
        QVERIFY(other.runnerId() != runner.runnerId());
        QCOMPARE(other.syncCallFunction("send", QVariantList() << "x").returnValue.toString(), other.runnerId());
        QCOMPARE(spy.count(), 1);
    }

    void talkbackMessageErrors()
    {
        PyRunnerQt runner(talkbackScript(), QStringList());
        QSignalSpy spy(&runner, &PyRunnerQt::messageReceived);
        PyFunctionCallResult r = runner.syncCallFunction("send_bad");
        QVERIFY(r.error);
        QVERIFY2(r.errorMessage.contains("TypeError"), qPrintable(r.errorMessage));
        QTest::qWait(50);
        QCOMPARE(spy.count(), 0);
    }

    void talkbackOutput()
    {
        PyRunnerQt runner(talkbackScript(), QStringList());
        QSignalSpy out(&runner, &PyRunnerQt::outputReceived);
        QSignalSpy err(&runner, &PyRunnerQt::errorOutputReceived);
        QVERIFY(!runner.syncCallFunction("lines").error);
        QTRY_COMPARE(out.count(), 2);
        QCOMPARE(spyStrings(out), QStringList({"a", "b"}));
        //a partial line is delivered when the call ends
        QVERIFY(!runner.syncCallFunction("partial").error);
        QTRY_COMPARE(out.count(), 3);
        QCOMPARE(out.at(2).at(0).toString(), QString("x"));
        QVERIFY(!runner.syncCallFunction("to_stderr").error);
        QTRY_COMPARE(err.count(), 1);
        QCOMPARE(err.at(0).at(0).toString(), QString("oops"));
        QTest::qWait(50);
        QCOMPARE(out.count(), 3);
    }

    void talkbackStreamBehavesLikeAStream()
    {
        PyRunnerQt runner(talkbackScript(), QStringList());
        PyFunctionCallResult r = runner.syncCallFunction("stream_info");
        QVERIFY2(!r.error, qPrintable(r.errorMessage));
        const QVariantList l = r.returnValue.toList();
        QCOMPARE(l.value(0).toString(), QString("_RoutedStream"));
        QVERIFY(!l.value(1).toString().isEmpty()); //encoding
        QVERIFY(!l.value(2).toString().isEmpty()); //errors
        QCOMPARE(l.value(4).toInt(), 5);           //write() returns the length
        QCOMPARE(l.value(5).toBool(), true);       //fileno()
        r = runner.syncCallFunction("write_bad");
        QVERIFY(r.error);
        QVERIFY2(r.errorMessage.contains("TypeError"), qPrintable(r.errorMessage));
    }

    void talkbackTwoRunners_data()
    {
        QTest::addColumn<bool>("sameScript");
        QTest::newRow("same script") << true;
        QTest::newRow("different scripts") << false;
    }

    void talkbackTwoRunners()
    {
        QFETCH(bool, sameScript);
        const int calls = 100;
        PyRunnerQt a(talkbackScript(), QStringList());
        PyRunnerQt b(sameScript ? talkbackScript() : codecScript(), codecDependencies());
        QSignalSpy outA(&a, &PyRunnerQt::outputReceived);
        QSignalSpy outB(&b, &PyRunnerQt::outputReceived);
        QSignalSpy errA(&a, &PyRunnerQt::errorOutputReceived);
        QSignalSpy errB(&b, &PyRunnerQt::errorOutputReceived);
        QAtomicInt failures = 0;
        QThread *ta = QThread::create([&]() {
            for (int i = 0; i < calls; i++)
                if (a.syncCallFunction("chatter", QVariantList() << "A" << 3).error)
                    failures.ref();
        });
        QThread *tb = QThread::create([&]() {
            for (int i = 0; i < calls; i++) {
                const bool bad = sameScript
                        ? b.syncCallFunction("chatter", QVariantList() << "B" << 3).error
                        : b.syncCallFunction("say", QVariantList() << "B-0\nB-1\nB-2").error;
                if (bad)
                    failures.ref();
            }
        });
        ta->start();
        tb->start();
        QVERIFY(ta->wait(60000));
        QVERIFY(tb->wait(60000));
        delete ta;
        delete tb;
        QCOMPARE(failures.loadRelaxed(), 0);
        //say() prints "B-0\nB-1\nB-2" plus a newline
        QTRY_COMPARE(outA.count(), calls * 3);
        QTRY_COMPARE(outB.count(), calls * 3);
        QTest::qWait(50);
        QCOMPARE(outA.count(), calls * 3);
        QCOMPARE(outB.count(), calls * 3);
        for (const QString &line : spyStrings(outA))
            QVERIFY2(line.startsWith("A-"), qPrintable(line));
        for (const QString &line : spyStrings(outB))
            QVERIFY2(line.startsWith("B-"), qPrintable(line));
        QCOMPARE(errA.count(), calls); //chatter writes to stderr once every 50 lines (i == 0 only)
        QCOMPARE(errB.count(), sameScript ? calls : 0);
        //every line arrived once, in order for each runner
        QStringList expected;
        for (int i = 0; i < calls; i++)
            expected << "A-0" << "A-1" << "A-2";
        QCOMPARE(spyStrings(outA), expected);
    }

    void talkbackThread()
    {
        PyRunnerQt runner(talkbackScript(), QStringList());
        PyRunnerQt other(talkbackScript(), QStringList());
        QSignalSpy out(&runner, &PyRunnerQt::outputReceived);
        QSignalSpy msg(&runner, &PyRunnerQt::messageReceived);
        QSignalSpy outOther(&other, &PyRunnerQt::outputReceived);
        QSignalSpy msgOther(&other, &PyRunnerQt::messageReceived);
        QVERIFY(!runner.syncCallFunction("thread_talks").error);
        QTRY_COMPARE(out.count(), 1);
        QTRY_COMPARE(msg.count(), 1);
        QCOMPARE(out.at(0).at(0).toString(), QString("from-thread"));
        QCOMPARE(msg.at(0).at(0).toString(), QString("msg-from-thread"));
        QTRY_VERIFY(runner.syncCallFunction("thread_runner_id").returnValue.toStringList().contains(runner.runnerId()));
        //the pool workers belong to the runner that submitted the work
        QVERIFY(!other.syncCallFunction("pool_talks").error);
        QTRY_COMPARE(outOther.count(), 4);
        QCOMPARE(msgOther.count(), 0);
        QCOMPARE(out.count(), 1);
    }

    void talkbackThreadPartialLine()
    {
        PyRunnerQt runner(talkbackScript(), QStringList());
        QSignalSpy out(&runner, &PyRunnerQt::outputReceived);
        QVERIFY(!runner.syncCallFunction("thread_partial").error);
        QTRY_COMPARE(out.count(), 1); //delivered when the thread ends
        QCOMPARE(out.at(0).at(0).toString(), QString("tail"));
    }

    void talkbackThreadException()
    {
        PyRunnerQt runner(talkbackScript(), QStringList());
        PyRunnerQt other(talkbackScript(), QStringList());
        QSignalSpy exc(&runner, &PyRunnerQt::unhandledException);
        QSignalSpy excOther(&other, &PyRunnerQt::unhandledException);
        QSignalSpy err(&runner, &PyRunnerQt::errorOutputReceived);
        QVERIFY(!runner.syncCallFunction("thread_fails").error);
        QTRY_COMPARE(exc.count(), 1);
        const QString tb = exc.at(0).at(0).toString();
        QVERIFY2(tb.contains("Traceback") && tb.contains("ValueError: thread-boom") && tb.contains("talkback.py"), qPrintable(tb));
        QTest::qWait(50);
        QCOMPARE(excOther.count(), 0);
        QCOMPARE(exc.count(), 1);
        QCOMPARE(err.count(), 0); //reported once, as an exception only
        //an exception inside a call is not an unhandled one
        QVERIFY(runner.syncCallFunction("send_bad").error);
        QTest::qWait(50);
        QCOMPARE(exc.count(), 1);
    }

    void talkbackUntrackedThread()
    {
        PyRunnerQt runner(talkbackScript(), QStringList());
        QSignalSpy msg(&runner, &PyRunnerQt::messageReceived);
        QVERIFY(!runner.syncCallFunction("raw_thread").error);
        QTRY_VERIFY(!runner.syncCallFunction("raw_thread_result").returnValue.toStringList().isEmpty());
        QCOMPARE(runner.syncCallFunction("raw_thread_result").returnValue.toStringList(), QStringList({"RuntimeError"}));
        QTest::qWait(50);
        QCOMPARE(msg.count(), 0);
    }

    void talkbackCallbackHandler()
    {
        PyRunnerQt runner(talkbackScript(), QStringList());
        RecordingHandler handler;
        runner.setCallbackHandler(&handler);
        QVERIFY(!runner.syncCallFunction("everything").error);
        QTRY_COMPARE(handler.exceptionCount(), 1);
        QTRY_COMPARE(handler.messageCount(), 1);
        QTRY_COMPARE(handler.outputCount(), 2);
        {
            QMutexLocker l(&handler.mutex);
            QCOMPARE(handler.output, QStringList({"line-1", "err-1"}));
            QCOMPARE(handler.messages, QStringList({"hello"}));
            QVERIFY2(handler.exceptions.at(0).contains("ZeroDivisionError"), qPrintable(handler.exceptions.at(0)));
        }
        //removed handlers get nothing
        runner.setCallbackHandler(nullptr);
        QVERIFY(!runner.syncCallFunction("lines").error);
        QTest::qWait(50);
        QCOMPARE(handler.outputCount(), 2);
    }

    void talkbackRunnerDestroyedWhileThreadPrints()
    {
        QSignalSpy *spy = nullptr;
        {
            PyRunnerQt doomed(talkbackScript(), QStringList());
            RecordingHandler handler;
            doomed.setCallbackHandler(&handler);
            QVERIFY(!doomed.syncCallFunction("ghost", QVariantList() << 1.0).error);
            QTRY_VERIFY(handler.outputCount() > 3);
            doomed.setCallbackHandler(nullptr);
            //destroyed here, its daemon thread keeps printing for a while
        }
        PyRunnerQt next(talkbackScript(), QStringList());
        QSignalSpy out(&next, &PyRunnerQt::outputReceived);
        spy = &out;
        for (int i = 0; i < 20; i++) {
            PyFunctionCallResult r = next.syncCallFunction("lines");
            QVERIFY2(!r.error, qPrintable(r.errorMessage));
            QTest::qWait(20);
        }
        QTest::qWait(1200); //the ghost goes on
        QCOMPARE(spy->count(), 40);
        for (const QString &line : spyStrings(*spy))
            QVERIFY(line == "a" || line == "b");
    }

    void virtualEnv()
    {
        PyFunctionCallResult r = call("venv_check");
        QVERIFY2(!r.error, qPrintable(r.errorMessage));
        QCOMPARE(r.returnValue.toString(), QString("from-venv"));
    }

    void syntaxErrorInScript()
    {
        PyRunnerQt bad(testsDir() + "/scripts/broken.py", QStringList());
        PyFunctionCallResult r = bad.syncCallFunction("f");
        QVERIFY(r.error);
        QVERIFY2(r.errorMessage.contains("SyntaxError") && r.errorMessage.contains("broken.py"), qPrintable(r.errorMessage));
    }

    void missingScript()
    {
        PyRunnerQt missing(testsDir() + "/scripts/no_such_script.py", QStringList());
        PyFunctionCallResult r = missing.syncCallFunction("f");
        QVERIFY(r.error);
        QVERIFY2(r.errorMessage.contains("cannot find script"), qPrintable(r.errorMessage));
    }

    void asyncCall()
    {
        QSignalSpy spy(m_runner, &PyRunnerQt::callCompletedSignal);
        const QString id = m_runner->asyncCallFunction("crc", QVariantList() << QByteArray("123456789"));
        QVERIFY(!QUuid(id).isNull());
        QVERIFY(m_runner->getAsyncCallsList().contains(id));
        QVERIFY(spy.count() == 1 || spy.wait(30000));
        QCOMPARE(spy.at(0).at(0).toString(), id);
        PyFunctionCallResult r = m_runner->getAsyncCallResult(id);
        QVERIFY2(!r.error, qPrintable(r.errorMessage));
        QCOMPARE(r.returnValue.toULongLong(), 0xCBF43926ULL);
        QVERIFY(!m_runner->getAsyncCallsList().contains(id));
        //a result can be retrieved only once
        QVERIFY(!m_runner->getAsyncCallResult(id).endTime.isValid());
    }

    void recreateRunnerOnSameScript()
    {
        for (int i = 0; i < 3; i++) {
            PyRunnerQt runner(codecScript(), codecDependencies());
            PyFunctionCallResult r = runner.syncCallFunction("echo", QVariantList() << i);
            QVERIFY2(!r.error, qPrintable(r.errorMessage));
            QCOMPARE(r.returnValue.toList().value(0).toInt(), i);
        }
        //and the long lived runner is not affected by the others coming and going
        QVERIFY(!call("echo").error);
    }

    void missingPythonHome()
    {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("NRPYCPP_PYTHONHOME", QDir::tempPath() + "/nrpycpp-no-such-python-home");
        QByteArray out;
        QVERIFY2(runChild("nohome", env, &out) == 0, out.constData());
    }

    void venvForAnotherPythonIsIgnored()
    {
        QTemporaryDir venv;
        QVERIFY(venv.isValid());
        QVERIFY(createFakeVenv(venv.path(), "3.1.4"));
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("NRPYCPP_VENV", venv.path());
        QByteArray out;
        QVERIFY2(runChild("venv-ignored", env, &out) == 0, out.constData());
        QVERIFY2(out.contains("ignoring it"), out.constData());
    }
};


/* the child scenarios: return 0 when the library behaves as expected */
static int childMain(const QString &scenario)
{
    if (scenario == "version") {
        if (!NrPyCpp::isPythonAvailable())
            return 1;
        PyRunnerQt runner(codecScript(), codecDependencies());
        PyFunctionCallResult r = runner.syncCallFunction("python_version");
        printf("VERSION=%s\n", qPrintable(r.returnValue.toString()));
        return r.error ? 1 : 0;
    }
    if (scenario == "nohome") {
        if (NrPyCpp::isPythonAvailable()) {
            printf("python started without a valid home\n");
            return 1;
        }
        PyRunnerQt runner(codecScript(), codecDependencies());
        PyFunctionCallResult r = runner.syncCallFunction("echo", QVariantList() << 1);
        printf("error: %s\n", qPrintable(r.errorMessage));
        return r.error && r.errorMessage.contains("not initialized") ? 0 : 1;
    }
    if (scenario == "venv-ignored") {
        if (!NrPyCpp::isPythonAvailable())
            return 1;
        PyRunnerQt runner(codecScript(), codecDependencies());
        PyFunctionCallResult r = runner.syncCallFunction("venv_check");
        printf("error: %s\n", qPrintable(r.errorMessage));
        return r.error && r.errorMessage.contains("ModuleNotFoundError") ? 0 : 1;
    }
    printf("unknown scenario %s\n", qPrintable(scenario));
    return 2;
}


int main(int argc, char *argv[])
{
    if (argc == 3 && qstrcmp(argv[1], "--child") == 0) {
        QCoreApplication app(argc, argv);
        const int ret = childMain(QString::fromLocal8Bit(argv[2]));
        fflush(stdout);
        return ret;
    }
    QCoreApplication app(argc, argv);
    TestNrPyCpp tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_nrpycpp.moc"
