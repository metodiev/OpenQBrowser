#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QTextStream>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineView>

#include "browser/PageSettings.h"
#include "network/Url.h"
#include "browser/BuiltinPages.h"
#include "ui/BuiltinScheme.h"
#include "ui/MainWindow.h"

using namespace oqb;

// The OpenQBrowser entry point.
//
// Pages are rendered by Qt WebEngine (Chromium), so the window shows sites the
// way Chrome does and plays HTML5 media. The browser can also be driven without
// a window: --dump-dom writes the document and --screenshot renders the page to
// a PNG, both through the same engine the window uses.

namespace {

/// Everything the command line can ask for.
struct CommandLine
{
    bool dumpDom = false;
    bool screenshot = false;
    bool window = false;

    QString dumpDomFile;
    QString screenshotFile;

    QString url;
    int width = 1024;
    int height = 768;
};

/// Turns what the user typed into a URL: an address becomes a navigation, a
/// phrase becomes a search.
network::Url resolveInput(const QString &input, const browser::PageSettings &settings)
{
    if (input.isEmpty())
        return network::Url::parse(QStringLiteral("about:home"));

    const network::Url url = network::Url::fromUserInput(input);
    if (url.isValid())
        return url;

    if (input.startsWith(QLatin1String("about:"), Qt::CaseInsensitive))
        return network::Url::parse(input);

    return network::Url::forSearchQuery(input, settings.searchTemplate);
}

/// Writes `text` to `path`, or to standard output when `path` is empty.
bool writeOutput(const QString &path, const QString &text)
{
    if (path.isEmpty()) {
        QTextStream out(stdout);
        out << text;
        return true;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QTextStream(stderr) << "openqbrowser: cannot write " << path << ": "
                            << file.errorString() << "\n";
        return false;
    }
    file.write(text.toUtf8());
    return true;
}

/// The set of options the program understands, in one place so that the parser
/// registration and the lookup cannot drift apart.
struct Options
{
    QCommandLineOption dumpDom;
    QCommandLineOption screenshot;
    QCommandLineOption width;
    QCommandLineOption height;
    QCommandLineOption window;
    QCommandLineOption noImages;

    Options()
        : dumpDom(QStringLiteral("dump-dom"),
                  QStringLiteral("Write the document to stdout, or to FILE with "
                                 "--dump-dom=FILE."))
        , screenshot(QStringLiteral("screenshot"),
                     QStringLiteral("Render the page to a PNG file named by "
                                    "--screenshot=FILE."))
        , width(QStringLiteral("width"), QStringLiteral("Viewport width in pixels."),
                QStringLiteral("px"))
        , height(QStringLiteral("height"), QStringLiteral("Viewport height in pixels."),
                 QStringLiteral("px"))
        , window(QStringLiteral("window"), QStringLiteral("Open the browser window."))
        , noImages(QStringLiteral("no-images"), QStringLiteral("Do not load images."))
    {
    }

    void registerWith(QCommandLineParser *parser) const
    {
        parser->addOption(dumpDom);
        parser->addOption(screenshot);
        parser->addOption(width);
        parser->addOption(height);
        parser->addOption(window);
        parser->addOption(noImages);
    }
};

/// The destination named by "--option=FILE", or an empty string for stdout.
QString destinationFor(const QStringList &destinations, const QString &option)
{
    const QString prefix = option + u'=';
    for (const QString &argument : destinations) {
        if (argument.startsWith(prefix))
            return argument.mid(prefix.size());
    }
    return {};
}

/// Fills `options` from the parsed command line.
void readCommandLine(const QCommandLineParser &parser, const Options &available,
                     const QStringList &destinations, CommandLine *options)
{
    const auto requested = [&parser, &destinations](const QCommandLineOption &option) {
        if (parser.isSet(option))
            return true;
        // A destination is written "--option=FILE", so a destination without the
        // bare flag still means the report was asked for.
        const QString prefix = u"--" + option.names().first() + u'=';
        for (const QString &argument : destinations) {
            if (argument.startsWith(prefix))
                return true;
        }
        return false;
    };

    options->dumpDom = requested(available.dumpDom);
    options->screenshot = requested(available.screenshot);
    options->window = parser.isSet(available.window);

    options->dumpDomFile = destinationFor(destinations, QStringLiteral("--dump-dom"));
    options->screenshotFile = destinationFor(destinations, QStringLiteral("--screenshot"));

    options->url = parser.positionalArguments().value(0);

    if (parser.isSet(available.width))
        options->width = qMax(1, parser.value(available.width).toInt());
    if (parser.isSet(available.height))
        options->height = qMax(1, parser.value(available.height).toInt());
}

} // namespace

