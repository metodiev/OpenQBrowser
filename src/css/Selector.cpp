#include "css/Selector.h"

#include "dom/Node.h"

#include <QRegularExpression>

namespace oqb::css {

QStringList splitSelectorList(const QString &text)
{
    QStringList parts;
    QString current;
    int depth = 0;
    QChar quote;

    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);

        if (quote.isNull()) {
            if (c == u'"' || c == u'\'') {
                quote = c;
            } else if (c == u'(' || c == u'[') {
                ++depth;
            } else if (c == u')' || c == u']') {
                depth = qMax(0, depth - 1);
            } else if (c == u',' && depth == 0) {
                parts.append(current.trimmed());
                current.clear();
                continue;
            } else if (c == u'\\') {
                current.append(c);
                if (i + 1 < text.size())
                    current.append(text.at(++i));
                continue;
            }
        } else if (c == quote) {
            quote = QChar();
        } else if (c == u'\\') {
            current.append(c);
            if (i + 1 < text.size())
                current.append(text.at(++i));
            continue;
        }

        current.append(c);
    }

    if (!current.trimmed().isEmpty())
        parts.append(current.trimmed());

    return parts;
}

// ------------------------------------------------------- CompoundSelector

bool CompoundSelector::matches(const dom::Element *element) const
{
    if (!element)
        return false;

    // A compound containing a pseudo-element never matches a real element,
    // because OpenQBrowser does not synthesise generated boxes yet.
    if (!pseudoElements.isEmpty())
        return false;

    if (!type.isEmpty() && !element->isTag(type))
        return false;

    for (const QString &id : ids) {
        if (element->id() != id)
            return false;
    }

    if (!classes.isEmpty()) {
        const QStringList elementClasses = element->classList();
        for (const QString &className : classes) {
            if (!elementClasses.contains(className))
                return false;
        }
    }

    for (const AttributeCondition &condition : attributes) {
        if (!element->hasAttribute(condition.name))
            return false;

        const QString actual = element->attribute(condition.name);
        const bool insensitive = condition.caseInsensitive;
        const auto compare = [insensitive](const QString &a, const QString &b) {
            return insensitive ? a.compare(b, Qt::CaseInsensitive) == 0 : a == b;
        };

        switch (condition.match) {
        case AttributeCondition::Match::Exists:
            break;
        case AttributeCondition::Match::Equals:
            if (!compare(actual, condition.value))
                return false;
            break;
        case AttributeCondition::Match::Includes: {
            const QStringList words = actual.split(QRegularExpression(QStringLiteral("\\s+")),
                                                   Qt::SkipEmptyParts);
            bool found = false;
            for (const QString &word : words) {
                if (compare(word, condition.value)) {
                    found = true;
                    break;
                }
            }
            if (!found)
                return false;
            break;
        }
        case AttributeCondition::Match::DashMatch:
            if (!compare(actual, condition.value)
                && !actual.startsWith(condition.value + u'-')) {
                return false;
            }
            break;
        case AttributeCondition::Match::Prefix:
            if (condition.value.isEmpty() || !actual.startsWith(condition.value,
                                                                insensitive ? Qt::CaseInsensitive
                                                                            : Qt::CaseSensitive))
                return false;
            break;
        case AttributeCondition::Match::Suffix:
            if (condition.value.isEmpty() || !actual.endsWith(condition.value,
                                                              insensitive ? Qt::CaseInsensitive
                                                                          : Qt::CaseSensitive))
                return false;
            break;
        case AttributeCondition::Match::Substring:
            if (condition.value.isEmpty() || !actual.contains(condition.value,
                                                              insensitive ? Qt::CaseInsensitive
                                                                          : Qt::CaseSensitive))
                return false;
            break;
        }
    }

    for (const PseudoClass &pseudo : pseudoClasses) {
        const QString &name = pseudo.name;

        if (name == QLatin1String("first-child")) {
            if (element->previousElementSibling())
                return false;
        } else if (name == QLatin1String("last-child")) {
            if (element->nextElementSibling())
                return false;
        } else if (name == QLatin1String("only-child")) {
            if (element->previousElementSibling() || element->nextElementSibling())
                return false;
        } else if (name == QLatin1String("root")) {
            if (element->parentElement())
                return false;
        } else if (name == QLatin1String("empty")) {
            // :empty matches elements with no element or text children; a
            // comment does not count.
            for (const auto &child : element->children()) {
                if (child->isElement() || child->isText())
                    return false;
            }
        } else if (name == QLatin1String("first-of-type")
                   || name == QLatin1String("last-of-type")
                   || name == QLatin1String("only-of-type")) {
            const dom::Element *parent = element->parentElement();
            if (!parent)
                return false;
            int index = 0;
            int count = 0;
            for (const dom::Element *sibling : parent->childElements()) {
                if (!sibling->isTag(element->tagName()))
                    continue;
                if (sibling == element)
                    index = count;
                ++count;
            }
            if (name == QLatin1String("first-of-type") && index != 0)
                return false;
            if (name == QLatin1String("last-of-type") && index != count - 1)
                return false;
            if (name == QLatin1String("only-of-type") && count != 1)
                return false;
        } else if (name == QLatin1String("nth-child") || name == QLatin1String("nth-last-child")
                   || name == QLatin1String("nth-of-type")
                   || name == QLatin1String("nth-last-of-type")) {
            const dom::Element *parent = element->parentElement();
            if (!parent)
                return false;

            const bool ofType = name.endsWith(QLatin1String("of-type"));
            const bool fromEnd = name.startsWith(QLatin1String("nth-last"));

            int index = 0;
            int count = 0;
            const QList<dom::Element *> siblings = parent->childElements();
            for (const dom::Element *sibling : siblings) {
                if (ofType && !sibling->isTag(element->tagName()))
                    continue;
                if (sibling == element)
                    index = count;
                ++count;
            }
            if (fromEnd)
                index = count - 1 - index;

            if (!matchesNth(pseudo.argument, index + 1))
                return false;
        } else if (name == QLatin1String("not")) {
            // :not() takes a selector list; the element matches when it matches
            // none of them. Only compound selectors are supported, matching the
            // Level 3 grammar.
            const QList<Selector> inner = SelectorParser::parseList(pseudo.argument);
            if (inner.isEmpty())
                return false;
            for (const Selector &selector : inner) {
                if (selector.matches(element))
                    return false;
            }
        } else if (name == QLatin1String("is") || name == QLatin1String("where")
                   || name == QLatin1String("matches")) {
            const QList<Selector> inner = SelectorParser::parseList(pseudo.argument);
            bool matched = false;
            for (const Selector &selector : inner) {
                if (selector.matches(element)) {
                    matched = true;
                    break;
                }
            }
            if (!matched)
                return false;
        } else if (name == QLatin1String("link") || name == QLatin1String("any-link")) {
            if (!element->hasAttribute(QStringLiteral("href")))
                return false;
        } else {
            // Unknown pseudo-classes make the compound fail to match, which is
            // what the specification requires for unsupported selectors.
            return false;
        }
    }

    return true;
}

