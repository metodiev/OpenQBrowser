#include "network/Url.h"

#include <QDir>
#include <QSet>
#include <QRegularExpression>
#include <QStringList>
#include <QUrl>

#include <utility>

namespace oqb::network {
namespace {

/// True when `text` begins with "scheme:"; `colonIndex` receives the colon.
bool looksLikeScheme(const QString &text, int *colonIndex)
{
    if (text.isEmpty() || !text.at(0).isLetter())
        return false;

    for (int i = 1; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == u':') {
            *colonIndex = i;
            return true;
        }
        if (c.isLetterOrNumber() || c == u'+' || c == u'-' || c == u'.')
            continue;
        return false;
    }
    return false;
}

/// RFC 3986 section 5.2.4, "Remove Dot Segments".
QString removeDotSegments(const QString &path)
{
    if (path.isEmpty())
        return path;

    const bool absolute = path.startsWith(u'/');
    // RFC 3986 §5.2.4: removing a trailing "." or ".." leaves the slash that
    // preceded it, so "/a/b/.." becomes "/a/" rather than "/a". A trailing
    // slash that was already there is preserved by the empty last segment.
    bool trailingSlash = path.endsWith(u'/');

    QStringList output;
    const QStringList segments = path.split(u'/', Qt::KeepEmptyParts);
    for (const QString &segment : segments) {
        if (segment == QLatin1String(".")) {
            // A trailing "." turns into a slash; an interior one disappears.
            trailingSlash = true;
            continue;
        }
        if (segment == QLatin1String("..")) {
            if (!output.isEmpty())
                output.removeLast();
            trailingSlash = true;
            continue;
        }
        // A real segment resets the flag; only a trailing "." or ".." leaves
        // the slash behind.
        if (!segment.isEmpty())
            trailingSlash = false;
        output.append(segment);
    }

    if (trailingSlash && (output.isEmpty() || !output.last().isEmpty()))
        output.append(QString());

    QString result = output.join(u'/');
    if (absolute && !result.startsWith(u'/'))
        result.prepend(u'/');
    if (absolute && result.isEmpty())
        result = QStringLiteral("/");
    return result;
}

/// Percent-encodes characters that are unsafe inside a URL component while
/// leaving existing percent escapes and readable characters untouched.
QString encodeUnsafe(const QString &component)
{
    QString out;
    out.reserve(component.size());
    for (int i = 0; i < component.size(); ++i) {
        const QChar c = component.at(i);
        const ushort code = c.unicode();
        if (code < 0x20 || code == 0x7F || c == u' ' || c == u'"' || c == u'<'
            || c == u'>' || c == u'\\' || c == u'^' || c == u'`' || c == u'{'
            || c == u'|' || c == u'}') {
            const QByteArray utf8 = QString(c).toUtf8();
            for (const char byte : utf8) {
                out += QStringLiteral("%%1").arg(static_cast<uchar>(byte), 2, 16,
                                                 QLatin1Char('0'))
                           .toUpper();
            }
        } else {
            out.append(c);
        }
    }
    return out;
}

} // namespace

int Url::effectivePort() const
{
    if (m_port >= 0)
        return m_port;
    if (isHttps())
        return 443;
    if (isHttp())
        return 80;
    return -1;
}

QString Url::aboutPage() const
{
    if (!isAbout())
        return {};
    QString name = m_path;
    while (name.startsWith(u'/'))
        name.remove(0, 1);
    return name.toLower();
}

QString Url::toRequestTarget() const
{
    QString target = m_path.isEmpty() ? QStringLiteral("/") : encodeUnsafe(m_path);
    if (m_hasQuery)
        target += u'?' + encodeUnsafe(m_query);
    return target;
}

QString Url::toString() const
{
    QString out;
    if (!m_scheme.isEmpty())
        out += m_scheme + u':';
    if (m_hasAuthority) {
        out += QStringLiteral("//");
        if (!m_userInfo.isEmpty())
            out += m_userInfo + u'@';
        out += m_host;
        if (m_port >= 0)
            out += u':' + QString::number(m_port);
    }
    out += m_path;
    if (m_hasQuery)
        out += u'?' + m_query;
    if (m_hasFragment)
        out += u'#' + m_fragment;
    return out;
}

QString Url::toLocalFile() const
{
    if (!isLocalFile())
        return {};
    return QUrl(toString()).toLocalFile();
}

QString Url::origin() const
{
    if (!m_hasAuthority)
        return m_scheme + QStringLiteral(":");
    return m_scheme + QStringLiteral("://") + m_host + u':'
        + QString::number(effectivePort());
}

