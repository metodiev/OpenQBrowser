#include <QtTest>

#include "network/Url.h"

using namespace oqb::network;

/// Tests for URL parsing and reference resolution (RFC 3986).
class UrlTest : public QObject
{
    Q_OBJECT

private slots:
    void parsesAbsoluteHttp();
    void parsesPorts();
    void normalizesDefaultPorts();
    void parsesQueryAndFragment();
    void rejectsEmptyHost();
    void resolvesRelativeReferences_data();
    void resolvesRelativeReferences();
    void resolvesDotSegments_data();
    void resolvesDotSegments();
    void buildsOrigins();
    void handlesFileUrls();
    void interpretsUserInput_data();
    void interpretsUserInput();
    void buildsSearchQueries();
    void extractsAboutPage();
    void understandsIpv6Hosts();
    void detectsSchemes();
};

void UrlTest::parsesAbsoluteHttp()
{
    const Url url = Url::parse(QStringLiteral("http://example.com/a/b?c=d#e"));

    QVERIFY(url.isValid());
    QCOMPARE(url.scheme(), QStringLiteral("http"));
    QCOMPARE(url.host(), QStringLiteral("example.com"));
    QCOMPARE(url.path(), QStringLiteral("/a/b"));
    QCOMPARE(url.query(), QStringLiteral("c=d"));
    QCOMPARE(url.fragment(), QStringLiteral("e"));
    QCOMPARE(url.effectivePort(), 80);
    QVERIFY(url.isHttp());
    QVERIFY(url.isRemote());
    QVERIFY(!url.isSecure());
}

void UrlTest::parsesPorts()
{
    const Url url = Url::parse(QStringLiteral("https://example.com:8443/x"));

    QVERIFY(url.isValid());
    QCOMPARE(url.host(), QStringLiteral("example.com"));
    QCOMPARE(url.port(), 8443);
    QCOMPARE(url.effectivePort(), 8443);
    QCOMPARE(url.toRequestTarget(), QStringLiteral("/x"));
}

void UrlTest::normalizesDefaultPorts()
{
    // A default port is redundant and is dropped, as the URL standard requires.
    const Url https = Url::parse(QStringLiteral("https://example.com:443/"));
    QCOMPARE(https.port(), -1);
    QCOMPARE(https.effectivePort(), 443);
    QCOMPARE(https.toString(), QStringLiteral("https://example.com/"));

    const Url http = Url::parse(QStringLiteral("http://example.com:80/"));
    QCOMPARE(http.port(), -1);
    QCOMPARE(http.toString(), QStringLiteral("http://example.com/"));
}

void UrlTest::parsesQueryAndFragment()
{
    const Url url = Url::parse(QStringLiteral("https://example.com/search?q=hello%20world&lang=en#results"));

    QCOMPARE(url.query(), QStringLiteral("q=hello%20world&lang=en"));
    QCOMPARE(url.fragment(), QStringLiteral("results"));
    // The fragment is never sent on the wire.
    QCOMPARE(url.toRequestTarget(), QStringLiteral("/search?q=hello%20world&lang=en"));
}

void UrlTest::rejectsEmptyHost()
{
    QVERIFY(!Url::parse(QStringLiteral("http://")).isValid());
    QVERIFY(!Url::parse(QStringLiteral("")).isValid());
}

void UrlTest::resolvesRelativeReferences_data()
{
    QTest::addColumn<QString>("base");
    QTest::addColumn<QString>("relative");
    QTest::addColumn<QString>("expected");

    QTest::newRow("absolute") << "http://a.com/x/y" << "http://b.com/z" << "http://b.com/z";
    QTest::newRow("root relative") << "http://a.com/x/y" << "/z" << "http://a.com/z";
    QTest::newRow("path relative") << "http://a.com/x/y" << "z" << "http://a.com/x/z";
    QTest::newRow("parent") << "http://a.com/x/y/z" << "../w" << "http://a.com/x/w";
    QTest::newRow("same dir") << "http://a.com/x/y" << "./z" << "http://a.com/x/z";
    QTest::newRow("scheme relative") << "https://a.com/x" << "//b.com/y" << "https://b.com/y";
    QTest::newRow("query only") << "http://a.com/x?q=1" << "?r=2" << "http://a.com/x?r=2";
    QTest::newRow("fragment only") << "http://a.com/x" << "#top" << "http://a.com/x#top";
    QTest::newRow("empty keeps base") << "http://a.com/x/y" << "" << "";
    QTest::newRow("port kept") << "http://a.com:81/x" << "y" << "http://a.com:81/y";
}

void UrlTest::resolvesRelativeReferences()
{
    QFETCH(QString, base);
    QFETCH(QString, relative);
    QFETCH(QString, expected);

    const Url resolved = Url::parse(base).resolved(relative);
    if (expected.isEmpty())
        QVERIFY(!resolved.isValid());
    else
        QCOMPARE(resolved.toString(), expected);
}

