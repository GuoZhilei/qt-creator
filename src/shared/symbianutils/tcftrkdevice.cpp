/**************************************************************************
**
** This file is part of Qt Creator
**
** Copyright (c) 2010 Nokia Corporation and/or its subsidiary(-ies).
**
** Contact: Nokia Corporation (qt-info@nokia.com)
**
** Commercial Usage
**
** Licensees holding valid Qt Commercial licenses may use this file in
** accordance with the Qt Commercial License Agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Nokia.
**
** GNU Lesser General Public License Usage
**
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** If you are unsure which license is appropriate for your use, please
** contact the sales department at http://qt.nokia.com/contact.
**
**************************************************************************/

#include "tcftrkdevice.h"
#include "json.h"

#include <QtNetwork/QAbstractSocket>
#include <QtCore/QDebug>
#include <QtCore/QVector>
#include <QtCore/QQueue>
#include <QtCore/QTextStream>
#include <QtCore/QDateTime>
#include <QtCore/QFileInfo>

enum { debug = 0 };

static const char messageTerminatorC[] = "\003\001";

namespace tcftrk {
// ------------- TcfTrkCommandError

TcfTrkCommandError::TcfTrkCommandError() : timeMS(0), code(0), alternativeCode(0)
{
}

void TcfTrkCommandError::clear()
{
    timeMS = 0;
    code = alternativeCode = 0;
    format.clear();
    alternativeOrganization.clear();
}

void TcfTrkCommandError::write(QTextStream &str) const
{
    if (isError()) {
        if (timeMS) {
            const QDateTime time(QDate(1970, 1, 1));
            str << time.addMSecs(timeMS).toString(Qt::ISODate) << ": ";
        }
        str << "Error code: " << code
                << " '" << format << '\'';
        if (!alternativeOrganization.isEmpty())
            str << " ('" << alternativeOrganization << "', code: " << alternativeCode << ')';
    } else{
        str << "<No error>";
    }
}

QString TcfTrkCommandError::toString() const
{
    QString rc;
    QTextStream str(&rc);
    write(str);
    return rc;
}

bool TcfTrkCommandError::isError() const
{
    return timeMS != 0 || code != 0 || !format.isEmpty() || alternativeCode != 0;
}

/* {"Time":1277459762255,"Code":1,"AltCode":-6,"AltOrg":"POSIX","Format":"Unknown error: -6"} */
bool TcfTrkCommandError::parse(const QVector<JsonValue> &values)
{
    // Parse an arbitrary hash (that could as well be a command response)
    // and check for error elements. It looks like sometimes errors are appended
    // to other values.
    unsigned errorKeyCount = 0;
    clear();
    do {
        if (values.isEmpty() || values.back().type() != JsonValue::Object)
            break;
        foreach (const JsonValue &c, values.back().children()) {
            if (c.name() == "Time") {
                timeMS = c.data().toULongLong();
                errorKeyCount++;
            } else if (c.name() == "Code") {
                code = c.data().toLongLong();
                errorKeyCount++;
            } else if (c.name() == "Format") {
                format = c.data();
                errorKeyCount++;
            } else if (c.name() == "AltCode") {
                alternativeCode = c.data().toULongLong();
                errorKeyCount++;
            } else if (c.name() == "AltOrg") {
                alternativeOrganization = c.data();
                errorKeyCount++;
            }
        }
    } while (false);
    const bool errorFound = errorKeyCount >= 2u; // Should be at least 'Time', 'Code'.
    if (!errorFound)
        clear();
    if (debug) {
        qDebug("TcfTrkCommandError::parse: Found error %d (%u): ", errorFound, errorKeyCount);
        if (!values.isEmpty())
            qDebug() << values.back().toString();
    }
    return errorFound;
}

// ------------ TcfTrkCommandResult

TcfTrkCommandResult::TcfTrkCommandResult(Type t) :
    type(t), service(LocatorService)
{
}

TcfTrkCommandResult::TcfTrkCommandResult(char typeChar, Services s,
                                         const QByteArray &r,
                                         const QVector<JsonValue> &v,
                                         const QVariant &ck) :
    type(FailReply), service(s), request(r), values(v), cookie(ck)
{
    switch (typeChar) {
    case 'N':
        type = FailReply;
        break;
    case 'P':
        type = ProgressReply;
        break;
    case 'R':
        type = commandError.parse(values) ? CommandErrorReply : SuccessReply;
        break;
    default:
        qWarning("Unknown TCF reply type '%c'", typeChar);
    }
}

QString TcfTrkCommandResult::errorString() const
{
    QString rc;
    QTextStream str(&rc);

    switch (type) {
    case SuccessReply:
    case ProgressReply:
        str << "<No error>";
        return rc;
    case FailReply:
        str << "NAK";
        return rc;
    case CommandErrorReply:
        commandError.write(str);
        break;
    }
    // Append the failed command for reference
    str << " (Command was: '";
    QByteArray printableRequest = request;
    printableRequest.replace('\0', '|');
    str << printableRequest << "')";
    return rc;
}

QString TcfTrkCommandResult::toString() const
{
    QString rc;
    QTextStream str(&rc);
    str << "Command answer ";
    switch (type) {
    case SuccessReply:
        str << "[success]";
        break;
    case CommandErrorReply:
        str << "[command error]";
        break;
    case FailReply:
        str << "[fail (NAK)]";
        break;
    case ProgressReply:
        str << "[progress]";
        break;
    }
    str << ", " << values.size() << " values(s) to request: '";
    QByteArray printableRequest = request;
    printableRequest.replace('\0', '|');
    str << printableRequest << "' ";
    if (cookie.isValid())
        str << " cookie: " << cookie.toString();
    str << '\n';
    for (int i = 0, count = values.size(); i < count; i++)
        str << '#' << i << ' ' << values.at(i).toString() << '\n';
    if (type == CommandErrorReply)
        str << "Error: " << errorString();
    return rc;
}

// Entry for send queue.
enum SpecialHandlingFlags { None =0,
                            FakeRegisterGetMIntermediate = 0x1,
                            FakeRegisterGetMFinal = 0x2 };

struct TcfTrkSendQueueEntry
{
    typedef TcfTrkDevice::MessageType MessageType;

