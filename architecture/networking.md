# Networking

`src/network/` implements the first stage of the pipeline: turning a URL into
bytes. It contains an RFC 3986 URL type, an HTTP message model, a hand-written
HTTP/1.1 client that runs on `QTcpSocket` and `QSslSocket`, and a loader that
caches resources and caps concurrency.

| File | Contents |
| --- | --- |
| `src/network/Url.h`, `Url.cpp` | `Url`: parsing, normalisation, reference resolution, origins. |
| `src/network/HttpMessage.h`, `HttpMessage.cpp` | `HeaderList`, `HttpRequest`, `HttpResponse` and the `http::` helpers. |
| `src/network/HttpClient.h`, `HttpClient.cpp` | The HTTP/1.1 state machine over TCP or TLS. |
| `src/network/ResourceLoader.h`, `ResourceLoader.cpp` | `Resource`, the in-memory cache and the request queue. |

`Url::parse()` is used instead of `QUrl` deliberately: the parsing rules are the
part of a browser most often hidden behind a library, and the code comments name
the RFC section each rule comes from.

## Url: parsing and RFC 3986 reference resolution

`Url` holds `m_scheme`, `m_userInfo`, `m_host`, `m_port`, `m_path`, `m_query`,
`m_fragment` plus `m_hasAuthority`, `m_hasQuery`, `m_hasFragment` and `m_valid`.
The `has*` flags matter because `http://host` and `http://host?` are different
URLs, and because `#top` on a page with no query must not invent one.

`Url::parse(input, base)` follows RFC 3986 §4.3 and §5.2:

1. A prefix matching `scheme:` (letters, digits, `+`, `-`, `.`, then `:`) is taken
   as a scheme. Otherwise the base's scheme, authority, host and port are
   inherited; with no base the result is invalid.
2. `//authority` — only when written in the reference itself — sets
   `m_hasAuthority` and is split into `userInfo@host:port`. IPv6 literals in
   `[...]` are handled; a missing `]` invalidates the URL. An empty host
   invalidates everything except `file`.
3. Credentials are dropped: `m_userInfo.clear()` before the URL is used, so they
   can never be sent on the wire. The address bar shows `Url::displayHost()`.
4. The fragment and then the query are split off, in that order.
5. The path is resolved per §5.2.2: an empty path keeps the base's path (so `?q=1`
   and `#top` work); a path starting with `/` is absolute; a relative path is
   joined to the base's directory (everything up to the last `/`).
6. `normalize()` lower-cases scheme and host, drops default ports
   (`80` for http, `443` for https), gives http/https an empty path a `/`, and
   runs `removeDotSegments()` (RFC 3986 §5.2.4). That helper preserves the
   trailing slash: `/a/b/..` becomes `/a/`, not `/a`.

Two deliberate tolerances, both commented in `Url.cpp`:

* `http:example.com` — a scheme with no authority and no leading `/` is treated
  as `http://example.com/`, which is what browsers do.
* `Url::fromUserInput()` only treats a `scheme:` prefix as a scheme when the name
  is in a small allow list (`http`, `https`, `file`, `about`, `data`, `ftp`,
  `view-source`, `mailto`). Otherwise `localhost:8080/x` would parse with the
  scheme `localhost` — the classic mistake. Input that does not look like a host
  (no `.`, no `:`, not `localhost`) returns an invalid URL, and the caller builds
  a search URL with `Url::forSearchQuery(query, template)`, which substitutes the
  percent-encoded query into the template's `%s`.

Useful accessors: `effectivePort()`, `origin()` (`scheme://host:port`),
`isSecure()` (https or about), `isRemote()`, `toRequestTarget()` (path + query,
percent-encoding the unsafe characters and never the fragment),
`toString()`, `toLocalFile()`, `aboutPage()`, `displayHost()` (strips `www.`).

## HeaderList

`HeaderList` is an ordered `QList<QPair<QString, QString>>` rather than a map, so
that header order and original casing survive for the inspector. Lookups are
case-insensitive: `contains()`, `value(name, default)`, `values(name)`,
`joined(name)` (comma-joins the repeated headers, which is how `Transfer-Encoding`
and `Content-Encoding` are read). `set()` removes then appends; `append()` keeps
duplicates, as HTTP allows.

## HttpRequest and HttpResponse

