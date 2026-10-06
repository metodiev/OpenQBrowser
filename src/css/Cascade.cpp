#include "css/Style.h"

#include "css/Tokenizer.h"

#include "dom/Document.h"
#include "dom/Node.h"

#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <functional>

namespace oqb::css {

const QString &StyleContext::userAgentStylesheet()
{
    // The browser defaults every engine starts from, so an unstyled page still
    // looks like a document rather than a stream of characters.
    static const QString kStylesheet = QStringLiteral(R"CSS(
html, body, address, article, aside, blockquote, div, dd, dl, dt, fieldset,
figcaption, figure, footer, form, h1, h2, h3, h4, h5, h6, header, hr, legend,
main, nav, ol, p, pre, section, summary, ul { display: block; }
head, link, meta, style, script, title, base, template, datalist, param { display: none; }

body { margin: 8px; }

p, blockquote, figure, dl, ol, ul, pre { margin-top: 1em; margin-bottom: 1em; }
blockquote, figure { margin-left: 40px; margin-right: 40px; }
ol, ul { padding-left: 40px; }
ul { list-style-type: disc; }
ul ul { list-style-type: circle; }
ul ul ul { list-style-type: square; }
ol { list-style-type: decimal; }
li { display: list-item; }

h1 { font-size: 2em;    margin-top: 0.67em; margin-bottom: 0.67em; font-weight: bold; }
h2 { font-size: 1.5em;  margin-top: 0.83em; margin-bottom: 0.83em; font-weight: bold; }
h3 { font-size: 1.17em; margin-top: 1em;    margin-bottom: 1em;    font-weight: bold; }
h4 { font-size: 1em;    margin-top: 1.33em; margin-bottom: 1.33em; font-weight: bold; }
h5 { font-size: 0.83em; margin-top: 1.67em; margin-bottom: 1.67em; font-weight: bold; }
h6 { font-size: 0.67em; margin-top: 2.33em; margin-bottom: 2.33em; font-weight: bold; }

pre, code, kbd, samp, tt { font-family: monospace; }
pre { white-space: pre; }
b, strong, th { font-weight: bold; }
i, em, cite, dfn, var, address { font-style: italic; }
small { font-size: smaller; }
big { font-size: larger; }
sub { vertical-align: sub; font-size: smaller; }
sup { vertical-align: super; font-size: smaller; }
u, ins, a { text-decoration: underline; }
s, strike, del { text-decoration: line-through; }
mark { background-color: yellow; color: black; }
a { color: #0000EE; }
a:visited { color: #551A8B; }

hr { margin-top: 0.5em; margin-bottom: 0.5em;
     border-top-width: 1px; border-top-style: inset;
     border-right-style: none; border-bottom-style: none; border-left-style: none; }
table { display: table; border-collapse: separate; border-spacing: 2px; }
thead { display: table-header-group; }
tbody { display: table-row-group; }
tfoot { display: table-footer-group; }
tr { display: table-row; }
td, th { display: table-cell; padding: 1px; vertical-align: middle; }
th { text-align: center; font-weight: bold; }
caption { display: table-caption; text-align: center; }

img, video, canvas, svg, iframe, object { display: inline-block; }
br, wbr { display: inline; }
iframe { border-width: 2px; border-style: inset; }
button, input, select, textarea, optgroup { font-family: sans-serif; font-size: 13.3333px; }
textarea { white-space: pre-wrap; }
fieldset { border-width: 2px; border-style: groove; padding: 0.35em 0.75em 0.625em; }
legend { padding-left: 2px; padding-right: 2px; }
[hidden] { display: none; }
)CSS");
    return kStylesheet;
}

namespace {

/// Reads a length or "auto" from a value, resolving absolute units to pixels.
/// Relative units are resolved against the values the caller passes in, which
/// are known by the time this runs.
LengthOrAuto resolveLength(const Value &value, double fontSize, double rootFontSize,
                            double viewportWidth, double viewportHeight, bool allowAuto,
                            bool allowPercentage = true, bool allowNegative = true)
{
    LengthOrAuto result;

    if (!value.isValid())
        return result;

    switch (value.kind) {
    case Value::Kind::Keyword:
        if (allowAuto && value.isKeyword(QStringLiteral("auto")))
            return LengthOrAuto::autoValue();
        if (value.isKeyword(QStringLiteral("none")))
            return LengthOrAuto::autoValue();
        return result;
    case Value::Kind::Percentage:
        if (!allowPercentage)
            return result;
        return LengthOrAuto::percent(value.number, value.original);
    case Value::Kind::Number:
        if (qFuzzyIsNull(value.number))
            return LengthOrAuto::pixels(0, QStringLiteral("0"));
        return result;
    case Value::Kind::Length: {
        double pixels = 0;
        if (!values::lengthToPixels(value.number, value.unit, fontSize, rootFontSize,
                                    viewportWidth, viewportHeight, &pixels)) {
            return result;
        }
        if (!allowNegative && pixels < 0)
            return result;
        return LengthOrAuto::pixels(pixels, value.original);
    }
    default:
        return result;
    }
}

/// Maps the author's text-align keyword onto the ones layout understands.
/// Anything unrecognised falls back to "start", which behaves as "left" in a
/// left-to-right document.
QString normalizeAlign(const QString &keyword)
{
    if (keyword == QLatin1String("left") || keyword == QLatin1String("right")
        || keyword == QLatin1String("center") || keyword == QLatin1String("justify")) {
        return keyword;
    }
    if (keyword == QLatin1String("end"))
        return QStringLiteral("right");
    if (keyword == QLatin1String("start") || keyword == QLatin1String("match-parent")
        || keyword == QLatin1String("-webkit-match-parent")) {
        return QStringLiteral("start");
    }
    return QStringLiteral("start");
}

/// The four values of a shorthand such as "margin: 1px 2px", expanded into
/// top/right/bottom/left following the CSS rules.
QList<QString> splitBoxValues(const QString &raw)
{
    const QStringList parts = raw.split(QRegularExpression(QStringLiteral("\\s+")),
                                        Qt::SkipEmptyParts);
    QList<QString> out;
    switch (parts.size()) {
    case 1:
        out = {parts.at(0), parts.at(0), parts.at(0), parts.at(0)};
        break;
    case 2:
        out = {parts.at(0), parts.at(1), parts.at(0), parts.at(1)};
        break;
    case 3:
        out = {parts.at(0), parts.at(1), parts.at(2), parts.at(1)};
        break;
    default:
        out = {parts.value(0), parts.value(1), parts.value(2), parts.value(3)};
        break;
    }
    return out;
}

/// Parses a standalone length text such as "4px" for shorthand expansion.
///
/// Lengths keep their unit so that percentage and em values can be resolved
/// later against the element's own font size and containing block, which is what
/// makes "margin: 2em" and "width: 50%" correct.
Value parseSingleValue(const QString &text)
{
    css::Tokenizer tokenizer(text);
    QList<Token> tokens;
    while (true) {
        const Token token = tokenizer.nextToken();
        if (token.type == TokenType::EndOfFile)
            break;
        tokens.append(token);
    }
    if (tokens.isEmpty())
        return Value::invalid();

    const Token &token = tokens.first();
    switch (token.type) {
    case TokenType::Ident:
        if (QColor color; values::colorFromKeyword(token.value, &color))
            return Value::fromColor(color, token.text);
        return Value::fromKeyword(token.value);
    case TokenType::Dimension:
        // Not resolved here: the caller applies its own font size.
        return Value::fromLength(token.number, token.unit, token.text);
    case TokenType::Percentage:
        return Value::fromPercentage(token.number, token.text);
    case TokenType::Number:
        return Value::fromNumber(token.number, token.text);
    case TokenType::Hash:
        return values::parseColor(token.text);
    default:
        return Value::invalid();
    }
}

/// Folds every presentational hint of an element into (property, raw value)
/// pairs, as HTML 4.01 Appendix A and the HTML standard's "presentational hints"
/// section require.
QList<QPair<QString, QString>> presentationalHints(const dom::Element *element)
{
    QList<QPair<QString, QString>> hints;

    const auto add = [&hints](const QString &property, const QString &value) {
        if (!value.isEmpty())
            hints.append({property, value});
    };

    const QString align = element->attribute(QStringLiteral("align"));
    const QString color = element->attribute(QStringLiteral("color"));
    const QString bgcolor = element->attribute(QStringLiteral("bgcolor"));
    const QString background = element->attribute(QStringLiteral("background"));
    const QString width = element->attribute(QStringLiteral("width"));
    const QString height = element->attribute(QStringLiteral("height"));
    const QString border = element->attribute(QStringLiteral("border"));
    const QString cellPadding = element->attribute(QStringLiteral("cellpadding"));
    const QString cellSpacing = element->attribute(QStringLiteral("cellspacing"));
    const QString face = element->attribute(QStringLiteral("face"));
    const QString size = element->attribute(QStringLiteral("size"));
    const QString href = element->attribute(QStringLiteral("href"));

    if (!align.isEmpty()) {
        static const QSet<QString> textTags = {
            QStringLiteral("div"), QStringLiteral("p"), QStringLiteral("h1"), QStringLiteral("h2"),
            QStringLiteral("h3"), QStringLiteral("h4"), QStringLiteral("h5"), QStringLiteral("h6"),
            QStringLiteral("td"), QStringLiteral("th"), QStringLiteral("tr"), QStringLiteral("col"),
            QStringLiteral("colgroup"), QStringLiteral("tbody"), QStringLiteral("thead"),
            QStringLiteral("tfoot"), QStringLiteral("caption"), QStringLiteral("table"),
            QStringLiteral("hr"), QStringLiteral("legend"),
        };
        if (textTags.contains(element->tagName()))
            add(QStringLiteral("text-align"), align);
        if (element->isTag(QStringLiteral("img")) || element->isTag(QStringLiteral("table"))
            || element->isTag(QStringLiteral("iframe")) || element->isTag(QStringLiteral("input"))) {
            if (align.compare(QLatin1String("left"), Qt::CaseInsensitive) == 0)
                add(QStringLiteral("float"), QStringLiteral("left"));
            else if (align.compare(QLatin1String("right"), Qt::CaseInsensitive) == 0)
                add(QStringLiteral("float"), QStringLiteral("right"));
            else
                add(QStringLiteral("vertical-align"), align);
        }
    }

    if (!color.isEmpty() && element->isTag(QStringLiteral("font")))
        add(QStringLiteral("color"), color);
    if (!bgcolor.isEmpty())
        add(QStringLiteral("background-color"), bgcolor);
    if (!background.isEmpty())
        add(QStringLiteral("background-image"), QStringLiteral("url(") + background + u')');
    if (!face.isEmpty() && element->isTag(QStringLiteral("font")))
        add(QStringLiteral("font-family"), face);

    // width/height apply to images, tables, cells and horizontal rules. A bare
    // number means pixels, per HTML 4's rules for these attributes.
    const bool sized = element->isTag(QStringLiteral("img")) || element->isTag(QStringLiteral("table"))
        || element->isTag(QStringLiteral("td")) || element->isTag(QStringLiteral("th"))
        || element->isTag(QStringLiteral("hr")) || element->isTag(QStringLiteral("iframe"))
        || element->isTag(QStringLiteral("embed")) || element->isTag(QStringLiteral("object"))
        || element->isTag(QStringLiteral("video")) || element->isTag(QStringLiteral("canvas"));

    if (sized) {
        if (!width.isEmpty())
            add(QStringLiteral("width"), width.contains(u'%') ? width : width + QStringLiteral("px"));
        if (!height.isEmpty() && !element->isTag(QStringLiteral("hr")))
            add(QStringLiteral("height"), height.contains(u'%') ? height : height + QStringLiteral("px"));
    }
    if (!height.isEmpty() && element->isTag(QStringLiteral("td"))
        && element->attribute(QStringLiteral("valign")).isEmpty()) {
        // no-op: kept explicit so the intent of the size rules stays visible
    }

    if (!border.isEmpty() && (element->isTag(QStringLiteral("table"))
                              || element->isTag(QStringLiteral("img")))) {
        add(QStringLiteral("border-top-width"), border + QStringLiteral("px"));
        add(QStringLiteral("border-right-width"), border + QStringLiteral("px"));
        add(QStringLiteral("border-bottom-width"), border + QStringLiteral("px"));
        add(QStringLiteral("border-left-width"), border + QStringLiteral("px"));
        add(QStringLiteral("border-top-style"), QStringLiteral("solid"));
        add(QStringLiteral("border-right-style"), QStringLiteral("solid"));
        add(QStringLiteral("border-bottom-style"), QStringLiteral("solid"));
        add(QStringLiteral("border-left-style"), QStringLiteral("solid"));
    }
    if (!cellPadding.isEmpty())
        add(QStringLiteral("padding"), cellPadding + QStringLiteral("px"));
    Q_UNUSED(cellSpacing);

    if (element->isTag(QStringLiteral("font")) && !size.isEmpty())
        add(QStringLiteral("font-size"), size);

    // Anchors without an href are not links and lose the link colour.
    if (element->isTag(QStringLiteral("a")) && href.isEmpty()) {
        add(QStringLiteral("color"), QStringLiteral("inherit"));
        add(QStringLiteral("text-decoration"), QStringLiteral("none"));
    }

    return hints;
}

} // namespace

QString ComputedStyle::describe() const
{
    QStringList parts;
    parts << QStringLiteral("display: ") + display;
    parts << QStringLiteral("font-size: ") + QString::number(fontSize) + QStringLiteral("px");
    if (width.isLength())
        parts << QStringLiteral("width: ") + QString::number(width.value) + QStringLiteral("px");
    if (backgroundColor.alpha() > 0)
        parts << QStringLiteral("background: ") + backgroundColor.name(QColor::HexRgb);
    return parts.join(QStringLiteral("; "));
}

// ------------------------------------------------------------ StyleEngine

StyleEngine::StyleEngine(const StyleContext &context)
    : m_context(context)
{
    // The user agent sheet is an ordinary stylesheet, so there is one code path
    // for every declaration and the inspector can show where a default came from.
    m_sheets.prepend(Stylesheet::parse(StyleContext::userAgentStylesheet(), -1, 0));
}

void StyleEngine::addStylesheet(const Stylesheet &stylesheet)
{
    m_sheets.append(stylesheet);
}

void StyleEngine::clearStylesheets()
{
    m_sheets.clear();
    m_sheets.prepend(Stylesheet::parse(StyleContext::userAgentStylesheet(), -1, 0));
}

void StyleEngine::setBaseColors(const QColor &background, const QColor &text)
{
    m_baseBackground = background.isValid() ? background.name(QColor::HexArgb) : QString();
    m_baseText = text.isValid() ? text.name(QColor::HexArgb) : QString();
}

const ComputedStyle &StyleEngine::initialStyle()
{
    static const ComputedStyle kInitial;
    return kInitial;
}

const ComputedStyle &StyleEngine::styleFor(const dom::Element *element) const
{
    const auto it = m_styleIndex.constFind(element);
    return it == m_styleIndex.constEnd() ? initialStyle() : *it.value();
}

bool StyleEngine::hasStyleFor(const dom::Element *element) const
{
    return m_styleIndex.contains(element);
}

QList<StyleEngine::AppliedDeclaration> StyleEngine::declarationsFor(const dom::Element *element) const
{
    return m_applied.value(element);
}

void StyleEngine::computeStyles(dom::Document *document)
{
    m_styles.clear();
    m_styleIndex.clear();
    m_applied.clear();

    if (!document)
        return;

    dom::Element *root = document->documentElement();
    if (!root)
        return;

    // Iterative walk with an explicit stack keeps deep documents from
    // exhausting the call stack, which hostile pages would otherwise do.
    struct Frame
    {
        dom::Element *element;
        const ComputedStyle *parentStyle;
    };

    QList<Frame> stack;
    stack.append({root, nullptr});

    while (!stack.isEmpty()) {
        const Frame frame = stack.takeLast();

        // The style is appended before its children are pushed, so the pointer
        // handed to them stays valid for the lifetime of the engine.
        m_styles.push_back(computeFor(frame.element, frame.parentStyle));
        ComputedStyle *stylePointer = &m_styles.back();
        m_styleIndex.insert(frame.element, stylePointer);

        // Push children in reverse so they are processed in document order.
        const QList<dom::Element *> children = frame.element->childElements();
        for (int i = static_cast<int>(children.size()) - 1; i >= 0; --i)
            stack.append({children.at(i), stylePointer});
    }
}

ComputedStyle StyleEngine::computeFor(const dom::Element *element, const ComputedStyle *parentStyle)
{
    ComputedStyle style;

    // 1. Inheritance first: inherited properties keep the parent's value unless
    //    a declaration overrides them below.
    inheritFrom(&style, parentStyle);
    style.fontSize = parentStyle ? parentStyle->fontSize : m_context.rootFontSize;

    // A relative font size always means "relative to the inherited size", never
    // relative to whatever another declaration already applied, so that value is
    // carried through the rest of this method explicitly.
    const double inheritedFontSize = style.fontSize;

    // 2. Collect every matching declaration with its cascade rank.
    QList<Candidate> candidates;

    for (const Stylesheet &sheet : m_sheets) {
        const QList<StyleRule> rules
            = sheet.rulesForMedia(QStringLiteral("screen"), m_context.viewportWidth);
        for (const StyleRule &rule : rules) {
            for (const Selector &selector : rule.selectors) {
                if (!selector.matches(element))
                    continue;
                for (const Declaration &declaration : rule.declarations) {
                    candidates.append({&declaration, selector.specificity(),
                                       rule.origin < 0 ? 0 : 1, rule.order, selector.source,
                                       false});
                }
            }
        }
    }

    // 3. Presentational hints sit between the user agent sheet and author
    //    styles, which is where HTML says they belong.
    QList<Declaration> inlineDeclarations;
    if (element->hasAttribute(QStringLiteral("style"))) {
        QStringList errors;
        inlineDeclarations
            = Stylesheet::parseDeclarationList(element->attribute(QStringLiteral("style")),
                                               &errors);
        for (const Declaration &declaration : inlineDeclarations) {
            // An inline declaration has no selector, so it carries no
            // specificity; its rank is what makes it win over author rules.
            candidates.append({&declaration, 0, 2, 1'000'000, QStringLiteral("style"), true});
        }
    }

    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate &a, const Candidate &b) {
                         // !important declarations outrank everything else, and
                         // among them a lower origin wins, which is the reversal
                         // CSS Cascade §6.4.4 describes.
                         if (a.declaration->important != b.declaration->important)
                             return !a.declaration->important;
                         if (a.originRank != b.originRank)
                             return a.originRank < b.originRank;
                         if (a.specificity != b.specificity)
                             return a.specificity < b.specificity;
                         return a.order < b.order;
                     });

    QList<AppliedDeclaration> applied;
    QSet<QString> expanded;

    for (const Candidate &candidate : candidates) {
        const Declaration &declaration = *candidate.declaration;

        // A declaration that this engine cannot use is recorded but marked as
        // unused so the inspector can explain why nothing changed.
        bool consumed = false;
        applyDeclaration(&style, declaration, inheritedFontSize, &consumed);
        applied.append({declaration.property, declaration.value.toString(),
                        candidate.selectorText, declaration.important, candidate.fromInline,
                        consumed});
        if (consumed)
            expanded.insert(declaration.property);
    }

    // 4. Presentational hints fill in properties the cascade left alone.
    applyPresentationalHints(&style, element, inheritedFontSize);

    // 5. Defaults that depend on other computed values.
    if (style.lineHeight <= 0)
        style.lineHeight = style.fontSize * 1.2;

    // A root element inherits the window's base colours when nothing set them.
    if (!parentStyle) {
        if (!m_baseText.isEmpty() && !m_styleIndex.contains(element)) {
            // The base text colour is applied only when no author rule set one,
            // which is approximated here by checking the colour is still black.
            if (style.color == QColor(0, 0, 0) && !m_baseText.isEmpty())
                style.color = QColor(m_baseText);
        }
        if (!m_baseBackground.isEmpty() && style.backgroundColor.alpha() == 0)
            style.backgroundColor = QColor(m_baseBackground);
    }

    // The declarations are recorded so the inspector can show which rule set
    // each property and in what order. Without this the "rules that applied"
    // view has nothing to read: the list was built here and then discarded.
    m_applied.insert(element, applied);

    return style;
}

void StyleEngine::inheritFrom(ComputedStyle *style, const ComputedStyle *parent)
{
    if (!style || !parent)
        return;

    style->fontFamilies = parent->fontFamilies;
    style->fontSize = parent->fontSize;
    style->fontWeight = parent->fontWeight;
    style->italic = parent->italic;
    style->lineHeight = parent->lineHeight;
    style->textAlign = parent->textAlign;
    style->textTransform = parent->textTransform;
    style->whiteSpace = parent->whiteSpace;
    style->letterSpacing = parent->letterSpacing;
    style->wordSpacing = parent->wordSpacing;
    style->textOverflow = parent->textOverflow;
    style->color = parent->color;
    style->visibility = parent->visibility;
    style->listStyleType = parent->listStyleType;
    style->listStylePosition = parent->listStylePosition;
    style->borderCollapse = parent->borderCollapse;
    style->captionSide = parent->captionSide;
    style->direction = parent->direction;
    style->cursor = parent->cursor;
}

void StyleEngine::applyPresentationalHints(ComputedStyle *style, const dom::Element *element,
                                           double inheritedFontSize)
{
    const auto hints = presentationalHints(element);
    if (hints.isEmpty())
        return;

    for (const auto &hint : hints) {
        Declaration declaration;
        declaration.property = hint.first;
        declaration.value = parseSingleValue(hint.second);
        if (!declaration.value.isValid())
            continue;
        bool consumed = false;
        applyDeclaration(style, declaration, inheritedFontSize, &consumed);
    }
}

void StyleEngine::applyDeclaration(ComputedStyle *style, const Declaration &declaration,
                                   double inheritedFontSize, bool *consumed)
{
    const QString &property = declaration.property;
    const Value &value = declaration.value;
    if (consumed)
        *consumed = true;

    // currentColor resolves against the element's own text colour, which the
    // cascade may set later, so it is resolved at the end of the cascade pass.
    const bool isCurrentColor = value.isKeyword(QStringLiteral("currentcolor"));
    const auto colorOf = [&](const QColor &fallback) {
        if (value.isColor())
            return value.color;
        if (isCurrentColor)
            return style->color;
        return fallback;
    };

    // Lengths resolve with the element's font size, which inheritance has
    // already established, plus the viewport for the viewport units.
    const auto length = [&](bool allowAuto, bool allowPercentage = true,
                            bool allowNegative = true) {
        return resolveLength(value, style->fontSize, m_context.rootFontSize,
                              m_context.viewportWidth, m_context.viewportHeight, allowAuto,
                              allowPercentage, allowNegative);
    };

    const auto keywordOr = [&](const QString &fallback) {
        return value.isValid() && value.kind == Value::Kind::Keyword ? value.keyword : fallback;
    };

    // ------------------------------------------------------------ display
    if (property == QLatin1String("display")) {
        if (value.kind != Value::Kind::Keyword) {
            if (consumed)
                *consumed = false;
            return;
        }
        static const QSet<QString> kKnown = {
            QStringLiteral("none"),         QStringLiteral("block"),      QStringLiteral("inline"),
            QStringLiteral("inline-block"), QStringLiteral("list-item"),  QStringLiteral("table"),
            QStringLiteral("table-row"),    QStringLiteral("table-cell"),
            QStringLiteral("table-row-group"), QStringLiteral("table-header-group"),
            QStringLiteral("table-footer-group"), QStringLiteral("table-caption"),
            QStringLiteral("table-column"), QStringLiteral("table-column-group"),
            QStringLiteral("flex"),
        };
        if (value.isKeyword(QStringLiteral("inline-flex")))
            style->display = QStringLiteral("flex");
        else if (kKnown.contains(value.keyword))
            style->display = value.keyword;
        else if (consumed)
            *consumed = false;
        style->isListItem = style->display == QLatin1String("list-item");
        return;
    }

    // --------------------------------------------------------------- flexbox
    //
    // The flex properties are validated here rather than at layout time, so an
    // unsupported keyword leaves the initial value in place and shows up in the
    // inspector instead of being silently treated as something else.

    if (property == QLatin1String("flex-direction")) {
        static const QSet<QString> kValid = {
            QStringLiteral("row"), QStringLiteral("row-reverse"), QStringLiteral("column"),
            QStringLiteral("column-reverse")};
        const QString keyword = keywordOr(style->flexDirection);
        if (!kValid.contains(keyword)) {
            if (consumed)
                *consumed = false;
            return;
        }
        style->flexDirection = keyword;
        return;
    }

    if (property == QLatin1String("flex-wrap")) {
        static const QSet<QString> kValid = {QStringLiteral("nowrap"), QStringLiteral("wrap"),
                                             QStringLiteral("wrap-reverse")};
        const QString keyword = keywordOr(style->flexWrap);
        if (!kValid.contains(keyword)) {
            if (consumed)
                *consumed = false;
            return;
        }
        style->flexWrap = keyword;
        return;
    }

    if (property == QLatin1String("justify-content")) {
        static const QSet<QString> kValid = {
            QStringLiteral("flex-start"), QStringLiteral("flex-end"), QStringLiteral("center"),
            QStringLiteral("space-between"), QStringLiteral("space-around"),
            QStringLiteral("space-evenly"), QStringLiteral("start"), QStringLiteral("end")};
        QString keyword = keywordOr(style->justifyContent);

        // "start" and "end" are the modern spelling of flex-start and flex-end.
        // They are normalised so layout only has one form to handle.
        if (keyword == QLatin1String("start"))
            keyword = QStringLiteral("flex-start");
        else if (keyword == QLatin1String("end"))
            keyword = QStringLiteral("flex-end");

        if (!kValid.contains(keyword)) {
            if (consumed)
                *consumed = false;
            return;
        }
        style->justifyContent = keyword;
        return;
    }

    if (property == QLatin1String("align-items")) {
        static const QSet<QString> kValid = {
            QStringLiteral("stretch"), QStringLiteral("flex-start"), QStringLiteral("flex-end"),
            QStringLiteral("center"), QStringLiteral("baseline"), QStringLiteral("start"),
            QStringLiteral("end")};
        QString keyword = keywordOr(style->alignItems);
        if (keyword == QLatin1String("start"))
            keyword = QStringLiteral("flex-start");
        else if (keyword == QLatin1String("end"))
            keyword = QStringLiteral("flex-end");

        if (!kValid.contains(keyword)) {
            if (consumed)
                *consumed = false;
            return;
        }
        style->alignItems = keyword;
        return;
    }

    if (property == QLatin1String("align-content")) {
        static const QSet<QString> kValid = {
            QStringLiteral("stretch"), QStringLiteral("flex-start"), QStringLiteral("flex-end"),
            QStringLiteral("center"), QStringLiteral("space-between"),
            QStringLiteral("space-around")};
        const QString keyword = keywordOr(style->alignContent);
        if (!kValid.contains(keyword)) {
            if (consumed)
                *consumed = false;
            return;
        }
        style->alignContent = keyword;
        return;
    }

    if (property == QLatin1String("align-self")) {
        static const QSet<QString> kValid = {
            QStringLiteral("auto"), QStringLiteral("stretch"), QStringLiteral("flex-start"),
            QStringLiteral("flex-end"), QStringLiteral("center"), QStringLiteral("baseline")};
        const QString keyword = keywordOr(style->alignSelf);
        if (!kValid.contains(keyword)) {
            if (consumed)
                *consumed = false;
            return;
        }
        style->alignSelf = keyword;
        return;
    }

    if (property == QLatin1String("order")) {
        if (value.kind != Value::Kind::Number) {
            if (consumed)
                *consumed = false;
            return;
        }
        style->order = static_cast<int>(value.number);
        return;
    }

    if (property == QLatin1String("flex-grow") || property == QLatin1String("flex-shrink")) {
        if (value.kind != Value::Kind::Number) {
            if (consumed)
                *consumed = false;
            return;
        }
        // A negative factor is invalid; the declaration is dropped rather than
        // clamped, which is what the specification asks for.
        if (value.number < 0) {
            if (consumed)
                *consumed = false;
            return;
        }
        if (property == QLatin1String("flex-grow"))
            style->flexGrow = value.number;
        else
            style->flexShrink = value.number;
        return;
    }

    if (property == QLatin1String("flex-basis")) {
        if (value.isKeyword(QStringLiteral("auto"))) {
            style->flexBasis = LengthOrAuto();
            style->hasFlexBasis = false;
            return;
        }
        if (value.isKeyword(QStringLiteral("content"))) {
            // "content" sizes from the item's content, which for this engine is
            // the same as auto sizing.
            style->flexBasis = LengthOrAuto();
            style->hasFlexBasis = false;
            return;
        }
        const LengthOrAuto basis
            = resolveLength(value, style->fontSize, m_context.rootFontSize,
                            m_context.viewportWidth, m_context.viewportHeight, true);

        // resolveLength leaves auto in place for an "auto" keyword and for a
        // value it cannot use, so an auto result here means the declaration was
        // not a length at all.
        if (basis.isAuto()) {
            if (consumed)
                *consumed = false;
            return;
        }

        style->flexBasis = basis;
        style->hasFlexBasis = true;
        return;
    }

    if (property == QLatin1String("flex")) {
        // The shorthand's three parts, in any of the forms CSS allows:
        //   flex: 1            -> 1 1 0%
        //   flex: 1 2          -> 1 2 0%
        //   flex: 1 2 30px     -> the three parts
        //   flex: auto         -> 1 1 auto
        //   flex: none         -> 0 0 auto
        const QString raw = value.toString().trimmed();

        if (raw == QLatin1String("none")) {
            style->flexGrow = 0;
            style->flexShrink = 0;
            style->flexBasis = LengthOrAuto();
            style->hasFlexBasis = false;
            return;
        }

        if (raw == QLatin1String("auto")) {
            style->flexGrow = 1;
            style->flexShrink = 1;
            style->flexBasis = LengthOrAuto();
            style->hasFlexBasis = false;
            return;
        }

        const QList<QString> parts = raw.split(QRegularExpression(QStringLiteral("\\s+")),
                                              Qt::SkipEmptyParts);
        if (parts.isEmpty() || parts.size() > 3) {
            if (consumed)
                *consumed = false;
            return;
        }

        double grow = 1;
        double shrink = 1;
        bool sawGrow = false;
        bool sawShrink = false;
        bool sawBasis = false;
        LengthOrAuto basis;

        for (const QString &part : parts) {
            const Value item = parseSingleValue(part);

            if (item.isKeyword(QStringLiteral("auto")) || item.isKeyword(QStringLiteral("content"))
                || item.isKeyword(QStringLiteral("min-content"))
                || item.isKeyword(QStringLiteral("max-content"))
                || item.isKeyword(QStringLiteral("fit-content"))) {
                // Any of the intrinsic keywords as the basis means "size from
                // content", which is this engine's auto.
                basis = LengthOrAuto();
                sawBasis = true;
                continue;
            }

            if (item.kind == Value::Kind::Number) {
                if (item.number < 0) {
                    if (consumed)
                        *consumed = false;
                    return;
                }
                // The first number is grow, the second shrink.
                if (!sawGrow) {
                    grow = item.number;
                    sawGrow = true;
                } else if (!sawShrink) {
                    shrink = item.number;
                    sawShrink = true;
                }
                continue;
            }

            // Anything else is a length, which is the basis.
            basis = resolveLength(item, style->fontSize, m_context.rootFontSize,
                                  m_context.viewportWidth, m_context.viewportHeight, true);
            if (basis.isAuto()) {
                if (consumed)
                    *consumed = false;
                return;
            }
            sawBasis = true;
        }

        style->flexGrow = grow;
        style->flexShrink = shrink;
        if (sawBasis) {
            style->flexBasis = basis;
            style->hasFlexBasis = true;
        } else {
            // A bare `flex: 1` gives a zero basis, which is what makes the item
            // share the free space rather than start from its own content size.
            style->flexBasis = LengthOrAuto::pixels(0);
            style->hasFlexBasis = true;
        }
        return;
    }

    if (property == QLatin1String("gap") || property == QLatin1String("grid-gap")) {
        // `gap` sets both axes; a second value sets the column gap separately.
        const QList<QString> parts = splitBoxValues(value.toString());
        const Value row = parseSingleValue(parts.value(0));
        const Value column = parts.size() > 1 ? parseSingleValue(parts.value(1)) : row;

        const auto resolve = [&](const Value &item) {
            return resolveLength(item, style->fontSize, m_context.rootFontSize,
                                 m_context.viewportWidth, m_context.viewportHeight, true);
        };
        style->rowGap = resolve(row);
        style->columnGap = resolve(column);
        return;
    }

    if (property == QLatin1String("row-gap") || property == QLatin1String("grid-row-gap")) {
        style->rowGap = resolveLength(value, style->fontSize, m_context.rootFontSize,
                                      m_context.viewportWidth, m_context.viewportHeight, true);
        return;
    }

    if (property == QLatin1String("column-gap") || property == QLatin1String("grid-column-gap")) {
        style->columnGap = resolveLength(value, style->fontSize, m_context.rootFontSize,
                                         m_context.viewportWidth, m_context.viewportHeight, true);
        return;
    }

    if (property == QLatin1String("position")) {
        const QString keyword = keywordOr(style->position);
        static const QSet<QString> kValid = {QStringLiteral("static"),
                                             QStringLiteral("relative"),
                                             QStringLiteral("absolute"),
                                             QStringLiteral("fixed"),
                                             QStringLiteral("sticky")};
        if (kValid.contains(keyword))
            style->position = keyword;
        else if (consumed)
            *consumed = false;
        return;
    }

    if (property == QLatin1String("z-index")) {
        // "auto" is the initial value and means "do not create a stacking
        // context". An integer sets the level.
        if (value.isKeyword(QStringLiteral("auto"))) {
            style->zIndex = 0;
            style->hasZIndex = false;
            return;
        }
        if (value.kind != Value::Kind::Number) {
            if (consumed)
                *consumed = false;
            return;
        }
        style->zIndex = static_cast<int>(value.number);
        style->hasZIndex = true;
        return;
    }

    if (property == QLatin1String("visibility")) {
        const QString keyword = keywordOr(style->visibility);
        if (keyword == QLatin1String("hidden") || keyword == QLatin1String("collapse"))
            style->visibility = keyword;
        else
            style->visibility = QStringLiteral("visible");
        style->visible = style->visibility != QLatin1String("hidden")
            && style->visibility != QLatin1String("collapse");
        return;
    }

    // ---------------------------------------------------- sizing and box
    if (property == QLatin1String("width")) {
        style->width = length(true);
        return;
    }
    if (property == QLatin1String("height")) {
        style->height = length(true);
        return;
    }
    if (property == QLatin1String("min-width")) {
        style->minWidth = length(false, true, false);
        return;
    }
    if (property == QLatin1String("max-width")) {
        style->maxWidth = length(true, true, false);
        return;
    }
    if (property == QLatin1String("min-height")) {
        style->minHeight = length(false, true, false);
        return;
    }
    if (property == QLatin1String("max-height")) {
        style->maxHeight = length(true, true, false);
        return;
    }

    if (property == QLatin1String("margin-top")) {
        style->marginTop = length(true);
        return;
    }
    if (property == QLatin1String("margin-right")) {
        style->marginRight = length(true);
        return;
    }
    if (property == QLatin1String("margin-bottom")) {
        style->marginBottom = length(true);
        return;
    }
    if (property == QLatin1String("margin-left")) {
        style->marginLeft = length(true);
        return;
    }
    if (property == QLatin1String("margin")) {
        // The value is a raw list, so it is re-tokenised to expand the shorthand.
        const QList<QString> parts = splitBoxValues(value.toString());
        const Value top = parseSingleValue(parts.value(0));
        const Value right = parseSingleValue(parts.value(1));
        const Value bottom = parseSingleValue(parts.value(2));
        const Value left = parseSingleValue(parts.value(3));
        const auto resolve = [&](const Value &item) {
            return resolveLength(item, style->fontSize, m_context.rootFontSize,
                                  m_context.viewportWidth, m_context.viewportHeight, true);
        };
        style->marginTop = resolve(top);
        style->marginRight = resolve(right);
        style->marginBottom = resolve(bottom);
        style->marginLeft = resolve(left);
        return;
    }

    if (property == QLatin1String("padding-top")) {
        style->paddingTop = length(false, true, false);
        return;
    }
    if (property == QLatin1String("padding-right")) {
        style->paddingRight = length(false, true, false);
        return;
    }
    if (property == QLatin1String("padding-bottom")) {
        style->paddingBottom = length(false, true, false);
        return;
    }
    if (property == QLatin1String("padding-left")) {
        style->paddingLeft = length(false, true, false);
        return;
    }
    if (property == QLatin1String("padding")) {
        const QList<QString> parts = splitBoxValues(value.toString());
        const auto resolve = [&](const QString &text) {
            return resolveLength(parseSingleValue(text), style->fontSize, m_context.rootFontSize,
                                  m_context.viewportWidth, m_context.viewportHeight, false, true,
                                  false);
        };
        style->paddingTop = resolve(parts.value(0));
        style->paddingRight = resolve(parts.value(1));
        style->paddingBottom = resolve(parts.value(2));
        style->paddingLeft = resolve(parts.value(3));
        return;
    }

    if (property == QLatin1String("top")) {
        style->top = length(true);
        return;
    }
    if (property == QLatin1String("right")) {
        style->right = length(true);
        return;
    }
    if (property == QLatin1String("bottom")) {
        style->bottom = length(true);
        return;
    }
    if (property == QLatin1String("left")) {
        style->left = length(true);
        return;
    }

    if (property == QLatin1String("box-sizing")) {
        const QString keyword = keywordOr(QStringLiteral("content-box"));
        style->boxSizing = keyword == QLatin1String("border-box") ? keyword
                                                                 : QStringLiteral("content-box");
        return;
    }

    // ---------------------------------------------------------- borders
    const auto setBorderStyle = [&](QString *slot) {
        const QString keyword = keywordOr(QStringLiteral("none"));
        static const QSet<QString> kStyles = {
            QStringLiteral("none"),   QStringLiteral("hidden"), QStringLiteral("dotted"),
            QStringLiteral("dashed"), QStringLiteral("solid"),  QStringLiteral("double"),
            QStringLiteral("groove"), QStringLiteral("ridge"),  QStringLiteral("inset"),
            QStringLiteral("outset"),
        };
        *slot = kStyles.contains(keyword) ? keyword : QStringLiteral("none");
    };
    const auto setBorderWidth = [&](double *slot, const QString &styleSlot) {
        if (styleSlot == QLatin1String("none") || styleSlot == QLatin1String("hidden")) {
            *slot = 0;
            return;
        }
        if (value.isKeyword(QStringLiteral("thin"))) {
            *slot = 1;
            return;
        }
        if (value.isKeyword(QStringLiteral("medium"))) {
            *slot = 3;
            return;
        }
        if (value.isKeyword(QStringLiteral("thick"))) {
            *slot = 5;
            return;
        }
        const LengthOrAuto resolved = length(false, false, false);
        *slot = resolved.kind == LengthOrAuto::Kind::Auto ? 0 : resolved.value;
    };
    const auto setBorderColor = [&](QColor *slot, const QColor &fallback) {
        *slot = colorOf(fallback);
    };

    if (property == QLatin1String("border-top-style")) {
        setBorderStyle(&style->borderTopStyle);
        return;
    }
    if (property == QLatin1String("border-right-style")) {
        setBorderStyle(&style->borderRightStyle);
        return;
    }
    if (property == QLatin1String("border-bottom-style")) {
        setBorderStyle(&style->borderBottomStyle);
        return;
    }
    if (property == QLatin1String("border-left-style")) {
        setBorderStyle(&style->borderLeftStyle);
        return;
    }
    if (property == QLatin1String("border-top-width")) {
        setBorderWidth(&style->borderTopWidth, style->borderTopStyle);
        return;
    }
    if (property == QLatin1String("border-right-width")) {
        setBorderWidth(&style->borderRightWidth, style->borderRightStyle);
        return;
    }
    if (property == QLatin1String("border-bottom-width")) {
        setBorderWidth(&style->borderBottomWidth, style->borderBottomStyle);
        return;
    }
    if (property == QLatin1String("border-left-width")) {
        setBorderWidth(&style->borderLeftWidth, style->borderLeftStyle);
        return;
    }
    if (property == QLatin1String("border-top-color")) {
        setBorderColor(&style->borderTopColor, style->color);
        return;
    }
    if (property == QLatin1String("border-right-color")) {
        setBorderColor(&style->borderRightColor, style->color);
        return;
    }
    if (property == QLatin1String("border-bottom-color")) {
        setBorderColor(&style->borderBottomColor, style->color);
        return;
    }
    if (property == QLatin1String("border-left-color")) {
        setBorderColor(&style->borderLeftColor, style->color);
        return;
    }

    // The border longhands and the "border" shorthand carry up to three
    // components in any order; the text is scanned for a style keyword, a width
    // and a colour, which is how every engine parses this shorthand.
    if (property == QLatin1String("border") || property.startsWith(QLatin1String("border-"))
        || property == QLatin1String("border-width") || property == QLatin1String("border-style")
        || property == QLatin1String("border-color")) {
        // Aggregate longhands first: "border-width: 1px 2px" sets the four sides.
        if (property == QLatin1String("border-style")) {
            const QList<QString> parts = splitBoxValues(value.toString());
            const auto setOne = [&](QString *slot, const QString &text) {
                const Value item = parseSingleValue(text);
                static const QSet<QString> kStyles = {
                    QStringLiteral("none"),   QStringLiteral("hidden"), QStringLiteral("dotted"),
                    QStringLiteral("dashed"), QStringLiteral("solid"),  QStringLiteral("double"),
                    QStringLiteral("groove"), QStringLiteral("ridge"),  QStringLiteral("inset"),
                    QStringLiteral("outset"),
                };
                const QString keyword = item.isValid() ? item.keyword.toLower() : QString();
                if (kStyles.contains(keyword))
                    *slot = keyword;
            };
            setOne(&style->borderTopStyle, parts.value(0));
            setOne(&style->borderRightStyle, parts.value(1));
            setOne(&style->borderBottomStyle, parts.value(2));
            setOne(&style->borderLeftStyle, parts.value(3));
            return;
        }

        if (property == QLatin1String("border-width")) {
            const QList<QString> parts = splitBoxValues(value.toString());
            const auto setOne = [&](double *slot, const QString &text, const QString &styleSlot) {
                if (styleSlot == QLatin1String("none") || styleSlot == QLatin1String("hidden")) {
                    *slot = 0;
                    return;
                }
                const Value item = parseSingleValue(text);
                if (item.isKeyword(QStringLiteral("thin"))) {
                    *slot = 1;
                } else if (item.isKeyword(QStringLiteral("medium"))) {
                    *slot = 3;
                } else if (item.isKeyword(QStringLiteral("thick"))) {
                    *slot = 5;
                } else {
                    const LengthOrAuto resolved = resolveLength(item, style->fontSize,
                                                               m_context.rootFontSize,
                                                               m_context.viewportWidth,
                                                               m_context.viewportHeight, false,
                                                               false, false);
                    *slot = resolved.kind == LengthOrAuto::Kind::Auto ? 0 : resolved.value;
                }
            };
            setOne(&style->borderTopWidth, parts.value(0), style->borderTopStyle);
            setOne(&style->borderRightWidth, parts.value(1), style->borderRightStyle);
            setOne(&style->borderBottomWidth, parts.value(2), style->borderBottomStyle);
            setOne(&style->borderLeftWidth, parts.value(3), style->borderLeftStyle);
            return;
        }

        if (property == QLatin1String("border-color")) {
            const QList<QString> parts = splitBoxValues(value.toString());
            const auto setOne = [&](QColor *slot, const QString &text, const QColor &fallback) {
                const Value item = parseSingleValue(text);
                if (item.isKeyword(QStringLiteral("currentcolor")))
                    *slot = style->color;
                else if (item.isColor())
                    *slot = item.color;
                else
                    *slot = fallback;
            };
            setOne(&style->borderTopColor, parts.value(0), style->borderTopColor);
            setOne(&style->borderRightColor, parts.value(1), style->borderRightColor);
            setOne(&style->borderBottomColor, parts.value(2), style->borderBottomColor);
            setOne(&style->borderLeftColor, parts.value(3), style->borderLeftColor);
            return;
        }

        if (property == QLatin1String("border-top") || property == QLatin1String("border-right")
            || property == QLatin1String("border-bottom")
            || property == QLatin1String("border-left") || property == QLatin1String("border")) {
            const QStringList parts = value.toString().split(
                QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            QString styleKeyword;
            double widthPixels = -1;
            QColor borderColor;
            bool haveColor = false;

            for (const QString &part : parts) {
                static const QSet<QString> kStyles = {
                    QStringLiteral("none"),   QStringLiteral("hidden"), QStringLiteral("dotted"),
                    QStringLiteral("dashed"), QStringLiteral("solid"),  QStringLiteral("double"),
                    QStringLiteral("groove"), QStringLiteral("ridge"),  QStringLiteral("inset"),
                    QStringLiteral("outset"),
                };
                const QString lowered = part.toLower();
                if (kStyles.contains(lowered)) {
                    styleKeyword = lowered;
                    continue;
                }
                if (lowered == QLatin1String("thin")) {
                    widthPixels = 1;
                    continue;
                }
                if (lowered == QLatin1String("medium")) {
                    widthPixels = 3;
                    continue;
                }
                if (lowered == QLatin1String("thick")) {
                    widthPixels = 5;
                    continue;
                }
                if (const Value color = values::parseColor(part); color.isValid()) {
                    borderColor = color.color;
                    haveColor = true;
                    continue;
                }
                const Value lengthValue = parseSingleValue(part);
                const LengthOrAuto resolved = resolveLength(
                    lengthValue, style->fontSize, m_context.rootFontSize, m_context.viewportWidth,
                    m_context.viewportHeight, false, false, false);
                if (resolved.kind == LengthOrAuto::Kind::Length)
                    widthPixels = resolved.value;
            }

            const auto assign = [&](QString *styleSlot, double *widthSlot, QColor *colorSlot) {
                if (!styleKeyword.isEmpty())
                    *styleSlot = styleKeyword;
                if (widthPixels >= 0 && *styleSlot != QLatin1String("none")
                    && *styleSlot != QLatin1String("hidden")) {
                    *widthSlot = widthPixels;
                }
                if (haveColor)
                    *colorSlot = borderColor;
            };

            if (property == QLatin1String("border")) {
                assign(&style->borderTopStyle, &style->borderTopWidth, &style->borderTopColor);
                assign(&style->borderRightStyle, &style->borderRightWidth,
                       &style->borderRightColor);
                assign(&style->borderBottomStyle, &style->borderBottomWidth,
                       &style->borderBottomColor);
                assign(&style->borderLeftStyle, &style->borderLeftWidth, &style->borderLeftColor);
            } else if (property == QLatin1String("border-top")) {
                assign(&style->borderTopStyle, &style->borderTopWidth, &style->borderTopColor);
            } else if (property == QLatin1String("border-right")) {
                assign(&style->borderRightStyle, &style->borderRightWidth,
                       &style->borderRightColor);
            } else if (property == QLatin1String("border-bottom")) {
                assign(&style->borderBottomStyle, &style->borderBottomWidth,
                       &style->borderBottomColor);
            } else if (property == QLatin1String("border-left")) {
                assign(&style->borderLeftStyle, &style->borderLeftWidth, &style->borderLeftColor);
            }
            return;
        }
        // The remaining border-* aggregate longhands are left to fail: without
        // matching, the property keeps whatever an earlier declaration set.
        if (consumed)
            *consumed = false;
        return;
    }

    if (property == QLatin1String("border-radius")) {
        style->borderRadius = value.toString();
        return;
    }

    // --------------------------------------------------------------- flow
    if (property == QLatin1String("float")) {
        const QString keyword = keywordOr(QStringLiteral("none"));
        style->floatSide = (keyword == QLatin1String("left") || keyword == QLatin1String("right")
                            || keyword == QLatin1String("inline-start")
                            || keyword == QLatin1String("inline-end"))
            ? keyword
            : QStringLiteral("none");
        if (style->floatSide == QLatin1String("inline-start"))
            style->floatSide = QStringLiteral("left");
        if (style->floatSide == QLatin1String("inline-end"))
            style->floatSide = QStringLiteral("right");
        return;
    }
    if (property == QLatin1String("clear")) {
        const QString keyword = keywordOr(QStringLiteral("none"));
        static const QSet<QString> kValid = {QStringLiteral("none"), QStringLiteral("left"),
                                             QStringLiteral("right"), QStringLiteral("both")};
        style->clear = kValid.contains(keyword) ? keyword : QStringLiteral("none");
        return;
    }
    if (property == QLatin1String("overflow") || property == QLatin1String("overflow-x")
        || property == QLatin1String("overflow-y")) {
        const QString keyword = keywordOr(QStringLiteral("visible"));
        static const QSet<QString> kValid = {QStringLiteral("visible"), QStringLiteral("hidden"),
                                             QStringLiteral("scroll"), QStringLiteral("auto"),
                                             QStringLiteral("clip")};
        if (property == QLatin1String("overflow"))
            style->overflow = kValid.contains(keyword) ? keyword : QStringLiteral("visible");
        return;
    }
    if (property == QLatin1String("opacity")) {
        if (value.kind == Value::Kind::Number || value.kind == Value::Kind::Percentage) {
            const double amount
                = value.kind == Value::Kind::Percentage ? value.number / 100.0 : value.number;
            style->opacity = qBound(0.0, amount, 1.0);
        } else if (consumed) {
            *consumed = false;
        }
        return;
    }

    // -------------------------------------------------------- typography
    if (property == QLatin1String("font-size")) {
        // The inherited size is already in place, so a relative font size is a
        // multiple of exactly that value. This is what makes nested "em" sizes
        // compound the way CSS requires.
        const double inherited = inheritedFontSize;

        if (value.kind == Value::Kind::Percentage) {
            style->fontSize = qMax(1.0, inherited * value.number / 100.0);
            return;
        }
        if (value.kind == Value::Kind::Length) {
            const QString unit = value.unit.toLower();
            if (unit == QLatin1String("em")) {
                style->fontSize = qMax(1.0, inherited * value.number);
                return;
            }
            double pixels = 0;
            if (values::lengthToPixels(value.number, unit, inherited, m_context.rootFontSize,
                                       m_context.viewportWidth, m_context.viewportHeight,
                                       &pixels)) {
                style->fontSize = qMax(1.0, pixels);
                return;
            }
            if (consumed)
                *consumed = false;
            return;
        }
        if (value.kind == Value::Kind::Keyword) {
            double pixels = 0;
            if (values::absoluteFontSizeKeyword(value.keyword, &pixels)) {
                style->fontSize = pixels;
                return;
            }
            bool recognised = false;
            const double relative
                = values::relativeFontSizeKeyword(value.keyword, inherited, &recognised);
            if (recognised) {
                style->fontSize = relative;
                return;
            }
        }
        if (consumed)
            *consumed = false;
        return;
    }

    if (property == QLatin1String("font-weight")) {
        const QString text = value.toString().trimmed().toLower();
        if (text == QLatin1String("normal"))
            style->fontWeight = 400;
        else if (text == QLatin1String("bold"))
            style->fontWeight = 700;
        else if (text == QLatin1String("bolder"))
            style->fontWeight = style->fontWeight >= 700 ? 900 : style->fontWeight + 300;
        else if (text == QLatin1String("lighter"))
            style->fontWeight = style->fontWeight <= 100 ? 100 : style->fontWeight - 300;
        else {
            bool ok = false;
            const int numeric = text.toInt(&ok);
            if (ok && numeric >= 1 && numeric <= 1000)
                style->fontWeight = numeric;
            else if (consumed)
                *consumed = false;
        }
        return;
    }

    if (property == QLatin1String("font-style")) {
        const QString keyword = keywordOr(QStringLiteral("normal"));
        if (keyword == QLatin1String("italic") || keyword == QLatin1String("oblique"))
            style->italic = true;
        else
            style->italic = false;
        return;
    }

    if (property == QLatin1String("font-family")) {
        // The family list keeps the author's order so the painter can try each
        // name in turn, falling back to the general family at the end.
        QStringList families;
        const QString raw = value.toString();
        for (const QString &part : raw.split(u',')) {
            const QString cleaned = values::unquote(part).trimmed();
            if (!cleaned.isEmpty())
                families.append(cleaned);
        }
        if (!families.isEmpty())
            style->fontFamilies = families;
        return;
    }

    if (property == QLatin1String("line-height")) {
        if (value.kind == Value::Kind::Number && value.number > 0) {
            // A unitless line-height is a multiplier of the element's own size,
            // and it inherits as the multiplier rather than the result.
            style->lineHeight = value.number * style->fontSize;
            return;
        }
        if (value.kind == Value::Kind::Percentage) {
            style->lineHeight = style->fontSize * value.number / 100.0;
            return;
        }
        if (value.kind == Value::Kind::Length) {
            style->lineHeight = value.number;
            return;
        }
        if (value.isKeyword(QStringLiteral("normal"))) {
            style->lineHeight = style->fontSize * 1.2;
            return;
        }
        if (consumed)
            *consumed = false;
        return;
    }

    if (property == QLatin1String("text-align")) {
        style->textAlign = normalizeAlign(value.toString().trimmed().toLower());
        return;
    }

    if (property == QLatin1String("text-decoration") || property == QLatin1String("text-decoration-line")) {
        const QString text = value.toString().trimmed().toLower();
        if (text.contains(QLatin1String("none")))
            style->textDecoration = QStringLiteral("none");
        else
            style->textDecoration = text;
        return;
    }

    if (property == QLatin1String("text-transform")) {
        const QString keyword = keywordOr(QStringLiteral("none"));
        static const QSet<QString> kValid = {QStringLiteral("none"), QStringLiteral("uppercase"),
                                             QStringLiteral("lowercase"),
                                             QStringLiteral("capitalize")};
        style->textTransform = kValid.contains(keyword) ? keyword : QStringLiteral("none");
        return;
    }

    if (property == QLatin1String("white-space")) {
        const QString raw = value.toString().trimmed().toLower();
        if (raw == QLatin1String("nowrap"))
            style->whiteSpace = QStringLiteral("nowrap");
        else if (raw == QLatin1String("pre"))
            style->whiteSpace = QStringLiteral("pre");
        else if (raw == QLatin1String("pre-wrap"))
            style->whiteSpace = QStringLiteral("pre-wrap");
        else if (raw == QLatin1String("pre-line"))
            style->whiteSpace = QStringLiteral("pre-line");
        else
            style->whiteSpace = QStringLiteral("normal");
        return;
    }

    if (property == QLatin1String("letter-spacing")) {
        style->letterSpacing = value.isKeyword(QStringLiteral("normal")) ? 0 : length(false).value;
        return;
    }
    if (property == QLatin1String("word-spacing")) {
        style->wordSpacing = value.isKeyword(QStringLiteral("normal")) ? 0 : length(false).value;
        return;
    }
    if (property == QLatin1String("text-overflow")) {
        style->textOverflow = value.toString().trimmed().toLower();
        return;
    }

    // ------------------------------------------------------------- colour
    if (property == QLatin1String("color")) {
        if (value.isColor())
            style->color = value.color;
        else if (isCurrentColor)
            style->color = style->color;
        else if (value.isKeyword(QStringLiteral("inherit")) || consumed)
            *consumed = value.isKeyword(QStringLiteral("inherit"));
        return;
    }
    if (property == QLatin1String("background-color")) {
        if (value.isColor())
            style->backgroundColor = value.color;
        else if (isCurrentColor)
            style->backgroundColor = style->color;
        else if (value.isKeyword(QStringLiteral("transparent")))
            style->backgroundColor = QColor(0, 0, 0, 0);
        else if (consumed)
            *consumed = false;
        return;
    }
    if (property == QLatin1String("background-image")) {
        if (value.isUrl())
            style->backgroundImage = value.keyword;
        else if (value.isKeyword(QStringLiteral("none")))
            style->backgroundImage.clear();
        else if (consumed)
            *consumed = false;
        return;
    }
    if (property == QLatin1String("background-repeat")) {
        style->backgroundRepeat = value.toString().trimmed().toLower();
        return;
    }
    if (property == QLatin1String("background-size")) {
        style->backgroundSize = value.toString().trimmed();
        return;
    }
    if (property == QLatin1String("background-position")) {
        style->backgroundPosition = value.toString().trimmed();
        return;
    }
    if (property == QLatin1String("background")) {
        // The shorthand accepts a colour, a url, a repeat and a position in any
        // order; each component is recognised by shape.
        const QString raw = value.toString();
        style->backgroundColor = QColor(0, 0, 0, 0);
        style->backgroundImage.clear();

        if (const Value color = values::parseColor(raw.split(QRegularExpression(
                                                      QStringLiteral("\\s+")))
                                                  .value(0));
            color.isValid()) {
            style->backgroundColor = color.color;
        }

        static const QRegularExpression urlPattern(QStringLiteral("url\\(\\s*['\"]?([^'\")]+)"));
        const auto urlMatch = urlPattern.match(raw);
        if (urlMatch.hasMatch())
            style->backgroundImage = urlMatch.captured(1);

        if (raw.contains(QLatin1String("no-repeat")))
            style->backgroundRepeat = QStringLiteral("no-repeat");
        else if (raw.contains(QLatin1String("repeat-x")))
            style->backgroundRepeat = QStringLiteral("repeat-x");
        else if (raw.contains(QLatin1String("repeat-y")))
            style->backgroundRepeat = QStringLiteral("repeat-y");
        return;
    }

    // ------------------------------------------------------ presentation
    if (property == QLatin1String("list-style-type") || property == QLatin1String("list-style")) {
        static const QSet<QString> kTypes = {
            QStringLiteral("disc"),    QStringLiteral("circle"),   QStringLiteral("square"),
            QStringLiteral("decimal"), QStringLiteral("lower-alpha"), QStringLiteral("upper-alpha"),
            QStringLiteral("lower-roman"), QStringLiteral("upper-roman"), QStringLiteral("none"),
            QStringLiteral("lower-latin"), QStringLiteral("upper-latin"),
        };
        const QStringList parts = value.toString().toLower().split(
            QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        for (const QString &part : parts) {
            if (kTypes.contains(part)) {
                style->listStyleType = part;
                break;
            }
        }
        if (parts.contains(QStringLiteral("inside")))
            style->listStylePosition = QStringLiteral("inside");
        else if (parts.contains(QStringLiteral("outside")))
            style->listStylePosition = QStringLiteral("outside");
        return;
    }

    if (property == QLatin1String("vertical-align")) {
        style->verticalAlign = value.toString().trimmed().toLower();
        return;
    }

    if (property == QLatin1String("cursor")) {
        style->cursor = value.toString().trimmed().toLower();
        return;
    }

    if (property == QLatin1String("border-collapse")) {
        style->borderCollapse = keywordOr(QStringLiteral("separate"));
        return;
    }

    if (property == QLatin1String("direction")) {
        style->direction = keywordOr(QStringLiteral("ltr"));
        return;
    }

    // Properties this engine parses but has no use for yet, such as
    // "box-shadow", "transform" or "transition", are recorded but marked as not
    // consumed so the inspector can show them as unsupported.
    if (consumed)
        *consumed = false;
}

} // namespace oqb::css
