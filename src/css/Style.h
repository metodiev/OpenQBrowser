#pragma once

#include <QColor>
#include <QHash>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

#include "css/Grid.h"
#include "css/Stylesheet.h"
#include "css/Value.h"

#include <deque>
#include <functional>

namespace oqb::dom {
class Document;
class Element;
} // namespace oqb::dom

namespace oqb::css {

/// A length that may still be pending: "width: 50%" and "width: 4em" cannot be
/// reduced to a pixel count until layout runs and the containing block exists.
struct LengthOrAuto
{
    enum class Kind { Auto, Length, Percentage };

    Kind kind = Kind::Auto;
    double value = 0;   ///< Pixels for Length, the percentage amount otherwise.
    QString original;   ///< As the author wrote it, for the inspector.

    bool isAuto() const { return kind == Kind::Auto; }
    bool isLength() const { return kind == Kind::Length; }
    bool isPercentage() const { return kind == Kind::Percentage; }

    /// Resolves against `base`.
    ///
    /// "auto" resolves to zero: every caller that treats auto specially checks
    /// for it first (a width of auto fills the containing block, a margin of
    /// auto absorbs leftover space), so a stray auto here would silently
    /// produce a huge length. Padding and border widths, which can never be
    /// auto, therefore default to zero as they should.
    double resolve(double base) const
    {
        switch (kind) {
        case Kind::Length: return value;
        case Kind::Percentage: return base * value / 100.0;
        case Kind::Auto: return 0;
        }
        return 0;
    }

    static LengthOrAuto autoValue() { return {}; }
    static LengthOrAuto pixels(double px, const QString &original = {})
    {
        LengthOrAuto length;
        length.kind = Kind::Length;
        length.value = px;
        length.original = original.isEmpty() ? QString::number(px) + QStringLiteral("px") : original;
        return length;
    }
    static LengthOrAuto percent(double amount, const QString &original = {})
    {
        LengthOrAuto length;
        length.kind = Kind::Percentage;
        length.value = amount;
        length.original = original.isEmpty() ? QString::number(amount) + u'%' : original;
        return length;
    }
};

/// One side of a box: used for padding and as the unresolved half of margins.
struct EdgeSizes
{
    double top = 0;
    double right = 0;
    double bottom = 0;
    double left = 0;

    double horizontal() const { return left + right; }
    double vertical() const { return top + bottom; }

    void setAll(double value)
    {
        top = right = bottom = left = value;
    }
};

/// The computed style of one element.
///
/// The fields are explicit rather than a generic property map: the layout engine
/// reads them directly, the inspector prints them without a lookup table, and a
/// typo in a property name becomes a compile error instead of a silent miss.
struct ComputedStyle
{
    // ---------------------------------------------------- box generation
    QString display = QStringLiteral("inline");
    QString position = QStringLiteral("static");
    bool visible = true; ///< From the visibility property.
    bool isListItem = false;

    // ------------------------------------------------------------ flexbox
    //
    // A flex container lays its children out along a main axis. The properties
    // are stored as the resolved keywords the layout engine reads, so that the
    // engine has no keyword lookup of its own and the inspector prints what the
    // cascade produced.
    bool isFlexContainer() const { return display == QLatin1String("flex"); }

    /// True for a grid container. An inline-grid is laid out exactly like a grid
    /// but is inline-level, so the two differ only in how the box is placed in
    /// its parent - not in how its own children are arranged.
    bool isGridContainer() const
    {
        return display == QLatin1String("grid") || display == QLatin1String("inline-grid");
    }

    /// "row", "row-reverse", "column" or "column-reverse".
    QString flexDirection = QStringLiteral("row");
    /// "nowrap", "wrap" or "wrap-reverse".
    QString flexWrap = QStringLiteral("nowrap");
    /// "flex-start", "flex-end", "center", "space-between", "space-around" or
    /// "space-evenly".
    QString justifyContent = QStringLiteral("flex-start");
    /// "stretch", "flex-start", "flex-end", "center" or "baseline".
    QString alignItems = QStringLiteral("stretch");
    /// "flex-start", "flex-end", "center", "space-between", "space-around" or
    /// "stretch".
    QString alignContent = QStringLiteral("stretch");
    /// The container's row and column gaps. `gap` sets both; `row-gap` and
    /// `column-gap` override their own axis.
    LengthOrAuto rowGap;
    LengthOrAuto columnGap;

