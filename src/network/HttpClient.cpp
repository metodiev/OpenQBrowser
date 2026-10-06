#include "network/HttpClient.h"

#include <QTimer>
#include <QTcpSocket>
#include <QSslSocket>

#include <algorithm>

namespace oqb::network {
namespace {

/// Splits the header block into a status line and header entries.
bool parseHead(const QByteArray &head, HttpResponse *response)
{
    const QList<QByteArray> lines = head.split('\n');
    if (lines.isEmpty())
        return false;

    const QByteArray statusLine = lines.first().trimmed();
    const QList<QByteArray> statusParts = statusLine.split(' ');
    if (statusParts.size() < 2)
        return false;

    const QByteArray version = statusParts.at(0);
    if (!version.startsWith("HTTP/"))
        return false;

    bool ok = false;
    const int code = statusParts.at(1).toInt(&ok);
    if (!ok)
        return false;

    response->httpVersion = QString::fromLatin1(version);
    response->statusCode = code;
    if (statusParts.size() > 2) {
        QByteArray reason = statusLine.mid(statusParts.at(0).size() + statusParts.at(1).size() + 2);
        response->reasonPhrase = QString::fromUtf8(reason).trimmed();
    }

    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i);
        if (line.trimmed().isEmpty())
            continue;
        const int colon = line.indexOf(':');
        if (colon <= 0)
            continue; // Obsolete line folding and junk are ignored.
        const QString name = QString::fromLatin1(line.left(colon)).trimmed();
        QString value = QString::fromUtf8(line.mid(colon + 1)).trimmed();
        // Unfold continuation lines (RFC 7230 section 3.2.4).
        int j = i + 1;
        while (j < lines.size()
               && (lines.at(j).startsWith(' ') || lines.at(j).startsWith('\t'))) {
            value += u' ' + QString::fromUtf8(lines.at(j)).trimmed();
            ++j;
        }
        response->headers.append(name, value);
    }
    return true;
}

bool isHttpLikeScheme(const Url &url)
{
    return url.isHttp() || url.isHttps();
}

} // namespace

HttpClient::HttpClient(QObject *parent)
    : QObject(parent)
    , m_timeoutTimer(new QTimer(this))
{
    m_timeoutTimer->setSingleShot(true);
    connect(m_timeoutTimer, &QTimer::timeout, this, &HttpClient::onTimeout);
}

HttpClient::~HttpClient() = default;

qint64 HttpClient::elapsedMs() const
{
    return m_clock.isValid() ? m_clock.elapsed() : 0;
}

void HttpClient::send(const HttpRequest &request)
{
    if (isRunning())
        abort();

    http::installSystemCaCertificates();

    m_original = request;
    m_redirectsForOriginal = 0;
    m_clock.start();
    beginRequest(request);
}

void HttpClient::abort()
{
    m_timeoutTimer->stop();
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    m_state = State::Idle;
    m_buffer.clear();
}

void HttpClient::beginRequest(const HttpRequest &request)
{
    if (!request.url.isValid() || !isHttpLikeScheme(request.url)) {
        fail(tr("Unsupported URL scheme: %1").arg(request.url.scheme()));
        return;
    }

    m_pending = request;
    m_buffer.clear();
    m_headerSize = -1;
    m_expectedBody = -1;
    m_chunked = false;
    m_readUntilClose = false;
    m_response = HttpResponse();
    m_response.finalUrl = request.url;
    m_lastProgressEmitted = 0;

    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->deleteLater();
        m_socket = nullptr;
    }

    const bool secure = request.url.isHttps();
    auto *socket = secure ? static_cast<QAbstractSocket *>(new QSslSocket(this))
                          : static_cast<QAbstractSocket *>(new QTcpSocket(this));
    m_socket = socket;

    connect(socket, &QAbstractSocket::connected, this, &HttpClient::onConnected);
    connect(socket, &QAbstractSocket::readyRead, this, &HttpClient::onReadyRead);
    connect(socket, &QAbstractSocket::disconnected, this, &HttpClient::onDisconnected);
    connect(socket, &QAbstractSocket::errorOccurred, this, &HttpClient::onSocketError);

    if (auto *ssl = qobject_cast<QSslSocket *>(socket)) {
        connect(ssl, &QSslSocket::encrypted, this, &HttpClient::onEncrypted);
        connect(ssl, &QSslSocket::sslErrors, this, &HttpClient::onSslErrors);
    }

    m_state = State::Connecting;
    if (request.timeoutMs > 0)
        m_timeoutTimer->start(request.timeoutMs);

    // A QSslSocket must be told to connect with TLS, otherwise the bytes that
    // follow would be plaintext HTTP aimed at a TLS port. connectToHostEncrypted
    // also carries the host name used for certificate verification and SNI.
    if (auto *ssl = qobject_cast<QSslSocket *>(socket))
        ssl->connectToHostEncrypted(request.url.host(),
                                    static_cast<quint16>(request.url.effectivePort()));
    else
        socket->connectToHost(request.url.host(),
                              static_cast<quint16>(request.url.effectivePort()));
}

