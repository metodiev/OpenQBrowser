#include <QElapsedTimer>

#include "browser/Page.h"

#include "browser/BuiltinPages.h"
#include "browser/PageSettings.h"
#include "renderer/BoxTree.h"
#include "renderer/Painter.h"
#include "security/SecurityPolicy.h"

#include <QImage>

#include <algorithm>
#include <functional>

namespace oqb::browser {
namespace {

/// Classifies a load failure so the error page can explain it precisely.
QString classifyFailure(const QString &message)
{
    const QString lowered = message.toLower();

    if (lowered.contains(QLatin1String("host not found"))
        || lowered.contains(QLatin1String("name resolution"))
        || lowered.contains(QLatin1String("no such host"))
        || lowered.contains(QLatin1String("dns"))) {
        return QStringLiteral("dns");
    }
    if (lowered.contains(QLatin1String("timed out")))
        return QStringLiteral("timeout");
    if (lowered.contains(QLatin1String("certificate")) || lowered.contains(QLatin1String("tls"))
        || lowered.contains(QLatin1String("ssl"))) {
        return QStringLiteral("tls");
    }
    if (lowered.contains(QLatin1String("refused")) || lowered.contains(QLatin1String("closed"))
        || lowered.contains(QLatin1String("connection"))) {
        return QStringLiteral("connect");
    }
    return QStringLiteral("network");
}

/// How many images a page may reference before the rest are skipped. A hostile
/// or generated page can name thousands; the cap keeps a single load bounded.
constexpr int kMaxSubresources = 200;

/// How long to wait for a burst of subresources to stop arriving before laying
/// the page out again, as a floor and as a multiple of the last pass.
///
/// The floor keeps a small page responsive. The multiple is what makes the
/// coalescing actually work on a large one: laying out a real news front page
/// takes several hundred milliseconds, and asking about once per image means the
/// page is never laid out at all until the burst ends - every request answers
/// with an image that has already arrived, so the pass is pure waste. Scaling the
/// wait to the cost of a pass turns twenty-three passes into two.
constexpr int kRelayoutDebounceMs = 60;
constexpr double kRelayoutDebounceFactor = 1.5;
constexpr int kRelayoutDebounceMaxMs = 2000;

} // namespace

Page::Page(const PageSettings &settings, QObject *parent)
    : QObject(parent)
    , m_settings(settings)
    // The concrete engine, not the base class: the base class is the seam a
    // build without QuickJS falls back to, and it runs nothing.
    , m_scripts(std::make_unique<javascript::QuickJsScriptEngine>())
{
    m_loader = std::make_unique<network::ResourceLoader>(this);
    m_loader->setMaxConcurrentRequests(m_settings.maxConcurrentRequests);
    m_loader->setUserAgent(m_settings.userAgent);
    m_loader->setRequestTimeout(m_settings.requestTimeoutMs);

    connect(m_loader.get(), &network::ResourceLoader::finished, this,
            [this](const network::Resource &resource) {
                // The state alone does not say what a resource is. A file://
                // script is read without any asynchronous step, so it completes
                // while the document is still being built; routing by state
                // would then feed the script's source to the HTML parser, or
                // drop it once the page had moved on.
                //
                // The document is identified by a flag rather than by its URL,
                // because a redirect answers at a URL that was never requested.
                // It is the first response that arrives while the document is
                // still being awaited, and there is exactly one of those.
                if (!m_documentArrived && m_state == State::LoadingDocument) {
                    handleDocumentResource(resource);
                    return;
                }

                handleSubresource(resource);
            });
}

Page::~Page() = default;

void Page::setCookieJar(storage::CookieJar *cookies)
{
    m_cookies = cookies;
    if (m_loader)
        m_loader->setCookieJar(cookies);
}

void Page::setSettings(const PageSettings &settings)
{
    m_settings = settings;
    if (m_loader) {
        m_loader->setMaxConcurrentRequests(settings.maxConcurrentRequests);
        m_loader->setUserAgent(settings.userAgent);
        m_loader->setRequestTimeout(settings.requestTimeoutMs);
    }
}

void Page::load(const network::Url &url)
{
    if (!url.isValid())
        return;

    stop();

    // Everything from here on belongs to a new load, so the loader stops treating
    // the previous page's entries as this load's memo. They remain available
    // under their own freshness rules, which is what makes navigating back to a
    // page cheap.
    if (m_loader)
        m_loader->beginLoad();

    m_url = url;
    m_finalUrl = url;
    m_errorPage = false;
    m_error.clear();
    m_requestedResources.clear();
    m_failedResources.clear();
    m_pendingSubresources.clear();
    m_inFlightSubresources = 0;
    m_scriptsToRun.clear();
    m_pendingScripts = 0;
    m_documentArrived = false;
    // Every load is a new navigation, so anything cached about the previous
    // document is invalidated by this changing.
    ++m_navigationId;
    m_domContentLoadedFired = false;
    m_document.reset();
    m_parsedDocument = nullptr;
    m_boxTree.reset();
    m_layout = renderer::LayoutResult();
    m_scripts->clearMessages();

    emit started(url);

    if (url.isAbout()) {
        loadBuiltinPage(url);
        return;
    }

    m_state = State::LoadingDocument;
    m_loader->fetch(url);
}

void Page::loadBuiltinPage(const oqb::network::Url &url)
{
    const QString name = url.aboutPage();

    // An unknown about: page becomes an error page; a known one is generated
    // here so it works offline and cannot be redirected by the network.
    if (!builtin::hasPage(name) && !name.isEmpty()) {
        finishWithError(QStringLiteral("notfound"),
                        QStringLiteral("There is no built-in page called \"%1\".").arg(name));
        return;
    }

    const QString html = builtin::documentFor(url, QStringLiteral(OPENQBROWSER_VERSION),
                                             m_settings.viewportWidth, m_history, m_bookmarks,
                                             m_tabCount);

    network::Resource resource;
    resource.url = url;
    resource.mimeType = QStringLiteral("text/html; charset=utf-8");
    resource.data = html.toUtf8();
    resource.statusCode = 200;

    buildDocument(resource);
}

void Page::reload()
{
    if (!m_documentUrl.isValid())
        return;

    // A reload means "ask the origin again", so every response the page is about
    // to request is marked stale. The document itself is included: a reload that
    // served the document from cache would not be a reload at all.
    if (m_loader) {
        m_loader->invalidate(m_documentUrl);
        for (const network::Url &url : m_pendingSubresources)
            m_loader->invalidate(url);
    }

    load(m_documentUrl);
}

void Page::stop()
{
    if (m_loader) {
        m_loader->cancelAll();
        // The cache deliberately survives a stop. A navigation no longer throws
        // away every response the previous page fetched, so a stylesheet or logo
        // shared by two pages is revalidated rather than downloaded again. The
        // loader applies the response's own freshness rules instead, and
        // beginLoad() below is what stops one load's entries from being treated
        // as this load's memo.
    }
    m_state = State::Idle;
    m_inFlightSubresources = 0;
    m_pendingSubresources.clear();
}

QString Page::title() const
{
    if (m_document) {
        const QString title = m_document->title();
        if (!title.isEmpty())
            return title;
    }
    if (m_errorPage && !m_error.isEmpty())
        return m_error;
    return m_url.displayHost();
}

QImage Page::renderToImage() const
{
    if (!m_boxTree)
        return {};

    renderer::Painter::Options options;
    options.defaultBackground = m_settings.defaultBackground;
    options.defaultTextColor = m_settings.defaultTextColor;

    return renderer::Painter::renderToImage(m_boxTree.get(),
                                            static_cast<int>(m_settings.viewportWidth),
                                            static_cast<int>(m_settings.viewportHeight),
                                            options);
}

// ------------------------------------------------------------- document load

void Page::handleDocumentResource(const network::Resource &resource)
{
    // Whatever arrives first is the document, redirect or not.
    m_documentArrived = true;
    m_finalUrl = resource.url.isValid() ? resource.url : m_url;

    if (!resource.ok()) {
        // An HTTP error still carries a body, and browsers show it rather than
        // an error page, so the document is parsed and only the failure is noted.
        if (resource.statusCode >= 400 && !resource.data.isEmpty()) {
            buildDocument(resource);
            m_failedResources.append(QStringLiteral("%1: HTTP %2")
                                         .arg(resource.url.toString())
                                         .arg(resource.statusCode));
            return;
        }

        const QString kind = classifyFailure(resource.error);
        finishWithError(kind, resource.error);
        return;
    }

    buildDocument(resource);
}

void Page::buildDocument(const network::Resource &resource)
{
    m_documentUrl = resource.url.isValid() ? resource.url : m_url;

    // The security policy is consulted before anything is parsed, so a refused
    // page never reaches the renderer.
    security::SecurityPolicy policy;
    if (const security::Decision decision = policy.checkTopLevelNavigation(m_documentUrl);
        !decision.allowed) {
        finishWithError(QStringLiteral("blocked"), decision.reason);
        return;
    }
    // A host reached over HTTPS is remembered, so a later plaintext reference to
    // it is refused rather than silently downgraded.
    if (m_documentUrl.isHttps())
        policy.rememberSecureHost(m_documentUrl.host());

    const QString text = resource.text();

    m_parseResult = html::Parser::parse(text, m_documentUrl);
    m_parsedDocument = m_parseResult.document.get();

    // The document is moved into the member so it outlives the parse result,
    // which the rest of the page then reads through m_document.
    m_document = std::move(m_parseResult.document);

    if (!m_document) {
        finishWithError(QStringLiteral("network"),
                        QStringLiteral("The document could not be parsed."));
        return;
    }
    m_parsedDocument = m_document.get();

    // The engine was created before there was a document to point it at, so the
    // bindings are told about this one before any script runs. Without this,
    // `document` would be null inside every script and the lifecycle events
    // would have nothing to dispatch to.
    if (javascript::Engine *eng = engine()) {
        eng->setDocument(m_document.get());

        // fetch() sends its requests through the same loader the page uses, so a
        // request from script shares the cache, the cookie jar, the security
        // policy and the connection limit with the requests the page itself
        // makes. Without a provider, fetch would reject rather than run, so this
        // is what makes the function real.
        if (m_loader)
            eng->setFetchProvider(m_loader.get());
    }

    // The scripts are planned before the subresources are collected, because a
    // script may insert elements that reference images or stylesheets of its
    // own, and those have to be fetched too. An external script is itself a
    // subresource, so it is requested at this point as well.
    if (!m_scripts->isAvailable()) {
        // Without an engine the scripts are listed rather than run and the
        // document still renders. Showing the page beats showing nothing, and
        // the inspector explains why nothing ran.
        for (dom::Element *element : m_document->getElementsByTagName(QStringLiteral("script"))) {
            const QString src = element->attribute(QStringLiteral("src"));
            m_scripts->noteSkippedScript(src.isEmpty()
                                             ? QStringLiteral("inline script")
                                             : m_document->resolveUrl(src).toString());
        }
        fireDomContentLoaded();
    } else {
        planScripts();
        // The inline scripts run now; the external ones run as their sources
        // arrive, which is what keeps them in document order.
        runReadyScripts();
    }

    m_state = State::LoadingSubresources;
    collectSubresources();

    // The layout is built before subresources arrive so the page is paintable
    // immediately, which is what a browser does with an incomplete document.
    buildLayout();

    emit ready();

    if (m_pendingSubresources.isEmpty() && m_pendingScripts == 0) {
        // Nothing left to wait for, so the page is as loaded as it will get and
        // the load event fires now.
        finishLoading();
        return;
    }

    requestNextSubresource();
}

void Page::finishLoading()
{
    // The document is fully parsed and every script has had its chance to run,
    // so the events a page waits for can be fired. Whatever is still in the plan
    // has no source and is reported rather than silently dropped.
    for (const PendingScript &script : m_scriptsToRun) {
        if (script.element && script.url.isValid())
            m_scripts->noteSkippedScript(script.url.toString());
    }
    m_scriptsToRun.clear();

    fireDomContentLoaded();
    fireLoadEvent();

    recordHistory();
    m_state = State::Idle;
    emit finished();
}

void Page::planScripts()
{
    if (!m_document)
        return;

    m_scriptsToRun.clear();
    m_pendingScripts = 0;

    // A module script executes after the document is parsed, and its imports
    // would need a module loader this engine does not have, so it is reported
    // rather than run. The attribute is still honoured as "do not run now" so
    // that a page using modules does not execute code out of order.
    const auto scriptElements
        = m_document->getElementsByTagName(QStringLiteral("script"));

    for (dom::Element *element : scriptElements) {
        const QString type = element->attribute(QStringLiteral("type")).trimmed().toLower();

        if (!isRunnableScript(element)) {
            // A data block such as <script type="application/json"> is not code.
            continue;
        }

        if (type == QLatin1String("module")) {
            m_scripts->noteSkippedScript(
                QStringLiteral("%1 (module scripts are not supported)")
                    .arg(element->attribute(QStringLiteral("src")).isEmpty()
                             ? QStringLiteral("inline module")
                             : element->attribute(QStringLiteral("src"))));
            continue;
        }

        PendingScript script;
        script.element = element;
        script.deferred = isDeferredScript(element);

        const QString src = element->attribute(QStringLiteral("src"));
        if (src.isEmpty()) {
            // An inline script has its code in the document, so it is ready now.
            script.source = element->textContent();
            script.url = m_documentUrl;
        } else {
            script.url = m_document->resolveUrl(src);
            script.ready = !script.url.isValid();
            if (!script.url.isValid()) {
                m_failedResources.append(
                    QStringLiteral("%1: the script URL could not be resolved").arg(src));
            }
        }

        m_scriptsToRun.push_back(script);
    }

    // The deferred scripts are moved to the end, keeping their relative order,
    // which is what the specification asks for: they run after every classic
    // script and after the document is parsed.
    std::stable_partition(m_scriptsToRun.begin(), m_scriptsToRun.end(),
                          [](const PendingScript &script) { return !script.deferred; });

    // The external scripts are requested through the loader. The URLs are
    // gathered first and fetched afterwards, because a fetch may complete
    // synchronously - a file:// URL is read without any asynchronous step - and
    // the completion path runs scripts, which replaces m_scriptsToRun. Fetching
    // while iterating over it would invalidate the iterator.
    std::vector<network::Url> toFetch;
    for (const PendingScript &script : m_scriptsToRun) {
        if (!script.ready)
            toFetch.push_back(script.url);
    }

    for (const network::Url &url : toFetch) {
        ++m_pendingScripts;
        if (m_requestedResources.size() < kMaxSubresources + 16)
            m_requestedResources.append(url.toString());
        m_loader->fetch(url, m_documentUrl.toString());
    }
}

bool Page::isRunnableScript(const dom::Element *element)
{
    // An absent type means JavaScript. An explicit type must be a JavaScript
    // MIME type, which is what keeps a JSON data block from being executed.
    const QString type = element->attribute(QStringLiteral("type")).trimmed().toLower();
    if (type.isEmpty())
        return true;

    return type == QLatin1String("text/javascript")
        || type == QLatin1String("application/javascript")
        || type == QLatin1String("text/ecmascript")
        || type == QLatin1String("application/ecmascript")
        || type == QLatin1String("application/x-javascript")
        || type == QLatin1String("module") || type == QLatin1String("text/babel");
}

bool Page::isDeferredScript(const dom::Element *element)
{
    // defer and async both mean "not where it appears". async would run as soon
    // as it arrived rather than in order, but the document is already parsed
    // here, so running in document order is the closest honest behaviour and is
    // what a page that relies on async for ordering must not depend on anyway.
    return element->hasAttribute(QStringLiteral("defer"))
        || element->hasAttribute(QStringLiteral("async"));
}

void Page::runReadyScripts()
{
    if (!m_document || m_pendingScripts > 0)
        return;

    // A classic script must run with the document as it stands, and a script
    // that inserts another one changes the list, so the plan is walked by index
    // rather than by iterator and re-read each time.
    std::vector<PendingScript> remaining;
    remaining.reserve(m_scriptsToRun.size());

    for (const PendingScript &script : m_scriptsToRun) {
        // A script whose source is still in flight is kept for the next pass;
        // the ones after it must not be run before it.
        if (!script.ready) {
            remaining.push_back(script);
            continue;
        }

        // A script removed by an earlier one does not run, which is what the
        // specification says and what a page that cleans up after itself needs.
        if (!script.element || !script.element->parent())
            continue;

        const bool external = !script.element->attribute(QStringLiteral("src")).isEmpty();
        if (external && script.source.isEmpty()) {
            // The fetch failed; the failure list already explains why.
            m_scripts->noteSkippedScript(script.url.toString());
            continue;
        }

        runScriptElement(script.element, external ? script.source : QString());
    }

    m_scriptsToRun = std::move(remaining);
}

void Page::runScriptElement(dom::Element *element, const QString &source)
{
    if (!m_document || !element)
        return;

    const QString src = element->attribute(QStringLiteral("src"));

    // An inline script is named by the document URL, since it has no location of
    // its own; an external one by its own URL, which is what a stack trace should
    // show.
    const QString sourceName = src.isEmpty() ? m_documentUrl.toString() + QStringLiteral("#inline")
                                             : element->attribute(QStringLiteral("src"));

    if (src.isEmpty()) {
        const QString code = element->textContent();
        if (code.trimmed().isEmpty())
            return; // An empty script element is legal and does nothing.
        m_scripts->execute(code, m_document.get(), sourceName);
        return;
    }

    if (!source.isEmpty()) {
        m_scripts->execute(source, m_document.get(), sourceName);
        return;
    }

    // The source never arrived, which the failure list already explains.
    m_scripts->noteSkippedScript(sourceName);
}

void Page::fireDomContentLoaded()
{
    if (m_domContentLoadedFired || !m_document)
        return;
    m_domContentLoadedFired = true;

#ifdef OPENQBROWSER_SCRIPTING
    // A page reads readyState to decide whether it may query the DOM, so it is
    // updated before the event rather than after.
    if (javascript::Engine *eng = engine()) {
        eng->setReadyState(QStringLiteral("interactive"));
        javascript::Events::dispatchLifecycle(eng->context(), m_document.get(),
                                              QStringLiteral("DOMContentLoaded"), true, false);
    }
#else
    // Without an engine there are no listeners, so there is nothing to dispatch.
#endif
}

void Page::fireLoadEvent()
{
    if (!m_document)
        return;

#ifdef OPENQBROWSER_SCRIPTING
    if (javascript::Engine *eng = engine()) {
        eng->setReadyState(QStringLiteral("complete"));
        javascript::Events::dispatchLifecycle(eng->context(), m_document.get(),
                                              QStringLiteral("load"), false, false);
    }
#endif
}

void Page::serviceScripts(qint64 nowMs)
{
    javascript::Engine *eng = engine();
    if (!eng)
        return;

#ifdef OPENQBROWSER_SCRIPTING

    eng->runDueTimers(nowMs);

    // A timer that changed the document has to be re-styled and re-laid out, or
    // the change would not be visible until something else caused a layout.
    if (eng->takeDocumentTouched()) {
        if (m_styles)
            m_styles->computeStyles(m_document.get());
        buildLayout();
        emit ready();
    }

    // The page has stopped doing work of its own. A caller waiting for a script
    // to fill the document in - a headless dump, a test - waits for this rather
    // than for finished(), which only reports the end of loading.
    if (!hasPendingScriptWork())
        emit settled();
#else
    Q_UNUSED(nowMs);
#endif
}

bool Page::runsScripts() const
{
    return m_scripts && m_scripts->isAvailable();
}

bool Page::hasPendingScriptWork() const
{
    const auto *concrete = dynamic_cast<const javascript::QuickJsScriptEngine *>(m_scripts.get());
    if (!concrete || !concrete->engine())
        return false;
    return concrete->engine()->hasPendingWork();
}

javascript::Engine *Page::engine()
{
    auto *concrete = dynamic_cast<javascript::QuickJsScriptEngine *>(m_scripts.get());
    return concrete ? concrete->engine() : nullptr;
}

void Page::collectSubresources()
{
    if (!m_document)
        return;

    const auto add = [this](const QString &value) {
        if (value.isEmpty() || m_pendingSubresources.size() >= kMaxSubresources)
            return;
        const network::Url resolved = m_document->resolveUrl(value);
        if (resolved.isValid())
            m_pendingSubresources.append(resolved);
    };

    // Stylesheets, in document order, so the cascade sees them in the order the
    // author wrote them.
    if (m_settings.loadExternalStylesheets) {
        for (dom::Element *element : m_document->getElementsByTagName(QStringLiteral("link"))) {
            const QString rel = element->attribute(QStringLiteral("rel")).toLower();
            if (rel.contains(QLatin1String("stylesheet")))
                add(element->attribute(QStringLiteral("href")));
        }
    }

    if (m_settings.loadImages) {
        for (dom::Element *element : m_document->getElementsByTagName(QStringLiteral("img"))) {
            const QString src = element->attribute(QStringLiteral("src"));
            // An inline data: URL needs no fetch.
            if (!src.startsWith(QLatin1String("data:"), Qt::CaseInsensitive))
                add(src);
        }
    }
}

void Page::requestNextSubresource()
{
    // The loader already queues internally, so every remaining resource is
    // handed over at once; it enforces the concurrency limit itself.
    const security::SecurityPolicy policy;

    while (!m_pendingSubresources.isEmpty()) {
        const network::Url url = m_pendingSubresources.takeFirst();

        // The policy is consulted per resource, not once for the document: a
        // secure page must not pull in an insecure subresource, and saying so
        // here is what makes that rule real rather than documented.
        if (const security::Decision decision = policy.canLoadSubresource(m_documentUrl, url);
            !decision.allowed) {
            m_failedResources.append(
                QStringLiteral("%1: blocked (%2)").arg(url.toString(), decision.reason));
            continue;
        }

        ++m_inFlightSubresources;
        m_requestedResources.append(url.toString());
        m_loader->fetch(url, m_documentUrl.toString());
    }
}

void Page::handleSubresource(const network::Resource &resource)
{
    // A script is fetched through the loader as well, but it is counted in
    // m_pendingScripts rather than in m_inFlightSubresources, so the two counts
    // are kept apart. The URL it was requested at is what identifies it, since a
    // redirect answers at a different one.
    const network::Url requestUrl
        = resource.requestedUrl.isValid() ? resource.requestedUrl : resource.url;
    const bool isScriptFetch
        = deliverScriptSource(requestUrl, resource.ok() ? resource.text() : QString());

    if (!isScriptFetch && m_inFlightSubresources > 0)
        --m_inFlightSubresources;

    if (isScriptFetch) {
        // Running the plan here is what keeps the execution order right: a script
        // that is ready runs before the ones after it, and a script still in
        // flight holds them back.
        runReadyScripts();
    }

    if (!resource.ok()) {
        m_failedResources.append(QStringLiteral("%1: %2")
                                     .arg(resource.url.toString(), resource.error));

    } else if (resource.isCss()) {
        // A late stylesheet still takes effect: the cascade is re-run and the
        // page is laid out again, which is what a browser does when a stylesheet
        // arrives after the document.
        if (m_styles) {
            // A late stylesheet is remembered and applied by the next pass, so
            // that several arriving together still cost one layout.
            m_lateStylesheets.append(resource.text());
            scheduleRelayout();
        }
    } else if (resource.isImage()) {
        // The decoded size is remembered against the URL rather than against a
        // box, because the box tree is rebuilt for every layout pass. The next
        // pass then gives the image its real dimensions.
        QImage image;
        image.loadFromData(resource.data);
        if (!image.isNull()) {
            m_imageSizes.insert(resource.url.toString(), image.size());
            // The re-layout is coalesced: a page with many images pays for one
            // pass per burst, not one pass per image.
            scheduleRelayout();
        }
    }

    // Only the subresource phase may end the load. A script source that arrives
    // while the document is still being built takes this path too, and ending the
    // load there would leave the scripts after it unrunnable.
    if (m_state != State::LoadingSubresources)
        return;

    if (m_inFlightSubresources == 0 && m_pendingSubresources.isEmpty() && m_pendingScripts == 0) {
        // One last pass, so a stylesheet or image that arrived with the final
        // subresource is reflected in the layout the caller sees. The coalesced
        // pass may not have run yet, and after this point it never would.
        m_state = State::Idle;
        buildLayout();
        emit ready();

        finishLoading();
    }
}

bool Page::deliverScriptSource(const network::Url &url, const QString &source)
{
    bool delivered = false;

    for (PendingScript &script : m_scriptsToRun) {
        if (script.ready || script.url != url)
            continue;

        // The source is handed over and the slot marked ready, but the script is
        // not run here: runReadyScripts walks the plan in order, so a script that
        // is still loading holds back the ones after it, which is what classic
        // script ordering requires.
        script.source = source;
        script.ready = true;
        delivered = true;
    }

    if (delivered && m_pendingScripts > 0)
        --m_pendingScripts;

    return delivered;
}











bool Page::hasPendingScripts() const
{
    return m_pendingScripts > 0;
}

void Page::scheduleRelayout()
{
    if (m_relayoutScheduled)
        return;
    m_relayoutScheduled = true;

    if (!m_relayoutTimer) {
        m_relayoutTimer = new QTimer(this);
        m_relayoutTimer->setSingleShot(true);
        // A burst of images arrives one per event-loop turn, so waiting for the
        // turn to end is not enough to coalesce them: the flag is cleared before
        // the next one lands. Waiting a few milliseconds for the burst to stop is
        // what actually turns fifty arriving images into one layout pass.
        m_relayoutTimer->setInterval(kRelayoutDebounceMs);
        connect(m_relayoutTimer, &QTimer::timeout, this, [this] {
            m_relayoutScheduled = false;
            if (m_state == State::Idle)
                return; // The final layout already happened.
            buildLayout();
            emit ready();
        });
    }

    m_relayoutTimer->setInterval(
        qBound(kRelayoutDebounceMs,
               static_cast<int>(m_lastLayoutMs * kRelayoutDebounceFactor),
               kRelayoutDebounceMaxMs));
    m_relayoutTimer->start();
}

void Page::buildLayout()
{
    // How long this pass takes decides how long to wait before starting the
    // next one: on a page where a pass costs hundreds of milliseconds, waiting
    // only for the event loop to drain would mean the page is laid out once per
    // arriving image, and every one of those passes is wasted because the images
    // it would place have already arrived.
    QElapsedTimer passTimer;
    passTimer.start();
    struct RecordCost
    {
        ~RecordCost() { *milliseconds = timer->nsecsElapsed() / 1e6; }
        QElapsedTimer *timer;
        double *milliseconds;
    } recordCost{&passTimer, &m_lastLayoutMs};
    if (!m_document || !m_document->documentElement()) {
        m_boxTree.reset();
        return;
    }

    // Styles are recomputed from scratch each time, so a late stylesheet cannot
    // leave stale values behind.
    css::StyleContext context;
    context.viewportWidth = m_settings.viewportWidth;
    context.viewportHeight = m_settings.viewportHeight;
    context.rootFontSize = m_settings.defaultFontSize;
    context.prefersDarkScheme = m_settings.prefersDarkScheme;

    m_styles = std::make_unique<css::StyleEngine>(context);
    m_styles->setBaseColors(m_settings.defaultBackground, m_settings.defaultTextColor);

    // <style> elements are author stylesheets, in document order.
    for (dom::Element *element : m_document->getElementsByTagName(QStringLiteral("style")))
        m_styles->addStylesheet(css::Stylesheet::parse(element->textContent()));

    // Stylesheets that arrived after the document are applied here, in the order
    // they were received. They are kept in a list rather than read back from the
    // cache so that a cached stylesheet is not re-parsed on every pass.
    for (const QString &source : m_lateStylesheets)
        m_styles->addStylesheet(css::Stylesheet::parse(source));

    m_styles->computeStyles(m_document.get());

    // Intrinsic sizes are applied before the tree is built, so an image that
    // already arrived gets its real dimensions on the first pass.
    renderer::BoxTreeBuilder::setImageSizes(&m_imageSizes);

    // A fresh box tree for every pass. Layout rewrites a tree's inline children
    // into line boxes, so re-laying out an existing tree would nest a new
    // generation of line boxes inside the old ones and grow without bound.
    renderer::BoxTreeBuilder::setStyleEngine(m_styles.get());
    m_boxTree = renderer::BoxTreeBuilder::build(m_document.get());

    if (m_boxTree) {
        renderer::LayoutEngine layout;
        layout.setViewport(m_settings.viewportWidth, m_settings.viewportHeight);
        m_layout = layout.layout(m_boxTree.get());
    } else {
        m_layout = renderer::LayoutResult();
    }
}

void Page::recordHistory()
{
    if (m_settings.recordHistory && m_history && !m_errorPage)
        m_history->visit(m_documentUrl, title());
}

void Page::finishWithError(const QString &kind, const QString &details)
{
    m_errorPage = true;
    m_error = details;

    const QString html = builtin::errorPage(m_url, kind, details);

    m_parseResult = html::Parser::parse(html, m_url);
    m_document = std::move(m_parseResult.document);
    m_parsedDocument = m_document.get();
    m_documentUrl = m_url;

    buildLayout();

    m_state = State::Idle;
    emit ready();
    emit failed(details);
    emit finished();
}

} // namespace oqb::browser
