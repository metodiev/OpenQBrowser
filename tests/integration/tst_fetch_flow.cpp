#include <QtTest>

#include <QEventLoop>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include "browser/Page.h"
#include "network/FetchPolicy.h"
#include "network/ResourceLoader.h"
#include "network/ScriptFetch.h"
#include "network/Url.h"

using namespace oqb;
using namespace oqb::network;

/// A server that reports exactly what it was asked for.
///
/// The unit suite proves what the bindings do with a response. What can only be
/// proven over a socket is that the request that left the browser was the one the
/// page asked for: the method, the body, the headers, and whether the cookies the
/// jar holds went out with it. So this server records the raw request line and
/// headers rather than answering from a table.
class RecordingServer : public QTcpServer
{
    Q_OBJECT

public:
    explicit RecordingServer(QObject *parent = nullptr)
        : QTcpServer(parent)
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = nextPendingConnection()) {
                // Shared because the lambda outlives this iteration, and the
                // handled flag because a request can span several readyRead
                // signals: without it the first chunk would be answered twice.
                auto buffer = QSharedPointer<QByteArray>::create();
                auto handled = QSharedPointer<bool>::create(false);

                connect(socket, &QTcpSocket::readyRead, socket,
                        [this, socket, buffer, handled] {
                            if (*handled)
                                return;
                            buffer->append(socket->readAll());

                            // A request is complete once the headers are, plus
                            // whatever body Content-Length promised. Answering
                            // before the body has arrived would report a POST
                            // with an empty payload.
                            const int headerEnd = buffer->indexOf("\r\n\r\n");
                            if (headerEnd < 0)
                                return;

                            const QByteArray head = buffer->left(headerEnd);
                            if (buffer->size() - headerEnd - 4 < lengthFrom(head))
                                return;

                            *handled = true;
                            const QByteArray request = *buffer;
                            m_requests.append(QString::fromUtf8(request));

                            // The response is written before the socket is closed:
                            // shutting down first would take the pending bytes
                            // with it, and every test would see a connection that
                            // closed without answering.
                            socket->write(responseFor(request));
                            socket->flush();
                            socket->disconnectFromHost();
                        });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    bool start() { return listen(QHostAddress::LocalHost, 0); }
    int port() const { return serverPort(); }

    Url urlFor(const QString &path) const
    {
        return Url::parse(QStringLiteral("http://127.0.0.1:%1%2").arg(port()).arg(path));
    }

    /// Every request the server received, verbatim.
    const QList<QString> &requests() const { return m_requests; }
    void clearRequests() { m_requests.clear(); }

    /// The headers a response carries, so a test can set up a CORS grant.
    QList<QPair<QString, QString>> extraHeaders;
    QByteArray body = "{\"ok\":true}";
    QByteArray contentType = "application/json";
    int status = 200;

    /// The last request's body, ignoring the headers.
    QByteArray lastBody() const
    {
        if (m_requests.isEmpty())
            return {};
        const QByteArray raw = m_requests.last().toUtf8();
        const int split = raw.indexOf("\r\n\r\n");
        return split < 0 ? QByteArray() : raw.mid(split + 4);
    }

private:
    static int lengthFrom(const QByteArray &head)
    {
        for (const QByteArray &line : head.split('\n')) {
            const QByteArray trimmed = line.trimmed();
            if (trimmed.toLower().startsWith("content-length:"))
                return trimmed.mid(trimmed.indexOf(':') + 1).trimmed().toInt();
        }
        return 0;
    }

    QByteArray responseFor(const QByteArray &request) const
    {
        Q_UNUSED(request);
        QByteArray out = "HTTP/1.1 " + QByteArray::number(status) + " OK\r\n";
        out += "Content-Type: " + contentType + "\r\n";
        out += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
        for (const auto &header : extraHeaders)
            out += header.first.toUtf8() + ": " + header.second.toUtf8() + "\r\n";
        out += "Connection: close\r\n\r\n";
        out += body;
        return out;
    }

    QList<QString> m_requests;
};

/// fetch() over a real socket.
///
/// These assert the exchange rather than the JavaScript-facing behaviour, which
/// the unit suite already covers: what only a socket can show is that the request
/// on the wire carried what the page asked for, and that the page's own loader
/// never mistakes a script response for one of its subresources.
class FetchFlowTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void sendsGetAndReceivesBody();
    void sendsPostBodyAndHeaders();
    void doesNotConfuseThePageLoader();
    void blocksCrossOriginWithoutGrant();
    void allowsCrossOriginWithGrant();
    void refusesASecurePageDowngrade();
    void seesOnlyTheHeadersItIsGranted();
    void completesWithoutAPageAttached();
    void cancelAllAfterScriptFetchCompletesIsSafe();

private:
    /// Runs `fetch` against `server` and waits for `signalIsTrue` to hold.
    bool waitFor(const std::function<bool()> &predicate, int timeoutMs = 5000);

    std::unique_ptr<RecordingServer> m_server;
    std::unique_ptr<ResourceLoader> m_loader;
    QList<ScriptFetchResponse> m_responses;
};

