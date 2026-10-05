#include "security/SecurityPolicy.h"

#include <QSet>

namespace oqb::security {

SecurityPolicy::SecurityPolicy() = default;

QString originKey(const network::Url &url)
{
    return url.origin();
}

bool SecurityPolicy::sameOrigin(const network::Url &a, const network::Url &b)
{
    // Two opaque origins are never the same origin, which is what keeps a file
    // URL from sharing anything with another file URL.
    if (!a.isValid() || !b.isValid())
        return false;
    if (a.scheme().isEmpty() || b.scheme().isEmpty())
        return false;
    if (a.isLocalFile() || b.isLocalFile())
        return false;

    return a.origin() == b.origin();
}

bool SecurityPolicy::isSchemeAllowed(const QString &scheme) const
{
    static const QSet<QString> kAllowed = {
        QStringLiteral("http"), QStringLiteral("https"), QStringLiteral("file"),
        QStringLiteral("about"), QStringLiteral("data"), QStringLiteral("blob"),
    };
    return kAllowed.contains(scheme.toLower());
}

Decision SecurityPolicy::canLoadSubresource(const network::Url &from, const network::Url &to) const
{
    if (!to.isValid())
        return Decision::deny(QStringLiteral("the resource URL is not valid"));

    if (!isSchemeAllowed(to.scheme()))
        return Decision::deny(QStringLiteral("the %1 scheme is not allowed").arg(to.scheme()));

    // Nothing secure may be pulled in from a plaintext connection, and no
    // plaintext resource may be requested from a secure document.
    if (from.isHttps() && to.isHttp()) {
        return Decision::deny(
            QStringLiteral("a secure page cannot load an insecure resource"));
    }

    if (m_enforceHsts && to.isHttp() && isSecureHost(to.host())) {
        return Decision::deny(QStringLiteral("this host requires a secure connection"));
    }

    return Decision::allow();
}

Decision SecurityPolicy::canNavigate(const network::Url &from, const network::Url &to) const
{
    Q_UNUSED(from);

    if (!to.isValid())
        return Decision::deny(QStringLiteral("the destination is not a valid URL"));

    if (!isSchemeAllowed(to.scheme()))
        return Decision::deny(QStringLiteral("the %1 scheme is not supported").arg(to.scheme()));

    return Decision::allow();
}

Decision SecurityPolicy::canSendCookies(const network::Url &from, const network::Url &to) const
{
    if (!from.isValid() || !to.isValid())
        return Decision::deny(QStringLiteral("one of the URLs is not valid"));

    // Cookies for a different site are only sent on a top-level navigation,
    // which is the practical form of the SameSite=Lax default.
    if (!sameOrigin(from, to)) {
        if (to.isHttp() && from.isHttps())
            return Decision::deny(QStringLiteral("refusing to send cookies to an insecure origin"));
    }

    return Decision::allow();
}

Decision SecurityPolicy::checkTopLevelNavigation(const network::Url &url) const
{
    if (!url.isValid())
        return Decision::deny(QStringLiteral("the address is not a valid URL"));

    // A data URL as a top level document is a phishing vector: the address bar
    // would show content that no server ever served.
    if (url.scheme() == QLatin1String("data")) {
        return Decision::deny(
            QStringLiteral("data: URLs cannot be opened as a page"));
    }

    return Decision::allow();
}

Decision SecurityPolicy::canUseFeature(const QString &feature, const network::Url &origin) const
{
    Q_UNUSED(origin);
    // Every permission is denied until the browser has a prompt and a store to
    // remember an answer, which is the only safe default.
    return Decision::deny(
        QStringLiteral("OpenQBrowser does not grant the '%1' permission yet").arg(feature));
}

void SecurityPolicy::rememberSecureHost(const QString &host)
{
    if (!host.isEmpty())
        m_secureHosts.insert(host.toLower());
}

bool SecurityPolicy::isSecureHost(const QString &host) const
{
    return m_secureHosts.contains(host.toLower());
}

} // namespace oqb::security
