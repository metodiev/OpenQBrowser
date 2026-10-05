#pragma once

#include <QSet>
#include <QString>

#include "network/Url.h"

namespace oqb::security {

/// The outcome of a policy check, carrying the reason so the UI can explain it
/// rather than silently doing nothing.
struct Decision
{
    bool allowed = true;
    QString reason;

    static Decision allow() { return {}; }
    static Decision deny(const QString &reason) { return {false, reason}; }
};

/// The browser's security policy: origins, permissions and transport rules.
///
/// OpenQBrowser follows the principle that a default must be safe and any
/// relaxation must be explicit and visible. Every check returns a Decision that
/// carries its reason, so a refusal can be shown to the user instead of looking
/// like a broken page.
class SecurityPolicy
{
public:
    SecurityPolicy();

    /// True when two URLs share a scheme, host and port. This is the check the
    /// same-origin policy is built on.
    static bool sameOrigin(const network::Url &a, const network::Url &b);

    /// Whether a document at `from` may load a subresource from `to`.
    ///
    /// Cross-origin subresources are permitted (that is how the web works), but
    /// a secure document may not be downgraded to an insecure one. That rule
    /// prevents an attacker who controls a plaintext response from injecting
    /// content into an HTTPS page.
    Decision canLoadSubresource(const network::Url &from, const network::Url &to) const;

    /// Whether a navigation from `from` to `to` is allowed. Navigations are
    /// never blocked by scheme, because that is how people leave a broken site.
    Decision canNavigate(const network::Url &from, const network::Url &to) const;

    /// Whether sending a cookie for `to` as part of a request from `from` is
    /// acceptable: the SameSite rules in their simplest useful form.
    Decision canSendCookies(const network::Url &from, const network::Url &to) const;

    /// The schemes a page is allowed to reference at all.
    bool isSchemeAllowed(const QString &scheme) const;

    /// True when the browser should refuse to load this URL for security
    /// reasons, such as a data URL in a navigation.
    Decision checkTopLevelNavigation(const network::Url &url) const;

    /// Whether a feature such as "geolocation" or "camera" may be used by
    /// `origin`. OpenQBrowser denies everything by default and has no UI to
    /// grant permissions yet, which is the safe state to ship in.
    Decision canUseFeature(const QString &feature, const network::Url &origin) const;

    /// Enables or disables strict transport security enforcement. When enabled,
    /// a host that was once reached over HTTPS is not silently downgraded.
    void setEnforceStrictTransportSecurity(bool enabled) { m_enforceHsts = enabled; }
    bool enforcesStrictTransportSecurity() const { return m_enforceHsts; }

    /// Records that `host` was successfully reached over HTTPS, which the
    /// strict transport policy then remembers.
    void rememberSecureHost(const QString &host);
    bool isSecureHost(const QString &host) const;

private:
    bool m_enforceHsts = true;
    QSet<QString> m_secureHosts;
};

/// A same-origin identity, used as the key for anything scoped to an origin.
QString originKey(const network::Url &url);

} // namespace oqb::security
