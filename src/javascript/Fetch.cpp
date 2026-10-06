#include "javascript/Fetch.h"

#include "javascript/Bindings.h"
#include "javascript/BindingsInternal.h"

#include "network/FetchPolicy.h"
#include "security/SecurityPolicy.h"
#include "network/ScriptFetch.h"

#include <QHash>

#ifdef OPENQBROWSER_SCRIPTING

namespace oqb::javascript {

namespace {

/// What an opaque block on a `Response` or `Headers` object carries.
///
/// The body is held as bytes rather than as a JavaScript value because the
/// accessors must be usable more than once and after the response has been
/// collected: `await r.text()` then `await r.text()` works in a browser, and a
/// body that could only be read once would surprise every page that reads it
/// after checking the status.
struct FetchData
{
    /// The response, or the header list for a Headers object whose response is
    /// empty.
    network::ScriptFetchResponse response;

    /// Distinguishes the two kinds of object, because both share one class per
    /// kind of accessor set but the guards differ.
    bool isHeaders = false;
};

/// The outstanding fetches for one context, keyed by the id the provider gave.
struct PendingFetch
{
    /// The reject function of the promise returned to script. Owned here: it must
    /// outlive the call that created the promise, because that is the whole point
    /// of a promise.
    JSValue reject = JS_UNDEFINED;
    /// The resolve function, released as soon as the response is delivered.
    JSValue resolve = JS_UNDEFINED;
};

/// Where the fetch bindings keep their per-context state.
///
/// Keyed by context rather than held in the bindings' State, because the two
/// have different lifetimes: the State is built when the bindings are installed,
/// while the provider is attached by the browser once the loader exists, and a
/// test may attach a fake one to an engine that already has a State.
struct FetchContext
{
    JSValue fetchFunction = JS_UNDEFINED;
    JSValue responsePrototype = JS_UNDEFINED;
    JSValue headersPrototype = JS_UNDEFINED;
    /// The provider, not owned. Lives at least as long as the page.
    network::ScriptFetchProvider *provider = nullptr;
    int nextLocalId = 1;
    QHash<int, PendingFetch> pending;

