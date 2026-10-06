#include "css/Stylesheet.h"

#include "css/Tokenizer.h"

#include <QRegularExpression>
#include <QSet>

#include <cmath>
#include <algorithm>

namespace oqb::css {
namespace {

/// True for units whose value depends on context that only the cascade knows.
bool isRelativeUnit(const QString &unit)
{
    static const QSet<QString> kRelative = {
        QStringLiteral("em"),   QStringLiteral("rem"), QStringLiteral("ex"),
        QStringLiteral("ch"),   QStringLiteral("vw"),  QStringLiteral("vh"),
        QStringLiteral("vmin"), QStringLiteral("vmax"),
    };
    return kRelative.contains(unit);
}

/// Builds a Length value from a dimension token.
///
/// Absolute units are converted to pixels immediately because they never
/// depend on context. Relative units keep their amount and unit so that the
/// cascade can resolve them against the element's own font size and the
/// viewport; resolving them here with a guessed font size would make
/// "font-size: 2em" wrong for every element that is not 16px.
Value makeLengthValue(const Token &token)
{
    if (!isRelativeUnit(token.unit)) {
        double pixels = 0;
        if (values::lengthToPixels(token.number, token.unit, 0, 0, 0, 0, &pixels))
            return Value::fromLength(pixels, QStringLiteral("px"), token.text);
    }

    // A relative unit, or a unit this engine does not implement such as "deg".
    // The raw amount and unit are preserved so the property can read them.
    Value value = Value::fromLength(token.number, token.unit, token.text);
    return value;
}

/// Turns the token stream of a declaration value into a Value.
///
/// A value can be a keyword, a length, a percentage, a number, a colour, a URL,
/// a string, or a space-separated list of the above (as in "1px solid red").
Value parseValueFromTokens(const QList<Token> &tokens)
{
    if (tokens.isEmpty())
        return Value::invalid();

    // Reconstruct the source text of the value for colour parsing and to keep
    // the original spelling for the inspector.
    QString text;
    for (const Token &token : tokens)
        text += token.text;
    text = text.trimmed();

    // Single-token fast paths.
    if (tokens.size() == 1) {
        const Token &token = tokens.first();
        switch (token.type) {
        case TokenType::Ident:
            if (QColor color; values::colorFromKeyword(token.value, &color))
                return Value::fromColor(color, token.text);
            return Value::fromKeyword(token.value);
        case TokenType::Hash: {
            const Value color = values::parseColor(token.text);
            return color.isValid() ? color : Value::fromKeyword(token.value);
        }
        case TokenType::Number:
            return Value::fromNumber(token.number, token.text);
        case TokenType::Percentage:
            return Value::fromPercentage(token.number, token.text);
        case TokenType::Dimension:
            return makeLengthValue(token);
        case TokenType::Url: {
            QString url = token.value;
            if (url.startsWith(u'"') || url.startsWith(u'\''))
                url = values::unquote(url);
            return Value::fromUrl(url);
        }
        case TokenType::String:
            return Value::fromString(token.value);
        case TokenType::Function:
            // A functional notation such as rgb(...) or calc(...) arrives as
            // several tokens, so it is handled by the multi-token path below.
            break;
        default:
            break;
        }
    }

    // Colour functions such as rgb() and hsl() span several tokens; the raw
    // text is the most reliable thing to hand to the colour parser.
    if (!text.isEmpty() && (text.contains(u'(') || text.contains(u','))) {
        if (const Value color = values::parseColor(text); color.isValid())
            return color;
    }

    // A space-separated list, e.g. "1px solid red" or "Arial, sans-serif".
    QList<Value> parts;
    for (const Token &token : tokens) {
        // Commas separate list items and are represented implicitly.
        if (token.type == TokenType::Comma || token.type == TokenType::Whitespace)
            continue;

        switch (token.type) {
        case TokenType::Ident:
            parts.append(Value::fromKeyword(token.value));
            break;
        case TokenType::Number:
            parts.append(Value::fromNumber(token.number, token.text));
            break;
        case TokenType::Percentage:
            parts.append(Value::fromPercentage(token.number, token.text));
            break;
        case TokenType::Dimension:
            parts.append(makeLengthValue(token));
            break;
        case TokenType::Hash:
            parts.append(values::parseColor(token.text));
            break;
        case TokenType::String:
            parts.append(Value::fromString(token.value));
            break;
        case TokenType::Url:
            parts.append(Value::fromUrl(values::unquote(token.value)));
            break;
        default:
            break;
        }
    }

    // A value whose tokens do not map onto anything meaningful keeps its raw
    // text so that the inspector can still show what the author wrote.
    if (parts.isEmpty()) {
        Value value = Value::fromKeyword(text);
        value.kind = Value::Kind::Keyword;
        return value;
    }

    if (parts.size() == 1)
        return parts.first();

    // Lists keep their raw text too; properties that need structure read it
    // from there, which avoids modelling every list grammar.
    Value list = Value::fromKeyword(text);
    list.kind = Value::Kind::Keyword;
    return list;
}

/// Reads the tokens of a single declaration's value, stopping at ';', '}' or EOF.
QList<Token> readValueTokens(Tokenizer *tokenizer)
{
    QList<Token> tokens;
    int depth = 0;

    while (true) {
        const Token token = tokenizer->nextToken();
        if (token.type == TokenType::EndOfFile)
            break;
        if (depth == 0 && (token.type == TokenType::Semicolon || token.type == TokenType::RightCurly)) {
            // Put back the terminator so the caller's loop sees it.
            tokenizer->pushBack(token);
            break;
        }
        if (token.type == TokenType::LeftParen || token.type == TokenType::Function) {
            ++depth;
        } else if (token.type == TokenType::RightParen) {
            --depth;
        }
        tokens.append(token);
    }

    // Trim leading and trailing whitespace tokens.
    while (!tokens.isEmpty() && tokens.first().type == TokenType::Whitespace)
        tokens.removeFirst();
    while (!tokens.isEmpty() && tokens.last().type == TokenType::Whitespace)
        tokens.removeLast();

    return tokens;
}

/// Strips "!important" from the end of a declaration's tokens.
bool extractImportant(QList<Token> *tokens)
{
    // Look for an ident "important" preceded by a '!' delim, ignoring
    // whitespace, at the very end of the value.
    int index = tokens->size() - 1;
    while (index >= 0 && tokens->at(index).type == TokenType::Whitespace)
        --index;
    if (index < 0)
        return false;
    if (!tokens->at(index).isIdent(QStringLiteral("important")))
        return false;

    int bangIndex = index - 1;
    while (bangIndex >= 0 && tokens->at(bangIndex).type == TokenType::Whitespace)
        --bangIndex;
    if (bangIndex < 0 || !tokens->at(bangIndex).isDelim(u'!'))
        return false;

    tokens->erase(tokens->begin() + bangIndex, tokens->begin() + index + 1);
    while (!tokens->isEmpty() && tokens->last().type == TokenType::Whitespace)
        tokens->removeLast();
    return true;
}

/// Reads a declaration list between balanced braces.
QList<Declaration> readDeclarationBlock(Tokenizer *tokenizer, QStringList *errors)
{
    QList<Declaration> declarations;

    while (true) {
        tokenizer->skipWhitespace();
        const Token token = tokenizer->peekToken();

        if (token.type == TokenType::EndOfFile)
            break;
        if (token.type == TokenType::RightCurly) {
            tokenizer->nextToken();
            break;
        }
        if (token.type == TokenType::Semicolon) {
            tokenizer->nextToken();
            continue;
        }

        // A nested block such as a CSS nesting rule or an at-rule: skip it.
        if (token.type == TokenType::LeftCurly) {
            tokenizer->nextToken();
            tokenizer->readUntilMatchingBrace();
            continue;
        }

        if (token.type != TokenType::Ident) {
            // Junk between declarations: skip to the next ';' or '}'.
            tokenizer->nextToken();
            while (true) {
                const Token skipped = tokenizer->nextToken();
                if (skipped.type == TokenType::Semicolon || skipped.type == TokenType::RightCurly
                    || skipped.type == TokenType::EndOfFile) {
                    if (skipped.type == TokenType::RightCurly)
                        tokenizer->pushBack(skipped);
                    break;
                }
            }
            continue;
        }

        const Token propertyToken = tokenizer->nextToken();
        tokenizer->skipWhitespace();

        const Token colon = tokenizer->nextToken();
        if (colon.type != TokenType::Colon) {
            if (errors)
                *errors = *errors
                    << QStringLiteral("Expected ':' after \"%1\"").arg(propertyToken.text);
            continue;
        }

        QList<Token> valueTokens = readValueTokens(tokenizer);

        Declaration declaration;
        declaration.property = propertyToken.value.toLower();
        declaration.important = extractImportant(&valueTokens);
        declaration.value = parseValueFromTokens(valueTokens);

        QString raw;
        for (const Token &valueToken : valueTokens)
            raw += valueToken.text;
        declaration.source = propertyToken.text + QStringLiteral(": ") + raw.trimmed()
            + (declaration.important ? QStringLiteral(" !important") : QString());

        if (declaration.value.isValid())
            declarations.append(declaration);
        else if (errors)
            *errors = *errors << QStringLiteral("Could not parse value for \"%1\"")
                                    .arg(declaration.property);
    }

    return declarations;
}

/// Splits a media query list on commas at the top level.
QStringList splitMediaList(const QString &text)
{
    QStringList parts;
    int depth = 0;
    QString current;
    for (const QChar c : text) {
        if (c == u'(')
            ++depth;
        else if (c == u')')
            --depth;
        else if (c == u',' && depth == 0) {
            parts.append(current.trimmed());
            current.clear();
            continue;
        }
        current.append(c);
    }
    if (!current.trimmed().isEmpty())
        parts.append(current.trimmed());
    return parts;
}

/// Evaluates a single media feature comparison.
bool evaluateFeature(const QString &feature, int value, double viewportWidth, double viewportHeight)
{
    if (feature == QLatin1String("min-width"))
        return viewportWidth >= value;
    if (feature == QLatin1String("max-width"))
        return viewportWidth <= value;
    if (feature == QLatin1String("width"))
        return qFuzzyCompare(viewportWidth, value);
    if (feature == QLatin1String("min-height"))
        return viewportHeight >= value;
    if (feature == QLatin1String("max-height"))
        return viewportHeight <= value;
    if (feature == QLatin1String("height"))
        return qFuzzyCompare(viewportHeight, value);
    return false;
}

bool featuresAreWidthLike(const QString &feature)
{
    return feature == QLatin1String("min-width") || feature == QLatin1String("max-width")
        || feature == QLatin1String("width") || feature == QLatin1String("min-height")
        || feature == QLatin1String("max-height") || feature == QLatin1String("height");
}

/// Converts a media feature value to a number of pixels, accepting both a bare
/// number (interpreted as px) and a value with a unit.
double mediaLengthToPixels(const QString &value, double viewportWidth, double viewportHeight)
{
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty())
        return 0;

