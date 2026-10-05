#include <QtTest>

#include "css/Style.h"
#include "html/Parser.h"
#include "network/Url.h"
#include "renderer/BoxTree.h"
#include "renderer/Layout.h"
#include "renderer/Painter.h"

using namespace oqb;

/// End-to-end tests of the browser pipeline: parse, style, build boxes, lay out
/// and paint. These are the tests that would catch a regression in how the
/// stages fit together, as opposed to how each one behaves alone.
class PipelineTest : public QObject
{
    Q_OBJECT

private:
    struct Rendered
    {
        html::ParseResult parsed;
        std::unique_ptr<css::StyleEngine> engine;
        std::unique_ptr<renderer::Box> boxTree;
        renderer::LayoutResult layout;
        QImage image;
        double width = 800;
        double height = 600;
    };

    /// Runs the whole pipeline over `html` and returns everything it produced.
    std::unique_ptr<Rendered> runPipeline(const QString &html, double width = 800,
                                          double height = 600)
    {
        auto rendered = std::make_unique<Rendered>();
        rendered->width = width;
        rendered->height = height;

        rendered->parsed = html::Parser::parse(
            html, network::Url::parse(QStringLiteral("https://example.test/")));

        css::StyleContext context;
        context.viewportWidth = width;
        context.viewportHeight = height;
        rendered->engine = std::make_unique<css::StyleEngine>(context);

        // External stylesheets arrive through the <style> elements here; the
        // network path is covered by the HTTP tests.
        for (auto *styleElement : rendered->parsed.document->getElementsByTagName(
                 QStringLiteral("style"))) {
            rendered->engine->addStylesheet(css::Stylesheet::parse(styleElement->textContent()));
        }
        rendered->engine->computeStyles(rendered->parsed.document.get());

        renderer::BoxTreeBuilder::setStyleEngine(rendered->engine.get());
        rendered->boxTree = renderer::BoxTreeBuilder::build(rendered->parsed.document.get());

        if (rendered->boxTree) {
            renderer::LayoutEngine layoutEngine;
            layoutEngine.setViewport(width, height);
            rendered->layout = layoutEngine.layout(rendered->boxTree.get());
            rendered->image
                = renderer::Painter::renderToImage(rendered->boxTree.get(), int(width), int(height));
        }

        return rendered;
    }

    /// Counts pixels in `image` matching a predicate.
    template <typename Predicate>
    static int countPixels(const QImage &image, Predicate predicate)
    {
        int count = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (predicate(image.pixelColor(x, y)))
                    ++count;
            }
        }
        return count;
    }

private slots:
    void rendersStyledDocument();
    void appliesExternalAndInlineStylesTogether();
    void rendersNestedLayoutCorrectly();
    void escapesAndRendersEntities();
    void handlesRealisticPage();
    void scalesWithViewport();
    void rendersManyElements();
    void survivesMalformedMarkup();
};

