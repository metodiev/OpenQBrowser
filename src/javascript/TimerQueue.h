#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

#include <functional>

#ifdef OPENQBROWSER_SCRIPTING
#include <quickjs.h>
#endif

namespace oqb::javascript {

/// The page's timers: setTimeout, setInterval, clearTimeout and
/// requestAnimationFrame.
///
/// Timers are held here rather than inside the engine so that the browser
/// decides when they run. That is what keeps a callback from firing in the
/// middle of layout, and it is what lets the tests drive time forward
/// deterministically rather than sleeping.
///
/// Ownership: each timer holds a QuickJS function value, which is a reference
/// the queue owns and must release. `clear()` releases them all and is called by
/// the engine before its context is destroyed, because a value outliving its
/// context is a use-after-free.
class TimerQueue
{
public:
#ifdef OPENQBROWSER_SCRIPTING
    /// One scheduled callback.
    struct Timer
    {
        int id = 0;
        /// The function to call. Owned by the queue.
        JSValue callback = JS_UNDEFINED;
        /// When the callback is due, on whichever clock `runDue` is given.
        qint64 dueAtMs = 0;
        /// Milliseconds between runs; 0 for a one-shot timer.
        qint64 intervalMs = 0;
        /// True when this is a setInterval rather than a setTimeout.
        bool repeating = false;
        /// The argument to pass, held as JSON so the queue owns one value per
        /// timer rather than two.
        QString argumentJson;
        bool hasArgument = false;
        /// How many timers were already running when this callback started, used
        /// to apply the clamp browsers apply to nested timers.
        int nestingLevel = 0;
        /// True when this is a requestAnimationFrame callback rather than a
        /// timer: it runs before the next repaint instead of after a delay.
        bool animationFrame = false;
    };
#else
    /// Without an engine there are no callbacks to hold, but the queue keeps its
    /// shape so that the rest of the browser compiles unchanged.
    struct Timer
    {
        int id = 0;
        qint64 dueAtMs = 0;
        qint64 intervalMs = 0;
        bool repeating = false;
        QString argumentJson;
        bool hasArgument = false;
        int nestingLevel = 0;
        bool animationFrame = false;
    };
#endif

    TimerQueue() = default;
    ~TimerQueue();

    TimerQueue(const TimerQueue &) = delete;
    TimerQueue &operator=(const TimerQueue &) = delete;

#ifdef OPENQBROWSER_SCRIPTING
    /// Schedules `callback`, taking ownership of the value. `delayMs` is clamped
    /// the way browsers clamp it, so a page cannot make the browser spin by
    /// asking for a zero-delay interval inside its own callback.
    int schedule(JSContext *context, JSValue callback, double delayMs, bool repeating,
                 const QString &argumentJson, bool hasArgument);

    /// Schedules a requestAnimationFrame callback, taking ownership.
    int scheduleAnimationFrame(JSContext *context, JSValue callback);

    /// Cancels a timer by id, releasing its callback. Returns true when a timer
    /// was found.
    bool cancel(JSContext *context, int id);

    /// Runs every timer due at `nowMs`. A callback that throws is reported and
    /// does not stop the others.
    void runDue(JSContext *context, qint64 nowMs);

    /// Releases every callback. Must run before the context is freed.
    void clear(JSContext *context);

    /// The animation-frame callbacks waiting for a frame, consumed by the caller
    /// when it paints. Their ids are removed from the queue.
    QList<int> takeAnimationFrameIds();
#endif

    bool isEmpty() const { return m_timers.isEmpty(); }
    int count() const { return static_cast<int>(m_timers.size()); }

    /// When the next timer is due, or -1 when nothing is scheduled.
    qint64 nextDueTime() const;

    /// How many animation-frame callbacks are waiting for the next frame.
    int pendingAnimationFrames() const;

    /// How a failure inside a callback is reported. The engine sets this so a
    /// thrown error reaches the same console the page writes to.
    void setErrorReporter(std::function<void(const QString &)> reporter)
    {
        m_onError = std::move(reporter);
    }

    /// The greatest interval at which a timer may be scheduled, so that a page
    /// cannot ask to be woken a million years from now and overflow the clock.
    static constexpr qint64 kMaximumDelayMs = 24 * 60 * 60 * 1000;

    /// The shortest delay a timer may have, so that a zero-delay timer still
    /// waits a turn rather than running inside the call that scheduled it.
    static constexpr double kMinimumDelayMs = 1.0;
    /// Once timers are nested deeper than this, the floor rises to 4ms, which is
    /// what the HTML specification does to keep a self-rescheduling callback
    /// from starving the browser.
    static constexpr int kNestedClampLevel = 5;
    static constexpr double kNestedMinimumDelayMs = 4.0;

private:
    /// The position of a timer in the list, or -1.
    int indexOfTimer(int id) const;

    QList<Timer> m_timers;
    int m_nextId = 1;
    /// The queue's own clock. It is moved only by runDue, so a timer is due
    /// relative to the time the caller last reported rather than to wall-clock
    /// time, which is what makes the browser's frame clock and the tests' fixed
    /// numbers both work.
    qint64 m_nowMs = 0;
    /// How deep inside nested timer callbacks the queue currently is, which is
    /// what the nested-delay clamp reads.
    int m_runningNestingLevel = 0;
    std::function<void(const QString &)> m_onError;
};

} // namespace oqb::javascript
