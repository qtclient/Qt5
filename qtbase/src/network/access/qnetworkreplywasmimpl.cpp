// Copyright (C) 2018 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:critical reason:data-parser

#include "qnetworkreplywasmimpl_p.h"
#include "qnetworkrequest.h"

#include <QtCore/qdatetime.h>
#include <QtCore/qcoreapplication.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qthread.h>
#include <QtCore/private/qstdweb_p.h>
#include <QtCore/private/qwasmglobal_p.h>
#include <QtCore/private/qoffsetstringarray_p.h>
#include <QtCore/private/qtools_p.h>

#include <private/qnetworkaccessmanager_p.h>
#include <private/qnetworkfile_p.h>

#include <emscripten.h>
#include <emscripten/val.h>

QT_BEGIN_NAMESPACE

using namespace Qt::StringLiterals;

namespace {

static constexpr auto BannedHeaders = qOffsetStringArray(
    "accept-charset",
    "accept-encoding",
    "access-control-request-headers",
    "access-control-request-method",
    "connection",
    "content-length",
    "cookie",
    "cookie2",
    "date",
    "dnt",
    "expect",
    "host",
    "keep-alive",
    "origin",
    "referer",
    "te",
    "trailer",
    "transfer-encoding",
    "upgrade",
    "via"
);

bool isUnsafeHeader(QLatin1StringView header) noexcept
{
    return header.startsWith("proxy-"_L1, Qt::CaseInsensitive)
        || header.startsWith("sec-"_L1, Qt::CaseInsensitive)
        || BannedHeaders.contains(header, Qt::CaseInsensitive);
}
} // namespace

QNetworkReplyWasmImplPrivate::QNetworkReplyWasmImplPrivate()
    : QNetworkReplyPrivate()
    , managerPrivate(0)
    , bytesDownloaded(0)
    , downloadBufferReadPosition(0)
    , downloadBufferCurrentSize(0)
    , totalDownloadSize(0)
    , percentFinished(0)
    , m_fetchContext(nullptr)
{
}

QNetworkReplyWasmImplPrivate::~QNetworkReplyWasmImplPrivate()
{

    if (m_fetchContext) { // fetch has been initiated
        std::unique_lock lock{ m_fetchContext->mutex };

        if (m_fetchContext->state == FetchContext::State::SCHEDULED
            || m_fetchContext->state == FetchContext::State::SENT
            || m_fetchContext->state == FetchContext::State::CANCELED) {
            // The in-flight promise's callback frees the context; aborting
            // makes it settle now instead of when the transfer ends.
            const bool sent = m_fetchContext->state == FetchContext::State::SENT;
            m_fetchContext->reply = nullptr;
            m_fetchContext->state = FetchContext::State::TO_BE_DESTROYED;
            if (sent) {
                FetchContext *context = m_fetchContext;
                lock.unlock();
                qwasmglobal::runOnMainThreadAsync([context]() {
                    context->abortController.call<void>("abort");
                });
            }
        } else if (m_fetchContext->state == FetchContext::State::FINISHED) {
            lock.unlock();
            delete m_fetchContext;
            m_fetchContext = nullptr;
        }
    }

}

QNetworkReplyWasmImpl::QNetworkReplyWasmImpl(QObject *parent)
    : QNetworkReply(*new QNetworkReplyWasmImplPrivate(), parent)
{
    Q_D( QNetworkReplyWasmImpl);
    d->state = QNetworkReplyPrivate::Idle;
}

QNetworkReplyWasmImpl::~QNetworkReplyWasmImpl()
{
    if (isRunning())
        abort();
    close();
}

QByteArray QNetworkReplyWasmImpl::methodName() const
{
    const Q_D( QNetworkReplyWasmImpl);
    switch (operation()) {
    case QNetworkAccessManager::HeadOperation:
        return "HEAD";
    case QNetworkAccessManager::GetOperation:
        return "GET";
    case QNetworkAccessManager::PutOperation:
        return "PUT";
    case QNetworkAccessManager::PostOperation:
        return "POST";
    case QNetworkAccessManager::DeleteOperation:
        return "DELETE";
    case QNetworkAccessManager::CustomOperation:
        return d->request.attribute(QNetworkRequest::CustomVerbAttribute).toByteArray();
    default:
        break;
    }
    return QByteArray();
}

