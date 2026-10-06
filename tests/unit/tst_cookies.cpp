#include <QtTest>

#include "network/HttpMessage.h"
#include "network/Url.h"
#include "storage/Cookies.h"

using namespace oqb;
using namespace oqb::storage;

/// Tests for the cookie jar: parsing, scoping, the security attributes and
/// expiry.
///
/// The rules tested here are the ones that decide whether a page can read a
/// cookie belonging to another, so each case is written as the situation it
/// protects against rather than as a restatement of the code.
class CookiesTest : public QObject
{
    Q_OBJECT

private:
    static network::Url url(const QString &text) { return network::Url::parse(text); }

private slots:
    // Parsing.
    void storesANameValuePair();
    void ignoresMalformedHeaders();
    void keepsAValueContainingEquals();
    void refusesANameItCannotSerialise();

    // Domain scoping.
    void defaultsTheDomainToTheRequestHost();
    void acceptsADomainSuffix();
    void refusesAnUnrelatedDomain();
    void refusesASuffixThatIsNotOnAHostBoundary();
    void refusesADomainClaimOnAnIpAddress();

    // Path scoping.
    void defaultsThePathToTheRequestDirectory();
    void matchesOnlyOnAPathBoundary();

    // Sending.
    void sendsOnlyMatchingCookies();
    void ordersByPathLength();
    void replacesACookieOfTheSameNameDomainAndPath();
    void keepsTwoCookiesWithDifferentPaths();

    // Security attributes.
    void neverSendsASecureCookieOverHttp();
    void sendsASecureCookieOverHttps();
    void recordsHttpOnly();
    void recordsSameSite();

    // Expiry.
    void ignoresAnExpiredCookie();
    void removalDeletesTheCookie();
    void maxAgeWinsOverExpires();
    void sessionCookieIsNotExpired();
    void prunesExpiredCookies();

    // Whole responses.
    void storesEverySetCookieHeader();
    void reportsTheRequestHeader();
};

// ------------------------------------------------------------------ parsing

void CookiesTest::storesANameValuePair()
{
    CookieJar jar;
    QVERIFY(jar.store(QStringLiteral("session=abc123"), url(QStringLiteral("https://example.com/"))));

    QCOMPARE(jar.count(), 1);
    QCOMPARE(jar.cookiesFor(url(QStringLiteral("https://example.com/"))).size(), 1);
    QCOMPARE(jar.cookiesFor(url(QStringLiteral("https://example.com/"))).first().value,
             QStringLiteral("abc123"));
}

void CookiesTest::ignoresMalformedHeaders()
{
    CookieJar jar;

    // No name, no equals, an empty name: none of these is a cookie.
    QVERIFY(!jar.store(QString(), url(QStringLiteral("https://example.com/"))));
    QVERIFY(!jar.store(QStringLiteral("novalue"), url(QStringLiteral("https://example.com/"))));
    QVERIFY(!jar.store(QStringLiteral("=value"), url(QStringLiteral("https://example.com/"))));
    QVERIFY(!jar.store(QStringLiteral("   =   "), url(QStringLiteral("https://example.com/"))));

    // A non-HTTP scheme has no origin a cookie could belong to.
    QVERIFY(!jar.store(QStringLiteral("a=b"), url(QStringLiteral("file:///tmp/x.html"))));
    QVERIFY(!jar.store(QStringLiteral("a=b"), url(QStringLiteral("about:home"))));

    QCOMPARE(jar.count(), 0);
}

void CookiesTest::keepsAValueContainingEquals()
{
    // A base64 value ends in "=", and splitting on the first equals only is what
    // keeps it intact.
    CookieJar jar;
    QVERIFY(jar.store(QStringLiteral("token=YWJj=="), url(QStringLiteral("https://example.com/"))));
    QCOMPARE(jar.cookiesFor(url(QStringLiteral("https://example.com/"))).first().value,
             QStringLiteral("YWJj=="));
}

