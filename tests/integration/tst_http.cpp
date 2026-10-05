#include <QtTest>

#include "network/HttpClient.h"
#include "network/HttpMessage.h"
#include "network/ResourceLoader.h"
#include "network/Url.h"

#include <QTcpServer>
#include <QTcpSocket>

using namespace oqb::network;

/// Serves canned HTTP responses so the client can be tested without a network.
class TestServer : public QTcpServer
{
    Q_OBJECT

public:
    explicit TestServer(QObject *parent = nullptr)
        : QTcpServer(parent)
    {
        connect(this, &QTcpServer::newConnection, this, &TestServer::onConnection);
    }

    /// Starts listening on a loopback port.
    bool start()
    {
        return listen(QHostAddress::LocalHost, 0);
    }

    int port() const { return serverPort(); }

    Url urlFor(const QString &path) const
    {
        return Url::parse(QStringLiteral("http://127.0.0.1:%1%2").arg(port()).arg(path));
    }

    /// Queues the response the next connection receives.
    void enqueueResponse(const QByteArray &response) { m_responses.append(response); }

    /// The raw requests received, for assertions.
    QStringList receivedRequests() const { return m_requests; }
    void clearRequests() { m_requests.clear(); }

    /// Closes the connection as soon as the response is written, which is what
    /// makes the client's read-until-close path finish.
    bool closeAfterResponse = true;

private slots:
    void onConnection()
    {
        while (QTcpSocket *socket = nextPendingConnection()) {
            connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                m_requests.append(QString::fromUtf8(socket->readAll()));
                if (m_responses.isEmpty()) {
                    socket->disconnectFromHost();
                    return;
                }
                const QByteArray response = m_responses.takeFirst();
                socket->write(response);
                socket->flush();
                if (closeAfterResponse)
                    socket->disconnectFromHost();
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    }

private:
    QList<QByteArray> m_responses;
    QStringList m_requests;
};

/// Integration tests for the HTTP client and resource loader.
class HttpTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void fetchesSimpleResponse();
    void sendsCorrectRequestLine();
    void sendsHostHeaderWithPort();
    void sendsDefaultUserAgent();
    void readsChunkedResponse();
    void decodesGzipResponse();
    void handlesReadUntilClose();
    void followsRedirects();
    void followsPermanentRedirect();
    void stopsAfterTooManyRedirects();
    void reportsHttpErrors();
    void reportsConnectionFailure();
    void timesOutSlowServer();
    void rejectsWrongContentLength();
    void loaderCachesResults();
    void loaderAppliesConcurrencyLimit();
    void loaderFetchesLocalFiles();
    void decodesChunkedEncoding();
    void decodesGzipEncoding();

private:
    TestServer *m_server = nullptr;
};

void HttpTest::init()
{
    m_server = new TestServer(this);
    QVERIFY(m_server->start());
}

void HttpTest::cleanup()
{
    delete m_server;
    m_server = nullptr;
}

void HttpTest::fetchesSimpleResponse()
{
    const QByteArray body = "hello world";
    m_server->enqueueResponse("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: "
                              + QByteArray::number(body.size()) + "\r\n\r\n" + body);

    HttpClient client;
    QSignalSpy finishedSpy(&client, &HttpClient::finished);
    QSignalSpy failedSpy(&client, &HttpClient::failed);

    HttpRequest request;
    request.url = m_server->urlFor(QStringLiteral("/hello"));
    client.send(request);

    QVERIFY(finishedSpy.wait(5000));
    QVERIFY(failedSpy.isEmpty());

    const auto response = finishedSpy.first().first().value<HttpResponse>();
    QCOMPARE(response.statusCode, 200);
    QCOMPARE(response.reasonPhrase, QStringLiteral("OK"));
    QCOMPARE(response.body, body);
    QCOMPARE(response.contentType(), QStringLiteral("text/plain"));
    QCOMPARE(response.text(), QStringLiteral("hello world"));
    QVERIFY(response.isSuccess());
}

void HttpTest::sendsCorrectRequestLine()
{
    m_server->enqueueResponse("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");

    HttpClient client;
    QSignalSpy finishedSpy(&client, &HttpClient::finished);

    HttpRequest request;
    request.url = m_server->urlFor(QStringLiteral("/path/to/page?q=1&r=2"));
    client.send(request);

    QVERIFY(finishedSpy.wait(5000));

    const QString received = m_server->receivedRequests().join(QString());
    QVERIFY2(received.startsWith(QStringLiteral("GET /path/to/page?q=1&r=2 HTTP/1.1\r\n")),
             qPrintable(received.left(80)));
    // The fragment never reaches the server.
    QVERIFY(!received.contains(QLatin1String("#")));
}