void QNetworkReplyWasmImpl::close()
{
    Q_D(QNetworkReplyWasmImpl);

    if (d->state != QNetworkReplyPrivate::Aborted &&
        d->state != QNetworkReplyPrivate::Finished &&
        d->state != QNetworkReplyPrivate::Idle) {
            d->state = QNetworkReplyPrivate::Finished;
            d->setCanceled();
    }
    QNetworkReply::close();
}

void QNetworkReplyWasmImpl::abort()
{
    Q_D(QNetworkReplyWasmImpl);

    if (d->state == QNetworkReplyPrivate::Finished || d->state == QNetworkReplyPrivate::Aborted)
        return;

    d->state = QNetworkReplyPrivate::Aborted;
    d->setCanceled();
}

void QNetworkReplyWasmImplPrivate::setCanceled()
{
    Q_Q(QNetworkReplyWasmImpl);
    {
        if (m_fetchContext) {
            std::unique_lock lock{ m_fetchContext->mutex };
            const bool sent = m_fetchContext->state == FetchContext::State::SENT;
            if (m_fetchContext->state == FetchContext::State::SCHEDULED
                || m_fetchContext->state == FetchContext::State::SENT)
                m_fetchContext->state = FetchContext::State::CANCELED;
            if (sent) {
                FetchContext *context = m_fetchContext;
                lock.unlock();
                qwasmglobal::runOnMainThreadAsync([context]() {
                    context->abortController.call<void>("abort");
                });
            }
        }
    }

    emitReplyError(QNetworkReply::OperationCanceledError, QStringLiteral("Operation canceled"));
    q->setFinished(true);
    emit q->finished();
}

qint64 QNetworkReplyWasmImpl::bytesAvailable() const
{
    Q_D(const QNetworkReplyWasmImpl);

    return QNetworkReply::bytesAvailable() + d->downloadBufferCurrentSize - d->downloadBufferReadPosition;
}

bool QNetworkReplyWasmImpl::isSequential() const
{
    return true;
}

qint64 QNetworkReplyWasmImpl::size() const
{
    return QNetworkReply::size();
}

/*!
    \internal
*/
qint64 QNetworkReplyWasmImpl::readData(char *data, qint64 maxlen)
{
    Q_D(QNetworkReplyWasmImpl);

    qint64 howMuch = qMin(maxlen, (d->downloadBuffer.size() - d->downloadBufferReadPosition));
    memcpy(data, d->downloadBuffer.constData() + d->downloadBufferReadPosition, howMuch);
    d->downloadBufferReadPosition += howMuch;
    // Everything read: drop the buffered bytes (a long body streams through).
    if (d->downloadBufferReadPosition == d->downloadBuffer.size()) {
        d->downloadBuffer.clear();
        d->downloadBufferReadPosition = 0;
        d->downloadBufferCurrentSize = 0;
    }

    return howMuch;
}

void QNetworkReplyWasmImplPrivate::setup(QNetworkAccessManager::Operation op, const QNetworkRequest &req, QIODevice *data)
{
    Q_Q(QNetworkReplyWasmImpl);

    outgoingData = data;
    request = req;
    url = request.url();
    operation = op;

    q->QIODevice::open(QIODevice::ReadOnly);
    if (outgoingData && outgoingData->isSequential()) {
        bool bufferingDisallowed =
            request.attribute(QNetworkRequest::DoNotBufferUploadDataAttribute, false).toBool();

        if (bufferingDisallowed) {
            // if a valid content-length header for the request was supplied, we can disable buffering
            // if not, we will buffer anyway
            if (!request.header(QNetworkRequest::ContentLengthHeader).isValid()) {
                state = Buffering;
                _q_bufferOutgoingData();
                return;
            }
        } else {
            // doSendRequest will be called when the buffering has finished.
            state = Buffering;
            _q_bufferOutgoingData();
            return;
        }
    }
    // No outgoing data (POST, ..)
    doSendRequest();
}

