#include "javascript/TimerQueue.h"

#include <QDateTime>

#include <algorithm>
#include <cmath>

namespace oqb::javascript {

#ifdef OPENQBROWSER_SCRIPTING

namespace {

/// Formats a thrown value from a timer callback for the console.
QString exceptionText(JSContext *context, JSValueConst exception)
{
    if (const char *text = JS_ToCString(context, exception)) {
        const QString message = QString::fromUtf8(text);
        JS_FreeCString(context, text);
        return message;
    }
    return QStringLiteral("a timer callback threw a value that could not be described");
}

/// Converts the stored argument JSON back into a value to pass to the callback.
/// A failure to parse it is not fatal: the callback is simply called with no
/// argument, which is what a browser does with an argument it cannot represent.
JSValue decodeArgument(JSContext *context, const TimerQueue::Timer &timer)
{
    if (!timer.hasArgument)
        return JS_UNDEFINED;

    const QByteArray json = timer.argumentJson.toUtf8();
    JSValue value = JS_ParseJSON(context, json.constData(), static_cast<size_t>(json.size()),
                                 "<timer argument>");
    if (JS_IsException(value)) {
        // Clear the pending exception so it does not surface later.
        JSValue error = JS_GetException(context);
        JS_FreeValue(context, error);
        return JS_UNDEFINED;
    }
    return value;
}

/// Runs one callback, reporting a thrown value instead of propagating it.
void invoke(JSContext *context, JSValueConst callback, JSValueConst argument,
            const std::function<void(const QString &)> &onError)
{
    JSValue result = JS_Call(context, callback, JS_UNDEFINED, 1, &argument);

    if (JS_IsException(result)) {
        JSValue exception = JS_GetException(context);
        if (onError)
            onError(exceptionText(context, exception));
        JS_FreeValue(context, exception);
    }

    JS_FreeValue(context, result);
}

} // namespace

TimerQueue::~TimerQueue() = default;

int TimerQueue::schedule(JSContext *context, JSValue callback, double delayMs, bool repeating,
                         const QString &argumentJson, bool hasArgument)
{
    Q_UNUSED(context);

    Timer timer;
    timer.id = m_nextId++;
    timer.callback = callback; // ownership transfers to the queue
    timer.repeating = repeating;
    timer.argumentJson = argumentJson;
    timer.hasArgument = hasArgument;

    // Browsers clamp a timer's delay, and the clamp is what stops a page from
    // spinning the browser: a delay of zero or less still waits a turn, and a
    // deeply nested timer is held to a longer floor still. This follows the
    // HTML specification's rule, where the nesting level is what decides.
    if (!(delayMs > 0) || std::isnan(delayMs))
        delayMs = 0;
    if (delayMs > double(kMaximumDelayMs))
        delayMs = double(kMaximumDelayMs);

    // A timer scheduled from inside a timer callback is nested, and past a few
    // levels the floor rises to 4ms.
    timer.nestingLevel = m_runningNestingLevel;

    double floor = kMinimumDelayMs;
    if (timer.nestingLevel > kNestedClampLevel)
        floor = kNestedMinimumDelayMs;
    if (delayMs < floor)
        delayMs = floor;

    timer.intervalMs = repeating ? qint64(delayMs) : 0;
    // Due times are measured on the queue's own clock, which only moves when the
    // caller reports that time has passed. That is what makes a timer due at a
    // predictable moment rather than relative to the wall clock.
    timer.dueAtMs = m_nowMs + qint64(delayMs);

    m_timers.append(timer);
    return timer.id;
}

int TimerQueue::scheduleAnimationFrame(JSContext *context, JSValue callback)
{
    Q_UNUSED(context);

    Timer timer;
    timer.id = m_nextId++;
    timer.callback = callback;
    timer.animationFrame = true;
    // An animation frame runs at the next paint rather than after a delay, so it
    // is due immediately and holds no interval.
    timer.dueAtMs = 0;
    timer.intervalMs = 0;

    m_timers.append(timer);
    return timer.id;
}

bool TimerQueue::cancel(JSContext *context, int id)
{
    for (int i = 0; i < m_timers.size(); ++i) {
        if (m_timers.at(i).id != id)
            continue;

        JS_FreeValue(context, m_timers.at(i).callback);
        m_timers.removeAt(i);
        return true;
    }
    return false;
}

/// The position of a timer in the list, or -1.
int TimerQueue::indexOfTimer(int id) const
{
    for (int i = 0; i < m_timers.size(); ++i) {
        if (m_timers.at(i).id == id)
            return i;
    }
    return -1;
}

void TimerQueue::runDue(JSContext *context, qint64 nowMs)
{
    // The caller's clock is the queue's clock. It only ever moves forward, so a
    // callback that asks for a time in the past cannot make the queue run
    // backwards and re-fire everything.
    if (nowMs > m_nowMs)
        m_nowMs = nowMs;

    // Animation-frame callbacks run first: a browser runs them immediately before
    // painting, and page script expects them before the timers that follow.
    QList<int> frameIds;
    for (const Timer &timer : m_timers) {
        if (timer.animationFrame)
            frameIds.append(timer.id);
    }

    for (const int id : frameIds) {
        Timer *timer = nullptr;
        for (Timer &candidate : m_timers) {
            if (candidate.id == id) {
                timer = &candidate;
                break;
            }
        }
        if (!timer)
            continue;

        // The callback value is moved out before the call, because the callback
        // may cancel or reschedule timers and invalidate the list. The timer is
        // removed first so that a callback which reschedules itself gets a fresh
        // entry rather than mutating the one being run.
        const int timerId = timer->id;
        JSValue callback = timer->callback;
        timer->callback = JS_UNDEFINED;
        m_timers.removeAt(indexOfTimer(timerId));

        invoke(context, callback, JS_UNDEFINED, m_onError);
        JS_FreeValue(context, callback);
    }

    // Timers are run in due order, and a callback that schedules another timer
    // is picked up on the next call rather than in this pass, which is what
    // keeps a self-rescheduling timer from looping forever inside one call.
    bool ranSomething = true;
    while (ranSomething) {
        ranSomething = false;

        int chosen = -1;
        for (int i = 0; i < m_timers.size(); ++i) {
            const Timer &timer = m_timers.at(i);
            if (timer.animationFrame || timer.dueAtMs > m_nowMs)
                continue;
            if (chosen < 0 || timer.dueAtMs < m_timers.at(chosen).dueAtMs)
                chosen = i;
        }

        if (chosen < 0)
            break;

        Timer timer = m_timers.at(chosen);
        m_timers.removeAt(chosen);
        ranSomething = true;

        JSValue argument = decodeArgument(context, timer);
        {
            // Anything scheduled from inside this callback is nested one level
            // deeper, which is what the delay clamp reads.
            const int previousLevel = m_runningNestingLevel;
            m_runningNestingLevel = timer.nestingLevel + 1;
            invoke(context, timer.callback, argument, m_onError);
            m_runningNestingLevel = previousLevel;
        }
        JS_FreeValue(context, argument);

        if (timer.repeating) {
            // An interval keeps its identity, so it is put back with its next
            // due time. The next time is measured from the previous one rather
            // than from now, so an interval does not drift later every time its
            // callback takes a while. A period that was missed entirely while
            // the callback ran is skipped rather than run in a burst.
            timer.dueAtMs += qMax<qint64>(timer.intervalMs, 1);
            // A due time that has already passed is pulled up to now, so a
            // callback that ran long is not followed by a burst of catch-up
            // calls; a due time that has exactly arrived is left alone and fires
            // in the loop below.
            if (timer.dueAtMs < m_nowMs)
                timer.dueAtMs = m_nowMs;

            m_timers.append(timer);
        } else {
            JS_FreeValue(context, timer.callback);
        }

        // Anything that became due while this callback ran is picked up by the
        // loop above, so a single pass stops once nothing else is due.
        // Time does not move on its own: the caller decides when the clock
        // advances, so a callback that schedules work is picked up on the next
        // call rather than looping inside this one.
    }
}

void TimerQueue::clear(JSContext *context)
{
    for (const Timer &timer : m_timers)
        JS_FreeValue(context, timer.callback);
    m_timers.clear();
}

QList<int> TimerQueue::takeAnimationFrameIds()
{
    QList<int> ids;
    for (const Timer &timer : m_timers) {
        if (timer.animationFrame)
            ids.append(timer.id);
    }
    return ids;
}

#else // no scripting

TimerQueue::~TimerQueue() = default;

#endif // OPENQBROWSER_SCRIPTING

qint64 TimerQueue::nextDueTime() const
{
    qint64 earliest = -1;
    for (const Timer &timer : m_timers) {
        if (timer.animationFrame)
            continue;
        if (earliest < 0 || timer.dueAtMs < earliest)
            earliest = timer.dueAtMs;
    }
    return earliest;
}

int TimerQueue::pendingAnimationFrames() const
{
    int count = 0;
    for (const Timer &timer : m_timers) {
        if (timer.animationFrame)
            ++count;
    }
    return count;
}

} // namespace oqb::javascript