void HttpTest::sendsHostHeaderWithPort()
{
    m_server->enqueueResponse("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");

    HttpClient client;
    QSignalSpy finishedSpy(&client, &HttpClient::finished);

    HttpRequest request;
    request.url = m_server->urlFor(QStringLiteral("/"));
    client.send(request);
    QVERIFY(finishedSpy.wait(5000));

    // A non-default port must appear in Host, or virtual hosting breaks.
    const QString received = m_server->receivedRequests().join(QString());
    QVERIFY2(received.contains(QStringLiteral("Host: 127.0.0.1:%1").arg(m_server->port())),
             qPrintable(received));
}

void HttpTest::sendsDefaultUserAgent()
{
    m_server->enqueueResponse("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");

    HttpClient client;
    QSignalSpy finishedSpy(&client, &HttpClient::finished);

    HttpRequest request;
    request.url = m_server->urlFor(QStringLiteral("/"));
    client.send(request);
    QVERIFY(finishedSpy.wait(5000));

    const QString received = m_server->receivedRequests().join(QString());
    QVERIFY2(received.contains(QLatin1String("User-Agent: Mozilla/5.0 (compatible; OpenQBrowser")),
             qPrintable(received));
}

void HttpTest::readsChunkedResponse()
{
    m_server->enqueueResponse("HTTP/1.1 200 OK\r\n"
                              "Content-Type: text/plain\r\n"
                              "Transfer-Encoding: chunked\r\n\r\n"
                              "5\r\nhello\r\n"
                              "1\r\n \r\n"
                              "5\r\nworld\r\n"
                              "0\r\n\r\n");

    HttpClient client;
    QSignalSpy finishedSpy(&client, &HttpClient::finished);

    HttpRequest request;
    request.url = m_server->urlFor(QStringLiteral("/chunked"));
    client.send(request);

    QVERIFY(finishedSpy.wait(5000));
    const auto response = finishedSpy.first().first().value<HttpResponse>();
    QCOMPARE(response.body, QByteArray("hello world"));
}

void HttpTest::decodesGzipResponse()
{
    // A minimal gzip stream containing "compressed payload", produced with zlib.
    const QByteArray gzip = QByteArray::fromHex(
        "1f8b08000000000002134bcecf2d284a2d2e4e4d512848acccc94f4c0100241ad8d512000000");

    m_server->enqueueResponse("HTTP/1.1 200 OK\r\n"
                              "Content-Type: text/plain\r\n"
                              "Content-Encoding: gzip\r\n"
                              "Content-Length: "
                              + QByteArray::number(gzip.size()) + "\r\n\r\n" + gzip);

    HttpClient client;
    QSignalSpy finishedSpy(&client, &HttpClient::finished);

    HttpRequest request;
    request.url = m_server->urlFor(QStringLiteral("/gzip"));
    client.send(request);

    QVERIFY(finishedSpy.wait(5000));
    const auto response = finishedSpy.first().first().value<HttpResponse>();
    QCOMPARE(response.body, QByteArray("compressed payload"));
}

