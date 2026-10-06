#include "renderer/BoxTree.h"

#include "css/Style.h"
#include "dom/Document.h"

#include <QSet>

namespace oqb::renderer {
namespace {

/// A style engine installed for the duration of a build. The box tree stores
/// raw style pointers, so the engine has to outlive it; keeping the pointer in
/// one place documents that requirement and keeps the recursive helpers free of
/// extra parameters.
const css::StyleEngine *g_styleEngine = nullptr;

/// Decoded image sizes by URL; null when the caller keeps none.
const QHash<QString, QSizeF> *g_imageSizes = nullptr;

/// Elements that are replaced: they have intrinsic dimensions and no children.
bool isReplacedElement(const dom::Element *element)
{
    if (!element)
        return false;

    static const QSet<QString> kReplaced = {
        QStringLiteral("img"),      QStringLiteral("image"),   QStringLiteral("video"),
        QStringLiteral("audio"),    QStringLiteral("canvas"),  QStringLiteral("iframe"),
        QStringLiteral("embed"),    QStringLiteral("object"),  QStringLiteral("input"),
        QStringLiteral("select"),   QStringLiteral("textarea"), QStringLiteral("svg"),
        QStringLiteral("math"),
    };
    return kReplaced.contains(element->tagName());
}

/// True when an element never renders content of its own.
bool isNonRenderedElement(const QString &tag)
{
    static const QSet<QString> kNonRendered = {
        QStringLiteral("head"),   QStringLiteral("title"),  QStringLiteral("meta"),
        QStringLiteral("link"),   QStringLiteral("style"),  QStringLiteral("script"),
        QStringLiteral("base"),   QStringLiteral("template"), QStringLiteral("noscript"),
        QStringLiteral("param"),  QStringLiteral("source"), QStringLiteral("track"),
        QStringLiteral("datalist"),
    };
    return kNonRendered.contains(tag);
}

/// Applies text-transform and white-space collapsing to a text run.
QString transformText(const QString &raw, const css::ComputedStyle *style, bool collapseWhitespace)
{
    QString text = collapseWhitespace ? raw.simplified() : raw;

    if (!style)
        return text;

    const QString &transform = style->textTransform;
    if (transform == QLatin1String("uppercase"))
        text = text.toUpper();
    else if (transform == QLatin1String("lowercase"))
        text = text.toLower();
    else if (transform == QLatin1String("capitalize")) {
        bool atWordStart = true;
        for (QChar &c : text) {
            if (c.isSpace()) {
                atWordStart = true;
                continue;
            }
            if (atWordStart) {
                c = c.toUpper();
                atWordStart = false;
            }
        }
    }

    return text;
}

/// True when a whitespace-only text node can be dropped: between block-level
/// siblings it has no effect on layout (CSS 2.2 §9.2.2.1).
bool isIgnorableWhitespace(dom::Node *node)
{
    if (!node || !node->isText())
        return false;
    const auto *text = static_cast<const dom::Text *>(node);
    return text->isWhitespaceOnly();
}

} // namespace

// ------------------------------------------------------------------- Box

dom::Element *Box::element() const
{
    if (m_node && m_node->isElement())
        return static_cast<dom::Element *>(m_node);
    return nullptr;
}

Box *Box::appendChild(std::unique_ptr<Box> child)
{
    if (!child)
        return nullptr;
    child->m_parent = this;
    Box *raw = child.get();
    m_children.push_back(std::move(child));
    return raw;
}

std::unique_ptr<Box> Box::detachChild(Box *child)
{
    for (auto it = m_children.begin(); it != m_children.end(); ++it) {
        if (it->get() == child) {
            std::unique_ptr<Box> owned = std::move(*it);
            m_children.erase(it);
            owned->m_parent = nullptr;
            return owned;
        }
    }
    return nullptr;
}

Box *Box::childAt(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_children.size()))
        return nullptr;
    return m_children[static_cast<size_t>(index)].get();
}

Box *Box::firstBlockChild() const
{
    for (const auto &child : m_children) {
        if (child->isBlockLevel())
            return child.get();
    }
    return nullptr;
}

Box *Box::containingBlock() const
{
    for (Box *ancestor = m_parent; ancestor; ancestor = ancestor->m_parent) {
        if (ancestor->isBlockLevel() || ancestor->type() == Type::InlineBlock)
            return ancestor;
    }
    return m_parent;
}

