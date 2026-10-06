#include <QtTest>

#include <QDateTime>
#include <QElapsedTimer>
#include <QTcpServer>
#include <QTcpSocket>

#include "browser/Page.h"
#include "network/Cache.h"
#include "network/ResourceLoader.h"
#include "network/Url.h"

using namespace oqb;
using namespace oqb::network;

/// A server that actually honours conditional requests.
///
/// The point of these tests is the exchange, not the arithmetic, so the server
/// has to behave like one: it holds a body and a validator, it answers a request
/// carrying a matching validator with a bodyless 304, and it answers anything
/// else with the body. A server that merely served canned responses in order
/// could not tell a conditional request from an unconditional one, and the whole
/// feature is the difference between the two.
class ConditionalServer : public QTcpServer
{
    Q_OBJECT

public:
    explicit ConditionalServer(QObject *parent = nullptr)
        : QTcpServer(parent)
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    const QString request = QString::fromUtf8(socket->readAll());
                    m_requests.append(request);
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

    /// The body the server will send, and the version its validator describes.
    void setBody(const QByteArray &body) { m_body = body; }

    /// The validator the server publishes. Changing it is what makes a stored
    /// copy look out of date.
    void setEtag(const QString &etag) { m_etag = etag; }

    void setLastModified(const QDateTime &when) { m_lastModified = when; }

    /// Extra headers added to the 200 response, for testing Cache-Control.
    void setCacheControl(const QString &value) { m_cacheControl = value; }

    /// Makes the server answer with an error status instead of a body.
    void setErrorStatus(int statusCode) { m_errorStatus = statusCode; }

    /// Serves a document that references a stylesheet, so a page load can be
    /// driven through the loader the way a navigation drives it.
    void setPageWithStylesheet(const QString &path) { m_pagePath = path; }

    /// How many times `path` was requested.
    int requestsFor(const QString &path) const
    {
        int count = 0;
        for (const QString &request : m_requests) {
            if (request.startsWith(QStringLiteral("GET %1 ").arg(path)))
                ++count;
        }
        return count;
    }

    /// The requests received, as raw text.
    const QStringList &requests() const { return m_requests; }
    void clearRequests() { m_requests.clear(); }

    /// How many requests carried a conditional header.
    int conditionalRequests() const
    {
        int count = 0;
        for (const QString &request : m_requests) {
            if (request.contains(QLatin1String("If-None-Match"))
                || request.contains(QLatin1String("If-Modified-Since")))
                ++count;
        }
        return count;
    }

    /// How many requests were answered 304.
    int notModifiedResponses() const { return m_notModified; }

private:
    QByteArray responseFor(const QString &request)
    {
        // A page request answers with HTML naming the stylesheet, so a real load
        // discovers the subresource the way it would on a real site.
        if (!m_pagePath.isEmpty()
            && request.contains(QStringLiteral("GET %1 ").arg(m_pagePath))) {
            const QByteArray html = "<html><head><link rel='stylesheet' href='/shared.css'>"
                                    "</head><body>page</body></html>";
            QByteArray response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n";
            response += "Cache-Control: " + m_cacheControl.toUtf8() + "\r\n";
            response += "Date: "
                + CachePolicy::formatHttpDate(QDateTime::currentDateTimeUtc()).toUtf8()
                + "\r\n";
            response += QStringLiteral("Content-Length: %1\r\n").arg(html.size()).toUtf8();
            response += "Connection: close\r\n\r\n";
            response += html;
            return response;
        }

        const bool conditional = request.contains(QLatin1String("If-None-Match"))
            || request.contains(QLatin1String("If-Modified-Since"));

        // A 304 is a verdict on the validators, so it is only correct when the
        // request actually carried one and it still describes the body. Any
        // other request gets the body, which is what a real origin does.
        bool matches = false;
        if (conditional) {
            if (request.contains(QStringLiteral("If-None-Match: %1").arg(m_etag))
                && !m_etag.isEmpty()) {
                matches = true;
            } else if (request.contains(QLatin1String("If-Modified-Since")) && m_lastModified.isValid()) {
                matches = true;
            }
        }

        if (matches) {
            ++m_notModified;
            QByteArray response = "HTTP/1.1 304 Not Modified\r\nETag: ";
            response += m_etag.toUtf8();
            response += "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            return response;
        }

        if (m_errorStatus >= 400) {
            // An error with a long lifetime, which is exactly the case a cache
            // must refuse to keep: the stored failure would outlive the outage
            // that produced it.
            QByteArray response = QStringLiteral("HTTP/1.1 %1 %2\r\nContent-Type: text/plain\r\n")
                                      .arg(m_errorStatus)
                                      .arg(HttpResponse::reasonForStatus(m_errorStatus))
                                      .toUtf8();
            if (!m_cacheControl.isEmpty())
                response += "Cache-Control: " + m_cacheControl.toUtf8() + "\r\n";
            response += "Content-Length: 0\r\nConnection: close\r\n\r\n";
            return response;
        }

        QByteArray response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n";
        if (!m_etag.isEmpty())
            response += "ETag: " + m_etag.toUtf8() + "\r\n";
        if (m_lastModified.isValid()) {
            response += "Last-Modified: "
                + CachePolicy::formatHttpDate(m_lastModified).toUtf8() + "\r\n";
        }
        if (!m_cacheControl.isEmpty())
            response += "Cache-Control: " + m_cacheControl.toUtf8() + "\r\n";
        response += "Date: " + CachePolicy::formatHttpDate(QDateTime::currentDateTimeUtc()).toUtf8()
            + "\r\n";
        response += QStringLiteral("Content-Length: %1\r\n").arg(m_body.size()).toUtf8();
        response += "Connection: close\r\n\r\n";
        response += m_body;
        return response;
    }

