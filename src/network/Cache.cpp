#include "network/Cache.h"

#include <QRegularExpression>
#include <QStringList>

namespace oqb::network {

namespace {

/// Splits a comma-separated directive list, trimming and lower-casing each token
/// and dropping empty ones.
QStringList splitDirectives(const QString &value)
{
    QStringList out;
    for (const QString &part : value.split(u',')) {
        const QString token = part.trimmed().toLower();
        if (!token.isEmpty())
            out << token;
    }
    return out;
}

/// The value of a `name=value` directive, or an empty string for a bare name.
/// A quoted value has its quotes removed, which is what servers send for
/// `private="Set-Cookie"`.
QString directiveValue(const QString &token, const QString &name)
{
    const QString prefix = name + u'=';
    if (!token.startsWith(prefix))
        return {};
    QString value = token.mid(prefix.size()).trimmed();
    if (value.size() >= 2 && value.startsWith(u'"') && value.endsWith(u'"'))
        value = value.mid(1, value.size() - 2);
    return value;
}

/// Parses a delta-seconds value. Returns -1 when it is absent or not a number,
/// which the caller reads as "the server did not say".
int deltaSeconds(const QString &value)
{
    if (value.isEmpty())
        return -1;
    bool ok = false;
    const qlonglong seconds = value.toLongLong(&ok);
    if (!ok || seconds < 0)
        return -1;
    // A value this large is indistinguishable from "never expires", and leaving
    // it unbounded would overflow the arithmetic that follows.
    return static_cast<int>(qMin<qlonglong>(seconds, 366LL * 24 * 60 * 60));
}

} // namespace

CachePolicy::Directives CachePolicy::parse(const HeaderList &headers)
{
    Directives out;

    // Several Cache-Control headers are equivalent to one comma-joined list, so
    // every one is read rather than only the first.
    QStringList tokens;
    for (const QString &value : headers.values(QStringLiteral("Cache-Control")))
        tokens += splitDirectives(value);

    for (const QString &token : tokens) {
        if (token == QLatin1String("no-store")) {
            out.noStore = true;
        } else if (token == QLatin1String("no-cache")) {
            // A bare `no-cache` means revalidate. The `no-cache="field"` form
            // means only the named fields must be revalidated, which this cache
            // cannot do without splitting a response, so it is treated as the
            // stricter bare form.
            out.noCache = true;
        } else if (token == QLatin1String("must-revalidate")
                   || token == QLatin1String("proxy-revalidate")) {
            out.mustRevalidate = true;
        } else if (token == QLatin1String("public")) {
            out.isPublic = true;
        } else if (token == QLatin1String("private")) {
            out.isPrivate = true;
        } else if (token.startsWith(QLatin1String("max-age"))) {
            const int seconds = deltaSeconds(directiveValue(token, QStringLiteral("max-age")));
            if (seconds >= 0)
                out.maxAgeSeconds = seconds;
        }
        // s-maxage, immutable and stale-while-revalidate are deliberately not
        // acted on. s-maxage addresses shared caches, which this is not, and the
        // other two only permit behaviour that is optional to implement.
    }

    return out;
}

bool CachePolicy::isStorable(const Directives &directives, int statusCode)
{
    // A response that says no-store, or answers a request that did, must not be
    // written. This is the one directive whose whole purpose is to be obeyed.
    if (directives.noStore)
        return false;

    // Only a response that carries a complete, reusable payload is worth
    // keeping. 206 is a partial body, and 304 is a revalidation result that is
    // merged into the entry it validates rather than stored on its own.
    if (statusCode == 206 || statusCode == 304)
        return false;

    // An error is not stored. A cached 500 would outlive the outage that caused
    // it and be served long after the server recovered.
    if (statusCode >= 400)
        return false;

    return true;
}

int CachePolicy::freshnessLifetime(const Directives &directives,
                                   const QDateTime &date,
                                   const QDateTime &expires,
                                   const QDateTime &lastModified)
{
    // An explicit max-age is the answer whenever it is present.
    if (directives.maxAgeSeconds >= 0)
        return directives.maxAgeSeconds;

    // Otherwise Expires, measured from the Date the server sent the response.
    // RFC 9111 section 4.2.1 defines the lifetime as `Expires - Date`, and using
    // `now` instead would make a clock skewed server's response look fresh for
    // as long as the skew.
    if (expires.isValid()) {
        const QDateTime base = date.isValid() ? date : QDateTime::currentDateTimeUtc();
        const qint64 seconds = base.secsTo(expires);
        return static_cast<int>(qBound<qint64>(0, seconds, 366LL * 24 * 60 * 60));
    }

    // With neither, the specification permits a heuristic. A tenth of the time
    // since the document last changed is the conventional choice, capped so a
    // file untouched for a year does not become fresh for a month.
    if (lastModified.isValid()) {
        const QDateTime base = date.isValid() ? date : QDateTime::currentDateTimeUtc();
        const qint64 sinceChange = lastModified.secsTo(base);
        if (sinceChange > 0)
            return static_cast<int>(qMin<qint64>(sinceChange / 10, 24 * 60 * 60));
    }

    // Nothing to go on: the response may be used, but only after asking.
    return 0;
}

QDateTime CachePolicy::parseHttpDate(const QString &value)
{
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty())
        return {};