QRectF Box::paddingBox() const
{
    if (!m_style)
        return m_borderBox;

    // Derived from the border box rather than from the content box, because the
    // content box is relative to the box's own origin and this has to be in
    // document coordinates. The border box is already absolute.
    QRectF rect = m_borderBox;
    rect.adjust(m_style->borderLeftWidth, m_style->borderTopWidth,
                -m_style->borderRightWidth, -m_style->borderBottomWidth);
    return rect;
}

QRectF Box::marginBox() const
{
    QRectF rect = m_borderBox;
    if (m_style) {
        const double top = m_style->marginTop.isLength() ? m_style->marginTop.value : 0;
        const double right = m_style->marginRight.isLength() ? m_style->marginRight.value : 0;
        const double bottom = m_style->marginBottom.isLength() ? m_style->marginBottom.value : 0;
        const double left = m_style->marginLeft.isLength() ? m_style->marginLeft.value : 0;
        rect.adjust(-left, -top, right, bottom);
    }
    return rect;
}

Box *Box::positionedAncestor() const
{
    for (Box *ancestor = m_parent; ancestor; ancestor = ancestor->m_parent) {
        const css::ComputedStyle *style = ancestor->style();
        if (style && style->isPositioned())
            return ancestor;
    }
    return nullptr;
}

bool Box::isOutOfFlow() const
{
    // A layout artifact has no element of its own and is never positioned, even
    // though it carries the style that produced it.
    switch (m_type) {
    case Type::Anonymous:
    case Type::Line:
    case Type::Text:
    case Type::Bullet:
        return false;
    default:
        break;
    }

    return m_style && m_style->isAbsolutelyPositioned();
}

bool Box::establishesStackingContext() const
{
    // The root box always does. Otherwise a z-index other than auto on a
    // positioned box does it. `opacity` and `transform` also create one, but
    // neither is honoured by this engine, so claiming they did would be wrong.
    if (!m_parent)
        return true;

    const css::ComputedStyle *style = m_style;
    if (!style)
        return false;
    return style->isPositioned() && style->hasZIndex;
}

void Box::translate(double dx, double dy)
{
    m_borderBox.translate(dx, dy);
    m_contentBox.translate(dx, dy);
    for (const auto &child : m_children)
        child->translate(dx, dy);
}

// ------------------------------------------------------- BoxTreeBuilder

void BoxTreeBuilder::setStyleEngine(const css::StyleEngine *engine)
{
    g_styleEngine = engine;
}

void BoxTreeBuilder::setImageSizes(const QHash<QString, QSizeF> *sizes)
{
    g_imageSizes = sizes;
}

bool BoxTreeBuilder::isReplaced(const dom::Element *element)
{
    return isReplacedElement(element);
}

std::unique_ptr<Box> BoxTreeBuilder::build(dom::Document *document)
{
    if (!document || !document->documentElement() || !g_styleEngine)
        return nullptr;

    return buildForNode(document->documentElement(), nullptr);
}

