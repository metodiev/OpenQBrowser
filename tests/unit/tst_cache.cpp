#include <QtTest>

#include "network/Cache.h"
#include "network/HttpMessage.h"

using namespace oqb;
using oqb::network::HeaderList;

/// Tests for the cache's freshness arithmetic.
///
/// The policy is a pure function of headers, so it is tested as one: no socket,
/// no loader, no clock. That matters because the mistakes available here are
/// arithmetic ones - a lifetime measured from the wrong instant, an unparseable
/// date read as the epoch, a `max-age` of zero treated as absent - and each of
/// them shows up as a page that either re-downloads everything or serves
/// something it should have asked about.
class CacheTest : public QObject
{
    Q_OBJECT

private slots:
    void parsesMaxAge();
    void ignoresUnrelatedDirectives();
    void readsNoStore();
    void readsNoCache();
    void readsMustRevalidate();
    void combinesSeveralHeaderValues();
    void ignoresSharedCacheDirectives();
    void ignoresMalformedMaxAge();

    void refusesToStoreNoStore();
    void refusesToStoreErrors();
    void refusesToStorePartialAndNotModified();
    void storesAnOrdinaryResponse();

    void maxAgeWinsOverExpires();
    void computesLifetimeFromExpiresAndDate();
    void clampsAnExpiredToZero();
    void usesLastModifiedHeuristic();
    void capsTheHeuristic();
    void returnsZeroWhenNothingIsKnown();

    void parsesImfFixdate();
    void parsesRfc850TwoDigitYear();
    void parsesAsctime();
    void rejectsGarbageDates();
    void treatsAnEmptyDateAsAbsent();
    void roundTripsAFormattedDate();

    void acceptsHarmlessVary();
    void refusesVaryOnAnUnkeyedHeader();

    void agesAResponseFromItsDate();
    void isFreshOnlyWithinItsLifetime();
    void noCacheIsNeverFresh();
};

// --------------------------------------------------------------- directives

void CacheTest::parsesMaxAge()
{
    HeaderList headers;
    headers.append(QStringLiteral("Cache-Control"), QStringLiteral("max-age=300"));

    const auto directives = network::CachePolicy::parse(headers);
    QCOMPARE(directives.maxAgeSeconds, 300);
    QVERIFY(!directives.noStore);
    QVERIFY(!directives.noCache);
}

void CacheTest::ignoresUnrelatedDirectives()
{
    // `immutable` and `stale-while-revalidate` are optional extensions. Ignoring
    // them is correct; treating one as `no-store` by accident would not be.
    HeaderList headers;
    headers.append(QStringLiteral("Cache-Control"),
                   QStringLiteral("immutable, stale-while-revalidate=60"));

    const auto directives = network::CachePolicy::parse(headers);
    QCOMPARE(directives.maxAgeSeconds, -1);
    QVERIFY(!directives.noStore);
    QVERIFY(!directives.noCache);
    QVERIFY(!directives.mustRevalidate);
}

void CacheTest::readsNoStore()
{
    HeaderList headers;
    headers.append(QStringLiteral("Cache-Control"), QStringLiteral("no-store"));

    const auto directives = network::CachePolicy::parse(headers);
    QVERIFY(directives.noStore);
    QVERIFY(!network::CachePolicy::isStorable(directives, 200));
}

void CacheTest::readsNoCache()
{
    // A bare `no-cache` means "store, but always revalidate" - not "do not
    // store". Confusing the two is the classic cache bug: it either loses the
    // benefit of storing or skips the revalidation the server asked for.
    HeaderList headers;
    headers.append(QStringLiteral("Cache-Control"), QStringLiteral("no-cache"));

    const auto directives = network::CachePolicy::parse(headers);
    QVERIFY(directives.noCache);
    QVERIFY(!directives.noStore);
    QVERIFY2(network::CachePolicy::isStorable(directives, 200),
             "no-cache must still be storable, or revalidation can never happen");
}

void CacheTest::readsMustRevalidate()
{
    HeaderList headers;
    headers.append(QStringLiteral("Cache-Control"),
                   QStringLiteral("max-age=0, must-revalidate"));

    const auto directives = network::CachePolicy::parse(headers);
    QVERIFY(directives.mustRevalidate);
    QCOMPARE(directives.maxAgeSeconds, 0);
}

