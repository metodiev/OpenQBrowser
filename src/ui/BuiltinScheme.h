#pragma once

#include <QString>
#include <QWebEngineUrlScheme>

#include <functional>

class QWebEngineProfile;

namespace oqb::ui {

/// The scheme the built-in pages are served from.
///
/// Chromium has no handler for `about:`, so a page loaded with it would be a
/// document with no address and no history entry: Back would be dead on the
/// very first site a user opens, and Reload would have nothing to reload.
/// Serving the pages from a scheme of our own makes them ordinary navigations
/// with an address, a history entry and a working reload.
///
/// The scheme name is internal. The window still shows and accepts `about:`
/// URLs, which WebTab translates, so nothing the user sees changes.
inline constexpr char kBuiltinScheme[] = "oqb";

/// Registers the scheme with Qt WebEngine.
///
/// Qt requires this to happen before the QApplication object exists, so it is
/// called first thing in main().
void registerBuiltinScheme();

/// Installs a handler on `profile` that answers `<scheme>:<page>` requests by
/// calling `render` with the page name.
///
/// `render` returns the document to serve, or an empty string for an unknown
/// page, which becomes a 404. The profile takes ownership of the handler.
void installBuiltinSchemeHandler(QWebEngineProfile *profile,
                                 std::function<QString(const QString &pageName)> render);

} // namespace oqb::ui