void QNetworkReplyWasmImplPrivate::setReplyAttributes(quintptr data, int statusCode, const QString &statusReason)
{
    QNetworkReplyWasmImplPrivate *handler = reinterpret_cast<QNetworkReplyWasmImplPrivate*>(data);
    Q_ASSERT(handler);

    handler->q_func()->setAttribute(QNetworkRequest::HttpStatusCodeAttribute, statusCode);
    if (!statusReason.isEmpty())
        handler->q_func()->setAttribute(QNetworkRequest::HttpReasonPhraseAttribute, statusReason);
}

void QNetworkReplyWasmImplPrivate::doSendRequest()
{
    Q_Q(QNetworkReplyWasmImpl);
    totalDownloadSize = 0;

    m_fetchContext = new FetchContext(this);
    if (outgoingDataBuffer) { // sequential upload data, buffered by _q_bufferOutgoingData
        m_fetchContext->requestData.resize(outgoingDataBuffer->size());
        outgoingDataBuffer->read(m_fetchContext->requestData.data(), outgoingDataBuffer->size());
    }
    else if (outgoingData)
        m_fetchContext->requestData = outgoingData->readAll();

    const QByteArray method = q->methodName();
    qwasmglobal::runOnMainThread([method, fetchContext = m_fetchContext]() {
        using emscripten::val;
        std::unique_lock lock{ fetchContext->mutex };
        if (fetchContext->state == FetchContext::State::CANCELED) {
            fetchContext->state = FetchContext::State::FINISHED;
            return;
        } else if (fetchContext->state == FetchContext::State::TO_BE_DESTROYED) {
            lock.unlock();
            delete fetchContext;
            return;
        }
        const auto reply = fetchContext->reply;
        const auto &request = reply->request;

        val headers = val::global("Headers").new_();
        QStringList trimmedHeaders;
        for (const auto &headerName : request.rawHeaderList()) {
            if (isUnsafeHeader(QLatin1StringView(headerName.constData()))) {
                trimmedHeaders.push_back(QString::fromLatin1(headerName));
                continue;
            }
            headers.call<void>("append", val(headerName.toStdString()),
                               val(request.rawHeader(headerName).toStdString()));
        }
        if (!trimmedHeaders.isEmpty()) {
            qWarning() << "Qt has trimmed the following forbidden headers from the request:"
                       << trimmedHeaders.join(QLatin1StringView(", "));
        }
        // fetch() rejects credentials in the URL: send them as Basic auth.
        QUrl url = request.url();
        if (!url.userInfo().isEmpty()) {
            const QByteArray credentials = url.userName().toUtf8() + ':' + url.password().toUtf8();
            headers.call<void>("set", val("Authorization"),
                               val("Basic " + credentials.toBase64().toStdString()));
            url.setUserInfo(QString());
        }

        val init = val::object();
        init.set("method", val(method.toStdString()));
        init.set("headers", headers);
        if (!fetchContext->requestData.isEmpty())
            init.set("body", qstdweb::Uint8Array::copyFrom(fetchContext->requestData).val());
        const bool withCredentials =
                request.attribute(QNetworkRequest::UseCredentialsAttribute, false).toBool();
        init.set("credentials", val(withCredentials ? "include" : "same-origin"));
        switch (request.attribute(QNetworkRequest::CacheLoadControlAttribute,
                                  QNetworkRequest::PreferNetwork).toInt()) {
        case QNetworkRequest::AlwaysNetwork:
            init.set("cache", val("no-store"));
            break;
        case QNetworkRequest::PreferCache:
        case QNetworkRequest::AlwaysCache:
            init.set("cache", val("force-cache"));
            break;
        default:
            break;
        }
        // Unset means the manager's default (it only sets other policies), which
        // follows redirects; 0 is ManualRedirectPolicy, so never toInt() it bare.
        if (request.attribute(QNetworkRequest::RedirectPolicyAttribute,
                              QNetworkRequest::NoLessSafeRedirectPolicy).toInt()
            == QNetworkRequest::ManualRedirectPolicy)
            init.set("redirect", val("manual"));

        fetchContext->abortController = val::global("AbortController").new_();
        val signal = fetchContext->abortController["signal"];
        if (const int timeout = request.transferTimeout(); timeout > 0) {
            val abortSignals = val::array();
            abortSignals.call<void>("push", signal);
            abortSignals.call<void>("push", val::global("AbortSignal").call<val>("timeout", timeout));
            signal = val::global("AbortSignal").call<val>("any", abortSignals);
        }
        init.set("signal", signal);

        val promise = val::global("fetch")(val(url.toString().toStdString()), init);
        fetchContext->state = FetchContext::State::SENT;
        lock.unlock();
        qstdweb::PromiseCallbacks callbacks;
        callbacks.thenFunc = [fetchContext](val response) { responseReceived(fetchContext, response); };
        callbacks.catchFunc = [fetchContext](val error) { fetchFailed(fetchContext, error); };
        qstdweb::Promise::adoptPromise(promise, std::move(callbacks));
    });
    state = Working;
}