void CacheTest::combinesSeveralHeaderValues()
{
    // Two Cache-Control headers are equivalent to one comma-joined list, so a
    // no-store hiding in the second must still be seen.
    HeaderList headers;
    headers.append(QStringLiteral("Cache-Control"), QStringLiteral("max-age=60"));
    headers.append(QStringLiteral("Cache-Control"), QStringLiteral("no-store"));

    const auto directives = network::CachePolicy::parse(headers);
    QVERIFY(directives.noStore);
    QCOMPARE(directives.maxAgeSeconds, 60);
}

void CacheTest::ignoresSharedCacheDirectives()
{
    // s-maxage addresses shared caches. This cache serves one user, so honouring
    // it would apply a proxy's freshness rule to a private cache.
    HeaderList headers;
    headers.append(QStringLiteral("Cache-Control"), QStringLiteral("s-maxage=600"));
    QCOMPARE(network::CachePolicy::parse(headers).maxAgeSeconds, -1);

    // `private` is about shared caches too, and must not be read as no-store:
    // the response is exactly the kind a private cache is allowed to keep.
    HeaderList privateHeaders;
    privateHeaders.append(QStringLiteral("Cache-Control"), QStringLiteral("private, max-age=60"));
    const auto directives = network::CachePolicy::parse(privateHeaders);
    QVERIFY(directives.isPrivate);
    QVERIFY(!directives.noStore);
    QVERIFY(network::CachePolicy::isStorable(directives, 200));
}

void CacheTest::ignoresMalformedMaxAge()
{
    // A value that is not a number must not be read as zero. Zero means "always
    // revalidate", which is a behaviour the server did not ask for; absent means
    // "fall through to Expires", which is what it did ask for.
    for (const QString &value : {QStringLiteral("max-age=abc"),
                                 QStringLiteral("max-age="),
                                 QStringLiteral("max-age=-5"),
                                 QStringLiteral("max-age")}) {
        HeaderList headers;
        headers.append(QStringLiteral("Cache-Control"), value);
        QCOMPARE(network::CachePolicy::parse(headers).maxAgeSeconds, -1);
    }
}

// --------------------------------------------------------------- storability

void CacheTest::refusesToStoreNoStore()
{
    network::CachePolicy::Directives directives;
    directives.noStore = true;
    QVERIFY(!network::CachePolicy::isStorable(directives, 200));
}

void CacheTest::refusesToStoreErrors()
{
    const network::CachePolicy::Directives directives;
    // A cached error outlives the outage that produced it, so a server that
    // recovers would still be reported as broken.
    QVERIFY(!network::CachePolicy::isStorable(directives, 404));
    QVERIFY(!network::CachePolicy::isStorable(directives, 500));
    QVERIFY(!network::CachePolicy::isStorable(directives, 503));
}

void CacheTest::refusesToStorePartialAndNotModified()
{
    const network::CachePolicy::Directives directives;
    // 206 is part of a body, and 304 is a revalidation result that gets merged
    // into the entry it validates rather than stored in place of it.
    QVERIFY(!network::CachePolicy::isStorable(directives, 206));
    QVERIFY(!network::CachePolicy::isStorable(directives, 304));
}

void CacheTest::storesAnOrdinaryResponse()
{
    const network::CachePolicy::Directives directives;
    QVERIFY(network::CachePolicy::isStorable(directives, 200));
}

// ---------------------------------------------------------------- freshness

void CacheTest::maxAgeWinsOverExpires()
{
    // When both are present, Cache-Control is the more recent standard and wins.
    network::CachePolicy::Directives directives;
    directives.maxAgeSeconds = 300;

    const QDateTime now = QDateTime::currentDateTimeUtc();
    QCOMPARE(network::CachePolicy::freshnessLifetime(directives, now, now.addSecs(9999),
                                                     QDateTime()),
             300);
}

