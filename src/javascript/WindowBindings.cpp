#include "javascript/BindingsInternal.h"

#ifdef OPENQBROWSER_SCRIPTING

namespace oqb::javascript::detail {
namespace {

// -------------------------------------------------------------- navigator

/// The navigator object. Every page sniffs it, so the values are the ones a page
/// needs to take its non-broken path: a modern engine, a desktop platform, and
/// no automation or vendor strings that pages special-case.
void installNavigator(JSContext *context, JSValue global)
{
    JSValue navigator = JS_NewObject(context);

    defineString(context, navigator, "userAgent",
                 QStringLiteral("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                                "AppleWebKit/537.36 (KHTML, like Gecko) OpenQBrowser/1.0"));
    defineString(context, navigator, "appName", QStringLiteral("Netscape"));
    defineString(context, navigator, "appCodeName", QStringLiteral("Mozilla"));
    defineString(context, navigator, "platform", QStringLiteral("MacIntel"));
    defineString(context, navigator, "language", QStringLiteral("en-US"));
    defineString(context, navigator, "product", QStringLiteral("Gecko"));
    defineString(context, navigator, "vendor", QString());

    // A fixed list, because the engine has no font, codec or API probe to base
    // it on and a page branching on it does better with a small truthful list
    // than with a plausible invention.
    JSValue languages = JS_NewArray(context);
    JS_SetPropertyUint32(context, languages, 0, newString(context, QStringLiteral("en-US")));
    JS_SetPropertyUint32(context, languages, 1, newString(context, QStringLiteral("en")));
    JS_DefinePropertyValueStr(context, navigator, "languages", languages, JS_PROP_C_W_E);

    detail::defineInt(context, navigator, "hardwareConcurrency", 4);
    detail::defineInt(context, navigator, "maxTouchPoints", 0);

    // The one feature test that changes which code path a modern page takes.
    // Reporting it as anything but false would be a lie the page then acts on.
    JS_DefinePropertyValueStr(context, navigator, "webdriver", JS_NewBool(context, false),
                              JS_PROP_C_W_E);

    JS_DefinePropertyValueStr(context, global, "navigator", navigator, JS_PROP_C_W_E);
}

// --------------------------------------------------------------- screen

void installScreen(JSContext *context, JSValue global)
{
    JSValue screen = JS_NewObject(context);
    detail::defineInt(context, screen, "width", 1440);
    detail::defineInt(context, screen, "height", 900);
    detail::defineInt(context, screen, "availWidth", 1440);
    detail::defineInt(context, screen, "availHeight", 860);
    detail::defineInt(context, screen, "colorDepth", 24);
    detail::defineInt(context, screen, "pixelDepth", 24);

    JS_DefinePropertyValueStr(context, global, "screen", screen, JS_PROP_C_W_E);
}

// ----------------------------------------------------------- the location

/// The document's location. Read-only except for href and the members built on
/// it, because the browser owns navigation and would otherwise have to resync
/// the engine mid-script.
void installLocation(JSContext *context, JSValue global, dom::Document *document)
{
    JSValue location = JS_NewObject(context);

    const network::Url url = document ? document->url() : network::Url();

    defineString(context, location, "href", url.toString());
    defineString(context, location, "protocol", url.scheme().isEmpty() ? QString() : url.scheme() + u':');
    defineString(context, location, "host", url.host());
    defineString(context, location, "hostname", url.host());
    defineString(context, location, "pathname", url.path());
    defineString(context, location, "search", url.query().isEmpty() ? QString() : u'?' + url.query());
    defineString(context, location, "hash", url.fragment().isEmpty() ? QString() : u'#' + url.fragment());
    defineString(context, location, "origin",
                 url.scheme().isEmpty() ? QStringLiteral("null")
                                        : QStringLiteral("%1://%2").arg(url.scheme(), url.host()));

    // Navigation is not exposed: a script calling location.reload() or assigning
    // a new href would have to unwind the very evaluation that is running, and
    // the browser's own loader is the only thing that may start a load. The
    // functions are present so a page that feature-detects them does not throw,
    // and they report through the console instead of navigating.
    auto noteNavigation = [](JSContext *ctx, JSValueConst, int argc, JSValueConst *argv, int) {
        const QString target = argc > 0 ? stringValue(ctx, argv[0]) : QString();
        reportMessage(ctx, ConsoleMessage::Level::Warning,
                      target.isEmpty()
                          ? QStringLiteral("navigation from script is not supported yet")
                          : QStringLiteral("navigation to \"%1\" from script is not supported yet")
                                .arg(target));
        return JS_UNDEFINED;
    };

    defineMagicMethod(context, location, "reload", noteNavigation, 0, 0);
    defineMagicMethod(context, location, "assign", noteNavigation, 1, 1);
    defineMagicMethod(context, location, "replace", noteNavigation, 1, 2);

    JS_DefinePropertyValueStr(context, global, "location", location, JS_PROP_C_W_E);
}

// ------------------------------------------------------------ alert etc.

/// alert, confirm and prompt. The browser has no dialog to show yet, so alert is
/// logged and the questions answer the way a dismissed dialog would. Reporting
/// through the console keeps the call visible rather than silently doing
/// nothing.
JSValue alertFunction(JSContext *context, JSValueConst, int argc, JSValueConst *argv)
{
    const QString text = argc > 0 ? stringValue(context, argv[0]) : QString();
    reportMessage(context, ConsoleMessage::Level::Log, QStringLiteral("[alert] %1").arg(text));
    return JS_UNDEFINED;
}

JSValue confirmFunction(JSContext *context, JSValueConst, int argc, JSValueConst *argv)
{
    const QString text = argc > 0 ? stringValue(context, argv[0]) : QString();
    reportMessage(context, ConsoleMessage::Level::Log,
                  QStringLiteral("[confirm] %1 -> false (no dialog in this browser)").arg(text));
    // False, which is what a browser returns when the user dismisses the dialog,
    // so a page taking the "cancel" path stays on its safe branch.
    return JS_NewBool(context, false);
}

JSValue promptFunction(JSContext *context, JSValueConst, int argc, JSValueConst *argv)
{
    const QString text = argc > 0 ? stringValue(context, argv[0]) : QString();
    reportMessage(context, ConsoleMessage::Level::Log,
                  QStringLiteral("[prompt] %1 -> null (no dialog in this browser)").arg(text));
    return JS_NULL;
}

/// The btoa and atob pair, which pages use for basic-auth headers and data URLs.
JSValue btoaFunction(JSContext *context, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc < 1)
        return newString(context, QString());

    // The argument is a binary string, so each character is a byte, which is why
    // it is read through Latin-1 rather than UTF-8.
    const QString text = stringValue(context, argv[0]);
    QByteArray bytes;
    bytes.reserve(text.size());
    for (const QChar c : text) {
        if (c.unicode() > 255) {
            return throwDomError(context, QStringLiteral("InvalidCharacterError"),
                                 QStringLiteral("btoa: the string contains a character "
                                                "outside Latin-1"));
        }
        bytes.append(static_cast<char>(c.unicode()));
    }

    return newString(context, QString::fromLatin1(bytes.toBase64()));
}

JSValue atobFunction(JSContext *context, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc < 1)
        return newString(context, QString());

    const QByteArray decoded
        = QByteArray::fromBase64(stringValue(context, argv[0]).toLatin1());

    // The result is a binary string, so the bytes become characters one for one.
    QString text;
    text.reserve(decoded.size());
    for (const char byte : decoded)
        text.append(QChar(static_cast<uchar>(byte)));

    return newString(context, text);
}

JSValue matchMediaFunction(JSContext *context, JSValueConst, int, JSValueConst *)
{
    // A media query object with no listeners. A page that only asks "does this
    // match" gets the right answer; one that listens for a change would need
    // the renderer to report viewport changes, which it does not yet.
    JSValue object = JS_NewObject(context);
    JS_DefinePropertyValueStr(context, object, "matches", JS_NewBool(context, false),
                              JS_PROP_C_W_E);
    defineString(context, object, "media", QString());
    return object;
}

/// getComputedStyle. The computed style lives in the style engine, which the DOM
/// bindings cannot reach without exposing the renderer, so only what the style
/// attribute carries is reported. A page reading back a property it set inline
/// gets the right answer; one reading a rule from a stylesheet gets an empty
/// string.
JSValue getComputedStyleFunction(JSContext *context, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc < 1)
        return JS_EXCEPTION;