    // A flex item's own properties.
    /// The flex shorthand's three parts. `flexBasis` keeps auto until layout
    /// resolves it, and a definite basis wins over the item's width.
    LengthOrAuto flexBasis;
    double flexGrow = 0;
    double flexShrink = 1;
    /// "auto", "flex-start", "flex-end", "center", "baseline" or "stretch".
    QString alignSelf = QStringLiteral("auto");
    int order = 0;

    /// True when `flex-basis` was set explicitly, which decides whether the
    /// item's width or its basis is the starting size.
    bool hasFlexBasis = false;

    // ------------------------------------------------------ grid container
    //
    // The template lists are kept as text and parsed by `css::parseTrackList()`
    // at layout time rather than here, because a track size may be a percentage
    // or a viewport unit whose base is not known until the grid has a containing
    // block. Parsing them here would freeze the wrong answer.
    QString gridTemplateColumns;
    QString gridTemplateRows;
    /// The size of a track the template did not define, for items placed outside
    /// it (`grid-auto-rows` and `-columns`).
    QString gridAutoColumns;
    QString gridAutoRows;
    /// `grid-auto-flow`: "row", "column", or either plus "dense".
    QString gridAutoFlow = QStringLiteral("row");
    /// How items are aligned inside their cells. `justify-items` is the inline
    /// axis and `align-items` the block axis, which for a grid means columns and
    /// rows. `align-items`, `justify-content` and `align-content` are shared with
    /// flexbox and are declared with the flex properties above.
    QString justifyItems = QStringLiteral("stretch");

    // -------------------------------------------------------- grid item
    //
    // `grid-column` and `grid-row` are the shorthands for
    // `<start> / <end>`; `grid-area` sets all four at once. The raw text is kept
    // so the inspector prints what the author wrote.
    QString gridColumn;
    QString gridRow;
    QString gridArea;
    /// The resolved placement of this item, parsed from the three above.
    GridPlacement gridColumnStart;
    GridPlacement gridRowStart;
    /// `justify-self` for the inline axis. `align-self` is shared with flexbox.
    QString justifySelf = QStringLiteral("auto");

    /// Width and height keep a percentage or auto until layout resolves them.
    LengthOrAuto width;
    LengthOrAuto height;
    LengthOrAuto minWidth;
    LengthOrAuto maxWidth;
    LengthOrAuto minHeight;
    LengthOrAuto maxHeight;

    /// Margins may be "auto", which is how a block is centred horizontally.
    // The initial value of a margin is zero, not auto (CSS 2.2 §8.1). An
    // unspecified margin must therefore start as a length, because only an
    // explicit `margin: auto` may absorb free space.
    LengthOrAuto marginTop = LengthOrAuto::pixels(0, QStringLiteral("0"));
    LengthOrAuto marginRight = LengthOrAuto::pixels(0, QStringLiteral("0"));
    LengthOrAuto marginBottom = LengthOrAuto::pixels(0, QStringLiteral("0"));
    LengthOrAuto marginLeft = LengthOrAuto::pixels(0, QStringLiteral("0"));

    /// Percentage margins resolve against the containing block's width.
    LengthOrAuto paddingTop;
    LengthOrAuto paddingRight;
    LengthOrAuto paddingBottom;
    LengthOrAuto paddingLeft;

    // ------------------------------------------------------- positioning
    //
    // `top` and `left` are the modern logical spellings of what CSS 2.2 called
    // `top` and `left`; `right` and `bottom` are the other two edges. Which of
    // them are read depends on the position value, because a positioned box is
    // pinned by whichever offsets it was given.
    LengthOrAuto top;
    LengthOrAuto right;
    LengthOrAuto bottom;
    LengthOrAuto left;

    /// The stacking level. Only meaningful for a positioned box; `auto` is
    /// stored as 0 with `hasZIndex` false, because an auto level is treated as
    /// zero for ordering but does not create a stacking context.
    int zIndex = 0;
    bool hasZIndex = false;

