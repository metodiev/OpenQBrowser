#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

#include "network/HttpMessage.h"
#include "network/Url.h"

namespace oqb::storage {

/// How a cookie may be sent, following the SameSite attribute (RFC 6265bis §4.1.2.7).
enum class SameSite {
    /// The attribute was absent. This browser treats an absent attribute the way
    /// modern browsers do - as Lax - because that is the default that protects a
    /// user without breaking the sites they visit.
    Unspecified,
    /// Sent only for requests that originate from the same site.
    Strict,
    /// Sent for same-site requests and for top-level navigations.
    Lax,
    /// Sent with every request, including cross-site subresources. A page has to
    /// ask for this explicitly.
    None,
};

/// One cookie, as stored in the jar.
///
/// The fields are the ones the jar needs to decide whether a cookie may be sent:
/// where it came from, what it is scoped to, when it expires, and the three
/// attributes that restrict it.
struct Cookie
{
    QString name;
    QString value;

    /// The domain the cookie applies to, as the server wrote it. A leading dot
    /// is stripped on parse, so this is always a bare host suffix.
    QString domain;
    /// The path prefix the cookie is scoped to. Always begins with "/".
    QString path;

    /// When the cookie expires, or an invalid QDateTime for a session cookie,
    /// which lives only as long as the browser runs.
    QDateTime expires;
    /// True when the cookie has an expiry, i.e. when it is not a session cookie.
    bool persistent = false;

    /// Only sent over HTTPS.
    bool secure = false;
    /// Not visible to page script. This engine does not expose a cookie API to
    /// script at all, so the flag is recorded and enforced by what is absent:
    /// `document.cookie` does not exist, so no script can read any cookie.
    bool httpOnly = false;
    SameSite sameSite = SameSite::Unspecified;

    /// When the cookie was created, used only to make ordering deterministic
    /// between cookies that are otherwise equal.
    QDateTime created;

    /// True when the cookie should no longer be sent, as of `now`.
    bool isExpired(const QDateTime &now) const;

    /// A one-line description for the inspector.
    QString describe() const;
};

/// The cookie jar: every cookie the browser holds, with the rules that decide
/// which of them a request may carry.
///
/// The jar is keyed by origin rather than by host string, and stores cookies in
/// the shape RFC 6265 defines, so the two security rules written in
/// `security.md` stay enforceable: a cookie is never sent to an insecure origin
/// when it was set by a secure one, and credentials are dropped on a cross-host
/// redirect.
///
/// Naming and matching follow the specification's own terms:
///
///   * a cookie's **domain** is a host suffix it applies to;
///   * its **path** is a prefix it applies to;
///   * a **request-host** and **request-path** are matched against both.
///
/// See architecture/storage.md.
class CookieJar
{
public:
    CookieJar() = default;

    /// Stores the cookies in a `Set-Cookie` header value, as sent by `url`.
    ///
    /// A single header may carry several cookies, which is done by repeating the
    /// header rather than by separating the values with commas - a comma is legal
    /// inside an `Expires` date, so splitting on it is a classic source of bugs.
    /// This takes one cookie's text at a time; use `storeFromHeaders` for the
    /// full response.
    ///
    /// Returns true when a cookie was stored or removed.
    bool store(const QString &setCookieValue, const network::Url &url);

    /// Stores every `Set-Cookie` header in a response.
    int storeFromHeaders(const network::HeaderList &headers, const network::Url &url);

    /// The `Cookie` header value for a request to `url`, or an empty string when
    /// the jar has nothing to send.
    ///
    /// The cookies are ordered by path length, longest first, and then by
    /// creation time, which is what a server expects when two cookies share a
    /// name and only one should win.
    QString requestHeader(const network::Url &url) const;

    /// Every cookie that would be sent to `url`. Used by the request header and
    /// by the inspector.
    QList<Cookie> cookiesFor(const network::Url &url) const;

    /// Every cookie in the jar, including ones that have expired and ones that
    /// would not be sent to any URL. The inspector shows these.
    const QList<Cookie> &all() const { return m_cookies; }

    /// Removes every expired cookie. Called before a lookup and after a store, so
    /// the jar does not grow without bound and a lookup never sees a stale entry.
    int pruneExpired(const QDateTime &now = QDateTime::currentDateTime());

    /// Removes every cookie.
    void clear() { m_cookies.clear(); }

    int count() const { return static_cast<int>(m_cookies.size()); }
    bool isEmpty() const { return m_cookies.isEmpty(); }

    /// True when a cookie for `domain` may be set by a page at `url`. A cookie
    /// may only be scoped to the host that set it or a suffix of it, which is
    /// what stops `evil.com` from setting a cookie for `example.com`.
    static bool domainAcceptableFrom(const QString &domain, const network::Url &url);

    /// True when `host` matches `domain` under the specification's rules: an
    /// exactly equal host, or `host` ending in `domain` preceded by a dot.
    static bool domainMatches(const QString &host, const QString &domain);

    /// True when `requestPath` is within the cookie's `path` under the
    /// specification's rules: a prefix, with a "/" boundary.
    static bool pathMatches(const QString &requestPath, const QString &cookiePath);

    /// The default path for a cookie set by a response from `url`, which is the
    /// directory part of the request path (RFC 6265 §5.1.4).
    static QString defaultPathFor(const network::Url &url);

private:
    /// Inserts or replaces a cookie. Two cookies are the same when their name,
    /// domain and path all match.
    void upsert(const Cookie &cookie);

    QList<Cookie> m_cookies;
};

} // namespace oqb::storage
