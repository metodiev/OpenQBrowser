#include "network/ScriptFetch.h"

namespace oqb::network {

ScriptFetchProvider::ScriptFetchProvider(QObject *parent)
    : QObject(parent)
{
    // The response type crosses a queued connection, so Qt has to be able to copy
    // it into the event loop's argument store. Registering it once here is what
    // makes a response delivered from another thread safe to hand to script.
    static const int registered = qRegisterMetaType<ScriptFetchResponse>("oqb::network::ScriptFetchResponse");
    Q_UNUSED(registered);
}

ScriptFetchProvider::~ScriptFetchProvider() = default;

} // namespace oqb::network
