#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QApplication>
#include <QFile>
#include <QImage>
#include <QTextStream>

#include "browser/Page.h"
#include "browser/Tab.h"
#include "devtools/Inspector.h"
#include "storage/Cookies.h"
#include "ui/MainWindow.h"

// The OpenQBrowser entry point.
//
// The browser can be driven without a window, which is what makes it testable
// and scriptable: --dump-dom, --dump-layout and --screenshot run the full
// pipeline and write their result to a file or to standard output. The windowed
// mode arrives with the Qt Widgets shell in src/ui/.

namespace {

/// Everything the command line can ask for.
struct CommandLine
{
    bool dumpDom = false;
    bool dumpLayout = false;
    bool dumpStyles = false;
    bool dumpBoxes = false;
    bool dumpAll = false;
    bool screenshot = false;
    bool window = false;

    QString dumpDomFile;
    QString dumpLayoutFile;
    QString dumpStylesFile;
    QString dumpBoxesFile;
    QString screenshotFile;

    QString url;
    int width = 1024;
    int height = 768;
};

/// Turns what the user typed into a URL: an address becomes a navigation, a
/// phrase becomes a search.
oqb::network::Url resolveInput(const QString &input, const oqb::browser::PageSettings &settings)
{
    if (input.isEmpty())
        return oqb::network::Url::parse(QStringLiteral("about:home"));

    const oqb::network::Url url = oqb::network::Url::fromUserInput(input);
    if (url.isValid())
        return url;

    if (input.startsWith(QLatin1String("about:"), Qt::CaseInsensitive))
        return oqb::network::Url::parse(input);

    return oqb::network::Url::forSearchQuery(input, settings.searchTemplate);
}

/// The computed style of every element in the document, in document order.
QString styleReport(const oqb::browser::Page &page)
{
    if (!page.document() || !page.styles())
        return QStringLiteral("(no styles)\n");

    const oqb::dom::Element *root = page.document()->documentElement();
    if (!root)
        return QStringLiteral("(empty document)\n");

    QString out;
    std::function<void(const oqb::dom::Element *)> walk = [&](const oqb::dom::Element *element) {
        if (page.styles()->hasStyleFor(element)) {
            out += QStringLiteral("--- %1 ---\n").arg(element->describe());
            out += oqb::devtools::Inspector::computedStyles(page.styles(), element);
        }
        for (const oqb::dom::Element *child : element->childElements())
            walk(child);
    };
    walk(root);
    return out;
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

/// Runs every report the command line asked for. Returns false on a write error.
bool produceReports(const CommandLine &options, const oqb::browser::Page &page)
{
    bool ok = true;

    if (options.dumpAll) {
        writeOutput(QString(), oqb::devtools::Inspector::fullReport(
                                   page.document(), page.styles(), page.boxTree(), page.layout(),
                                   page.scripts()));
    }
    if (options.dumpDom)
        ok = writeOutput(options.dumpDomFile, oqb::devtools::Inspector::domTree(page.document()))
            && ok;
    if (options.dumpLayout) {
        ok = writeOutput(options.dumpLayoutFile,
                         oqb::devtools::Inspector::boxTree(page.boxTree()))
            && ok;
    }
    if (options.dumpBoxes) {
        ok = writeOutput(options.dumpBoxesFile,
                         oqb::devtools::Inspector::geometry(page.boxTree()))
            && ok;
    }
    if (options.dumpStyles)
        ok = writeOutput(options.dumpStylesFile, styleReport(page)) && ok;

    if (options.screenshot) {
        const QImage image = page.renderToImage();
        if (image.isNull()) {
            QTextStream(stderr) << "openqbrowser: the page produced no image\n";
            ok = false;
        } else if (!image.save(options.screenshotFile)) {
            QTextStream(stderr) << "openqbrowser: cannot write " << options.screenshotFile
                                << "\n";
            ok = false;
        }
    }

    return ok;
}

/// The set of options the program understands, in one place so that the parser
/// registration and the lookup cannot drift apart.
struct Options
{
    QCommandLineOption dumpDom;
    QCommandLineOption dumpLayout;
    QCommandLineOption dumpStyles;
    QCommandLineOption dumpBoxes;
    QCommandLineOption dumpAll;
    QCommandLineOption screenshot;
    QCommandLineOption width;
    QCommandLineOption height;
    QCommandLineOption window;
    QCommandLineOption noImages;

    Options()
        // These options take no value from Qt's point of view. A destination
        // file is written as "--dump-dom=FILE", which Qt parses unambiguously:
        // accepting it as a separate argument would make "--dump-dom URL" read
        // the URL as a file name, because every URL contains a colon.
        : dumpDom(QStringLiteral("dump-dom"),
                  QStringLiteral("Write the document tree to stdout, or to FILE with "
                                 "--dump-dom=FILE."))
        , dumpLayout(QStringLiteral("dump-layout"),
                     QStringLiteral("Write the box tree to stdout, or to FILE with "
                                    "--dump-layout=FILE."))
        , dumpStyles(QStringLiteral("dump-styles"),
                     QStringLiteral("Write computed styles to stdout, or to FILE with "
                                    "--dump-styles=FILE."))
        , dumpBoxes(QStringLiteral("dump-boxes"),
                    QStringLiteral("Write box geometry to stdout, or to FILE with "
                                   "--dump-boxes=FILE."))
        , dumpAll(QStringLiteral("dump-all"), QStringLiteral("Write every report to stdout."))
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
        parser->addOption(dumpLayout);
        parser->addOption(dumpStyles);
        parser->addOption(dumpBoxes);
        parser->addOption(dumpAll);
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
        // A destination without the bare flag still means the report was asked for.
        const QString prefix = u"--" + option.names().value(0) + u'=';
        for (const QString &argument : destinations) {
            if (argument.startsWith(prefix))
                return true;
        }
        return false;
    };

    options->dumpDom = requested(available.dumpDom);
    options->dumpLayout = requested(available.dumpLayout);
    options->dumpStyles = requested(available.dumpStyles);
    options->dumpBoxes = requested(available.dumpBoxes);
    options->screenshot = requested(available.screenshot);
    options->dumpAll = parser.isSet(available.dumpAll);
    options->window = parser.isSet(available.window);

    options->dumpDomFile = destinationFor(destinations, QStringLiteral("--dump-dom"));
    options->dumpLayoutFile = destinationFor(destinations, QStringLiteral("--dump-layout"));
    options->dumpStylesFile = destinationFor(destinations, QStringLiteral("--dump-styles"));
    options->dumpBoxesFile = destinationFor(destinations, QStringLiteral("--dump-boxes"));
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
    // Font metrics come from the platform, so even a headless run needs an
    // application object, and the windowed mode needs widgets. QApplication
    // covers both; the offscreen platform plugin keeps it windowless when there
    // is no display, which is the caller's choice of QT_QPA_PLATFORM.
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral(OPENQBROWSER_APP_NAME));
    QCoreApplication::setApplicationVersion(QStringLiteral(OPENQBROWSER_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("An open-source web browser built from scratch."));
    parser.addHelpOption();
    parser.addVersionOption();
    const Options available;
    available.registerWith(&parser);
    parser.addPositionalArgument(QStringLiteral("url"), QStringLiteral("The page to load."));

    // Destinations are written "--option=FILE" and are stripped out here, so Qt
    // only ever sees the bare flags. That keeps "--dump-dom about:home" working:
    // a URL contains a colon, which Qt would otherwise treat as an option value.
    static const QStringList kOutputOptions = {
        QStringLiteral("--dump-dom"),    QStringLiteral("--dump-layout"),
        QStringLiteral("--dump-styles"), QStringLiteral("--dump-boxes"),
        QStringLiteral("--screenshot"),
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
        // Everything else, including "--width=800", is left for Qt to parse.
        if (!isDestination)
            cleaned.append(argument);
    }

    parser.process(cleaned);

    CommandLine options;
    readCommandLine(parser, available, destinations, &options);

    if (options.url.isEmpty())
        options.url = QStringLiteral("about:home");

    const bool wantsReport = options.dumpDom || options.dumpLayout || options.dumpStyles
        || options.dumpBoxes || options.dumpAll || options.screenshot;

    // With no reporting option the window opens, which is what running a browser
    // with no arguments should do.
    if (!wantsReport)
        options.window = true;

    oqb::browser::PageSettings settings;
    settings.viewportWidth = options.width;
    settings.viewportHeight = options.height;
    settings.loadImages = !parser.isSet(available.noImages);

    oqb::browser::Page page(settings);

    // The headless page gets a jar of its own. Without one a redirect that sets a
    // cookie loses it, and the follow-up request goes out without the session -
    // which is exactly the case a report is asked for when a site behaves
    // differently than expected.
    oqb::storage::CookieJar cookies;
    page.setCookieJar(&cookies);

    if (options.window) {
        // The window owns its own tabs and pages, so the standalone Page above
        // is not used in this mode.
        auto *window = new oqb::ui::MainWindow(settings);
        window->show();
        window->openUrl(resolveInput(options.url, settings));

        // A window closes when the last one is; the application then ends.
        QObject::connect(qApp, &QApplication::lastWindowClosed, qApp,
                         &QCoreApplication::quit);
        return app.exec();
    }

    int exitCode = 0;

    QObject::connect(&page, &oqb::browser::Page::finished, [&] {
        if (!produceReports(options, page))
            exitCode = 1;
        QCoreApplication::exit(exitCode);
    });

    QObject::connect(&page, &oqb::browser::Page::failed, [&](const QString &message) {
        // The error page is still rendered, so a dump succeeds; the failure is
        // reported on stderr where a script can see it.
        QTextStream(stderr) << "openqbrowser: " << message << "\n";
    });

    // The load is started once the event loop is running. An about: page is
    // built synchronously, so loading it here would emit finished() before
    // exec() had begun and the quit would be lost.
    const oqb::network::Url url = resolveInput(options.url, settings);
    QMetaObject::invokeMethod(&page, [&page, url] { page.load(url); }, Qt::QueuedConnection);

    return app.exec();
}