QString Url::serialisedOrigin() const
{
    if (!m_hasAuthority)
        return m_scheme + QStringLiteral(":");

    // A port that the scheme already implies is left out, because that is how
    // every server writes the header: "https://example.com", never
    // "https://example.com:443".
    const int port = effectivePort();
    const bool isDefault = (m_scheme == QLatin1String("http") && port == 80)
        || (m_scheme == QLatin1String("https") && port == 443)
        || (m_scheme == QLatin1String("ws") && port == 80)
        || (m_scheme == QLatin1String("wss") && port == 443);

    if (isDefault)
        return m_scheme + QStringLiteral("://") + m_host;
    return m_scheme + QStringLiteral("://") + m_host + u':' + QString::number(port);
}

QString Url::displayHost() const
{
    if (m_host.isEmpty())
        return {};
    QString host = QUrl::fromPercentEncoding(m_host.toUtf8());
    if (host.startsWith(QLatin1String("www.")))
        host.remove(0, 4);
    return host;
}

void Url::normalize()
{
    m_scheme = m_scheme.toLower();
    m_host = m_host.toLower();

    if (isHttp() || isHttps()) {
        if (m_port == (isHttps() ? 443 : 80))
            m_port = -1;
        if (m_path.isEmpty())
            m_path = QStringLiteral("/");
    }
    m_path = removeDotSegments(m_path);
}

Url Url::parse(const QString &input)
{
    return parse(input, Url());
}

Url Url::parse(const QString &input, const Url &base)
{
    Url url;
    QString rest = input.trimmed();
    if (rest.isEmpty()) {
        url.m_valid = false;
        return url;
    }

    int colon = -1;
    const bool hadScheme = looksLikeScheme(rest, &colon);
    if (hadScheme) {
        url.m_scheme = rest.left(colon).toLower();
        rest = rest.mid(colon + 1);
        url.m_valid = true;
    } else if (base.isValid()) {
        url.m_scheme = base.m_scheme;
        url.m_hasAuthority = base.m_hasAuthority;
        url.m_userInfo = base.m_userInfo;
        url.m_host = base.m_host;
        url.m_port = base.m_port;
        url.m_valid = true;
    } else {
        url.m_valid = false;
        return url;
    }

    // Authority component. `authorityFromInput` distinguishes "//host/path" in
    // the reference itself from an authority inherited from the base URL, which
    // decides whether the path is absolute or relative to the base's directory.
    bool authorityFromInput = false;
    if (rest.startsWith(QLatin1String("//"))) {
        rest.remove(0, 2);
        authorityFromInput = true;
        url.m_hasAuthority = true;
        url.m_userInfo.clear();
        url.m_host.clear();
        url.m_port = -1;

        int end = rest.size();
        for (int i = 0; i < rest.size(); ++i) {
            const QChar c = rest.at(i);
            if (c == u'/' || c == u'?' || c == u'#') {
                end = i;
                break;
            }
        }

        QString authority = rest.left(end);
        rest = rest.mid(end);

        const int at = authority.lastIndexOf(u'@');
        if (at >= 0) {
            url.m_userInfo = authority.left(at);
            authority = authority.mid(at + 1);
        }

        if (authority.startsWith(u'[')) {
            const int close = authority.indexOf(u']');
            if (close < 0) {
                url.m_valid = false;
                return url;
            }
            url.m_host = authority.left(close + 1);
            const QString remainder = authority.mid(close + 1);
            if (remainder.startsWith(u':'))
                url.m_port = remainder.mid(1).toInt();
        } else {
            const int portColon = authority.lastIndexOf(u':');
            if (portColon >= 0) {
                url.m_host = authority.left(portColon);
                bool ok = false;
                const int port = authority.mid(portColon + 1).toInt(&ok);
                url.m_port = ok ? port : -1;
            } else {
                url.m_host = authority;
            }
        }

        // "file:///path" has an empty authority by definition, so only the
        // schemes that need a host are rejected for lacking one.
        if (url.m_host.isEmpty() && url.m_scheme != QLatin1String("file")) {
            url.m_valid = false;
            return url;
        }
        // Credentials are never sent on the wire and are dropped here so they
        // cannot leak through a redirect or a log.
        url.m_userInfo.clear();
    } else if (hadScheme && !url.m_hasAuthority && (url.isHttp() || url.isHttps())
               && !rest.startsWith(u'/') && !rest.isEmpty() && rest.at(0) != u'?'
               && rest.at(0) != u'#') {
        // Tolerate "http:example.com" the way browsers do.
        url.m_hasAuthority = true;
        authorityFromInput = true;
        const int slash = rest.indexOf(u'/');
        url.m_host = slash < 0 ? rest : rest.left(slash);
        rest = slash < 0 ? QString() : rest.mid(slash);
        if (url.m_host.isEmpty()) {
            url.m_valid = false;
            return url;
        }
    }

    // Fragment.
    const int hash = rest.indexOf(u'#');
    if (hash >= 0) {
        url.m_hasFragment = true;
        url.m_fragment = rest.mid(hash + 1);
        rest = rest.left(hash);
    }

    // Query.
    const int question = rest.indexOf(u'?');
    if (question >= 0) {
        url.m_hasQuery = true;
        url.m_query = rest.mid(question + 1);
        rest = rest.left(question);
    }

    // Path, with the reference-resolution rules from RFC 3986 section 5.2.2.
    //
    // Only a reference *without* a scheme is resolved against the base: one that
    // names its own scheme is absolute and stands alone (§5.2.2 ignores the base
    // as soon as R.scheme is defined). Inheriting anyway used to append the
    // current page's path to a typed address, so "www.dir.bg" entered on the
    // start page became "https://www.dir.bghome".
    const bool inheritsFromBase = base.isValid() && !hadScheme;
    if (rest.isEmpty()) {
        // With no path of its own, a relative reference keeps the base's path,
        // which is what makes "?q=1" and "#top" work (RFC 3986 §5.2.2). Only an
        // authority written in the reference itself starts from the root.
        if (inheritsFromBase) {
            url.m_path = base.m_path;
            if (!url.m_hasQuery && !url.m_hasFragment) {
                url.m_hasQuery = base.m_hasQuery;
                url.m_query = base.m_query;
            }
        } else if (authorityFromInput || url.m_hasAuthority) {
            url.m_path = QStringLiteral("/");
        } else if (url.isAbout()) {
            url.m_path = QStringLiteral("blank");
        } else {
            url.m_path = QStringLiteral("/");
        }
    } else if (rest.startsWith(u'/')) {
        url.m_path = rest;
    } else if (authorityFromInput) {
        url.m_path = u'/' + rest;
    } else if (url.isAbout()) {
        url.m_path = rest;
    } else if (inheritsFromBase) {
        QString basePath = base.m_path;
        const int lastSlash = basePath.lastIndexOf(u'/');
        basePath = lastSlash >= 0 ? basePath.left(lastSlash + 1) : QString();
        url.m_path = basePath + rest;
    } else {
        url.m_path = rest;
    }

    url.normalize();
    return url;
}