void CookiesTest::refusesANameItCannotSerialise()
{
    CookieJar jar;

    // A semicolon ends the cookie, so "bad=va;lue" is the cookie "bad=va" with a
    // trailing attribute "lue". Storing it is correct: a browser reads exactly
    // this header the same way, and refusing it would drop a cookie a server
    // legitimately sent.
    QVERIFY(jar.store(QStringLiteral("bad=va;lue"), url(QStringLiteral("https://example.com/"))));
    QCOMPARE(jar.count(), 1);
    QCOMPARE(jar.all().first().name, QStringLiteral("bad"));
    QCOMPARE(jar.all().first().value, QStringLiteral("va"));

    // A name that cannot be written into a request header is refused, because
    // emitting it would produce a request the server would misread.
    jar.clear();
    QVERIFY(!jar.store(QStringLiteral("bad name=1"), url(QStringLiteral("https://example.com/"))));
    QCOMPARE(jar.count(), 0);
}

// ------------------------------------------------------------ domain scoping

void CookiesTest::defaultsTheDomainToTheRequestHost()
{
    CookieJar jar;
    jar.store(QStringLiteral("a=1"), url(QStringLiteral("https://www.example.com/page")));

    const Cookie cookie = jar.all().first();
    QCOMPARE(cookie.domain, QStringLiteral("www.example.com"));

    // Without an explicit Domain the cookie is host-only, so it is not sent to a
    // sibling host.
    QCOMPARE(jar.cookiesFor(url(QStringLiteral("https://www.example.com/"))).size(), 1);
    QCOMPARE(jar.cookiesFor(url(QStringLiteral("https://other.example.com/"))).size(), 0);
    QCOMPARE(jar.cookiesFor(url(QStringLiteral("https://example.com/"))).size(), 0);
}

void CookiesTest::acceptsADomainSuffix()
{
    CookieJar jar;
    // A leading dot is the older spelling of the same thing and is stripped.
    QVERIFY(jar.store(QStringLiteral("a=1; Domain=.example.com"),
                      url(QStringLiteral("https://www.example.com/"))));

    QCOMPARE(jar.all().first().domain, QStringLiteral("example.com"));

    // A cookie scoped to the parent domain reaches every subdomain.
    QCOMPARE(jar.cookiesFor(url(QStringLiteral("https://www.example.com/"))).size(), 1);
    QCOMPARE(jar.cookiesFor(url(QStringLiteral("https://api.example.com/"))).size(), 1);
    QCOMPARE(jar.cookiesFor(url(QStringLiteral("https://example.com/"))).size(), 1);
}

void CookiesTest::refusesAnUnrelatedDomain()
{
    CookieJar jar;

    // This is the attack the rule exists for: a page at one host trying to set a
    // cookie that another host would then send. The cookie is refused outright
    // rather than narrowed, so the page cannot be surprised by a cookie it did
    // not ask for either.
    QVERIFY(!jar.store(QStringLiteral("a=1; Domain=example.com"),
                       url(QStringLiteral("https://evil.test/"))));
    QVERIFY(!jar.store(QStringLiteral("a=1; Domain=com"),
                       url(QStringLiteral("https://evil.test/"))));
    QVERIFY(!jar.store(QStringLiteral("a=1; Domain=example.com.evil.test"),
                       url(QStringLiteral("https://evil.test/"))));

    QCOMPARE(jar.count(), 0);
}

void CookiesTest::refusesASuffixThatIsNotOnAHostBoundary()
{
    // "evilexample.com" ends with "example.com" as a string but is a different
    // site, so a suffix match has to be on a dot boundary.
    QVERIFY(!CookieJar::domainMatches(QStringLiteral("evilexample.com"),
                                      QStringLiteral("example.com")));
    QVERIFY(CookieJar::domainMatches(QStringLiteral("www.example.com"),
                                     QStringLiteral("example.com")));
    QVERIFY(CookieJar::domainMatches(QStringLiteral("example.com"),
                                     QStringLiteral("example.com")));

    // Case is not significant in a host name.
    QVERIFY(CookieJar::domainMatches(QStringLiteral("WWW.Example.COM"),
                                     QStringLiteral("example.com")));
}

void CookiesTest::refusesADomainClaimOnAnIpAddress()
{
    // A suffix of an address is not a domain, so the exact-match-only rule
    // applies. Without it "1.2.3.4" would match a cookie scoped to "2.3.4".
    QVERIFY(!CookieJar::domainAcceptableFrom(QStringLiteral("0.0.1"),
                                             url(QStringLiteral("http://127.0.0.1/"))));
    QVERIFY(CookieJar::domainAcceptableFrom(QStringLiteral("127.0.0.1"),
                                            url(QStringLiteral("http://127.0.0.1/"))));

    CookieJar jar;
    QVERIFY(!jar.store(QStringLiteral("a=1; Domain=0.0.1"),
                       url(QStringLiteral("http://127.0.0.1/"))));
    QCOMPARE(jar.count(), 0);
}