    // ------------------------------------------------------- border
    QString borderTopStyle = QStringLiteral("none");
    QString borderRightStyle = QStringLiteral("none");
    QString borderBottomStyle = QStringLiteral("none");
    QString borderLeftStyle = QStringLiteral("none");
    double borderTopWidth = 0;
    double borderRightWidth = 0;
    double borderBottomWidth = 0;
    double borderLeftWidth = 0;
    QColor borderTopColor = Qt::black;
    QColor borderRightColor = Qt::black;
    QColor borderBottomColor = Qt::black;
    QColor borderLeftColor = Qt::black;
    QString borderRadius = QStringLiteral("0");

    QString boxSizing = QStringLiteral("content-box");

    // ---------------------------------------------------------- flow
    QString floatSide = QStringLiteral("none");
    QString clear = QStringLiteral("none");
    QString overflow = QStringLiteral("visible");
    QString textAlign = QStringLiteral("start");
    QString verticalAlign = QStringLiteral("baseline");

    // ---------------------------------------------------- typography
    QStringList fontFamilies;
    double fontSize = 16.0;
    int fontWeight = 400;
    bool italic = false;
    double lineHeight = 0; ///< Pixels; resolved from "normal" or a unitless value.
    QString textDecoration = QStringLiteral("none");
    QString textTransform = QStringLiteral("none");
    QString whiteSpace = QStringLiteral("normal");
    double letterSpacing = 0;
    double wordSpacing = 0;
    QString textOverflow = QStringLiteral("clip");

    // ------------------------------------------------------------ colour
    QColor color = QColor(0, 0, 0);
    QColor backgroundColor = QColor(0, 0, 0, 0);
    QString backgroundImage; ///< Empty when there is no image.
    QString backgroundRepeat = QStringLiteral("repeat");
    QString backgroundSize = QStringLiteral("auto");
    QString backgroundPosition = QStringLiteral("0% 0%");

    // ---------------------------------------------------- presentation
    QString listStyleType = QStringLiteral("disc");
    QString listStylePosition = QStringLiteral("outside");
    double opacity = 1.0;
    QString cursor = QStringLiteral("auto");

    // ------------------------------------------------------- inherited
    QString visibility = QStringLiteral("visible");
    QString borderCollapse = QStringLiteral("separate");
    QString captionSide = QStringLiteral("top");
    QString direction = QStringLiteral("ltr");

    /// True when the element produces a box that participates in layout.
    bool generatesBox() const { return display != QLatin1String("none"); }

    bool isBlockLevel() const
    {
        return display == QLatin1String("block") || display == QLatin1String("list-item")
            || display == QLatin1String("table") || display == QLatin1String("flex")
            || display == QLatin1String("grid") || display == QLatin1String("inline-grid")
            || display == QLatin1String("table-row") || display == QLatin1String("table-row-group")
            || display == QLatin1String("table-header-group")
            || display == QLatin1String("table-footer-group") || display == QLatin1String("table-caption");
    }

    bool isInlineLevel() const { return !isBlockLevel(); }
    bool isFloating() const { return floatSide != QLatin1String("none"); }
    bool isAbsolutelyPositioned() const
    {
        return position == QLatin1String("absolute") || position == QLatin1String("fixed");
    }

    /// True for any position other than static, which is what decides whether
    /// the offsets apply and whether the box establishes a containing block for
    /// its absolutely positioned descendants.
    bool isPositioned() const { return position != QLatin1String("static"); }

    /// The element's own text colour, used to resolve currentColor.
    QColor effectiveTextColor() const { return color; }

    /// A one-line summary for the inspector.
    QString describe() const;
};

/// The rendering environment the cascade needs to evaluate media queries and
/// viewport-relative units.
struct StyleContext
{
    double viewportWidth = 1024;
    double viewportHeight = 768;
    double rootFontSize = 16.0;
    bool prefersDarkScheme = false;

    /// The browser's default stylesheet (CSS 2.2 Appendix D, abbreviated).
    static const QString &userAgentStylesheet();
};

/// Computes the style of every element in a document.
///
/// The cascade follows CSS Cascade §6: declarations from the user agent sheet
/// and the author sheets are collected per element, then sorted by importance,
/// origin, specificity and document order. Inheritance is applied first so that
/// inherited properties keep the parent's value unless overridden, and relative
/// units are resolved against the values that are already final.
class StyleEngine
{
public:
    explicit StyleEngine(const StyleContext &context = {});

