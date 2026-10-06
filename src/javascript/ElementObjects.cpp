#include "javascript/BindingsInternal.h"

#ifdef OPENQBROWSER_SCRIPTING

namespace oqb::javascript::detail {
namespace {

// -------------------------------------------------------------- classList

/// The classList object: a live view of the class attribute, so a change made
/// through classList shows up in className and the other way round.
///
/// It holds the element rather than a copy of the names, which is what makes it
/// live and what keeps it correct when the attribute changes behind its back.
struct ClassList
{
    dom::Element *element = nullptr;
};

ClassList *classListOf(JSValueConst value)
{
    return static_cast<ClassList *>(opaqueOf(value));
}

void finalizeClassList(JSRuntime *runtime, JSValueConst value)
{
    Q_UNUSED(runtime);
    delete classListOf(value);
}

/// Rewrites the class attribute from a list of names, trimming each and dropping
/// duplicates, so the attribute cannot accumulate stray whitespace.
void writeClasses(JSContext *context, ClassList *list, const QStringList &classes)
{
    QStringList cleaned;
    for (const QString &name : classes) {
        const QString trimmed = name.trimmed();
        if (!trimmed.isEmpty() && !cleaned.contains(trimmed))
            cleaned.append(trimmed);
    }
    list->element->attr().set(QStringLiteral("class"), cleaned.join(u' '));
    touchDocument(context);
}

JSValue classListGetLength(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    ClassList *list = classListOf(thisValue);
    if (!list || !list->element)
        return JS_EXCEPTION;
    return JS_NewInt32(context, static_cast<int>(list->element->classList().size()));
}

JSValue classListGetValue(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    ClassList *list = classListOf(thisValue);
    if (!list || !list->element)
        return JS_EXCEPTION;
    return newString(context, list->element->className());
}

JSValue classListContains(JSContext *context, JSValueConst thisValue, int argc, JSValueConst *argv)
{
    ClassList *list = classListOf(thisValue);
    if (!list || !list->element)
        return JS_EXCEPTION;

    if (argc < 1)
        return JS_NewBool(context, false);
    return JS_NewBool(context, list->element->classList().contains(stringValue(context, argv[0])));
}

JSValue classListAdd(JSContext *context, JSValueConst thisValue, int argc, JSValueConst *argv)
{
    ClassList *list = classListOf(thisValue);
    if (!list || !list->element)
        return JS_EXCEPTION;

    QStringList classes = list->element->classList();
    for (int i = 0; i < argc; ++i)
        classes.append(stringValue(context, argv[i]));

    writeClasses(context, list, classes);
    return JS_UNDEFINED;
}

JSValue classListRemove(JSContext *context, JSValueConst thisValue, int argc, JSValueConst *argv)
{
    ClassList *list = classListOf(thisValue);
    if (!list || !list->element)
        return JS_EXCEPTION;

    QStringList classes = list->element->classList();
    for (int i = 0; i < argc; ++i)
        classes.removeAll(stringValue(context, argv[i]));

    writeClasses(context, list, classes);
    return JS_UNDEFINED;
}

JSValue classListToggle(JSContext *context, JSValueConst thisValue, int argc, JSValueConst *argv)
{
    ClassList *list = classListOf(thisValue);
    if (!list || !list->element)
        return JS_EXCEPTION;

    if (argc < 1) {
        return throwDomError(context, QStringLiteral("TypeError"),
                             QStringLiteral("toggle needs a class name"));
    }

    const QString name = stringValue(context, argv[0]);
    QStringList classes = list->element->classList();

    bool add = !classes.contains(name);
    // A second argument states which way to go rather than inverting, which the
    // standard allows and script uses to make the outcome explicit.
    if (argc > 1)
        add = boolValue(context, argv[1]);

    if (add)
        classes.append(name);
    else
        classes.removeAll(name);

    writeClasses(context, list, classes);
    return JS_NewBool(context, add);
}

JSValue classListReplace(JSContext *context, JSValueConst thisValue, int argc, JSValueConst *argv)
{
    ClassList *list = classListOf(thisValue);
    if (!list || !list->element)
        return JS_EXCEPTION;

    if (argc < 2) {
        return throwDomError(context, QStringLiteral("TypeError"),
                             QStringLiteral("replace needs a name and a replacement"));
    }

    const QString from = stringValue(context, argv[0]);
    const QString to = stringValue(context, argv[1]);

    QStringList classes = list->element->classList();
    const int index = static_cast<int>(classes.indexOf(from));
    if (index < 0)
        return JS_NewBool(context, false);

    classes[index] = to;
    writeClasses(context, list, classes);
    return JS_NewBool(context, true);
}

JSValue classListItem(JSContext *context, JSValueConst thisValue, int argc, JSValueConst *argv)
{
    ClassList *list = classListOf(thisValue);
    if (!list || !list->element)
        return JS_EXCEPTION;

    const QStringList classes = list->element->classList();
    const int index = argc > 0 ? intValue(context, argv[0]) : 0;

    // Out of range yields null, which is the standard's indexed getter and what
    // script tests against null.
    if (index < 0 || index >= classes.size())
        return JS_NULL;
    return newString(context, classes.at(index));
}

/// classList is callable as well as indexable: `classList(0)` returns the first
/// name, which the DOMTokenList legacy callable interface allows.
JSValue classListCall(JSContext *context, JSValueConst thisValue, int argc, JSValueConst *argv,
                      int, int)
{
    return classListItem(context, thisValue, argc, argv);
}

// ------------------------------------------------------------------ style

/// The `style` object: a live view of the style attribute, so that
/// `el.style.color = "red"` writes `color: red` into the attribute and the
/// cascade picks it up on the next layout.
struct StyleDeclaration
{
    dom::Element *element = nullptr;
};

StyleDeclaration *styleOf(JSValueConst value)
{
    return static_cast<StyleDeclaration *>(opaqueOf(value));
}

void finalizeStyleDeclaration(JSRuntime *runtime, JSValueConst value)
{
    Q_UNUSED(runtime);
    delete styleOf(value);
}

/// Splits a style attribute into name/value pairs so that a property the script
/// does not touch survives a change to another one. A semicolon inside a value,
/// as in a data URL, would split wrongly; the declarations this browser supports
/// do not contain one.
QList<QPair<QString, QString>> parseInlineStyle(const QString &text)
{
    QList<QPair<QString, QString>> declarations;
    for (const QString &part : text.split(u';', Qt::SkipEmptyParts)) {
        const int colon = part.indexOf(u':');
        if (colon <= 0)
            continue;
        declarations.append({part.left(colon).trimmed().toLower(), part.mid(colon + 1).trimmed()});
    }
    return declarations;
}

QString serializeInlineStyle(const QList<QPair<QString, QString>> &declarations)
{
    QStringList parts;
    for (const auto &declaration : declarations)
        parts.append(declaration.first + QStringLiteral(": ") + declaration.second);
    return parts.join(QStringLiteral("; "));
}

QList<QPair<QString, QString>> readStyle(const dom::Element *element)
{
    return parseInlineStyle(element->attribute(QStringLiteral("style")));
}

void writeDeclarations(dom::Element *element, const QList<QPair<QString, QString>> &declarations)
{
    element->attr().set(QStringLiteral("style"), serializeInlineStyle(declarations));
}

void setStyleProperty(dom::Element *element, const QString &property, const QString &value)
{
    QList<QPair<QString, QString>> declarations = readStyle(element);

    for (auto &declaration : declarations) {
        if (declaration.first == property) {
            // An existing declaration keeps its position, so the order the page
            // wrote is preserved and later declarations still win.
            declaration.second = value;
            writeDeclarations(element, declarations);
            return;
        }
    }

    declarations.append({property, value});
    writeDeclarations(element, declarations);
}

QString stylePropertyOf(const dom::Element *element, const QString &property)
{
    for (const auto &declaration : readStyle(element)) {
        if (declaration.first == property)
            return declaration.second;
    }
    return {};
}

void removeStyleProperty(dom::Element *element, const QString &property)
{
    QList<QPair<QString, QString>> declarations = readStyle(element);
    for (int i = static_cast<int>(declarations.size()) - 1; i >= 0; --i) {
        if (declarations.at(i).first == property)
            declarations.removeAt(i);
    }
    writeDeclarations(element, declarations);
}

/// camelCase to the dashed form the style attribute stores: backgroundColor
/// becomes background-color.
QString dashedProperty(const QString &name)
{
    QString out;
    for (const QChar c : name) {
        if (c.isUpper()) {
            out += u'-';
            out += c.toLower();
        } else {
            out += c;
        }
    }
    return out;
}

/// The reverse, so a dashed property can be exposed as a camelCase member.
QString camelProperty(const QString &dashed)
{
    QString out;
    bool upperNext = false;
    for (const QChar c : dashed) {
        if (c == u'-') {
            upperNext = true;
            continue;
        }
        out += upperNext ? c.toUpper() : c;
        upperNext = false;
    }
    return out;
}

JSValue styleGetProperty(JSContext *context, JSValueConst thisValue, int argc, JSValueConst *argv)
{
    StyleDeclaration *style = styleOf(thisValue);
    if (!style || !style->element)
        return JS_EXCEPTION;

    if (argc < 1)
        return newString(context, QString());
    return newString(context, stylePropertyOf(style->element, stringValue(context, argv[0])));
}

JSValue styleSetProperty(JSContext *context, JSValueConst thisValue, int argc, JSValueConst *argv)
{
    StyleDeclaration *style = styleOf(thisValue);
    if (!style || !style->element)
        return JS_EXCEPTION;

    if (argc < 2)
        return JS_UNDEFINED;

    const QString property = stringValue(context, argv[0]);
    const QString value = stringValue(context, argv[1]);

    // An empty value removes the property, which the CSSOM specifies, so
    // setProperty("color", "") clears it rather than writing an empty one.
    if (value.isEmpty())
        removeStyleProperty(style->element, property);
    else
        setStyleProperty(style->element, property, value);

    touchDocument(context);
    return JS_UNDEFINED;
}

JSValue styleRemoveProperty(JSContext *context, JSValueConst thisValue, int argc,
                            JSValueConst *argv)
{
    StyleDeclaration *style = styleOf(thisValue);
    if (!style || !style->element)
        return JS_EXCEPTION;

    if (argc < 1)
        return newString(context, QString());

    const QString property = stringValue(context, argv[0]);
    const QString previous = stylePropertyOf(style->element, property);
    removeStyleProperty(style->element, property);

    touchDocument(context);
    // The value that was there is returned, which script uses to restore it.
    return newString(context, previous);
}

JSValue styleGetCssText(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    StyleDeclaration *style = styleOf(thisValue);
    if (!style || !style->element)
        return JS_EXCEPTION;
    return newString(context, style->element->attribute(QStringLiteral("style")));
}

JSValue styleSetCssText(JSContext *context, JSValueConst thisValue, int argc, JSValueConst *argv)
{
    StyleDeclaration *style = styleOf(thisValue);
    if (!style || !style->element)
        return JS_EXCEPTION;

    if (argc > 0)
        style->element->attr().set(QStringLiteral("style"), stringValue(context, argv[0]));

    touchDocument(context);
    return JS_UNDEFINED;
}

/// The property names exposed as direct camelCase members: the set a page
/// actually sets. A property outside the list is still reachable through
/// setProperty, so nothing is unreachable.
const QStringList &stylePropertyList()
{
    static const QStringList names = {
        QStringLiteral("align-items"),         QStringLiteral("background"),
        QStringLiteral("background-color"),    QStringLiteral("background-image"),
        QStringLiteral("background-position"), QStringLiteral("background-repeat"),
        QStringLiteral("background-size"),     QStringLiteral("border"),
        QStringLiteral("border-bottom"),       QStringLiteral("border-color"),
        QStringLiteral("border-left"),         QStringLiteral("border-radius"),
        QStringLiteral("border-right"),        QStringLiteral("border-style"),
        QStringLiteral("border-top"),          QStringLiteral("border-width"),
        QStringLiteral("bottom"),              QStringLiteral("box-shadow"),
        QStringLiteral("box-sizing"),          QStringLiteral("clear"),
        QStringLiteral("color"),               QStringLiteral("content"),
        QStringLiteral("cursor"),              QStringLiteral("direction"),
        QStringLiteral("display"),             QStringLiteral("fill"),
        QStringLiteral("flex"),                QStringLiteral("flex-basis"),
        QStringLiteral("flex-direction"),      QStringLiteral("flex-grow"),
        QStringLiteral("flex-shrink"),         QStringLiteral("flex-wrap"),
        QStringLiteral("float"),               QStringLiteral("font"),
        QStringLiteral("font-family"),         QStringLiteral("font-size"),
        QStringLiteral("font-style"),          QStringLiteral("font-weight"),
        QStringLiteral("gap"),                 QStringLiteral("height"),
        QStringLiteral("justify-content"),     QStringLiteral("left"),
        QStringLiteral("letter-spacing"),      QStringLiteral("line-height"),
        QStringLiteral("list-style"),          QStringLiteral("margin"),
        QStringLiteral("margin-bottom"),       QStringLiteral("margin-left"),
        QStringLiteral("margin-right"),        QStringLiteral("margin-top"),
        QStringLiteral("max-height"),          QStringLiteral("max-width"),
        QStringLiteral("min-height"),          QStringLiteral("min-width"),
        QStringLiteral("object-fit"),          QStringLiteral("opacity"),
        QStringLiteral("order"),               QStringLiteral("outline"),
        QStringLiteral("overflow"),            QStringLiteral("overflow-x"),
        QStringLiteral("overflow-y"),          QStringLiteral("padding"),
        QStringLiteral("padding-bottom"),      QStringLiteral("padding-left"),
        QStringLiteral("padding-right"),       QStringLiteral("padding-top"),
        QStringLiteral("pointer-events"),      QStringLiteral("position"),
        QStringLiteral("right"),               QStringLiteral("stroke"),
        QStringLiteral("text-align"),          QStringLiteral("text-decoration"),
        QStringLiteral("text-indent"),         QStringLiteral("text-overflow"),
        QStringLiteral("text-shadow"),         QStringLiteral("text-transform"),
        QStringLiteral("top"),                 QStringLiteral("transform"),
        QStringLiteral("transition"),          QStringLiteral("user-select"),
        QStringLiteral("vertical-align"),      QStringLiteral("visibility"),
        QStringLiteral("white-space"),         QStringLiteral("width"),
        QStringLiteral("word-break"),          QStringLiteral("word-spacing"),
        QStringLiteral("word-wrap"),           QStringLiteral("z-index"),
    };
    return names;
}

/// One accessor pair serves every property in the list: the magic number is the
/// property's index, which avoids ninety near-identical pairs of functions.
JSValue stylePropertyGetter(JSContext *context, JSValueConst thisValue, int, JSValueConst *,
                            int magic)
{
    StyleDeclaration *style = styleOf(thisValue);
    if (!style || !style->element)
        return JS_EXCEPTION;

    const QStringList &names = stylePropertyList();
    if (magic < 0 || magic >= names.size())
        return JS_UNDEFINED;

    return newString(context, stylePropertyOf(style->element, names.at(magic)));
}

JSValue stylePropertySetter(JSContext *context, JSValueConst thisValue, int argc,
                            JSValueConst *argv, int magic)
{
    StyleDeclaration *style = styleOf(thisValue);
    if (!style || !style->element)
        return JS_EXCEPTION;

    const QStringList &names = stylePropertyList();
    if (magic < 0 || magic >= names.size())
        return JS_UNDEFINED;

    const QString value = argc > 0 ? stringValue(context, argv[0]) : QString();
    if (value.isEmpty())
        removeStyleProperty(style->element, names.at(magic));
    else
        setStyleProperty(style->element, names.at(magic), value);

    touchDocument(context);
    return JS_UNDEFINED;
}

// -------------------------------------------------- reflected attributes

/// The attributes a page reads and writes as plain properties: `el.href`,
/// `img.src` and `input.value` are the attribute of the same name, which is how
/// HTML reflects them.
struct ReflectedAttribute
{
    const char *name;
    /// True for the attributes whose property value is their presence rather
    /// than their text.
    bool boolean;
};

const ReflectedAttribute *reflectedAttributes(int *count)
{
    static const ReflectedAttribute kAttributes[] = {
        {"id", false},         {"title", false},      {"lang", false},
        {"dir", false},        {"href", false},       {"src", false},
        {"alt", false},        {"value", false},      {"name", false},
        {"type", false},       {"rel", false},        {"target", false},
        {"width", false},      {"height", false},     {"placeholder", false},
        {"content", false},    {"charset", false},    {"action", false},
        {"method", false},     {"for", false},        {"accept", false},
        {"autocomplete", false}, {"download", false}, {"loading", false},
        {"decoding", false},   {"colspan", false},    {"rowspan", false},
        {"disabled", true},    {"checked", true},     {"selected", true},
        {"readonly", true},    {"required", true},    {"multiple", true},
        {"hidden", true},      {"autofocus", true},   {"open", true},
    };
    *count = static_cast<int>(std::size(kAttributes));
    return kAttributes;
}

/// One getter serves every reflected attribute; the magic number is its index.
JSValue reflectedAttributeGetter(JSContext *context, JSValueConst thisValue, int, JSValueConst *,
                                 int magic)
{
    dom::Element *element = thisElement(context, thisValue);
    if (!element)
        return JS_EXCEPTION;

    int count = 0;
    const ReflectedAttribute *attributes = reflectedAttributes(&count);
    if (magic < 0 || magic >= count)
        return JS_UNDEFINED;

    const ReflectedAttribute &attribute = attributes[magic];
    const QString name = QString::fromLatin1(attribute.name);

    // A boolean attribute reflects its presence, so `input.disabled` is true
    // when the attribute is there at all, whatever its value.
    if (attribute.boolean)
        return JS_NewBool(context, element->hasAttribute(name));

    return newString(context, element->attribute(name));
}

JSValue reflectedAttributeSetter(JSContext *context, JSValueConst thisValue, int argc,
                                 JSValueConst *argv, int magic)
{
    dom::Element *element = thisElement(context, thisValue);
    if (!element)
        return JS_EXCEPTION;

    int count = 0;
    const ReflectedAttribute *attributes = reflectedAttributes(&count);
    if (magic < 0 || magic >= count)
        return JS_UNDEFINED;

    const ReflectedAttribute &attribute = attributes[magic];
    const QString name = QString::fromLatin1(attribute.name);

    if (attribute.boolean) {
        // Setting a boolean property to false removes the attribute, which is
        // how `input.disabled = false` re-enables a control.
        if (argc > 0 && boolValue(context, argv[0]))
            element->attr().set(name, QString());
        else
            element->attr().remove(name);
    } else {
        element->attr().set(name, argc > 0 ? stringValue(context, argv[0]) : QString());
    }

    touchDocument(context);
    return JS_UNDEFINED;
}

} // namespace

// ---------------------------------------------------------- constructors

JSValue makeClassListObject(JSContext *context, dom::Element *element)
{
    JSValue object = JS_NewObjectClass(
        context, auxiliaryClassId(context, "DOMTokenList", finalizeClassList));
    if (JS_IsException(object))
        return object;

    auto *list = new ClassList;
    list->element = element;
    if (JS_SetOpaque(object, list) < 0) {
        delete list;
        JS_FreeValue(context, object);
        return JS_UNDEFINED;
    }

    defineGetter(context, object, "length", classListGetLength);
    defineGetter(context, object, "value", classListGetValue);
    defineMethod(context, object, "item", classListItem, 1);
    defineMethod(context, object, "contains", classListContains, 1);
    defineMethod(context, object, "add", classListAdd, 1);
    defineMethod(context, object, "remove", classListRemove, 1);
    defineMethod(context, object, "toggle", classListToggle, 1);
    defineMethod(context, object, "replace", classListReplace, 2);
    defineMethod(context, object, "toString", classListGetValue, 0);

    // The indexed members, so `classList[0]` works as well as `classList.item(0)`.
    // They are plain properties because the token list is read as a snapshot.
    const QStringList classes = element->classList();
    for (int i = 0; i < classes.size(); ++i) {
        const QByteArray index = QByteArray::number(i);
        defineString(context, object, index.constData(), classes.at(i));
    }

    Q_UNUSED(classListCall);
    return object;
}

JSValue makeStyleDeclarationObject(JSContext *context, dom::Element *element)
{
    JSValue object = JS_NewObjectClass(
        context, auxiliaryClassId(context, "CSSStyleDeclaration", finalizeStyleDeclaration));
    if (JS_IsException(object))
        return object;

    auto *style = new StyleDeclaration;
    style->element = element;
    if (JS_SetOpaque(object, style) < 0) {
        delete style;
        JS_FreeValue(context, object);
        return JS_UNDEFINED;
    }

    // Every property becomes a camelCase accessor, so `el.style.backgroundColor =
    // "red"` works exactly as it would in a browser.
    const QStringList &names = stylePropertyList();
    for (int i = 0; i < names.size(); ++i) {
        const QByteArray camel = camelProperty(names.at(i)).toUtf8();

        JSAtom atom = JS_NewAtom(context, camel.constData());
        JSValue getter = JS_NewCFunctionMagic(context, stylePropertyGetter, camel.constData(), 0,
                                             JS_CFUNC_generic_magic, i);
        JSValue setter = JS_NewCFunctionMagic(context, stylePropertySetter, camel.constData(), 1,
                                             JS_CFUNC_generic_magic, i);
        JS_DefinePropertyGetSet(context, object, atom, getter, setter,
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE | JS_PROP_HAS_GET
                                    | JS_PROP_HAS_SET);
        JS_FreeAtom(context, atom);
    }

    // The CSSOM members the accessors are built on.
    defineMethod(context, object, "getPropertyValue", styleGetProperty, 1);
    defineMethod(context, object, "setProperty", styleSetProperty, 2);
    defineMethod(context, object, "removeProperty", styleRemoveProperty, 1);
    defineAccessor(context, object, "cssText", styleGetCssText, styleSetCssText);

    Q_UNUSED(dashedProperty);
    return object;
}

JSValue classListOf(JSContext *context, JSValueConst thisValue)
{
    dom::Element *element = thisElement(context, thisValue);
    if (!element)
        return JS_EXCEPTION;
    return makeClassListObject(context, element);
}

JSValue styleDeclarationOf(JSContext *context, JSValueConst thisValue)
{
    dom::Element *element = thisElement(context, thisValue);
    if (!element)
        return JS_EXCEPTION;
    return makeStyleDeclarationObject(context, element);
}

void installReflectedAttributes(JSContext *context, JSValue prototype)
{
    int count = 0;
    const ReflectedAttribute *attributes = reflectedAttributes(&count);

    for (int i = 0; i < count; ++i) {
        const char *name = attributes[i].name;

        JSAtom atom = JS_NewAtom(context, name);
        JSValue getter = JS_NewCFunctionMagic(context, reflectedAttributeGetter, name, 0,
                                              JS_CFUNC_generic_magic, i);
        JSValue setter = JS_NewCFunctionMagic(context, reflectedAttributeSetter, name, 1,
                                              JS_CFUNC_generic_magic, i);
        JS_DefinePropertyGetSet(context, prototype, atom, getter, setter,
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE | JS_PROP_HAS_GET
                                    | JS_PROP_HAS_SET);
        JS_FreeAtom(context, atom);
    }
}

} // namespace oqb::javascript::detail

#endif // OPENQBROWSER_SCRIPTING
