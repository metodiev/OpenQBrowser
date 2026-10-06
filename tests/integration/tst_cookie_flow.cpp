#include <QtTest>

#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>

#include "network/HttpClient.h"
#include "network/ResourceLoader.h"
#include "storage/Cookies.h"

using namespace oqb;
using namespace oqb::network;

/// A loopback server that answers each request from a queued response and
/// records the requests it received, so an assertion can read the headers the
/// client actually sent.
class Server : public QTcpServer
{
    Q_OBJECT

public:
    explicit Server(QObject *parent = nullptr)
        : QTcpServer(parent)
    {
        connect(this, &QTcpServer::newConnection, this, &Server::onConnection);
    }

    bool start() { return listen(QHostAddress::LocalHost, 0); }
    int port() const { return serverPort(); }

    Url urlFor(const QString &path) const
    {
        return Url::parse(QStringLiteral("http://127.0.0.1:%1%2").arg(port()).arg(path));
    }

    void enqueue(const QByteArray &response) { m_responses.append(response); }

    const QStringList &requests() const { return m_requests; }
    void clearRequests() { m_requests.clear(); }

    /// The raw request text for `index`, or an empty string.
    QString requestAt(int index) const { return m_requests.value(index); }

    /// Convenience: a 200 response carrying `body` and any extra headers.
    static QByteArray ok(const QByteArray &body, const QByteArray &extraHeaders = {})
    {
        QByteArray response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n";
        response += extraHeaders;
        response += QStringLiteral("Content-Length: %1\r\n").arg(body.size()).toUtf8();
        response += "Connection: close\r\n\r\n";
        response += body;
        return response;
    }

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
                socket->write(m_responses.takeFirst());
                socket->flush();
                socket->disconnectFromHost();
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    }

private:
    QList<QByteArray> m_responses;
    QStringList m_requests;
};

/// Cookies end to end: a real socket, a real request, and a real response.
///
/// The unit tests prove the jar's rules. These prove the wiring: that a cookie a
/// server sets is stored against the right host, that it comes back on the next
/// request, and that the security rules survive the round trip through the
/// loader rather than only holding inside the jar.
class CookieFlowTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void sendsBackACookieTheServerSet();
    void omitsTheHeaderWhenThereIsNothingToSend();
    void storesCookiesFromARedirectingResponse();
    void doesNotSendACookieToAnotherHost();
    void keepsACookieAcrossSeparateLoaders();
    void derivesEachRedirectHopsCookieHeaderSeparately();
    void sendsACookieAfterARelativeRedirect();

private:
    /// Runs one fetch and returns the resource it produced.
    Resource fetch(ResourceLoader *loader, const Url &url)
    {
        QSignalSpy spy(loader, &ResourceLoader::finished);
        loader->fetch(url);

        QElapsedTimer clock;
        clock.start();
        while (spy.isEmpty() && clock.elapsed() < 5000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

        if (spy.isEmpty())
            return {};
        return spy.last().at(0).value<Resource>();
    }

    Server *m_server = nullptr;
};

void CookieFlowTest::init()
{
    m_server = new Server(this);
    QVERIFY(m_server->start());
}

void CookieFlowTest::cleanup()
{
    delete m_server;
    m_server = nullptr;
}

void CookieFlowTest::sendsBackACookieTheServerSet()
{
    storage::CookieJar jar;
    ResourceLoader loader;
    loader.setCookieJar(&jar);

    // The server sets a session cookie on the first response...
    m_server->enqueue(Server::ok("<html><body>first</body></html>",
                                 "Set-Cookie: session=abc123; Path=/\r\n"));
    const Resource first = fetch(&loader, m_server->urlFor(QStringLiteral("/")));
    QVERIFY(first.ok());
    QCOMPARE(jar.count(), 1);

    // ...and the next request carries it, which is the whole point of a jar.
    m_server->enqueue(Server::ok("<html><body>second</body></html>"));
    const Resource second = fetch(&loader, m_server->urlFor(QStringLiteral("/other")));
    QVERIFY(second.ok());

    QCOMPARE(m_server->requests().size(), 2);
    const QString request = m_server->requestAt(1);
    QVERIFY2(request.contains(QLatin1String("Cookie: session=abc123")), qPrintable(request));
}