QNetworkReplyWasmImplPrivate *QNetworkReplyWasmImplPrivate::liveReply(FetchContext *context,
                                                                       bool finalStep)
{
    std::unique_lock lock{ context->mutex };
    switch (context->state) {
    case FetchContext::State::SENT:
        if (finalStep)
            context->state = FetchContext::State::FINISHED;
        return context->reply;
    case FetchContext::State::TO_BE_DESTROYED:
        lock.unlock();
        delete context;
        return nullptr;
    case FetchContext::State::CANCELED:
        // The reply already reported the cancel; stop here.
        context->state = FetchContext::State::FINISHED;
        return nullptr;
    default:
        return nullptr;
    }
}

void QNetworkReplyWasmImplPrivate::responseReceived(FetchContext *context, emscripten::val response)
{
    using emscripten::val;
    const auto reply = liveReply(context, false);
    if (!reply)
        return;
    context->status = response["status"].as<int>();
    reply->setStatusCode(context->status, QByteArray::fromStdString(response["statusText"].as<std::string>()));

    QByteArray headerText;
    val entries = val::global("Array").call<val>("from", response["headers"].call<val>("entries"));
    const int count = entries["length"].as<int>();
    for (int i = 0; i < count; ++i) {
        val entry = entries[i];
        headerText += QByteArray::fromStdString(entry[0].as<std::string>()) + ": "
                + QByteArray::fromStdString(entry[1].as<std::string>()) + '\n';
    }
    const val contentLength = response["headers"].call<val>("get", val("content-length"));
    context->total = contentLength.isString()
            ? QByteArray::fromStdString(contentLength.as<std::string>()).toLongLong()
            : 0;
    reply->headersReceived(headerText);

    val body = response["body"];
    if (body.isNull() || body.isUndefined()) {
        val done = val::object();
        done.set("done", true);
        chunkReceived(context, done);
        return;
    }
    context->reader = body.call<val>("getReader");
    readNextChunk(context);
}

void QNetworkReplyWasmImplPrivate::readNextChunk(FetchContext *context)
{
    using emscripten::val;
    qstdweb::PromiseCallbacks callbacks;
    callbacks.thenFunc = [context](val result) { chunkReceived(context, result); };
    callbacks.catchFunc = [context](val error) { fetchFailed(context, error); };
    qstdweb::Promise::adoptPromise(context->reader.call<val>("read"), std::move(callbacks));
}

void QNetworkReplyWasmImplPrivate::chunkReceived(FetchContext *context, emscripten::val result)
{
    const bool done = result["done"].as<bool>();
    const auto reply = liveReply(context, done);
    if (!reply)
        return;
    if (!done) {
        const QByteArray chunk = qstdweb::Uint8Array(result["value"]).copyToQByteArray();
        context->received += chunk.size();
        reply->dataReceived(chunk);
        reply->emitDataReadProgress(context->received,
                                    context->total > 0 ? context->total : context->received);
        readNextChunk(context);
        return;
    }
    if (context->status >= 400) {
        reply->emitReplyError(statusCodeFromHttp(context->status, reply->request.url()),
                              reply->q_func()->attribute(QNetworkRequest::HttpReasonPhraseAttribute)
                                      .toString());
    }
    reply->setReplyFinished();
}

void QNetworkReplyWasmImplPrivate::fetchFailed(FetchContext *context, emscripten::val error)
{
    const auto reply = liveReply(context, true);
    if (!reply)
        return;
    const std::string name = error["name"].isString() ? error["name"].as<std::string>() : std::string();
    const QString message = error["message"].isString()
            ? QString::fromStdString(error["message"].as<std::string>())
            : QStringLiteral("Network error");
    if (name == "TimeoutError")
        reply->emitReplyError(QNetworkReply::TimeoutError, QStringLiteral("Operation timed out"));
    else if (name == "AbortError")
        reply->emitReplyError(QNetworkReply::OperationCanceledError, QStringLiteral("Operation canceled"));
    else
        reply->emitReplyError(QNetworkReply::UnknownNetworkError, message);
    reply->setReplyFinished();
}