void PipelineTest::rendersStyledDocument()
{
    auto rendered = runPipeline(QStringLiteral(R"(
        <!DOCTYPE html>
        <html><head><style>
          body { margin: 0; background: #ffffff; font-family: sans-serif }
          h1 { color: rgb(200, 0, 0); font-size: 36px; margin: 0; padding: 10px }
          p { color: #333333; font-size: 16px }
        </style></head>
        <body><h1>Heading</h1><p>Body text here.</p></body></html>)"));

    QVERIFY(rendered->boxTree != nullptr);
    QVERIFY(!rendered->image.isNull());

    // The heading's red pixels must be present, which proves the cascade fed
    // the painter through layout.
    const int redPixels = countPixels(rendered->image, [](const QColor &c) {
        return c.red() > 150 && c.green() < 90 && c.blue() < 90;
    });
    QVERIFY2(redPixels > 50, qPrintable(QStringLiteral("red pixels: %1").arg(redPixels)));

    // And dark grey body text.
    const int greyPixels = countPixels(rendered->image, [](const QColor &c) {
        return c.lightness() > 30 && c.lightness() < 90;
    });
    QVERIFY2(greyPixels > 20, qPrintable(QStringLiteral("grey pixels: %1").arg(greyPixels)));
}

void PipelineTest::appliesExternalAndInlineStylesTogether()
{
    auto rendered = runPipeline(QStringLiteral(R"(
        <!DOCTYPE html><html><head><style>
          #box { background: #0000ff; width: 100px; height: 100px }
        </style></head>
        <body style="margin: 0">
          <div id="box" style="background: #00ff00"></div>
        </body></html>)"));

    // The inline style wins over the stylesheet.
    const int greenPixels = countPixels(rendered->image, [](const QColor &c) {
        return c.green() > 200 && c.red() < 80 && c.blue() < 80;
    });
    const int bluePixels = countPixels(rendered->image, [](const QColor &c) {
        return c.blue() > 200 && c.red() < 80 && c.green() < 80;
    });

    QVERIFY2(greenPixels > 5000, qPrintable(QStringLiteral("green: %1").arg(greenPixels)));
    QCOMPARE(bluePixels, 0);
}

void PipelineTest::rendersNestedLayoutCorrectly()
{
    auto rendered = runPipeline(QStringLiteral(R"(
        <!DOCTYPE html><html><head><style>
          body { margin: 0 }
          #outer { width: 400px; padding: 20px; background: #eeeeee }
          #inner { width: 100%; height: 50px; background: #ff00ff }
        </style></head>
        <body><div id="outer"><div id="inner"></div></div></body></html>)"));

    auto *inner = rendered->parsed.document->getElementById(QStringLiteral("inner"));
    QVERIFY(inner != nullptr);

    // Find the box for #inner and check it was sized to the parent's content
    // width, not the viewport.
    renderer::Box *innerBox = nullptr;
    std::function<void(renderer::Box *)> walk = [&](renderer::Box *box) {
        if (box->node() == inner)
            innerBox = box;
        for (const auto &child : box->children())
            walk(child.get());
    };
    walk(rendered->boxTree.get());

    QVERIFY(innerBox != nullptr);
    QCOMPARE(innerBox->width(), 400.0);
    QCOMPARE(innerBox->height(), 50.0);

    // The magenta box is drawn at that size.
    const int magenta = countPixels(rendered->image, [](const QColor &c) {
        return c.red() > 200 && c.blue() > 200 && c.green() < 80;
    });
    QVERIFY2(magenta > 18000, qPrintable(QStringLiteral("magenta: %1").arg(magenta)));
}

void PipelineTest::escapesAndRendersEntities()
{
    auto rendered = runPipeline(QStringLiteral(R"(
        <!DOCTYPE html><html><head><style>body { margin: 0 }</style></head>
        <body><p>&lt;not a tag&gt; &amp; &copy; &#65;</p></body></html>)"));

    // The escaped markup must appear as text, not as elements.
    QCOMPARE(rendered->parsed.document->getElementsByTagName(QStringLiteral("not")).size(), 0);

    const QString text = rendered->parsed.document->body()->textContent();
    QVERIFY(text.contains(QStringLiteral("<not a tag>")));
    QVERIFY(text.contains(QStringLiteral("\u00A9")));
    QVERIFY(text.contains(QStringLiteral("A")));
}

void PipelineTest::handlesRealisticPage()
{
    auto rendered = runPipeline(QStringLiteral(R"(
        <!DOCTYPE html>
        <html lang="en">
        <head>
          <meta charset="utf-8">
          <title>A Realistic Page</title>
          <style>
            * { box-sizing: content-box }
            body { margin: 0; font-family: sans-serif; font-size: 16px; color: #222;
                   background: #fafafa }
            header { background: #2c3e50; color: white; padding: 20px }
            header h1 { margin: 0; font-size: 28px }
            nav { background: #34495e; padding: 10px 20px }
            nav a { color: #ecf0f1; margin-right: 15px; text-decoration: none }
            main { padding: 20px; max-width: 700px }
            article { margin-bottom: 30px }
            article h2 { color: #2980b9; font-size: 22px; margin: 0 0 8px 0 }
            article p { line-height: 24px; margin: 0 0 12px 0 }
            .tag { display: inline-block; background: #e74c3c; color: white;
                   padding: 2px 8px; font-size: 12px }
            ul { padding-left: 24px }
            li { margin-bottom: 4px }
            footer { background: #2c3e50; color: #bdc3c7; padding: 15px; font-size: 13px }
          </style>
        </head>
        <body>
          <header><h1>OpenQBrowser</h1></header>
          <nav><a href="/">Home</a><a href="/docs">Docs</a><a href="/about">About</a></nav>
          <main>
            <article>
              <h2>First article</h2>
              <span class="tag">release</span>
              <p>This is the first article. It has enough text in it to wrap onto
                 more than one line at the width the layout engine is given.</p>
            </article>
            <article>
              <h2>Second article</h2>
              <p>A second paragraph, shorter than the first.</p>
              <ul><li>First point</li><li>Second point</li><li>Third point</li></ul>
            </article>
          </main>
          <footer>&copy; 2026 OpenQBrowser</footer>
        </body></html>)"));

    QVERIFY(rendered->boxTree != nullptr);
    QVERIFY(rendered->parsed.warnings.isEmpty());
    QVERIFY(rendered->layout.documentHeight > 400);

    // The dark header band must be present at the top.
    QVERIFY(rendered->image.pixelColor(400, 10).lightness() < 100);

    // The red tags must be rendered as inline blocks.
    const int redTagPixels = countPixels(rendered->image, [](const QColor &c) {
        return c.red() > 200 && c.green() < 100 && c.blue() < 100;
    });
    QVERIFY2(redTagPixels > 100, qPrintable(QStringLiteral("tag pixels: %1").arg(redTagPixels)));

    // The footer band is the last thing on the page.
    QVERIFY(rendered->layout.documentHeight > 400);

    // Every heading, paragraph and list item produced a box.
    QCOMPARE(rendered->parsed.document->getElementsByTagName(QStringLiteral("h2")).size(), 2);
    QCOMPARE(rendered->parsed.document->getElementsByTagName(QStringLiteral("li")).size(), 3);
}

void PipelineTest::scalesWithViewport()
{
    const QString html = QStringLiteral(R"(
        <!DOCTYPE html><html><head><style>
          body { margin: 0 }
          #col { width: 50%; height: 100px; background: #00ffff }
        </style></head><body><div id="col"></div></body></html>)");

    auto wide = runPipeline(html, 1000, 600);
    auto narrow = runPipeline(html, 500, 600);

    auto widthOf = [&](const std::unique_ptr<Rendered> &rendered) {
        auto *element = rendered->parsed.document->getElementById(QStringLiteral("col"));
        renderer::Box *found = nullptr;
        std::function<void(renderer::Box *)> walk = [&](renderer::Box *box) {
            if (box->node() == element)
                found = box;
            for (const auto &child : box->children())
                walk(child.get());
        };
        walk(rendered->boxTree.get());
        return found ? found->width() : -1.0;
    };

    // A percentage width follows the viewport.
    QCOMPARE(widthOf(wide), 500.0);
    QCOMPARE(widthOf(narrow), 250.0);

    // And the painted area follows the layout.
    const int widePixels = countPixels(wide->image, [](const QColor &c) {
        return c.blue() > 200 && c.green() > 200 && c.red() < 80;
    });
    const int narrowPixels = countPixels(narrow->image, [](const QColor &c) {
        return c.blue() > 200 && c.green() > 200 && c.red() < 80;
    });
    QVERIFY(narrowPixels < widePixels);
}

void PipelineTest::rendersManyElements()
{
    // A page with enough elements that the tree walk and the painter's culling
    // both matter.
    QString rows;
    for (int i = 0; i < 200; ++i) {
        rows += QStringLiteral("<div class=\"row\" id=\"row%1\">Row number %1</div>").arg(i);
    }

    auto rendered = runPipeline(QStringLiteral(R"(
        <!DOCTYPE html><html><head><style>
          body { margin: 0 }
          .row { padding: 4px; font-size: 12px }
          .row:nth-child(odd) { background: #eeeeee }
        </style></head><body>)") + rows + QStringLiteral("</body></html>"));

    QVERIFY(rendered->boxTree != nullptr);
    QCOMPARE(rendered->parsed.document->getElementsByClass(QStringLiteral("row")).size(), 200);

    // The document is tall and the rows stack in order.
    QVERIFY(rendered->layout.documentHeight > 1000);

    dom::Element *first = rendered->parsed.document->getElementById(QStringLiteral("row0"));
    dom::Element *last = rendered->parsed.document->getElementById(QStringLiteral("row199"));
    QVERIFY(first && last);

    // The odd rows are shaded, so the page is not a blank field.
    int shaded = 0;
    for (int y = 0; y < rendered->image.height(); ++y) {
        if (qAbs(rendered->image.pixelColor(5, y).red() - 238) < 8)
            ++shaded;
    }
    QVERIFY(shaded > 10);
}

void PipelineTest::survivesMalformedMarkup()
{
    // Unclosed tags, a stray end tag, an unquoted attribute and a bare "<".
    auto rendered = runPipeline(QStringLiteral(R"(
        <!DOCTYPE html><html><head><style>body { margin: 0 }</style></head>
        <body><div><p>unclosed
        <span>text</p></div></span>
        <img src=x.png alt=missing>
        <p>a < b</p>
        </body></html>)"));

    // The pipeline completes and still produces something sensible.
    QVERIFY(rendered->boxTree != nullptr);
    QVERIFY(!rendered->image.isNull());
    QVERIFY(rendered->layout.documentHeight > 0);
    QVERIFY(rendered->parsed.document->body() != nullptr);

    // The text survives even though the markup was broken.
    const QString text = rendered->parsed.document->body()->textContent();
    QVERIFY(text.contains(QStringLiteral("unclosed")));
    QVERIFY(text.contains(QStringLiteral("a < b")));
}

QTEST_MAIN(PipelineTest)
#include "tst_pipeline.moc"