void CookieFlowTest::omitsTheHeaderWhenThereIsNothingToSend()
{
    storage::CookieJar jar;
    ResourceLoader loader;
    loader.setCookieJar(&jar);

    m_server->enqueue(Server::ok("<html><body>x</body></html>"));
    QVERIFY(fetch(&loader, m_server->urlFor(QStringLiteral("/"))).ok());

    // No cookie was ever set, so the header must be absent rather than empty: an
    // empty Cookie header is a request a server would have to guess at.
    QVERIFY2(!m_server->requestAt(0).contains(QLatin1String("Cookie:")),
             qPrintable(m_server->requestAt(0)));
}

void CookieFlowTest::storesCookiesFromARedirectingResponse()
{
    storage::CookieJar jar;
    ResourceLoader loader;
    loader.setCookieJar(&jar);

    // A redirect that sets a cookie: the cookie belongs to the server that sent
    // it, so it is stored even though the request then goes elsewhere.
    const Url start = m_server->urlFor(QStringLiteral("/old"));
    const Url target = m_server->urlFor(QStringLiteral("/new"));

    m_server->enqueue(QByteArray("HTTP/1.1 302 Found\r\nLocation: ")
                      + target.toString().toUtf8()
                      + "\r\nSet-Cookie: fromredirect=1; Path=/\r\nContent-Length: 0\r\n"
                        "Connection: close\r\n\r\n");
    m_server->enqueue(Server::ok("<html><body>arrived</body></html>"));

    const Resource resource = fetch(&loader, start);
    QVERIFY(resource.ok());

    QCOMPARE(jar.count(), 1);
    QCOMPARE(jar.all().first().name, QStringLiteral("fromredirect"));

    // And the request that followed the redirect carried it, because the target
    // is the same host.
    QCOMPARE(m_server->requests().size(), 2);
    QVERIFY2(m_server->requestAt(1).contains(QLatin1String("Cookie: fromredirect=1")),
             qPrintable(m_server->requestAt(1)));
}

void CookieFlowTest::doesNotSendACookieToAnotherHost()
{
    storage::CookieJar jar;
    ResourceLoader loader;
    loader.setCookieJar(&jar);

    // A cookie is set for 127.0.0.1, then a request goes to "localhost". They are
    // different hosts as far as the cookie rules are concerned even though they
    // resolve to the same machine, which is exactly the case that shows the rules
    // are host-scoped rather than address-scoped.
    m_server->enqueue(Server::ok("<html><body>x</body></html>",
                                 "Set-Cookie: secret=1; Path=/\r\n"));
    QVERIFY(fetch(&loader, m_server->urlFor(QStringLiteral("/"))).ok());
    QCOMPARE(jar.count(), 1);

    const Url localhost = Url::parse(
        QStringLiteral("http://localhost:%1/").arg(m_server->port()));
    m_server->enqueue(Server::ok("<html><body>y</body></html>"));
    QVERIFY(fetch(&loader, localhost).ok());

    QVERIFY2(!m_server->requestAt(1).contains(QLatin1String("secret")),
             qPrintable(m_server->requestAt(1)));
}