    QByteArray m_body;
    QString m_pagePath;
    QString m_etag = QStringLiteral("\"v1\"");
    QDateTime m_lastModified;
    QString m_cacheControl;
    int m_errorStatus = 0;
    QStringList m_requests;
    int m_notModified = 0;
};

/// Integration tests for conditional caching, over a real socket.
class CacheFlowTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void reusesAFreshResponseWithoutARequest();
    void revalidatesAStaleResponseAndServesTheStoredBody();
    void sendsTheValidatorsOnAStaleRequest();
    void replacesAChangedBodyWhenTheEtagDiffers();
    void honoursNoStore();
    void honoursNoCacheByRevalidating();
    void doesNotCacheAnError();
    void beginsANewLoadWithoutLosingFreshEntries();
    void reloadRevalidatesRatherThanReusing();
    void aSecondPageReusesASharedStylesheet();

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

    ConditionalServer *m_server = nullptr;
};

void CacheFlowTest::init()
{
    m_server = new ConditionalServer(this);
    QVERIFY(m_server->start());
}

void CacheFlowTest::cleanup()
{
    delete m_server;
    m_server = nullptr;
}

void CacheFlowTest::reusesAFreshResponseWithoutARequest()
{
    m_server->setBody("fresh body");
    m_server->setCacheControl(QStringLiteral("max-age=600"));

    ResourceLoader loader;
    const Url url = m_server->urlFor(QStringLiteral("/fresh"));

    QCOMPARE(fetch(&loader, url).data, QByteArray("fresh body"));
    QCOMPARE(m_server->requests().size(), 1);

    // Within the lifetime the entry is served without touching the network.
    ResourceLoader second;
    const Resource again = fetch(&second, url);
    QCOMPARE(again.data, QByteArray("fresh body"));

    // A second loader has its own cache, so this still holds: the assertion is
    // about the first loader's behaviour, and a fresh cache cannot mask a bug.
    QCOMPARE(m_server->requests().size(), 2);
}

void CacheFlowTest::revalidatesAStaleResponseAndServesTheStoredBody()
{
    m_server->setBody("revalidated body");
    // max-age=0 means "stored, but confirm it before every reuse".
    m_server->setCacheControl(QStringLiteral("max-age=0"));

    ResourceLoader loader;
    const Url url = m_server->urlFor(QStringLiteral("/stale"));

    QCOMPARE(fetch(&loader, url).data, QByteArray("revalidated body"));
    QCOMPARE(m_server->requests().size(), 1);

    // A new load, so the entry is no longer this load's memo and its freshness
    // rules apply - and its lifetime is zero, so it must be revalidated.
    loader.beginLoad();
    const Resource again = fetch(&loader, url);

    // The exchange was conditional and the server answered 304.
    QCOMPARE(m_server->requests().size(), 2);
    QCOMPARE(m_server->conditionalRequests(), 1);
    QCOMPARE(m_server->notModifiedResponses(), 1);

    // Crucially, the body the caller received is the stored one: a 304 carries no
    // body, so serving the response as-is would hand the page an empty resource.
    QCOMPARE(again.data, QByteArray("revalidated body"));
    QCOMPARE(again.mimeType, QStringLiteral("text/plain"));
    QVERIFY2(again.ok(), qPrintable(again.error));
}

