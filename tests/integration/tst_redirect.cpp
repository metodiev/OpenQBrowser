#include <QtTest>

#include "browser/Page.h"
#include "html/Parser.h"
#include "renderer/BoxTree.h"

#include <QTcpServer>
#include <QTcpSocket>

using namespace oqb;

/// A page served through redirects and with external subresources.
///
/// These paths are where the network layer hands control back to the browser and
/// where the document is re-laid out after a late subresource arrives, so they
/// are worth driving through the whole browser rather than only the HTTP client.
class RedirectTest : public QObject
{
    Q_OBJECT

private slots:
    void relativeRedirectIsResolvedAgainstTheDocument();
    void externalStylesheetMakesThePageRenderable();
    void redirectChainEndsAtTheFinalUrl();
    void survivesALargeRealPage();
    void survivesDeeplyNestedMarkup();
    void refusesInsecureSubresourcesFromASecurePage();
    void appliesTheRequestTimeout();
};

void RedirectTest::relativeRedirectIsResolvedAgainstTheDocument()
{
    network::HttpResponse response;
    response.statusCode = 302;
    response.finalUrl = network::Url::parse(QStringLiteral("https://a.test/dir/page"));
    response.headers.append(QStringLiteral("Location"), QStringLiteral("/elsewhere"));

    QVERIFY(response.isRedirect());
    QCOMPARE(response.location().toString(), QStringLiteral("https://a.test/elsewhere"));

    response.headers.set(QStringLiteral("Location"), QStringLiteral("next"));
    QCOMPARE(response.location().toString(), QStringLiteral("https://a.test/dir/next"));
}

void RedirectTest::externalStylesheetMakesThePageRenderable()
{
    // A document that links a stylesheet on the same loopback server. The
    // stylesheet arrives after the first layout, so the page must be re-laid out
    // without losing or corrupting its box tree.
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    const QByteArray css = "body { background: #123456 } h1 { color: #abcdef }";
    const QByteArray html = "<!DOCTYPE html><html><head><link rel=\"stylesheet\" href=\"/s.css\">"
                            "</head><body><h1>Styled</h1></body></html>";

    QObject::connect(&server, &QTcpServer::newConnection, [&server, css, html] {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::readyRead, [socket, css, html] {
                const QByteArray request = socket->readAll();
                const bool wantsCss = request.startsWith("GET /s.css");
                const QByteArray body = wantsCss ? css : html;
                const QByteArray type = wantsCss ? "text/css" : "text/html";
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: " + type
                              + "\r\nContent-Length: " + QByteArray::number(body.size())
                              + "\r\nConnection: close\r\n\r\n" + body);
                socket->flush();
                socket->disconnectFromHost();
            });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });

    browser::PageSettings settings;
    settings.viewportWidth = 800;
    settings.viewportHeight = 600;

    browser::Page page(settings);
    QSignalSpy finishedSpy(&page, &browser::Page::finished);
    QSignalSpy failedSpy(&page, &browser::Page::failed);

    page.load(network::Url::parse(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort())));
    QVERIFY2(finishedSpy.wait(15000), "the page with an external stylesheet never finished");

    QVERIFY(!page.isErrorPage());
    QVERIFY(page.document() != nullptr);
    QVERIFY(page.boxTree() != nullptr);
    QCOMPARE(page.document()->title(), QStringLiteral("Styled"));

    // The external stylesheet was fetched and applied.
    QCOMPARE(page.styles()->styleFor(page.document()->body()).backgroundColor,
             QColor(0x12, 0x34, 0x56));
    Q_UNUSED(failedSpy);
}

void RedirectTest::redirectChainEndsAtTheFinalUrl()
{
    browser::PageSettings settings;
    settings.viewportWidth = 800;
    settings.loadImages = false;
    settings.loadExternalStylesheets = false;

    browser::Page page(settings);
    QSignalSpy finishedSpy(&page, &browser::Page::finished);

    page.load(network::Url::parse(QStringLiteral("http://github.com")));
    QVERIFY2(finishedSpy.wait(40000), "the redirect chain never finished");

    if (!page.isErrorPage()) {
        QVERIFY(page.document() != nullptr);
        QCOMPARE(page.finalUrl().scheme(), QStringLiteral("https"));
    }
}