void CookieFlowTest::keepsACookieAcrossSeparateLoaders()
{
    // Two loaders sharing one jar is the shape the browser actually has: a jar is
    // owned by the window and every tab reads from it. A session has to survive
    // that split.
    storage::CookieJar jar;

    {
        ResourceLoader first;
        first.setCookieJar(&jar);
        m_server->enqueue(Server::ok("<html><body>x</body></html>",
                                     "Set-Cookie: session=shared; Path=/\r\n"));
        QVERIFY(fetch(&first, m_server->urlFor(QStringLiteral("/"))).ok());
    }

    ResourceLoader second;
    second.setCookieJar(&jar);
    m_server->enqueue(Server::ok("<html><body>y</body></html>"));
    QVERIFY(fetch(&second, m_server->urlFor(QStringLiteral("/"))).ok());

    // The second loader never saw the Set-Cookie, but it sends the cookie,
    // because the jar outlives either of them.
    QVERIFY2(m_server->requestAt(1).contains(QLatin1String("Cookie: session=shared")),
             qPrintable(m_server->requestAt(1)));
}

QTEST_MAIN(CookieFlowTest)
#include "tst_cookie_flow.moc"

void CookieFlowTest::derivesEachRedirectHopsCookieHeaderSeparately()
{
    storage::CookieJar jar;
    ResourceLoader loader;
    loader.setCookieJar(&jar);

    // The first hop is asked for a cookie the jar already holds, and answers with
    // a redirect that both replaces that cookie and sets a new one. The hop after
    // the redirect must carry the new values, not the ones that were sent to the
    // old address - a redirect chain is a sequence of separate requests, and the
    // jar is the authority on each of them.
    jar.store(QStringLiteral("session=old; Path=/"),
              m_server->urlFor(QStringLiteral("/")));

    const Url start = m_server->urlFor(QStringLiteral("/a"));
    const Url target = m_server->urlFor(QStringLiteral("/b"));

    m_server->enqueue(QByteArray("HTTP/1.1 302 Found\r\nLocation: ")
                      + target.toString().toUtf8()
                      + "\r\nSet-Cookie: session=new; Path=/\r\n"
                        "Set-Cookie: added=1; Path=/\r\nContent-Length: 0\r\n"
                        "Connection: close\r\n\r\n");
    m_server->enqueue(Server::ok("<html><body>arrived</body></html>"));

    QVERIFY(fetch(&loader, start).ok());
    QCOMPARE(m_server->requests().size(), 2);

    const QString first = m_server->requestAt(0);
    QVERIFY2(first.contains(QLatin1String("Cookie: session=old")), qPrintable(first));

    // The hop after the redirect carries both cookies, with the replaced value.
    const QString second = m_server->requestAt(1);
    QVERIFY2(second.contains(QLatin1String("session=new")), qPrintable(second));
    QVERIFY2(second.contains(QLatin1String("added=1")), qPrintable(second));
    QVERIFY2(!second.contains(QLatin1String("session=old")), qPrintable(second));
}

void CookieFlowTest::sendsACookieAfterARelativeRedirect()
{
    storage::CookieJar jar;
    ResourceLoader loader;
    loader.setCookieJar(&jar);

    // The shape a real site uses: the Location is a bare path, so it has to be
    // resolved against the URL the redirect came from. This is the case that
    // matters in practice - httpbin, and most login forms, redirect to a sibling
    // path rather than to an absolute URL.
    m_server->enqueue(QByteArray("HTTP/1.1 302 Found\r\nLocation: /cookies\r\n")
                      + "Set-Cookie: oqb=abc; Path=/\r\nContent-Length: 0\r\n"
                        "Connection: close\r\n\r\n");
    m_server->enqueue(Server::ok("<html><body>{\"cookies\": {}}</body></html>"));

    QVERIFY(fetch(&loader, m_server->urlFor(QStringLiteral("/cookies/set?oqb=abc"))).ok());

    QCOMPARE(m_server->requests().size(), 2);
    // The hop after the redirect went to the resolved path...
    QVERIFY2(m_server->requestAt(1).startsWith(QLatin1String("GET /cookies ")),
             qPrintable(m_server->requestAt(1)));
    // ...and carried the cookie the redirect set.
    QVERIFY2(m_server->requestAt(1).contains(QLatin1String("Cookie: oqb=abc")),
             qPrintable(m_server->requestAt(1)));
}
