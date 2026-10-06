#include "css/Value.h"

#include "css/Tokenizer.h"

#include <QHash>
#include <QRegularExpression>

#include <cmath>

namespace oqb::css {

Value Value::invalid()
{
    return {};
}

Value Value::fromKeyword(const QString &keyword)
{
    Value value;
    value.kind = Kind::Keyword;
    value.keyword = keyword.toLower();
    value.original = keyword;
    return value;
}

Value Value::fromLength(double pixels, const QString &unit, const QString &original)
{
    Value value;
    value.kind = Kind::Length;
    value.number = pixels;
    value.unit = unit;
    value.original = original.isEmpty() ? QString::number(pixels) + unit : original;
    return value;
}

Value Value::fromNumber(double number, const QString &original)
{
    Value value;
    value.kind = Kind::Number;
    value.number = number;
    value.original = original.isEmpty() ? QString::number(number) : original;
    return value;
}

Value Value::fromPercentage(double percentage, const QString &original)
{
    Value value;
    value.kind = Kind::Percentage;
    value.number = percentage;
    value.unit = QStringLiteral("%");
    value.original = original.isEmpty() ? QString::number(percentage) + u'%' : original;
    return value;
}

Value Value::fromColor(const QColor &color, const QString &original)
{
    Value value;
    value.kind = Kind::Color;
    value.color = color;
    value.original = original;
    return value;
}

Value Value::fromUrl(const QString &url)
{
    Value value;
    value.kind = Kind::Url;
    value.keyword = url;
    value.original = QStringLiteral("url(") + url + u')';
    return value;
}

Value Value::fromString(const QString &text)
{
    Value value;
    value.kind = Kind::String;
    value.keyword = text;
    value.original = u'"' + text + u'"';
    return value;
}

bool Value::toPixels(double base, double *pixels) const
{
    if (!pixels)
        return false;

    switch (kind) {
    case Kind::Length:
        // A relative unit such as em or vw is only meaningful with the context
        // that resolveLength() supplies, so it must not be read as pixels here.
        if (!unit.isEmpty() && unit != QLatin1String("px"))
            return false;
        *pixels = number;
        return true;
    case Kind::Percentage:
        *pixels = base * number / 100.0;
        return true;
    case Kind::Number:
        // A unitless zero is a valid length; any other number is not.
        if (qFuzzyIsNull(number)) {
            *pixels = 0;
            return true;
        }
        return false;
    default:
        return false;
    }
}

QString Value::toString() const
{
    if (!original.isEmpty())
        return original;

    switch (kind) {
    case Kind::Invalid:
        return QStringLiteral("invalid");
    case Kind::Keyword:
        return keyword;
    case Kind::Length:
        if (qFuzzyIsNull(number))
            return QStringLiteral("0");
        return QString::number(number, 'g', 6) + (unit.isEmpty() ? QStringLiteral("px") : unit);
    case Kind::Percentage:
        return QString::number(number, 'g', 6) + u'%';
    case Kind::Number:
        return QString::number(number, 'g', 6);
    case Kind::Color:
        return color.alpha() == 255 ? color.name(QColor::HexRgb) : color.name(QColor::HexArgb);
    case Kind::Url:
        return QStringLiteral("url(") + keyword + u')';
    case Kind::String:
        return u'"' + keyword + u'"';
    }
    return {};
}

bool Value::operator==(const Value &other) const
{
    if (kind != other.kind)
        return false;
    switch (kind) {
    case Kind::Invalid:
        return true;
    case Kind::Keyword:
    case Kind::Url:
    case Kind::String:
        return keyword == other.keyword;
    case Kind::Length:
        return qFuzzyCompare(number + 1, other.number + 1) && unit == other.unit;
    case Kind::Percentage:
    case Kind::Number:
        return qFuzzyCompare(number + 1, other.number + 1);
    case Kind::Color:
        return color == other.color;
    }
    return false;
}

namespace values {
namespace {

/// The CSS named colours, including the extended set (CSS Color 4 §6.1).
const QHash<QString, QRgb> &namedColors()
{
    static const QHash<QString, QRgb> kColors = [] {
        QHash<QString, QRgb> colors;
        const auto add = [&colors](const char *name, QRgb rgb) {
            colors.insert(QString::fromLatin1(name), rgb);
        };

        add("aliceblue", 0xFFF0F8FF);      add("antiquewhite", 0xFFFAEBD7);
        add("aqua", 0xFF00FFFF);           add("aquamarine", 0xFF7FFFD4);
        add("azure", 0xFFF0FFFF);          add("beige", 0xFFF5F5DC);
        add("bisque", 0xFFFFE4C4);         add("black", 0xFF000000);
        add("blanchedalmond", 0xFFFFEBCD); add("blue", 0xFF0000FF);
        add("blueviolet", 0xFF8A2BE2);     add("brown", 0xFFA52A2A);
        add("burlywood", 0xFFDEB887);      add("cadetblue", 0xFF5F9EA0);
        add("chartreuse", 0xFF7FFF00);     add("chocolate", 0xFFD2691E);
        add("coral", 0xFFFF7F50);          add("cornflowerblue", 0xFF6495ED);
        add("cornsilk", 0xFFFFF8DC);       add("crimson", 0xFFDC143C);
        add("cyan", 0xFF00FFFF);           add("darkblue", 0xFF00008B);
        add("darkcyan", 0xFF008B8B);       add("darkgoldenrod", 0xFFB8860B);
        add("darkgray", 0xFFA9A9A9);       add("darkgreen", 0xFF006400);
        add("darkgrey", 0xFFA9A9A9);       add("darkkhaki", 0xFFBDB76B);
        add("darkmagenta", 0xFF8B008B);    add("darkolivegreen", 0xFF556B2F);
        add("darkorange", 0xFFFF8C00);     add("darkorchid", 0xFF9932CC);
        add("darkred", 0xFF8B0000);        add("darksalmon", 0xFFE9967A);
        add("darkseagreen", 0xFF8FBC8F);   add("darkslateblue", 0xFF483D8B);
        add("darkslategray", 0xFF2F4F4F);  add("darkslategrey", 0xFF2F4F4F);
        add("darkturquoise", 0xFF00CED1);  add("darkviolet", 0xFF9400D3);
        add("deeppink", 0xFFFF1493);       add("deepskyblue", 0xFF00BFFF);
        add("dimgray", 0xFF696969);        add("dimgrey", 0xFF696969);
        add("dodgerblue", 0xFF1E90FF);     add("firebrick", 0xFFB22222);
        add("floralwhite", 0xFFFFFAF0);    add("forestgreen", 0xFF228B22);
        add("fuchsia", 0xFFFF00FF);        add("gainsboro", 0xFFDCDCDC);
        add("ghostwhite", 0xFFF8F8FF);     add("gold", 0xFFFFD700);
        add("goldenrod", 0xFFDAA520);      add("gray", 0xFF808080);
        add("green", 0xFF008000);          add("greenyellow", 0xFFADFF2F);
        add("grey", 0xFF808080);           add("honeydew", 0xFFF0FFF0);
        add("hotpink", 0xFFFF69B4);        add("indianred", 0xFFCD5C5C);
        add("indigo", 0xFF4B0082);         add("ivory", 0xFFFFFFF0);
        add("khaki", 0xFFF0E68C);          add("lavender", 0xFFE6E6FA);
        add("lavenderblush", 0xFFFFF0F5);  add("lawngreen", 0xFF7CFC00);
        add("lemonchiffon", 0xFFFFFACD);   add("lightblue", 0xFFADD8E6);
        add("lightcoral", 0xFFF08080);     add("lightcyan", 0xFFE0FFFF);
        add("lightgoldenrodyellow", 0xFFFAFAD2); add("lightgray", 0xFFD3D3D3);
        add("lightgreen", 0xFF90EE90);     add("lightgrey", 0xFFD3D3D3);
        add("lightpink", 0xFFFFB6C1);      add("lightsalmon", 0xFFFFA07A);
        add("lightseagreen", 0xFF20B2AA);  add("lightskyblue", 0xFF87CEFA);
        add("lightslategray", 0xFF778899); add("lightslategrey", 0xFF778899);
        add("lightsteelblue", 0xFFB0C4DE); add("lightyellow", 0xFFFFFFE0);
        add("lime", 0xFF00FF00);           add("limegreen", 0xFF32CD32);
        add("linen", 0xFFFAF0E6);          add("magenta", 0xFFFF00FF);
        add("maroon", 0xFF800000);         add("mediumaquamarine", 0xFF66CDAA);
        add("mediumblue", 0xFF0000CD);     add("mediumorchid", 0xFFBA55D3);
        add("mediumpurple", 0xFF9370DB);   add("mediumseagreen", 0xFF3CB371);
        add("mediumslateblue", 0xFF7B68EE); add("mediumspringgreen", 0xFF00FA9A);
        add("mediumturquoise", 0xFF48D1CC); add("mediumvioletred", 0xFFC71585);
        add("midnightblue", 0xFF191970);   add("mintcream", 0xFFF5FFFA);
        add("mistyrose", 0xFFFFE4E1);      add("moccasin", 0xFFFFE4B5);
        add("navajowhite", 0xFFFFDEAD);    add("navy", 0xFF000080);
        add("oldlace", 0xFFFDF5E6);        add("olive", 0xFF808000);
        add("olivedrab", 0xFF6B8E23);      add("orange", 0xFFFFA500);
        add("orangered", 0xFFFF4500);      add("orchid", 0xFFDA70D6);
        add("palegoldenrod", 0xFFEEE8AA);  add("palegreen", 0xFF98FB98);
        add("paleturquoise", 0xFFAFEEEE);  add("palevioletred", 0xFFDB7093);
        add("papayawhip", 0xFFFFEFD5);     add("peachpuff", 0xFFFFDAB9);
        add("peru", 0xFFCD853F);           add("pink", 0xFFFFC0CB);
        add("plum", 0xFFDDA0DD);           add("powderblue", 0xFFB0E0E6);
        add("purple", 0xFF800080);         add("rebeccapurple", 0xFF663399);
        add("red", 0xFFFF0000);            add("rosybrown", 0xFFBC8F8F);
        add("royalblue", 0xFF4169E1);      add("saddlebrown", 0xFF8B4513);
        add("salmon", 0xFFFA8072);         add("sandybrown", 0xFFF4A460);
        add("seagreen", 0xFF2E8B57);       add("seashell", 0xFFFFF5EE);
        add("sienna", 0xFFA0522D);         add("silver", 0xFFC0C0C0);
        add("skyblue", 0xFF87CEEB);        add("slateblue", 0xFF6A5ACD);
        add("slategray", 0xFF708090);      add("slategrey", 0xFF708090);
        add("snow", 0xFFFFFAFA);           add("springgreen", 0xFF00FF7F);
        add("steelblue", 0xFF4682B4);      add("tan", 0xFFD2B48C);
        add("teal", 0xFF008080);           add("thistle", 0xFFD8BFD8);
        add("tomato", 0xFFFF6347);         add("turquoise", 0xFF40E0D0);
        add("violet", 0xFFEE82EE);         add("wheat", 0xFFF5DEB3);
        add("white", 0xFFFFFFFF);          add("whitesmoke", 0xFFF5F5F5);
        add("yellow", 0xFFFFFF00);         add("yellowgreen", 0xFF9ACD32);
        return colors;
    }();
    return kColors;
}

int componentToByte(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.endsWith(u'%')) {
        const double percent = trimmed.left(trimmed.size() - 1).toDouble();
        return qBound(0, static_cast<int>(std::lround(percent * 255.0 / 100.0)), 255);
    }
    return qBound(0, static_cast<int>(std::lround(trimmed.toDouble())), 255);
}

double componentToAlpha(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.endsWith(u'%'))
        return qBound(0.0, trimmed.left(trimmed.size() - 1).toDouble() / 100.0, 1.0);
    return qBound(0.0, trimmed.toDouble(), 1.0);
}

/// Renders a hue/saturation/lightness triple as RGB (CSS Color 4 §7).
QColor fromHsl(double hue, double saturation, double lightness, double alpha)
{
    hue = std::fmod(hue, 360.0);
    if (hue < 0)
        hue += 360.0;
    saturation = qBound(0.0, saturation, 1.0);
    lightness = qBound(0.0, lightness, 1.0);

    const double m2 = lightness <= 0.5 ? lightness * (saturation + 1.0)
                                       : lightness + saturation - lightness * saturation;
    const double m1 = lightness * 2.0 - m2;

    const auto channel = [&](double h) {
        if (h < 0)
            h += 6.0;
        if (h > 6)
            h -= 6.0;
        if (h < 1)
            return m1 + (m2 - m1) * h;
        if (h < 3)
            return m2;
        if (h < 4)
            return m1 + (m2 - m1) * (4.0 - h);
        return m1;
    };

    QColor color;
    color.setRgbF(qBound(0.0, channel(hue / 60.0 + 2.0), 1.0),
                  qBound(0.0, channel(hue / 60.0), 1.0),
                  qBound(0.0, channel(hue / 60.0 - 2.0), 1.0),
                  qBound(0.0, alpha, 1.0));
    return color;
}

} // namespace

