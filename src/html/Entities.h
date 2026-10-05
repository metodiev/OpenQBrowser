#pragma once

#include <QHash>
#include <QString>

namespace oqb::html {

/// Named character references, as defined by the HTML standard (§13.5).
///
/// OpenQBrowser ships a curated subset covering the references that appear in
/// real documents rather than the full two-thousand-entry list, which keeps the
/// table readable while remaining correct for everyday pages. Unknown
/// references are left untouched, exactly as the standard requires.
namespace entities {

/// References that require a trailing semicolon, keyed without the leading
/// ampersand, e.g. "amp;" -> "&".
const QHash<QString, QString> &table();

/// References that legacy documents write without a semicolon, keyed without it,
/// e.g. "amp" -> "&". These are only accepted when not followed by an
/// alphanumeric character or '='.
const QHash<QString, QString> &legacyTable();

/// Expands every character reference in `text`.
QString decode(const QString &text);

/// The replacement character substituted for malformed references.
QString replacementCharacter();

} // namespace entities

} // namespace html
