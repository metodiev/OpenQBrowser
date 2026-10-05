#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace oqb::dom {
class Element;
} // namespace oqb::dom

namespace oqb::css {

/// One compound selector: a type/universal selector plus any number of class,
/// id, attribute and pseudo-class conditions, all of which must match the same
/// element.
struct CompoundSelector
{
    /// The type selector, lower-cased, or empty for the universal selector.
    QString type;
    bool universal = false;

    struct AttributeCondition
    {
        enum class Match { Exists, Equals, Includes, DashMatch, Prefix, Suffix, Substring };

        QString name;
        QString value;
        Match match = Match::Exists;
        bool caseInsensitive = false;
    };

    struct PseudoClass
    {
        QString name;     ///< Lower-cased, e.g. "first-child".
        QString argument; ///< Raw argument for functional pseudo-classes.
    };

    QStringList classes;
    QStringList ids;
    QList<AttributeCondition> attributes;
    QList<PseudoClass> pseudoClasses;
    /// Pseudo-elements such as ::before are parsed but never match, because
    /// OpenQBrowser does not generate them yet.
    QStringList pseudoElements;

    bool isEmpty() const
    {
        return type.isEmpty() && !universal && classes.isEmpty() && ids.isEmpty()
            && attributes.isEmpty() && pseudoClasses.isEmpty() && pseudoElements.isEmpty();
    }

    /// Tests this compound against a single element, ignoring combinators.
    bool matches(const dom::Element *element) const;

    QString toString() const;
};

/// A complete complex selector: a target compound plus the combinators and
/// compounds to its left, stored right to left so matching can walk up the tree.
///
/// "div.note > p em" becomes [em] --descendant--> [p] --child--> [div.note]
struct Selector
{
    enum class Combinator {
        None,       ///< The combinator to the left of the right-most compound.
        Descendant, ///< " "
        Child,      ///< ">"
        Adjacent,   ///< "+"
        General,    ///< "~"
    };

    struct Step
    {
        CompoundSelector compound;
        /// How this compound connects to the compound to its right.
        Combinator combinator = Combinator::None;
    };

    /// Compounds ordered right to left; steps.first() is the subject.
    QList<Step> steps;

    /// Specificity parts from CSS Cascade §6.4.1.
    int specificityA = 0; ///< id selectors
    int specificityB = 0; ///< class, attribute and pseudo-class selectors
    int specificityC = 0; ///< type selectors and pseudo-elements

    /// The original text, for the inspector.
    QString source;

    bool isValid() const { return !steps.isEmpty(); }

    /// Tests the whole selector against `element`.
    bool matches(const dom::Element *element) const;

    /// The packed specificity as one sortable integer.
    quint32 specificity() const;

    QString toString() const { return source; }

private:
    /// Recursive helper: `element` must satisfy steps[stepIndex] and its
    /// left-hand context.
    bool matchSteps(const dom::Element *element, int stepIndex) const;
};

/// Parses selector lists such as "p.note, a:hover".
class SelectorParser
{
public:
    /// Parses `text` into the selectors it contains. Selectors this engine does
    /// not support are reported through `unsupported`, which the inspector
    /// shows; the supported ones are still used.
    static QList<Selector> parseList(const QString &text, QStringList *unsupported = nullptr);

    /// Parses a single complex selector; the result is invalid on error.
    static Selector parse(const QString &text, QString *error = nullptr);
};

/// Splits a selector list on top-level commas, ignoring commas inside
/// parentheses, brackets, strings and escapes.
QStringList splitSelectorList(const QString &text);

/// Evaluates an An+B expression against a 1-based index, as used by the
/// :nth-child() family of pseudo-classes.
bool matchesNth(const QString &expression, int index);

} // namespace oqb::css