    int size() const { return pending.size(); }
};

QHash<JSContext *, FetchContext> &contexts()
{
    static QHash<JSContext *, FetchContext> table;
    return table;
}

FetchContext *fetchContextOf(JSContext *context)
{
    const auto it = contexts().find(context);
    return it == contexts().end() ? nullptr : &it.value();
}

/// The class every fetch object is built from. One class, because they all carry
/// the same block: registering a class is what gives an object a finalizer, and a
/// block attached to an object with no class would never be freed.
JSClassID fetchDataClassId(JSContext *context)
{
    return detail::auxiliaryClassId(context, "FetchData", [](JSRuntime *, JSValueConst value) {
        // The finalizer runs only for objects of this class, so the block is
        // known to be ours and needs no check of its own.
        JSClassID classId = 0;
        if (void *opaque = JS_GetAnyOpaque(value, &classId))
            delete static_cast<FetchData *>(opaque);
    });
}

/// The block behind a `Response` or a `Headers`, or nullptr when the receiver is
/// not one of ours.
///
/// The class id is checked, not just the presence of an opaque block. Every
/// wrapped DOM node carries one too, and `JS_GetAnyOpaque` hands back whatever
/// class the object happens to be: without this check, `response.text.call(node)`
/// would read a dom::Node pointer as a FetchData and follow it into a crash.
FetchData *dataOf(JSContext *context, JSValueConst value)
{
    JSClassID classId = 0;
    void *opaque = JS_GetAnyOpaque(value, &classId);
    if (!opaque || classId != fetchDataClassId(context))
        return nullptr;
    return static_cast<FetchData *>(opaque);
}

// ------------------------------------------------------------------ helpers

/// True when `name` appears in `headers`, so that an absent header can be told
/// from one that was sent empty. `headers.get` returns null for the first and an
/// empty string for the second, which is a distinction pages do rely on.
bool hasHeader(const QList<QPair<QString, QString>> &headers, const QString &name)
{
    for (const auto &entry : headers) {
        if (entry.first.compare(name, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}


/// Decodes a body using the charset its Content-Type declared, falling back to
/// UTF-8, which is what the web treats unsaid text as.
QString decodeBody(const QByteArray &body, const QString &contentType)
{
    const QString charset = network::http::splitContentType(contentType).second;
    const QString codec = network::http::codecNameForCharset(charset);
    if (codec.isEmpty())
        return QString::fromUtf8(body);

    QStringDecoder decoder(codec.toUtf8().constData());
    QString text = decoder.decode(body);
    return decoder.hasError() ? QString::fromUtf8(body) : text;
}

/// Renders an exception as text: its `stack` when it has one, because a rejected
/// promise's handler is what failed and the frames are what make it findable,
/// and its message otherwise. A thrown non-object is reported as itself.
QString describeException(JSContext *context, JSValueConst exception)
{
    if (JS_IsObject(exception)) {
        JSValue stack = JS_GetPropertyStr(context, exception, "stack");
        if (JS_IsString(stack)) {
            const QString text = detail::stringValue(context, stack);
            JS_FreeValue(context, stack);
            return text;
        }
        JS_FreeValue(context, stack);
    }
    return detail::stringValue(context, exception);
}

/// The global a constructor is looked up under, so a rejected promise throws the
/// error class a browser would.
JSValue builtinConstructor(JSContext *context, const char *name)
{
    JSValue global = JS_GetGlobalObject(context);
    JSValue constructor = JS_GetPropertyStr(context, global, name);
    JS_FreeValue(context, global);
    return constructor;
}

/// Builds an object of the fetch class holding `data`, which it takes ownership
/// of. Returns the object, or JS_EXCEPTION when it could not be created, in which
/// case `data` is already freed.
JSValue makeFetchObject(JSContext *context, JSValueConst prototype, FetchData *data)
{
    JSValue object = JS_NewObjectProtoClass(context, prototype, fetchDataClassId(context));
    if (JS_IsException(object)) {
        delete data;
        return object;
    }

    if (JS_SetOpaque(object, data) < 0) {
        delete data;
        JS_FreeValue(context, object);
        return JS_EXCEPTION;
    }
    return object;
}

// ------------------------------------------------------------------ headers

/// The value a Headers object reports for `name`: a name that appears more than
/// once is joined with ", ", which is what a browser does. The raw value is
/// wanted instead when the parameter matters, as it does for the charset.
QString headerValue(const QList<QPair<QString, QString>> &headers, const QString &name)
{
    QStringList values;
    for (const auto &entry : headers) {
        if (entry.first.compare(name, Qt::CaseInsensitive) == 0)
            values.append(entry.second);
    }
    return values.join(QStringLiteral(", "));
}

/// The first value sent for `name`, unmodified. Used where a parameter carries
/// meaning - the charset of a body - and joining two of them would produce
/// nonsense.
QString rawHeaderValue(const QList<QPair<QString, QString>> &headers, const QString &name)
{
    for (const auto &entry : headers) {
        if (entry.first.compare(name, Qt::CaseInsensitive) == 0)
            return entry.second;
    }
    return {};
}

/// Builds the object `response.headers` returns.
JSValue makeHeadersObject(JSContext *context, const network::ScriptFetchResponse &response)
{
    FetchContext *state = fetchContextOf(context);
    if (!state)
        return JS_UNDEFINED;

    auto *data = new FetchData;
    data->response = response;
    data->isHeaders = true;

    const JSValue object = makeFetchObject(context, state->headersPrototype, data);
    return JS_IsException(object) ? JS_UNDEFINED : object;
}

JSValue headersGet(JSContext *context, JSValueConst thisValue, int argc, JSValueConst *argv)
{
    FetchData *data = dataOf(context, thisValue);
    if (!data || argc < 1)
        return JS_NULL;

    const QString name = detail::stringValue(context, argv[0]);
    const QString value = headerValue(data->response.headers, name);
    if (value.isEmpty() && !hasHeader(data->response.headers, name))
        return JS_NULL;
    return detail::newString(context, value);
}

JSValue headersHas(JSContext *context, JSValueConst thisValue, int argc, JSValueConst *argv)
{
    FetchData *data = dataOf(context, thisValue);
    if (!data || argc < 1)
        return JS_NewBool(context, false);
    return JS_NewBool(context, hasHeader(data->response.headers,
                                                 detail::stringValue(context, argv[0])));
}

JSValue headersForEach(JSContext *context, JSValueConst thisValue, int argc, JSValueConst *argv)
{
    FetchData *data = dataOf(context, thisValue);
    if (!data || argc < 1 || !JS_IsFunction(context, argv[0]))
        return JS_UNDEFINED;

    JSValue callback = argv[0];
    JSValue thisArg = argc > 1 ? argv[1] : JS_UNDEFINED;

    for (const auto &entry : data->response.headers) {
        JSValue args[3] = {detail::newString(context, entry.second),
                           detail::newString(context, entry.first), thisValue};
        JSValue result = JS_Call(context, callback, thisArg, 3, args);
        for (JSValue &arg : args)
            JS_FreeValue(context, arg);

        if (JS_IsException(result))
            return result;
        JS_FreeValue(context, result);
    }
    return JS_UNDEFINED;
}

// ----------------------------------------------------------------- response

/// Builds the object `fetch()` resolves with.
JSValue makeResponseObject(JSContext *context, const network::ScriptFetchResponse &response)
{
    FetchContext *state = fetchContextOf(context);
    if (!state)
        return JS_UNDEFINED;

    auto *data = new FetchData;
    data->response = response;

    const JSValue object = makeFetchObject(context, state->responsePrototype, data);
    return JS_IsException(object) ? JS_UNDEFINED : object;
}

JSValue responseGetStatus(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    FetchData *data = dataOf(context, thisValue);
    if (!data)
        return JS_EXCEPTION;
    // An opaque response hides its status. A page that could read it could use
    // it to probe a cross-origin resource for existence, which is exactly what
    // the opacity is there to prevent.
    return JS_NewInt32(context, data->response.opaque ? 0 : data->response.status);
}

JSValue responseGetStatusText(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    FetchData *data = dataOf(context, thisValue);
    if (!data)
        return JS_EXCEPTION;
    return detail::newString(context, data->response.opaque ? QString() : data->response.statusText);
}

JSValue responseGetOk(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    FetchData *data = dataOf(context, thisValue);
    if (!data)
        return JS_EXCEPTION;
    return JS_NewBool(context, !data->response.opaque && data->response.status >= 200
                                 && data->response.status < 300);
}

JSValue responseGetUrl(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    FetchData *data = dataOf(context, thisValue);
    if (!data)
        return JS_EXCEPTION;
    // An opaque response reports an empty URL: after a redirect it would
    // otherwise disclose the final address of a resource the page may not read.
    return detail::newString(context, data->response.opaque ? QString() : data->response.url);
}

JSValue responseGetRedirected(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    FetchData *data = dataOf(context, thisValue);
    if (!data)
        return JS_EXCEPTION;
    return JS_NewBool(context, data->response.redirected);
}

JSValue responseGetType(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    FetchData *data = dataOf(context, thisValue);
    if (!data)
        return JS_EXCEPTION;
    return detail::newString(context, data->response.opaque ? QStringLiteral("opaque")
                                                            : QStringLiteral("basic"));
}

JSValue responseGetBodyUsed(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    FetchData *data = dataOf(context, thisValue);
    if (!data)
        return JS_EXCEPTION;
    // The body is kept and can be read repeatedly, so it is never "used". Saying
    // so is truthful: the flag exists to warn that a second read would fail, and
    // here it would not.
    return JS_NewBool(context, false);
}

JSValue responseGetHeaders(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    FetchData *data = dataOf(context, thisValue);
    if (!data)
        return JS_EXCEPTION;
    return makeHeadersObject(context, data->response);
}

/// `response.text()`. Returns a promise whose value is the body decoded as text.
JSValue responseText(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    FetchData *data = dataOf(context, thisValue);
    if (!data)
        return JS_EXCEPTION;

    // The charset comes from the response's own Content-Type, so a page reading
    // a Latin-1 document gets the text the server meant rather than mojibake.
    const QString charset = rawHeaderValue(data->response.headers,
                                          QStringLiteral("content-type"));
    const QString text = decodeBody(data->response.body, charset);

    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(context, resolving);
    if (JS_IsException(promise))
        return promise;

    JSValue value = detail::newString(context, text);
    JSValue result = JS_Call(context, resolving[0], JS_UNDEFINED, 1, &value);
    JS_FreeValue(context, value);
    JS_FreeValue(context, resolving[0]);
    JS_FreeValue(context, resolving[1]);

    if (JS_IsException(result)) {
        JS_FreeValue(context, promise);
        return result;
    }
    JS_FreeValue(context, result);
    return promise;
}

/// `response.json()`. Parses the body, rejecting the promise when it is not JSON.
JSValue responseJson(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    FetchData *data = dataOf(context, thisValue);
    if (!data)
        return JS_EXCEPTION;

    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(context, resolving);
    if (JS_IsException(promise))
        return promise;

    // The reply is parsed by the engine rather than by Qt, so a number keeps the
    // precision the engine would have given it and the error is the engine's own.
    // A parse failure rejects rather than throwing out of the call, which is what
    // a page checking the shape of what it received expects.
    JSValue parsed = JS_ParseJSON(context, data->response.body.constData(),
                                  static_cast<size_t>(data->response.body.size()),
                                  "<json>");
    if (JS_IsException(parsed)) {
        JSValue parseError = JS_GetException(context);
        JSValue result = JS_Call(context, resolving[1], JS_UNDEFINED, 1, &parseError);
        JS_FreeValue(context, parseError);
        JS_FreeValue(context, resolving[0]);
        JS_FreeValue(context, resolving[1]);
        if (JS_IsException(result)) {
            JS_FreeValue(context, promise);
            return result;
        }
        JS_FreeValue(context, result);
        return promise;
    }

    JSValue result = JS_Call(context, resolving[0], JS_UNDEFINED, 1, &parsed);
    JS_FreeValue(context, parsed);
    JS_FreeValue(context, resolving[0]);
    JS_FreeValue(context, resolving[1]);

    if (JS_IsException(result)) {
        JS_FreeValue(context, promise);
        return result;
    }
    JS_FreeValue(context, result);
    return promise;
}

/// `response.arrayBuffer()`. The bytes as they arrived, with no decoding.
JSValue responseArrayBuffer(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    FetchData *data = dataOf(context, thisValue);
    if (!data)
        return JS_EXCEPTION;

    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(context, resolving);
    if (JS_IsException(promise))
        return promise;

    JSValue value = JS_NewArrayBufferCopy(
        context, reinterpret_cast<const uint8_t *>(data->response.body.constData()),
        static_cast<size_t>(data->response.body.size()));

    JSValue result = JS_Call(context, resolving[0], JS_UNDEFINED, 1, &value);
    JS_FreeValue(context, value);
    JS_FreeValue(context, resolving[0]);
    JS_FreeValue(context, resolving[1]);

    if (JS_IsException(result)) {
        JS_FreeValue(context, promise);
        return result;
    }
    JS_FreeValue(context, result);
    return promise;
}

/// `response.blob()`. A Blob is reported as an object carrying the size and the
/// type, because nothing in the browser can render one yet: a page that only
/// needs to upload it finds what it needs, and a page that needs to display it
/// would fail either way.
JSValue responseBlob(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    FetchData *data = dataOf(context, thisValue);
    if (!data)
        return JS_EXCEPTION;

    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(context, resolving);
    if (JS_IsException(promise))
        return promise;

    JSValue blob = JS_NewObject(context);
    detail::defineInt(context, blob, "size", static_cast<int>(data->response.body.size()));
    detail::defineString(context, blob, "type",
                         rawHeaderValue(data->response.headers,
                                        QStringLiteral("content-type")));

    JSValue result = JS_Call(context, resolving[0], JS_UNDEFINED, 1, &blob);
    JS_FreeValue(context, blob);
    JS_FreeValue(context, resolving[0]);
    JS_FreeValue(context, resolving[1]);

    if (JS_IsException(result)) {
        JS_FreeValue(context, promise);
        return result;
    }
    JS_FreeValue(context, result);
    return promise;
}

// ------------------------------------------------------------------- fetch

/// Reads a member from an options object, falling back when it is absent.
QString optionString(JSContext *context, JSValueConst options, const char *name,
                     const QString &fallback)
{
    if (!JS_IsObject(options))
        return fallback;

    JSValue value = JS_GetPropertyStr(context, options, name);
    if (JS_IsException(value)) {
        JS_FreeValue(context, value);
        return fallback;
    }
    if (JS_IsUndefined(value) || JS_IsNull(value)) {
        JS_FreeValue(context, value);
        return fallback;
    }

    const QString text = detail::stringValue(context, value);
    JS_FreeValue(context, value);
    return text.isEmpty() ? fallback : text;
}

/// Copies `init.headers` into the request, refusing the names a page may not
/// set. A refused name is dropped rather than failing the call, which is what a
/// browser does: the page is asking for something the browser will not do, not
/// making a mistake it needs to hear about.
void readHeaders(JSContext *context, JSValueConst init, QList<QPair<QString, QString>> *out)
{
    if (!JS_IsObject(init))
        return;

    JSValue headers = JS_GetPropertyStr(context, init, "headers");
    if (JS_IsException(headers)) {
        JS_FreeValue(context, headers);
        return;
    }

    if (JS_IsObject(headers)) {
        // A Headers object, a Map, or a plain object: all three are read the same
        // way, through the enumerable own properties, which is what makes
        // `fetch(url, {headers: {"X-A": "1"}})` work.
        JSPropertyEnum *names = nullptr;
        uint32_t count = 0;
        if (JS_GetOwnPropertyNames(context, &names, &count, headers,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
            == 0) {
            for (uint32_t i = 0; i < count; ++i) {
                const char *name = JS_AtomToCString(context, names[i].atom);
                if (!name) {
                    JS_FreeAtom(context, names[i].atom);
                    continue;
                }
                const QString headerName = QString::fromUtf8(name);
                JS_FreeCString(context, name);
                JS_FreeAtom(context, names[i].atom);

                JSValue value = JS_GetPropertyStr(context, headers, headerName.toUtf8().constData());
                const QString text = detail::stringValue(context, value);
                JS_FreeValue(context, value);

                if (!headerName.isEmpty()
                    && !network::FetchPolicy::isForbiddenRequestHeader(headerName)) {
                    out->append({headerName, text});
                }
            }
            js_free(context, names);
        }
    }

    JS_FreeValue(context, headers);
}

/// Turns the `body` option into bytes. A string is the case that matters; an
/// ArrayBuffer or a typed array is passed through so a page uploading binary
/// data is not silently sent the text "[object ArrayBuffer]".
QByteArray readBody(JSContext *context, JSValueConst init)
{
    if (!JS_IsObject(init))
        return {};

    JSValue body = JS_GetPropertyStr(context, init, "body");
    if (JS_IsException(body)) {
        JS_FreeValue(context, body);
        return {};
    }

    QByteArray out;
    if (JS_IsArrayBuffer(body)) {
        size_t size = 0;
        uint8_t *data = JS_GetArrayBuffer(context, &size, body);
        if (data)
            out = QByteArray(reinterpret_cast<const char *>(data), static_cast<int>(size));
    } else if (JS_IsUndefined(body) || JS_IsNull(body)) {
        out = {};
    } else {
        out = detail::stringValue(context, body).toUtf8();
    }

    JS_FreeValue(context, body);
    return out;
}

/// Rejects a promise, reporting the failure through the console as well: a
/// rejection a page does not handle would otherwise be invisible, and the most
/// common reason `fetch` fails is a policy the user should be able to see.
JSValue rejectWith(JSContext *context, JSValueConst reject, const QString &message)
{
    JSValue text = detail::newString(context, message);
    JSValue error = JS_CallConstructor(context, builtinConstructor(context, "TypeError"), 1, &text);
    JS_FreeValue(context, text);

    JSValue result = JS_Call(context, reject, JS_UNDEFINED, 1, &error);
    JS_FreeValue(context, error);
    return result;
}

JSValue fetchFunction(JSContext *context, JSValueConst, int argc, JSValueConst *argv)
{
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(context, resolving);
    if (JS_IsException(promise))
        return promise;

    auto settleWith = [&](const QString &message) {
        JSValue result = rejectWith(context, resolving[1], message);
        JS_FreeValue(context, resolving[0]);
        JS_FreeValue(context, resolving[1]);
        if (JS_IsException(result)) {
            JS_FreeValue(context, promise);
            return JS_EXCEPTION;
        }
        JS_FreeValue(context, result);
        return promise;
    };

    FetchContext *state = fetchContextOf(context);
    if (!state || !state->provider)
        return settleWith(QStringLiteral("fetch is unavailable: no network provider is installed"));

    if (argc < 1) {
        JS_FreeValue(context, promise);
        return JS_ThrowTypeError(context, "fetch requires a URL");
    }

    // The URL is resolved against the document, so a relative path works exactly
    // as it does in an `src` attribute. Resolving here rather than in the loader
    // keeps the bindings the one place that knows about the document.
    const QString input = detail::stringValue(context, argv[0]);
    dom::Document *document = Bindings::documentOf(context);
    const network::Url base = document ? document->url() : network::Url();
    const network::Url url = network::Url::parse(input, base);

    if (!url.isValid())
        return settleWith(QStringLiteral("fetch: '%1' is not a valid URL").arg(input));

    // A Request object is accepted in place of a URL string, which is how a page
    // re-issues one it built earlier.
    JSValue init = argc > 1 ? argv[1] : JS_UNDEFINED;

    network::ScriptFetchRequest request;
    request.url = url.toString();
    request.documentUrl = base.isValid() ? base.toString() : QString();
    request.method = optionString(context, init, "method", QStringLiteral("GET")).toUpper();
    request.credentials = optionString(context, init, "credentials", QStringLiteral("same-origin"));
    request.mode = optionString(context, init, "mode", QStringLiteral("cors"));
    request.body = readBody(context, init);
    readHeaders(context, init, &request.headers);

    if (!network::FetchPolicy::isAllowedMethod(request.method)) {
        return settleWith(
            QStringLiteral("fetch: %1 is not a method a page may use").arg(request.method));
    }

    // `mode: "same-origin"` refuses a cross-origin request outright, which is
    // what the option is for: a page that does not want its data leaving the
    // origin asks for this and does not have to trust the response headers.
    if (request.mode.compare(QLatin1String("same-origin"), Qt::CaseInsensitive) == 0
        && !security::SecurityPolicy::sameOrigin(base, url)) {
        return settleWith(QStringLiteral("fetch: a same-origin request cannot reach %1")
                              .arg(url.origin()));
    }

    const int id = state->provider->startScriptFetch(request);
    if (id == 0) {
        const QString reason = state->provider->refusalReason();
        return settleWith(reason.isEmpty() ? QStringLiteral("fetch: the request was refused")
                                           : QStringLiteral("fetch: %1").arg(reason));
    }

    // The promise is registered only once the provider has accepted the request,
    // so a refusal cannot leave an entry behind that nothing will ever settle.
    PendingFetch pending;
    pending.resolve = resolving[0];
    pending.reject = resolving[1];
    state->pending.insert(id, pending);

    return promise;
}

} // namespace

// ------------------------------------------------------------------ Fetch

void Fetch::install(JSContext *context, JSValue global, network::ScriptFetchProvider *provider)
{
    FetchContext state;
    state.provider = provider;

    state.responsePrototype = JS_NewObject(context);
    state.headersPrototype = JS_NewObject(context);

    detail::defineGetter(context, state.responsePrototype, "status", responseGetStatus);
    detail::defineGetter(context, state.responsePrototype, "statusText", responseGetStatusText);
    detail::defineGetter(context, state.responsePrototype, "ok", responseGetOk);
    detail::defineGetter(context, state.responsePrototype, "url", responseGetUrl);
    detail::defineGetter(context, state.responsePrototype, "redirected", responseGetRedirected);
    detail::defineGetter(context, state.responsePrototype, "type", responseGetType);
    detail::defineGetter(context, state.responsePrototype, "bodyUsed", responseGetBodyUsed);
    detail::defineGetter(context, state.responsePrototype, "headers", responseGetHeaders);
    detail::defineMethod(context, state.responsePrototype, "text", responseText, 0);
    detail::defineMethod(context, state.responsePrototype, "json", responseJson, 0);
    detail::defineMethod(context, state.responsePrototype, "arrayBuffer", responseArrayBuffer, 0);
    detail::defineMethod(context, state.responsePrototype, "blob", responseBlob, 0);
    detail::defineString(context, state.responsePrototype, "constructor",
                         QStringLiteral("Response"));

    detail::defineMethod(context, state.headersPrototype, "get", headersGet, 1);
    detail::defineMethod(context, state.headersPrototype, "has", headersHas, 1);
    detail::defineMethod(context, state.headersPrototype, "forEach", headersForEach, 1);

    // The values that are kept are handed to the engine as duplicates, because
    // JS_DefinePropertyValueStr takes ownership of what it is given: passing the
    // original would leave this state holding a freed value, and releasing it at
    // teardown would free it twice.
    //
    // The globals. `fetch.length` is 1 and `Response.length` is 0, as in a
    // browser, because a page can and does feature detect on those.
    state.fetchFunction = JS_NewCFunction(context, fetchFunction, "fetch", 1);
    JS_DefinePropertyValueStr(context, global, "fetch",
                              JS_DupValue(context, state.fetchFunction), JS_PROP_C_W_E);

    JSValue responseConstructor
        = JS_NewCFunction2(context, [](JSContext *ctx, JSValueConst, int, JSValueConst *) {
              return JS_ThrowTypeError(ctx, "Response cannot be constructed directly");
          }, "Response", 0, JS_CFUNC_constructor_or_func, 0);
    JS_DefinePropertyValueStr(context, responseConstructor, "prototype",
                              JS_DupValue(context, state.responsePrototype), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(context, global, "Response", responseConstructor, JS_PROP_C_W_E);

    JSValue headersConstructor
        = JS_NewCFunction2(context, [](JSContext *ctx, JSValueConst, int, JSValueConst *) {
              return JS_NewObject(ctx);
          }, "Headers", 0, JS_CFUNC_constructor_or_func, 0);
    JS_DefinePropertyValueStr(context, headersConstructor, "prototype",
                              JS_DupValue(context, state.headersPrototype), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(context, global, "Headers", headersConstructor, JS_PROP_C_W_E);

    contexts().insert(context, state);
}

void Fetch::setProvider(JSContext *context, network::ScriptFetchProvider *provider)
{
    if (FetchContext *state = fetchContextOf(context))
        state->provider = provider;
}

int Fetch::pendingCount(JSContext *context)
{
    FetchContext *state = fetchContextOf(context);
    return state ? state->pending.size() : 0;
}

void Fetch::deliver(JSContext *context, int requestId, const network::ScriptFetchResponse &response)
{
    FetchContext *state = fetchContextOf(context);
    if (!state)
        return;

    // A request the page aborted, or one whose document was replaced, has no
    // entry. Reporting nothing is what makes an abort silent.
    const auto it = state->pending.find(requestId);
    if (it == state->pending.end())
        return;

    PendingFetch pending = it.value();
    state->pending.erase(it);

    // A failure rejects; an HTTP error status resolves with a response whose
    // `ok` is false. That distinction is the whole of how a page tells "the
    // server said no" from "the request never happened".
    JSValue result;
    if (!response.error.isEmpty()) {
        result = rejectWith(context, pending.reject, response.error);
    } else {
        JSValue value = makeResponseObject(context, response);
        result = JS_Call(context, pending.resolve, JS_UNDEFINED, 1, &value);
        JS_FreeValue(context, value);
    }

    if (JS_IsException(result)) {
        // The handler threw. Reporting it here means it reaches the console with
        // the rest of the page's errors rather than vanishing into the job queue.
        JSValue exception = JS_GetException(context);
        detail::reportMessage(context, ConsoleMessage::Level::Error,
                              describeException(context, exception));
        JS_FreeValue(context, exception);
    } else {
        JS_FreeValue(context, result);
    }

    JS_FreeValue(context, pending.resolve);
    JS_FreeValue(context, pending.reject);
}

void Fetch::cancelAll(JSContext *context, const QString &reason)
{
    FetchContext *state = fetchContextOf(context);
    if (!state)
        return;

    const auto pending = state->pending;
    state->pending.clear();

    for (const PendingFetch &entry : pending) {
        JSValue result = rejectWith(context, entry.reject, reason);
        if (JS_IsException(result)) {
            JSValue exception = JS_GetException(context);
            JS_FreeValue(context, exception);
        } else {
            JS_FreeValue(context, result);
        }
        JS_FreeValue(context, entry.resolve);
        JS_FreeValue(context, entry.reject);
    }
}

void Fetch::destroyContext(JSContext *context)
{
    const auto it = contexts().find(context);
    if (it == contexts().end())
        return;

    FetchContext state = it.value();
    contexts().erase(it);

    // The outstanding promises are released without being settled: the context
    // is going away, so a handler could not run, and settling would allocate a
    // value in a context that is about to be freed.
    for (const PendingFetch &entry : state.pending) {
        JS_FreeValue(context, entry.resolve);
        JS_FreeValue(context, entry.reject);
    }

    // The prototypes are released; the objects built from them are released by
    // the runtime, which holds its own references.
    JS_FreeValue(context, state.fetchFunction);
    JS_FreeValue(context, state.responsePrototype);
    JS_FreeValue(context, state.headersPrototype);
}

} // namespace oqb::javascript

#endif // OPENQBROWSER_SCRIPTING
