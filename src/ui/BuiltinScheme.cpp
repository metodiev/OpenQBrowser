#include "ui/BuiltinScheme.h"

#include <QBuffer>
#include <QWebEngineProfile>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlSchemeHandler>

namespace oqb::ui {

namespace {

/// Serves the built-in pages.
///
/// The document is produced on demand rather than kept in memory, because the
/// pages read live state: the history page lists what was visited, and the new
/// tab page counts the open tabs.
class BuiltinSchemeHandler : public QWebEngineUrlSchemeHandler
{
    Q_OBJECT

public:
    using Renderer = std::function<QString(const QString &pageName)>;

    explicit BuiltinSchemeHandler(Renderer render, QObject *parent = nullptr)
        : QWebEngineUrlSchemeHandler(parent)
        , m_render(std::move(render))
    {
    }

    void requestStarted(QWebEngineUrlRequestJob *job) override
    {
        // "oqb:home" parses with an empty host and "home" as the path.
        QString name = job->requestUrl().path();
        if (name.startsWith(u'/'))
            name.remove(0, 1);

        const QString html = m_render ? m_render(name) : QString();
        if (html.isEmpty()) {
            job->fail(QWebEngineUrlRequestJob::UrlNotFound);
            return;
        }

        // The buffer is owned by the job, which is what keeps it alive until the
        // reply has been read; a local QByteArray would be freed underneath it.
        auto *buffer = new QBuffer(job);
        buffer->setData(html.toUtf8());
        buffer->open(QIODevice::ReadOnly);
        job->reply(QByteArrayLiteral("text/html"), buffer);
    }

private:
    Renderer m_render;
};

} // namespace

void registerBuiltinScheme()
{
    // Idempotent: Qt refuses a scheme that is already registered, and the
    // registration also happens automatically below, so an explicit call is a
    // way to make the dependency visible rather than the only way it happens.
    static bool registered = false;
    if (registered)
        return;
    registered = true;

    // Braces, not parentheses: a parenthesised declaration here would be read as
    // a function prototype rather than a constructor call.
    QWebEngineUrlScheme scheme{QByteArray(kBuiltinScheme)};

    // Path syntax is what makes "<scheme>:home" parse with "home" as the path
    // rather than as an opaque string.
    scheme.setSyntax(QWebEngineUrlScheme::Syntax::Path);

    // The pages are part of the browser, so they are treated as a secure
    // context: that is what lets them be a real origin rather than something
    // Chromium refuses to give storage to.
    scheme.setFlags(QWebEngineUrlScheme::SecureScheme | QWebEngineUrlScheme::LocalAccessAllowed
                    | QWebEngineUrlScheme::ViewSourceAllowed);

    QWebEngineUrlScheme::registerScheme(scheme);
}

namespace {

/// Registers the scheme before main() runs.
///
/// Qt requires the registration to happen before the QApplication object is
/// created, which is earlier than any of our code gets to run - so it is done
/// from a static initialiser. Leaving it to main() would silently skip it in
/// every program that has a main() of its own, which is exactly what the test
/// executables have.
const bool schemeRegistered = [] {
    registerBuiltinScheme();
    return true;
}();

} // namespace

void installBuiltinSchemeHandler(QWebEngineProfile *profile,
                                 std::function<QString(const QString &pageName)> render)
{
    if (!profile)
        return;
    profile->installUrlSchemeHandler(QByteArray(kBuiltinScheme),
                                     new BuiltinSchemeHandler(std::move(render), profile));
}

} // namespace oqb::ui

#include "BuiltinScheme.moc"