    static const QRegularExpression pattern(
        QStringLiteral("^([0-9.]+)\\s*([a-z%]*)$"));
    const auto match = pattern.match(trimmed.toLower());
    if (!match.hasMatch())
        return 0;

    const double amount = match.captured(1).toDouble();
    const QString unit = match.captured(2);

    double pixels = 0;
    if (values::lengthToPixels(amount, unit, 16.0, 16.0, viewportWidth, viewportHeight, &pixels))
        return pixels;
    return amount;
}

/// A parsed media query: a media type plus a list of feature conditions.
struct MediaQueryTerm
{
    QString type = QStringLiteral("all");
    bool negated = false;
    bool only = false;
    struct Condition
    {
        QString feature;
        QString value;
    };
    QList<Condition> conditions;
};

MediaQueryTerm parseMediaTerm(const QString &text)
{
    MediaQueryTerm term;
    QString remaining = text.trimmed().toLower();

    // Leading "not" or "only".
    if (remaining.startsWith(QLatin1String("not "))) {
        term.negated = true;
        remaining = remaining.mid(4).trimmed();
    } else if (remaining.startsWith(QLatin1String("only "))) {
        term.only = true;
        remaining = remaining.mid(5).trimmed();
    }

    // A leading bare word is the media type (screen, print, all, ...).
    static const QRegularExpression featurePattern(QStringLiteral("^\\s*([a-z-]+)\\s*:\\s*([^)]*)"));

    const int paren = remaining.indexOf(u'(');
    if (paren != 0 && paren > 0) {
        // "screen and (max-width: 100px)" carries its type before the "and" that
        // joins it to the first condition. Keeping the keyword in the type makes
        // it compare unequal to "screen", and the whole query then never matches:
        // a stylesheet written entirely with `@media screen and (...)` - which is
        // how most real sites are written - would contribute no rules at all.
        QString typeText = remaining.left(paren).trimmed();
        if (typeText.endsWith(QLatin1String("and")))
            typeText = typeText.left(typeText.size() - 3).trimmed();
        term.type = typeText;
        remaining = remaining.mid(paren);
    } else if (paren < 0) {
        // No features at all: the whole text was the media type.
        if (!remaining.isEmpty() && !remaining.contains(u':'))
            term.type = remaining;
        return term;
    }

    // Every parenthesised group is one condition.
    int index = 0;
    while (index < remaining.size()) {
        const int open = remaining.indexOf(u'(', index);
        if (open < 0)
            break;
        const int close = remaining.indexOf(u')', open);
        if (close < 0)
            break;

        const QString inside = remaining.mid(open + 1, close - open - 1).trimmed();
        const int colon = inside.indexOf(u':');
        if (colon > 0) {
            term.conditions.append({inside.left(colon).trimmed(), inside.mid(colon + 1).trimmed()});
        } else if (!inside.isEmpty()) {
            // A boolean feature such as "(hover)".
            term.conditions.append({inside, QString()});
        }
        index = close + 1;
    }

    return term;
}

} // namespace

