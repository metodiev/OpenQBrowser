#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include "css/Selector.h"
#include "css/Value.h"

namespace oqb::css {

/// A single declaration, e.g. "color: red".
struct Declaration
{
    QString property; ///< Lower-cased property name.
    Value value;
    bool important = false;
    /// The declaration as written, for the DevTools inspector.
    QString source;

    bool isValid() const { return !property.isEmpty() && value.isValid(); }
};

/// A style rule: a selector list plus a declaration block.
struct StyleRule
{
    QList<Selector> selectors;
    QList<Declaration> declarations;
    /// The media query that guards this rule, or empty for unconditional rules.
    QString mediaQuery;
    /// Index of the stylesheet this rule came from, for cascade ordering.
    int origin = 0;
    /// Order in the document, used to break specificity ties.
    int order = 0;

    QString selectorText() const;
};

/// An @media / @supports block, kept as a nested rule list.
struct AtRule
{
    QString name;      ///< "media", "supports", "font-face" and so on.
    QString prelude;   ///< The text between the name and the block.
    QList<StyleRule> rules;
};

/// A parsed stylesheet.
///
/// Parsing is deliberately forgiving: an unparsable rule is skipped and the rest
/// of the sheet is still used, which is what makes real-world CSS work.
class Stylesheet
{
public:
    /// Parses `source`. `origin` and `baseOrder` let the cascade date rules
    /// across multiple sheets; `errors` receives diagnostics for the inspector.
    static Stylesheet parse(const QString &source, int origin = 0, int baseOrder = 0,
                            QStringList *errors = nullptr);

    /// Parses a bare declaration list, as found in a style attribute:
    /// "color: red; margin: 0" has no selector and no braces. The result is a
    /// single rule with no selectors, which the cascade applies directly.
    static QList<Declaration> parseDeclarationList(const QString &source,
                                                   QStringList *errors = nullptr);

    const QList<StyleRule> &rules() const { return m_rules; }
    const QList<AtRule> &atRules() const { return m_atRules; }
    const QString &sourceUrl() const { return m_sourceUrl; }
    void setSourceUrl(const QString &url) { m_sourceUrl = url; }

    bool isEmpty() const { return m_rules.isEmpty() && m_atRules.isEmpty(); }
    int ruleCount() const;

    /// Every rule from this sheet and its at-rules whose media query matches.
    QList<StyleRule> rulesForMedia(const QString &mediaType, double viewportWidth) const;

private:
    QList<StyleRule> m_rules;
    QList<AtRule> m_atRules;
    QString m_sourceUrl;
};

/// Evaluates a media query list against a viewport (Media Queries Level 3).
///
/// Only the features OpenQBrowser can answer are supported: width, height,
/// min/max variants of both, and the orientation and prefers-color-scheme
/// features. Unsupported features evaluate to false, which is the safe default.
class MediaQuery
{
public:
    /// True when `query` matches a viewport of the given size.
    static bool matches(const QString &query, double viewportWidth, double viewportHeight,
                        bool prefersDark = false);

    /// True when the query list is understood well enough to be evaluated.
    static bool isSupported(const QString &query);
};

} // namespace oqb::css