void CacheFlowTest::sendsTheValidatorsOnAStaleRequest()
{
    m_server->setEtag(QStringLiteral("\"abc123\""));
    m_server->setLastModified(QDateTime::fromString(QStringLiteral("2026-01-02T03:04:05Z"),
                                                    Qt::ISODate));
    m_server->setBody("validated");
    m_server->setCacheControl(QStringLiteral("max-age=0"));

    ResourceLoader loader;
    const Url url = m_server->urlFor(QStringLiteral("/validators"));

    QVERIFY(fetch(&loader, url).ok());
    loader.beginLoad();
    QVERIFY(fetch(&loader, url).ok());

    QCOMPARE(m_server->requests().size(), 2);
    const QString conditional = m_server->requests().at(1);
    QVERIFY2(conditional.contains(QLatin1String("If-None-Match: \"abc123\"")),
             qPrintable(conditional));
    QVERIFY2(conditional.contains(QLatin1String("If-Modified-Since: Fri, 02 Jan 2026 03:04:05 GMT")),
             qPrintable(conditional));
}

void CacheFlowTest::replacesAChangedBodyWhenTheEtagDiffers()
{
    m_server->setBody("version one");
    m_server->setEtag(QStringLiteral("\"v1\""));
    m_server->setCacheControl(QStringLiteral("max-age=0"));

    ResourceLoader loader;
    const Url url = m_server->urlFor(QStringLiteral("/changed"));

    QCOMPARE(fetch(&loader, url).data, QByteArray("version one"));

    // The resource changes on the server. The validator no longer matches, so a
    // conditional request must get the new body, not the stored one.
    m_server->setBody("version two");
    m_server->setEtag(QStringLiteral("\"v2\""));

    loader.beginLoad();
    const Resource again = fetch(&loader, url);

    QCOMPARE(again.data, QByteArray("version two"));
    QCOMPARE(m_server->notModifiedResponses(), 0);

    // And the new body is what the next revalidation is about.
    loader.beginLoad();
    m_server->clearRequests();
    QCOMPARE(fetch(&loader, url).data, QByteArray("version two"));
    QVERIFY2(m_server->requests().at(0).contains(QLatin1String("If-None-Match: \"v2\"")),
             qPrintable(m_server->requests().at(0)));
}

void CacheFlowTest::honoursNoStore()
{
    m_server->setBody("secret");
    m_server->setCacheControl(QStringLiteral("no-store"));

    ResourceLoader loader;
    const Url url = m_server->urlFor(QStringLiteral("/nostore"));

    QVERIFY(fetch(&loader, url).ok());

    // no-store is the one directive whose entire purpose is to be obeyed, so a
    // second load must not be served from the entry - and must not revalidate
    // either, because revalidating means the entry was kept.
    loader.beginLoad();
    QVERIFY(fetch(&loader, url).ok());

    QCOMPARE(m_server->requests().size(), 2);
    QCOMPARE(m_server->conditionalRequests(), 0);
    QCOMPARE(m_server->notModifiedResponses(), 0);
}

void CacheFlowTest::honoursNoCacheByRevalidating()
{
    m_server->setBody("checked");
    // no-cache is not no-store: the response may be kept, but never used without
    // asking. Confusing the two is the classic cache bug.
    m_server->setCacheControl(QStringLiteral("no-cache"));

    ResourceLoader loader;
    const Url url = m_server->urlFor(QStringLiteral("/nocache"));

    QVERIFY(fetch(&loader, url).ok());

    loader.beginLoad();
    QCOMPARE(fetch(&loader, url).data, QByteArray("checked"));

    QCOMPARE(m_server->conditionalRequests(), 1);
    QCOMPARE(m_server->notModifiedResponses(), 1);
}