void CacheTest::computesLifetimeFromExpiresAndDate()
{
    // RFC 9111 section 4.2.1: the lifetime is `Expires - Date`, not
    // `Expires - now`. Using now would give a different answer every time it was
    // asked, and would be wrong by the age of the response.
    const QDateTime date = QDateTime::fromString(QStringLiteral("2026-01-01T00:00:00Z"),
                                                 Qt::ISODate);
    const QDateTime expires = date.addSecs(600);

    const network::CachePolicy::Directives directives;
    QCOMPARE(network::CachePolicy::freshnessLifetime(directives, date, expires, QDateTime()),
             600);

    // A Date that is later than the Expires yields a negative lifetime, which
    // must be clamped rather than returned: a negative freshness would make the
    // staleness test depend on the sign of a subtraction.
    QCOMPARE(network::CachePolicy::freshnessLifetime(directives, expires.addSecs(100), expires,
                                                     QDateTime()),
             0);
}

void CacheTest::clampsAnExpiredToZero()
{
    const QDateTime date = QDateTime::fromString(QStringLiteral("2026-01-01T00:00:00Z"),
                                                 Qt::ISODate);
    const network::CachePolicy::Directives directives;

    // Already in the past relative to its own Date: stale immediately, not
    // negative.
    QCOMPARE(network::CachePolicy::freshnessLifetime(directives, date, date.addSecs(-60),
                                                     QDateTime()),
             0);
}

void CacheTest::usesLastModifiedHeuristic()
{
    const QDateTime date = QDateTime::fromString(QStringLiteral("2026-01-01T00:00:00Z"),
                                                 Qt::ISODate);
    const network::CachePolicy::Directives directives;

    // A tenth of the time since the document last changed, which is the
    // heuristic the specification permits when nothing else is given.
    const QDateTime modified = date.addSecs(-1000);
    QCOMPARE(network::CachePolicy::freshnessLifetime(directives, date, QDateTime(), modified),
             100);
}

void CacheTest::capsTheHeuristic()
{
    const QDateTime date = QDateTime::fromString(QStringLiteral("2026-01-01T00:00:00Z"),
                                                 Qt::ISODate);
    const network::CachePolicy::Directives directives;

    // A file untouched for a year would otherwise be fresh for a month. The cap
    // keeps a heuristic from becoming a long-lived cache.
    const QDateTime ancient = date.addDays(-365);
    QCOMPARE(network::CachePolicy::freshnessLifetime(directives, date, QDateTime(), ancient),
             24 * 60 * 60);
}

void CacheTest::returnsZeroWhenNothingIsKnown()
{
    // No max-age, no Expires, no Last-Modified: the response may be stored, but
    // only used after the server confirms it.
    const network::CachePolicy::Directives directives;
    QCOMPARE(network::CachePolicy::freshnessLifetime(directives, QDateTime(), QDateTime(),
                                                     QDateTime()),
             0);
}

// -------------------------------------------------------------- HTTP dates

void CacheTest::parsesImfFixdate()
{
    // The form RFC 9110 requires a sender to produce.
    const QDateTime parsed = network::CachePolicy::parseHttpDate(
        QStringLiteral("Sun, 06 Nov 1994 08:49:37 GMT"));
    QVERIFY(parsed.isValid());
    QCOMPARE(parsed.toUTC().toString(Qt::ISODate), QStringLiteral("1994-11-06T08:49:37Z"));
}

void CacheTest::parsesRfc850TwoDigitYear()
{
    // The obsolete form servers still send. A two-digit year is read as 19xx,
    // which is what RFC 9110 says for a date more than 50 years in the past.
    const QDateTime parsed = network::CachePolicy::parseHttpDate(
        QStringLiteral("Sunday, 06-Nov-94 08:49:37 GMT"));
    QVERIFY(parsed.isValid());
    QCOMPARE(parsed.toUTC().date(), QDate(1994, 11, 6));
}

void CacheTest::parsesAsctime()
{
    const QDateTime parsed = network::CachePolicy::parseHttpDate(
        QStringLiteral("Sun Nov  6 08:49:37 1994"));
    QVERIFY(parsed.isValid());
    QCOMPARE(parsed.toUTC().toString(Qt::ISODate), QStringLiteral("1994-11-06T08:49:37Z"));
}

void CacheTest::rejectsGarbageDates()
{
    // An unparseable date must be invalid, not the epoch. Reading it as 1970
    // would make the response look ancient when the server simply sent nonsense.
    for (const QString &value : {QStringLiteral("not a date"),
                                 QStringLiteral("2026-01-01"),
                                 QStringLiteral("Sun, 99 Xxx 1994 08:49:37 GMT"),
                                 QStringLiteral("garbage,")}) {
        QVERIFY2(!network::CachePolicy::parseHttpDate(value).isValid(), qPrintable(value));
    }
}

