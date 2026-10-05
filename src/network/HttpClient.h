#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QSslError>
#include <QString>

#include "network/HttpMessage.h"

class QAbstractSocket;
class QTimer;

namespace oqb::network {

/// A minimal, explicit HTTP/1.1 client.
///
/// OpenQBrowser speaks HTTP itself instead of using QNetworkAccessManager so
/// that every step of the protocol is visible in the source and explainable in
/// the DevTools inspector. The client handles:
///
///   * http and https (TLS via QSslSocket with certificate verification)
///   * Content-Length, chunked and read-until-close body framing
///   * gzip and deflate content encodings
///   * redirect chains, with method rules from RFC 7231
///   * timeouts and body size limits
///
/// One client instance performs one request chain and then emits exactly one of
/// finished() or failed(). Instances are cheap; ResourceLoader creates one per
/// resource so that requests can run in parallel.
class HttpClient : public QObject
{
    Q_OBJECT

public:
    /// Hard ceiling on a single response body, guarding against hostile or
    /// misbehaving servers. 64 MiB comfortably exceeds any real web page.
    static constexpr qint64 kMaxBodyBytes = 64 * 1024 * 1024;

    explicit HttpClient(QObject *parent = nullptr);
    ~HttpClient() override;

    /// Starts `request`. Redirects are followed automatically.
    void send(const HttpRequest &request);

    /// Cancels an in-flight request; no finished() or failed() is emitted.
    void abort();

    bool isRunning() const { return m_state != State::Idle; }

    /// The URL of the request currently in flight, for the status bar.
    Url currentUrl() const { return m_pending.url; }

    /// Milliseconds spent on the request chain so far, for the inspector.
    qint64 elapsedMs() const;

signals:
    /// Emitted when a complete response has been read.
    void finished(const oqb::network::HttpResponse &response);
    /// Emitted on transport, protocol or TLS failure.
    void failed(const QString &error);
    /// Emitted after every redirect hop, so the UI can show the real progress.
    void redirected(const oqb::network::Url &from, const oqb::network::Url &to);
    /// Emitted as the body grows, for the loading progress indicator.
    void progress(qint64 receivedBytes, qint64 expectedBytes);
    /// Emitted once the socket is connected, before any bytes are sent.
    void connected();

private slots:
    void onConnected();
    void onEncrypted();
    void onReadyRead();
    void onDisconnected();
    void onSocketError();
    void onSslErrors(const QList<QSslError> &errors);
    void onTimeout();

private:
    enum class State { Idle, Connecting, Sending, Reading, Redirecting };

    /// Outcome of a parsing pass over the receive buffer.
    enum class ParseResult {
        Incomplete, ///< More bytes are needed.
        Complete,   ///< The whole response is in m_response.
        Handled,    ///< A redirect was followed; the response was discarded.
        Error,      ///< The response is unusable and failed() was emitted.
    };

    void beginRequest(const HttpRequest &request);
    void sendRequestBytes();
    void fail(const QString &message);
    void handleRedirect(const HttpResponse &response);

    /// Consumes as much of the receive buffer as possible.
    ParseResult parseAvailable();

    /// Byte offset just past the header terminator, or -1 when incomplete.
    int headerEnd() const;

    HttpRequest m_pending;
    HttpRequest m_original;
    QAbstractSocket *m_socket = nullptr;
    QTimer *m_timeoutTimer = nullptr;
    QElapsedTimer m_clock;

    State m_state = State::Idle;
    QByteArray m_buffer;
    int m_headerSize = -1;
    qint64 m_expectedBody = -1;
    bool m_chunked = false;
    bool m_readUntilClose = false;
    HttpResponse m_response;
    int m_redirectCount = 0;
    int m_redirectsForOriginal = 0;
    qint64 m_lastProgressEmitted = 0;
};

} // namespace oqb::network