std::unique_ptr<Box> BoxTreeBuilder::buildForNode(dom::Node *node,
                                                  const css::ComputedStyle *parentStyle)
{
    if (!node)
        return nullptr;

    // Text nodes become text boxes under the parent's style.
    if (node->isText()) {
        auto *textNode = static_cast<dom::Text *>(node);
        auto box = std::make_unique<Box>(Box::Type::Text, node, parentStyle);
        const bool collapseWhitespace = !parentStyle
            || (parentStyle->whiteSpace != QLatin1String("pre")
                && parentStyle->whiteSpace != QLatin1String("pre-wrap"));
        box->setText(transformText(textNode->data(), parentStyle, collapseWhitespace));
        return box;
    }

    if (!node->isElement())
        return nullptr; // Comments and doctypes generate no boxes.

    auto *element = static_cast<dom::Element *>(node);

    const css::ComputedStyle *style = g_styleEngine->hasStyleFor(element)
        ? &g_styleEngine->styleFor(element)
        : parentStyle;

    if (!style || !style->generatesBox() || isNonRenderedElement(element->tagName()))
        return nullptr;

    // A replaced element produces a leaf box with intrinsic dimensions.
    if (isReplacedElement(element)) {
        auto box = std::make_unique<Box>(Box::Type::Replaced, node, style);
        const QString source = element->attribute(QStringLiteral("src"));
        if (!source.isEmpty()) {
            box->setSourceUrl(source);
            // An image that has already been decoded contributes its real size
            // here, so layout gives it the right dimensions on this pass.
            if (g_imageSizes) {
                const network::Url resolved = element->ownerDocument()
                    ? element->ownerDocument()->resolveUrl(source)
                    : network::Url::parse(source);
                if (const auto it = g_imageSizes->constFind(resolved.toString());
                    it != g_imageSizes->constEnd()) {
                    box->setIntrinsicSize(it.value());
                }
            }
        }
        return box;
    }

    Box::Type type = Box::Type::Inline;
    if (style->isBlockLevel())
        type = Box::Type::Block;
    else if (style->display == QLatin1String("inline-block"))
        type = Box::Type::InlineBlock;
    else if (style->isPositioned() && style->display == QLatin1String("inline")) {
        // A positioned inline element keeps a box of its own rather than being
        // dissolved into the line, because an absolutely positioned descendant
        // is placed against it. `<span style="position:relative">` wrapping a
        // dropdown is the pattern this exists for, and dissolving the span would
        // leave the dropdown with the viewport as its containing block.
        //
        // The specification puts the containing block at the union of the
        // inline's fragments, which would let the span break across lines. An
        // inline-block is the closest this engine gets: it is atomic, so it does
        // not break, which for a label or a menu trigger is what the author
        // wants anyway.
        type = Box::Type::InlineBlock;
    }

    auto box = std::make_unique<Box>(type, node, style);

    // <br> is not replaced, but it has no children and forces a line break that
    // the inline layout pass reads from its tag name.
    if (element->isTag(QStringLiteral("br")))
        return box;

    // Build the children, then wrap any inline content in an anonymous block so
    // that a block container never mixes block and inline children directly.
    std::vector<std::unique_ptr<Box>> inlineRun;

    const auto flushInlineRun = [&]() {
        if (inlineRun.empty())
            return;
        // A block container and an inline-block both need the wrapper, because
        // both establish a formatting context of their own. A plain inline box
        // keeps its children as they are so that text can flow through it.
        //
        // A flex container is the exception: its children are flex items, and
        // each one has to be an item in its own right. Wrapping them in an
        // anonymous block would make the whole run a single item, which is why
        // `justify-content` had nothing to distribute between.
        // A grid container is the same: its children are grid items, and wrapping
        // them in an anonymous block would make the whole run one item that
        // occupies a single cell.
        const bool flexContainer = style->isFlexContainer() || style->isGridContainer();
        if (!flexContainer && (type == Box::Type::Block || type == Box::Type::InlineBlock)) {
            auto anonymous = std::make_unique<Box>(Box::Type::Anonymous, nullptr, style);
            for (auto &child : inlineRun)
                anonymous->appendChild(std::move(child));
            inlineRun.clear();
            box->appendChild(std::move(anonymous));
        } else {
            for (auto &child : inlineRun)
                box->appendChild(std::move(child));
            inlineRun.clear();
        }
    };

    for (const auto &childNode : element->children()) {
        // Whitespace-only text between block children is discarded; keeping it
        // would create anonymous blocks with no content.
        if (isIgnorableWhitespace(childNode.get()) && type == Box::Type::Block) {
            bool hasBlockSibling = false;
            for (const auto &sibling : element->children()) {
                if (sibling->isElement() && g_styleEngine->hasStyleFor(
                        static_cast<dom::Element *>(sibling.get()))
                    && g_styleEngine->styleFor(static_cast<dom::Element *>(sibling.get()))
                           .isBlockLevel()) {
                    hasBlockSibling = true;
                    break;
                }
            }
            if (hasBlockSibling)
                continue;
        }

        std::unique_ptr<Box> childBox = buildForNode(childNode.get(), style);
        if (!childBox)
            continue;

        // A block-level child of a block container starts a new block; an
        // inline-block is itself inline, so its own content is wrapped below
        // rather than being split across the parent's formatting context.
        if (childBox->isBlockLevel() && type == Box::Type::Block) {
            flushInlineRun();
            box->appendChild(std::move(childBox));
        } else {
            inlineRun.push_back(std::move(childBox));
        }
    }

    flushInlineRun();

    return box;
}

} // namespace oqb::renderer
