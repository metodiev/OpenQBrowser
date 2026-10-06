#pragma once

#include <QString>
#include <QStringList>

#include "network/HttpMessage.h"

namespace oqb::network {

/// The part of `fetch()`'s behaviour that depends on nothing but the request and
/// the CORS headers of the response.
///
/// These are pure functions on purpose. The rules that decide what script may
/// send, and what it may read back, are the whole security boundary of the
/// feature: a page that can set `Host` or read a cross-origin body without
/// permission is a page that can forge requests and exfiltrate other sites'
/// data. A rule that could only be exercised through a socket could not be
/// tested properly, so the decisions live here, where the tests can reach them
/// directly, and the loader and the bindings only carry them out.
///
/// See architecture/security.md.
class FetchPolicy
{
public:
    // ------------------------------------------------------------- the request

    /// True when script may use `method` at all. CONNECT, TRACE and TRACK are
    /// refused because they are not safe to expose to a page, and anything that
    /// is not a valid HTTP token is refused because it would not survive being
    /// written into a request line.
    static bool isAllowedMethod(const QString &method);

    /// True when the method may be used for a cross-origin request without a
    /// preflight, which is what makes the request "simple".
    static bool isSimpleMethod(const QString &method);

    /// True when script may set `name` on a request.
    ///
    /// Names that describe the connection, the destination or the session are
    /// refused: `Host` would let a page address a different virtual host on the
    /// same address, `Content-Length` would let it frame a body differently from
    /// the one it sent, and `Cookie` would let it read out a session it cannot
    /// otherwise see. The browser sets all of these itself.
    static bool isForbiddenRequestHeader(const QString &name);

    /// True when `name: value` is safelisted, so a request carrying it is still
    /// simple.
    static bool isSafelistedRequestHeader(const QString &name, const QString &value);

    /// True when the request needs no preflight: a simple method carrying only
    /// safelisted headers.
    static bool isSimpleRequest(const QString &method, const HeaderList &headers);

    // ------------------------------------------------------------ the response

    /// True when a cross-origin response exposes `name` to script without being
    /// named in `Access-Control-Expose-Headers`.
    static bool isSafelistedResponseHeader(const QString &name);

    /// True when script may read `name` from a cross-origin response.
    ///
    /// `Access-Control-Expose-Headers: *` is a wildcard only for a request that
    /// carried no credentials. Once the request carried the user's session, the
    /// server has to name each header it means to expose: a wildcard would
    /// otherwise publish every header the response happened to carry.
    static bool exposesHeader(const QString &name, const QStringList &exposedHeaders,
                              bool credentialsIncluded = false);

    /// The headers of a **cross-origin** response that script may read.
    ///
    /// A response's headers describe more than its content: a request id, an
    /// internal host name, a rate-limit bucket. Without this filter every one of
    /// them would be readable by any page that asked for the resource.
    ///
    /// Only for a cross-origin response. A same-origin one is not filtered at
    /// all - the page could have read the resource itself - so a caller decides
    /// by origin and calls this only when they differ. The name says so, because
    /// calling it on a same-origin response would hide headers the page is
    /// entitled to.
    static HeaderList crossOriginReadableHeaders(const HeaderList &headers,
                                                 const QString &allowOrigin,
                                                 const QStringList &exposedHeaders,
                                                 bool credentialsIncluded);

    /// The `Access-Control-*` headers of a response, gathered so the decision can
    /// be a function of them alone.
    struct CorsHeaders
    {
        /// `Access-Control-Allow-Origin`, verbatim. Empty when absent.
        QString allowOrigin;
        /// True when `Access-Control-Allow-Credentials: true` was sent.
        bool allowCredentials = false;
        /// `Access-Control-Expose-Headers`, split on commas.
        QStringList exposeHeaders;

        static CorsHeaders fromHeaders(const HeaderList &headers);
    };

    struct CorsResult
    {
        bool readable = false;
        QString reason;
    };

    /// Whether the body of a cross-origin response may be handed to script.
    ///
    /// `credentialsIncluded` is the request's credentials mode, not the
    /// `Access-Control-Allow-Credentials` header: a wildcard allow-origin cannot
    /// authorise a request that carried the user's session, because a server
    /// that answers `*` has not decided to trust any particular origin with it.
    static CorsResult checkReadable(const CorsHeaders &headers, const QString &requestOrigin,
                                   bool credentialsIncluded);

    /// Splits a comma-separated header value into trimmed, non-empty parts.
    static QStringList splitHeaderList(const QString &value);

    /// The status line text for a status code, so a Response built from a cached
    /// or synthetic answer reads the same as one from a socket.
    static QString statusTextFor(int statusCode);
};

} // namespace oqb::network