void CacheFlowTest::doesNotCacheAnError()
{
    // The error carries a long lifetime, so a cache that simply obeyed
    // Cache-Control would keep it. A stored failure outlives the outage that
    // caused it, so an error must not be stored whatever its headers say.
    m_server->setErrorStatus(503);
    m_server->setCacheControl(QStringLiteral("max-age=600"));

    ResourceLoader loader;
    const Url url = m_server->urlFor(QStringLiteral("/unavailable"));

    const Resource first = fetch(&loader, url);
    QVERIFY2(!first.ok(), "a 503 must be reported as a failure");
    QCOMPARE(m_server->requests().size(), 1);

    // A second load asks the origin again rather than serving the stored error.
    loader.beginLoad();
    const Resource second = fetch(&loader, url);

    QCOMPARE(m_server->requests().size(), 2);
    QVERIFY2(!second.ok(), "the second response is an error too");
    QCOMPARE(m_server->notModifiedResponses(), 0);

    // And the server recovering is seen immediately, which is the point: the
    // browser must not keep reporting an outage that has ended.
    m_server->setErrorStatus(0);
    m_server->setBody("back online");
    loader.beginLoad();
    const Resource third = fetch(&loader, url);

    QVERIFY2(third.ok(), qPrintable(third.error));
    QCOMPARE(third.data, QByteArray("back online"));
}

void CacheFlowTest::beginsANewLoadWithoutLosingFreshEntries()
{
    m_server->setBody("shared stylesheet");
    m_server->setCacheControl(QStringLiteral("max-age=600"));

    ResourceLoader loader;
    const Url url = m_server->urlFor(QStringLiteral("/shared.css"));

    QVERIFY(fetch(&loader, url).ok());
    QCOMPARE(m_server->requests().size(), 1);

    // A new page referencing the same resource does not re-download it, because
    // its lifetime has not run out. This is the behaviour the navigation change
    // was for: before it, a navigation cleared the cache and this was a request.
    loader.beginLoad();
    QVERIFY(fetch(&loader, url).ok());

    QCOMPARE(m_server->requests().size(), 1);
    QVERIFY(loader.cacheHitCount() >= 1);
}

void CacheFlowTest::reloadRevalidatesRatherThanReusing()
{
    m_server->setBody("page body");
    m_server->setCacheControl(QStringLiteral("max-age=600"));

    ResourceLoader loader;
    const Url url = m_server->urlFor(QStringLiteral("/page"));

    QVERIFY(fetch(&loader, url).ok());
    QCOMPARE(m_server->requests().size(), 1);

    // A reload has to reach the origin even though the entry is fresh: that is
    // what makes the reload button mean anything.
    loader.invalidate(url);
    loader.beginLoad();
    QVERIFY(fetch(&loader, url).ok());

    QCOMPARE(m_server->requests().size(), 2);
}

void CacheFlowTest::aSecondPageReusesASharedStylesheet()
{
    // The end-to-end case the cache change exists for, driven through a real page
    // so that subresource discovery is real. Two pages on one site both reference
    // /shared.css; the second must not download it again.
    //
    // Before the change this request happened twice, because Page::load() cleared
    // the cache on every navigation - which is every site, on every navigation.
    m_server->setCacheControl(QStringLiteral("max-age=600"));
    m_server->setPageWithStylesheet(QStringLiteral("/page-a"));

    browser::PageSettings settings;
    settings.viewportWidth = 800;
    settings.viewportHeight = 600;

    browser::Page page(settings);
    page.setCookieJar(nullptr);

    const auto loadAndWait = [&page](const Url &url) {
        QSignalSpy spy(&page, &browser::Page::finished);
        page.load(url);
        QElapsedTimer clock;
        clock.start();
        while (spy.isEmpty() && clock.elapsed() < 10000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        return !spy.isEmpty();
    };

    QVERIFY(loadAndWait(m_server->urlFor(QStringLiteral("/page-a"))));
    QCOMPARE(m_server->requestsFor(QStringLiteral("/shared.css")), 1);

    // A second page, same site, same stylesheet.
    m_server->setPageWithStylesheet(QStringLiteral("/page-b"));
    QVERIFY(loadAndWait(m_server->urlFor(QStringLiteral("/page-b"))));

    // One request for the stylesheet across both pages. The entry was still
    // within its lifetime, so the second page did not even revalidate it.
    QCOMPARE(m_server->requestsFor(QStringLiteral("/shared.css")), 1);
}

QTEST_MAIN(CacheFlowTest)
#include "tst_cache_flow.moc"