bool colorFromKeyword(const QString &keyword, QColor *out)
{
    if (!out)
        return false;

    const QString lowered = keyword.trimmed().toLower();
    if (lowered == QLatin1String("transparent")) {
        *out = QColor(0, 0, 0, 0);
        return true;
    }
    // currentColor depends on the element's own colour, so the cascade resolves
    // it later rather than here.
    if (lowered == QLatin1String("currentcolor"))
        return false;

    const auto it = namedColors().constFind(lowered);
    if (it == namedColors().constEnd())
        return false;

    *out = QColor::fromRgba(it.value());
    return true;
}

Value parseColor(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return Value::invalid();

    const QString lowered = trimmed.toLower();

    if (QColor keyword; colorFromKeyword(lowered, &keyword))
        return Value::fromColor(keyword, trimmed);

    const auto hexDigit = [](QChar c) -> int {
        if (c >= u'0' && c <= u'9')
            return c.unicode() - u'0';
        if (c >= u'a' && c <= u'f')
            return c.unicode() - u'a' + 10;
        return -1;
    };

    // #rgb, #rgba, #rrggbb, #rrggbbaa
    if (lowered.startsWith(u'#')) {
        const QString hex = lowered.mid(1);
        for (const QChar c : hex) {
            if (hexDigit(c) < 0)
                return Value::invalid();
        }

        QColor color;
        if (hex.size() == 3 || hex.size() == 4) {
            const int a = hex.size() == 4 ? hexDigit(hex.at(3)) : 15;
            color.setRgb(hexDigit(hex.at(0)) * 17, hexDigit(hex.at(1)) * 17,
                         hexDigit(hex.at(2)) * 17, a * 17);
        } else if (hex.size() == 6 || hex.size() == 8) {
            const int a = hex.size() == 8 ? hexDigit(hex.at(6)) * 16 + hexDigit(hex.at(7)) : 255;
            color.setRgb(hexDigit(hex.at(0)) * 16 + hexDigit(hex.at(1)),
                         hexDigit(hex.at(2)) * 16 + hexDigit(hex.at(3)),
                         hexDigit(hex.at(4)) * 16 + hexDigit(hex.at(5)), a);
        } else {
            return Value::invalid();
        }
        return Value::fromColor(color, trimmed);
    }

    // Functional notations. Both the legacy comma syntax and the modern
    // space-separated syntax with an optional "/ alpha" are accepted.
    const int open = lowered.indexOf(u'(');
    if (open > 0 && lowered.endsWith(u')')) {
        const QString function = lowered.left(open).trimmed();
        const QString arguments = lowered.mid(open + 1, lowered.size() - open - 2);

        QStringList parts = arguments.split(u',');
        if (parts.size() == 1)
            parts = arguments.split(QRegularExpression(QStringLiteral("[\\s/]+")),
                                    Qt::SkipEmptyParts);

        QStringList clean;
        for (const QString &part : parts) {
            const QString item = part.trimmed();
            if (!item.isEmpty())
                clean.append(item);
        }

        if (function == QLatin1String("rgb") || function == QLatin1String("rgba")) {
            if (clean.size() < 3)
                return Value::invalid();
            const int r = componentToByte(clean.at(0));
            const int g = componentToByte(clean.at(1));
            const int b = componentToByte(clean.at(2));
            const double a = clean.size() >= 4 ? componentToAlpha(clean.at(3)) : 1.0;
            return Value::fromColor(QColor(r, g, b, static_cast<int>(std::lround(a * 255.0))),
                                    trimmed);
        }

        if (function == QLatin1String("hsl") || function == QLatin1String("hsla")) {
            if (clean.size() < 3)
                return Value::invalid();

            const QString hueText = clean.at(0);
            double hue = 0;
            if (hueText.endsWith(QLatin1String("deg")))
                hue = hueText.left(hueText.size() - 3).toDouble();
            else if (hueText.endsWith(QLatin1String("turn")))
                hue = hueText.left(hueText.size() - 4).toDouble() * 360.0;
            else if (hueText.endsWith(QLatin1String("grad")))
                hue = hueText.left(hueText.size() - 4).toDouble() * 0.9;
            else if (hueText.endsWith(QLatin1String("rad")))
                hue = hueText.left(hueText.size() - 3).toDouble() * 180.0 / M_PI;
            else
                hue = hueText.toDouble();

            const auto fraction = [](const QString &value) {
                return value.endsWith(u'%')
                    ? value.left(value.size() - 1).toDouble() / 100.0
                    : value.toDouble();
            };

            const double saturation = fraction(clean.at(1));
            const double lightness = fraction(clean.at(2));
            const double alpha = clean.size() >= 4 ? componentToAlpha(clean.at(3)) : 1.0;

            return Value::fromColor(fromHsl(hue, saturation, lightness, alpha), trimmed);
        }
    }

    return Value::invalid();
}