QString StyleRule::selectorText() const
{
    QStringList parts;
    parts.reserve(selectors.size());
    for (const Selector &selector : selectors)
        parts.append(selector.source);
    return parts.join(QStringLiteral(", "));
}

int Stylesheet::ruleCount() const
{
    int count = static_cast<int>(m_rules.size());
    for (const AtRule &atRule : m_atRules) {
        count += static_cast<int>(atRule.rules.size());
    }
    return count;
}

QList<StyleRule> Stylesheet::rulesForMedia(const QString &mediaType, double viewportWidth) const
{
    QList<StyleRule> out;
    out.reserve(m_rules.size());

    for (const StyleRule &rule : m_rules) {
        if (rule.mediaQuery.isEmpty()) {
            out.append(rule);
        } else if (MediaQuery::matches(rule.mediaQuery, viewportWidth, 0)) {
            out.append(rule);
        }
    }

    // Nested at-rule content is flattened, with the media query kept on each
    // rule so the cascade can re-evaluate it later if the viewport changes.
    for (const AtRule &atRule : m_atRules) {
        if (atRule.name != QLatin1String("media"))
            continue;
        for (StyleRule rule : atRule.rules) {
            if (rule.mediaQuery.isEmpty())
                rule.mediaQuery = atRule.prelude;
            if (MediaQuery::matches(rule.mediaQuery, viewportWidth, 0))
                out.append(rule);
        }
    }

    // A conditional rule is not *later* than an unconditional one; it sits where
    // it was written. The two loops above walk separate lists, so their results
    // are concatenated and the source order has to be restored here. Without
    // this, every `@media` rule outranks every plain rule, and the narrowest
    // breakpoint in a stylesheet wins at every viewport width - which is what
    // stacked a real news page into one column instead of a responsive grid.
    std::stable_sort(out.begin(), out.end(),
                     [](const StyleRule &a, const StyleRule &b) { return a.order < b.order; });

    Q_UNUSED(mediaType);
    return out;
}

