// Copyright (C) 2018 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

#ifndef QNETWORKREPLYWASMIMPL_H
#define QNETWORKREPLYWASMIMPL_H

//
//  W A R N I N G
//  -------------
//
// This file is not part of the Qt API.  It exists for the convenience
// of the Network Access API.  This header file may change from
// version to version without notice, or even be removed.
//
// We mean it.
//

#include "qnetworkreply.h"
#include "qnetworkreply_p.h"
#include "qnetworkaccessmanager.h"

#include <QtCore/qfile.h>

#include <private/qtnetworkglobal_p.h>
#include <private/qabstractfileengine_p.h>

#include <emscripten.h>
#include <emscripten/val.h>

#include <memory>
#include <mutex>

QT_BEGIN_NAMESPACE

class QIODevice;

class QNetworkReplyWasmImplPrivate;
class QNetworkReplyWasmImpl: public QNetworkReply
{
    Q_OBJECT
public:
    QNetworkReplyWasmImpl(QObject *parent = nullptr);
    ~QNetworkReplyWasmImpl();
    virtual void abort() override;

    // reimplemented from QNetworkReply
    virtual void close() override;
    virtual qint64 bytesAvailable() const override;
    virtual bool isSequential () const override;
    qint64 size() const override;

    virtual qint64 readData(char *data, qint64 maxlen) override;

    void setup(QNetworkAccessManager::Operation op, const QNetworkRequest &request,
               QIODevice *outgoingData);

    Q_DECLARE_PRIVATE(QNetworkReplyWasmImpl)

    Q_PRIVATE_SLOT(d_func(), void emitReplyError(QNetworkReply::NetworkError errorCode, const QString &errorString))
    Q_PRIVATE_SLOT(d_func(), void emitDataReadProgress(qint64 done, qint64 total))
    Q_PRIVATE_SLOT(d_func(), void dataReceived(const QByteArray &buffer))

private:
    QByteArray methodName() const;
};

class QNetworkReplyWasmImplPrivate;

/*!
    The FetchContext class ensures the request state remains valid while a
    fetch is pending. The browser fetch runs on the main thread and reports
    through promises adopted by QWasmSuspendResumeControl, so every callback
    runs as a Qt event (safe under JSPI, where a raw JS-to-wasm call while the
    main thread is suspended would run on its stack). Exactly one promise is in
    flight per fetch; its callback frees the context once the reply is gone.
    A mutex guards the state because the reply may be destroyed on another
    thread.
*/
struct FetchContext
{
    enum class State { SCHEDULED, SENT, FINISHED, CANCELED, TO_BE_DESTROYED };
    FetchContext(QNetworkReplyWasmImplPrivate *networkReply) : reply(networkReply) { }
    QNetworkReplyWasmImplPrivate *reply{ nullptr };
    std::mutex mutex;
    QByteArray requestData;
    State state{ State::SCHEDULED };

    // Main thread only.
    emscripten::val abortController = emscripten::val::undefined();
    emscripten::val reader = emscripten::val::undefined();
    int status = 0;
    qint64 received = 0;
    qint64 total = 0;
};

class QNetworkReplyWasmImplPrivate: public QNetworkReplyPrivate
{
public:
    QNetworkReplyWasmImplPrivate();
    ~QNetworkReplyWasmImplPrivate();

    QNetworkAccessManagerPrivate *managerPrivate;
    void doSendRequest();
    static void setReplyAttributes(quintptr data, int statusCode, const QString &statusReason);

    void emitReplyError(QNetworkReply::NetworkError errorCode, const QString &);
    void emitDataReadProgress(qint64 done, qint64 total);
    void dataReceived(const QByteArray &buffer);
    void headersReceived(const QByteArray &buffer);

    void setStatusCode(int status, const QByteArray &statusText);

    void setup(QNetworkAccessManager::Operation op, const QNetworkRequest &request,
               QIODevice *outgoingData);

    State state;
    void _q_bufferOutgoingData();
    void _q_bufferOutgoingDataFinished();

    std::shared_ptr<QAtomicInt> pendingDownloadData;
    std::shared_ptr<QAtomicInt> pendingDownloadProgress;

    qint64 bytesDownloaded;
    qint64 bytesBuffered;

    qint64 downloadBufferReadPosition;
    qint64 downloadBufferCurrentSize;
    qint64 totalDownloadSize;
    qint64 percentFinished;
    QByteArray downloadBuffer;

    QIODevice *outgoingData;
    std::shared_ptr<QRingBuffer> outgoingDataBuffer;

    // Fetch steps, on the main thread, each from an adopted promise.
    static void responseReceived(FetchContext *context, emscripten::val response);
    static void readNextChunk(FetchContext *context);
    static void chunkReceived(FetchContext *context, emscripten::val result);
    static void fetchFailed(FetchContext *context, emscripten::val error);
    // Locks the context; returns the live reply, or null after handling a
    // canceled / destroyed reply (the context may be freed then).
    static QNetworkReplyWasmImplPrivate *liveReply(FetchContext *context, bool finalStep);

    static QNetworkReply::NetworkError statusCodeFromHttp(int httpStatusCode, const QUrl &url);

    FetchContext *m_fetchContext;
    void setReplyFinished();
    void setCanceled();

    Q_DECLARE_PUBLIC(QNetworkReplyWasmImpl)
};

QT_END_NAMESPACE

#endif // QNETWORKREPLYWASMIMPL_H