void HttpClient::onConnected()
{
    emit connected();
    // For https the request is written from onEncrypted() so that no plaintext
    // bytes ever reach the wire.
    if (auto *ssl = qobject_cast<QSslSocket *>(m_socket); ssl && !ssl->isEncrypted())
        return;
    sendRequestBytes();
}

void HttpClient::onEncrypted()
{
    sendRequestBytes();
}

void HttpClient::sendRequestBytes()
{
    if (m_state != State::Connecting && m_state != State::Redirecting)
        return;
    m_state = State::Sending;

    HttpRequest outgoing = m_pending;
    if (outgoing.userAgent.isEmpty())
        outgoing.userAgent = http::defaultUserAgent();
    m_socket->write(outgoing.serialize());
}

void HttpClient::onSslErrors(const QList<QSslError> &errors)
{
    // OpenQBrowser never ignores certificate problems: a page that cannot be
    // verified produces an error page instead of being rendered.
    QStringList messages;
    for (const QSslError &error : errors)
        messages.append(error.errorString());
    fail(tr("TLS certificate verification failed: %1").arg(messages.join(QStringLiteral("; "))));
}

void HttpClient::onSocketError()
{
    if (!m_socket || m_state == State::Idle)
        return;

    // A remote close surfaces as RemoteHostClosedError. For a body framed by
    // connection close that is the normal end of the response, so the body is
    // finalised rather than reported as a failure.
    if (m_readUntilClose && m_socket->error() == QAbstractSocket::RemoteHostClosedError) {
        onDisconnected();
        return;
    }

    fail(m_socket->errorString());
}

void HttpClient::onReadyRead()
{
    if (!m_socket)
        return;

    const QByteArray chunk = m_socket->readAll();
    if (!chunk.isEmpty()) {
        if (m_buffer.size() + chunk.size() > kMaxBodyBytes) {
            fail(tr("Response exceeded the %1 MiB limit").arg(kMaxBodyBytes / (1024 * 1024)));
            return;
        }
        m_buffer += chunk;
    }

    if (m_state == State::Sending)
        m_state = State::Reading;

    switch (parseAvailable()) {
    case ParseResult::Complete:
        m_timeoutTimer->stop();
        m_socket->disconnect(this);
        m_socket->abort();
        m_state = State::Idle;
        emit finished(m_response);
        break;
    case ParseResult::Handled:
    case ParseResult::Error:
        // handleRedirect()/fail() already dealt with the socket and state.
        break;
    case ParseResult::Incomplete:
        break;
    }
}

void HttpClient::onDisconnected()
{
    // Disconnected and errorOccurred both fire on a closed connection; only the
    // first one may complete the request, so the state check guards the rest.
    if (m_state == State::Idle)
        return;

    if (m_state != State::Reading || !m_readUntilClose) {
        // A close before the response was framed is a failure.
        fail(tr("The connection was closed before a complete response was received"));
        return;
    }

    // The body is complete: mark the state first so a second signal from the
    // same close cannot report the request twice.
    m_state = State::Idle;
    m_timeoutTimer->stop();

    const QByteArray raw = m_buffer.size() > m_headerSize && m_headerSize >= 0
        ? m_buffer.mid(m_headerSize)
        : QByteArray();
    m_response.body = raw;

    const QString encoding = m_response.headers.joined(QStringLiteral("Content-Encoding"));
    if (!encoding.isEmpty()) {
        const auto decoded = http::decodeContentEncoding(encoding, m_response.body);
        if (decoded.first)
            m_response.body = decoded.second;
    }

    m_socket->disconnect(this);
    emit finished(m_response);
}

void HttpClient::onTimeout()
{
    fail(tr("The request timed out after %1 seconds").arg(m_pending.timeoutMs / 1000));
}

int HttpClient::headerEnd() const
{
    const int position = m_buffer.indexOf("\r\n\r\n");
    return position < 0 ? -1 : position + 4;
}

