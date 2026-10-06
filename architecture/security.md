# Security

`src/security/SecurityPolicy.{h,cpp}` is a small, explicit set of checks, and the
rest of the security posture is a property of the network and browser code rather
than of that file. The model is: **every default is safe, every relaxation is
explicit, and every refusal carries a reason.** A `Decision` is
`{bool allowed, QString reason}` with `Decision::allow()` and
`Decision::deny(reason)` factories, so a caller can always explain a refusal
instead of appearing to do nothing.

`about:about` itself states the position: OpenQBrowser verifies TLS certificates
and refuses to downgrade a secure page, "but it has not been audited and has no
sandbox."

## Strict TLS verification, with no bypass

`HttpClient` creates a `QSslSocket` for an `https` URL and calls
`connectToHostEncrypted(host, port)`; that call is what supplies the host name for
certificate verification and SNI, and what stops plaintext bytes from being
written to a TLS port (`onConnected()` only writes for non-TLS sockets;
`HttpClient.cpp:onEncrypted()` writes for TLS).

`HttpClient::onSslErrors()` collects `error.errorString()` from every `QSslError`
and fails the request:

```cpp
fail(tr("TLS certificate verification failed: %1").arg(messages.join("; ")));
```

There is no `ignoreSslErrors()` call anywhere in the tree, no environment variable
or command-line switch to relax verification, and no "proceed anyway" page. A host
whose certificate cannot be verified produces the `tls` error page
("Secure connection failed. OpenQBrowser refuses to load a page whose identity it
cannot confirm.") and no content is fetched.

`http::installSystemCaCertificates()` exists to make verification *work*, not to
weaken it: it is called at the start of `HttpClient::send()` and, when Qt's default
configuration has no CA certificates, loads a system bundle from a list of
well-known paths (`/etc/ssl/cert.pem`,
`/opt/homebrew/etc/ca-certificates/cert.pem`, `/etc/ssl/certs/ca-certificates.crt`,
and so on) with `QSslCertificate::fromData(..., QSsl::Pem)`. If no bundle is found
the default configuration is left alone, and verification fails closed.

Protocol negotiation is whatever Qt's `QSslSocket` does with its default
configuration; OpenQBrowser sets no minimum protocol version and does not pin
ciphers. That is a deliberate reliance on Qt's defaults, and it is one of the
things the word "unaudited" covers.

## No insecure subresources from a secure page

`SecurityPolicy::canLoadSubresource(from, to)` is the rule that protects an HTTPS
document from a plaintext injection:

1. An invalid URL is denied ("the resource URL is not valid").
2. A scheme outside the allow list — `http`, `https`, `file`, `about`, `data`,
   `blob` (`isSchemeAllowed()`) — is denied ("the `<scheme>` scheme is not
   allowed").
3. `from.isHttps() && to.isHttp()` is denied: "a secure page cannot load an
   insecure resource".
4. Strict transport security: if `m_enforceHsts` is on (the default) and `to` is
   `http` to a host in `m_secureHosts`, it is denied: "this host requires a secure
   connection".

`rememberSecureHost(host)` records a host that was reached over HTTPS and
`isSecureHost(host)` reads it back; the set lives in the `SecurityPolicy` instance
for the lifetime of the process. `setEnforceStrictTransportSecurity(false)` turns
the rule off. This is strict transport security in its simplest form — an in-memory
"this host is secure" set — not an RFC 6797 implementation: there is no
`Strict-Transport-Security` header parsing, no `max-age`, no preload list, and no
persistence across runs.

### Where this is wired — and where it is not

`SecurityPolicy` is instantiated in exactly one place today:
`Page::buildDocument()` calls `checkTopLevelNavigation()`. The other checks are
implemented and **not yet called** from the pipeline:

| Check | Call sites outside `SecurityPolicy` |
| --- | --- |
| `checkTopLevelNavigation()` | `Page::buildDocument()` |
| `canLoadSubresource()` | none |
| `canNavigate()` | none |
| `canSendCookies()` | `ResourceLoader::applyCookies()`, and the cookie provider for each redirect hop |
| `canUseFeature()` | none |
| `sameOrigin()`, `originKey()`, `isSchemeAllowed()` | none |

So the secure-page/no-insecure-subresource rule and the HSTS rule are **enforced by
the policy object, but the policy object is not yet consulted by
`Page::collectSubresources()` or `ResourceLoader`.** What a reader observes: a
page loaded over HTTPS can currently request an `http://` image, and it will be
fetched, because that path does not call `canLoadSubresource()` yet. The rules are
the intended contract and are covered by their own reasoning in the source; making
them effective is a wiring change in `Page` and `ResourceLoader`, not a new policy.

## Same origin

`Url::origin()` returns `scheme://host:effectivePort`, and two URLs have the same
origin exactly when their origin strings are equal.

`SecurityPolicy::sameOrigin(a, b)` adds the cases the Origin specification cares
about:

* an invalid URL, or one with an empty scheme, is never the same origin as
  anything;
* a `file:` URL is never the same origin as another URL, including another
  `file:` URL — file URLs are treated as opaque origins, which is what keeps one
  local file from sharing anything with another.

`originKey(url)` is a one-line wrapper over `Url::origin()`, for use as a map key
in anything scoped to an origin. It has no callers yet, because no store is yet
keyed by origin (see `storage.md`).

### The rules as applied

`Page::requestNextSubresource()` asks `canLoadSubresource()` about every
subresource before it is handed to the loader, so the rules above are enforced on
a real load rather than only being available. A refused resource is recorded in
`Page::failedResources()` with its reason, which is what the DevTools panel shows.
`Page::buildDocument()` calls `rememberSecureHost()` for an https document, so the
strict-transport rule has a record of the host to consult.

Note the deliberate default-port normalisation: `Url::normalize()` drops `:80` for
http and `:443` for https, so `http://example.test` and `http://example.test:80`
produce the same origin string and compare equal.

## Cookies

A cookie jar exists in `src/storage/Cookies.{h,cpp}` and is described in
[storage.md](storage.md#what-exists-cookies). It is in memory only, so every cookie
dies with the process, and it honours the rules RFC 6265 lays down — `Domain`
refusal rather than narrowing, `Path` boundary matching, `Max-Age` over `Expires`,
`Secure` limited to `https`, and `HttpOnly` recorded.

The two rules that mattered while the jar was missing are still the two that carry
the security weight, and both are now reachable from the request path:

* `HttpClient::handleRedirect()` removes `Cookie` **and** `Authorization` from the
  outgoing headers when a redirect crosses to a different host, and then re-derives
  the `Cookie` header from the jar for the new host through the cookie provider. So
  a cross-host hop neither carries the old host's credentials nor picks up the new
  host's cookies by accident: it gets exactly what the jar says the new host
  should get.
* `SecurityPolicy::canSendCookies(from, to)` refuses to send cookies to an insecure
  origin: when the two URLs are not the same origin and the target is `http` while
  the source is `https`, the decision is "refusing to send cookies to an insecure
  origin". `ResourceLoader::applyCookies()` calls this before building the header,
  and a refusal sends **no** cookie at all rather than a partial set, because a
  server that receives half a session is worse off than one that receives none.

`Remote-host` cookies are scoped by host, not by address. `127.0.0.1` and
`localhost` are different hosts as far as the jar is concerned even though they
reach the same machine, which is the case `tst_cookie_flow.cpp` pins.

### What is not protected

The jar is deliberately explicit about its limits:

* **No `SameSite` request context beyond the referrer.** `SameSite=Strict` and
  `Lax` are parsed and recorded, and a cross-origin request with no referrer is
  treated as no referrer rather than as a site. There is no registrable-domain
  ("site") computation, so the distinction between same-site and same-origin is not
  made; the conservative reading is the one taken.
* **No third-party cookie policy and no partitioning.** Every cookie the jar holds
  for a host is offered to that host whether the request is first- or third-party.
* **No `document.cookie`.** This is a limitation of the JavaScript bindings, but it
  also means `HttpOnly` cannot be bypassed by script, because no script can read a
  cookie at all.
* **No persistence**, so there is no cookie the user cannot clear by restarting.

## Denial by default for permissions

`canUseFeature(feature, origin)` denies everything, unconditionally:

```cpp
return Decision::deny(
    QStringLiteral("OpenQBrowser does not grant the '%1' permission yet").arg(feature));
```

The comment records the reasoning: every permission is denied until the browser
has both a prompt and a store that remembers an answer, which is the only safe
state to ship. No feature — geolocation, camera, microphone, notifications,
clipboard, fullscreen — has an implementation that could use a permission, so the
rule cannot be worked around by a page.

## Top-level navigation

`checkTopLevelNavigation(url)`:

* an invalid URL is denied ("the address is not a valid URL");
* a `data:` URL is denied ("data: URLs cannot be opened as a page"), because a
  data URL as a top-level document is a phishing vector — the address bar would
  show content no server served.

Navigations are otherwise unrestricted, and `canNavigate()` says so explicitly:
navigations are never blocked by scheme, because that is how people leave a broken
site. The distinction between "blocked navigation" and "blocked subresource" is
the intended posture.

## Transport security across the pipeline

| Concern | Where it is handled |
| --- | --- |
| TLS handshake and certificate chain | `HttpClient::beginRequest()` / `onEncrypted()` / `onSslErrors()` (Qt). |
| CA bundle discovery | `http::installSystemCaCertificates()` in `HttpMessage.cpp`, called from `HttpClient::send()`. |
| Certificate failure reporting | `fail("TLS certificate verification failed: …")` → `Page::classifyFailure()` → `tls` error page. |
| Security scheme allow list | `SecurityPolicy::isSchemeAllowed()`. |
| Secure → insecure page loads | `SecurityPolicy::canLoadSubresource()` (implemented; not yet called from the loader). |
| HSTS-style no-downgrade | `SecurityPolicy::m_enforceHsts`, `rememberSecureHost()`, `isSecureHost()` (in-memory only). |
| Credential stripping on redirect | `HttpClient::handleRedirect()`. |
| Cookies across origins | `SecurityPolicy::canSendCookies()` from `ResourceLoader::applyCookies()`; `HttpClient::handleRedirect()` strips and re-derives the header per hop. |
| Permissions | `SecurityPolicy::canUseFeature()` (deny-all). |
| Top-level navigation filter | `SecurityPolicy::checkTopLevelNavigation()` in `Page::buildDocument()`. |
| Request limits that bound hostile input | `HttpClient::kMaxBodyBytes` (64 MiB), the 512 KiB header cap, `Page::kMaxSubresources` (200), `ParseOptions::maxDepth` (400). |

The limits in the last row are security-relevant and are actual: they bound how
much memory and how much work a single hostile or broken server can cause.

## What is knowingly missing

* **Process isolation and sandboxing.** The renderer, the network code and the UI
  run in one process. A bug in the tokenizer, the CSS parser or the layout engine
  is a bug in the browser process, and there is no OS-level sandbox restricting
  what that process may do. The README's feature list marks this as planned; it is
  not present.
* **Content Security Policy.** The `Content-Security-Policy` header is parsed into
  `HttpResponse::headers` like any other header and never read.
* **Same-origin policy in practice.** There is only one document per page and no
  iframes or subdocuments, so there is nothing to isolate yet. `sameOrigin()` is
  written for the day there is.
* **Mixed-content blocking in the live path**, as described above: the rule exists,
  the wiring does not.
* **HSTS header handling, certificate pinning, CT, and a trust-on-first-use
  policy.** None exist.
* **Storage partitioning, third-party cookie policy, and private browsing.**
  None exist. A cookie jar does, but it is one jar for the whole window: a
  third-party request gets whatever the jar holds for its host. Private browsing is
  approximated only by `HistoryStore::clear()`.
* **Subresource integrity, CORS, referrer policy.** Not implemented. A `Referer`
  header is sent (`ResourceLoader::startRequest()` adds the document URL as
  `Referer` for subresources) with no policy controlling it beyond that.
* **Downloads, `Content-Disposition` handling, file uploads, and form submission.**
  No download path exists, so a response is either rendered or shown as an error.
* **An audit.** The browser has not been reviewed by anyone other than its
  authors, and its parsing code has no fuzzing harness.