    return styleDeclarationOf(context, argv[0]);
}

JSValue getSelectionFunction(JSContext *context, JSValueConst, int, JSValueConst *)
{
    // There is no selection model yet, so this reports an empty selection.
    JSValue selection = JS_NewObject(context);
    defineString(context, selection, "type", QStringLiteral("None"));
    defineInt(context, selection, "rangeCount", 0);
    return selection;
}

/// The members that are present so feature detection succeeds but that the
/// browser has no subsystem behind.
void installStorageStubs(JSContext *context, JSValue global)
{
    // localStorage and sessionStorage are objects with a length and the members a
    // page calls. Nothing is stored, and a page that reads back what it wrote
    // gets an empty result rather than an error, which is what a browser reports
    // when storage is blocked.
    for (const char *name : {"localStorage", "sessionStorage"}) {
        JSValue storage = JS_NewObject(context);
        detail::defineInt(context, storage, "length", 0);

        auto stub = [](JSContext *ctx, JSValueConst, int, JSValueConst *, int) {
            reportMessage(ctx, ConsoleMessage::Level::Warning,
                          QStringLiteral("web storage is not supported yet"));
            return JS_NULL;
        };
        defineMagicMethod(context, storage, "getItem", stub, 1, 0);
        defineMagicMethod(context, storage, "setItem", stub, 2, 1);
        defineMagicMethod(context, storage, "removeItem", stub, 1, 2);
        defineMagicMethod(context, storage, "clear", stub, 0, 3);
        defineMagicMethod(context, storage, "key", stub, 1, 4);

        JS_DefinePropertyValueStr(context, global, name, storage, JS_PROP_C_W_E);
    }
}

/// Stubs for the APIs a page feature-detects before using. Each is a function
/// that reports once through the console, so a page that calls it degrades to
/// its own fallback instead of throwing a TypeError.
void installApiStubs(JSContext *context, JSValue global)
{
    // The name the page called, carried in the magic number, so one function can
    // report all of them accurately.
    static const char *const kApiNames[] = {
        "window.open", "window.close", "window.stop", "window.scrollTo",
        "window.scroll", "window.focus", "window.blur", "window.print",
    };

    auto stub = [](JSContext *ctx, JSValueConst, int, JSValueConst *, int magic) {
        const int index = qBound(0, magic, static_cast<int>(std::size(kApiNames)) - 1);
        reportMessage(ctx, ConsoleMessage::Level::Warning,
                      QStringLiteral("%1 is not supported yet")
                          .arg(QString::fromLatin1(kApiNames[index])));
        return JS_UNDEFINED;
    };

    const char *const names[] = {"open", "close", "stop", "scrollTo",
                                 "scroll", "focus", "blur", "print"};
    for (int i = 0; i < 8; ++i)
        defineMagicMethod(context, global, names[i], stub, 1, i);
}

} // namespace

