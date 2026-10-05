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
};

/// The seam between the browser and a JavaScript engine.
///
/// OpenQBrowser does not embed a JavaScript engine yet: writing a correct one is
/// a project of its own and the browser is more useful without a half-finished
/// implementation of it. This interface exists so that the surrounding design is
/// already right when one arrives:
///
///   * the page loader already knows where a <script> element sits in the
///     document and in what order scripts must run;
///   * the console and error reporting paths are defined, so a message from a
///     script reaches the user through the same channel as a network error;
///   * the DOM is the only thing an engine would need to be given.
///
/// Until an engine is plugged in, execute() reports what it would have run, and
/// the browser continues to render the document as if scripts were disabled.
/// That is deliberately visible rather than silent: the inspector lists the
/// scripts that were skipped, and the console explains why.
///
/// See architecture/javascript.md for the plan.
class ScriptEngine
{
public:
    ScriptEngine() = default;
    virtual ~ScriptEngine();

    /// True when an engine is present and can run code. Always false today.
    virtual bool isAvailable() const { return false; }

    /// Runs `source` against `document`.
    virtual ExecutionResult execute(const QString &source, dom::Document *document,
                                    const QString &sourceName = {});

    /// The console messages collected so far.
    const QList<ConsoleMessage> &messages() const { return m_messages; }
    void clearMessages() { m_messages.clear(); }

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