void UrlTest::resolvesDotSegments_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<QString>("expected");

    QTest::newRow("single dot") << "http://a.com/a/./b" << "http://a.com/a/b";
    QTest::newRow("double dot") << "http://a.com/a/b/../c" << "http://a.com/a/c";
    QTest::newRow("leading double") << "http://a.com/../a" << "http://a.com/a";
    QTest::newRow("trailing dot") << "http://a.com/a/." << "http://a.com/a/";
    QTest::newRow("multiple") << "http://a.com/a/./b/../c/./d" << "http://a.com/a/c/d";
}

void UrlTest::resolvesDotSegments()
{
    QFETCH(QString, input);
    QFETCH(QString, expected);

    QCOMPARE(Url::parse(input).toString(), expected);
}

void UrlTest::buildsOrigins()
{
    QCOMPARE(Url::parse(QStringLiteral("https://a.com/x")).origin(),
             QStringLiteral("https://a.com:443"));
    QCOMPARE(Url::parse(QStringLiteral("http://a.com:8080/x")).origin(),
             QStringLiteral("http://a.com:8080"));

    // The same host on a different scheme or port is a different origin.
    QVERIFY(Url::parse(QStringLiteral("http://a.com")).origin()
            != Url::parse(QStringLiteral("https://a.com")).origin());
    QVERIFY(Url::parse(QStringLiteral("http://a.com")).origin()
            != Url::parse(QStringLiteral("http://a.com:81")).origin());
}

void UrlTest::handlesFileUrls()
{
    const Url url = Url::fromLocalFile(QStringLiteral("/tmp/example page.html"));

    QVERIFY(url.isValid());
    QVERIFY(url.isLocalFile());
    QVERIFY(!url.isRemote());
    QCOMPARE(url.scheme(), QStringLiteral("file"));
    // The space is percent-encoded in the URL but decodes back to the path.
    QCOMPARE(url.toLocalFile(), QStringLiteral("/tmp/example page.html"));
}

void UrlTest::interpretsUserInput_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<QString>("expected");

    QTest::newRow("bare domain") << "example.com" << "https://example.com/";
    QTest::newRow("with path") << "example.com/a/b" << "https://example.com/a/b";
    QTest::newRow("explicit scheme") << "http://example.com" << "http://example.com/";
    QTest::newRow("localhost is http") << "localhost:8080/x" << "http://localhost:8080/x";
    QTest::newRow("about") << "about:home" << "about:home";
    QTest::newRow("search phrase is rejected") << "how tall is everest" << "";
}

void UrlTest::interpretsUserInput()
{
    QFETCH(QString, input);
    QFETCH(QString, expected);

    const Url url = Url::fromUserInput(input);
    if (expected.isEmpty())
        QVERIFY(!url.isValid()); // Callers turn this into a search query.
    else
        QCOMPARE(url.toString(), expected);
}

void UrlTest::buildsSearchQueries()
{
    const Url url = Url::forSearchQuery(QStringLiteral("open q browser"),
                                        QStringLiteral("https://duckduckgo.com/?q=%s"));

    QVERIFY(url.isValid());
    QCOMPARE(url.host(), QStringLiteral("duckduckgo.com"));
    QCOMPARE(url.query(), QStringLiteral("q=open%20q%20browser"));
}

void UrlTest::extractsAboutPage()
{
    QCOMPARE(Url::parse(QStringLiteral("about:home")).aboutPage(), QStringLiteral("home"));
    QCOMPARE(Url::parse(QStringLiteral("about:blank")).aboutPage(), QStringLiteral("blank"));
    QVERIFY(Url::parse(QStringLiteral("https://a.com")).aboutPage().isEmpty());
}

void UrlTest::understandsIpv6Hosts()
{
    const Url url = Url::parse(QStringLiteral("http://[::1]:8080/x"));

    QVERIFY(url.isValid());
    QCOMPARE(url.host(), QStringLiteral("[::1]"));
    QCOMPARE(url.port(), 8080);
    QCOMPARE(url.path(), QStringLiteral("/x"));
}

void UrlTest::detectsSchemes()
{
    QVERIFY(Url::parse(QStringLiteral("https://a.com")).isSecure());
    QVERIFY(!Url::parse(QStringLiteral("http://a.com")).isSecure());
    QVERIFY(Url::parse(QStringLiteral("about:blank")).isSecure());
    QVERIFY(Url::parse(QStringLiteral("ftp://a.com")).isValid());
    QVERIFY(!Url::parse(QStringLiteral("ftp://a.com")).isRemote());

    // Scheme matching is case-insensitive.
    QCOMPARE(Url::parse(QStringLiteral("HTTPS://A.COM/X")).scheme(), QStringLiteral("https"));
    QCOMPARE(Url::parse(QStringLiteral("HTTPS://A.COM/X")).host(), QStringLiteral("a.com"));
}

QTEST_MAIN(UrlTest)
#include "tst_url.moc"
