// Copyright (C) 2018 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:critical reason:network-protocol

#include "qwebsocket_p.h"

#include <QtCore/private/qstdweb_p.h>

#include <emscripten/threading.h>
#include <emscripten/val.h>

#include <QHostAddress>

// The browser WebSocket API, with every event delivered through
// QWasmSuspendResumeControl (QWasmEventHandler), like Qt's own DOM events.
// Emscripten's websocket library calls into wasm straight from the event,
// which under JSPI runs on the stack of a suspended main thread.

using emscripten::val;

namespace {
enum ReadyState { Connecting = 0, Open = 1, Closing = 2, Closed = 3 };

int readyState(const val &socket)
{
    return socket.isUndefined() ? Closed : socket["readyState"].as<int>();
}
} // namespace

qint64 QWebSocketPrivate::sendTextMessage(const QString &message)
{
    if (readyState(m_socket) != Open) {
        qWarning() << "Could not send message. Websocket is not open";
        return 0;
    }
    const QByteArray utf8 = message.toUtf8();
    m_socket.call<void>("send", val(utf8.toStdString()));
    return utf8.size();
}

qint64 QWebSocketPrivate::sendBinaryMessage(const QByteArray &data)
{
    if (readyState(m_socket) != Open) {
        qWarning() << "Could not send message. Websocket is not open";
        return 0;
    }
    m_socket.call<void>("send", qstdweb::Uint8Array::copyFrom(data).val());
    return data.size();
}

void QWebSocketPrivate::close(QWebSocketProtocol::CloseCode closeCode, QString reason)
{
    Q_Q(QWebSocket);
    m_closeCode = closeCode;
    m_closeReason = reason;
    Q_EMIT q->aboutToClose();
    setSocketState(QAbstractSocket::ClosingState);

    const int state = readyState(m_socket);
    if (state == Connecting || state == Open) {
        // Browsers only accept 1000 and 3000-4999 from script.
        const int code = closeCode == QWebSocketProtocol::CloseCodeNormal
                        || (closeCode >= 3000 && closeCode <= 4999)
                ? int(closeCode)
                : int(QWebSocketProtocol::CloseCodeNormal);
        m_socket.call<void>("close", code, val(reason.toStdString()));
    }
    setSocketState(QAbstractSocket::UnconnectedState);
    emit q->disconnected();
}

void QWebSocketPrivate::open(const QNetworkRequest &request,
                             const QWebSocketHandshakeOptions &options, bool mask)
{
    Q_UNUSED(mask);

    const int state = readyState(m_socket);
    if (state == Connecting || state == Open) {
        emitErrorOccurred(QAbstractSocket::OperationError);
        return;
    }
    if (!emscripten_is_main_runtime_thread()) {
        // QWasmSuspendResumeControl delivers events to the main thread only.
        setErrorString(QWebSocket::tr("WebSockets are only supported on the main thread"));
        emitErrorOccurred(QAbstractSocket::UnsupportedSocketOperationError);
        return;
    }

    const QUrl url = request.url();

    val navProtocol = val::global("self")["location"]["protocol"];

    //  An insecure WebSocket connection may not be initiated from a page loaded over HTTPS.
    bool isSecureContext = (navProtocol.as<std::string>().find("https") == 0);

    if (!url.isValid()
            || url.toString().contains(QStringLiteral("\r\n"))) {
        setErrorString(QWebSocket::tr("Connection refused"));

        emitErrorOccurred(QAbstractSocket::ConnectionRefusedError);

        return;
    }
    // exception for localhost/127.0.0.1/[::1]
    // localhost has special privledges

    QHostAddress hostAddress(url.host());

    bool hostAddressIsLocal = (hostAddress == QHostAddress::LocalHost
            || hostAddress == QHostAddress::LocalHostIPv6);

    if (url.host() != QStringLiteral("localhost") && !hostAddressIsLocal) {
        if (isSecureContext && url.scheme() == QStringLiteral("ws")) {
            const QString message =
                    QWebSocket::tr("Unsupported WebSocket scheme: %1").arg(url.scheme());
            setErrorString(message);
            emitErrorOccurred(QAbstractSocket::UnsupportedSocketOperationError);
            return;
        }
    }

    setRequest(request, options);

    // HTML WebSockets do not support arbitrary request headers, but
    // do support the WebSocket protocol header. This header is
    // required for some use cases like MQTT.
    val protocols = val::array();
    for (const QString &protocol : requestedSubProtocols())
        protocols.call<void>("push", val(protocol.toStdString()));

    setSocketState(QAbstractSocket::ConnectingState);
    m_socket = val::global("WebSocket")
                       .new_(val(url.toString(QUrl::FullyEncoded).toStdString()), protocols);
    m_socket.set("binaryType", val("arraybuffer"));

    m_onOpen = std::make_unique<QWasmEventHandler>(m_socket, "open", [this](val) {
        Q_Q(QWebSocket);
        setSocketState(QAbstractSocket::ConnectedState);
        emit q->connected();
    });
    m_onMessage = std::make_unique<QWasmEventHandler>(m_socket, "message", [this](val event) {
        Q_Q(QWebSocket);
        val data = event["data"];
        if (data.isString()) {
            emit q->textMessageReceived(QString::fromStdString(data.as<std::string>()));
        } else {
            const QByteArray buffer =
                    qstdweb::Uint8Array(qstdweb::ArrayBuffer(data)).copyToQByteArray();
            if (!buffer.isEmpty())
                emit q->binaryMessageReceived(buffer);
        }
    });
    m_onError = std::make_unique<QWasmEventHandler>(m_socket, "error", [this](val) {
        Q_Q(QWebSocket);
        emit q->errorOccurred(error());
    });
    m_onClose = std::make_unique<QWasmEventHandler>(m_socket, "close", [this](val event) {
        setSocketClosed(event["code"].as<int>(),
                        QString::fromStdString(event["reason"].as<std::string>()),
                        event["wasClean"].as<bool>());
    });
}