bool matchesNth(const QString &expression, int index)
{
    // Accepts An+B: "2n+1", "odd", "even", "-n+3", "5".
    QString text = expression.trimmed().toLower().remove(QRegularExpression(QStringLiteral("\\s+")));

    if (text.isEmpty())
        return false;
    if (text == QLatin1String("odd"))
        return index % 2 == 1;
    if (text == QLatin1String("even"))
        return index % 2 == 0;

    const int nPosition = text.indexOf(u'n');
    if (nPosition < 0) {
        bool ok = false;
        const int value = text.toInt(&ok);
        return ok && value == index;
    }

    const QString aText = text.left(nPosition);
    const QString bText = text.mid(nPosition + 1);

    int a = 0;
    if (aText.isEmpty() || aText == QLatin1String("+"))
        a = 1;
    else if (aText == QLatin1String("-"))
        a = -1;
    else
        a = aText.toInt();

    int b = 0;
    if (!bText.isEmpty())
        b = bText.toInt();

    if (a == 0)
        return index == b;

    const int remainder = index - b;
    if (remainder % a != 0)
        return false;
    return remainder / a >= 0;
}

QString CompoundSelector::toString() const
{
    QString out;
    if (universal)
        out += u'*';
    out += type;
    for (const QString &id : ids)
        out += u'#' + id;
    for (const QString &className : classes)
        out += u'.' + className;
    for (const AttributeCondition &condition : attributes) {
        out += u'[' + condition.name;
        switch (condition.match) {
        case AttributeCondition::Match::Exists: break;
        case AttributeCondition::Match::Equals: out += u"="; break;
        case AttributeCondition::Match::Includes: out += QStringLiteral("~="); break;
        case AttributeCondition::Match::DashMatch: out += QStringLiteral("|="); break;
        case AttributeCondition::Match::Prefix: out += QStringLiteral("^="); break;
        case AttributeCondition::Match::Suffix: out += QStringLiteral("$="); break;
        case AttributeCondition::Match::Substring: out += QStringLiteral("*="); break;
        }
        if (condition.match != AttributeCondition::Match::Exists)
            out += u'"' + condition.value + u'"';
        out += u']';
    }
    for (const PseudoClass &pseudo : pseudoClasses) {
        out += u':';
        out += pseudo.name;
        if (!pseudo.argument.isEmpty())
            out += u'(' + pseudo.argument + u')';
    }
    for (const QString &pseudo : pseudoElements) {
        out += QStringLiteral("::");
        out += pseudo;
    }
    return out;
}