void QNetworkReplyWasmImplPrivate::emitReplyError(QNetworkReply::NetworkError errorCode, const QString &errorString)
{
    Q_Q(QNetworkReplyWasmImpl);

    q->setError(errorCode, errorString);
    emit q->errorOccurred(errorCode);
}

void QNetworkReplyWasmImplPrivate::emitDataReadProgress(qint64 bytesReceived, qint64 bytesTotal)
{
    Q_Q(QNetworkReplyWasmImpl);

    totalDownloadSize = bytesTotal;
    percentFinished = bytesTotal ? (bytesReceived * 100) / bytesTotal : 100;

    emit q->downloadProgress(bytesReceived, bytesTotal);
}

void QNetworkReplyWasmImplPrivate::dataReceived(const QByteArray &buffer)
{
    Q_Q(QNetworkReplyWasmImpl);

    const qsizetype bufferSize = buffer.size();
    if (bufferSize == 0)
        return;
    bytesDownloaded += bufferSize;
    downloadBufferCurrentSize += bufferSize;
    downloadBuffer.append(buffer);

    emit q->readyRead();
}

//taken from qnetworkrequest.cpp
static int parseHeaderName(const QByteArray &headerName)
{
    if (headerName.isEmpty())
        return -1;

    auto is = [&](const char *what) {
        return qstrnicmp(headerName.data(), headerName.size(), what) == 0;
    };

    switch (QtMiscUtils::toAsciiLower(headerName.front())) {
    case 'c':
        if (is("content-type"))
            return QNetworkRequest::ContentTypeHeader;
        else if (is("content-length"))
            return QNetworkRequest::ContentLengthHeader;
        else if (is("cookie"))
            return QNetworkRequest::CookieHeader;
        break;

    case 'l':
        if (is("location"))
            return QNetworkRequest::LocationHeader;
        else if (is("last-modified"))
            return QNetworkRequest::LastModifiedHeader;
        break;

    case 's':
        if (is("set-cookie"))
            return QNetworkRequest::SetCookieHeader;
        else if (is("server"))
            return QNetworkRequest::ServerHeader;
        break;

    case 'u':
        if (is("user-agent"))
            return QNetworkRequest::UserAgentHeader;
        break;
    }

    return -1; // nothing found
}


void QNetworkReplyWasmImplPrivate::headersReceived(const QByteArray &buffer)
{
    Q_Q(QNetworkReplyWasmImpl);

    if (!buffer.isEmpty()) {
        QList<QByteArray> headers = buffer.split('\n');

        for (auto &&header : headers) {
            if (auto splitPos = header.indexOf(':');
                splitPos != -1) { // headers include final \x00, so skip
                auto headerName = header.first(splitPos).trimmed();
                auto headerValue = header.sliced(splitPos + 1).trimmed();
                headerValue = headerValue.replace('\x00', "").trimmed();

                if (headerName.isEmpty() || headerValue.isEmpty())
                    continue;

                int headerIndex = parseHeaderName(headerName);

                if (headerIndex == -1)
                    q->setRawHeader(headerName, headerValue);
                else
                    q->setHeader(static_cast<QNetworkRequest::KnownHeaders>(headerIndex),
                                 (QVariant)headerValue);
            }
        }
    }
    emit q->metaDataChanged();
}

void QNetworkReplyWasmImplPrivate::_q_bufferOutgoingDataFinished()
{
    Q_Q(QNetworkReplyWasmImpl);

    // make sure this is only called once, ever.
    //_q_bufferOutgoingData may call it or the readChannelFinished emission
    if (state != Buffering)
        return;

    // disconnect signals
    QObject::disconnect(outgoingData, SIGNAL(readyRead()), q, SLOT(_q_bufferOutgoingData()));
    QObject::disconnect(outgoingData, SIGNAL(readChannelFinished()), q, SLOT(_q_bufferOutgoingDataFinished()));

    // finally, start the request
    doSendRequest();
}