    /// Appends an author stylesheet. Call in document order.
    void addStylesheet(const Stylesheet &stylesheet);
    /// Removes every author stylesheet, keeping the user agent sheet.
    void clearStylesheets();

    /// Adds a page-level background and text colour, as the user's settings
    /// request. Applied at the lowest priority.
    void setBaseColors(const QColor &background, const QColor &text);

    /// Computes styles for the whole document.
    void computeStyles(dom::Document *document);

    const ComputedStyle &styleFor(const dom::Element *element) const;
    bool hasStyleFor(const dom::Element *element) const;

    /// One applied declaration, for the DevTools style panel.
    struct AppliedDeclaration
    {
        QString property;
        QString value;
        QString selector;
        bool important = false;
        bool fromInlineStyle = false;
        bool used = true; ///< False when a later declaration won.
    };

    /// The declarations that applied to `element`, in cascade order.
    QList<AppliedDeclaration> declarationsFor(const dom::Element *element) const;

    const StyleContext &context() const { return m_context; }
    const QStringList &warnings() const { return m_warnings; }

    /// The initial value of every property.
    static const ComputedStyle &initialStyle();

    /// The font size that "medium" resolves to, i.e. the user's default.
    double defaultFontSize() const { return m_context.rootFontSize; }

private:
    struct Candidate
    {
        const Declaration *declaration = nullptr;
        quint32 specificity = 0;
        int originRank = 0; ///< -1 user agent, 0 presentational hints, 1 author, 2 inline.
        int order = 0;
        /// Global source position; breaks ties `order` cannot, because each
        /// author sheet numbers its own rules from zero.
        quint32 sourceIndex = 0;
        QString selectorText;
        bool fromInline = false;
    };

    /// One rule selector that may match an element, indexed by its subject.
    ///
    /// The index removes the O(elements x rules) brute force that made styling a
    /// real page quadratic: instead of testing every rule against every element,
    /// an element tests only the rules whose right-most compound could match it.
    struct IndexedRule
    {
        const StyleRule *rule = nullptr;
        const Selector *selector = nullptr;
        /// Position in source order across all sheets. Author sheets are each
        /// parsed with order numbering starting at zero, so `rule.order` alone
        /// cannot tell two rules from different sheets apart; this index can.
        quint32 sourceIndex = 0;
    };

    ComputedStyle computeFor(const dom::Element *element, const ComputedStyle *parentStyle);
    /// Rebuilds m_effectiveRules and m_ruleIndex from the current sheets and
    /// viewport. Called once per computeStyles() pass.
    void buildRuleIndex();
    void inheritFrom(ComputedStyle *style, const ComputedStyle *parent);
    /// Applies one declaration. `inheritedFontSize` is the size the element
    /// would have with no font-size declaration at all, which is what relative
    /// font sizes are measured against.
    void applyDeclaration(ComputedStyle *style, const Declaration &declaration,
                          double inheritedFontSize, bool *consumed);
    void applyPresentationalHints(ComputedStyle *style, const dom::Element *element,
                                  double inheritedFontSize);

    StyleContext m_context;
    QList<Stylesheet> m_sheets;

    /// Every rule whose media query matches the current viewport, built once per
    /// pass. m_ruleIndex points into these, so the vector must not be resized
    /// while the index is in use.
    QVector<StyleRule> m_effectiveRules;
    /// Selectors bucketed by a key derived from their right-most compound's id,
    /// class or type, so an element only tests the rules that can match it.
    QHash<QString, QVector<IndexedRule>> m_ruleIndex;
    /// Selectors whose subject has no id, class or type: tested against every
    /// element, like the universal selector itself.
    QVector<IndexedRule> m_universalRules;

    /// The styles themselves, owned here so that each has a fixed address: the
    /// box tree stores a pointer to its style and must be able to rely on it.
    /// A hash of values would move them on every rehash.
    std::deque<ComputedStyle> m_styles;
    /// Index into m_styles, so lookups stay constant time without ever handing
    /// out a pointer into a container that can move its elements.
    QHash<const dom::Element *, ComputedStyle *> m_styleIndex;
    QHash<const dom::Element *, QList<AppliedDeclaration>> m_applied;
    QStringList m_warnings;
    QString m_baseBackground;
    QString m_baseText;
};

} // namespace oqb::css
