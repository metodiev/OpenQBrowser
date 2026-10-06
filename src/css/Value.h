#pragma once

#include <QColor>
#include <QList>
#include <QString>

namespace oqb::css {

/// A computed CSS value.
///
/// OpenQBrowser uses a small tagged union rather than a variant so that every
/// value carries exactly the units it needs and the layout engine can switch on
/// the kind explicitly. Lengths are stored in CSS pixels; percentages stay
/// symbolic because they resolve against a containing block that only exists
/// during layout.
struct Value
{
    enum class Kind {
        Invalid,
        Keyword,    ///< A named keyword such as "auto", "block", "red".
        Length,     ///< A length, converted to pixels where the unit allows.
        Percentage, ///< A percentage of the containing block dimension.
        Number,     ///< A unitless number, e.g. line-height: 1.5.
        Color,      ///< A resolved colour.
        Url,        ///< A url() value, kept as text.
        String,     ///< A quoted string, e.g. a font family name.
    };

    Kind kind = Kind::Invalid;

    /// Keyword text, lower-cased. Also holds the text of a URL value.
    QString keyword;
    /// Pixels for Length, the amount for Percentage and Number.
    double number = 0;
    /// The original unit, retained for the inspector.
    QString unit;
    /// Resolved colour.
    QColor color;
    /// The author's original text, used to re-serialise for the inspector.
    QString original;

    bool isValid() const { return kind != Kind::Invalid; }

    static Value invalid();
    static Value fromKeyword(const QString &keyword);
    static Value fromLength(double pixels, const QString &unit, const QString &original = {});
    static Value fromNumber(double number, const QString &original = {});
    static Value fromPercentage(double percentage, const QString &original = {});
    static Value fromColor(const QColor &color, const QString &original = {});
    static Value fromUrl(const QString &url);
    static Value fromString(const QString &text);

    /// True when this value is indistinguishable from the given keyword.
    bool isKeyword(const QString &name) const
    {
        return kind == Kind::Keyword && keyword.compare(name, Qt::CaseInsensitive) == 0;
    }
    bool isLength() const { return kind == Kind::Length; }
    bool isPercentage() const { return kind == Kind::Percentage; }
    bool isNumber() const { return kind == Kind::Number; }
    bool isColor() const { return kind == Kind::Color; }
    bool isUrl() const { return kind == Kind::Url; }

    /// True for "auto", which means "decide from context" for most properties.
    bool isAuto() const { return isKeyword(QStringLiteral("auto")); }
    /// True for "none" and "normal", which select the initial value.
    bool isNone() const
    {
        return isKeyword(QStringLiteral("none")) || isKeyword(QStringLiteral("normal"));
    }

    /// To Pixels: resolves Length directly and Percentage against `base`.
    /// Also accepts a unitless zero, which CSS allows for lengths.
    bool toPixels(double base, double *pixels) const;

    /// The value as CSS text, for the inspector.
    QString toString() const;

    bool operator==(const Value &other) const;
    bool operator!=(const Value &other) const { return !(*this == other); }
};

namespace values {

/// Parses a colour from a keyword, #rgb/#rrggbb/#rrggbbaa, rgb(), rgba(),
/// hsl(), hsla(), or the transparent keyword.
Value parseColor(const QString &text);

/// Looks up a CSS colour keyword such as "rebeccapurple".
bool colorFromKeyword(const QString &keyword, QColor *out);

/// Resolves a length to pixels. Relative units need context the caller
/// supplies: `fontSize` for em, `rootFontSize` for rem and `viewportWidth`/
/// `viewportHeight` for the viewport units.
bool lengthToPixels(double amount, const QString &unit, double fontSize, double rootFontSize,
                    double viewportWidth, double viewportHeight, double *out);

/// The pixel size a CSS font-size keyword such as "medium" means.
bool absoluteFontSizeKeyword(const QString &keyword, double *pixels);

/// Parses one standalone component value, such as "4px" or "auto".
///
/// Lengths keep their unit: a percentage or an `em` cannot be reduced to pixels
/// until layout runs and the containing block and font size are known, which is
/// what makes `margin: 2em` and `width: 50%` correct. Only the first token is
/// read, so this is for a value already known to be a single component - a
/// shorthand's parts, or one item of a track list.
///
/// It lives in `values` alongside the other parsing and resolution helpers,
/// because both the cascade's shorthand expansion and the grid track parser need
/// exactly these rules and two copies of "what is a length" would drift.
Value parseComponentValue(const QString &text);

/// Converts the font-size keywords "larger" and "smaller" relative to `parent`.
double relativeFontSizeKeyword(const QString &keyword, double parent, bool *recognised);

/// Trims quotes from a quoted CSS string value.
QString unquote(const QString &text);

} // namespace values

} // namespace oqb::css