// --------------------------------------------------------------- Selector

quint32 Selector::specificity() const
{
    // Packed so that a simple integer comparison orders selectors correctly:
    // ids dominate classes, which dominate types (CSS Cascade §6.4.3).
    return static_cast<quint32>(specificityA) * 10000u
        + static_cast<quint32>(specificityB) * 100u
        + static_cast<quint32>(specificityC);
}

bool Selector::matches(const dom::Element *element) const
{
    if (!isValid() || !element)
        return false;

    // Walk the selector right to left: the first step is the subject, and each
    // following step must be satisfied by an ancestor or sibling.
    return matchSteps(element, 0);
}

bool Selector::matchSteps(const dom::Element *element, int stepIndex) const
{
    const Step &step = steps.at(stepIndex);
    if (!step.compound.matches(element))
        return false;

    if (stepIndex + 1 >= steps.size())
        return true;

    // step.combinator records how this compound relates to the compound to its
    // left, which after reversal is steps[stepIndex + 1].
    switch (step.combinator) {
    case Combinator::Descendant: {
        for (const dom::Element *ancestor = element->parentElement(); ancestor;
             ancestor = ancestor->parentElement()) {
            if (matchSteps(ancestor, stepIndex + 1))
                return true;
        }
        return false;
    }
    case Combinator::Child: {
        const dom::Element *parent = element->parentElement();
        return parent && matchSteps(parent, stepIndex + 1);
    }
    case Combinator::Adjacent: {
        const dom::Element *sibling = element->previousElementSibling();
        return sibling && matchSteps(sibling, stepIndex + 1);
    }
    case Combinator::General: {
        for (const dom::Element *sibling = element->previousElementSibling(); sibling;
             sibling = sibling->previousElementSibling()) {
            if (matchSteps(sibling, stepIndex + 1))
                return true;
        }
        return false;
    }
    case Combinator::None:
        // A missing combinator between two compounds cannot be satisfied.
        return false;
    }
    return false;
}

// -------------------------------------------------------- SelectorParser