void FetchFlowTest::init()
{
    m_server = std::make_unique<RecordingServer>();
    QVERIFY(m_server->start());

    m_loader = std::make_unique<ResourceLoader>();
    m_responses.clear();

    connect(m_loader.get(), &ScriptFetchProvider::fetchFinished, this,
            [this](int, const ScriptFetchResponse &response) { m_responses.append(response); });
}

void FetchFlowTest::cleanup()
{
    m_loader.reset();
    m_server.reset();
}

bool FetchFlowTest::waitFor(const std::function<bool()> &predicate, int timeoutMs)
{
    if (predicate())
        return true;

    QEventLoop loop;
    QTimer deadline(this);
    deadline.setSingleShot(true);
    connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);

    QTimer poll(this);
    poll.setInterval(1);
    connect(&poll, &QTimer::timeout, this, [&] {
        if (predicate())
            loop.quit();
    });

    poll.start();
    deadline.start(timeoutMs);
    loop.exec();
    return predicate();
}

void FetchFlowTest::sendsGetAndReceivesBody()
{
    ScriptFetchRequest request;
    request.url = m_server->urlFor(QStringLiteral("/data.json")).toString();
    request.documentUrl = m_server->urlFor(QStringLiteral("/page")).toString();

    const int id = m_loader->startScriptFetch(request);
    QVERIFY(id != 0);

    QVERIFY(waitFor([&] { return !m_responses.isEmpty(); }));
    QCOMPARE(m_responses.first().status, 200);
    QCOMPARE(m_responses.first().body, QByteArray("{\"ok\":true}"));
    QCOMPARE(m_responses.first().error, QString());

    // The request that reached the server was a GET for the path that was asked
    // for, with the browser's own headers filled in.
    QCOMPARE(m_server->requests().size(), 1);
    const QString requestText = m_server->requests().first();
    QVERIFY2(requestText.startsWith(QStringLiteral("GET /data.json HTTP/1.1")),
             qPrintable(requestText.left(80)));
    QVERIFY(requestText.contains(QStringLiteral("Origin:")));
}

void FetchFlowTest::sendsPostBodyAndHeaders()
{
    ScriptFetchRequest request;
    request.url = m_server->urlFor(QStringLiteral("/submit")).toString();
    request.documentUrl = m_server->urlFor(QStringLiteral("/page")).toString();
    request.method = QStringLiteral("POST");
    request.body = "name=openq&n=42";
    request.headers.append({QStringLiteral("X-Token"), QStringLiteral("t1")});
    request.headers.append({QStringLiteral("Content-Type"),
                            QStringLiteral("application/x-www-form-urlencoded")});

    const int id = m_loader->startScriptFetch(request);
    QVERIFY(id != 0);
    QVERIFY(waitFor([&] { return !m_responses.isEmpty(); }));

    const QString requestText = m_server->requests().first();
    QVERIFY2(requestText.startsWith(QStringLiteral("POST /submit HTTP/1.1")),
             qPrintable(requestText.left(80)));

    // The page's own headers went out, and the body arrived intact. A POST that
    // lost its payload would be the kind of failure a page reports as a server
    // bug, so it is worth asserting on the wire.
    QVERIFY(requestText.contains(QStringLiteral("X-Token: t1")));
    QVERIFY(requestText.contains(
        QStringLiteral("Content-Type: application/x-www-form-urlencoded")));
    QCOMPARE(m_server->lastBody(), QByteArray("name=openq&n=42"));
}

void FetchFlowTest::doesNotConfuseThePageLoader()
{
    // A page is loading, and script fetches something meanwhile. The page's own
    // finished() handler must not see the script response: it would decrement the
    // page's in-flight counter, be treated as a stylesheet or an image, and on a
    // page with nothing else outstanding could end the load early.
    int pageCompletions = 0;
    connect(m_loader.get(), &ResourceLoader::finished, this,
            [&pageCompletions](const Resource &) { ++pageCompletions; });

    ScriptFetchRequest request;
    request.url = m_server->urlFor(QStringLiteral("/data.json")).toString();
    request.documentUrl = m_server->urlFor(QStringLiteral("/page")).toString();

    QVERIFY(m_loader->startScriptFetch(request) != 0);
    QVERIFY(waitFor([&] { return !m_responses.isEmpty(); }));

    QCOMPARE(pageCompletions, 0);
    QCOMPARE(m_responses.size(), 1);

    // An ordinary page request still takes the page path, so the separation is
    // not simply "nothing reaches finished() any more".
    m_loader->fetch(m_server->urlFor(QStringLiteral("/style.css")));
    QVERIFY(waitFor([&] { return pageCompletions > 0; }));
    QCOMPARE(pageCompletions, 1);
}