// -------------------------------------------------------------- path scoping

void CookiesTest::defaultsThePathToTheRequestDirectory()
{
    CookieJar jar;
    // RFC 6265 §5.1.4: the default path is the directory of the request path,
    // so a cookie set by /docs/page is scoped to /docs and not to /.
    jar.store(QStringLiteral("a=1"), url(QStringLiteral("https://example.com/docs/page.html")));
    QCOMPARE(jar.all().first().path, QStringLiteral("/docs"));

    jar.clear();
    jar.store(QStringLiteral("b=2"), url(QStringLiteral("https://example.com/page.html")));
    QCOMPARE(jar.all().first().path, QStringLiteral("/"));

    jar.clear();
    jar.store(QStringLiteral("c=3"), url(QStringLiteral("https://example.com/")));
    QCOMPARE(jar.all().first().path, QStringLiteral("/"));

    // An explicit Path is honoured when it is absolute.
    jar.clear();
    jar.store(QStringLiteral("d=4; Path=/admin"), url(QStringLiteral("https://example.com/x/y")));
    QCOMPARE(jar.all().first().path, QStringLiteral("/admin"));
}

void CookiesTest::matchesOnlyOnAPathBoundary()
{
    // A cookie for /docs must not reach /docsomething.
    QVERIFY(CookieJar::pathMatches(QStringLiteral("/docs/page"), QStringLiteral("/docs")));
    QVERIFY(CookieJar::pathMatches(QStringLiteral("/docs"), QStringLiteral("/docs")));
    QVERIFY(CookieJar::pathMatches(QStringLiteral("/docs/"), QStringLiteral("/docs")));
    QVERIFY(!CookieJar::pathMatches(QStringLiteral("/docsomething"), QStringLiteral("/docs")));

    // The root path matches everything.
    QVERIFY(CookieJar::pathMatches(QStringLiteral("/anything"), QStringLiteral("/")));
}

// ------------------------------------------------------------------ sending

void CookiesTest::sendsOnlyMatchingCookies()
{
    CookieJar jar;
    jar.store(QStringLiteral("site=1; Domain=example.com; Path=/"),
              url(QStringLiteral("https://www.example.com/")));
    jar.store(QStringLiteral("admin=1; Path=/admin"), url(QStringLiteral("https://www.example.com/admin")));
    jar.store(QStringLiteral("other=1"), url(QStringLiteral("https://other.test/")));

    QCOMPARE(jar.cookiesFor(url(QStringLiteral("https://www.example.com/"))).size(), 1);
    QCOMPARE(jar.cookiesFor(url(QStringLiteral("https://www.example.com/admin/x"))).size(), 2);
    QCOMPARE(jar.cookiesFor(url(QStringLiteral("https://api.example.com/"))).size(), 1);
    QCOMPARE(jar.cookiesFor(url(QStringLiteral("https://other.test/"))).size(), 1);
}

void CookiesTest::ordersByPathLength()
{
    CookieJar jar;
    jar.store(QStringLiteral("a=root; Path=/"), url(QStringLiteral("https://example.com/")));
    jar.store(QStringLiteral("a=deep; Path=/one/two"), url(QStringLiteral("https://example.com/")));
    jar.store(QStringLiteral("a=middle; Path=/one"), url(QStringLiteral("https://example.com/")));

    const QList<Cookie> cookies = jar.cookiesFor(url(QStringLiteral("https://example.com/one/two/x")));
    QCOMPARE(cookies.size(), 3);

    // A server that sees two cookies with the same name reads the first, so the
    // most specific path has to come first for the ordering to mean anything.
    QCOMPARE(cookies.at(0).path, QStringLiteral("/one/two"));
    QCOMPARE(cookies.at(1).path, QStringLiteral("/one"));
    QCOMPARE(cookies.at(2).path, QStringLiteral("/"));
}

void CookiesTest::replacesACookieOfTheSameNameDomainAndPath()
{
    CookieJar jar;
    jar.store(QStringLiteral("a=first"), url(QStringLiteral("https://example.com/")));
    jar.store(QStringLiteral("a=second"), url(QStringLiteral("https://example.com/")));

    // The identity of a cookie is its name, domain and path, so a second one
    // with the same three replaces the first.
    QCOMPARE(jar.count(), 1);
    QCOMPARE(jar.cookiesFor(url(QStringLiteral("https://example.com/"))).first().value,
             QStringLiteral("second"));
}

