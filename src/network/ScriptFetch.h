#pragma once

#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QPair>
#include <QString>

namespace oqb::network {

/// A request `fetch()` makes on a page's behalf.
///
/// The fields are the ones that change how the request is sent and what may be
/// read back. Nothing here is a JavaScript value: the seam the bindings talk to
/// is plain Qt types, so a test can drive the whole of `fetch()` through a fake
/// provider without a socket, an engine, or a page.
struct ScriptFetchRequest
{
    /// Absolute URL, already resolved against the document by the bindings.
    QString url;
    /// The document the call came from. It supplies the referrer and the origin
    /// a CORS grant has to match.
    QString documentUrl;
    QString method = QStringLiteral("GET");
    QList<QPair<QString, QString>> headers;
    QByteArray body;
    /// `omit`, `same-origin` or `include`, as fetch()'s `credentials` names them.
    QString credentials = QStringLiteral("same-origin");
    /// `cors`, `same-origin` or `no-cors`, as fetch()'s `mode` names them.
    QString mode = QStringLiteral("cors");
};

/// What came back from a script request.
struct ScriptFetchResponse
{
    int status = 0;
    QString statusText;
    QList<QPair<QString, QString>> headers;
    QByteArray body;
    /// The URL the response was finally read from, which after a redirect is not
    /// the URL that was requested.
    QString url;
    /// True when the request was redirected to get here.
    bool redirected = false;
    /// True when the response has no readable body at all, which is what
    /// `mode: "no-cors"` produces for a cross-origin response. The status is
    /// hidden too, because a script that could read it could use it to probe.
    bool opaque = false;
    /// Non-empty when no response was produced. fetch() rejects its promise on
    /// this, and resolves with a response for an HTTP error status.
    QString error;
};

/// Where the bindings' `fetch()` gets its answers.
///
/// `ResourceLoader` implements this. It is an interface rather than a direct
/// dependency so that the script-facing behaviour - promise settling, headers,
/// the body accessors - can be tested against a provider that answers
/// immediately, which is the only way to test the parts of fetch() that are not
/// network behaviour at all.
///
/// A provider **must not** emit `fetchFinished` before start has returned: the
/// bindings register the promise against the id that call produces, so a
/// response delivered inside it would arrive with nothing to hand it to.
class ScriptFetchProvider : public QObject
{
    Q_OBJECT

public:
    explicit ScriptFetchProvider(QObject *parent = nullptr);
    ~ScriptFetchProvider() override;

    /// Begins `request` and returns a non-zero id, or 0 when the request was
    /// refused. `refusalReason()` then says why. Exactly one `fetchFinished`
    /// with that id follows each accepted call, including when it fails.
    virtual int startScriptFetch(const ScriptFetchRequest &request) = 0;

    /// Abandons `requestId`. Its completion is not reported afterwards, which is
    /// what makes an aborted fetch stay silent.
    virtual void abortScriptFetch(int requestId) = 0;

    /// Why the last startScriptFetch() call was refused. Empty when it was not.
    virtual QString refusalReason() const { return {}; }

signals:
    void fetchFinished(int requestId, const oqb::network::ScriptFetchResponse &response);

    /// A redirect hop of a script request, reported so the inspector can show it
    /// and so its cookies are absorbed even though the hop itself is discarded.
    void fetchRedirected(int requestId, const oqb::network::ScriptFetchResponse &response);
};

} // namespace oqb::network

Q_DECLARE_METATYPE(oqb::network::ScriptFetchResponse)