QList<Declaration> Stylesheet::parseDeclarationList(const QString &source, QStringList *errors)
{
    Tokenizer tokenizer(source);
    // The block reader stops at a closing brace, so a synthetic one is appended
    // to let an unterminated declaration list still yield its declarations.
    const QList<Declaration> declarations = readDeclarationBlock(&tokenizer, errors);
    return declarations;
}

Stylesheet Stylesheet::parse(const QString &source, int origin, int baseOrder, QStringList *errors)
{
    Stylesheet sheet;
    Tokenizer tokenizer(source);

    int order = baseOrder;

    while (true) {
        tokenizer.skipWhitespace();

        const Token token = tokenizer.peekToken();
        if (token.type == TokenType::EndOfFile)
            break;
        // Stray braces and semicolons are skipped, and HTML comment wrappers
        // inside a <style> element are ignored entirely.
        if (token.type == TokenType::RightCurly || token.type == TokenType::Semicolon
            || token.type == TokenType::Cdo || token.type == TokenType::Cdc) {
            tokenizer.nextToken();
            continue;
        }

        if (token.type == TokenType::AtKeyword) {
            tokenizer.nextToken();
            const QString atName = token.value;

            // Read the prelude up to '{' or ';'.
            QString prelude;
            while (true) {
                const Token part = tokenizer.nextToken();
                if (part.type == TokenType::EndOfFile)
                    break;
                if (part.type == TokenType::LeftCurly || part.type == TokenType::Semicolon) {
                    if (part.type == TokenType::Semicolon)
                        prelude.clear(); // "@import url(x);" style statement
                    break;
                }
                prelude += part.text;
            }

            AtRule atRule;
            atRule.name = atName;
            atRule.prelude = prelude.trimmed();

            if (atName == QLatin1String("media") || atName == QLatin1String("supports")) {
                // Parse the nested rule list by recursing on the block's text.
                const QString block = tokenizer.readUntilMatchingBrace();
                QStringList nested;
                const Stylesheet inner = Stylesheet::parse(block, origin, order, &nested);
                for (const StyleRule &rule : inner.rules()) {
                    StyleRule copy = rule;
                    copy.mediaQuery = atName == QLatin1String("media") ? atRule.prelude : QString();
                    atRule.rules.append(copy);
                }
                order += static_cast<int>(inner.ruleCount());
                if (errors)
                    *errors = *errors << nested;
            } else {
                // @font-face, @keyframes and other block at-rules are kept for
                // the inspector but do not contribute declarations yet.
                if (tokenizer.peekToken().type == TokenType::LeftCurly) {
                    tokenizer.nextToken();
                    tokenizer.readUntilMatchingBrace();
                }
            }

            sheet.m_atRules.append(atRule);
            continue;
        }

        // Otherwise this must be a style rule: selector list, then a block.
        QString selectorText;
        bool sawBlock = false;
        while (true) {
            const Token part = tokenizer.nextToken();
            if (part.type == TokenType::EndOfFile)
                break;
            if (part.type == TokenType::LeftCurly) {
                sawBlock = true;
                break;
            }
            if (part.type == TokenType::Semicolon) {
                // A stray semicolon in selector position; discard the fragment.
                selectorText.clear();
                break;
            }
            selectorText += part.text;
        }

        if (!sawBlock) {
            if (!selectorText.trimmed().isEmpty() && errors)
                *errors = *errors << QStringLiteral("Rule without a block: \"%1\"")
                                        .arg(selectorText.trimmed().left(40));
            continue;
        }

        QStringList unsupported;
        const QList<Selector> selectors
            = SelectorParser::parseList(selectorText.trimmed(), &unsupported);

        const QList<Declaration> declarations = readDeclarationBlock(&tokenizer, errors);

        if (errors)
            *errors = *errors << unsupported;

        if (!selectors.isEmpty() && !declarations.isEmpty()) {
            StyleRule rule;
            rule.selectors = selectors;
            rule.declarations = declarations;
            rule.origin = origin;
            rule.order = order++;
            sheet.m_rules.append(rule);
        }
    }

    // Nested at-rule rules need an order too, so the cascade sees a total order.
    for (AtRule &atRule : sheet.m_atRules) {
        for (StyleRule &rule : atRule.rules) {
            if (rule.order == 0)
                rule.order = order++;
        }
    }

    return sheet;
}