bool lengthToPixels(double amount, const QString &unit, double fontSize, double rootFontSize,
                    double viewportWidth, double viewportHeight, double *out)
{
    if (!out)
        return false;

    const QString u = unit.trimmed().toLower();

    if (u.isEmpty() || u == QLatin1String("px")) {
        *out = amount;
        return true;
    }
    if (u == QLatin1String("pt")) {
        *out = amount * 96.0 / 72.0;
        return true;
    }
    if (u == QLatin1String("pc")) {
        *out = amount * 16.0;
        return true;
    }
    if (u == QLatin1String("in")) {
        *out = amount * 96.0;
        return true;
    }
    if (u == QLatin1String("cm")) {
        *out = amount * 96.0 / 2.54;
        return true;
    }
    if (u == QLatin1String("mm")) {
        *out = amount * 96.0 / 25.4;
        return true;
    }
    if (u == QLatin1String("q")) {
        *out = amount * 96.0 / 101.6;
        return true;
    }
    if (u == QLatin1String("em")) {
        *out = amount * fontSize;
        return true;
    }
    if (u == QLatin1String("rem")) {
        *out = amount * rootFontSize;
        return true;
    }
    if (u == QLatin1String("ex")) {
        // Without font metrics half an em is the conventional approximation and
        // is what browsers fall back on for many fonts.
        *out = amount * fontSize * 0.5;
        return true;
    }
    if (u == QLatin1String("ch")) {
        *out = amount * fontSize * 0.5;
        return true;
    }
    if (u == QLatin1String("vw")) {
        *out = amount * viewportWidth / 100.0;
        return true;
    }
    if (u == QLatin1String("vh")) {
        *out = amount * viewportHeight / 100.0;
        return true;
    }
    if (u == QLatin1String("vmin")) {
        *out = amount * qMin(viewportWidth, viewportHeight) / 100.0;
        return true;
    }
    if (u == QLatin1String("vmax")) {
        *out = amount * qMax(viewportWidth, viewportHeight) / 100.0;
        return true;
    }

    return false;
}