bool QWebSocketPrivate::isValid() const
{
    return !m_socket.isUndefined() && m_socketState == QAbstractSocket::ConnectedState;
}

void QWebSocketPrivate::setSocketClosed(int code, const QString &reason, bool wasClean)
{
    Q_Q(QWebSocket);
    m_closeCode = QWebSocketProtocol::CloseCode(code);
    m_closeReason = reason.isEmpty() ? closeCodeToString(m_closeCode) : reason;

    if (m_socketState == QAbstractSocket::ConnectedState) {
        Q_EMIT q->aboutToClose();
        setSocketState(QAbstractSocket::ClosingState);
    }

    if (!wasClean) {
        m_errorString = QStringLiteral("The remote host closed the connection");
        emit q->errorOccurred(error());
    }

    // close() already reported a disconnect it initiated.
    if (m_socketState != QAbstractSocket::UnconnectedState) {
        setSocketState(QAbstractSocket::UnconnectedState);
        emit q->disconnected();
    }
    // Release the closed socket after this event: a handler may not be
    // destroyed from inside its own callback. A reconnect from the
    // disconnected() slot has replaced m_socket by then; keep that one.
    QMetaObject::invokeMethod(q, [this, closed = m_socket]() {
        if (!m_socket.strictlyEquals(closed))
            return;
        m_onOpen.reset();
        m_onMessage.reset();
        m_onError.reset();
        m_onClose.reset();
        m_socket = val::undefined();
    }, Qt::QueuedConnection);
}

QString QWebSocketPrivate::closeCodeToString(QWebSocketProtocol::CloseCode code)
{
    switch (code) {
        case QWebSocketProtocol::CloseCodeNormal: return QStringLiteral("Normal closure");
        case QWebSocketProtocol::CloseCodeGoingAway: return QStringLiteral("Going away");
        case QWebSocketProtocol::CloseCodeProtocolError: return QStringLiteral("Protocol error");
        case QWebSocketProtocol::CloseCodeDatatypeNotSupported: return QStringLiteral("Unsupported data");
        case QWebSocketProtocol::CloseCodeReserved1004: return QStringLiteral("Reserved");
        case QWebSocketProtocol::CloseCodeMissingStatusCode: return QStringLiteral("No status received");
        case QWebSocketProtocol::CloseCodeAbnormalDisconnection: return QStringLiteral("Abnormal closure");
        case QWebSocketProtocol::CloseCodeWrongDatatype: return QStringLiteral("Invalid frame payload data");
        case QWebSocketProtocol::CloseCodePolicyViolated: return QStringLiteral("Policy violation");
        case QWebSocketProtocol::CloseCodeTooMuchData: return QStringLiteral("Message too big");
        case QWebSocketProtocol::CloseCodeMissingExtension: return QStringLiteral("Mandatory extension missing");
        case QWebSocketProtocol::CloseCodeBadOperation: return QStringLiteral("Internal server error");
        case QWebSocketProtocol::CloseCodeTlsHandshakeFailed: return QStringLiteral("TLS handshake failed");
    };
    return QStringLiteral("");
}