    // IMF-fixdate is what RFC 9110 requires a sender to produce:
    //   Sun, 06 Nov 1994 08:49:37 GMT
    // Qt's RFC2822Date parser is very nearly right for it, but RFC 2822 wants a
    // numeric zone ("+0000") and rejects the literal "GMT" that HTTP mandates.
    // So the zone name is rewritten to +0000 first. Every form the specification
    // allows is in UTC, so nothing is lost by doing so.
    static const QRegularExpression zone(
        QStringLiteral(R"(\s+(?:GMT|UTC|UT|Z)\s*$)"), QRegularExpression::CaseInsensitiveOption);
    QString normalized = trimmed;
    normalized.replace(zone, QStringLiteral(" +0000"));

    const QDateTime modern = QDateTime::fromString(normalized, Qt::RFC2822Date);
    if (modern.isValid())
        return modern.toUTC();

    // RFC 850: "Sunday, 06-Nov-94 08:49:37 GMT". Two-digit years are read by Qt
    // as 19xx, which is what RFC 9110 says to do for a date more than 50 years
    // in the past, so no correction is applied.
    //
    // Each pattern is one raw string rather than several concatenated ones: a
    // raw string keeps its quote characters, so splitting one across lines puts
    // a literal `"` into the pattern and nothing matches.
    static const QRegularExpression rfc850(
        QStringLiteral(R"(^[A-Za-z]+,\s*(\d{1,2})-([A-Za-z]{3})-(\d{2,4})\s+(\d{1,2}:\d{2}:\d{2})\s*\w*$)"));
    const auto match850 = rfc850.match(trimmed);
    if (match850.hasMatch()) {
        // A two-digit year has to be expanded before Qt is asked to read it:
        // "yyyy" will not accept two digits, and RFC 9110 section 5.6.7 gives the
        // rule - read it as 19xx, unless that lands more than 50 years in the
        // future, in which case it is 20xx.
        QString year = match850.captured(3);
        if (year.size() == 2) {
            const int twoDigits = year.toInt();
            const int asNineteen = 1900 + twoDigits;
            year = QString::number(asNineteen > QDate::currentDate().year() + 50
                                       ? 2000 + twoDigits
                                       : asNineteen);
        }

        const QDateTime parsed = QDateTime::fromString(
            QStringLiteral("%1 %2 %3 %4")
                .arg(match850.captured(2), match850.captured(1), match850.captured(4), year),
            QStringLiteral("MMM d hh:mm:ss yyyy"));
        if (parsed.isValid())
            return parsed.toUTC();
    }

    // asctime: "Sun Nov  6 08:49:37 1994". The day is space-padded to two
    // columns, which Qt's format string tolerates.
    static const QRegularExpression asctime(
        QStringLiteral(R"(^[A-Za-z]{3}\s+([A-Za-z]{3})\s+(\d{1,2})\s+(\d{1,2}:\d{2}:\d{2})\s*(\d{4})$)"));
    const auto matchAsc = asctime.match(trimmed);
    if (matchAsc.hasMatch()) {
        const QDateTime parsed = QDateTime::fromString(
            QStringLiteral("%1 %2 %3 %4")
                .arg(matchAsc.captured(1), matchAsc.captured(2), matchAsc.captured(3),
                     matchAsc.captured(4)),
            QStringLiteral("MMM d hh:mm:ss yyyy"));
        if (parsed.isValid())
            return parsed.toUTC();
    }

    return {};
}

QString CachePolicy::formatHttpDate(const QDateTime &date)
{
    if (!date.isValid())
        return {};
    return date.toUTC().toString(QStringLiteral("ddd, dd MMM yyyy HH:mm:ss 'GMT'"));
}

bool CachePolicy::varyIsHarmless(const QString &vary)
{
    const QString trimmed = vary.trimmed().toLower();
    if (trimmed.isEmpty())
        return true;

    // `Vary: *` and `Vary: Cookie` both mean the response depends on something
    // this cache does not key on, so storing it would risk serving one variant
    // to a request that wanted another. Accept-Encoding is the one header worth
    // ignoring, because this browser sends a single fixed value and never
    // negotiates anything else.
    for (const QString &field : trimmed.split(u',')) {
        const QString name = field.trimmed();
        if (name.isEmpty() || name == QLatin1String("accept-encoding"))
            continue;
        return false;
    }
    return true;
}

qint64 CachePolicy::ageSeconds(const QDateTime &storedAt, const QDateTime &date,
                               const QDateTime &now)
{
    if (!storedAt.isValid())
        return 0;

    // The age is measured from when the response was generated, which the Date
    // header states, not from when it arrived. A proxy may have held it for a
    // while, so a Date in the past means part of the lifetime was already spent
    // before this browser ever saw the response.
    qint64 age = storedAt.secsTo(now);
    if (date.isValid())
        age += qMax<qint64>(0, -storedAt.secsTo(date));

    return qMax<qint64>(0, age);
}

bool CachePolicy::isFresh(qint64 age, int freshnessLifetimeSeconds, bool noCache)
{
    // `no-cache` means the response must be confirmed with the server before
    // every reuse, however long its lifetime. Checking it first keeps that
    // instruction from being overridden by a long max-age.
    if (noCache)
        return false;

    // A lifetime of 0 means "no freshness information", which is not the same as
    // "fresh for zero seconds by accident": either way the answer is that the
    // origin has to be asked.
    if (freshnessLifetimeSeconds <= 0)
        return false;

    return age < freshnessLifetimeSeconds;
}

} // namespace oqb::network
