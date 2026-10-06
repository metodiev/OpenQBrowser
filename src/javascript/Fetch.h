#pragma once

#include <QString>

#ifdef OPENQBROWSER_SCRIPTING
#include <quickjs.h>
#endif

namespace oqb::network {
class ScriptFetchProvider;
struct ScriptFetchResponse;
}

namespace oqb::javascript {

/// The seam between `fetch()` and the network.
///
/// The bindings need the loader, but they cannot own it and the JS engine must
/// keep working in a build or a test with no network at all. So the provider is
/// installed on the engine rather than reached for: an engine with none reports
/// `fetch` as unavailable and rejects rather than hanging, and a test installs a
/// fake provider and exercises the whole of the JavaScript-facing behaviour -
/// promise settling, headers, the body accessors - without a socket.
class Fetch
{
public:
#ifdef OPENQBROWSER_SCRIPTING
    /// Installs `fetch`, `Response`, `Headers` and `Request` on `global`, and
    /// remembers `provider` as where requests go.
    ///
    /// A null provider still installs the globals, because a page that feature
    /// detects `fetch` and then finds it broken is harder to explain than one
    /// whose fetch rejects with a reason. The rejection says no provider was
    /// installed, which is what an inspector shows.
    static void install(JSContext *context, JSValue global, network::ScriptFetchProvider *provider);

    /// Points fetch() at `provider`, replacing any installed before.
    ///
    /// Separated from install() because the two happen at different times: the
    /// globals are created with the engine, while the loader that answers them is
    /// attached once a document exists. A page therefore always sees `fetch`, and
    /// only a build that never attaches a provider sees it refuse.
    static void setProvider(JSContext *context, network::ScriptFetchProvider *provider);

    /// Hands one response to the promise that is waiting for it. Called by the
    /// browser when the loader reports a script request finished.
    ///
    /// A response with no id, or one nothing is waiting for, is ignored: an
    /// abort deliberately leaves a promise pending, and a request that outlived
    /// its document must not resolve into a context that is being torn down.
    static void deliver(JSContext *context, int requestId, const network::ScriptFetchResponse &response);

    /// Settles every outstanding fetch with a failure. Called when the document
    /// is replaced, so a promise from the old page cannot resolve into the new
    /// one, and at teardown, so nothing holds a value of a freed context.
    static void cancelAll(JSContext *context, const QString &reason);

    /// How many fetches are waiting. The browser uses it to keep its frame clock
    /// running while a request is in flight, which is what lets a promise
    /// created by a script settle after the script returned.
    static int pendingCount(JSContext *context);

    /// Frees every value the fetch bindings hold for a context. Called before the
    /// context is destroyed.
    static void destroyContext(JSContext *context);
#endif
};

} // namespace oqb::javascript
