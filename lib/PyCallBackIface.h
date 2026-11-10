/**************************************************************************
 *  Copyright (C) 2026 by NetResults S.r.l. ( http://www.netresults.it )  *
 *  Author( s ) :                                                         *
 *         Francesco Lamonica      <f.lamonica@netresults.it>             *
 **************************************************************************/

#ifndef PYCALLBACKIFACE_H
#define PYCALLBACKIFACE_H

#include <QString>
#include <QSharedPointer>

/*
 * Callbacks from the scripts of a runner (see PyRunnerQt::setCallbackHandler()), same interface as
 * the one of the previous releases. The methods are called synchronously from the python thread that
 * produced the event: keep them short and thread safe.
 *   onStdOutput   a line printed by the script, on stdout or on stderr (without the final "\n")
 *   onException   the traceback of an exception that escaped a python thread started by the script
 *   onSendMessage a text sent by the script with nrpycpp.send_message()
 */
class PyCallBackIface
{
public:
    virtual ~PyCallBackIface() {}
    virtual void onStdOutput(const QSharedPointer<QString> & log) = 0;
    virtual void onException(const QSharedPointer<QString> & exceptionMsg) = 0;
    virtual void onSendMessage(const QSharedPointer<QString> & message) = 0;
};

#endif // PYCALLBACKIFACE_H