namespace {

/// Reads a CSS identifier from `text` starting at `position`.
QString readIdentifier(const QString &text, int *position)
{
    QString out;
    int i = *position;

    while (i < text.size()) {
        const QChar c = text.at(i);
        if (c.isLetterOrNumber() || c == u'-' || c == u'_' || c.unicode() >= 0x80) {
            out.append(c);
            ++i;
            continue;
        }
        if (c == u'\\' && i + 1 < text.size()) {
            // Escaped character: the backslash and the next character are
            // taken literally, which covers class names such as ".sm\:block".
            out.append(text.at(i + 1));
            i += 2;
            continue;
        }
        break;
    }

    *position = i;
    return out;
}

/// Reads a balanced parenthesised argument, assuming `position` is on '('.
QString readParenthesised(const QString &text, int *position)
{
    int i = *position;
    if (i >= text.size() || text.at(i) != u'(')
        return {};

    int depth = 0;
    const int start = i + 1;
    QChar quote;

    for (; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (!quote.isNull()) {
            if (c == u'\\') {
                ++i;
                continue;
            }
            if (c == quote)
                quote = QChar();
            continue;
        }
        if (c == u'"' || c == u'\'') {
            quote = c;
            continue;
        }
        if (c == u'(') {
            ++depth;
            continue;
        }
        if (c == u')') {
            --depth;
            if (depth == 0) {
                *position = i + 1;
                return text.mid(start, i - start);
            }
        }
    }

    *position = i;
    return text.mid(start);
}

/// Reads a quoted or bare attribute value, assuming `position` is on it.
QString readAttributeValue(const QString &text, int *position)
{
    int i = *position;
    if (i >= text.size())
        return {};

    const QChar quote = text.at(i);
    if (quote == u'"' || quote == u'\'') {
        ++i;
        QString value;
        while (i < text.size() && text.at(i) != quote) {
            if (text.at(i) == u'\\' && i + 1 < text.size()) {
                value.append(text.at(++i));
                ++i;
                continue;
            }
            value.append(text.at(i));
            ++i;
        }
        if (i < text.size())
            ++i;
        *position = i;
        return value;
    }

    return readIdentifier(text, position);
}

} // namespace

