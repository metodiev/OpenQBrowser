#pragma once

#include <QString>
#include <QStringList>

namespace oqb::dom {
class Document;
}

namespace oqb::javascript {

/// A message produced by running a script, shown in the DevTools console.
struct ConsoleMessage
{
    enum class Level { Log, Info, Warning, Error };
    Level level = Level::Log;
    QString text;
    /// Where the message came from, for the console's source column.
    QString source;
    int line = 0;
};

/// The outcome of executing a script.
struct ExecutionResult
{
    bool success = true;
    QString error;
    int line = 0;
    QList<ConsoleMessage> messages;
    /// The script's completion value, already converted to text. A console shows
    /// it under each evaluation, and a test can assert on it directly.
    QString value;
};

/// The seam between the browser and a JavaScript engine.
///
/// The browser talks to this interface rather than to QuickJS directly, so the
/// engine is one replaceable piece. Two implementations exist:
///
///   * `QuickJsScriptEngine`, which owns a real engine and is what a normal
///     build constructs; it is declared in Engine.h.
///   * This base class, whose execute() reports what it would have run. It is
///     what a build with `OPENQBROWSER_SCRIPTING=OFF` falls back to, and its
///     behaviour is deliberately visible rather than silent: the inspector lists
///     the scripts that were skipped, and the console explains why.
///
/// See architecture/javascript.md.
class ScriptEngine
{
public:
    ScriptEngine() = default;
    virtual ~ScriptEngine();

    /// True when an engine is present and can run code. False for the base
    /// class, which is what a build without QuickJS falls back to.
    virtual bool isAvailable() const { return false; }

    /// Runs `source` against `document`.
    virtual ExecutionResult execute(const QString &source, dom::Document *document,
                                    const QString &sourceName = {});

    /// Points the engine at a document. The browser calls this once the document
    /// is parsed, because an engine is created before one exists.
    virtual void setDocument(dom::Document *document) { Q_UNUSED(document); }

    /// The console messages collected so far.
    const QList<ConsoleMessage> &messages() const { return m_messages; }

    /// Forgets every message. An engine that keeps its own log overrides this so
    /// that the two do not disagree: clearing only this list would leave the
    /// engine re-reporting messages the browser had already discarded.
    virtual void clearMessages() { m_messages.clear(); }

    /// Scripts the page declared but which could not be run.
    QStringList skippedScripts() const { return m_skipped; }
    void noteSkippedScript(const QString &description) { m_skipped.append(description); }

    /// A human readable note explaining the current state, shown in the
    /// inspector so the behaviour is not a mystery.
    static QString availabilityNote();

    /// Whether the browser should keep the page from rendering until scripts
    /// have run. Always true without an engine: there is nothing to wait for.
    bool scriptsBlockRendering() const { return !isAvailable(); }

protected:
    QList<ConsoleMessage> m_messages;
    QStringList m_skipped;
};

} // namespace oqb::javascript