void CacheTest::treatsAnEmptyDateAsAbsent()
{
    QVERIFY(!network::CachePolicy::parseHttpDate(QString()).isValid());
    QVERIFY(!network::CachePolicy::parseHttpDate(QStringLiteral("   ")).isValid());
}

void CacheTest::roundTripsAFormattedDate()
{
    // What the browser writes into If-Modified-Since must be what it can read
    // back, or a revalidation would never match.
    const QDateTime original = QDateTime::fromString(QStringLiteral("2026-03-04T05:06:07Z"),
                                                     Qt::ISODate);
    const QString text = network::CachePolicy::formatHttpDate(original);
    QCOMPARE(text, QStringLiteral("Wed, 04 Mar 2026 05:06:07 GMT"));
    QCOMPARE(network::CachePolicy::parseHttpDate(text), original);
}

// ---------------------------------------------------------------------- vary

void CacheTest::acceptsHarmlessVary()
{
    // This browser sends one fixed Accept-Encoding and never negotiates, so a
    // response varying by it has only one variant here.
    QVERIFY(network::CachePolicy::varyIsHarmless(QString()));
    QVERIFY(network::CachePolicy::varyIsHarmless(QStringLiteral("Accept-Encoding")));
    QVERIFY(network::CachePolicy::varyIsHarmless(QStringLiteral("accept-encoding")));
    QVERIFY(network::CachePolicy::varyIsHarmless(QStringLiteral("Accept-Encoding, ")));
}

void CacheTest::refusesVaryOnAnUnkeyedHeader()
{
    // Anything else means a response that could differ from the one stored, so
    // storing it risks serving the wrong variant.
    QVERIFY(!network::CachePolicy::varyIsHarmless(QStringLiteral("*")));
    QVERIFY(!network::CachePolicy::varyIsHarmless(QStringLiteral("Cookie")));
    QVERIFY(!network::CachePolicy::varyIsHarmless(QStringLiteral("Accept-Language")));
    QVERIFY(!network::CachePolicy::varyIsHarmless(QStringLiteral("Accept-Encoding, Cookie")));
}

// ----------------------------------------------------------------------- age

void CacheTest::agesAResponseFromItsDate()
{
    const QDateTime now = QDateTime::fromString(QStringLiteral("2026-01-01T00:10:00Z"),
                                                Qt::ISODate);

    // Stored ten minutes ago, with a Date matching that instant: ten minutes old.
    QCOMPARE(network::CachePolicy::ageSeconds(now.addSecs(-600), now.addSecs(-600), now), 600);

    // A response that was already five minutes old when it arrived is fifteen
    // minutes old, because the lifetime runs from the origin's Date. Ignoring
    // that would let a proxy-extended entry be served past its lifetime.
    QCOMPARE(network::CachePolicy::ageSeconds(now.addSecs(-600), now.addSecs(-900), now), 900);

    // A Date in the future is clock skew, not a negative age.
    QCOMPARE(network::CachePolicy::ageSeconds(now.addSecs(-600), now.addSecs(600), now), 600);
}

void CacheTest::isFreshOnlyWithinItsLifetime()
{
    // Strictly less than: at exactly the lifetime the response is stale, which is
    // what makes max-age=0 mean "always revalidate".
    QVERIFY(network::CachePolicy::isFresh(0, 60, false));
    QVERIFY(network::CachePolicy::isFresh(59, 60, false));
    QVERIFY(!network::CachePolicy::isFresh(60, 60, false));
    QVERIFY(!network::CachePolicy::isFresh(61, 60, false));

    // A lifetime of 0 is "the server did not say", so the origin is asked.
    QVERIFY(!network::CachePolicy::isFresh(0, 0, false));
}

void CacheTest::noCacheIsNeverFresh()
{
    // Even with a long lifetime, no-cache means revalidate first. A long max-age
    // must not be allowed to override it.
    QVERIFY(!network::CachePolicy::isFresh(0, 86400, true));
}

QTEST_MAIN(CacheTest)
#include "tst_cache.moc"