void CookiesTest::keepsTwoCookiesWithDifferentPaths()
{
    CookieJar jar;
    jar.store(QStringLiteral("a=one; Path=/one"), url(QStringLiteral("https://example.com/")));
    jar.store(QStringLiteral("a=two; Path=/two"), url(QStringLiteral("https://example.com/")));

    QCOMPARE(jar.count(), 2);
}

// -------------------------------------------------------- security attributes

void CookiesTest::neverSendsASecureCookieOverHttp()
{
    CookieJar jar;
    QVERIFY(jar.store(QStringLiteral("token=secret; Secure"),
                      url(QStringLiteral("https://example.com/"))));

    // This is the rule that keeps a session token off a plaintext connection, so
    // it is asserted in both directions.
    QCOMPARE(jar.cookiesFor(url(QStringLiteral("https://example.com/"))).size(), 1);
    QCOMPARE(jar.cookiesFor(url(QStringLiteral("http://example.com/"))).size(), 0);

    // It is still in the jar: refusing to send it is not refusing to store it.
    QCOMPARE(jar.count(), 1);
}

void CookiesTest::sendsASecureCookieOverHttps()
{
    CookieJar jar;
    jar.store(QStringLiteral("a=1; Secure"), url(QStringLiteral("https://example.com/")));

    const QList<Cookie> cookies = jar.cookiesFor(url(QStringLiteral("https://example.com/deep/path")));
    QCOMPARE(cookies.size(), 1);
    QVERIFY(cookies.first().secure);
}

void CookiesTest::recordsHttpOnly()
{
    CookieJar jar;
    jar.store(QStringLiteral("sid=1; HttpOnly"), url(QStringLiteral("https://example.com/")));

    // The flag is recorded and enforced by there being no cookie API for script
    // at all: `document.cookie` does not exist in this engine.
    QVERIFY(jar.all().first().httpOnly);
}

void CookiesTest::recordsSameSite()
{
    CookieJar jar;
    jar.store(QStringLiteral("a=1; SameSite=Strict"), url(QStringLiteral("https://example.com/")));
    QCOMPARE(jar.all().first().sameSite, SameSite::Strict);

    jar.clear();
    jar.store(QStringLiteral("a=1; SameSite=Lax"), url(QStringLiteral("https://example.com/")));
    QCOMPARE(jar.all().first().sameSite, SameSite::Lax);

    jar.clear();
    jar.store(QStringLiteral("a=1; SameSite=None; Secure"),
              url(QStringLiteral("https://example.com/")));
    QCOMPARE(jar.all().first().sameSite, SameSite::None);

    // An absent attribute is recorded as such; treating it as Lax is a sending
    // decision made by the policy, not a stored one.
    jar.clear();
    jar.store(QStringLiteral("a=1"), url(QStringLiteral("https://example.com/")));
    QCOMPARE(jar.all().first().sameSite, SameSite::Unspecified);

    // The attribute name is case-insensitive, as all header tokens are.
    jar.clear();
    jar.store(QStringLiteral("a=1; samesite=strict"), url(QStringLiteral("https://example.com/")));
    QCOMPARE(jar.all().first().sameSite, SameSite::Strict);
}

// ------------------------------------------------------------------- expiry

void CookiesTest::ignoresAnExpiredCookie()
{
    CookieJar jar;
    // A cookie set with a date in the past is already expired. The return value
    // reports whether the jar changed, and with nothing to delete it has not, so
    // false is correct here even though the header was perfectly valid.
    QVERIFY(!jar.store(QStringLiteral("a=1; Expires=Thu, 01 Jan 1970 00:00:00 GMT"),
                       url(QStringLiteral("https://example.com/"))));

    QCOMPARE(jar.count(), 0);
    QCOMPARE(jar.cookiesFor(url(QStringLiteral("https://example.com/"))).size(), 0);

    // The same header against an existing cookie does change the jar, because it
    // removes it. That is what makes a logout work.
    jar.store(QStringLiteral("a=1"), url(QStringLiteral("https://example.com/")));
    QCOMPARE(jar.count(), 1);
    QVERIFY(jar.store(QStringLiteral("a=1; Expires=Thu, 01 Jan 1970 00:00:00 GMT"),
                      url(QStringLiteral("https://example.com/"))));
    QCOMPARE(jar.count(), 0);
}