`HttpRequest` carries `method` (`Get`, `Post`, `Head`), `url`, `headers`, `body`,
`maxRedirects` (default 20), `timeoutMs` (default 30000) and `userAgent`.
`serialize()` writes the request line, then fills in defaults the caller did not
set: `Host` (host plus explicit port), `User-Agent` (`http::defaultUserAgent()`),
`Accept`, `Accept-Encoding: gzip, deflate`, `Connection: close` and, for POST,
`Content-Length`. `Connection: close` is what makes the read-until-close framing
rule below safe. The body is appended for every method except HEAD.

`HttpResponse` carries `statusCode`, `reasonPhrase`, `httpVersion`, `headers`,
`body` and `finalUrl` (the URL after redirects). Helpers: `isSuccess()`,
`isRedirect()` (301/302/303/307/308), `isError()`, `contentType()`, `charset()`,
`text()`, `location()`, `statusText()` and `reasonForStatus()`. `text()` decodes
the body with the declared charset; when the server sent no charset and the type
is `text/html`, it sniffs a `<meta ... charset=...>` in the first 4 KiB, which is
what the HTML standard prescribes. Unknown charset labels fall back to
Windows-1252, matching browser rules. `http::codecNameForCharset()` holds the
label aliases (`latin1` → `ISO-8859-1`, `gb2312` → `GBK`, and so on).

## HttpClient: the state machine

`HttpClient` is a `QObject` that performs exactly one request chain and emits
exactly one of `finished(HttpResponse)` or `failed(QString)`. Instances are cheap;
`ResourceLoader` creates one per resource. Its states are

```
  Idle --send()--> Connecting --connected/encrypted--> Sending --bytes--> Reading
                     ^                                                    |
                     |                                                    |
                     +---------------- Redirecting <--- 3xx + Location <--+
```

Signals: `finished`, `failed`, `redirected(from, to)`, `progress(received,
expected)`, `connected`. `isRunning()`, `currentUrl()` and `elapsedMs()` (driven
by a `QElapsedTimer` started in `send()`) support the UI and the inspector.

The socket is created in `beginRequest()`: `QSslSocket` for https, `QTcpSocket`
for http. For TLS the client calls `connectToHostEncrypted(host, port)` rather
than `connectToHost()`, because a `QSslSocket` that is not told otherwise would
put plaintext bytes on a TLS port; the host name passed here is also the one used
for certificate verification and SNI. `onConnected()` writes the request only for
plain sockets; for TLS the write happens in `onEncrypted()`, so no plaintext byte
ever reaches the wire.

`http::installSystemCaCertificates()` is called at the start of `send()`. It looks
for a CA bundle in a list of well-known paths (`/etc/ssl/cert.pem`,
`/opt/homebrew/etc/ca-certificates/cert.pem`, …) when Qt's default configuration
has no certificates, which is the situation on a minimal Linux install.
### Timeouts, limits, cancellation

* One timer covers the whole chain, including redirects: `timeoutMs` from
  `HttpRequest` is (re)started in `beginRequest()`, so each hop gets a fresh
  budget. Firing calls `fail("The request timed out after N seconds")`.
  `HttpRequest::timeoutMs` defaults to 30000, and a value of 0 disables the timer.
  Note that `ResourceLoader::startRequest()` builds the `HttpRequest` itself and
  does not copy `PageSettings::requestTimeoutMs` into it, so every network request
  the browser makes today runs with the 30-second default.
* `kMaxBodyBytes` is 64 MiB. It is enforced twice: as the receive buffer grows in
  `onReadyRead()`, and immediately from a declared `Content-Length` that exceeds
  it. Headers are separately capped at 512 KiB without a `\r\n\r\n`.
* `abort()` stops the timer, disconnects and aborts the socket, and emits nothing.
  `ResourceLoader::cancelAll()` uses it.
* `onSslErrors()` never ignores an error: it collects `errorString()` from each
  `QSslError` and fails with `"TLS certificate verification failed: …"`. There is
  no override flag and no "proceed anyway" path anywhere in the client.

## Body framing

After the status line and headers are parsed, `parseAvailable()` chooses one of
three framings. This order matters and is the order in the code:

| Condition | Framing | Where |
| --- | --- | --- |
| HEAD, 204, 304, or 1xx | no body (`m_expectedBody = 0`) | `parseAvailable()` |
| `Transfer-Encoding` contains `chunked` | chunked | `http::decodeChunked()` |
| `Content-Length` present | fixed length | counted in `parseAvailable()` |
| neither header | read until the peer closes | `m_readUntilClose`, completed in `onDisconnected()` |

Details worth knowing:

* A non-numeric or negative `Content-Length` fails the request
  (`"Invalid Content-Length header"`).