void QNetworkReplyWasmImplPrivate::_q_bufferOutgoingData()
{
    Q_Q(QNetworkReplyWasmImpl);

    if (!outgoingDataBuffer) {
        // first call, create our buffer
        outgoingDataBuffer = std::make_shared<QRingBuffer>();

        QObject::connect(outgoingData, SIGNAL(readyRead()), q, SLOT(_q_bufferOutgoingData()));
        QObject::connect(outgoingData, SIGNAL(readChannelFinished()), q, SLOT(_q_bufferOutgoingDataFinished()));
    }

    qint64 bytesBuffered = 0;
    qint64 bytesToBuffer = 0;

    // read data into our buffer
    forever {
        bytesToBuffer = outgoingData->bytesAvailable();
        // unknown? just try 2 kB, this also ensures we always try to read the EOF
        if (bytesToBuffer <= 0)
            bytesToBuffer = 2*1024;

        char *dst = outgoingDataBuffer->reserve(bytesToBuffer);
        bytesBuffered = outgoingData->read(dst, bytesToBuffer);

        if (bytesBuffered == -1) {
            // EOF has been reached.
            outgoingDataBuffer->chop(bytesToBuffer);

            _q_bufferOutgoingDataFinished();
            break;
        } else if (bytesBuffered == 0) {
            // nothing read right now, just wait until we get called again
            outgoingDataBuffer->chop(bytesToBuffer);

            break;
        } else {
            // don't break, try to read() again
            outgoingDataBuffer->chop(bytesToBuffer - bytesBuffered);
        }
    }
}

void QNetworkReplyWasmImplPrivate::setReplyFinished()
{
    Q_Q(QNetworkReplyWasmImpl);
    state = QNetworkReplyPrivate::Finished;
    q->setFinished(true);
    emit q->readChannelFinished();
    emit q->finished();
}

void QNetworkReplyWasmImplPrivate::setStatusCode(int status, const QByteArray &statusText)
{
    Q_Q(QNetworkReplyWasmImpl);
    q->setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
    q->setAttribute(QNetworkRequest::HttpReasonPhraseAttribute, statusText);
}

//taken from qhttpthreaddelegate.cpp
QNetworkReply::NetworkError QNetworkReplyWasmImplPrivate::statusCodeFromHttp(int httpStatusCode, const QUrl &url)
{
    QNetworkReply::NetworkError code;
    // we've got an error
    switch (httpStatusCode) {
    case 400:               // Bad Request
        code = QNetworkReply::ProtocolInvalidOperationError;
        break;

    case 401:               // Authorization required
        code = QNetworkReply::AuthenticationRequiredError;
        break;

    case 403:               // Access denied
        code = QNetworkReply::ContentAccessDenied;
        break;

    case 404:               // Not Found
        code = QNetworkReply::ContentNotFoundError;
        break;

    case 405:               // Method Not Allowed
        code = QNetworkReply::ContentOperationNotPermittedError;
        break;

    case 407:
        code = QNetworkReply::ProxyAuthenticationRequiredError;
        break;

    case 409:               // Resource Conflict
        code = QNetworkReply::ContentConflictError;
        break;

    case 410:               // Content no longer available
        code = QNetworkReply::ContentGoneError;
        break;

    case 418:               // I'm a teapot
        code = QNetworkReply::ProtocolInvalidOperationError;
        break;

    case 500:               // Internal Server Error
        code = QNetworkReply::InternalServerError;
        break;

    case 501:               // Server does not support this functionality
        code = QNetworkReply::OperationNotImplementedError;
        break;

    case 503:               // Service unavailable
        code = QNetworkReply::ServiceUnavailableError;
        break;

    default:
        if (httpStatusCode > 500) {
            // some kind of server error
            code = QNetworkReply::UnknownServerError;
        } else if (httpStatusCode >= 400) {
            // content error we did not handle above
            code = QNetworkReply::UnknownContentError;
        } else {
            qWarning("QNetworkAccess: got HTTP status code %d which is not expected from url: \"%s\"",
                     httpStatusCode, qPrintable(url.toString()));
            code = QNetworkReply::ProtocolFailure;
        }
    };

    return code;
}

QT_END_NAMESPACE

#include "moc_qnetworkreplywasmimpl_p.cpp"