void CookiesTest::removalDeletesTheCookie()
{
    CookieJar jar;
    jar.store(QStringLiteral("a=1"), url(QStringLiteral("https://example.com/")));
    QCOMPARE(jar.count(), 1);

    // A logout is a Set-Cookie with the same name and an immediate expiry. The
    // domain and path have to match the original or it would create a second
    // cookie instead of removing the first.
    QVERIFY(jar.store(QStringLiteral("a=; Expires=Thu, 01 Jan 1970 00:00:00 GMT"),
                      url(QStringLiteral("https://example.com/"))));
    QCOMPARE(jar.count(), 0);

    // Max-Age=0 is the other spelling of the same thing.
    jar.store(QStringLiteral("b=1"), url(QStringLiteral("https://example.com/")));
    QCOMPARE(jar.count(), 1);
    jar.store(QStringLiteral("b=; Max-Age=0"), url(QStringLiteral("https://example.com/")));
    QCOMPARE(jar.count(), 0);
}

void CookiesTest::maxAgeWinsOverExpires()
{
    // A stale Expires beside a live Max-Age is common on the wire: the server
    // means the Max-Age.
    CookieJar jar;
    jar.store(QStringLiteral("a=1; Expires=Thu, 01 Jan 1970 00:00:00 GMT; Max-Age=3600"),
              url(QStringLiteral("https://example.com/")));

    QCOMPARE(jar.count(), 1);
    QCOMPARE(jar.cookiesFor(url(QStringLiteral("https://example.com/"))).size(), 1);
    QVERIFY(jar.all().first().persistent);
}

void CookiesTest::sessionCookieIsNotExpired()
{
    CookieJar jar;
    jar.store(QStringLiteral("a=1"), url(QStringLiteral("https://example.com/")));

    const Cookie cookie = jar.all().first();
    QVERIFY(!cookie.persistent);
    // A session cookie survives for the run, which is what "session" means here.
    QVERIFY(!cookie.isExpired(QDateTime::currentDateTime().addYears(1)));
}

void CookiesTest::prunesExpiredCookies()
{
    CookieJar jar;
    jar.store(QStringLiteral("keep=1"), url(QStringLiteral("https://example.com/")));
    jar.store(QStringLiteral("gone=1; Max-Age=60"), url(QStringLiteral("https://example.com/")));
    QCOMPARE(jar.count(), 2);

    // Pruning at a time past the expiry removes only the expired one.
    const int removed = jar.pruneExpired(QDateTime::currentDateTime().addSecs(120));
    QCOMPARE(removed, 1);
    QCOMPARE(jar.count(), 1);
    QCOMPARE(jar.all().first().name, QStringLiteral("keep"));
}

// --------------------------------------------------------------- whole header

void CookiesTest::storesEverySetCookieHeader()
{
    network::HeaderList headers;
    // A response carries one header per cookie, which is why they are not joined
    // with a comma: a comma is legal inside an Expires date.
    headers.append(QStringLiteral("Set-Cookie"), QStringLiteral("a=1; Path=/"));
    headers.append(QStringLiteral("Set-Cookie"), QStringLiteral("b=2; Path=/"));
    headers.append(QStringLiteral("Content-Type"), QStringLiteral("text/html"));

    CookieJar jar;
    QCOMPARE(jar.storeFromHeaders(headers, url(QStringLiteral("https://example.com/"))), 2);
    QCOMPARE(jar.count(), 2);
}

void CookiesTest::reportsTheRequestHeader()
{
    CookieJar jar;
    jar.store(QStringLiteral("a=1; Path=/"), url(QStringLiteral("https://example.com/")));
    jar.store(QStringLiteral("b=2; Path=/deep"), url(QStringLiteral("https://example.com/")));

    // The header names the most specific cookie first.
    QCOMPARE(jar.requestHeader(url(QStringLiteral("https://example.com/deep/page"))),
             QStringLiteral("b=2; a=1"));

    // Nothing to send is an empty string rather than a stray separator.
    QCOMPARE(jar.requestHeader(url(QStringLiteral("https://other.test/"))), QString());

    CookieJar empty;
    QCOMPARE(empty.requestHeader(url(QStringLiteral("https://example.com/"))), QString());
}

QTEST_MAIN(CookiesTest)
#include "tst_cookies.moc"
