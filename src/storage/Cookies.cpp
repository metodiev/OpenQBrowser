#include "storage/Cookies.h"

#include <QRegularExpression>
#include <QStringList>

#include <algorithm>

namespace oqb::storage {

namespace {

/// Splits a Set-Cookie value into its parts. The separator is always a
/// semicolon, and a semicolon cannot appear inside a cookie value unquoted, so
/// this is a plain split rather than a state machine.
QStringList splitAttributes(const QString &value)
{
    return value.split(u';');
}

/// Reads a cookie attribute by name, returning the trimmed text after the first
/// "=" or an empty string when the attribute is absent.
QString attributeValue(const QStringList &parts, const QString &name)
{
    for (const QString &part : parts) {
        const QString trimmed = part.trimmed();
        const int equals = trimmed.indexOf(u'=');
        const QString key = (equals < 0 ? trimmed : trimmed.left(equals)).trimmed().toLower();
        if (key == name)
            return equals < 0 ? QString() : trimmed.mid(equals + 1).trimmed();
    }
    return {};
}

bool hasFlag(const QStringList &parts, const QString &name)
{
    for (const QString &part : parts) {
        if (part.trimmed().toLower() == name)
            return true;
    }
    return false;
}

/// The date formats an `Expires` attribute may use.
///
/// The specification names exactly one, but servers in the wild send half a
/// dozen variants, and a browser that only accepted the specified format would
/// treat a persistent cookie as a session cookie. The list is ordered
/// most-specific first.
const QList<QString> &expiryFormats()
{
    static const QList<QString> formats = {
        QStringLiteral("ddd, dd MMM yyyy HH:mm:ss 'GMT'"),
        QStringLiteral("dddd, dd-MMM-yy HH:mm:ss 'GMT'"),
        QStringLiteral("ddd MMM d HH:mm:ss yyyy"),
        QStringLiteral("ddd, dd-MMM-yyyy HH:mm:ss 'GMT'"),
        QStringLiteral("ddd, dd MMM yyyy HH:mm:ss t"),
        QStringLiteral("yyyy-MM-dd'T'HH:mm:ss'Z'"),
    };
    return formats;
}

} // namespace

bool Cookie::isExpired(const QDateTime &now) const
{
    // A session cookie has no expiry, so it is never expired during a run.
    if (!persistent)
        return false;
    return expires.isValid() && expires <= now;
}

QString Cookie::describe() const
{
    QStringList traits;
    if (secure)
        traits << QStringLiteral("Secure");
    if (httpOnly)
        traits << QStringLiteral("HttpOnly");

    switch (sameSite) {
    case SameSite::Strict: traits << QStringLiteral("SameSite=Strict"); break;
    case SameSite::Lax: traits << QStringLiteral("SameSite=Lax"); break;
    case SameSite::None: traits << QStringLiteral("SameSite=None"); break;
    case SameSite::Unspecified: break;
    }

    traits << (persistent ? expires.toString(QStringLiteral("yyyy-MM-dd HH:mm"))
                          : QStringLiteral("session"));

    return QStringLiteral("%1=%2; domain=%3; path=%4; %5")
        .arg(name, value, domain, path, traits.join(QStringLiteral("; ")));
}

// ----------------------------------------------------------------- matching

bool CookieJar::domainMatches(const QString &host, const QString &domain)
{
    if (host.isEmpty() || domain.isEmpty())
        return false;

    // Host names are case-insensitive.
    const QString lowerHost = host.toLower();
    const QString lowerDomain = domain.toLower();

    if (lowerHost == lowerDomain)
        return true;

    // A suffix match is only valid on a host boundary, so that "evilexample.com"
    // does not match a cookie scoped to "example.com". This is the check that
    // makes the rule a security boundary rather than a string search.
    return lowerHost.endsWith(QLatin1Char('.') + lowerDomain);
}

bool CookieJar::pathMatches(const QString &requestPath, const QString &cookiePath)
{
    if (cookiePath.isEmpty())
        return true;

    const QString path = requestPath.isEmpty() ? QStringLiteral("/") : requestPath;
    if (path == cookiePath)
        return true;

    if (!path.startsWith(cookiePath))
        return false;

    // The prefix only counts when it ends on a path boundary, so a cookie for
    // "/foo" is not sent to "/foobar".
    if (cookiePath.endsWith(u'/'))
        return true;
    return path.size() > cookiePath.size()
        && path.at(cookiePath.size()) == QLatin1Char('/');
}

QString CookieJar::defaultPathFor(const network::Url &url)
{
    const QString path = url.path();
    if (path.isEmpty() || !path.startsWith(u'/'))
        return QStringLiteral("/");

    const int lastSlash = path.lastIndexOf(u'/');
    if (lastSlash <= 0)
        return QStringLiteral("/");

    return path.left(lastSlash);
}

bool CookieJar::domainAcceptableFrom(const QString &domain, const network::Url &url)
{
    const QString host = url.host().toLower();
    const QString candidate = domain.toLower();

    if (candidate.isEmpty() || host.isEmpty())
        return false;

    // A cookie may be scoped to the exact host, or to a suffix of it. It may not
    // be scoped to a suffix that is not its own - that is the rule that stops a
    // page at "evil.com" claiming "com", or "example.com.evil.com" claiming
    // "evil.com" and reading its cookies.
    if (candidate == host)
        return true;

    // A literal IP address may only be matched exactly: a suffix of an address is
    // not a domain.
    static const QRegularExpression ipv4(QStringLiteral("^\\d{1,3}(\\.\\d{1,3}){3}$"));
    if (ipv4.match(host).hasMatch() || host.contains(u':'))
        return false;

    return host.endsWith(QLatin1Char('.') + candidate);
}

// ------------------------------------------------------------------ storing

bool CookieJar::store(const QString &setCookieValue, const network::Url &url)
{
    if (setCookieValue.trimmed().isEmpty() || !url.isValid())
        return false;

    // Only a scheme that can carry cookies may set one. `isRemote()` is the test
    // rather than `isHttp()`, because the latter is true only for the plain
    // scheme - testing it alone would refuse every cookie an HTTPS site sets.
    // A `file://` or `data:` document has no origin a cookie could belong to.
    if (!url.isRemote())
        return false;

    const QStringList parts = splitAttributes(setCookieValue);

    // The first part is "name=value"; the rest are attributes.
    const QString first = parts.value(0).trimmed();
    const int equals = first.indexOf(u'=');
    if (equals <= 0)
        return false; // No name, so this is not a cookie.

    Cookie cookie;
    cookie.name = first.left(equals).trimmed();
    cookie.value = first.mid(equals + 1).trimmed();
    cookie.created = QDateTime::currentDateTime();

    // A name or value containing these characters cannot be represented in the
    // header this browser writes, so the cookie is refused rather than emitted in
    // a form a server would misread.
    static const QRegularExpression forbidden(QStringLiteral("[\\x00-\\x20\\x7f;,=\"\\\\]"));
    if (cookie.name.isEmpty() || forbidden.match(cookie.name).hasMatch())
        return false;

    // ------------------------------------------------------------ domain
    //
    // An explicit Domain is honoured only when it is acceptable for the host that
    // set it; otherwise the cookie is refused, as the specification requires. A
    // refused cookie is not silently narrowed to the host, because that would let
    // a page set a cookie it asked not to be host-only.
    const QString domainAttribute = attributeValue(parts, QStringLiteral("domain"));
    if (!domainAttribute.isEmpty()) {
        const QString cleaned = domainAttribute.startsWith(u'.') ? domainAttribute.mid(1)
                                                                 : domainAttribute;
        if (!domainAcceptableFrom(cleaned, url))
            return false;
        cookie.domain = cleaned.toLower();
    } else {
        cookie.domain = url.host().toLower();
    }

    // ------------------------------------------------------------- path
    const QString pathAttribute = attributeValue(parts, QStringLiteral("path"));
    if (!pathAttribute.isEmpty() && pathAttribute.startsWith(u'/'))
        cookie.path = pathAttribute;
    else
        cookie.path = defaultPathFor(url);

    // ------------------------------------------------------------ expiry
    //
    // Max-Age wins over Expires when both are present, which is the rule that
    // makes a session cookie out of a stale Expires date.
    const QString maxAge = attributeValue(parts, QStringLiteral("max-age"));
    if (!maxAge.isEmpty()) {
        bool ok = false;
        const qint64 seconds = maxAge.toLongLong(&ok);
        if (ok) {
            cookie.persistent = true;
            // A Max-Age of zero or less means "delete now", which is how a server
            // logs a user out.
            cookie.expires = QDateTime::currentDateTime().addSecs(seconds);
        }
    }

    if (!cookie.persistent && !attributeValue(parts, QStringLiteral("expires")).isEmpty()) {
        const QString text = attributeValue(parts, QStringLiteral("expires"));
        for (const QString &format : expiryFormats()) {
            const QDateTime parsed = QDateTime::fromString(text, format);
            if (parsed.isValid()) {
                cookie.persistent = true;
                // An Expires date is always UTC on the wire, whatever zone the
                // string implied, so it is compared as UTC.
                cookie.expires = parsed.toUTC();
                break;
            }
        }
    }

    // --------------------------------------------------------- attributes
    cookie.secure = hasFlag(parts, QStringLiteral("secure"));
    cookie.httpOnly = hasFlag(parts, QStringLiteral("httponly"));

    const QString sameSite = attributeValue(parts, QStringLiteral("samesite")).toLower();
    if (sameSite == QLatin1String("strict"))
        cookie.sameSite = SameSite::Strict;
    else if (sameSite == QLatin1String("lax"))
        cookie.sameSite = SameSite::Lax;
    else if (sameSite == QLatin1String("none"))
        cookie.sameSite = SameSite::None;

    // A cookie that is no longer valid is a deletion: the server is telling the
    // jar to forget it, which is how a logout works.
    if (cookie.isExpired(QDateTime::currentDateTime())) {
        for (int i = static_cast<int>(m_cookies.size()) - 1; i >= 0; --i) {
            const Cookie &existing = m_cookies.at(i);
            if (existing.name == cookie.name && existing.domain == cookie.domain
                && existing.path == cookie.path) {
                m_cookies.removeAt(i);
                return true;
            }
        }
        return false;
    }

    upsert(cookie);
    return true;
}

void CookieJar::upsert(const Cookie &cookie)
{
    // A cookie is identified by its name, domain and path: a second cookie with
    // the same three replaces the first rather than being stored beside it.
    for (Cookie &existing : m_cookies) {
        if (existing.name == cookie.name && existing.domain == cookie.domain
            && existing.path == cookie.path) {
            // The creation time is kept, because ordering between equally
            // specific cookies is by creation and a refresh must not change it.
            const QDateTime created = existing.created;
            existing = cookie;
            existing.created = created;
            return;
        }
    }

    m_cookies.append(cookie);
}

int CookieJar::storeFromHeaders(const network::HeaderList &headers, const network::Url &url)
{
    int stored = 0;
    // A response may carry several Set-Cookie headers, and each is one cookie.
    for (const QString &value : headers.values(QStringLiteral("Set-Cookie"))) {
        if (store(value, url))
            ++stored;
    }

    if (stored > 0)
        pruneExpired();

    return stored;
}

// ------------------------------------------------------------------ sending

QList<Cookie> CookieJar::cookiesFor(const network::Url &url) const
{
    QList<Cookie> matched;
    if (!url.isValid() || !url.isRemote())
        return matched;

    const QDateTime now = QDateTime::currentDateTime();
    const QString host = url.host();
    const QString path = url.path().isEmpty() ? QStringLiteral("/") : url.path();
    const bool secure = url.isSecure();

    for (const Cookie &cookie : m_cookies) {
        if (cookie.isExpired(now))
            continue;

        if (!domainMatches(host, cookie.domain))
            continue;

        if (!pathMatches(path, cookie.path))
            continue;

        // A Secure cookie is only ever sent over a secure scheme, which is what
        // stops it being read by an attacker who can observe plaintext traffic.
        if (cookie.secure && !secure)
            continue;

        matched.append(cookie);
    }

    // Longest path first, then earliest creation. A server that receives two
    // cookies with the same name reads the first, so the more specific one has to
    // come first for the ordering to be useful.
    std::stable_sort(matched.begin(), matched.end(), [](const Cookie &a, const Cookie &b) {
        if (a.path.size() != b.path.size())
            return a.path.size() > b.path.size();
        return a.created < b.created;
    });

    return matched;
}

QString CookieJar::requestHeader(const network::Url &url) const
{
    const QList<Cookie> cookies = cookiesFor(url);
    if (cookies.isEmpty())
        return {};

    QStringList pairs;
    pairs.reserve(cookies.size());
    for (const Cookie &cookie : cookies)
        pairs << cookie.name + u'=' + cookie.value;

    return pairs.join(QStringLiteral("; "));
}

int CookieJar::pruneExpired(const QDateTime &now)
{
    const int before = static_cast<int>(m_cookies.size());
    for (int i = static_cast<int>(m_cookies.size()) - 1; i >= 0; --i) {
        if (m_cookies.at(i).isExpired(now))
            m_cookies.removeAt(i);
    }
    return before - static_cast<int>(m_cookies.size());
}

} // namespace oqb::storage