* `decodeChunked()` parses hex sizes, ignores chunk extensions after `;`,
  requires each chunk's trailing CRLF, consumes trailer headers up to the empty
  line, and returns `{false, {}}` on any malformation. `parseAvailable()` turns
  that into `Incomplete`, so a partially received chunk waits for more bytes.
* For read-until-close, `RemoteHostClosedError` is not an error: `onSocketError()`
  forwards it to `onDisconnected()`, which marks the state `Idle` first — the
  guard that stops `disconnected` and `errorOccurred` from reporting the same
  close twice — then finalises the body and emits `finished`. A close in any other
  state fails with `"The connection was closed before a complete response was
  received"`.

## Content encodings

`http::decodeContentEncoding(encoding, body)` handles the encodings advertised in
`Accept-Encoding`:

* `identity` or empty — passed through.
* `gzip` — zlib's `inflateInit2(&stream, 16 + MAX_WBITS)` to select gzip framing,
  inflating in 64 KiB chunks. A truncated stream still yields everything decoded
  so far, which is what browsers render when a connection is cut mid-response.
* `deflate`, `x-deflate` — zlib framing (`MAX_WBITS`) is tried first, then raw
  deflate (`-MAX_WBITS`), because servers disagree about which one `deflate`
  means. Only a complete `Z_STREAM_END` counts as success.
* Anything else — returned untouched rather than treated as an error.

Chunked decoding happens before content decoding, and both happen before the body
reaches `ResourceLoader`, so `HttpResponse::body` is always the decoded payload.

## Redirects

`handleRedirect(response)` runs when a 3xx arrives with a `Location` header:

1. `HttpResponse::location()` resolves the header against `finalUrl`, so a
   relative `Location` works.
2. A target that is not http/https fails with `"Redirect to an unsupported location"`.
3. The hop counter `m_redirectsForOriginal` is compared against
   `m_pending.maxRedirects`; exceeding it fails with `"Too many redirects (20)"`.
4. Method rewriting follows RFC 7231 §6.4: 303 always becomes GET, and 301/302
   from a POST becomes GET as well, dropping the body, `Content-Type` and
   `Content-Length`. 307 and 308 keep the method and body.
5. **Credential stripping:** if the new host differs from the current one,
   `Authorization` and `Cookie` are removed from the outgoing headers. Credentials
   must not leak to another origin.
6. The receive buffer and the framing fields are reset, because bytes already read
   belong to the redirect response, and `redirected(from, to)` is emitted before
   the next request begins.

## ResourceLoader

`ResourceLoader` is what `Page` actually calls. `fetch(url, referrer)`:

* emits `finished` synchronously for a cache hit, an invalid URL, a non-remote
  scheme, or a `file://` URL (read with `loadLocalFile()`, which uses
  `QMimeDatabase` to guess the type), otherwise
* appends to `m_queue` and calls `pump()`.

`pump()` starts queued requests while fewer than `m_maxConcurrent` (default 6,
set from `PageSettings::maxConcurrentRequests`) clients are in flight — one
`HttpClient` per request, so requests genuinely overlap. Each request is started
with `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` so `fetch()` always
returns before any signal is emitted.

The cache is a `QHash<QString, Resource>` keyed by `Url::toString()`:

* Successes **and failures** are cached, so a broken URL is not retried on every
  repaint. `Resource::ok()` is `error.isEmpty()`.
* A 301 or 308 response is stored twice: under the final URL and under the
  original, so a later lookup of the old URL resolves without another round trip.
* `clearCache()`, `cacheSize()`, `cached()`, `store()` and `requestCount()` are
  public — the tests in `tests/integration/tst_http.cpp` use them directly, and
  `Page::reload()` -> `stop()` clears the cache.

`Resource` carries `url`, `data`, `mimeType`, `error` and `statusCode`, plus
`isHtml()`, `isCss()`, `isImage()`, `isScript()` and `text()` (decodes with the
MIME charset, defaulting to UTF-8 — subresources rarely declare one).

## What is not here

* HTTP/2 and HTTP/3; QUIC; ALPN beyond what Qt negotiates by default.
* Connection reuse: every request opens a socket and sends `Connection: close`.
* Conditional requests (`If-None-Match`, `If-Modified-Since`), `Cache-Control`,
  `ETag` or disk caching. The only cache is the in-memory `ResourceLoader` map.
* Proxy configuration, HSTS preload lists (see `security.md`), cookies beyond the
  stripping rules, and `Authorization` handling outside redirects.
* `Content-Type` sniffing beyond the `<meta charset>` rule and the MIME database
  for local files; a page served as `application/octet-stream` is not rendered as
  HTML.
