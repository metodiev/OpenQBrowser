#include "browser/BuiltinPages.h"

#include "storage/Bookmarks.h"
#include "storage/History.h"

#include <QDateTime>
#include <QHash>

namespace oqb::browser::builtin {
namespace {

/// Escapes text for embedding in generated markup.
QString escape(const QString &text)
{
    QString out = text;
    out.replace(QLatin1String("&"), QLatin1String("&amp;"));
    out.replace(QLatin1String("<"), QLatin1String("&lt;"));
    out.replace(QLatin1String(">"), QLatin1String("&gt;"));
    out.replace(QLatin1String("\""), QLatin1String("&quot;"));
    return out;
}

/// The styles every built-in page shares. Keeping them in one place means the
/// pages look like a set, and it exercises the parts of CSS the renderer
/// supports, which has already caught regressions.
QString sharedStyles()
{
    return QStringLiteral(R"(
      :root { color-scheme: light dark }
      body { font-family: -apple-system, "Helvetica Neue", sans-serif;
             margin: 0; padding: 40px; background: #f8f9fa; color: #212529;
             line-height: 24px; }
      h1 { font-size: 32px; margin: 0 0 8px 0; color: #212529 }
      h2 { font-size: 20px; margin: 28px 0 8px 0; color: #343a40 }
      p { margin: 0 0 16px 0; max-width: 640px }
      .subtitle { color: #6c757d; font-size: 15px }
      a { color: #0b7285; text-decoration: none }
      ul { margin: 0 0 16px 0; padding-left: 24px }
      li { margin-bottom: 6px }
      .card { background: white; border: 1px solid #dee2e6; padding: 16px;
              margin-bottom: 12px; max-width: 640px }
      .badge { display: inline-block; background: #0b7285; color: white;
               padding: 2px 8px; font-size: 12px; margin-right: 6px }
      .meta { color: #868e96; font-size: 13px }
      table { border-collapse: collapse; max-width: 640px }
      td, th { text-align: left; padding: 6px 12px 6px 0; vertical-align: top }
      code { font-family: Menlo, monospace; background: #f1f3f5; padding: 1px 4px }
    )");
}

QString pageShell(const QString &title, const QString &body)
{
    return QStringLiteral("<!DOCTYPE html><html><head><title>%1</title><style>%2</style>"
                          "</head><body>%3</body></html>")
        .arg(escape(title), sharedStyles(), body);
}

QString homePage(const QString &version, int tabCount)
{
    QString body;
    body += QStringLiteral("<h1>OpenQBrowser</h1>\n");
    body += QStringLiteral("<p class=\"subtitle\">An open-source browser built from scratch. "
                           "Version %1.</p>\n").arg(escape(version));

    body += QStringLiteral("<h2>Search or enter an address</h2>\n");
    body += QStringLiteral("<p>Type in the address bar above. OpenQBrowser treats something with "
                           "a dot in it as a host name and anything else as a search.</p>\n");

    body += QStringLiteral("<h2>Built-in pages</h2>\n<ul>\n");
    for (const QString &name : pageNames())
        body += QStringLiteral("<li><a href=\"about:%1\">about:%1</a></li>\n").arg(name);
    body += QStringLiteral("</ul>\n");

    body += QStringLiteral("<h2>What works today</h2>\n");
    body += QStringLiteral("<ul>\n");
    body += QStringLiteral("<li>HTTP/1.1 and HTTPS, with redirects, chunked bodies and "
                           "compressed responses</li>\n");
    body += QStringLiteral("<li>An HTML parser that builds the same tree a browser would, "
                           "including implied elements</li>\n");
    body += QStringLiteral("<li>A CSS engine with selectors, specificity, inheritance and "
                           "media queries</li>\n");
    body += QStringLiteral("<li>Block and inline layout with margin collapsing, floats and "
                           "replaced elements</li>\n");
    body += QStringLiteral("<li>Rendering to the window or to an image, headlessly</li>\n");
    body += QStringLiteral("</ul>\n");

    body += QStringLiteral("<h2>What does not work yet</h2>\n");
    body += QStringLiteral("<ul>\n");
    body += QStringLiteral("<li><strong>JavaScript.</strong> Scripts are parsed but never run, "
                           "so interactive pages will not respond.</li>\n");
    body += QStringLiteral("<li><strong>Flexbox and grid.</strong> Those layout modes are "
                           "recognised but not implemented.</li>\n");
    body += QStringLiteral("<li><strong>Forms and cookies.</strong> Nothing is submitted or "
                           "stored between sessions.</li>\n");
    body += QStringLiteral("</ul>\n");

    body += QStringLiteral("<p class=\"meta\">%1 tab%2 open. See <a href=\"about:version\">"
                           "about:version</a> for build details.</p>\n")
                .arg(tabCount)
                .arg(tabCount == 1 ? QString() : QStringLiteral("s"));

    return pageShell(QStringLiteral("New Tab"), body);
}

QString versionPage(const QString &version)
{
    QString body = QStringLiteral("<h1>Version</h1>\n");
    body += QStringLiteral("<p><span class=\"badge\">%1</span></p>\n").arg(escape(version));
    body += QStringLiteral("<h2>Components</h2>\n<ul>\n");
    body += QStringLiteral("<li><code>network</code> — URLs, HTTP/1.1, TLS, redirects, gzip</li>\n");
    body += QStringLiteral("<li><code>html</code> — tokenizer, tree construction, entities</li>\n");
    body += QStringLiteral("<li><code>css</code> — tokenizer, selectors, cascade, media queries</li>\n");
    body += QStringLiteral("<li><code>dom</code> — node tree with attributes and classes</li>\n");
    body += QStringLiteral("<li><code>renderer</code> — box tree, layout, painting</li>\n");
    body += QStringLiteral("<li><code>browser</code> — pages, tabs, history</li>\n");
    body += QStringLiteral("<li><code>javascript</code> — integration seam only</li>\n");
    body += QStringLiteral("</ul>\n");
    return pageShell(QStringLiteral("Version"), body);
}

QString historyPage(const storage::HistoryStore *history)
{
    if (!history || history->count() == 0) {
        return pageShell(QStringLiteral("History"),
                         QStringLiteral("<h1>History</h1><p>Nothing visited yet in this "
                                        "session.</p>"));
    }

    QString body = QStringLiteral("<h1>History</h1>\n");
    body += QStringLiteral("<p class=\"subtitle\">%1 entr%2 in this session, most recent "
                           "first.</p>\n")
                .arg(history->count())
                .arg(history->count() == 1 ? QStringLiteral("y") : QStringLiteral("ies"));

    body += QStringLiteral("<ul>\n");
    for (int i = history->count() - 1; i >= 0; --i) {
        const storage::HistoryEntry &entry = history->at(i);
        const QString title = entry.title.isEmpty() ? entry.url.toString() : entry.title;
        body += QStringLiteral("<li><a href=\"%1\">%2</a>")
                    .arg(escape(entry.url.toString()), escape(title));
        body += QStringLiteral(" <span class=\"meta\">%1</span>")
                    .arg(escape(entry.url.displayHost()));
        if (entry.visitCount > 1)
            body += QStringLiteral(" <span class=\"meta\">%1 visits</span>").arg(entry.visitCount);
        body += QStringLiteral("</li>\n");
    }
    body += QStringLiteral("</ul>\n");

    return pageShell(QStringLiteral("History"), body);
}

QString bookmarksPage(const storage::BookmarkStore *bookmarks)
{
    if (!bookmarks || bookmarks->isEmpty()) {
        return pageShell(QStringLiteral("Bookmarks"),
                         QStringLiteral("<h1>Bookmarks</h1><p>No bookmarks yet. Use the star "
                                        "button in the toolbar to save the page you are on.</p>"));
    }
    return pageShell(QStringLiteral("Bookmarks"), bookmarks->toHtml());
}

QString blankPage()
{
    return QStringLiteral("<!DOCTYPE html><html><head><title>Blank</title></head>"
                          "<body></body></html>");
}

QString aboutAbout()
{
    QString body = QStringLiteral("<h1>Built-in pages</h1>\n");

    const QHash<QString, QString> descriptions = {
        {QStringLiteral("home"), QStringLiteral("The new tab page")},
        {QStringLiteral("about"), QStringLiteral("This list")},
        {QStringLiteral("version"), QStringLiteral("Build and component details")},
        {QStringLiteral("history"), QStringLiteral("Pages visited in this session")},
        {QStringLiteral("bookmarks"), QStringLiteral("Pages you have saved")},
        {QStringLiteral("blank"), QStringLiteral("An empty document")},
    };

    body += QStringLiteral("<ul>\n");
    for (const QString &name : pageNames()) {
        body += QStringLiteral("<li><a href=\"about:%1\">about:%1</a> — %2</li>\n")
                    .arg(name, escape(descriptions.value(name, QStringLiteral("A built-in page"))));
    }
    body += QStringLiteral("</ul>\n");

    return pageShell(QStringLiteral("Built-in pages"), body);
}

} // namespace

QStringList pageNames()
{
    return {QStringLiteral("home"), QStringLiteral("about"), QStringLiteral("version"),
            QStringLiteral("history"), QStringLiteral("bookmarks"), QStringLiteral("blank")};
}

bool hasPage(const QString &name)
{
    return pageNames().contains(name.toLower());
}

QString documentFor(const network::Url &url, const QString &version, double viewportWidth,
                    const storage::HistoryStore *history,
                    const storage::BookmarkStore *bookmarks, int tabCount)
{
    Q_UNUSED(viewportWidth);

    const QString name = url.aboutPage();

    if (name == QLatin1String("home") || name.isEmpty())
        return homePage(version, tabCount);
    if (name == QLatin1String("about"))
        return aboutAbout();
    if (name == QLatin1String("version"))
        return versionPage(version);
    if (name == QLatin1String("history"))
        return historyPage(history);
    if (name == QLatin1String("bookmarks"))
        return bookmarksPage(bookmarks);
    if (name == QLatin1String("blank"))
        return blankPage();

    // An unknown about: page is an error page, not a blank one, so the user
    // learns that the page does not exist.
    return errorPage(url, QStringLiteral("notfound"),
                     QStringLiteral("There is no built-in page called \"%1\".").arg(name));
}

QString errorPage(const network::Url &url, const QString &kind, const QString &details)
{
    QString heading;
    QString explanation;
    QString hint;

    if (kind == QLatin1String("dns")) {
        heading = QStringLiteral("Server not found");
        explanation = QStringLiteral("OpenQBrowser could not look up the address \"%1\".")
                          .arg(escape(url.host()));
        hint = QStringLiteral("Check the address for a typo, and check your network "
                              "connection.");
    } else if (kind == QLatin1String("connect")) {
        heading = QStringLiteral("Cannot reach this site");
        explanation = QStringLiteral("A connection to %1 could not be established.")
                          .arg(escape(url.host()));
        hint = QStringLiteral("The server may be down, or a firewall may be blocking the "
                              "connection.");
    } else if (kind == QLatin1String("tls")) {
        heading = QStringLiteral("Secure connection failed");
        explanation = QStringLiteral("The certificate presented by %1 could not be verified.")
                          .arg(escape(url.host()));
        hint = QStringLiteral("OpenQBrowser refuses to load a page whose identity it cannot "
                              "confirm. If this site is genuinely yours, check its certificate.");
    } else if (kind == QLatin1String("timeout")) {
        heading = QStringLiteral("The site took too long to respond");
        explanation = QStringLiteral("%1 did not answer in time.").arg(escape(url.host()));
        hint = QStringLiteral("Try again, or check whether the server is overloaded.");
    } else if (kind == QLatin1String("http")) {
        heading = QStringLiteral("The server returned an error");
        explanation = QStringLiteral("The response from %1 is shown below.")
                          .arg(escape(url.displayHost()));
    } else if (kind == QLatin1String("blocked")) {
        heading = QStringLiteral("This address was blocked");
        explanation = QStringLiteral("OpenQBrowser refused to load %1.")
                          .arg(escape(url.toString()));
        hint = QStringLiteral("See about:version for the security rules in force.");
    } else if (kind == QLatin1String("notfound")) {
        heading = QStringLiteral("Page not found");
        explanation = escape(details);
    } else {
        heading = QStringLiteral("This page could not be loaded");
        explanation = escape(details);
    }

    QString body = QStringLiteral("<h1>%1</h1>\n").arg(heading);
    body += QStringLiteral("<p>%1</p>\n").arg(explanation);

    if (!url.toString().isEmpty() && kind != QLatin1String("notfound")) {
        body += QStringLiteral("<p class=\"meta\">%1</p>\n").arg(escape(url.toString()));
    }
    if (!hint.isEmpty())
        body += QStringLiteral("<p>%1</p>\n").arg(hint);
    if (!details.isEmpty() && kind != QLatin1String("notfound")) {
        body += QStringLiteral("<div class=\"card\"><p class=\"meta\">%1</p></div>\n")
                    .arg(escape(details));
    }

    body += QStringLiteral("<p><a href=\"about:home\">Start page</a></p>\n");

    return pageShell(heading, body);
}

} // namespace oqb::browser::builtin
