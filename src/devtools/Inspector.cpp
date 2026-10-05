#include "devtools/Inspector.h"

#include "css/Style.h"
#include "dom/Document.h"
#include "javascript/ScriptEngine.h"
#include "renderer/BoxTree.h"
#include "renderer/Layout.h"

#include <functional>

namespace oqb::devtools {
namespace {

QString indent(int depth)
{
    return QString(depth * 2, u' ');
}

void walkBoxes(const renderer::Box *box, int depth, QString *out, bool geometryOnly)
{
    if (!box)
        return;

    // Anonymous and line boxes are structural noise in a geometry listing, but
    // they are worth showing in the full tree.
    const bool interesting = geometryOnly
        ? (box->type() != renderer::Box::Type::Anonymous)
        : true;

    if (interesting) {
        *out += indent(depth);

        if (geometryOnly) {
            *out += QStringLiteral("%1 %2 %3 %4")
                        .arg(box->x(), 0, 'f', 1)
                        .arg(box->y(), 0, 'f', 1)
                        .arg(box->width(), 0, 'f', 1)
                        .arg(box->height(), 0, 'f', 1);
        } else {
            static const QHash<int, QString> kTypeNames = {
                {int(renderer::Box::Type::Block), QStringLiteral("block")},
                {int(renderer::Box::Type::Inline), QStringLiteral("inline")},
                {int(renderer::Box::Type::InlineBlock), QStringLiteral("inline-block")},
                {int(renderer::Box::Type::Text), QStringLiteral("text")},
                {int(renderer::Box::Type::Replaced), QStringLiteral("replaced")},
                {int(renderer::Box::Type::Line), QStringLiteral("line")},
                {int(renderer::Box::Type::Anonymous), QStringLiteral("anonymous")},
                {int(renderer::Box::Type::Bullet), QStringLiteral("bullet")},
            };

            *out += kTypeNames.value(int(box->type()), QStringLiteral("box"));
            if (const dom::Element *element = box->element())
                *out += u' ' + element->describe();
            else if (box->node())
                *out += u' ' + box->node()->nodeName();
        }

        if (!geometryOnly) {
            *out += QStringLiteral("  [%1, %2, %3 x %4]")
                        .arg(box->x(), 0, 'f', 0)
                        .arg(box->y(), 0, 'f', 0)
                        .arg(box->width(), 0, 'f', 0)
                        .arg(box->height(), 0, 'f', 0);
            if (!box->text().isEmpty())
                *out += QStringLiteral("  \"") + box->text().left(40) + u'"';
        }

        *out += u'\n';
    }

    for (const auto &child : box->children())
        walkBoxes(child.get(), depth + 1, out, geometryOnly);
}

/// Collects the external resources a document references.
struct ResourceReference
{
    QString kind;
    QString url;
};

QList<ResourceReference> references(const dom::Document *document)
{
    QList<ResourceReference> out;
    if (!document)
        return out;

    for (dom::Element *element : document->getElementsByTagName(QStringLiteral("link"))) {
        const QString rel = element->attribute(QStringLiteral("rel")).toLower();
        if (rel.contains(QLatin1String("stylesheet"))) {
            out.append({QStringLiteral("stylesheet"),
                        element->attribute(QStringLiteral("href"))});
        }
    }
    for (dom::Element *element : document->getElementsByTagName(QStringLiteral("script"))) {
        const QString src = element->attribute(QStringLiteral("src"));
        if (!src.isEmpty())
            out.append({QStringLiteral("script"), src});
    }
    for (dom::Element *element : document->getElementsByTagName(QStringLiteral("img"))) {
        const QString src = element->attribute(QStringLiteral("src"));
        if (!src.isEmpty())
            out.append({QStringLiteral("image"), src});
    }
    return out;
}

} // namespace

QString Inspector::domTree(const dom::Document *document)
{
    if (!document)
        return QStringLiteral("(no document)\n");
    return document->toTreeString();
}

QString Inspector::computedStyles(const css::StyleEngine *engine, const dom::Element *element)
{
    if (!engine || !element)
        return QStringLiteral("(no element)\n");

    if (!engine->hasStyleFor(element)) {
        return QStringLiteral("(element has no computed style; it may not generate a box)\n");
    }

    const css::ComputedStyle &style = engine->styleFor(element);

    QString out;
    out += QStringLiteral("display: %1\n").arg(style.display);
    out += QStringLiteral("position: %1\n").arg(style.position);
    out += QStringLiteral("width: %1\n")
               .arg(style.width.isAuto() ? QStringLiteral("auto")
                                         : QString::number(style.width.resolve(0)) + QStringLiteral("px"));
    out += QStringLiteral("height: %1\n")
               .arg(style.height.isAuto()
                        ? QStringLiteral("auto")
                        : QString::number(style.height.resolve(0)) + QStringLiteral("px"));
    out += QStringLiteral("color: %1\n").arg(style.color.name(QColor::HexRgb));
    out += QStringLiteral("background-color: %1\n")
               .arg(style.backgroundColor.alpha() == 0
                        ? QStringLiteral("transparent")
                        : style.backgroundColor.name(QColor::HexArgb));
    out += QStringLiteral("font-size: %1px\n").arg(style.fontSize);
    out += QStringLiteral("font-weight: %1\n").arg(style.fontWeight);
    out += QStringLiteral("font-style: %1\n")
               .arg(style.italic ? QStringLiteral("italic") : QStringLiteral("normal"));
    out += QStringLiteral("line-height: %1px\n").arg(style.lineHeight);
    out += QStringLiteral("text-align: %1\n").arg(style.textAlign);
    out += QStringLiteral("margin: %1 %2 %3 %4\n")
               .arg(style.marginTop.resolve(0))
               .arg(style.marginRight.resolve(0))
               .arg(style.marginBottom.resolve(0))
               .arg(style.marginLeft.resolve(0));
    out += QStringLiteral("padding: %1 %2 %3 %4\n")
               .arg(style.paddingTop.resolve(0))
               .arg(style.paddingRight.resolve(0))
               .arg(style.paddingBottom.resolve(0))
               .arg(style.paddingLeft.resolve(0));
    out += QStringLiteral("border: %1 %2 %3\n")
               .arg(style.borderTopWidth)
               .arg(style.borderTopStyle)
               .arg(style.borderTopColor.name(QColor::HexRgb));
    if (!style.fontFamilies.isEmpty())
        out += QStringLiteral("font-family: %1\n").arg(style.fontFamilies.join(QStringLiteral(", ")));

    return out;
}

QString Inspector::appliedRules(const css::StyleEngine *engine, const dom::Element *element)
{
    if (!engine || !element)
        return QStringLiteral("(no element)\n");

    const QList<css::StyleEngine::AppliedDeclaration> declarations
        = engine->declarationsFor(element);

    if (declarations.isEmpty())
        return QStringLiteral("(no declarations matched this element)\n");

    QString out;
    for (const auto &declaration : declarations) {
        out += declaration.fromInlineStyle ? QStringLiteral("<style attribute>")
                                           : declaration.selector;
        out += QStringLiteral("  { ") + declaration.property + QStringLiteral(": ")
            + declaration.value;
        if (declaration.important)
            out += QStringLiteral(" !important");
        out += QStringLiteral(" }");
        if (!declaration.used)
            out += QStringLiteral("   [not applied]");
        out += u'\n';
    }
    return out;
}

QString Inspector::boxTree(const renderer::Box *root)
{
    QString out;
    walkBoxes(root, 0, &out, false);
    return out.isEmpty() ? QStringLiteral("(no boxes)\n") : out;
}

QString Inspector::geometry(const renderer::Box *root)
{
    QString out;
    walkBoxes(root, 0, &out, true);
    return out.isEmpty() ? QStringLiteral("(no boxes)\n") : out;
}

QString Inspector::layoutSummary(const renderer::LayoutResult &result)
{
    QString out;
    out += QStringLiteral("document: %1 x %2\n")
               .arg(result.documentWidth, 0, 'f', 1)
               .arg(result.documentHeight, 0, 'f', 1);
    out += QStringLiteral("line boxes: %1\n").arg(result.lineBoxes.size());
    for (const QString &warning : result.warnings)
        out += QStringLiteral("warning: %1\n").arg(warning);
    return out;
}

QString Inspector::scriptSummary(const javascript::ScriptEngine *engine,
                                 const dom::Document *document)
{
    QString out;

    int inlineScripts = 0;
    int externalScripts = 0;
    if (document) {
        for (dom::Element *element : document->getElementsByTagName(QStringLiteral("script"))) {
            if (element->hasAttribute(QStringLiteral("src")))
                ++externalScripts;
            else
                ++inlineScripts;
        }
    }

    out += QStringLiteral("scripts in the document: %1 inline, %2 external\n")
               .arg(inlineScripts)
               .arg(externalScripts);

    if (!engine) {
        out += QStringLiteral("no script engine\n");
        return out;
    }

    out += engine->isAvailable() ? QStringLiteral("engine: available\n")
                                 : QStringLiteral("engine: none\n");
    out += javascript::ScriptEngine::availabilityNote();
    out += u'\n';

    for (const QString &skipped : engine->skippedScripts())
        out += QStringLiteral("skipped: %1\n").arg(skipped);

    for (const javascript::ConsoleMessage &message : engine->messages())
        out += QStringLiteral("console: %1\n").arg(message.text);

    return out;
}

QString Inspector::resourceSummary(const dom::Document *document)
{
    const QList<ResourceReference> refs = references(document);
    if (refs.isEmpty())
        return QStringLiteral("(none)\n");

    QString out;
    for (const ResourceReference &reference : refs) {
        out += QStringLiteral("%1: %2\n")
                   .arg(reference.kind.leftJustified(10), reference.url);
    }
    return out;
}

QString Inspector::fullReport(const dom::Document *document, const css::StyleEngine *engine,
                              const renderer::Box *root, const renderer::LayoutResult &layout,
                              const javascript::ScriptEngine *scripts)
{
    QString out;

    out += QStringLiteral("=== DOM ===\n");
    out += domTree(document);

    out += QStringLiteral("\n=== LAYOUT ===\n");
    out += layoutSummary(layout);
    out += QStringLiteral("\n=== BOXES ===\n");
    out += boxTree(root);

    out += QStringLiteral("\n=== STYLES ===\n");
    out += QStringLiteral("stylesheet rules parsed: %1\n")
               .arg(engine ? engine->declarationsFor(document ? document->body() : nullptr).size() : 0);
    if (engine) {
        for (const QString &warning : engine->warnings())
            out += QStringLiteral("warning: %1\n").arg(warning);
    }

    out += QStringLiteral("\n=== SCRIPTS ===\n");
    out += scriptSummary(scripts, document);

    out += QStringLiteral("\n=== RESOURCES ===\n");
    out += resourceSummary(document);

    return out;
}

QString Inspector::pageSummary(const dom::Document *document, const renderer::Box *root)
{
    const QString title = document ? document->title() : QString();
    if (!title.isEmpty())
        return title;
    if (document)
        return document->url().displayHost();
    Q_UNUSED(root);
    return QStringLiteral("OpenQBrowser");
}

} // namespace oqb::devtools