void installGlobalMembers(JSContext *context, JSValue global, dom::Document *document)
{
    installNavigator(context, global);
    installScreen(context, global);
    installLocation(context, global, document);
    installStorageStubs(context, global);
    installApiStubs(context, global);

    defineMethod(context, global, "alert", alertFunction, 1);
    defineMethod(context, global, "confirm", confirmFunction, 1);
    defineMethod(context, global, "prompt", promptFunction, 2);
    defineMethod(context, global, "btoa", btoaFunction, 1);
    defineMethod(context, global, "atob", atobFunction, 1);
    defineMethod(context, global, "matchMedia", matchMediaFunction, 1);
    defineMethod(context, global, "getComputedStyle", getComputedStyleFunction, 1);
    defineMethod(context, global, "getSelection", getSelectionFunction, 0);

    // The timer-precision guarantee a page can feature-detect. Reporting 0 says
    // the engine does not coalesce timers, which is true.
    defineInt(context, global, "devicePixelRatio", 1);
    defineString(context, global, "name", QString());
    defineInt(context, global, "innerWidth", 1440);
    defineInt(context, global, "innerHeight", 900);
    defineInt(context, global, "outerWidth", 1440);
    defineInt(context, global, "outerHeight", 900);
    defineInt(context, global, "scrollX", 0);
    defineInt(context, global, "scrollY", 0);
    defineInt(context, global, "pageXOffset", 0);
    defineInt(context, global, "pageYOffset", 0);

    // The load state, which document.readyState reads and the script runner
    // updates as the document is built.
    defineString(context, global, "__readyState", QStringLiteral("loading"));
}

} // namespace oqb::javascript::detail

#endif // OPENQBROWSER_SCRIPTING
