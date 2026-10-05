#pragma once

#include <QString>

#include <memory>

#include "dom/Document.h"
#include "network/Url.h"

namespace oqb::html {

/// Options controlling tree construction.
struct ParseOptions
{
    /// Maximum element nesting depth. Documents deeper than this are truncated,
    /// which keeps hostile input from exhausting the stack.
    int maxDepth = 400;
};

/// The result of parsing a document.
struct ParseResult
{
    std::unique_ptr<dom::Document> document;
    int tokenCount = 0;
    /// Diagnostics for the inspector: unclosed tags, stray end tags and so on.
    QStringList warnings;

    bool ok() const { return document != nullptr; }
};

/// Builds a DOM tree from HTML source.
///
/// The implementation follows the insertion-mode structure of the HTML standard
/// (§13.2.6) closely enough to build the same tree as a browser for real-world
/// documents, including the implied <html>, <head> and <body> elements, the
/// auto-closing behaviour of <p>, <li> and table cells, and text handling inside
/// <script> and <style>. It is intentionally not a complete implementation of
/// the specification's error recovery.
class Parser
{
public:
    /// Parses `html` as a document located at `url`.
    static ParseResult parse(const QString &html, const oqb::network::Url &url = {},
                             const ParseOptions &options = {});

    /// Parses an HTML fragment and returns the nodes it produced. Used by the
    /// innerHTML-style APIs and by tests.
    static QList<dom::Node *> parseFragment(const QString &html, dom::Document *document,
                                            const QString &contextTag = QStringLiteral("div"));
};

} // namespace oqb::html
