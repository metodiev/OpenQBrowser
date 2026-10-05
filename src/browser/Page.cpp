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

} // namespace

Page::Page(const PageSettings &settings, QObject *parent)
    : QObject(parent)
    , m_settings(settings)
    , m_scripts(std::make_unique<javascript::ScriptEngine>())
{
    m_loader = std::make_unique<network::ResourceLoader>(this);
    m_loader->setMaxConcurrentRequests(m_settings.maxConcurrentRequests);
    m_loader->setUserAgent(m_settings.userAgent);
    m_loader->setRequestTimeout(m_settings.requestTimeoutMs);

    connect(m_loader.get(), &network::ResourceLoader::finished, this,
            [this](const network::Resource &resource) {
                if (m_state == State::LoadingDocument)
                    handleDocumentResource(resource);
                else if (m_state == State::LoadingSubresources)
                    handleSubresource(resource);
            });
}

Page::~Page() = default;

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

    m_url = url;
    m_finalUrl = url;
    m_errorPage = false;
    m_error.clear();
    m_requestedResources.clear();
    m_failedResources.clear();
    m_pendingSubresources.clear();
    m_inFlightSubresources = 0;
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
    if (m_documentUrl.isValid())
        load(m_documentUrl);
}

void Page::stop()
{
    if (m_loader) {
        m_loader->cancelAll();
        m_loader->clearCache();
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

    // Scripts are discovered and reported, but not run.
    for (dom::Element *element : m_document->getElementsByTagName(QStringLiteral("script"))) {
        const QString src = element->attribute(QStringLiteral("src"));
        if (!src.isEmpty()) {
            m_scripts->execute(QString(), m_document.get(), m_document->resolveUrl(src).toString());
        } else {
            m_scripts->execute(element->textContent(), m_document.get(), QStringLiteral("inline"));
        }
    }

    m_state = State::LoadingSubresources;
    collectSubresources();

    // The layout is built before subresources arrive so the page is paintable
    // immediately, which is what a browser does with an incomplete document.
    buildLayout();

    emit ready();

    if (m_pendingSubresources.isEmpty()) {
        recordHistory();
        m_state = State::Idle;
        emit finished();
        return;
    }

    requestNextSubresource();
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
    if (m_inFlightSubresources > 0)
        --m_inFlightSubresources;

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

    if (m_inFlightSubresources == 0 && m_pendingSubresources.isEmpty()) {
        m_state = State::Idle;

        // One last pass, so a stylesheet or image that arrived with the final
        // subresource is reflected in the layout the caller sees. The coalesced
        // pass may not have run yet, and after this point it never would.
        buildLayout();

        recordHistory();
        emit ready();
        emit finished();
    }
}

void Page::scheduleRelayout()
{
    if (m_relayoutScheduled)
        return;
    m_relayoutScheduled = true;

    // A queued call runs after the current burst of socket events has been
    // handled, which is exactly when every image that arrived together is known.
    QMetaObject::invokeMethod(
        this,
        [this] {
            m_relayoutScheduled = false;
            if (m_state == State::Idle)
                return; // The final layout already happened.
            buildLayout();
            emit ready();
        },
        Qt::QueuedConnection);
}

void Page::buildLayout()
{
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