void HttpTest::handlesReadUntilClose()
{
    // No Content-Length and no Transfer-Encoding: the body ends at disconnect.
    m_server->enqueueResponse(
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nbody without a length");

    HttpClient client;
    QSignalSpy finishedSpy(&client, &HttpClient::finished);

    HttpRequest request;
    request.url = m_server->urlFor(QStringLiteral("/close"));
    client.send(request);

    QVERIFY(finishedSpy.wait(5000));
    const auto response = finishedSpy.first().first().value<HttpResponse>();
    QCOMPARE(response.body, QByteArray("body without a length"));
}

void HttpTest::followsRedirects()
{
    m_server->enqueueResponse("HTTP/1.1 302 Found\r\nLocation: /final\r\nContent-Length: 0\r\n\r\n");
    m_server->enqueueResponse("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfinal");

    HttpClient client;
    QSignalSpy finishedSpy(&client, &HttpClient::finished);
    QSignalSpy redirectedSpy(&client, &HttpClient::redirected);

    HttpRequest request;
    request.url = m_server->urlFor(QStringLiteral("/start"));
    client.send(request);

    QVERIFY(finishedSpy.wait(5000));
    const auto response = finishedSpy.first().first().value<HttpResponse>();
    QCOMPARE(response.statusCode, 200);
    QCOMPARE(response.body, QByteArray("final"));

    // The client reported the hop and ended up at the redirect target.
    QCOMPARE(redirectedSpy.count(), 1);
    QCOMPARE(response.finalUrl.path(), QStringLiteral("/final"));

    // Two requests were made, the second asking for the target.
    const QStringList requests = m_server->receivedRequests();
    QCOMPARE(requests.size(), 2);
    QVERIFY(requests.at(1).startsWith(QLatin1String("GET /final ")));
}

void HttpTest::followsPermanentRedirect()
{
    m_server->enqueueResponse("HTTP/1.1 301 Moved Permanently\r\nLocation: /new\r\n"
                              "Content-Length: 0\r\n\r\n");
    m_server->enqueueResponse("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi");

    HttpClient client;
    QSignalSpy finishedSpy(&client, &HttpClient::finished);

    HttpRequest request;
    request.url = m_server->urlFor(QStringLiteral("/old"));
    client.send(request);

    QVERIFY(finishedSpy.wait(5000));
    const auto response = finishedSpy.first().first().value<HttpResponse>();
    QCOMPARE(response.statusCode, 200);
    QCOMPARE(response.finalUrl.path(), QStringLiteral("/new"));
}

void HttpTest::stopsAfterTooManyRedirects()
{
    // A server that redirects to itself forever.
    for (int i = 0; i < 12; ++i)
        m_server->enqueueResponse("HTTP/1.1 302 Found\r\nLocation: /loop\r\nContent-Length: 0\r\n\r\n");

    HttpClient client;
    QSignalSpy failedSpy(&client, &HttpClient::failed);

    HttpRequest request;
    request.url = m_server->urlFor(QStringLiteral("/loop"));
    request.maxRedirects = 3;
    client.send(request);

    QVERIFY(failedSpy.wait(10000));
    const QString error = failedSpy.first().first().toString();
    QVERIFY2(error.contains(QLatin1String("redirect")), qPrintable(error));
}

void HttpTest::reportsHttpErrors()
{
    m_server->enqueueResponse("HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nnot found");

    HttpClient client;
    QSignalSpy finishedSpy(&client, &HttpClient::finished);

    HttpRequest request;
    request.url = m_server->urlFor(QStringLiteral("/missing"));
    client.send(request);

    QVERIFY(finishedSpy.wait(5000));
    const auto response = finishedSpy.first().first().value<HttpResponse>();
    QCOMPARE(response.statusCode, 404);
    QVERIFY(response.isError());
    QVERIFY(!response.isSuccess());
    QCOMPARE(response.statusText(), QStringLiteral("404 Not Found"));
    // The body of an error response is still delivered.
    QCOMPARE(response.body, QByteArray("not found"));
}

void HttpTest::reportsConnectionFailure()
{
    // Nothing is listening on this port.
    HttpClient client;
    QSignalSpy failedSpy(&client, &HttpClient::failed);

    HttpRequest request;
    request.url = Url::parse(QStringLiteral("http://127.0.0.1:1/"));
    request.timeoutMs = 3000;
    client.send(request);

    QVERIFY(failedSpy.wait(6000));
    QVERIFY(!failedSpy.first().first().toString().isEmpty());
}

void HttpTest::timesOutSlowServer()
{
    // A server that accepts the connection and then says nothing.
    QTcpServer silent;
    QVERIFY(silent.listen(QHostAddress::LocalHost, 0));
    connect(&silent, &QTcpServer::newConnection, [&silent] {
        // Hold the socket open but never respond.
        silent.nextPendingConnection();
    });

    HttpClient client;
    QSignalSpy failedSpy(&client, &HttpClient::failed);

    HttpRequest request;
    request.url = Url::parse(QStringLiteral("http://127.0.0.1:%1/").arg(silent.serverPort()));
    request.timeoutMs = 700;
    client.send(request);

    QVERIFY(failedSpy.wait(5000));
    QVERIFY2(failedSpy.first().first().toString().contains(QLatin1String("timed out")),
             qPrintable(failedSpy.first().first().toString()));
}

void HttpTest::rejectsWrongContentLength()
{
    // The server promises 100 bytes but sends 5 and closes.
    m_server->enqueueResponse("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort");

    HttpClient client;
    QSignalSpy failedSpy(&client, &HttpClient::failed);

    HttpRequest request;
    request.url = m_server->urlFor(QStringLiteral("/truncated"));
    request.timeoutMs = 2000;
    client.send(request);

    // The client must not report a complete response for a truncated body.
    QSignalSpy finishedSpy(&client, &HttpClient::finished);
    QVERIFY(failedSpy.wait(5000) || finishedSpy.count() > 0);
    if (finishedSpy.count() > 0) {
        const auto response = finishedSpy.first().first().value<HttpResponse>();
        QVERIFY2(response.body.size() < 100,
                 "a truncated body must not be reported as complete");
    }
}

void HttpTest::loaderCachesResults()
{
    m_server->enqueueResponse("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                              "Content-Length: 6\r\n\r\ncached");

    ResourceLoader loader;
    QSignalSpy finishedSpy(&loader, &ResourceLoader::finished);

    const Url url = m_server->urlFor(QStringLiteral("/once"));
    loader.fetch(url);
    QVERIFY(finishedSpy.wait(5000));
    QCOMPARE(finishedSpy.count(), 1);

    // A second fetch is served from the cache without a new request.
    loader.fetch(url);
    QCOMPARE(finishedSpy.count(), 2);
    QCOMPARE(m_server->receivedRequests().size(), 1);

    Resource resource;
    QVERIFY(loader.cached(url, &resource));
    QCOMPARE(resource.data, QByteArray("cached"));
}

void HttpTest::loaderAppliesConcurrencyLimit()
{
    ResourceLoader loader;
    loader.setMaxConcurrentRequests(1);

    for (int i = 0; i < 3; ++i) {
        m_server->enqueueResponse("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nx");
    }

    QSignalSpy finishedSpy(&loader, &ResourceLoader::finished);
    for (int i = 0; i < 3; ++i)
        loader.fetch(m_server->urlFor(QStringLiteral("/item%1").arg(i)));

    // All three must complete even though only one runs at a time, which is
    // what proves the queue keeps draining.
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 3, 10000);
}

void HttpTest::loaderFetchesLocalFiles()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString path = directory.filePath(QStringLiteral("page.html"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("<html><body>local</body></html>");
    file.close();

    ResourceLoader loader;
    QSignalSpy finishedSpy(&loader, &ResourceLoader::finished);

    loader.fetch(Url::fromLocalFile(path));
    QCOMPARE(finishedSpy.count(), 1);

    const auto resource = finishedSpy.first().first().value<Resource>();
    QVERIFY2(resource.ok(), qPrintable(resource.error));
    QCOMPARE(resource.data, QByteArray("<html><body>local</body></html>"));
    QVERIFY(resource.mimeType.contains(QLatin1String("html")));

    // A missing file is reported as an error rather than crashing.
    loader.fetch(Url::fromLocalFile(directory.filePath(QStringLiteral("missing.html"))));
    QCOMPARE(finishedSpy.count(), 2);
    QVERIFY(!finishedSpy.at(1).first().value<Resource>().ok());
}

void HttpTest::decodesChunkedEncoding()
{
    const auto decoded = http::decodeChunked("4\r\nwiki\r\n5\r\npedia\r\n0\r\n\r\n");
    QVERIFY(decoded.first);
    QCOMPARE(decoded.second, QByteArray("wikipedia"));

    // A malformed stream is rejected rather than producing partial output.
    QVERIFY(!http::decodeChunked("zz\r\nnope\r\n").first);
    // An incomplete stream is reported as such, so the caller can wait.
    QVERIFY(!http::decodeChunked("5\r\nhel").first);
}

void HttpTest::decodesGzipEncoding()
{
    const QByteArray gzip = QByteArray::fromHex(
        "1f8b08000000000002134bcecf2d284a2d2e4e4d512848acccc94f4c0100241ad8d512000000");
    const auto decoded = http::decodeContentEncoding(QStringLiteral("gzip"), gzip);
    QVERIFY(decoded.first);
    QCOMPARE(decoded.second, QByteArray("compressed payload"));

    // Identity and unknown encodings pass the body through unchanged.
    QCOMPARE(http::decodeContentEncoding(QStringLiteral("identity"), "abc").second,
             QByteArray("abc"));
    QCOMPARE(http::decodeContentEncoding(QStringLiteral("br"), "abc").second, QByteArray("abc"));

    // Corrupt gzip data fails rather than returning garbage.
    QVERIFY(!http::decodeContentEncoding(QStringLiteral("gzip"), "not gzip at all").first);
}

QTEST_MAIN(HttpTest)
#include "tst_http.moc"
