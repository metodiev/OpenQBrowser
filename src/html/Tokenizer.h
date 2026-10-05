#pragma once

#include <QList>
#include <QString>

namespace oqb::html {

/// One attribute of a start tag.
struct TokenAttribute
{
    QString name;
    QString value;
};

/// A token produced by the tokenizer, following the categories in the HTML
/// standard (§13.2.5): character, comment, doctype, start tag, end tag, EOF.
struct Token
{
    enum class Type {
        Character,
        Comment,
        Doctype,
        StartTag,
        EndTag,
        EndOfFile,
    };

    Type type = Type::EndOfFile;
    QString name;                 ///< Tag name, comment data or doctype name.
    QString data;                 ///< Text for character tokens.
    QList<TokenAttribute> attributes;
    bool selfClosing = false;     ///< The start tag ended with "/>".
    bool forceQuirks = false;     ///< A doctype that cannot enable standards mode.

    bool isStartTag(const QString &tag) const
    {
        return type == Type::StartTag && name == tag;
    }
    bool isEndTag(const QString &tag) const { return type == Type::EndTag && name == tag; }
};

/// Context-sensitive tokenizer.
///
/// The parser drives the tokenizer: after a start tag for <script>, <style>,
/// <title> or <textarea> the tokenizer switches into the corresponding raw text
/// mode, which is how the standard models these elements as well.
class Tokenizer
{
public:
    /// The text modes of §13.2.5, minus the states that are not observable
    /// through this API.
    enum class TextMode {
        Data,      ///< Normal HTML content; character references are decoded.
        RcData,    ///< <title> and <textarea>: no tags, but references decode.
        RawText,   ///< <style> and friends: text up to the matching end tag.
        ScriptData ///< <script>: like raw text in this implementation.
    };

    explicit Tokenizer(const QString &input);

    /// Produces the next token. Returns an EndOfFile token once input is spent.
    Token nextToken();

    /// Returns a token that has already been produced, so the parser can look
    /// ahead without losing it.
    void pushBack(const Token &token);

    /// The position in the input, for error reporting.
    int position() const { return m_position; }

    bool hasMoreInput() const { return m_position < m_input.size() || !m_pushedBack.isEmpty(); }

private:
    Token readData();
    Token readCharacterRun();
    Token readStartTag();
    Token readEndTag();
    Token readMarkupDeclaration();
    Token readBogusComment();
    Token readComment();
    Token readDoctype();
    Token readTextUntilEndTag();
    Token makeEof();

    QChar peek(int offset = 0) const;
    bool atEnd(int offset = 0) const;
    void skipWhitespace();

    /// Reads a tag name starting at the current position.
    QString readTagName();
    /// Reads attributes until '>' or "/>"; assumes the position is just past the
    /// tag name.
    bool readAttributes(QList<TokenAttribute> *attributes, bool *selfClosing);

    QString m_input;
    int m_position = 0;
    TextMode m_textMode = TextMode::Data;
    QString m_rawTextTag; ///< The tag whose end tag terminates the current mode.
    QList<Token> m_pushedBack;
    bool m_eofEmitted = false;
};

} // namespace oqb::html
