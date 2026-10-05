#pragma once

#include <QString>
#include <QStringList>

namespace oqb::css {

/// The kind of token produced by the CSS tokenizer, following CSS Syntax §4.
enum class TokenType {
    Ident,
    Function,
    AtKeyword,
    Hash,
    String,
    BadString,
    Url,
    BadUrl,
    Delim,
    Number,
    Percentage,
    Dimension,
    Whitespace,
    Colon,
    Semicolon,
    Comma,
    LeftSquare,
    RightSquare,
    LeftParen,
    RightParen,
    LeftCurly,
    RightCurly,
    Cdo,
    Cdc,
    EndOfFile,
};

/// A single CSS token.
///
/// The tokenizer keeps the raw source text alongside the parsed value so that
/// the inspector can show exactly what the author wrote, and so that the parser
/// can compare values without re-serialising them.
struct Token
{
    TokenType type = TokenType::EndOfFile;
    /// The token as it appeared in the source, verbatim.
    QString text;
    /// The decoded value: the identifier, string contents, or URL.
    QString value;
    /// Numeric value for Number, Percentage and Dimension tokens.
    double number = 0;
    /// The unit of a Dimension token, lower-cased.
    QString unit;
    /// True when a Hash token is a valid ID selector (e.g. #main).
    bool isId = false;
    /// True when a Number was written with a leading '+' or '-'.
    bool hasSign = false;

    bool is(TokenType expected) const { return type == expected; }
    bool isIdent(const QString &name) const
    {
        return type == TokenType::Ident && value.compare(name, Qt::CaseInsensitive) == 0;
    }
    bool isDelim(QChar c) const { return type == TokenType::Delim && value == QString(c); }

    /// A human readable name used in error messages.
    QString typeName() const;
};

/// Turns CSS source text into a token stream (CSS Syntax Module Level 3).
///
/// The tokenizer is hand written in the same spirit as the HTML tokenizer: it
/// consumes the input character by character and never throws, so malformed
/// stylesheets degrade into ignorable tokens rather than breaking the page.
class Tokenizer
{
public:
    explicit Tokenizer(const QString &input);

    /// Returns the next token, or an EndOfFile token once the input is spent.
    Token nextToken();

    /// Returns the next token without consuming it.
    const Token &peekToken();

    /// Puts a token back so the next call returns it again.
    void pushBack(const Token &token);

    /// Reads the next token and returns its raw text.
    QString consumeRawValue();

    /// Skips whitespace and comments.
    void skipWhitespace();

    /// The offset in the source, used for diagnostics.
    int position() const { return m_position; }

    /// The source text, so a parser can re-read a component value verbatim.
    const QString &input() const { return m_input; }

    /// Reads a balanced component value starting at the current position, used
    /// for unknown at-rules and declarations this engine does not understand.
    QString readUntilMatchingBrace();

private:
    QChar peek(int offset = 0) const;
    bool atEnd(int offset = 0) const;
    bool wouldStartIdentifier(int offset) const;
    bool wouldStartNumber(int offset) const;

    Token consumeNumeric();
    Token consumeIdentLike();
    Token consumeIdentLikeAsFunction(int start, const QString &name);
    Token consumeString(QChar quote);
    Token consumeUrl();
    Token consumeEscape(QString *out);
    void consumeComment();

    QString m_input;
    int m_position = 0;
    Token m_lookahead;
    bool m_hasLookahead = false;
    QList<Token> m_pushedBack;
};

} // namespace oqb::css
