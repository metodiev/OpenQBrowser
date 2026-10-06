# Storage

`src/storage/` holds three in-memory models, and `src/network/` holds the HTTP
cache. Nothing is written to disk.

| File | Contents |
| --- | --- |
| `src/storage/History.h`, `History.cpp` | `HistoryEntry`, `HistoryStore`. |
| `src/storage/Bookmarks.h`, `Bookmarks.cpp` | `Bookmark`, `BookmarkStore`. |
| `src/storage/Cookies.h`, `Cookies.cpp` | `Cookie`, `SameSite`, `CookieJar`. |
| `src/network/Cache.h`, `Cache.cpp` | `CachePolicy`, `CacheEntry`. |

None of them includes a Qt I/O class. There is no `QSettings`, no `QFile` read or
write, no `QStandardPaths` use anywhere in `src/`, so a reader can be certain that
**nothing survives the process**: history, bookmarks, cookies and cached responses
are all gone when OpenQBrowser exits.

## What exists: history

`HistoryEntry` is `url`, `title`, `visitedAt` (ms since the epoch) and
`visitCount`. `HistoryStore` keeps a `QList<HistoryEntry>` and an integer cursor
`m_currentIndex`, and is documented in `History.h` as the model the back and
forward buttons drive: a tab holds an index into this list rather than its own
copy.

Semantics, all in `History.cpp`:

* `visit(url, title)` — if the URL equals the entry at the cursor, that entry's
  timestamp and count are updated and its title replaced when a non-empty one is
  given; otherwise every entry after the cursor is removed (forward truncation),
  a new entry is appended, and the cursor moves to it. `kMaxEntries = 5000`; when
  the list grows past it, entries are removed from the front and the cursor is
  decremented by the same amount.
* `updateCurrentTitle(title)` — replaces the title of the cursor entry, for a page
  that reports one late. `Page` does not call it today; `recordHistory()` runs at
  the end of the load, when the title is already known.
* `goBack()`, `goForward()`, `goTo(index)` — move the cursor; each returns false
  when the move is impossible. They never record a visit.
* `currentUrl()`, `currentTitle()`, `canGoBack()`, `canGoForward()`,
  `currentIndex()`, `count()`, `at(index)`, `entries()`.
* `search(text, limit)` — case-insensitive substring match on URL or title, newest
  first; with an empty needle it returns the most recent entries. This is what an
  address-bar dropdown would use.
* `mostVisited(limit)` — sums `visitCount` per URL in a `QHash`, sorts by count
  and breaks ties by the more recent visit.
* `describe()` — the plain-text listing with `*` marking the cursor, used by
  `about:history`.
* `clear()` — empties the list and sets the cursor to -1, described in the header
  as what a private browsing window does when it closes.

Ownership: `ui::MainWindow` owns one `HistoryStore` and passes it to every tab
with `Tab::setHistory()`, which forwards it to `Page::setHistory()`. `Page` stores
a non-owning pointer and calls `visit()` from `recordHistory()` at the end of a
load, when `PageSettings::recordHistory` is on, a store is attached, and the page
is not an error page. `about:history` and the new tab page read the same store
live through `builtin::documentFor()`.

## What exists: bookmarks

`Bookmark` is `url`, `title`, `addedAt`, `folder` (empty means top level).
`BookmarkStore` keeps a `QList<Bookmark>`:

* `add(url, title, folder)` — updates the existing entry for that URL rather than
  duplicating it, filling in the title with `Url::displayHost()` when none is
  given.
* `remove(url)`, `contains(url)`, `toggle(url, title)` (returns the new state),
  `count()`, `isEmpty()`, `bookmarks()`, `clear()`.
* `inFolder(folder)` — the entries in a folder, or every entry when `folder` is
  empty. Folders are just a name on an entry; there is no folder object and no
  nesting.
* `folders()` — the distinct non-empty folder names, sorted.
* `search(text, limit)` — newest first.
* `toHtml()` — the `about:bookmarks` document, grouped by folder with an "Other"
  section for the top level, escaped by a local `escape()` helper.

`BookmarkStore.h` states the position plainly: bookmarks live only in memory for
now, persistence is planned alongside a storage directory, and keeping the API
separate from a file format means adding persistence later does not disturb the
menus that use it. The toolbar's star in
`MainWindow::onToggleBookmark()` is the only writer.

## What exists: cookies