void FetchFlowTest::blocksCrossOriginWithoutGrant()
{
    // The document is on one origin and the request goes to another, which is
    // what this server always is. With no Access-Control-Allow-Origin the body
    // must not be handed over.
    m_server->extraHeaders.clear();

    ScriptFetchRequest request;
    request.url = m_server->urlFor(QStringLiteral("/secret")).toString();
    request.documentUrl = QStringLiteral("http://example.com/page");

    QVERIFY(m_loader->startScriptFetch(request) != 0);
    QVERIFY(waitFor([&] { return !m_responses.isEmpty(); }));

    QVERIFY2(!m_responses.first().error.isEmpty(), "a cross-origin response without a grant must fail");
    QVERIFY2(m_responses.first().body.isEmpty(), "the body must not reach script");
}

void FetchFlowTest::allowsCrossOriginWithGrant()
{
    m_server->extraHeaders.append({QStringLiteral("Access-Control-Allow-Origin"),
                                   QStringLiteral("http://example.com")});

    ScriptFetchRequest request;
    request.url = m_server->urlFor(QStringLiteral("/public")).toString();
    request.documentUrl = QStringLiteral("http://example.com/page");

    QVERIFY(m_loader->startScriptFetch(request) != 0);
    QVERIFY(waitFor([&] { return !m_responses.isEmpty(); }));

    QCOMPARE(m_responses.first().error, QString());
    QCOMPARE(m_responses.first().body, QByteArray("{\"ok\":true}"));
}

void FetchFlowTest::refusesASecurePageDowngrade()
{
    // A secure document may not be made to pull a plaintext resource in: that is
    // how an attacker who controls a plaintext response injects content into an
    // HTTPS page. The refusal happens before a request is built, so the server
    // must not see one.
    ScriptFetchRequest request;
    request.url = m_server->urlFor(QStringLiteral("/downgrade")).toString();
    request.documentUrl = QStringLiteral("https://example.com/page");

    QCOMPARE(m_loader->startScriptFetch(request), 0);
    QVERIFY(!m_loader->refusalReason().isEmpty());
    QVERIFY(m_server->requests().isEmpty());
}

void FetchFlowTest::seesOnlyTheHeadersItIsGranted()
{
    m_server->extraHeaders.append({QStringLiteral("Access-Control-Allow-Origin"),
                                   QStringLiteral("*")});
    m_server->extraHeaders.append({QStringLiteral("X-Internal-Id"),
                                   QStringLiteral("node-7")});

    ScriptFetchRequest request;
    request.url = m_server->urlFor(QStringLiteral("/public")).toString();
    request.documentUrl = QStringLiteral("http://example.com/page");

    QVERIFY(m_loader->startScriptFetch(request) != 0);
    QVERIFY(waitFor([&] { return !m_responses.isEmpty(); }));

    QList<QPair<QString, QString>> visible;
    for (const auto &header : m_responses.first().headers)
        visible.append(header);

    bool sawInternal = false;
    for (const auto &header : visible) {
        if (header.first.compare(QStringLiteral("X-Internal-Id"), Qt::CaseInsensitive) == 0)
            sawInternal = true;
    }

    // The grant is a wildcard and the request carried no credentials, so the body
    // is readable - but an unnamed header is not, because a response's headers
    // describe more than its content.
    QVERIFY2(!sawInternal, "an unnamed header must not be visible cross-origin");
    QCOMPARE(m_responses.first().error, QString());
}

void FetchFlowTest::completesWithoutAPageAttached()
{
    // A loader on its own has no page to route responses to. A script request
    // must still complete, because the loader is what a test and the headless
    // path use.
    ScriptFetchRequest request;
    request.url = m_server->urlFor(QStringLiteral("/data.json")).toString();
    request.documentUrl = m_server->urlFor(QStringLiteral("/page")).toString();

    QVERIFY(m_loader->startScriptFetch(request) != 0);
    QVERIFY(waitFor([&] { return !m_responses.isEmpty(); }));
    QCOMPARE(m_responses.first().status, 200);
}

void FetchFlowTest::cancelAllAfterScriptFetchCompletesIsSafe()
{
    // A script request lives in both the script map and the shared client map.
    // When it completed, the client was scheduled for deletion. If it was left
    // in the shared map, the next navigation's cancelAll() would abort memory
    // that had already been freed - the crash a link click used to trigger.
    ScriptFetchRequest request;
    request.url = m_server->urlFor(QStringLiteral("/data.json")).toString();
    request.documentUrl = m_server->urlFor(QStringLiteral("/page")).toString();

    QVERIFY(m_loader->startScriptFetch(request) != 0);
    QVERIFY(waitFor([&] { return !m_responses.isEmpty(); }));

    // Actually run the deferred deletion, so any lingering reference to the
    // completed request's client is now dangling.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    // This is what Page::load() calls when a navigation interrupts a load.
    // It must be a no-op for requests that already finished, not a crash.
    m_loader->cancelAll();

    // And a fresh request afterwards must still work: the loader was not left
    // in a state where a stale entry blocks new work.
    m_responses.clear();
    const int second = m_loader->startScriptFetch(request);
    QVERIFY(second != 0);
    QVERIFY(waitFor([&] { return !m_responses.isEmpty(); }));
    QCOMPARE(m_responses.first().status, 200);
}

QTEST_MAIN(FetchFlowTest)
#include "tst_fetch_flow.moc"