bool absoluteFontSizeKeyword(const QString &keyword, double *pixels)
{
    if (!pixels)
        return false;

    static const QHash<QString, double> kSizes = {
        {QStringLiteral("xx-small"), 9.0},  {QStringLiteral("x-small"), 10.0},
        {QStringLiteral("small"), 13.0},    {QStringLiteral("medium"), 16.0},
        {QStringLiteral("large"), 18.0},    {QStringLiteral("x-large"), 24.0},
        {QStringLiteral("xx-large"), 32.0}, {QStringLiteral("xxx-large"), 48.0},
    };

    const auto it = kSizes.constFind(keyword.trimmed().toLower());
    if (it == kSizes.constEnd())
        return false;
    *pixels = it.value();
    return true;
}

double relativeFontSizeKeyword(const QString &keyword, double parent, bool *recognised)
{
    const QString lowered = keyword.trimmed().toLower();
    if (lowered == QLatin1String("larger")) {
        if (recognised)
            *recognised = true;
        return parent * 1.2;
    }
    if (lowered == QLatin1String("smaller")) {
        if (recognised)
            *recognised = true;
        return parent / 1.2;
    }
    if (recognised)
        *recognised = false;
    return parent;
}

QString unquote(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.size() >= 2
        && ((trimmed.startsWith(u'"') && trimmed.endsWith(u'"'))
            || (trimmed.startsWith(u'\'') && trimmed.endsWith(u'\'')))) {
        return trimmed.mid(1, trimmed.size() - 2);
    }
    return trimmed;
}


Value parseComponentValue(const QString &text)
{
    Tokenizer tokenizer(text);
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

} // namespace values

} // namespace oqb::css