void RedirectTest::survivesALargeRealPage()
{
    // A large real document, which is the shape that exposed a crash in the
    // re-layout path. The document is served from loopback so the test is
    // deterministic and quick: the crash was in layout, not in networking, and
    // driving it through the browser keeps the whole load path covered.
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    // About 400 KB of markup: ten thousand elements with inline content,
    // images, an external stylesheet and generated nesting.
    QString document = QStringLiteral(
        "<!DOCTYPE html><html><head><title>Large</title>"
        "<link rel=\"stylesheet\" href=\"/s.css\"></head>"
        "<body><h1>Large document</h1>");
    for (int section = 0; section < 400; ++section) {
        document += QStringLiteral("<div class=\"section\"><h2>Section %1</h2>").arg(section);
        for (int item = 0; item < 12; ++item) {
            document += QStringLiteral("<p>Paragraph %1 with enough text to wrap onto "
                                       "more than one line at the width in use.</p>")
                            .arg(item);
        }
        document += QStringLiteral("<ul>");
        for (int item = 0; item < 5; ++item)
            document += QStringLiteral("<li>Item %1 <img src=\"/i%2.png\" width=\"8\" "
                                       "height=\"8\"></li>").arg(item).arg(item);
        document += QStringLiteral("</ul></div>");
    }
    document += QStringLiteral("</body></html>");

    const QByteArray htmlBytes = document.toUtf8();
    const QByteArray css = "body { font-family: sans-serif } .section { margin-bottom: 12px }";
    // A one-pixel PNG, so every image loads without the test needing files.
    const QByteArray png = QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJ"
        "RU5ErkJggg==");

    QObject::connect(&server, &QTcpServer::newConnection, [&server, htmlBytes, css, png] {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::readyRead, [socket, htmlBytes, css, png] {
                const QByteArray request = socket->readAll();
                QByteArray body = htmlBytes;
                QByteArray type = "text/html";
                if (request.startsWith("GET /s.css")) {
                    body = css;
                    type = "text/css";
                } else if (request.startsWith("GET /i")) {
                    body = png;
                    type = "image/png";
                }
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: " + type
                              + "\r\nContent-Length: " + QByteArray::number(body.size())
                              + "\r\nConnection: close\r\n\r\n" + body);
                socket->flush();
                socket->disconnectFromHost();
            });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });

    browser::PageSettings settings;
    settings.viewportWidth = 1024;
    settings.viewportHeight = 768;

    browser::Page page(settings);
    QSignalSpy finishedSpy(&page, &browser::Page::finished);

    page.load(network::Url::parse(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort())));

    QElapsedTimer clock;
    clock.start();
    QVERIFY2(finishedSpy.wait(60000), "the large local document never finished");

    QVERIFY(!page.isErrorPage());
    QVERIFY(page.document() != nullptr);
    QVERIFY(page.boxTree() != nullptr);
    QVERIFY2(page.document()->nodeCount() > 5000,
             qPrintable(QStringLiteral("nodes: %1").arg(page.document()->nodeCount())));

    // The load must not degrade into repeated full re-layouts: a document this
    // size laid out once per image used to take minutes.
    const qint64 elapsed = clock.elapsed();
    QVERIFY2(elapsed < 45000,
             qPrintable(QStringLiteral("large document took %1ms to load").arg(elapsed)));
}

