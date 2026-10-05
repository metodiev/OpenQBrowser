#include "javascript/ScriptEngine.h"

#include "dom/Document.h"

namespace oqb::javascript {

ScriptEngine::~ScriptEngine() = default;

ExecutionResult ScriptEngine::execute(const QString &source, dom::Document *document,
                                      const QString &sourceName)
{
    Q_UNUSED(document);

    ExecutionResult result;

    if (isAvailable()) {
        // A concrete engine overrides this method entirely; reaching here means
        // one claimed to be available without doing so.
        result.success = false;
        result.error = QStringLiteral("the script engine reported itself available "
                                      "but did not implement execution");
        return result;
    }

    // No engine: report clearly instead of pretending the script ran. The
    // document keeps rendering, which is the behaviour of a browser with
    // scripting disabled.
    const int lineCount = source.count(u'\n') + 1;
    result.messages.append({ConsoleMessage::Level::Info,
                            QStringLiteral("Script not executed: OpenQBrowser has no "
                                           "JavaScript engine yet (%1 line%2 from %3).")
                                .arg(lineCount)
                                .arg(lineCount == 1 ? QString() : QStringLiteral("s"))
                                .arg(sourceName.isEmpty() ? QStringLiteral("an inline script")
                                                          : sourceName),
                            sourceName.isEmpty() ? QStringLiteral("inline") : sourceName,
                            0});
    result.messages.append({ConsoleMessage::Level::Warning,
                            QStringLiteral("The page may not work as intended without scripting. "
                                           "See architecture/javascript.md for the plan."),
                            sourceName.isEmpty() ? QStringLiteral("inline") : sourceName,
                            0});

    m_messages.append(result.messages);
    noteSkippedScript(sourceName.isEmpty() ? QStringLiteral("inline script") : sourceName);
    return result;
}

QString ScriptEngine::availabilityNote()
{
    return QStringLiteral(
        "Scripting is not implemented yet. OpenQBrowser parses and renders the "
        "document but does not execute <script> content, so interactive pages "
        "will not respond. The interface in src/javascript/ScriptEngine.h is the "
        "seam an engine would plug into.");
}

} // namespace oqb::javascript
