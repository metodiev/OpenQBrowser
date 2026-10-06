#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <QHash>
#include <QSizeF>

#include <memory>

#include "browser/PageSettings.h"
#include "css/Style.h"
#include "html/Parser.h"
#include "javascript/Engine.h"
#include "javascript/Events.h"
#include "javascript/ScriptEngine.h"
#include "network/ResourceLoader.h"
#include "renderer/Layout.h"
#include "storage/Bookmarks.h"
#include "storage/History.h"

namespace oqb::renderer {
class Box;
}

namespace oqb::browser {

/// A loaded page: its document, styles, box tree and layout.
///
/// A Page owns the whole pipeline for one navigation. Everything it needs is a
/// member, so a Page can be created, driven to completion and queried without
/// any global state, which is what makes the headless and test paths identical
/// to the windowed one.
///
/// Lifecycle:
///   load(url)  ->  started() ... finished() or failed()
///   then document(), boxTree() and layout() describe what to draw.
class Page : public QObject
{
    Q_OBJECT

public:
    explicit Page(const PageSettings &settings = {}, QObject *parent = nullptr);
    ~Page() override;

    /// Begins loading `url`. A previous load is abandoned first.
    void load(const network::Url &url);

    /// Reloads the current URL, bypassing the cache.
    void reload();

    /// Stops the load in progress.
    void stop();

    bool isLoading() const { return m_state != State::Idle; }
    const network::Url &url() const { return m_url; }
    /// The URL after any redirects, or the requested one while loading.
    const network::Url &finalUrl() const { return m_finalUrl.isValid() ? m_finalUrl : m_url; }

    const dom::Document *document() const { return m_document.get(); }
    dom::Document *document() { return m_document.get(); }
    const css::StyleEngine *styles() const { return m_styles.get(); }
    const renderer::Box *boxTree() const { return m_boxTree.get(); }
    renderer::Box *boxTree() { return m_boxTree.get(); }
    const renderer::LayoutResult &layout() const { return m_layout; }

    /// The page title: <title>, then the first heading, then the host.
    QString title() const;

    /// True when the load ended in an error page rather than the requested
    /// document.
    bool isErrorPage() const { return m_errorPage; }
    QString errorMessage() const { return m_error; }

    /// The JavaScript engine the page's scripts run in. Its console messages and
    /// error reports are what the DevTools panel shows.
    javascript::ScriptEngine *scripts() { return m_scripts.get(); }
    const javascript::ScriptEngine *scripts() const { return m_scripts.get(); }

    /// The engine behind the ScriptEngine seam, for callers that need the timers
    /// or the document-touched flag. Null when no engine is available.
    javascript::Engine *engine();

    /// Runs any timer callbacks that are due, and re-lays the page out when a
    /// script changed the document. The browser's frame timer calls this; a
    /// headless caller can call it directly with its own clock.
    void serviceScripts(qint64 nowMs);

    /// True when page scripts actually run, which is false only in a build
    /// without a JavaScript engine.
    bool runsScripts() const;

    /// How many navigations this page has performed. It changes for every load,
    /// including a reload, and never repeats.
    ///
    /// A consumer that caches anything about the document - the DevTools panel
    /// caches its element tree - needs this rather than a document pointer,
    /// because the allocator reuses the address of the document it just freed.
    /// Comparing pointers therefore reports "same document" after a navigation,
    /// which is how a stale tree survived a page change.
    quint64 navigationId() const { return m_navigationId; }

    /// True while a script has a timer or animation frame waiting, so the
    /// browser knows whether its frame clock has anything to do.
    bool hasPendingScriptWork() const;

    /// The external resources that were requested, for the DevTools panel.
    QStringList requestedResources() const { return m_requestedResources; }
    /// Resources that failed to load, with the reason.
    QStringList failedResources() const { return m_failedResources; }

    /// Links the history and bookmark stores so the page can record visits and
    /// resolve about: pages against live data.
    void setHistory(storage::HistoryStore *history) { m_history = history; }
    void setBookmarks(storage::BookmarkStore *bookmarks) { m_bookmarks = bookmarks; }

    /// The number of open tabs, used by the new tab page's summary line.
    void setTabCount(int count) { m_tabCount = qMax(1, count); }

    const PageSettings &settings() const { return m_settings; }
    void setSettings(const PageSettings &settings);

    /// Renders the page to an image at the page's viewport size. Used by the
    /// screenshot mode and by the tests.
    QImage renderToImage() const;

signals:
    /// The navigation has started, before any bytes arrive.
    void started(const oqb::network::Url &url);
    /// A redirect moved the navigation to `to`.
    void redirected(const oqb::network::Url &from, const oqb::network::Url &to);
    /// The main document has been parsed, styled and laid out. The page is
    /// paintable at this point, even if subresources are still arriving.
    void ready();
    /// Subresources have been resolved and the page is as complete as it will get.
    void finished();
    /// The load failed; an error page has been built and be displayed.
    void failed(const QString &message);
    /// The document title, once it is known.
    void titleChanged(const QString &title);

private:
    enum class State { Idle, LoadingDocument, LoadingSubresources };

