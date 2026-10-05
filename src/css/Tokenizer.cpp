#include "css/Tokenizer.h"

#include <QHash>

namespace oqb::css {
namespace {

bool isWhitespace(QChar c)
{
    return c == u' ' || c == u'\t' || c == u'\n' || c == u'\r' || c == u'\f';
}

bool isDigit(QChar c)
{
    return c >= u'0' && c <= u'9';
}

bool isHexDigit(QChar c)
{
    return isDigit(c) || (c.toLower() >= u'a' && c.toLower() <= u'f');
}

bool isNameStart(QChar c)
{
    if (c.isLetter() || c == u'_' || c.unicode() >= 0x80)
        return true;
    return false;
}

bool isNameChar(QChar c)
{
    return isNameStart(c) || isDigit(c) || c == u'-';
}

} // namespace

QString Token::typeName() const
{
    switch (type) {
    case TokenType::Ident: return QStringLiteral("identifier");
    case TokenType::Function: return QStringLiteral("function");
    case TokenType::AtKeyword: return QStringLiteral("at-keyword");
    case TokenType::Hash: return QStringLiteral("hash");
    case TokenType::String: return QStringLiteral("string");
    case TokenType::BadString: return QStringLiteral("bad-string");
    case TokenType::Url: return QStringLiteral("url");
    case TokenType::BadUrl: return QStringLiteral("bad-url");
    case TokenType::Delim: return QStringLiteral("delimiter");
    case TokenType::Number: return QStringLiteral("number");
    case TokenType::Percentage: return QStringLiteral("percentage");
    case TokenType::Dimension: return QStringLiteral("dimension");
    case TokenType::Whitespace: return QStringLiteral("whitespace");
    case TokenType::Colon: return QStringLiteral("':'");
    case TokenType::Semicolon: return QStringLiteral("';'");
    case TokenType::Comma: return QStringLiteral("','");
    case TokenType::LeftSquare: return QStringLiteral("'['");
    case TokenType::RightSquare: return QStringLiteral("']'");
    case TokenType::LeftParen: return QStringLiteral("'('");
    case TokenType::RightParen: return QStringLiteral("')'");
    case TokenType::LeftCurly: return QStringLiteral("'{'");
    case TokenType::RightCurly: return QStringLiteral("'}'");
    case TokenType::Cdo: return QStringLiteral("'<!--'");
    case TokenType::Cdc: return QStringLiteral("'-->'");
    case TokenType::EndOfFile: return QStringLiteral("end of file");
    }
    return QStringLiteral("token");
}

Tokenizer::Tokenizer(const QString &input)
    : m_input(input)
{
}

QChar Tokenizer::peek(int offset) const
{
    const int index = m_position + offset;
    if (index < 0 || index >= m_input.size())
        return QChar();
    return m_input.at(index);
}

bool Tokenizer::atEnd(int offset) const
{
    return m_position + offset >= m_input.size();
}

bool Tokenizer::wouldStartIdentifier(int offset) const
{
    const QChar first = peek(offset);
    const QChar second = peek(offset + 1);
    const QChar third = peek(offset + 2);

    if (first == u'-') {
        if (isNameStart(second) || second == u'-')
            return true;
        // "-" followed by an escape.
        return second == u'\\' && third != u'\n';
    }
    if (isNameStart(first))
        return true;
    return first == u'\\' && second != u'\n';
}

bool Tokenizer::wouldStartNumber(int offset) const
{
    const QChar first = peek(offset);
    const QChar second = peek(offset + 1);

    if (isDigit(first))
        return true;
    if (first == u'.')
        return isDigit(second);
    if (first == u'+' || first == u'-')
        return isDigit(second) || (second == u'.' && isDigit(peek(offset + 2)));
    return false;
}

void Tokenizer::consumeComment()
{
    // Position sits on the '/' of "/*".
    m_position += 2;
    while (!atEnd()) {
        if (peek() == u'*' && peek(1) == u'/') {
            m_position += 2;
            return;
        }
        ++m_position;
    }
}

void Tokenizer::skipWhitespace()
{
    while (!atEnd()) {
        if (isWhitespace(peek())) {
            ++m_position;
            continue;
        }
        if (peek() == u'/' && peek(1) == u'*') {
            consumeComment();
            continue;
        }
        break;
    }
}

Token Tokenizer::consumeEscape(QString *out)
{
    // Position sits on the backslash.
    ++m_position;

    if (atEnd()) {
        out->append(QChar(0xFFFD));
        return {};
    }

    const QChar c = peek();
    if (isHexDigit(c)) {
        // Up to six hex digits, optionally followed by one whitespace character.
        QString hex;
        for (int i = 0; i < 6 && isHexDigit(peek()); ++i) {
            hex.append(peek());
            ++m_position;
        }
        if (isWhitespace(peek()))
            ++m_position;

        bool ok = false;
        const uint code = hex.toUInt(&ok, 16);
        char32_t codePoint = ok ? code : 0xFFFD;
        if (codePoint == 0 || codePoint > 0x10FFFF
            || (codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
            codePoint = 0xFFFD;
        }
        out->append(QString::fromUcs4(&codePoint, 1));
        return {};
    }

    out->append(c);
    ++m_position;
    return {};
}

Token Tokenizer::consumeString(QChar quote)
{
    Token token;
    token.type = TokenType::String;
    const int start = m_position;

    ++m_position; // opening quote
    QString value;

    while (!atEnd()) {
        const QChar c = peek();
        if (c == quote) {
            ++m_position;
            token.text = m_input.mid(start, m_position - start);
            token.value = value;
            return token;
        }
        if (c == u'\n') {
            // An unescaped newline ends the string as a bad-string token.
            token.type = TokenType::BadString;
            token.text = m_input.mid(start, m_position - start);
            token.value = value;
            return token;
        }
        if (c == u'\\') {
            if (peek(1) == u'\n') {
                m_position += 2; // escaped newline is elided
                continue;
            }
            consumeEscape(&value);
            continue;
        }
        value.append(c);
        ++m_position;
    }

    token.text = m_input.mid(start);
    token.value = value;
    return token;
}

Token Tokenizer::consumeUrl()
{
    Token token;
    token.type = TokenType::Url;
    const int start = m_position;

    // Position sits just after "url(".
    skipWhitespace();

    QString value;
    while (!atEnd()) {
        const QChar c = peek();
        if (c == u')') {
            ++m_position;
            token.text = m_input.mid(start, m_position - start);
            token.value = value;
            return token;
        }
        if (isWhitespace(c)) {
            skipWhitespace();
            if (peek() == u')') {
                ++m_position;
                token.text = m_input.mid(start, m_position - start);
                token.value = value;
                return token;
            }
            // Anything but ')' after whitespace makes this a bad-url.
            token.type = TokenType::BadUrl;
            while (!atEnd() && peek() != u')')
                ++m_position;
            if (!atEnd())
                ++m_position;
            token.text = m_input.mid(start, m_position - start);
            return token;
        }
        if (c == u'"' || c == u'\'' || c == u'(') {
            token.type = TokenType::BadUrl;
            while (!atEnd() && peek() != u')')
                ++m_position;
            if (!atEnd())
                ++m_position;
            token.text = m_input.mid(start, m_position - start);
            return token;
        }
        if (c == u'\\') {
            if (peek(1) == u'\n') {
                token.type = TokenType::BadUrl;
                while (!atEnd() && peek() != u')')
                    ++m_position;
                if (!atEnd())
                    ++m_position;
                token.text = m_input.mid(start, m_position - start);
                return token;
            }
            consumeEscape(&value);
            continue;
        }
        value.append(c);
        ++m_position;
    }

    token.text = m_input.mid(start);
    token.value = value.trimmed();
    return token;
}

Token Tokenizer::consumeIdentLike()
{
    const int start = m_position;
    QString value;

    while (!atEnd()) {
        const QChar c = peek();
        if (isNameChar(c)) {
            value.append(c);
            ++m_position;
            continue;
        }
        if (c == u'\\' && peek(1) != u'\n') {
            consumeEscape(&value);
            continue;
        }
        break;
    }

    if (peek() == u'(') {
        ++m_position;
        // The url() function has a dedicated token with its own rules, except
        // when it is quoted, in which case it stays a normal function.
        if (value.compare(QLatin1String("url"), Qt::CaseInsensitive) == 0) {
            skipWhitespace();
            if (peek() == u'"' || peek() == u'\'') {
                // A quoted url() is still a url token; its value is the string
                // contents without the quotes.
                const Token stringToken = consumeString(peek());
                skipWhitespace();
                if (peek() == u')')
                    ++m_position;
                Token token;
                token.type = TokenType::Url;
                token.text = m_input.mid(start, m_position - start);
                token.value = stringToken.value;
                return token;
            }
            return consumeUrl();
        }
        return consumeIdentLikeAsFunction(start, value);
    }

    Token token;
    token.type = TokenType::Ident;
    token.text = m_input.mid(start, m_position - start);
    token.value = value;
    return token;
}

Token Tokenizer::consumeIdentLikeAsFunction(int start, const QString &name)
{
    Token token;
    token.type = TokenType::Function;
    token.text = m_input.mid(start, m_position - start);
    token.value = name.toLower();
    return token;
}

Token Tokenizer::consumeNumeric()
{
    const int start = m_position;
    Token token;
    token.type = TokenType::Number;

    QString number;
    if (peek() == u'+' || peek() == u'-') {
        token.hasSign = true;
        number.append(peek());
        ++m_position;
    }
    while (isDigit(peek())) {
        number.append(peek());
        ++m_position;
    }
    if (peek() == u'.' && isDigit(peek(1))) {
        number.append(peek());
        ++m_position;
        while (isDigit(peek())) {
            number.append(peek());
            ++m_position;
        }
    }
    // Scientific notation.
    if ((peek() == u'e' || peek() == u'E')
        && (isDigit(peek(1))
            || ((peek(1) == u'+' || peek(1) == u'-') && isDigit(peek(2))))) {
        number.append(peek());
        ++m_position;
        if (peek() == u'+' || peek() == u'-') {
            number.append(peek());
            ++m_position;
        }
        while (isDigit(peek())) {
            number.append(peek());
            ++m_position;
        }
    }

    token.number = number.toDouble();

    if (peek() == u'%') {
        ++m_position;
        token.type = TokenType::Percentage;
        token.text = m_input.mid(start, m_position - start);
        token.value = token.text;
        return token;
    }

    if (wouldStartIdentifier(0)) {
        QString unit;
        while (!atEnd()) {
            const QChar c = peek();
            if (isNameChar(c)) {
                unit.append(c);
                ++m_position;
                continue;
            }
            if (c == u'\\' && peek(1) != u'\n') {
                consumeEscape(&unit);
                continue;
            }
            break;
        }
        token.type = TokenType::Dimension;
        token.unit = unit.toLower();
        token.text = m_input.mid(start, m_position - start);
        token.value = token.text;
        return token;
    }

    token.text = m_input.mid(start, m_position - start);
    token.value = token.text;
    return token;
}

Token Tokenizer::nextToken()
{
    if (m_hasLookahead) {
        m_hasLookahead = false;
        return m_lookahead;
    }

    if (!m_pushedBack.isEmpty())
        return m_pushedBack.takeFirst();

    if (atEnd()) {
        Token token;
        token.type = TokenType::EndOfFile;
        return token;
    }

    const QChar c = peek();

    // Whitespace runs collapse into a single token (§4.3.1), and comments are
    // swallowed by the same run: they separate tokens without contributing
    // anything themselves, so "a /* c */ b" has exactly one separator.
    if (isWhitespace(c) || (c == u'/' && peek(1) == u'*')) {
        const int start = m_position;
        while (!atEnd()) {
            if (isWhitespace(peek())) {
                ++m_position;
                continue;
            }
            if (peek() == u'/' && peek(1) == u'*') {
                consumeComment();
                continue;
            }
            break;
        }
        Token token;
        token.type = TokenType::Whitespace;
        token.text = m_input.mid(start, m_position - start);
        return token;
    }

    if (c == u'"' || c == u'\'')
        return consumeString(c);

    if (c == u'#') {
        const int start = m_position;
        ++m_position;
        if (isNameChar(peek()) || (peek() == u'\\' && peek(1) != u'\n')) {
            QString value;
            while (!atEnd()) {
                const QChar current = peek();
                if (isNameChar(current)) {
                    value.append(current);
                    ++m_position;
                    continue;
                }
                if (current == u'\\' && peek(1) != u'\n') {
                    consumeEscape(&value);
                    continue;
                }
                break;
            }
            Token token;
            token.type = TokenType::Hash;
            token.text = m_input.mid(start, m_position - start);
            token.value = value;
            // A hash is an ID when its value is a valid identifier.
            token.isId = !value.isEmpty() && (isNameStart(value.at(0))
                                              || value.at(0) == u'-' || value.at(0) == u'_');
            return token;
        }
        Token token;
        token.type = TokenType::Delim;
        token.value = QStringLiteral("#");
        token.text = token.value;
        return token;
    }

    if (c == u'@') {
        const int start = m_position;
        ++m_position;
        if (wouldStartIdentifier(0)) {
            QString value;
            while (!atEnd()) {
                const QChar current = peek();
                if (isNameChar(current)) {
                    value.append(current);
                    ++m_position;
                    continue;
                }
                if (current == u'\\' && peek(1) != u'\n') {
                    consumeEscape(&value);
                    continue;
                }
                break;
            }
            Token token;
            token.type = TokenType::AtKeyword;
            token.text = m_input.mid(start, m_position - start);
            token.value = value.toLower();
            return token;
        }
        Token token;
        token.type = TokenType::Delim;
        token.value = QStringLiteral("@");
        token.text = token.value;
        return token;
    }

    if (wouldStartNumber(0))
        return consumeNumeric();

    if (wouldStartIdentifier(0))
        return consumeIdentLike();

    // Simple one-character tokens.
    const int start = m_position;
    ++m_position;
    Token token;
    token.text = m_input.mid(start, 1);

    switch (c.unicode()) {
    case u':': token.type = TokenType::Colon; break;
    case u';': token.type = TokenType::Semicolon; break;
    case u',': token.type = TokenType::Comma; break;
    case u'[': token.type = TokenType::LeftSquare; break;
    case u']': token.type = TokenType::RightSquare; break;
    case u'(': token.type = TokenType::LeftParen; break;
    case u')': token.type = TokenType::RightParen; break;
    case u'{': token.type = TokenType::LeftCurly; break;
    case u'}': token.type = TokenType::RightCurly; break;
    default:
        token.type = TokenType::Delim;
        token.value = token.text;
        break;
    }

    // CDO and CDC are recognized so that HTML comment wrappers inside <style>
    // are discarded rather than treated as selectors.
    if (token.type == TokenType::Delim && c == u'<' && m_input.mid(start, 4) == QLatin1String("<!--")) {
        m_position = start + 4;
        token.type = TokenType::Cdo;
        token.text = QStringLiteral("<!--");
    } else if (token.type == TokenType::Delim && c == u'-' && m_input.mid(start, 3) == QLatin1String("-->")) {
        m_position = start + 3;
        token.type = TokenType::Cdc;
        token.text = QStringLiteral("-->");
    }

    return token;
}

const Token &Tokenizer::peekToken()
{
    if (!m_hasLookahead) {
        m_lookahead = nextToken();
        m_hasLookahead = true;
    }
    return m_lookahead;
}

void Tokenizer::pushBack(const Token &token)
{
    m_pushedBack.prepend(token);
}

QString Tokenizer::consumeRawValue()
{
    const Token &token = peekToken();
    if (token.type == TokenType::EndOfFile)
        return {};
    const Token consumed = nextToken();
    return consumed.text;
}

QString Tokenizer::readUntilMatchingBrace()
{
    // Assumes the opening '{' has already been consumed.
    const int start = m_position;
    int depth = 1;
    while (!atEnd() && depth > 0) {
        const QChar c = peek();
        if (c == u'{')
            ++depth;
        else if (c == u'}')
            --depth;
        ++m_position;
    }
    const int end = depth == 0 ? m_position - 1 : m_position;
    return m_input.mid(start, end - start);
}

} // namespace oqb::css
