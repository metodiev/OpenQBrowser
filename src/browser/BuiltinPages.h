#pragma once

#include <QString>

#include "network/Url.h"

namespace oqb::storage {
class BookmarkStore;
class HistoryStore;
}

namespace oqb::browser {

/// The pages OpenQBrowser serves from inside itself, under the about: scheme.
///
/// They are ordinary HTML documents, so they render through exactly the same
/// pipeline as a network page. That is deliberate: if the new tab page looks
/// wrong, the bug is in the renderer, not hidden behind a native widget.
namespace builtin {

/// The name of every built-in page, for the DevTools listing and about:about.
QStringList pageNames();

/// True when `name` is one of the built-in pages.
bool hasPage(const QString &name);

/// The document source for an about: URL, or an empty string when the page is
/// unknown. `viewer` describes the browser window and is used for the version
/// line; the stores are read live so the pages always show current data.
QString documentFor(const network::Url &url, const QString &version, double viewportWidth,
                    const storage::HistoryStore *history = nullptr,
                    const storage::BookmarkStore *bookmarks = nullptr,
                    int tabCount = 1);

/// The HTML OpenQBrowser shows for a URL it cannot load. `kind` selects the
/// wording: "dns", "connect", "tls", "timeout", "http" or "blocked".
QString errorPage(const network::Url &url, const QString &kind, const QString &details);

} // namespace builtin

} // namespace oqb::browser