int main(int argc, char **argv)
{
    // WebEngine's compositor shares OpenGL contexts; this has to be set before
    // the application object exists.
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    // Qt requires a URL scheme to be registered before the application object
    // exists. The built-in pages are served from it.
    oqb::ui::registerBuiltinScheme();

    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral(OPENQBROWSER_APP_NAME));
    QCoreApplication::setApplicationVersion(QStringLiteral(OPENQBROWSER_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("An open-source web browser."));
    parser.addHelpOption();
    parser.addVersionOption();
    const Options available;
    available.registerWith(&parser);
    parser.addPositionalArgument(QStringLiteral("url"), QStringLiteral("The page to load."));

    // Destinations are written "--option=FILE" and are stripped out here, so Qt
    // only ever sees the bare flags.
    static const QStringList kOutputOptions = {
        QStringLiteral("--dump-dom"), QStringLiteral("--screenshot"),
    };

    QStringList cleaned;
    QStringList destinations;
    for (const QString &argument : app.arguments()) {
        bool isDestination = false;
        for (const QString &option : kOutputOptions) {
            if (argument.startsWith(option + u'=')) {
                destinations.append(argument);
                isDestination = true;
                break;
            }
        }
        if (!isDestination)
            cleaned.append(argument);
    }

    parser.process(cleaned);

    CommandLine options;
    readCommandLine(parser, available, destinations, &options);

    if (options.url.isEmpty())
        options.url = QStringLiteral("about:home");

    browser::PageSettings settings;
    settings.viewportWidth = options.width;
    settings.viewportHeight = options.height;
    settings.loadImages = !parser.isSet(available.noImages);

    // A Chrome-shaped user agent keeps sites serving their desktop layout; the
    // default Qt user agent is routinely misread as a bot or a mobile client.
    QWebEngineProfile *profile = QWebEngineProfile::defaultProfile();
    profile->setHttpUserAgent(
        QStringLiteral("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                       "AppleWebKit/537.36 (KHTML, like Gecko) "
                       "Chrome/126.0.0.0 Safari/537.36 OpenQBrowser/%1")
            .arg(QStringLiteral(OPENQBROWSER_VERSION)));
    profile->settings()->setAttribute(QWebEngineSettings::AutoLoadImages, settings.loadImages);

    // The built-in pages are served from the browser's own scheme, so the
    // headless modes can report on them too.
    ui::installBuiltinSchemeHandler(profile, [](const QString &pageName) {
        return browser::builtin::documentFor(
            network::Url::parse(QStringLiteral("about:") + pageName),
            QStringLiteral(OPENQBROWSER_VERSION), 1280);
    });

    const bool wantsReport = options.dumpDom || options.screenshot;
    if (!wantsReport)
        options.window = true;

    const network::Url url = resolveInput(options.url, settings);

    // ----------------------------------------------------------------- window
    if (options.window) {
        auto *window = new ui::MainWindow(settings);
        window->show();
        window->openUrl(url);

        QObject::connect(qApp, &QApplication::lastWindowClosed, qApp, &QCoreApplication::quit);
        return app.exec();
    }

    // ----------------------------------------------------------------- reports
    int exitCode = 0;
    QElapsedTimer deadline;
    deadline.start();
    static constexpr int kMaxReportMs = 30000;

    // A report never answers until the load has finished; a safety timer keeps a
    // misbehaving page from hanging the process forever.
    auto *guard = new QTimer(&app);
    guard->setSingleShot(true);
    guard->setInterval(kMaxReportMs);
    QObject::connect(guard, &QTimer::timeout, &app, [&] {
        QTextStream(stderr) << "openqbrowser: the report timed out\n";
        QCoreApplication::exit(2);
    });
    guard->start();

    const QUrl target = url.isAbout() ? QUrl(QStringLiteral("about:blank")) : QUrl(url.toString());

    if (options.dumpDom) {
        // toHtml() needs no view, so the document can be dumped headlessly.
        auto *page = new QWebEnginePage(profile, &app);
        QObject::connect(page, &QWebEnginePage::loadFinished, &app,
                         [&, page](bool ok) {
                             guard->stop();
                             if (!ok)
                                 exitCode = 1;
                             page->toHtml([&, page](const QString &html) {
                                 if (!writeOutput(options.dumpDomFile, html))
                                     exitCode = 1;
                                 delete page;
                                 QCoreApplication::exit(exitCode);
                             });
                         });
        if (url.isAbout())
            page->setHtml(QStringLiteral("<html><body></body></html>"), target);
        else
            page->load(target);
        return app.exec();
    }

    if (options.screenshot) {
        // Screenshots need a rendered view; the window is shown briefly and
        // grabbed once the page has painted.
        auto *view = new QWebEngineView;
        view->resize(options.width, options.height);
        QObject::connect(view->page(), &QWebEnginePage::loadFinished, view,
                         [&, view](bool ok) {
                             if (!ok)
                                 exitCode = 1;
                             // Give the compositor a frame to paint after load.
                             QTimer::singleShot(500, view, [&, view] {
                                 guard->stop();
                                 const QImage image = view->grab().toImage();
                                 if (image.isNull() || !image.save(options.screenshotFile)) {
                                     QTextStream(stderr)
                                         << "openqbrowser: cannot write "
                                         << options.screenshotFile << "\n";
                                     exitCode = 1;
                                 }
                                 delete view;
                                 QCoreApplication::exit(exitCode);
                             });
                         });
        view->show();
        if (url.isAbout())
            view->setHtml(QStringLiteral("<html><body></body></html>"), target);
        else
            view->load(target);
        return app.exec();
    }

    return exitCode;
}