    void handleDocumentResource(const network::Resource &resource);
    /// Builds a page served from inside the browser, under the about: scheme.
    void loadBuiltinPage(const oqb::network::Url &url);
    void buildDocument(const oqb::network::Resource &resource);
    /// Fires the remaining lifecycle events once nothing is outstanding.
    void finishLoading();
    /// Collects the document's classic scripts in the order they must run and
    /// asks the loader for the external ones. Running happens once every source
    /// is available, because a classic script must see the document as the
    /// parser left it and must run before the next script is fetched.
    void planScripts();
    /// Runs every planned script that has its source available, in order,
    /// stopping at the first one that is still loading.
    void runReadyScripts();
    /// Runs one script element; `source` is its code when it is external.
    void runScriptElement(dom::Element *element, const QString &source);
    /// True when the element is a classic script that should execute.
    static bool isRunnableScript(const dom::Element *element);
    /// Whether the element's script runs after the document is parsed rather
    /// than where it appears.
    static bool isDeferredScript(const dom::Element *element);
    /// Records the source of a script the loader delivered, returning true when
    /// it was one the plan was waiting for.
    bool deliverScriptSource(const network::Url &url, const QString &source);
    /// True while a script fetch is outstanding.
    bool hasPendingScripts() const;
    /// Fires the lifecycle events a page listens for, in order.
    void fireDomContentLoaded();
    void fireLoadEvent();
    void collectSubresources();
    void requestNextSubresource();
    void handleSubresource(const oqb::network::Resource &resource);
    void finishWithError(const QString &kind, const QString &details);
    void buildLayout();
    void recordHistory();

    /// Marks the layout as needing to be rebuilt and schedules a single pass.
    /// Coalescing matters: without it a page with many images is laid out once
    /// per image, which is quadratic in the number of subresources.
    void scheduleRelayout();

    PageSettings m_settings;
    State m_state = State::Idle;
    network::Url m_url;
    network::Url m_finalUrl;
    bool m_errorPage = false;
    QString m_error;

    std::unique_ptr<network::ResourceLoader> m_loader;
    dom::Document *m_parsedDocument = nullptr;
    html::ParseResult m_parseResult;
    std::unique_ptr<dom::Document> m_document;
    std::unique_ptr<css::StyleEngine> m_styles;
    std::unique_ptr<renderer::Box> m_boxTree;
    renderer::LayoutResult m_layout;
    std::unique_ptr<javascript::ScriptEngine> m_scripts;

    /// One script waiting to run, with the URL its source comes from.
    struct PendingScript
    {
        dom::Element *element = nullptr;
        /// Empty for an inline script.
        network::Url url;
        /// True when the script waits for the document to be parsed.
        bool deferred = false;
        /// True once the source is in `source`, or when the script is inline.
        bool ready = true;
        /// The code to run.
        QString source;
    };

    /// The scripts of the current document, in the order they must run.
    std::vector<PendingScript> m_scriptsToRun;
    /// How many script fetches are outstanding. A page is not past its script
    /// phase while this is non-zero.
    int m_pendingScripts = 0;
    /// True once the document's own fetch has been answered. A URL comparison is
    /// not enough to identify the document, because a redirect answers at a URL
    /// that was never requested.
    bool m_documentArrived = false;
    /// True once DOMContentLoaded has fired, so it fires exactly once.
    bool m_domContentLoadedFired = false;
    /// Incremented by every load, so a consumer can tell that the document it
    /// described has been replaced.
    quint64 m_navigationId = 0;

    /// Decoded image sizes by URL, so that a rebuilt box tree still knows how
    /// large each image is.
    QHash<QString, QSizeF> m_imageSizes;

    /// True while a coalesced re-layout is already scheduled.
    bool m_relayoutScheduled = false;

    /// Stylesheets that arrived after the document, waiting for the next pass.
    QStringList m_lateStylesheets;

    /// Subresources still to fetch, in the order they were discovered.
    QList<network::Url> m_pendingSubresources;
    int m_inFlightSubresources = 0;
    QStringList m_requestedResources;
    QStringList m_failedResources;

    storage::HistoryStore *m_history = nullptr;
    storage::BookmarkStore *m_bookmarks = nullptr;
    int m_tabCount = 1;

    /// The document's own URL, used to resolve relative references in the page.
    network::Url m_documentUrl;
};

} // namespace oqb::browser
