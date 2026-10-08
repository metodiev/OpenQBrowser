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

/// The styles every built-in page shares.
///
/// The pages are rendered by Chromium, so modern CSS is available: custom
/// properties, flexbox, grid and calc are all used here rather than worked
/// around. The palette follows the desktop's light or dark appearance through
/// prefers-color-scheme, which is what keeps a built-in page looking like part
/// of the browser rather than a document from another era.
QString sharedStyles()
{
    return QStringLiteral(R"(
      :root {
        color-scheme: light dark;
        --bg: #f6f7f9;
        --panel: #ffffff;
        --panel-hover: #f0f3f7;
        --text: #1b212b;
        --dim: #5f6772;
        --faint: #8a929e;
        --border: #e2e6ec;
        --accent: #1d74d8;
        --accent-soft: #e8f1fd;
        --radius: 12px;
      }
      @media (prefers-color-scheme: dark) {
        :root {
          --bg: #16181d;
          --panel: #1e2126;
          --panel-hover: #262a31;
          --text: #f2f5f9;
          --dim: #9aa3af;
          --faint: #7b838f;
          --border: #30353d;
          --accent: #5aa9ff;
          --accent-soft: #1b2a3d;
        }
      }
      * { box-sizing: border-box }
      body {
        margin: 0;
        padding: 0;
        background: var(--bg);
        color: var(--text);
        font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
        font-size: 15px;
        line-height: 1.55;
        -webkit-font-smoothing: antialiased;
      }
      .page {
        max-width: 720px;
        margin: 0 auto;
        padding: 56px 28px 80px;
      }
      .page--centered { text-align: center }
      h1 {
        font-size: 30px;
        line-height: 1.2;
        margin: 0 0 10px;
        letter-spacing: -0.02em;
      }
      h2 {
        font-size: 13px;
        text-transform: uppercase;
        letter-spacing: 0.08em;
        color: var(--faint);
        margin: 36px 0 12px;
        font-weight: 600;
      }
      p { margin: 0 0 14px; color: var(--dim) }
      a { color: var(--accent); text-decoration: none }
      a:hover { text-decoration: underline }
      .mark {
        width: 56px; height: 56px;
        border-radius: 16px;
        background: var(--accent);
        color: #fff;
        display: inline-flex;
        align-items: center;
        justify-content: center;
        font-size: 26px;
        font-weight: 700;
        margin-bottom: 18px;
      }
      .subtitle { font-size: 16px; color: var(--dim); margin-bottom: 28px }
      .grid {
        display: grid;
        grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
        gap: 12px;
      }
      .card {
        background: var(--panel);
        border: 1px solid var(--border);
        border-radius: var(--radius);
        padding: 16px 18px;
        display: block;
        color: inherit;
        transition: background 120ms ease, border-color 120ms ease;
      }
      a.card:hover {
        background: var(--panel-hover);
        border-color: var(--accent);
        text-decoration: none;
      }
      .card .title { font-weight: 600; margin-bottom: 2px }
      .card .desc { font-size: 13px; color: var(--faint) }
      .list { display: grid; gap: 8px }
      .list a.item, .item {
        background: var(--panel);
        border: 1px solid var(--border);
        border-radius: 10px;
        padding: 12px 14px;
        color: inherit;
        display: block;
      }
      .list a.item:hover {
        background: var(--panel-hover);
        border-color: var(--accent);
        text-decoration: none;
      }
      .item .host { font-size: 12px; color: var(--faint); margin-top: 2px }
      .meta { font-size: 13px; color: var(--faint) }
      .badge {
        display: inline-block;
        background: var(--accent-soft);
        color: var(--accent);
        border-radius: 999px;
        padding: 3px 11px;
        font-size: 12px;
        font-weight: 600;
      }
      .keys { display: grid; gap: 6px }
      .keys .row {
        display: flex;
        justify-content: space-between;
        gap: 16px;
        padding: 9px 14px;
        background: var(--panel);
        border: 1px solid var(--border);
        border-radius: 10px;
        font-size: 14px;
      }
      .keys .row span:last-child { color: var(--faint) }
      kbd {
        display: inline-block;
        min-width: 22px;
        text-align: center;
        background: var(--panel-hover);
        border: 1px solid var(--border);
        border-bottom-width: 2px;
        border-radius: 6px;
        padding: 1px 7px;
        font-family: inherit;
        font-size: 12px;
        font-weight: 600;
        color: var(--text);
      }
      code {
        font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
        background: var(--panel-hover);
        border-radius: 5px;
        padding: 2px 6px;
        font-size: 13px;
      }
      .error .mark { background: #d9534f }
      .error h1 { color: var(--text) }
    )");
}

QString pageShell(const QString &title, const QString &body, const QString &extraClass = {})
{
    return QStringLiteral("<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
                          "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
                          "<title>%1</title><style>%2</style></head>"
                          "<body><div class=\"page%4\">%3</div></body></html>")
        .arg(escape(title), sharedStyles(), body, extraClass);
}

QString homePage(const QString &version, int tabCount)
{
    QString body;
    body += QStringLiteral("<div class=\"page--centered\">");
    body += QStringLiteral("<div class=\"mark\">Q</div>");
    body += QStringLiteral("<h1>OpenQBrowser</h1>");
    body += QStringLiteral("<p class=\"subtitle\">Search the web, or type an address above. "
                           "Pages are rendered by Chromium, so modern sites work.</p>");
    body += QStringLiteral("</div>");

    body += QStringLiteral("<h2>Built-in pages</h2>\n<div class=\"grid\">\n");
    const QHash<QString, QString> descriptions = {
        {QStringLiteral("home"), QStringLiteral("This page")},
        {QStringLiteral("about"), QStringLiteral("Every built-in page")},
        {QStringLiteral("version"), QStringLiteral("Build and component details")},
        {QStringLiteral("history"), QStringLiteral("Pages visited in this session")},
        {QStringLiteral("bookmarks"), QStringLiteral("Pages you have saved")},
        {QStringLiteral("blank"), QStringLiteral("An empty document")},
    };
    for (const QString &name : pageNames()) {
        body += QStringLiteral("<a class=\"card\" href=\"about:%1\">"
                               "<div class=\"title\">about:%1</div>"
                               "<div class=\"desc\">%2</div></a>\n")
                    .arg(name, escape(descriptions.value(name, QStringLiteral("A built-in page"))));
    }
    body += QStringLiteral("</div>\n");

    body += QStringLiteral("<h2>Keyboard shortcuts</h2>\n<div class=\"keys\">\n");
    const QList<QPair<QString, QString>> shortcuts = {
        {QStringLiteral("New tab"), QStringLiteral("Ctrl/⌘ + T")},
        {QStringLiteral("Close tab"), QStringLiteral("Ctrl/⌘ + W")},
        {QStringLiteral("Focus the address bar"), QStringLiteral("Ctrl/⌘ + L")},
        {QStringLiteral("Reload"), QStringLiteral("F5 or Ctrl/⌘ + R")},
        {QStringLiteral("Back and forward"), QStringLiteral("Alt + ← / →")},
        {QStringLiteral("Developer tools"), QStringLiteral("F12")},
    };
    for (const auto &entry : shortcuts) {
        body += QStringLiteral("<div class=\"row\"><span>%1</span><span><kbd>%2</kbd></span></div>\n")
                    .arg(entry.first, entry.second);
    }
    body += QStringLiteral("</div>\n");

    body += QStringLiteral("<h2>Session</h2>\n");
    body += QStringLiteral("<p class=\"meta\">Version %1 &middot; %2 tab%3 open &middot; "
                           "<a href=\"about:version\">details</a></p>\n")
                .arg(escape(version))
                .arg(tabCount)
                .arg(tabCount == 1 ? QString() : QStringLiteral("s"));

    return pageShell(QStringLiteral("New Tab"), body);
}

QString versionPage(const QString &version)
{
    QString body = QStringLiteral("<h1>Version</h1>\n");
    body += QStringLiteral("<p class=\"subtitle\"><span class=\"badge\">%1</span></p>\n")
                .arg(escape(version));

    body += QStringLiteral("<h2>Rendering</h2>\n<div class=\"grid\">\n");
    body += QStringLiteral("<div class=\"card\"><div class=\"title\">Qt WebEngine</div>"
                           "<div class=\"desc\">Chromium: HTML, CSS, JavaScript and HTML5 "
                           "media</div></div>\n");
    body += QStringLiteral("<div class=\"card\"><div class=\"title\">Chromium version</div>"
                           "<div class=\"desc\" id=\"chrome\">…</div></div>\n");
    body += QStringLiteral("</div>\n");

    body += QStringLiteral("<h2>Core library</h2>\n<p>The browser was built from scratch "
                           "before it adopted Chromium, and that pipeline is still compiled "
                           "and tested. It is what the unit and integration suites exercise.</p>\n");
    body += QStringLiteral("<div class=\"keys\">\n");
    const QList<QPair<QString, QString>> components = {
        {QStringLiteral("network"), QStringLiteral("HTTP/1.1, TLS, redirects, cookies, caching")},
        {QStringLiteral("html"), QStringLiteral("Tokenizer, tree construction, entities")},
        {QStringLiteral("css"), QStringLiteral("Tokenizer, selectors, cascade, media queries")},
        {QStringLiteral("dom"), QStringLiteral("Node tree with attributes and classes")},
        {QStringLiteral("renderer"), QStringLiteral("Box tree, layout, painting")},
        {QStringLiteral("javascript"), QStringLiteral("Embedded QuickJS engine and bindings")},
        {QStringLiteral("storage"), QStringLiteral("History, bookmarks, cookies")},
    };
    for (const auto &entry : components) {
        body += QStringLiteral("<div class=\"row\"><span><code>%1</code></span>"
                               "<span>%2</span></div>\n")
                    .arg(entry.first, entry.second);
    }
    body += QStringLiteral("</div>\n");

    body += QStringLiteral("<p class=\"meta\">Qt WebEngine reports its Chromium version in "
                           "the card above. See <a href=\"about:home\">the start page</a>.</p>\n");

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

    body += QStringLiteral("<div class=\"list\">\n");
    for (int i = history->count() - 1; i >= 0; --i) {
        const storage::HistoryEntry &entry = history->at(i);
        const QString title = entry.title.isEmpty() ? entry.url.toString() : entry.title;
        body += QStringLiteral("<a class=\"item\" href=\"%1\">%2")
                    .arg(escape(entry.url.toString()), escape(title));
        body += QStringLiteral("<div class=\"host\">%1").arg(escape(entry.url.displayHost()));
        if (entry.visitCount > 1)
            body += QStringLiteral(" &middot; %1 visits").arg(entry.visitCount);
        body += QStringLiteral("</div></a>\n");
    }
    body += QStringLiteral("</div>\n");

    return pageShell(QStringLiteral("History"), body);
}

QString bookmarksPage(const storage::BookmarkStore *bookmarks)
{
    if (!bookmarks || bookmarks->isEmpty()) {
        return pageShell(QStringLiteral("Bookmarks"),
                         QStringLiteral("<h1>Bookmarks</h1><p class=\"subtitle\">Nothing saved "
                                        "yet. Press the star in the toolbar to keep the page you "
                                        "are on.</p>"));
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
    body += QStringLiteral("<p class=\"subtitle\">These pages come from inside the browser, "
                           "so they work with no network connection.</p>\n");

    const QHash<QString, QString> descriptions = {
        {QStringLiteral("home"), QStringLiteral("The new tab page")},
        {QStringLiteral("about"), QStringLiteral("This list")},
        {QStringLiteral("version"), QStringLiteral("Build and component details")},
        {QStringLiteral("history"), QStringLiteral("Pages visited in this session")},
        {QStringLiteral("bookmarks"), QStringLiteral("Pages you have saved")},
        {QStringLiteral("blank"), QStringLiteral("An empty document")},
    };

    body += QStringLiteral("<div class=\"grid\">\n");
    for (const QString &name : pageNames()) {
        body += QStringLiteral("<a class=\"card\" href=\"about:%1\">"
                               "<div class=\"title\">about:%1</div>"
                               "<div class=\"desc\">%2</div></a>\n")
                    .arg(name, escape(descriptions.value(name, QStringLiteral("A built-in page"))));
    }
    body += QStringLiteral("</div>\n");

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
    body += QStringLiteral("<p class=\"subtitle\">%1</p>\n").arg(explanation);

    if (!url.toString().isEmpty() && kind != QLatin1String("notfound")) {
        body += QStringLiteral("<p class=\"meta\">%1</p>\n").arg(escape(url.toString()));
    }
    if (!hint.isEmpty())
        body += QStringLiteral("<p>%1</p>\n").arg(hint);
    if (!details.isEmpty() && kind != QLatin1String("notfound")) {
        body += QStringLiteral("<div class=\"card\"><p class=\"meta\" style=\"margin:0\">%1</p></div>\n")
                    .arg(escape(details));
    }

    body += QStringLiteral("<p style=\"margin-top:24px\"><a href=\"about:home\">Start page</a></p>\n");

    return pageShell(heading, body, QStringLiteral(" error"));
}

} // namespace oqb::browser::builtin
