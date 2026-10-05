#pragma once

#include <QString>

namespace oqb::network {

/// A URL, parsed and resolved following RFC 3986.
///
/// OpenQBrowser parses URLs itself rather than delegating to QUrl so that the
/// behaviour is explicit, explainable and testable. This is the first stage of
/// the browser pipeline (see architecture/pipeline.md).
class Url
{
public:
    Url() = default;

    /// Parses `input` on its own, with no base URL to resolve against.
    static Url parse(const QString &input);
    /// Parses `input`, resolving relative references against `base`.
    static Url parse(const QString &input, const Url &base);

    /// Interprets `input` the way an address bar would. Returns an invalid URL
    /// when the input looks like a search phrase rather than a location; the
    /// caller then builds a search URL with forSearchQuery().
    static Url fromUserInput(const QString &input);
    static Url fromUserInput(const QString &input, const Url &base);

    /// Builds a file:// URL for a local path.
    static Url fromLocalFile(const QString &path);

    /// Builds the URL of a search query for the given engine template, which
    /// must contain "%s".
    static Url forSearchQuery(const QString &query, const QString &searchTemplate);

    bool isValid() const { return m_valid; }

    QString scheme() const { return m_scheme; }
    QString userInfo() const { return m_userInfo; }
    QString host() const { return m_host; }
    int port() const { return m_port; }
    QString path() const { return m_path; }
    QString query() const { return m_query; }
    QString fragment() const { return m_fragment; }

    /// The port actually used, falling back to the scheme default.
    int effectivePort() const;

    bool hasAuthority() const { return m_hasAuthority; }
    bool isHttp() const { return m_scheme == QLatin1String("http"); }
    bool isHttps() const { return m_scheme == QLatin1String("https"); }
    bool isAbout() const { return m_scheme == QLatin1String("about"); }
    bool isLocalFile() const { return m_scheme == QLatin1String("file"); }
    bool isSecure() const { return isHttps() || isAbout(); }
    bool isLocal() const { return isLocalFile(); }

    /// True when the URL can be fetched over the network.
    bool isRemote() const { return isHttp() || isHttps(); }

    /// The `about:` page name, e.g. "home" for about:home.
    QString aboutPage() const;

    /// Absolute URL without the fragment: what is sent on the wire.
    QString toRequestTarget() const;

    /// Full absolute URL including the fragment.
    QString toString() const;

    /// Filesystem path of a file:// URL.
    QString toLocalFile() const;

    /// Origin (scheme, host, port) used for same-origin checks.
    QString origin() const;

    /// Resolves `relative` against this URL.
    Url resolved(const QString &relative) const;

    /// Host suited for display in the address bar.
    QString displayHost() const;

    bool operator==(const Url &other) const { return toString() == other.toString(); }
    bool operator!=(const Url &other) const { return !(*this == other); }

private:
    void normalize();

    QString m_scheme;
    QString m_userInfo;
    QString m_host;
    int m_port = -1;
    QString m_path;
    QString m_query;
    QString m_fragment;
    bool m_hasAuthority = false;
    bool m_hasQuery = false;
    bool m_hasFragment = false;
    bool m_valid = false;
};

} // namespace oqb::network