HttpClient::ParseResult HttpClient::parseAvailable()
{
    if (m_headerSize < 0) {
        const int end = headerEnd();
        if (end < 0) {
            // Guard against a server that never sends a header terminator.
            if (m_buffer.size() > 512 * 1024) {
                fail(tr("Response headers exceeded 512 KiB without terminating"));
                return ParseResult::Error;
            }
            return ParseResult::Incomplete;
        }

        const QByteArray head = m_buffer.left(end - 4);
        if (!parseHead(head, &m_response)) {
            fail(tr("Malformed HTTP response status line"));
            return ParseResult::Error;
        }

        m_headerSize = end;

        const QString transferEncoding
            = m_response.headers.joined(QStringLiteral("Transfer-Encoding")).toLower();
        const QString contentLength = m_response.header(QStringLiteral("Content-Length"));

        if (m_pending.method == HttpRequest::Method::Head || m_response.statusCode == 204
            || m_response.statusCode == 304
            || (m_response.statusCode >= 100 && m_response.statusCode < 200)) {
            m_expectedBody = 0;
        } else if (transferEncoding.contains(QLatin1String("chunked"))) {
            m_chunked = true;
            m_expectedBody = -1;
        } else if (!contentLength.isEmpty()) {
            bool ok = false;
            m_expectedBody = contentLength.trimmed().toLongLong(&ok);
            if (!ok || m_expectedBody < 0) {
                fail(tr("Invalid Content-Length header"));
                return ParseResult::Error;
            }
        } else {
            // Neither framing header: the body ends when the server closes the
            // connection, which the `Connection: close` we send guarantees.
            m_readUntilClose = true;
            m_expectedBody = -1;
        }

        if (m_expectedBody >= 0 && m_expectedBody > kMaxBodyBytes) {
            fail(tr("Response body of %1 bytes exceeds the size limit").arg(m_expectedBody));
            return ParseResult::Error;
        }
    }

    const qint64 received = m_buffer.size() - m_headerSize;

    QByteArray raw;
    if (m_expectedBody == 0) {
        raw.clear();
    } else if (m_expectedBody > 0) {
        emit progress(std::min(received, m_expectedBody), m_expectedBody);
        if (received < m_expectedBody)
            return ParseResult::Incomplete;
        raw = m_buffer.mid(m_headerSize, static_cast<int>(m_expectedBody));
    } else if (m_chunked) {
        const auto decoded = http::decodeChunked(m_buffer.mid(m_headerSize));
        if (!decoded.first)
            return ParseResult::Incomplete;
        raw = decoded.second;
        emit progress(raw.size(), raw.size());
    } else if (m_readUntilClose) {
        // Only a disconnect completes the response; reaching here means the
        // socket closed mid-body, so whatever arrived is what we have.
        raw = m_buffer.mid(m_headerSize);
        emit progress(received, -1);
    }

    m_response.body = raw;

    const QString encoding = m_response.headers.joined(QStringLiteral("Content-Encoding"));
    if (!encoding.isEmpty()) {
        const auto decoded = http::decodeContentEncoding(encoding, m_response.body);
        if (!decoded.first) {
            fail(tr("Could not decode a %1 encoded response").arg(encoding));
            return ParseResult::Error;
        }
        m_response.body = decoded.second;
    }

    if (m_response.isRedirect() && !m_response.header(QStringLiteral("Location")).isEmpty()) {
        m_timeoutTimer->stop();
        const HttpResponse completed = m_response;
        handleRedirect(completed);
        return ParseResult::Handled;
    }

    if (m_readUntilClose) {
        // A body framed by connection close is only complete once the peer has
        // closed: onDisconnected() and onSocketError() finalise it. Reaching
        // here means more bytes may still arrive.
        return ParseResult::Incomplete;
    }

    return ParseResult::Complete;
}

void HttpClient::fail(const QString &message)
{
    m_timeoutTimer->stop();
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    m_state = State::Idle;
    emit failed(message);
}

void HttpClient::handleRedirect(const HttpResponse &response)
{
    // The redirect response is reported first, before anything can fail: it was
    // received, so its Set-Cookie headers are real regardless of what happens to
    // the hop that follows.
    emit redirectResponse(response);

    const Url target = response.location();
    if (!target.isValid() || !isHttpLikeScheme(target)) {
        fail(tr("Redirect to an unsupported location: %1").arg(response.header(QStringLiteral("Location"))));
        return;
    }

    ++m_redirectCount;
    ++m_redirectsForOriginal;

    if (m_redirectsForOriginal > m_pending.maxRedirects) {
        fail(tr("Too many redirects (%1)").arg(m_pending.maxRedirects));
        return;
    }

    emit redirected(m_response.finalUrl, target);

    HttpRequest next = m_pending;
    next.url = target;
    next.userAgent = m_original.userAgent;

    // RFC 7231 section 6.4: 303 always becomes GET, and 301/302 have
    // historically been treated as GET by browsers, which the web now relies on.
    if (response.statusCode == 303
        || ((response.statusCode == 301 || response.statusCode == 302)
            && next.method == HttpRequest::Method::Post)) {
        next.method = HttpRequest::Method::Get;
        next.body.clear();
        next.headers.remove(QStringLiteral("Content-Type"));
        next.headers.remove(QStringLiteral("Content-Length"));
    }

    // Credentials must never leak to another origin.
    if (target.host() != m_pending.url.host()) {
        next.headers.remove(QStringLiteral("Authorization"));
        next.headers.remove(QStringLiteral("Cookie"));
    }

    // The Cookie header is rebuilt from scratch for the hop that is about to be
    // sent. The redirect may have set a cookie, or moved to a host the jar holds
    // nothing for, so the header that suited the previous hop is not the header
    // that suits this one. Browsers re-ask the jar here for the same reason.
    next.headers.remove(QStringLiteral("Cookie"));
    if (m_cookieProvider) {
        const QString header = m_cookieProvider(target);
        if (!header.isEmpty())
            next.headers.set(QStringLiteral("Cookie"), header);
    }

    // Any bytes already buffered belong to the redirect response, not to the
    // request that follows it.
    m_buffer.clear();
    m_headerSize = -1;
    m_expectedBody = -1;
    m_chunked = false;
    m_readUntilClose = false;

    m_state = State::Redirecting;
    beginRequest(next);
}

} // namespace oqb::network
