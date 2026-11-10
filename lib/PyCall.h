#ifndef PYCALL_H
#define PYCALL_H

#include <QDateTime>
#include <QUuid>
#include <QVariantList>
#include <QDebug>

struct PyFunctionCallResult
{
    QDateTime startTime;
    QDateTime endTime;
    QVariant returnValue;
    bool error = false;
    QString errorMessage;
};

class PyFunctionCall
{
    QUuid m_CallID;
    QString m_functionName;
    QVariantList m_paramsList;
    bool m_isSyncCall = true;
    bool m_valid = true;
    PyFunctionCallResult m_CallResult;
public:
    PyFunctionCall(const QString &i_pyFunctionToCall, QVariantList i_funcParams, bool i_syncCall)
        : m_functionName(i_pyFunctionToCall)
        , m_paramsList(i_funcParams)
        , m_isSyncCall(i_syncCall)
    {
        m_CallID = QUuid::createUuid();
    }
    //PyFunctionCall(const PyFunctionCall &rhs) = delete;
    PyFunctionCall() { m_valid = false; }; //needed to use in QHash / QMap
    PyFunctionCallResult& result() { return m_CallResult; }
    QUuid id() const { return m_CallID; }
    QString functionName() const { return m_functionName; }
    bool isSync() const { return m_isSyncCall; }
    bool isValid() const { return m_valid; }
    QVariantList params() const { return m_paramsList; }
    bool operator==(const PyFunctionCall &rhs) { return m_CallID == rhs.m_CallID; }
    QString getInfo() const {
        QString s("Call Id: %1, function: %2 (%3), params: %4, result info: %5");
        QStringList params;
        foreach (QVariant v, m_paramsList)
            { params << v.toString(); }
        s = s.arg(id().toString(), functionName(), (isSync() ? "SYNC" : "ASYNC"), params.join("/"), m_CallResult.returnValue.toString());
        return s;
    }
};

Q_DECLARE_METATYPE(PyFunctionCall)


#endif // PYCALL_H