    explicit TcfTrkSendQueueEntry(MessageType mt,
                                  int tok,
                                  Services s,
                                  const QByteArray &d,
                                  const TcfTrkCallback &cb= TcfTrkCallback(),
                                  const QVariant &ck = QVariant(),
                                  unsigned sh = 0) :
        messageType(mt), service(s), data(d), token(tok), cookie(ck), callback(cb),
        specialHandling(sh) {}

    MessageType messageType;
    Services service;
    QByteArray data;
    int token;
    QVariant cookie;
    TcfTrkCallback callback;
    unsigned specialHandling;
};

struct TcfTrkDevicePrivate {
    typedef TcfTrkDevice::IODevicePtr IODevicePtr;
    typedef QHash<int, TcfTrkSendQueueEntry> TokenWrittenMessageMap;

    TcfTrkDevicePrivate();

    const QByteArray m_messageTerminator;

    IODevicePtr m_device;
    unsigned m_verbose;
    QByteArray m_readBuffer;
    int m_token;
    QQueue<TcfTrkSendQueueEntry> m_sendQueue;
    TokenWrittenMessageMap m_writtenMessages;
    QVector<QByteArray> m_registerNames;
    QVector<QByteArray> m_fakeGetMRegisterValues;
};

TcfTrkDevicePrivate::TcfTrkDevicePrivate() :
    m_messageTerminator(messageTerminatorC),
    m_verbose(0), m_token(0)
{
}

TcfTrkDevice::TcfTrkDevice(QObject *parent) :
    QObject(parent), d(new TcfTrkDevicePrivate)
{
}

TcfTrkDevice::~TcfTrkDevice()
{
    delete d;
}

QVector<QByteArray> TcfTrkDevice::registerNames() const
{
    return d->m_registerNames;
}

void TcfTrkDevice::setRegisterNames(const QVector<QByteArray>& n)
{
    d->m_registerNames = n;
    if (d->m_verbose) {
        QString msg;
        QTextStream str(&msg);
        const int count = n.size();
        str << "Registers (" << count << "): ";
        for (int i = 0; i < count; i++)
            str << '#' << i << '=' << n.at(i) << ' ';
        emitLogMessage(msg);
    }
}

TcfTrkDevice::IODevicePtr TcfTrkDevice::device() const
{
    return d->m_device;
}

TcfTrkDevice::IODevicePtr TcfTrkDevice::takeDevice()
{
    const IODevicePtr old = d->m_device;
    if (!old.isNull()) {
        old.data()->disconnect(this);
        d->m_device = IODevicePtr();
    }
    d->m_readBuffer.clear();
    d->m_token = 0;
    d->m_sendQueue.clear();
    return old;
}

void TcfTrkDevice::setDevice(const IODevicePtr &dp)
{
    if (dp.data() == d->m_device.data())
        return;
    if (dp.isNull()) {
        emitLogMessage(QLatin1String("Internal error: Attempt to set NULL device."));
        return;
    }
    takeDevice();
    d->m_device = dp;
    connect(dp.data(), SIGNAL(readyRead()), this, SLOT(slotDeviceReadyRead()));
    if (QAbstractSocket *s = qobject_cast<QAbstractSocket *>(dp.data())) {
        connect(s, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(slotDeviceError()));
        connect(s, SIGNAL(stateChanged(QAbstractSocket::SocketState)), this, SLOT(slotDeviceSocketStateChanged()));
    }
}

void TcfTrkDevice::slotDeviceError()
{
    const QString message = d->m_device->errorString();
    emitLogMessage(message);
    emit error(message);
}

void TcfTrkDevice::slotDeviceSocketStateChanged()
{
    if (const QAbstractSocket *s = qobject_cast<const QAbstractSocket *>(d->m_device.data())) {
        const QAbstractSocket::SocketState st = s->state();
        switch (st) {
        case QAbstractSocket::UnconnectedState:
            emitLogMessage(QLatin1String("Unconnected"));
            break;
        case QAbstractSocket::HostLookupState:
            emitLogMessage(QLatin1String("HostLookupState"));
            break;
        case QAbstractSocket::ConnectingState:
            emitLogMessage(QLatin1String("Connecting"));
            break;
        case QAbstractSocket::ConnectedState:
            emitLogMessage(QLatin1String("Connected"));
            break;
        case QAbstractSocket::ClosingState:
            emitLogMessage(QLatin1String("Closing"));
            break;
        default:
            emitLogMessage(QString::fromLatin1("State %1").arg(st));
            break;
        }
    }
}

static inline QString debugMessage(QByteArray  message, const char *prefix = 0)
{
    message.replace('\0', '|');
    const QString messageS = QString::fromLatin1(message);
    return prefix ?
            (QLatin1String(prefix) + messageS) :  messageS;
}

void TcfTrkDevice::slotDeviceReadyRead()
{
    d->m_readBuffer += d->m_device->readAll();
    // Take complete message off front of readbuffer.
    do {
        const int messageEndPos = d->m_readBuffer.indexOf(d->m_messageTerminator);        
        if (messageEndPos == -1)
            break;
        if (messageEndPos == 0) {
            // TCF TRK 4.0.5 emits empty messages on errors.
            emitLogMessage(QString::fromLatin1("An empty TCF TRK message has been received."));
        } else {
            const QByteArray message = d->m_readBuffer.left(messageEndPos);
            if (debug)
                qDebug("Read %d bytes:\n%s", message.size(), qPrintable(formatData(message)));
            if (const int errorCode = parseMessage(message)) {
                emitLogMessage(QString::fromLatin1("Parse error %1 : %2").
                               arg(errorCode).arg(debugMessage(message)));
                if (debug)
                    qDebug("Parse error %d for %d bytes:\n%s", errorCode,
                           message.size(), qPrintable(formatData(message)));
            }
        }
        d->m_readBuffer.remove(0, messageEndPos + d->m_messageTerminator.size());
    } while (!d->m_readBuffer.isEmpty());
    checkSendQueue(); // Send off further message
}

// Split \0-terminated message into tokens, skipping the initial type character
static inline QVector<QByteArray> splitMessage(const QByteArray &message)
{
    QVector<QByteArray> tokens;
    tokens.reserve(7);
    const int messageSize = message.size();
    for (int pos = 2; pos < messageSize; ) {
        const int nextPos = message.indexOf('\0', pos);
        if (nextPos == -1)
            break;
        tokens.push_back(message.mid(pos, nextPos - pos));
        pos = nextPos + 1;
    }
    return tokens;
}

int TcfTrkDevice::parseMessage(const QByteArray &message)
{
    if (d->m_verbose)
        emitLogMessage(debugMessage(message, "TCF ->"));
    // Special JSON parse error message or protocol format error.
    // The port is usually closed after receiving it.
    // "\3\2{"Time":1276096098255,"Code":3,"Format": "Protocol format error"}"
    if (message.startsWith("\003\002")) {
        QByteArray text = message.mid(2);
        const QString errorMessage = QString::fromLatin1("Parse error received: %1").arg(QString::fromAscii(text));
        emit error(errorMessage);
        return 0;
    }
    if (message.size() < 4 || message.at(1) != '\0')
        return 1;
    // Split into tokens
    const char type = message.at(0);
    const QVector<QByteArray> tokens = splitMessage(message);
    switch (type) {
    case 'E':
        return parseTcfEvent(tokens);
    case 'R': // Command replies
    case 'N':
    case 'P':
        return parseTcfCommandReply(type, tokens);
    default:
        emitLogMessage(QString::fromLatin1("Unhandled message type: %1").arg(debugMessage(message)));
        return 756;
    }
    return 0;
}

int TcfTrkDevice::parseTcfCommandReply(char type, const QVector<QByteArray> &tokens)
{
    typedef TcfTrkDevicePrivate::TokenWrittenMessageMap::iterator TokenWrittenMessageMapIterator;
    // Find the corresponding entry in the written messages hash.
    const int tokenCount = tokens.size();
    if (tokenCount < 1)
        return 234;
    bool tokenOk;
    const int token = tokens.at(0).toInt(&tokenOk);
    if (!tokenOk)
        return 235;
    const TokenWrittenMessageMapIterator it = d->m_writtenMessages.find(token);
    if (it == d->m_writtenMessages.end()) {
        qWarning("TcfTrkDevice: Internal error: token %d not found for '%s'",
                 token, qPrintable(joinByteArrays(tokens)));
        return 236;
    }
    // No callback: remove entry from map, happy
    const unsigned specialHandling = it.value().specialHandling;
    if (!it.value().callback && specialHandling == 0u) {
        d->m_writtenMessages.erase(it);
        return 0;
    }
    // Parse values into JSON
    QVector<JsonValue> values;
    values.reserve(tokenCount);
    for (int i = 1; i < tokenCount; i++) {
        if (!tokens.at(i).isEmpty()) { // Strange: Empty tokens occur.
            const JsonValue value(tokens.at(i));
            if (value.isValid()) {
                values.push_back(value);
            } else {
                qWarning("JSON parse error for reply to command token %d: #%d '%s'",
                         token, i, tokens.at(i).constData());
                d->m_writtenMessages.erase(it);
                return -1;
            }
        }
    }
    // Construct result and invoke callback, remove entry from map.
    TcfTrkCommandResult result(type, it.value().service, it.value().data,
                               values, it.value().cookie);

    // Check special handling
    if (specialHandling) {
        if (!result) {
            qWarning("Error in special handling: %s", qPrintable(result.errorString()));
            return -2;
        }
        // Fake 'Registers:getm': Store single register values in cache
        if ((specialHandling & FakeRegisterGetMIntermediate)
                || (specialHandling & FakeRegisterGetMFinal)) {
            if (result.values.size() == 1) {
                const int index = int(specialHandling) >> 16;
                if (index >= 0 && index < d->m_fakeGetMRegisterValues.size()) {
                    const QByteArray base64 = result.values.front().data();
                    d->m_fakeGetMRegisterValues[index] = base64;
                    if (d->m_verbose) {
                        const QString msg = QString::fromLatin1("Caching register value #%1 '%2' 0x%3 (%4)").
                                arg(index).arg(QString::fromAscii(d->m_registerNames.at(index))).
                                arg(QString::fromAscii(QByteArray::fromBase64(base64).toHex())).
                                arg(QString::fromAscii(base64));
                        emitLogMessage(msg);
                    }
                }
            }
        }
        // Fake 'Registers:getm' final value: Reformat entries as array and send off
        if (specialHandling & FakeRegisterGetMFinal) {
            QByteArray str;
            str.append('[');
            foreach(const QByteArray &rval, d->m_fakeGetMRegisterValues)
                if (!rval.isEmpty()) {
                    if (str.size() > 1)
                        str.append(',');
                    str.append('"');
                    str.append(rval);
                    str.append('"');
                }
            str.append(']');
            result.values[0] = JsonValue(str);
        }
    }
    if (it.value().callback)
        it.value().callback(result);
    d->m_writtenMessages.erase(it);
    return 0;
}

static const char locatorAnswerC[] = "E\0Locator\0Hello\0[\"Locator\"]";

int TcfTrkDevice::parseTcfEvent(const QVector<QByteArray> &tokens)
{
    // Event: Ignore the periodical heartbeat event, answer 'Hello',
    // emit signal for the rest
    if (tokens.size() < 3)
        return 433;
    const Services service = serviceFromName(tokens.at(0).constData());
    if (service == LocatorService && tokens.at(1) == "peerHeartBeat")
        return 0;
    QVector<JsonValue> values;
    for (int i = 2; i < tokens.size(); i++) {
        const JsonValue value(tokens.at(i));
        if (!value.isValid())
            return 434;
        values.push_back(value);
    }
    // Parse known events, emit signals
    QScopedPointer<TcfTrkEvent> knownEvent(TcfTrkEvent::parseEvent(service, tokens.at(1), values));
    if (!knownEvent.isNull()) {
        // Answer hello event.
        if (knownEvent->type() == TcfTrkEvent::LocatorHello)
            writeMessage(QByteArray(locatorAnswerC, sizeof(locatorAnswerC)));
        emit tcfEvent(*knownEvent);
    }
    emit genericTcfEvent(service, tokens.at(1), values);

    if (debug || d->m_verbose) {
        QString msg;
        QTextStream str(&msg);
        if (knownEvent.isNull()) {
            str << "Event: " << tokens.at(0) << ' ' << tokens.at(1) << '\n';
            foreach(const JsonValue &val, values)
                str << "  " << val.toString() << '\n';
        } else {
            str << knownEvent->toString();
        }
        emitLogMessage(msg);
    }

    return 0;
}

unsigned TcfTrkDevice::verbose() const
{
    return d->m_verbose;
}

void TcfTrkDevice::setVerbose(unsigned v)
{
    d->m_verbose = v;
}

void TcfTrkDevice::emitLogMessage(const QString &m)
{
    if (debug)
        qWarning("%s", qPrintable(m));
    emit logMessage(m);
}

bool TcfTrkDevice::checkOpen()
{
    if (d->m_device.isNull()) {
        emitLogMessage(QLatin1String("Internal error: No device set on TcfTrkDevice."));
        return false;
    }
    if (!d->m_device->isOpen()) {
        emitLogMessage(QLatin1String("Internal error: Device not open in TcfTrkDevice."));
        return false;
    }
    return true;
}

void TcfTrkDevice::sendTcfTrkMessage(MessageType mt, Services service, const char *command,
                                     const char *commandParameters, // may contain '\0'
                                     int commandParametersLength,
                                     const TcfTrkCallback &callBack,
                                     const QVariant &cookie)
{
    sendTcfTrkMessage(mt, service, command, commandParameters, commandParametersLength,
                      callBack, cookie, 0u);
}

void TcfTrkDevice::sendTcfTrkMessage(MessageType mt, Services service,
                       const char *command,
                       const char *commandParameters, int commandParametersLength,
                       const TcfTrkCallback &callBack, const QVariant &cookie,
                       unsigned specialHandling)

{
    if (!checkOpen())
        return;
    // Format the message
    const int  token = d->m_token++;
    QByteArray data;
    data.reserve(30 + commandParametersLength);
    data.append('C');
    data.append('\0');
    data.append(QByteArray::number(token));
    data.append('\0');
    data.append(serviceName(service));
    data.append('\0');
    data.append(command);
    data.append('\0');
    if (commandParametersLength)
        data.append(commandParameters, commandParametersLength);
    const TcfTrkSendQueueEntry entry(mt, token, service, data, callBack, cookie, specialHandling);
    d->m_sendQueue.enqueue(entry);
    checkSendQueue();
}

void TcfTrkDevice::sendTcfTrkMessage(MessageType mt, Services service, const char *command,
                                     const QByteArray &commandParameters,
                                     const TcfTrkCallback &callBack,
                                     const QVariant &cookie)
{
    sendTcfTrkMessage(mt, service, command, commandParameters.constData(), commandParameters.size(),
                      callBack, cookie);
}

// Enclose in message frame and write.
void TcfTrkDevice::writeMessage(QByteArray data)
{
    if (!checkOpen())
        return;

    if (d->m_verbose)
        emitLogMessage(debugMessage(data, "TCF <-"));

    // Ensure \0-termination which easily gets lost in QByteArray CT.
    if (!data.endsWith('\0'))
        data.append('\0');
    data += d->m_messageTerminator;

    if (debug > 1)
        qDebug("Writing:\n%s", qPrintable(formatData(data)));

    d->m_device->write(data);
    if (QAbstractSocket *as = qobject_cast<QAbstractSocket *>(d->m_device.data()))
        as->flush();
}

void TcfTrkDevice::checkSendQueue()
{
    // Fire off messages or invoke noops until a message with reply is found
    // and an entry to writtenMessages is made.
    while (d->m_writtenMessages.empty()) {
        if (d->m_sendQueue.isEmpty())
            break;
        TcfTrkSendQueueEntry entry = d->m_sendQueue.dequeue();
        switch (entry.messageType) {
        case MessageWithReply:
            d->m_writtenMessages.insert(entry.token, entry);
            writeMessage(entry.data);
            break;
        case MessageWithoutReply:
            writeMessage(entry.data);
            break;
        case NoopMessage: // Invoke the noop-callback for synchronization
            if (entry.callback) {
                TcfTrkCommandResult noopResult(TcfTrkCommandResult::SuccessReply);
                noopResult.cookie = entry.cookie;
                entry.callback(noopResult);
            }
            break;
        }
    }
}

// Fix slashes
static inline QString fixFileName(QString in)
{
    in.replace(QLatin1Char('/'), QLatin1Char('\\'));
    return in;
}

// Start a process (consisting of a non-reply setSettings and start).
void TcfTrkDevice::sendProcessStartCommand(const TcfTrkCallback &callBack,
                                                 const QString &binaryIn,
                                                 unsigned uid,
                                                 QStringList arguments,
                                                 QString workingDirectory,
                                                 bool debugControl,
                                                 const QStringList &additionalLibraries,
                                                 const QVariant &cookie)
{
    // Obtain the bin directory, expand by c:/sys/bin if missing
    const QChar backSlash('\\');
    int slashPos = binaryIn.lastIndexOf(QLatin1Char('/'));
    if (slashPos == -1)
        slashPos = binaryIn.lastIndexOf(backSlash);
    const QString sysBin = QLatin1String("c:/sys/bin");
    const QString binaryFileName  = slashPos == -1 ? binaryIn : binaryIn.mid(slashPos + 1);
    const QString binaryDirectory = slashPos == -1 ? sysBin : binaryIn.left(slashPos);
    const QString binary = fixFileName(binaryDirectory + QLatin1Char('/') + binaryFileName);

    // Fixup: Does argv[0] convention exist on Symbian?
    arguments.push_front(binary);
    if (workingDirectory.isEmpty())
        workingDirectory = sysBin;

    // Format settings with empty dummy parameter
    QByteArray setData;
    JsonInputStream setStr(setData);
    setStr << "" << '\0'
            << '[' << "exeToLaunch" << ',' << "addExecutables" << ',' << "addLibraries" << ']'
            << '\0' << '['
                << binary << ','
                << '{' << binaryFileName << ':' << QString::number(uid, 16) << '}' << ','
                << additionalLibraries
            << ']';
    sendTcfTrkMessage(
#if 1
                MessageWithReply,    // TCF TRK 4.0.5 onwards
#else
                MessageWithoutReply, // TCF TRK 4.0.2
#endif
                SettingsService, "set", setData);

    QByteArray startData;
    JsonInputStream startStr(startData);
    startStr << fixFileName(workingDirectory)
            << '\0' << binary << '\0' << arguments << '\0'
            << QStringList() << '\0' // Env is an array ["PATH=value"] (non-standard)
            << debugControl;
    sendTcfTrkMessage(MessageWithReply, ProcessesService, "start", startData, callBack, cookie);
}

void TcfTrkDevice::sendProcessTerminateCommand(const TcfTrkCallback &callBack,
                                               const QByteArray &id,
                                               const QVariant &cookie)
{
    QByteArray data;
    JsonInputStream str(data);
    str << id;
    sendTcfTrkMessage(MessageWithReply, ProcessesService, "terminate", data, callBack, cookie);
}

void TcfTrkDevice::sendRunControlTerminateCommand(const TcfTrkCallback &callBack,
                                                  const QByteArray &id,
                                                  const QVariant &cookie)
{
    QByteArray data;
    JsonInputStream str(data);
    str << id;
    sendTcfTrkMessage(MessageWithReply, RunControlService, "terminate", data, callBack, cookie);
}

// Non-standard: Remove executable from settings
void TcfTrkDevice::sendSettingsRemoveExecutableCommand(const QString &binaryIn,
                                                       unsigned uid,
                                                       const QStringList &additionalLibraries,
                                                       const QVariant &cookie)
{
    QByteArray setData;
    JsonInputStream setStr(setData);
    setStr << "" << '\0'
            << '[' << "removedExecutables" << ',' << "removedLibraries" << ']'
            << '\0' << '['
                << '{' << QFileInfo(binaryIn).fileName() << ':' << QString::number(uid, 16) << '}' << ','
                << additionalLibraries
            << ']';
    sendTcfTrkMessage(MessageWithoutReply, SettingsService, "set", setData, TcfTrkCallback(), cookie);
}

void TcfTrkDevice::sendRunControlResumeCommand(const TcfTrkCallback &callBack,
                                               const QByteArray &id,
                                               RunControlResumeMode mode,
                                               unsigned count,
                                               quint64 rangeStart,
                                               quint64 rangeEnd,
                                               const QVariant &cookie)
{
    QByteArray resumeData;
    JsonInputStream str(resumeData);
    str << id << '\0' << int(mode) << '\0' << count;
    switch (mode) {
    case RM_STEP_OVER_RANGE:
    case RM_STEP_INTO_RANGE:
    case RM_REVERSE_STEP_OVER_RANGE:
    case RM_REVERSE_STEP_INTO_RANGE:
        str << '\0' << '{' << "RANGE_START" << ':' << rangeStart
                << ',' << "RANGE_END" << ':' << rangeEnd << '}';
        break;
    default:
        break;
    }
    sendTcfTrkMessage(MessageWithReply, RunControlService, "resume", resumeData, callBack, cookie);
}

void TcfTrkDevice::sendRunControlSuspendCommand(const TcfTrkCallback &callBack,
                                                const QByteArray &id,
                                                const QVariant &cookie)
{
    QByteArray data;
    JsonInputStream str(data);
    str << id;
    sendTcfTrkMessage(MessageWithReply, RunControlService, "suspend", data, callBack, cookie);
}

void TcfTrkDevice::sendRunControlResumeCommand(const TcfTrkCallback &callBack,
                                               const QByteArray &id,
                                               const QVariant &cookie)
{
    sendRunControlResumeCommand(callBack, id, RM_RESUME, 1, 0, 0, cookie);
}

void TcfTrkDevice::sendBreakpointsAddCommand(const TcfTrkCallback &callBack,
                                             const Breakpoint &bp,
                                             const QVariant &cookie)
{
    QByteArray data;
    JsonInputStream str(data);
    str << bp;
    sendTcfTrkMessage(MessageWithReply, BreakpointsService, "add", data, callBack, cookie);
}

void TcfTrkDevice::sendBreakpointsRemoveCommand(const TcfTrkCallback &callBack,
                                                const QByteArray &id,
                                                const QVariant &cookie)
{
    sendBreakpointsRemoveCommand(callBack, QVector<QByteArray>(1, id), cookie);
}

void TcfTrkDevice::sendBreakpointsRemoveCommand(const TcfTrkCallback &callBack,
                                                const QVector<QByteArray> &ids,
                                                const QVariant &cookie)
{
    QByteArray data;
    JsonInputStream str(data);
    str << ids;
    sendTcfTrkMessage(MessageWithReply, BreakpointsService, "remove", data, callBack, cookie);
}

void TcfTrkDevice::sendBreakpointsEnableCommand(const TcfTrkCallback &callBack,
                                                const QByteArray &id,
                                                bool enable,
                                                const QVariant &cookie)
{
    sendBreakpointsEnableCommand(callBack, QVector<QByteArray>(1, id), enable, cookie);
}

void TcfTrkDevice::sendBreakpointsEnableCommand(const TcfTrkCallback &callBack,
                                                const QVector<QByteArray> &ids,
                                                bool enable,
                                                const QVariant &cookie)
{
    QByteArray data;
    JsonInputStream str(data);
    str << ids;
    sendTcfTrkMessage(MessageWithReply, BreakpointsService,
                      enable ? "enable" : "disable",
                      data, callBack, cookie);
}

void TcfTrkDevice::sendMemorySetCommand(const TcfTrkCallback &callBack,
                                        const QByteArray &contextId,
                                        quint64 start, const QByteArray& data,
                                        const QVariant &cookie)
{
    QByteArray getData;
    JsonInputStream str(getData);
    // start/word size/mode. Mode should ideally be 1 (continue on error?)
    str << contextId << '\0' << start << '\0' << 1 << '\0' << data.size() << '\0' << 1
        << '\0' << data.toBase64();
    sendTcfTrkMessage(MessageWithReply, MemoryService, "set", getData, callBack, cookie);
}

void TcfTrkDevice::sendMemoryGetCommand(const TcfTrkCallback &callBack,
                                        const QByteArray &contextId,
                                        quint64 start, quint64 size,
                                        const QVariant &cookie)
{
    QByteArray data;
    JsonInputStream str(data);
    // start/word size/mode. Mode should ideally be 1 (continue on error?)
    str << contextId << '\0' << start << '\0' << 1 << '\0' << size << '\0' << 1;
    sendTcfTrkMessage(MessageWithReply, MemoryService, "get", data, callBack, cookie);
}

QByteArray TcfTrkDevice::parseMemoryGet(const TcfTrkCommandResult &r)
{
    if (r.type != TcfTrkCommandResult::SuccessReply || r.values.size() < 1)
        return QByteArray();
    const JsonValue &memoryV = r.values.front();

    if (memoryV.type() != JsonValue::String || memoryV.data().size() < 2
        || !memoryV.data().endsWith('='))
        return QByteArray();
    // Catch errors reported as hash:
    // R.4."TlVMTA==".{"Time":1276786871255,"Code":1,"AltCode":-38,"AltOrg":"POSIX","Format":"BadDescriptor"}
    // Not sure what to make of it.
    if (r.values.size() >= 2 && r.values.at(1).type() == JsonValue::Object)
        qWarning("TcfTrkDevice::parseMemoryGet(): Error retrieving memory: %s", r.values.at(1).toString(false).constData());
    // decode
    const QByteArray memory = QByteArray::fromBase64(memoryV.data());
    if (memory.isEmpty())
        qWarning("Base64 decoding of %s failed.", memoryV.data().constData());
    if (debug)
        qDebug("TcfTrkDevice::parseMemoryGet: received %d bytes", memory.size());
    return memory;
}

// Parse register children (array of names)
QVector<QByteArray> TcfTrkDevice::parseRegisterGetChildren(const TcfTrkCommandResult &r)
{
    QVector<QByteArray> rc;
    if (!r || r.values.size() < 1 || r.values.front().type() != JsonValue::Array)
        return rc;
    const JsonValue &front = r.values.front();
    rc.reserve(front.childCount());
    foreach(const JsonValue &v, front.children())
        rc.push_back(v.data());
    return rc;
}

void TcfTrkDevice::sendRegistersGetChildrenCommand(const TcfTrkCallback &callBack,
                                     const QByteArray &contextId,
                                     const QVariant &cookie)
{
    QByteArray data;
    JsonInputStream str(data);
    str << contextId;
    sendTcfTrkMessage(MessageWithReply, RegistersService, "getChildren", data, callBack, cookie);
}

// Format id of register get request (needs contextId containing process and thread)
static inline QByteArray registerId(const QByteArray &contextId, QByteArray id)
{
    QByteArray completeId = contextId;
    if (!completeId.isEmpty())
        completeId.append('.');
    completeId.append(id);
    return completeId;
}

// Format parameters of register get request
static inline QByteArray registerGetData(const QByteArray &contextId, QByteArray id)
{
    QByteArray data;
    JsonInputStream str(data);
    str << registerId(contextId, id);
    return data;
}

void TcfTrkDevice::sendRegistersGetCommand(const TcfTrkCallback &callBack,
                                            const QByteArray &contextId,
                                            QByteArray id,
                                            const QVariant &cookie)
{
    sendTcfTrkMessage(MessageWithReply, RegistersService, "get",
                      registerGetData(contextId, id), callBack, cookie);
}

void TcfTrkDevice::sendRegistersGetMCommand(const TcfTrkCallback &callBack,
                                            const QByteArray &contextId,
                                            const QVector<QByteArray> &ids,
                                            const QVariant &cookie)
{
    // TODO: use "Registers" (which uses base64-encoded values)
#if 0 // Once 'getm' is supported:
    // Manually format the complete register ids as an array
    QByteArray data;
    JsonInputStream str(data);
    str << '[';
    const int count = ids.size();
    for (int r = 0; r < count; r++) {
        if (r)
            str << ',';
        str << registerId(contextId, ids.at(r));
    }
    str << ']';
    sendTcfTrkMessage(MessageWithReply, RegistersService, "getm", data, callBack, cookie);
#else
    // TCF TRK 4.0.5: Fake 'getm' by sending all requests, pass on callback to the last
    // @Todo: Hopefully, we get 'getm'?
    const int last = ids.size() - 1;
    d->m_fakeGetMRegisterValues = QVector<QByteArray>(ids.size(), QByteArray());
    for (int r = 0; r <= last; r++) {
        const QByteArray data = registerGetData(contextId, ids.at(r));
        // Determine special flags along with index
        unsigned specialFlags = r == last ? FakeRegisterGetMFinal : FakeRegisterGetMIntermediate;
        const int index = d->m_registerNames.indexOf(ids.at(r));
        if (index == -1) { // Should not happen
            qWarning("Invalid register name %s", ids.at(r).constData());
            return;
        }
        specialFlags |= unsigned(index) << 16;
        sendTcfTrkMessage(MessageWithReply, RegistersService, "get",
                          data.constData(), data.size(),
                          r == last ? callBack : TcfTrkCallback(),
                          cookie, specialFlags);
    }
#endif
}

void TcfTrkDevice::sendRegistersGetMRangeCommand(const TcfTrkCallback &callBack,
                                                 const QByteArray &contextId,
                                                 unsigned start, unsigned count)
{
    const unsigned end = start + count;
    if (end > (unsigned)d->m_registerNames.size()) {
        qWarning("TcfTrkDevice: No register name set for index %u (size: %d).", end, d->m_registerNames.size());
        return;
    }

    QVector<QByteArray> ids;
    ids.reserve(count);
    for (unsigned i = start; i < end; i++)
        ids.push_back(d->m_registerNames.at(i));
    sendRegistersGetMCommand(callBack, contextId, ids, QVariant(start));
}

// Set register
void TcfTrkDevice::sendRegistersSetCommand(const TcfTrkCallback &callBack,
                                           const QByteArray &contextId,
                                           QByteArray id,
                                           const QByteArray &value,
                                           const QVariant &cookie)
{
    QByteArray data;
    JsonInputStream str(data);
    if (!contextId.isEmpty()) {
        id.prepend('.');
        id.prepend(contextId);
    }
    str << id << '\0' << value.toBase64();
    sendTcfTrkMessage(MessageWithReply, RegistersService, "set", data, callBack, cookie);
}

// Set register
void TcfTrkDevice::sendRegistersSetCommand(const TcfTrkCallback &callBack,
                                           const QByteArray &contextId,
                                           unsigned registerNumber,
                                           const QByteArray &value,
                                           const QVariant &cookie)
{
    if (registerNumber >= (unsigned)d->m_registerNames.size()) {
        qWarning("TcfTrkDevice: No register name set for index %u (size: %d).", registerNumber, d->m_registerNames.size());
        return;
    }
    sendRegistersSetCommand(callBack, contextId,
                            d->m_registerNames[registerNumber],
                            value, cookie);
}

static const char outputListenerIDC[] = "org.eclipse.cdt.debug.edc.ui.ProgramOutputConsoleLogger";

void TcfTrkDevice::sendLoggingAddListenerCommand(const TcfTrkCallback &callBack,
                                                 const QVariant &cookie)
{
    QByteArray data;
    JsonInputStream str(data);
    str << outputListenerIDC;
    sendTcfTrkMessage(MessageWithReply, LoggingService, "addListener", data, callBack, cookie);
}

} // namespace tcftrk
