#pragma once

#include <QColor>
#include <QString>

namespace oqb::browser {

/// The browser's own settings, as opposed to anything a page sets.
///
/// These are what a real browser puts behind a preferences window; keeping them
/// in one structure means the location of a default is obvious and the command
/// line, the window and the tests can all configure a page the same way.
struct PageSettings
{
    /// Viewport in device-independent pixels.
    double viewportWidth = 1024;
    double viewportHeight = 768;

    /// The user's default font size, which CSS "medium" resolves to.
    double defaultFontSize = 16.0;

    /// The colour behind a page that sets none.
    QColor defaultBackground = QColor(255, 255, 255);
    /// The colour of text a page does not colour itself.
    QColor defaultTextColor = QColor(0, 0, 0);

    /// Whether the browser reports prefers-color-scheme: dark.
    bool prefersDarkScheme = false;

    /// Where a search typed into the address bar is sent. "%s" is replaced.
    QString searchTemplate = QStringLiteral("https://duckduckgo.com/?q=%s");

    /// Sent as the User-Agent header. Empty means the browser's default.
    QString userAgent;

    /// Milliseconds before a request is abandoned.
    int requestTimeoutMs = 30000;

    /// The greatest number of subresources fetched at the same time.
    int maxConcurrentRequests = 6;

    /// Load images referenced by the page. Turning this off makes a slow page
    /// usable on a slow connection, and keeps the tests independent of images.
    bool loadImages = true;

    /// Fetch external stylesheets referenced by <link rel="stylesheet">.
    bool loadExternalStylesheets = true;

    /// Record visits in the history.
    bool recordHistory = true;
};

} // namespace oqb::browser