// ------------------------------------------------------------- MediaQuery

bool MediaQuery::isSupported(const QString &query)
{
    const QStringList terms = splitMediaList(query);
    if (terms.isEmpty())
        return false;

    for (const QString &termText : terms) {
        const MediaQueryTerm term = parseMediaTerm(termText);
        for (const MediaQueryTerm::Condition &condition : term.conditions) {
            const QString &feature = condition.feature;
            const bool known = feature == QLatin1String("min-width")
                || feature == QLatin1String("max-width") || feature == QLatin1String("width")
                || feature == QLatin1String("min-height")
                || feature == QLatin1String("max-height") || feature == QLatin1String("height")
                || feature == QLatin1String("orientation")
                || feature == QLatin1String("prefers-color-scheme")
                || feature == QLatin1String("prefers-reduced-motion")
                || feature == QLatin1String("min-resolution")
                || feature == QLatin1String("max-resolution")
                || feature == QLatin1String("hover") || feature == QLatin1String("pointer");
            if (!known)
                return false;
        }
    }

    return true;
}

bool MediaQuery::matches(const QString &query, double viewportWidth, double viewportHeight,
                         bool prefersDark)
{
    const QString trimmed = query.trimmed();
    if (trimmed.isEmpty())
        return true;

    // An unparsable query is treated as "not matching". CSS says an unknown
    // media type never matches, and this engine cannot do better than that.
    const QStringList terms = splitMediaList(trimmed);
    if (terms.isEmpty())
        return false;

    for (const QString &termText : terms) {
        const MediaQueryTerm term = parseMediaTerm(termText);

        bool matchesTerm = true;

        const QString &type = term.type;
        if (type != QLatin1String("all") && type != QLatin1String("screen") && !type.isEmpty())
            matchesTerm = false;

        for (const MediaQueryTerm::Condition &condition : term.conditions) {
            if (!matchesTerm)
                break;

            const QString &feature = condition.feature;
            const QString &value = condition.value;

            if (featuresAreWidthLike(feature)) {
                const double numeric = mediaLengthToPixels(value, viewportWidth, viewportHeight);
                if (!evaluateFeature(feature, static_cast<int>(std::lround(numeric)),
                                     viewportWidth, viewportHeight)) {
                    matchesTerm = false;
                }
            } else if (feature == QLatin1String("orientation")) {
                const bool landscape = viewportWidth >= viewportHeight;
                if ((value == QLatin1String("landscape")) != landscape)
                    matchesTerm = false;
            } else if (feature == QLatin1String("prefers-color-scheme")) {
                const bool wantsDark = value == QLatin1String("dark");
                if (wantsDark != prefersDark)
                    matchesTerm = false;
            } else if (feature == QLatin1String("prefers-reduced-motion")) {
                // OpenQBrowser renders statically, so motion is never requested.
                if (value == QLatin1String("reduce"))
                    matchesTerm = false;
            } else if (feature == QLatin1String("hover") || feature == QLatin1String("pointer")) {
                // A desktop browser with a pointing device.
                if (value == QLatin1String("none"))
                    matchesTerm = false;
            } else if (feature == QLatin1String("min-resolution")
                       || feature == QLatin1String("max-resolution")) {
                // Assume a standard 96 DPI display.
                const double dpi = 96.0;
                const bool isMin = feature.startsWith(QLatin1String("min"));
                const double wanted = value.endsWith(QLatin1String("dppx"))
                    ? value.left(value.size() - 5).toDouble() * 96.0
                    : value.left(value.size() - 3).toDouble();
                if (isMin ? dpi < wanted : dpi > wanted)
                    matchesTerm = false;
            } else {
                // Unknown feature: the query cannot match.
                matchesTerm = false;
            }
        }

        bool termResult = matchesTerm;
        if (term.negated)
            termResult = !termResult;
        if (termResult)
            return true;
    }

    return false;
}

} // namespace oqb::css
