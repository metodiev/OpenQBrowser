#include <QtTest>

#include "css/Style.h"
#include "html/Parser.h"
#include "renderer/BoxTree.h"
#include "renderer/Layout.h"

using namespace oqb;

/// Guards against layout behaving worse than linearly in the size of the
/// document. These are not micro-benchmarks: they exist because a real page
/// (a Wikipedia article) took over a minute to lay out, which is the shape of
/// bug that only shows up on content nobody wrote by hand.
class PerfTest : public QObject
{
    Q_OBJECT

private:
    /// Lays out `html` and returns the milliseconds it took.
    static qint64 timeLayout(const QString &html, int viewport = 1024)
    {
        auto parsed = html::Parser::parse(html);
        css::StyleEngine engine;
        // The user agent sheet is applied by the engine itself.
        for (auto *style : parsed.document->getElementsByTagName(QStringLiteral("style")))
            engine.addStylesheet(css::Stylesheet::parse(style->textContent()));
        engine.computeStyles(parsed.document.get());

        renderer::BoxTreeBuilder::setStyleEngine(&engine);
        auto boxTree = renderer::BoxTreeBuilder::build(parsed.document.get());
        if (!boxTree)
            return -1;

        renderer::LayoutEngine layout;
        layout.setViewport(viewport, 768);

        QElapsedTimer clock;
        clock.start();
        layout.layout(boxTree.get());
        return clock.elapsed();
    }

private slots:
    void flatDocumentIsFast();
    void manyInlineBlocksAreFast();
    void nestedInlineBlocksAreFast();
    void scalesLinearlyWithElementCount();
};

void PerfTest::flatDocumentIsFast()
{
    QString html = QStringLiteral("<!DOCTYPE html><html><body style=\"margin:0\">");
    for (int i = 0; i < 2000; ++i)
        html += QStringLiteral("<p>Paragraph number %1 with a little text in it.</p>").arg(i);
    html += QStringLiteral("</body></html>");

    const qint64 elapsed = timeLayout(html);
    QVERIFY2(elapsed >= 0 && elapsed < 5000,
             qPrintable(QStringLiteral("2000 paragraphs took %1ms").arg(elapsed)));
}

void PerfTest::manyInlineBlocksAreFast()
{
    QString html = QStringLiteral("<!DOCTYPE html><html><head><style>"
                                  ".b{display:inline-block;padding:1px;background:#eee}"
                                  "</style></head><body style=\"margin:0\"><p>");
    for (int i = 0; i < 200; ++i)
        html += QStringLiteral("<span class=\"b\">badge %1</span> ").arg(i);
    html += QStringLiteral("</p></body></html>");

    const qint64 elapsed = timeLayout(html);
    QVERIFY2(elapsed >= 0 && elapsed < 5000,
             qPrintable(QStringLiteral("200 inline-blocks took %1ms").arg(elapsed)));
}

void PerfTest::nestedInlineBlocksAreFast()
{
    // Inline-blocks inside inline-blocks, which is what a wiki page's icon and
    // badge markup produces.
    QString html = QStringLiteral("<!DOCTYPE html><html><head><style>"
                                  ".b{display:inline-block;padding:1px}"
                                  "</style></head><body style=\"margin:0\"><p>");
    for (int i = 0; i < 60; ++i) {
        html += QStringLiteral("<span class=\"b\">outer %1 <span class=\"b\">inner</span> "
                               "<span class=\"b\">another</span></span> ").arg(i);
    }
    html += QStringLiteral("</p></body></html>");

    const qint64 elapsed = timeLayout(html);
    QVERIFY2(elapsed >= 0 && elapsed < 8000,
             qPrintable(QStringLiteral("nested inline-blocks took %1ms").arg(elapsed)));
}

void PerfTest::scalesLinearlyWithElementCount()
{
    const auto documentWith = [](int count) {
        QString html = QStringLiteral("<!DOCTYPE html><html><body style=\"margin:0\">"
                                      "<div><div><div>");
        for (int i = 0; i < count; ++i)
            html += QStringLiteral("<p>Text %1</p>").arg(i);
        return html + QStringLiteral("</div></div></div></body></html>");
    };

    const qint64 small = timeLayout(documentWith(300));
    const qint64 large = timeLayout(documentWith(2400));

    // Eight times the elements must not cost dramatically more than eight times
    // the time; a generous factor leaves room for allocator noise while still
    // catching quadratic behaviour.
    QVERIFY2(small >= 0 && large >= 0, "layout failed");
    const qint64 budget = qMax<qint64>(300, small * 24);
    QVERIFY2(large < budget,
             qPrintable(QStringLiteral("300 elements: %1ms, 2400 elements: %2ms")
                            .arg(small)
                            .arg(large)));
}

QTEST_MAIN(PerfTest)
#include "tst_perf.moc"
