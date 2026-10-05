# Storage

`src/storage/` currently holds two in-memory models, and nothing is written to
disk.

| File | Contents |
| --- | --- |
| `src/storage/History.h`, `History.cpp` | `HistoryEntry`, `HistoryStore`. |
| `src/storage/Bookmarks.h`, `Bookmarks.cpp` | `Bookmark`, `BookmarkStore`. |

Neither file includes a Qt I/O class. There is no `QSettings`, no `QFile`, no
`QStandardPaths` use anywhere in `src/`, so a reader can be certain that **nothing
survives the process**: history, bookmarks, cookies and any future cache are gone
when OpenQBrowser exits.

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

## What is planned

| Feature | State |
| --- | --- |
| Cookie jar | Not implemented. No `Set-Cookie` parsing, no storage, no `Cookie` request header. |
| HTTP cache | Not implemented beyond `ResourceLoader`'s in-memory map, which is cleared by `Page::stop()` and dies with the process. |
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

5. **A cookie jar should be keyed by origin, not by host string.**
   `security::originKey(url)` exists for exactly this and has no callers yet. The
   two rules in `security.md` — refusing to send cookies to an insecure origin,
   and stripping credentials on a cross-host redirect — are written against
   origins, so a store keyed by
   `scheme://host:effectivePort` keeps those rules enforceable. Cookie attributes
   (`Domain`, `Path`, `Expires`, `Max-Age`, `Secure`, `HttpOnly`, `SameSite`) would
   have to be parsed and honoured; none of that exists.

6. **A disk cache must preserve the framing decision, not just the bytes.**
   `HttpResponse::body` reaching `ResourceLoader` has already had chunked framing
   and content encoding removed, and `ResourceLoader`'s cache key is the
   normalised URL string. A persistent cache that stored the raw wire bytes would
   need to repeat that work; one that stores the decoded body plus `mimeType`,
   `statusCode` and the response headers preserves the existing contract. The
   `Resource` struct is already the natural on-disk record, and
   `ResourceLoader::store()`/`cached()` are already the two entry points to
   interpose.

7. **Cache invalidation data does not exist.** No code reads `Cache-Control`,
   `ETag`, `Last-Modified` or `Expires`; `HttpRequest` never sends
   `If-None-Match` or `If-Modified-Since`. A persistent cache without those is a
   cache that can only be cleared by hand, so freshness rules come first.

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