Url Url::resolved(const QString &relative) const
{
    return parse(relative, *this);
}

Url Url::fromLocalFile(const QString &path)
{
    const QString absolute = QDir::isAbsolutePath(path) ? path : QDir::current().absoluteFilePath(path);
    return Url::parse(QUrl::fromLocalFile(QDir::cleanPath(absolute)).toString());
}

Url Url::forSearchQuery(const QString &query, const QString &searchTemplate)
{
    QString templ = searchTemplate.trimmed();
    if (templ.isEmpty() || !templ.contains(QLatin1String("%s")))
        templ = QStringLiteral("https://duckduckgo.com/?q=%s");
    templ.replace(QLatin1String("%s"),
                  QString::fromUtf8(QUrl::toPercentEncoding(query)));
    return Url::parse(templ);
}

Url Url::fromUserInput(const QString &input)
{
    return fromUserInput(input, Url());
}

Url Url::fromUserInput(const QString &input, const Url &base)
{
    const QString trimmed = input.trimmed();
    if (trimmed.isEmpty())
        return {};

    // Only a scheme this build understands is treated as one. Without this,
    // "localhost:8080/x" parses as the scheme "localhost", which is exactly the
    // class of mistake the URL standard warns about.
    int colon = -1;
    if (looksLikeScheme(trimmed, &colon)) {
        static const QSet<QString> kKnownSchemes = {
            QStringLiteral("http"),      QStringLiteral("https"),  QStringLiteral("file"),
            QStringLiteral("about"),     QStringLiteral("data"),   QStringLiteral("ftp"),
            QStringLiteral("view-source"), QStringLiteral("mailto"),
        };
        const QString candidate = trimmed.left(colon).toLower();
        if (kKnownSchemes.contains(candidate))
            return Url::parse(trimmed, base);
    }

    const QString firstSegment = trimmed.split(u'/').first();
    const bool looksLikeHost = !firstSegment.contains(u' ')
        && (firstSegment.contains(u'.') || firstSegment.contains(u':')
            || firstSegment == QLatin1String("localhost")
            || firstSegment.startsWith(QLatin1String("localhost:")));

    if (!looksLikeHost)
        return {}; // Treated as a search phrase by the caller.

    const bool plainHttp = firstSegment == QLatin1String("localhost")
        || firstSegment.startsWith(QLatin1String("localhost:"))
        || firstSegment.startsWith(QLatin1String("127.0.0.1"))
        || firstSegment.startsWith(QLatin1String("0.0.0.0"));
    const QString scheme = plainHttp ? QStringLiteral("http") : QStringLiteral("https");
    return Url::parse(scheme + QStringLiteral("://") + trimmed, base);
}

} // namespace oqb::network