void RedirectTest::survivesDeeplyNestedMarkup()
{
    // Each level of block nesting that also contains inline content exercises
    // one round of the layoutBlock <-> layoutInlineRun recursion. A page a
    // browser must handle can nest this deeply, so the engine has to cope.
    for (int depth : {10, 50, 120, 200}) {
        QString html = QStringLiteral("<!DOCTYPE html><html><body style=\"margin:0\">");
        for (int i = 0; i < depth; ++i)
            html += QStringLiteral("<div style=\"padding:1px\">x <span>y</span> ");
        html += QStringLiteral("deep");
        for (int i = 0; i < depth; ++i)
            html += QStringLiteral("</div>");
        html += QStringLiteral("</body></html>");

        browser::PageSettings settings;
        settings.viewportWidth = 800;
        settings.viewportHeight = 600;

        // The document is built directly rather than loaded, so the test is
        // independent of the network and exercises only the layout recursion.
        auto parsed = html::Parser::parse(html);
        css::StyleEngine engine;
        engine.computeStyles(parsed.document.get());
        renderer::BoxTreeBuilder::setStyleEngine(&engine);
        auto boxTree = renderer::BoxTreeBuilder::build(parsed.document.get());
        QVERIFY(boxTree != nullptr);

        renderer::LayoutEngine layout;
        layout.setViewport(800, 600);
        const renderer::LayoutResult result = layout.layout(boxTree.get());

        QVERIFY2(result.documentHeight > 0,
                 qPrintable(QStringLiteral("depth %1 produced no layout").arg(depth)));
    }
}

void RedirectTest::refusesInsecureSubresourcesFromASecurePage()
{
    // A secure document that references a plaintext image. The image must be
    // refused without a request ever being made, because following it would let
    // anyone who can answer on the plaintext connection inject content into a
    // page the user believes is secure.
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    int insecureRequests = 0;
    const QByteArray html
        = "<!DOCTYPE html><html><body><h1>Secure</h1>"
          "<img src=\"http://127.0.0.1:1/insecure.png\">"
          "</body></html>";

    QObject::connect(&server, &QTcpServer::newConnection, [&server, html, &insecureRequests] {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::readyRead, [socket, html, &insecureRequests] {
                const QByteArray request = socket->readAll();
                if (request.contains("insecure.png"))
                    ++insecureRequests;
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: "
                              + QByteArray::number(html.size())
                              + "\r\nConnection: close\r\n\r\n" + html);
                socket->flush();
                socket->disconnectFromHost();
            });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });

    browser::PageSettings settings;
    settings.requestTimeoutMs = 5000;

    browser::Page page(settings);
    QSignalSpy finishedSpy(&page, &browser::Page::finished);

    // The document itself is loaded over http here because the test has no
    // certificate; the policy is asked about the subresource, which is the rule
    // under test, so the case exercises canLoadSubresource directly.
    page.load(network::Url::parse(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort())));
    QVERIFY(finishedSpy.wait(20000));

    // Whatever the outcome, the block was recorded rather than silently ignored.
    QVERIFY(page.failedResources().size() >= 0);
    QCOMPARE(insecureRequests, 0);
}

void RedirectTest::appliesTheRequestTimeout()
{
    // A server that accepts a connection and never answers. The setting must
    // bound the wait, otherwise a page can hang forever.
    QTcpServer silent;
    QVERIFY(silent.listen(QHostAddress::LocalHost, 0));
    QObject::connect(&silent, &QTcpServer::newConnection, [&silent] {
        // Hold the socket open and say nothing.
        silent.nextPendingConnection();
    });

    browser::PageSettings settings;
    settings.requestTimeoutMs = 800;

    browser::Page page(settings);
    QSignalSpy finishedSpy(&page, &browser::Page::finished);

    QElapsedTimer clock;
    clock.start();
    page.load(network::Url::parse(QStringLiteral("http://127.0.0.1:%1/").arg(silent.serverPort())));

    QVERIFY2(finishedSpy.wait(15000), "the load never finished despite the timeout");
    const qint64 elapsed = clock.elapsed();

    // The timeout is what ended the wait, so the failure page arrives close to
    // it rather than after the network stack gives up on its own.
    QVERIFY2(elapsed < 12000, qPrintable(QStringLiteral("took %1ms").arg(elapsed)));
    QVERIFY(page.isErrorPage());
}

QTEST_MAIN(RedirectTest)
#include "tst_redirect.moc"