`Cookie` is a value type — `name`, `value`, `domain`, `path`, `expires`,
`persistent`, `secure`, `httpOnly`, `sameSite` and `created`. `CookieJar` holds a
`QList<Cookie>` and speaks the parts of RFC 6265 a browser needs:

* `store(setCookie, url)` — parses one `Set-Cookie` value and stores it, returning
  whether the jar changed. An expired cookie is a deletion, which is why the
  return value is "changed" rather than "stored".
* `storeFromHeaders(headers, url)` — every `Set-Cookie` in a response, since a
  response may carry several and each is one cookie. Returns how many were stored.
* `requestHeader(url)` — the `Cookie` header value, or empty.
* `cookiesFor(url)` — the cookies that would be sent, for the inspector.
* `all()`, `count()`, `isEmpty()`, `clear()`, `pruneExpired()`.

The rules the implementation enforces, each of which has a matching case in
`tests/unit/tst_cookies.cpp`:

* **Scheme.** Only `http` and `https` may set or receive a cookie. The test is
  `Url::isRemote()`, not `Url::isHttp()`: the latter is true only for the plain
  scheme, and testing it alone refuses every cookie an HTTPS site sets.
* **Domain.** An explicit `Domain` is honoured only when it is the host that set
  the cookie or a dot-boundary suffix of it. An unacceptable domain **refuses the
  cookie outright** instead of silently narrowing it to the host, because narrowing
  would store a cookie the page asked not to be host-only. A literal IPv4 address
  may only be matched exactly, so `127.0.0.1` cannot set a cookie for `0.0.1`.
* **Path.** A request path matches a cookie path when the cookie path is a prefix
  ending on a `/` boundary. An absent `Path` defaults to the directory of the
  request URL, which is why one page's cookie does not leak to a sibling.
* **Expiry.** `Max-Age` wins over `Expires`. `pruneExpired()` runs after a store and
  is implicit in a lookup, so the jar neither grows without bound nor returns a
  stale entry.
* **`Secure` and `HttpOnly`.** `Secure` cookies are only sent over `https`.
  `HttpOnly` is recorded and enforced by absence: `document.cookie` does not exist
  in this engine, so no script can read any cookie, let alone an `HttpOnly` one.
* **`SameSite`.** Parsed and recorded, and the outbound decision is delegated to
  `security::SecurityPolicy::canSendCookies()`, which refuses the whole request's
  cookie set rather than a partial one.
* **Ordering.** Longest path first, then earliest created, on a stable sort, so a
  more specific cookie wins and two cookies for the same path keep their age order.

Outbound, the header is written with `set()` rather than `append()`: a caller may
have supplied a `Cookie` header of its own, and two `Cookie` headers are not
equivalent to one.

### Where it is wired

`ResourceLoader` owns the decision, not `Cookie`:

* `setCookieJar()` / `cookieJar()` — the jar, or none. Every path tolerates a null
  jar, so a loader without storage still works.
* `applyCookies(request, referrer)` — consults the policy, then builds the header.
* `absorbCookies(response, url)` — stores what the response set. It runs **first**
  in `handleHttpResponse()`, before the error check, because a response that is
  about to be discarded as an error still sets cookies — which is how a server
  reports a failed login. It stores against `response.finalUrl` when that is valid,
  since the host that sent the header is the host the cookie belongs to.

A redirect is followed inside `HttpClient`, so its response never reaches
`ResourceLoader`. Two seams exist for that reason, and both are needed:

* `HttpClient::redirectResponse(response)` — emitted for a redirect before the next
  hop is sent, so `absorbCookies()` can see headers that never reach `finished()`.
* `HttpClient::setCookieProvider()` — a callback the client asks for each hop's
  `Cookie` header. A redirect may have just set or replaced a cookie, so the header
  that suited the previous hop is not the header that suits the next. Without this
  the first hop's header would be copied onto every later hop, sending a cookie to a
  host the jar would refuse and omitting one the redirect just set.

Ownership: `ui::MainWindow` owns one `CookieJar` per window and gives it to every
tab with `Tab::setCookieJar()`, which forwards to `Page::setCookieJar()`. The jar is
a window's, not a tab's, so two tabs on the same host share a session — which is
what makes a login in one tab visible in the other. The headless `main.cpp` path
creates a jar of its own for the same reason.

`tests/integration/tst_cookie_flow.cpp` covers the wiring over a real loopback
socket: a cookie is stored and returned, a redirect's cookie survives, a relative
`Location` is resolved before the cookie is sent, and each hop's header is derived
separately. The DevTools panel shows the whole jar in its **Cookies** tab, marking
with `!` the cookies the jar would not send to the page on screen.