Selector SelectorParser::parse(const QString &text, QString *error)
{
    Selector selector;
    selector.source = text.trimmed();

    const QString source = selector.source;
    if (source.isEmpty()) {
        if (error)
            *error = QStringLiteral("empty selector");
        return selector;
    }

    // Parse into a temporary left-to-right list first, then reverse.
    QList<Selector::Step> leftToRight;

    int i = 0;
    Selector::Combinator pendingCombinator = Selector::Combinator::None;
    bool havePending = false;

    while (i < source.size()) {
        const QChar c = source.at(i);

        // Whitespace implies a descendant combinator only when no combinator is
        // already pending; an explicit '>', '+' or '~' that follows the
        // whitespace takes precedence over it.
        if (c.isSpace()) {
            while (i < source.size() && source.at(i).isSpace())
                ++i;
            if (i < source.size() && source.at(i) != u',' && !havePending) {
                pendingCombinator = Selector::Combinator::Descendant;
                havePending = true;
            }
            continue;
        }

        if (c == u'>' || c == u'+' || c == u'~') {
            pendingCombinator = c == u'>' ? Selector::Combinator::Child
                : c == u'+'             ? Selector::Combinator::Adjacent
                                        : Selector::Combinator::General;
            havePending = true;
            ++i;
            continue;
        }

        CompoundSelector compound;
        bool parsedSomething = false;

        while (i < source.size() && !source.at(i).isSpace()) {
            const QChar current = source.at(i);

            if (current == u'*') {
                compound.universal = true;
                ++i;
                parsedSomething = true;
                continue;
            }

            if (current == u'#') {
                ++i;
                const QString id = readIdentifier(source, &i);
                if (id.isEmpty())
                    break;
                compound.ids.append(id);
                parsedSomething = true;
                continue;
            }

            if (current == u'.') {
                ++i;
                const QString className = readIdentifier(source, &i);
                if (className.isEmpty())
                    break;
                compound.classes.append(className);
                parsedSomething = true;
                continue;
            }

            if (current == u'[') {
                ++i;
                CompoundSelector::AttributeCondition condition;
                condition.name = readIdentifier(source, &i).toLower();

                // Operators, longest first so that "~=" is not read as "~".
                const QString remainder = source.mid(i);
                if (remainder.startsWith(QLatin1String("~="))) {
                    condition.match = CompoundSelector::AttributeCondition::Match::Includes;
                    i += 2;
                } else if (remainder.startsWith(QLatin1String("|="))) {
                    condition.match = CompoundSelector::AttributeCondition::Match::DashMatch;
                    i += 2;
                } else if (remainder.startsWith(QLatin1String("^="))) {
                    condition.match = CompoundSelector::AttributeCondition::Match::Prefix;
                    i += 2;
                } else if (remainder.startsWith(QLatin1String("$="))) {
                    condition.match = CompoundSelector::AttributeCondition::Match::Suffix;
                    i += 2;
                } else if (remainder.startsWith(QLatin1String("*="))) {
                    condition.match = CompoundSelector::AttributeCondition::Match::Substring;
                    i += 2;
                } else if (remainder.startsWith(u'=')) {
                    condition.match = CompoundSelector::AttributeCondition::Match::Equals;
                    i += 1;
                }

                while (i < source.size() && source.at(i).isSpace())
                    ++i;

                if (condition.match != CompoundSelector::AttributeCondition::Match::Exists)
                    condition.value = readAttributeValue(source, &i);

                while (i < source.size() && source.at(i).isSpace())
                    ++i;

                // The optional case-sensitivity flag, e.g. [href$=".pdf" i].
                if (i < source.size() && (source.at(i) == u'i' || source.at(i) == u'I')
                    && i + 1 < source.size() && source.at(i + 1) == u']') {
                    condition.caseInsensitive = true;
                    ++i;
                }

                if (i < source.size() && source.at(i) == u']')
                    ++i;

                compound.attributes.append(condition);
                parsedSomething = true;
                continue;
            }

            if (current == u':') {
                ++i;
                bool pseudoElement = false;
                if (i < source.size() && source.at(i) == u':') {
                    pseudoElement = true;
                    ++i;
                }

                const QString name = readIdentifier(source, &i).toLower();
                QString argument;
                if (i < source.size() && source.at(i) == u'(')
                    argument = readParenthesised(source, &i);

                if (pseudoElement)
                    compound.pseudoElements.append(name);
                else
                    compound.pseudoClasses.append({name, argument});
                parsedSomething = true;
                continue;
            }

            // A type selector, which must come first in a compound.
            if (compound.type.isEmpty() && !compound.universal
                && (current.isLetter() || current == u'_' || current == u'-'
                    || current.unicode() >= 0x80 || current == u'\\')) {
                const QString type = readIdentifier(source, &i);
                if (type.isEmpty())
                    break;
                // An "|-" namespace separator is accepted and ignored: with no
                // namespace declarations every element is in the default one.
                if (i < source.size() && source.at(i) == u'|') {
                    ++i;
                    const QString local = readIdentifier(source, &i);
                    compound.type = (local.isEmpty() ? type : local).toLower();
                } else {
                    compound.type = type.toLower();
                    if (type == QLatin1String("*"))
                        compound.universal = true;
                }
                parsedSomething = true;
                continue;
            }

            break;
        }

        if (!parsedSomething) {
            if (error)
                *error = QStringLiteral("unsupported selector syntax near \"%1\"")
                             .arg(source.mid(i, 20));
            return selector;
        }

        Selector::Step step;
        step.compound = compound;
        step.combinator = havePending ? pendingCombinator : Selector::Combinator::None;
        leftToRight.append(step);
        pendingCombinator = Selector::Combinator::None;
        havePending = false;
    }

    if (leftToRight.isEmpty()) {
        if (error)
            *error = QStringLiteral("selector contains no compounds");
        return selector;
    }

    // Convert to right-to-left order: steps.first() is the subject. Each step
    // keeps the combinator that preceded its compound in the source, which
    // after reversal connects it to the step on its left.
    for (int index = leftToRight.size() - 1; index >= 0; --index)
        selector.steps.append(leftToRight.at(index));

    // Specificity.
    for (const Selector::Step &step : std::as_const(leftToRight)) {
        selector.specificityA += static_cast<int>(step.compound.ids.size());
        selector.specificityB += static_cast<int>(step.compound.classes.size())
            + static_cast<int>(step.compound.attributes.size())
            + static_cast<int>(step.compound.pseudoClasses.size());
        if (!step.compound.type.isEmpty())
            ++selector.specificityC;
        selector.specificityC += static_cast<int>(step.compound.pseudoElements.size());
    }

    return selector;
}

QList<Selector> SelectorParser::parseList(const QString &text, QStringList *unsupported)
{
    QList<Selector> selectors;

    const QStringList parts = splitSelectorList(text);
    for (const QString &part : parts) {
        QString error;
        Selector selector = SelectorParser::parse(part, &error);
        if (selector.isValid()) {
            selectors.append(selector);
        } else if (unsupported) {
            unsupported->append(part);
        }
    }

    return selectors;
}

} // namespace oqb::css
