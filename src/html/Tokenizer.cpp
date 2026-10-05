#include "html/Tokenizer.h"

#include "html/Entities.h"

#include <QRegularExpression>

namespace oqb::html {
namespace {

/// Elements whose content the tokenizer reads as text rather than markup.
Tokenizer::TextMode textModeForTag(const QString &tag)
{
    if (tag == QLatin1String("title") || tag == QLatin1String("textarea"))
        return Tokenizer::TextMode::RcData;
    if (tag == QLatin1String("script"))
        return Tokenizer::TextMode::ScriptData;
    if (tag == QLatin1String("style") || tag == QLatin1String("xmp")
        || tag == QLatin1String("iframe") || tag == QLatin1String("noembed")
        || tag == QLatin1String("noframes") || tag == QLatin1String("plaintext")
        || tag == QLatin1String("noscript")) {
        return Tokenizer::TextMode::RawText;
    }
    return Tokenizer::TextMode::Data;
}
bool isTagNameTerminator(QChar c)
{
    return c.isSpace() || c == u'/' || c == u'>';
}

} // namespace

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

void Tokenizer::skipWhitespace()
{
    while (!atEnd() && m_input.at(m_position).isSpace())
        ++m_position;
}

void Tokenizer::pushBack(const Token &token)
{
    m_pushedBack.prepend(token);
}

Token Tokenizer::makeEof()
{
    Token token;
    token.type = Token::Type::EndOfFile;
    m_eofEmitted = true;
    return token;
}

Token Tokenizer::nextToken()
{
    if (!m_pushedBack.isEmpty())
        return m_pushedBack.takeFirst();

    if (m_textMode != TextMode::Data)
        return readTextUntilEndTag();

    if (m_position >= m_input.size())
        return makeEof();

    return readData();
}

Token Tokenizer::readData()
{
    const QChar c = m_input.at(m_position);

    if (c == u'<') {
        const QChar next = peek(1);
        if (next == u'!') {
            if (m_input.mid(m_position + 1, 3) == QLatin1String("!--"))
                return readComment();
            return readMarkupDeclaration();
        }
        if (next == u'/') {
            // "</" not followed by a letter is a bogus comment.
            const QChar after = peek(2);
            if (after.isLetter())
                return readEndTag();
            if (after == u'>') {
                // "</>" is ignored by the parser; emit an empty comment so the
                // token stream stays aligned with the input.
                m_position += 3;
                Token token;
                token.type = Token::Type::Comment;
                token.data = QString();
                return token;
            }
            return readBogusComment();
        }
        if (next == u'?') {
            return readBogusComment();
        }
        if (next.isLetter()) {
            return readStartTag();
        }

        // A "<" that starts nothing is ordinary text. Emitting it here, and
        // advancing, is what keeps the tokenizer making progress: without this
        // the run reader would return an empty token on the same offset.
        Token token;
        token.type = Token::Type::Character;
        token.data = QStringLiteral("<");
        ++m_position;
        return token;
    }

    return readCharacterRun();
}

Token Tokenizer::readCharacterRun()
{
    const int start = m_position;
    while (m_position < m_input.size() && m_input.at(m_position) != u'<')
        ++m_position;

    // A run is never empty: a caller that sees no progress would loop forever,
    // so if there is nothing to consume the "<" itself is returned as text.
    if (m_position == start && start < m_input.size()) {
        ++m_position;
        Token token;
        token.type = Token::Type::Character;
        token.data = QString(m_input.at(start));
        return token;
    }

    Token token;
    token.type = Token::Type::Character;
    token.data = entities::decode(m_input.mid(start, m_position - start));
    return token;
}

QString Tokenizer::readTagName()
{
    const int start = m_position;
    while (m_position < m_input.size()) {
        const QChar c = m_input.at(m_position);
        if (isTagNameTerminator(c))
            break;
        ++m_position;
    }
    return m_input.mid(start, m_position - start).toLower();
}

bool Tokenizer::readAttributes(QList<TokenAttribute> *attributes, bool *selfClosing)
{
    *selfClosing = false;

    while (!atEnd()) {
        skipWhitespace();
        if (atEnd())
            return false;

        const QChar c = m_input.at(m_position);

        if (c == u'>') {
            ++m_position;
            return true;
        }

        if (c == u'/') {
            if (peek(1) == u'>') {
                m_position += 2;
                *selfClosing = true;
                return true;
            }
            // A stray slash is ignored, as the standard requires.
            ++m_position;
            continue;
        }

        // Attribute name.
        const int nameStart = m_position;
        while (!atEnd()) {
            const QChar current = m_input.at(m_position);
            if (current.isSpace() || current == u'=' || current == u'>'
                || current == u'/') {
                break;
            }
            ++m_position;
        }

        if (m_position == nameStart) {
            // Nothing consumed; avoid spinning on malformed input.
            ++m_position;
            continue;
        }

        TokenAttribute attribute;
        attribute.name = m_input.mid(nameStart, m_position - nameStart).toLower();

        skipWhitespace();
        if (!atEnd() && m_input.at(m_position) == u'=') {
            ++m_position;
            skipWhitespace();

            if (!atEnd()) {
                const QChar quote = m_input.at(m_position);
                if (quote == u'"' || quote == u'\'') {
                    ++m_position;
                    const int valueStart = m_position;
                    while (!atEnd() && m_input.at(m_position) != quote)
                        ++m_position;
                    attribute.value = m_input.mid(valueStart, m_position - valueStart);
                    if (!atEnd())
                        ++m_position; // closing quote
                } else {
                    const int valueStart = m_position;
                    while (!atEnd()) {
                        const QChar current = m_input.at(m_position);
                        if (current.isSpace() || current == u'>')
                            break;
                        ++m_position;
                    }
                    attribute.value = m_input.mid(valueStart, m_position - valueStart);
                }
            }
        }

        attribute.value = entities::decode(attribute.value);

        // Duplicate attributes: the first occurrence wins (§13.2.5.33).
        bool duplicate = false;
        for (const TokenAttribute &existing : *attributes) {
            if (existing.name == attribute.name) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
            attributes->append(attribute);
    }

    return false; // Unterminated tag at EOF.
}

Token Tokenizer::readStartTag()
{
    ++m_position; // consume '<'

    Token token;
    token.type = Token::Type::StartTag;
    token.name = readTagName();

    if (token.name.isEmpty()) {
        // "<" followed by something unexpected: treat as text.
        token.type = Token::Type::Character;
        token.data = QStringLiteral("<");
        return token;
    }

    const bool closed = readAttributes(&token.attributes, &token.selfClosing);
    if (!closed)
        token.selfClosing = false;

    // Switch into a text mode for elements whose content is not markup. Void
    // and self-closing tags never enter a text mode.
    if (!token.selfClosing) {
        const TextMode mode = textModeForTag(token.name);
        if (mode != TextMode::Data) {
            m_textMode = mode;
            m_rawTextTag = token.name;
        }
    }

    return token;
}

Token Tokenizer::readEndTag()
{
    m_position += 2; // consume "</"

    Token token;
    token.type = Token::Type::EndTag;
    token.name = readTagName();

    if (token.name.isEmpty()) {
        token.type = Token::Type::Character;
        token.data = QStringLiteral("</");
        return token;
    }

    // Ignore any attributes; end tags only carry a name.
    QList<TokenAttribute> ignored;
    bool selfClosing = false;
    readAttributes(&ignored, &selfClosing);
    return token;
}

Token Tokenizer::readComment()
{
    m_position += 4; // consume "<!--"

    Token token;
    token.type = Token::Type::Comment;

    const int start = m_position;
    const int end = m_input.indexOf(QStringLiteral("-->"), m_position);
    if (end < 0) {
        token.data = m_input.mid(start);
        m_position = m_input.size();
    } else {
        token.data = m_input.mid(start, end - start);
        m_position = end + 3;
    }
    return token;
}

Token Tokenizer::readBogusComment()
{
    // "<?...>" and "</...>" without a name become a comment up to '>'.
    m_position += peek(1) == u'/' ? 2 : 1;

    Token token;
    token.type = Token::Type::Comment;
    const int end = m_input.indexOf(u'>', m_position);
    if (end < 0) {
        token.data = m_input.mid(m_position);
        m_position = m_input.size();
    } else {
        token.data = m_input.mid(m_position, end - m_position);
        m_position = end + 1;
    }
    return token;
}

Token Tokenizer::readMarkupDeclaration()
{
    if (m_input.mid(m_position + 2, 7).compare(QLatin1String("DOCTYPE"), Qt::CaseInsensitive)
        == 0) {
        return readDoctype();
    }

    // "<![CDATA[" is only special in foreign content, which OpenQBrowser does
    // not implement yet; it becomes a comment.
    return readBogusComment();
}

Token Tokenizer::readDoctype()
{
    m_position += 9; // consume "<!DOCTYPE"
    skipWhitespace();

    Token token;
    token.type = Token::Type::Doctype;

    // Read up to the closing '>'.
    const int end = m_input.indexOf(u'>', m_position);
    const QString content
        = end < 0 ? m_input.mid(m_position) : m_input.mid(m_position, end - m_position);
    m_position = end < 0 ? m_input.size() : end + 1;

    const QString trimmed = content.trimmed();
    const int space = trimmed.indexOf(QRegularExpression(QStringLiteral("\\s")));
    token.name = (space < 0 ? trimmed : trimmed.left(space)).toLower();

    // The only doctype that enables standards mode is a well-formed
    // "<!DOCTYPE html>" with no PUBLIC/SYSTEM identifier.
    if (token.name != QLatin1String("html") || trimmed.contains(QLatin1String("PUBLIC"))
        || trimmed.contains(QLatin1String("SYSTEM"))) {
        token.forceQuirks = true;
    }

    return token;
}

Token Tokenizer::readTextUntilEndTag()
{
    // Locate the matching end tag, case-insensitively, at a tag boundary.
    const QString closing = QStringLiteral("</") + m_rawTextTag;

    int searchFrom = m_position;
    int endTagStart = -1;
    while (searchFrom < m_input.size()) {
        const int found = m_input.indexOf(closing, searchFrom, Qt::CaseInsensitive);
        if (found < 0)
            break;
        const int after = found + closing.size();
        if (after >= m_input.size() || isTagNameTerminator(m_input.at(after))) {
            endTagStart = found;
            break;
        }
        searchFrom = found + 2;
    }

    if (endTagStart < 0) {
        // No end tag: the rest of the input is text, then EOF.
        Token token;
        token.type = Token::Type::Character;
        token.data = m_input.mid(m_position);
        m_position = m_input.size();
        m_textMode = TextMode::Data;
        m_rawTextTag.clear();
        return token;
    }

    if (endTagStart > m_position) {
        Token token;
        token.type = Token::Type::Character;
        token.data = m_input.mid(m_position, endTagStart - m_position);
        // RCDATA (title, textarea) does decode character references.
        if (m_textMode == TextMode::RcData)
            token.data = entities::decode(token.data);
        m_position = endTagStart;
        return token;
    }

    // Position sits on the end tag: leave text mode and tokenize it normally.
    m_textMode = TextMode::Data;
    m_rawTextTag.clear();
    return readEndTag();
}

} // namespace oqb::html