## What exists: the HTTP cache

`src/network/Cache.{h,cpp}` holds the policy, and `ResourceLoader` holds the
entries. The split is deliberate: the freshness arithmetic is the part that is
easy to get subtly wrong, and it is a pure function of headers, so it is testable
without a socket.

`CachePolicy` decides four things:

* **Storability.** `no-store` refuses outright. So do `206` (part of a body) and
  `304` (a verdict, not a representation). So does any `4xx` or `5xx`, whatever
  its `Cache-Control` says: a stored failure outlives the outage that caused it.
* **Lifetime.** `max-age` when present, else `Expires - Date` (never
  `Expires - now`, which would drift with the clock), else a tenth of the time
  since `Last-Modified`, capped at a day.
* **Freshness.** An entry is fresh when its *age* is below its lifetime. Age is
  measured from the origin's `Date`, not from arrival, because a proxy may have
  held the response before this browser saw it.
* **`Vary`.** A response varying on a header the cache does not key on is not
  stored, since serving one variant for another is the bug `Vary` prevents.
  `Accept-Encoding` is the one header treated as harmless, because this browser
  sends a single fixed value.

`CacheEntry` keeps the `Resource` whole, and that is not incidental: a
revalidation answers `304` with no body, so the stored body is what gets served.
Discarding it would turn every successful revalidation into an empty response.

### Two reuse rules, not one

An entry is reused without a request in two different circumstances, and mixing
them up produces either a broken page or a slow one:

* **Within one document load**, any stored entry is reused whatever its headers
  say. This is a memo, not a cache: a page that references the same stylesheet ten
  times makes one request, and a missing image is not retried once per element
  that points at it. `generation` on the entry is what scopes this.
* **Across loads**, the freshness rules decide. A fresh entry is served with no
  request at all; a stale one with a validator becomes a conditional request
  whose `304` costs a round trip and no body.

`Page::load()` no longer clears the cache. It calls `beginLoad()`, which bumps the
generation so the previous page's entries stop counting as this load's memo while
remaining available under their own headers. Before this, every navigation threw
away every response, so two pages sharing a stylesheet downloaded it twice —
`tst_cache_flow.cpp::aSecondPageReusesASharedStylesheet()` pins exactly that.

`Page::reload()` is the exception: it calls `invalidate()` on the document and
every pending subresource, so a reload reaches the origin even for an entry that
is still fresh. A reload marker is deliberately *not* consumed by the first cache
lookup, because `startRequest()` consults the cache a second time after the queue
and would otherwise serve the very entry the reload was meant to bypass. It is
cleared when the response arrives or the request fails, so it cannot leak into a
later fetch of the same URL.

### What the cache does not do

* **Nothing survives the process.** There is no disk cache.
* **No `stale-while-revalidate` / `stale-if-error`.** Both are optional, and both
  would serve an unconfirmed body.
* **No `s-maxage`.** It addresses shared caches; this one serves a single user.
* **No partial-content reuse.** `206` responses are not stored.
* **No eviction policy.** Entries are dropped when the loader is destroyed or
  `clearCache()` is called. A size cap belongs with the disk cache, where the
  problem is real.

## What is planned

| Feature | State |
| --- | --- |
| Cookie jar | **Implemented, in memory.** `Set-Cookie` parsing, origin- and path-scoped storage, expiry, `Secure`, `HttpOnly` and `SameSite`, plus the `Cookie` request header for every hop of a redirect chain. Dies with the process. |
| HTTP cache | **Implemented, in memory.** Conditional caching: `Cache-Control`, `Expires` and the `Last-Modified` heuristic for freshness, and `ETag` / `Last-Modified` for revalidation, so a stale entry costs a round trip and no body instead of a re-download. Dies with the process. |
| Persistence of history and bookmarks | Not implemented. |
| Session restore | Not implemented; a new window always opens one tab on `about:home`. |
| Downloads directory, settings file, profile directory | Not implemented. |
| Private browsing | Not implemented; `HistoryStore::clear()` is the closest thing. |

`.gitignore` reserves `.openqbrowser/` as "local runtime state", which is where a
profile directory would go, but no code reads or writes that path today.

## Design constraints for adding persistence

The existing code shapes what a persistence layer must be, rather than the other
way round. These constraints come from the code as it stands:

1. **The stores are plain models with no signals and no I/O.** `HistoryStore` and
   `BookmarkStore` are value types with no `QObject` base, no `changed()` signal
   and no file handling. A persistence layer should therefore be a separate owner
   that loads a snapshot at startup and writes after mutations, or the stores must
   become `QObject`s with change signals. Adding file I/O inside the existing
   methods would put a blocking write on the UI thread of every navigation.

2. **`HistoryStore` needs `updateCurrentTitle`, and `visit()` needs to be cheap.**
   Because `visit()` is called by `Page::recordHistory()` at the end of every load,
   any write it triggers is on the load-completion path. A debounce or a
   write-behind queue is required for a responsive window; the current API gives
   nothing to debounce on, so a change signal is the natural addition.

3. **Timestamps are already recorded.** `visitedAt` and `addedAt` are
   `QDateTime::currentMSecsSinceEpoch()`; `visitCount` is maintained; `folder` is
   a plain string. A serialised form can be generated from the existing fields
   without changing the model.

4. **URLs must round-trip through `Url::parse()`.** `BookmarkStore::add()` and
   `HistoryStore::visit()` accept a `network::Url`, and
   `ResourceLoader`'s cache is keyed by `Url::toString()`. A stored URL is
   therefore the absolute URL text, and loading it must go through
   `Url::parse()` so normalisation (case, default port, dot segments) is applied
   identically on the way in and on the way out.

5. **The cookie jar is not keyed by origin, and that is deliberate.**
   `security::originKey(url)` was written for a jar keyed by
   `scheme://host:effectivePort`, but the jar as built keys on `Domain` and `Path`
   instead, because that is what RFC 6265 specifies and what servers expect: a
   cookie set on `example.com` has to reach `www.example.com`, and an origin key
   would prevent that. The two rules in `security.md` are preserved by other
   means — `SecurityPolicy::canSendCookies()` refuses the insecure case before the
   header is built, and `HttpClient::handleRedirect()` strips `Cookie` **and**
   `Authorization` on a cross-host hop, then re-derives the cookie header from the
   jar for the new host. `originKey()` therefore still has no callers, and is
   needed only if storage partitioning (one jar per top-level site) is added.

6. **A disk cache must preserve the framing decision, not just the bytes.**
   `HttpResponse::body` reaching `ResourceLoader` has already had chunked framing
   and content encoding removed, and the cache is keyed by the normalised URL
   string. A persistent cache that stored the raw wire bytes would need to repeat
   that work; one that stores the decoded body plus `mimeType`, `statusCode` and
   the response headers preserves the existing contract. `CacheEntry` is already
   the natural on-disk record, and `ResourceLoader::absorbIntoCache()` /
   `tryCache()` are already the two entry points to interpose. What a disk cache
   adds is serialisation and an eviction policy; the freshness rules it needs are
   in place.

7. **Cache invalidation data now exists, and is enforced in the live path.**
   `CachePolicy` reads `Cache-Control` (`max-age`, `no-store`, `no-cache`,
   `must-revalidate`, `public`, `private`), `Expires` and `Last-Modified`, and
   `ResourceLoader` sends `If-None-Match` and `If-Modified-Since` and merges a 304
   into the entry it validates. `Vary` is honoured by declining to store a
   response that varies on a header the cache does not key on. This was the
   precondition for a disk cache and is now met; what a disk cache adds is
   surviving a restart, not correctness.

8. **Where the files live.** Qt's `QStandardPaths::AppDataLocation` with the
   application name and version that `main.cpp` already sets
   (`QCoreApplication::setApplicationName(OPENQBROWSER_APP_NAME)`) is the
   conventional answer; `.gitignore`'s `.openqbrowser/` entry means a project-local
   profile directory is also anticipated. Either way, the path must be created on
   demand and a failure to write must not break a navigation — the same
   `Decision`-style explicitness the security policy uses applies here: record the
   failure and continue with an empty store.

9. **Testing.** The rule in `contributing.md` applies: a persistence change comes
   with tests, which means the file path must be injectable rather than hard-coded
   inside a store. `QTemporaryDir` is already used by
   `tests/integration/tst_http.cpp::loaderFetchesLocalFiles()` and is the obvious
   tool for a store test.

10. **Error pages must not be recorded.** `Page::recordHistory()` already skips a
    page whose `isErrorPage()` is true, and a persistence layer should preserve
    that: an error page in a saved history would make a broken link look like a
    visited site.
