#include <QtTest>

#include "dom/Document.h"
#include "html/Parser.h"
#include "javascript/Engine.h"
#include "javascript/ScriptEngine.h"
#include "network/FetchPolicy.h"
#include "network/ScriptFetch.h"
#include "network/Url.h"

#include <QEventLoop>
#include <QSet>
#include <QTimer>

using namespace oqb;
using namespace oqb::javascript;

/// A provider that answers from a table rather than from a socket.
///
/// Every case here can then assert what the *bindings* do - promise settling,
/// header visibility, the body accessors, the policy refusals - without a server,
/// which is what makes the failures diagnosable: a failing test names the
/// behaviour rather than the network.
class FakeProvider : public network::ScriptFetchProvider
{
    Q_OBJECT

public:
    struct Answer
    {
        int status = 200;
        QString statusText;
        QList<QPair<QString, QString>> headers;
        QByteArray body;
        QString url;
        bool redirected = false;
        QString error;
    };

    /// The answer the next request gets. An empty map means 200 with no body.
    QHash<QString, Answer> answers;
    Answer fallback;

    /// What was asked for, in order, so a test can assert the request that was
    /// actually built rather than only the response that came back.
    QList<network::ScriptFetchRequest> requests;

    /// When set, startScriptFetch() refuses with this reason and returns 0.
    QString refuseWith;

    /// When true, an accepted request is never answered. Used to prove that a
    /// page with a request in flight keeps the frame clock alive.
    bool neverAnswers = false;

    int startScriptFetch(const network::ScriptFetchRequest &request) override
    {
        if (!refuseWith.isEmpty())
            return 0;

        requests.append(request);

        if (neverAnswers)
            return m_nextId++;

        const int id = m_nextId++;
        const Answer answer = answers.value(request.url, fallback);

        QMetaObject::Connection *connection = new QMetaObject::Connection;
        *connection = connect(this, &network::ScriptFetchProvider::fetchFinished, this,
                              [this, id, answer, connection] {
                                  disconnect(*connection);
                                  delete connection;
                              });

        // Delivered from the event loop, never inside the call: a response that
        // arrived synchronously would settle the promise before the page had run
        // the line that returns it, which is not what a browser does and would
        // hide exactly the order-dependent bugs this suite is for.
        QTimer::singleShot(0, this, [this, id, answer] { deliver(id, answer); });
        return id;
    }

    void abortScriptFetch(int requestId) override
    {
        // An aborted request stays silent, which is what the abort test asserts.
        m_aborted.insert(requestId);
    }

    QString refusalReason() const override { return refuseWith; }

signals:
    void answered();

private:
    void deliver(int id, const Answer &answer)
    {
        if (m_aborted.contains(id))
            return;

        network::ScriptFetchResponse response;
        response.status = answer.status;
        response.statusText = answer.statusText.isEmpty()
            ? network::FetchPolicy::statusTextFor(answer.status)
            : answer.statusText;
        response.headers = answer.headers;
        response.body = answer.body;
        response.url = answer.url;
        response.redirected = answer.redirected;
        response.error = answer.error;

        emit fetchFinished(id, response);
        emit answered();
    }

    int m_nextId = 1;
    QSet<int> m_aborted;
};

/// Tests for fetch() and the policy that bounds it.
///
/// The policy cases run without an engine at all: the rules that decide what a
/// page may send and read are the security boundary of the feature, so they are
/// pure functions and are tested as such. The binding cases drive a real engine
/// against a fake provider, which is what makes them assert the JavaScript-facing
/// behaviour - what a page sees - rather than an implementation detail.
class FetchTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    // The policy: what a page may send.
    void allowsOnlyRealMethods();
    void refusesConnectionAndTrace();
    void refusesForbiddenRequestHeaders();
    void refusesReservedHeaderPrefixes();
    void allowsSafelistedRequestHeaders();
    void refusesOversizedSafelistedValue();
    void refusesHeaderInjectionInValue();
    void treatsCorsSimpleRequestsAsSimple();

    // The policy: what a page may read.
    void exposesSafelistedResponseHeaders();
    void filtersHeadersOfACrossOriginResponse();
    void exposesNamedResponseHeaders();
    void refusesCrossOriginWithoutGrant();
    void corsGrantMatchesTheSerialisedOrigin();
    void acceptsMatchingOrigin();
    void acceptsWildcardForAnonymousRequest();
    void refusesWildcardWhenCredentialsSent();
    void refusesNamedOriginWithoutCredentialsFlag();
    void refusesGrantForAnotherOrigin();
    void splitsHeaderLists();

    // The bindings.
    void exposesFetchAsAFunction();
    void rejectsWhenNoProviderIsInstalled();
    void resolvesWithStatusAndHeaders();
    void resolvesHttpErrorStatusInsteadOfRejecting();
    void rejectsOnTransportFailure();
    void readsTextBody();
    void readsJsonBody();
    void rejectsJsonThatIsNotJson();
    void readsArrayBuffer();
    void reportsAnAbsentHeaderAsNull();
    void reportsResponseUrlAndRedirected();
    void refusesForbiddenHeaderAtTheBinding();
    void sendsPostBodyAndCustomHeader();
    void resolvesRelativeUrlAgainstDocument();
    void rejectsInvalidUrlWithoutARequest();
    void runsThenHandlersAcrossTurns();
    void keepsPendingWorkVisibleWhileInFlight();
    void aRepeatingTimerNeverStopsBeingWork();
    void abortsWithoutSettling();
    void cancelsOutstandingFetchesOnNavigation();

private:
    void load(const QString &html);
    /// Runs `source` and returns the console value it produced.
    QString eval(const QString &source);
    /// Spins the event loop until `predicate` holds or the deadline passes.
    bool waitFor(const std::function<bool()> &predicate, int timeoutMs = 3000);

    std::unique_ptr<dom::Document> m_document;
    std::unique_ptr<Engine> m_engine;
    FakeProvider *m_provider = nullptr;
};

void FetchTest::init()
{
    if (!Engine::isSupported())
        QSKIP("this build has no JavaScript engine (OPENQBROWSER_SCRIPTING=OFF)");

    m_provider = new FakeProvider;
    load(QStringLiteral("<html><body><div id='out'>start</div></body></html>"));
}

void FetchTest::cleanup()
{
    m_engine.reset();
    m_document.reset();
    delete m_provider;
    m_provider = nullptr;
}

void FetchTest::load(const QString &html)
{
    auto parsed = html::Parser::parse(html, network::Url::parse(QStringLiteral("https://example.com/page")));
    m_document = std::move(parsed.document);

    if (!m_engine) {
        m_engine = std::make_unique<Engine>(m_document.get());
        m_engine->setFetchProvider(m_provider);
    } else {
        m_engine->setDocument(m_document.get());
    }
}

QString FetchTest::eval(const QString &source)
{
    const ExecutionResult result = m_engine->evaluate(source);
    if (!result.success)
        return QStringLiteral("ERROR: ") + result.error;
    return result.value;
}

bool FetchTest::waitFor(const std::function<bool()> &predicate, int timeoutMs)
{
    if (predicate())
        return true;

    QEventLoop loop;
    QTimer deadline(this);
    deadline.setSingleShot(true);

    bool satisfied = false;
    auto *poll = new QTimer(this);
    poll->setInterval(1);
    connect(poll, &QTimer::timeout, this, [&] {
        if (predicate()) {
            satisfied = true;
            loop.quit();
        }
    });
    connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);

    poll->start();
    deadline.start(timeoutMs);
    loop.exec();
    poll->stop();
    delete poll;

    return satisfied || predicate();
}

// ------------------------------------------------------------------- policy

void FetchTest::allowsOnlyRealMethods()
{
    // The methods a page legitimately uses, including the extension methods
    // that are not in the original set but are valid tokens.
    for (const QString &method : {QStringLiteral("GET"), QStringLiteral("POST"),
                                 QStringLiteral("HEAD"), QStringLiteral("PUT"),
                                 QStringLiteral("DELETE"), QStringLiteral("PATCH"),
                                 QStringLiteral("OPTIONS")}) {
        QVERIFY2(network::FetchPolicy::isAllowedMethod(method),
                 qPrintable(QStringLiteral("%1 should be allowed").arg(method)));
    }

    // A method must be a valid token: one containing a space or a control
    // character would not survive being written into a request line.
    QVERIFY(!network::FetchPolicy::isAllowedMethod(QStringLiteral("GE T")));
    QVERIFY(!network::FetchPolicy::isAllowedMethod(QStringLiteral("GET\r\nHost: evil")));
    QVERIFY(!network::FetchPolicy::isAllowedMethod(QString()));
    QVERIFY(!network::FetchPolicy::isAllowedMethod(QStringLiteral("GET/")));
}

void FetchTest::refusesConnectionAndTrace()
{
    // CONNECT opens a tunnel through the browser, and TRACE reflects the request
    // back, which is how a cross-site scripting filter was defeated. Neither may
    // be reachable from a page, whatever case it is spelled in.
    QVERIFY(!network::FetchPolicy::isAllowedMethod(QStringLiteral("CONNECT")));
    QVERIFY(!network::FetchPolicy::isAllowedMethod(QStringLiteral("connect")));
    QVERIFY(!network::FetchPolicy::isAllowedMethod(QStringLiteral("TRACE")));
    QVERIFY(!network::FetchPolicy::isAllowedMethod(QStringLiteral("track")));
}

void FetchTest::refusesForbiddenRequestHeaders()
{
    // Host would let a page address a different virtual host on the same
    // address, Content-Length would let it frame a body other than the one it
    // sent, and Cookie would let it read out a session it cannot otherwise see.
    for (const QString &name : {QStringLiteral("Host"), QStringLiteral("Content-Length"),
                               QStringLiteral("Cookie"), QStringLiteral("Origin"),
                               QStringLiteral("Referer"), QStringLiteral("Connection"),
                               QStringLiteral("Transfer-Encoding"), QStringLiteral("Upgrade"),
                               QStringLiteral("Expect")}) {
        QVERIFY2(network::FetchPolicy::isForbiddenRequestHeader(name),
                 qPrintable(QStringLiteral("%1 must not be settable by script").arg(name)));
    }

    // The names are case-insensitive, and a page may spell them any way it likes.
    QVERIFY(network::FetchPolicy::isForbiddenRequestHeader(QStringLiteral("hOsT")));
    QVERIFY(network::FetchPolicy::isForbiddenRequestHeader(QStringLiteral("  cookie  ")));
}

void FetchTest::refusesReservedHeaderPrefixes()
{
    // Proxy- would reach a proxy the user configured, and Sec- names the headers
    // that carry the browser's own security decisions.
    QVERIFY(network::FetchPolicy::isForbiddenRequestHeader(QStringLiteral("Proxy-Authorization")));
    QVERIFY(network::FetchPolicy::isForbiddenRequestHeader(QStringLiteral("Sec-Fetch-Site")));
    QVERIFY(network::FetchPolicy::isForbiddenRequestHeader(QStringLiteral("sec-fetch-mode")));

    // A name that merely contains one of the prefixes is ordinary.
    QVERIFY(!network::FetchPolicy::isForbiddenRequestHeader(QStringLiteral("X-Proxy-Thing")));
    QVERIFY(!network::FetchPolicy::isForbiddenRequestHeader(QStringLiteral("X-Section")));
}

void FetchTest::allowsSafelistedRequestHeaders()
{
    QVERIFY(network::FetchPolicy::isSafelistedRequestHeader(QStringLiteral("Accept"),
                                                            QStringLiteral("application/json")));
    QVERIFY(network::FetchPolicy::isSafelistedRequestHeader(QStringLiteral("Accept-Language"),
                                                            QStringLiteral("en-US")));
    QVERIFY(network::FetchPolicy::isSafelistedRequestHeader(QStringLiteral("Content-Language"),
                                                            QStringLiteral("en")));

    // Only the three content types that cannot carry a meaningful payload
    // without a preflight.
    QVERIFY(network::FetchPolicy::isSafelistedRequestHeader(
        QStringLiteral("Content-Type"), QStringLiteral("text/plain")));
    QVERIFY(network::FetchPolicy::isSafelistedRequestHeader(
        QStringLiteral("Content-Type"), QStringLiteral("application/x-www-form-urlencoded")));
    QVERIFY(network::FetchPolicy::isSafelistedRequestHeader(
        QStringLiteral("Content-Type"), QStringLiteral("text/plain;charset=UTF-8")));

    QVERIFY(!network::FetchPolicy::isSafelistedRequestHeader(
        QStringLiteral("Content-Type"), QStringLiteral("application/json")));
    // Parameters do not change which type it is, so a multipart body with a
    // boundary is still the safelisted type.
    QVERIFY(network::FetchPolicy::isSafelistedRequestHeader(
        QStringLiteral("Content-Type"), QStringLiteral("multipart/form-data; boundary=x")));

    // Anything not on the list at all is not safelisted, including the names
    // that are forbidden outright.
    QVERIFY(!network::FetchPolicy::isSafelistedRequestHeader(QStringLiteral("X-Custom"),
                                                            QStringLiteral("1")));
    QVERIFY(!network::FetchPolicy::isSafelistedRequestHeader(QStringLiteral("Cookie"),
                                                            QStringLiteral("a=b")));
}

void FetchTest::refusesOversizedSafelistedValue()
{
    // A safelisted name with a long value is no longer simple, because its length
    // is what would let it carry a payload.
    const QString longValue(200, u'x');
    QVERIFY(!network::FetchPolicy::isSafelistedRequestHeader(QStringLiteral("Accept"), longValue));

    // Just under the limit is still fine.
    const QString okValue(100, u'x');
    QVERIFY(network::FetchPolicy::isSafelistedRequestHeader(QStringLiteral("Accept"), okValue));
}

void FetchTest::refusesHeaderInjectionInValue()
{
    // A value carrying a newline is a header-injection attempt: it would end the
    // header and start another one on the wire.
    QVERIFY(!network::FetchPolicy::isSafelistedRequestHeader(
        QStringLiteral("Accept"), QStringLiteral("text/plain\r\nCookie: session=stolen")));
    QVERIFY(!network::FetchPolicy::isSafelistedRequestHeader(
        QStringLiteral("Accept"), QStringLiteral("text/plain\nX: y")));
}

void FetchTest::treatsCorsSimpleRequestsAsSimple()
{
    network::HeaderList headers;
    QVERIFY(network::FetchPolicy::isSimpleRequest(QStringLiteral("GET"), headers));
    QVERIFY(network::FetchPolicy::isSimpleRequest(QStringLiteral("POST"), headers));

    // A safelisted header keeps it simple.
    headers.set(QStringLiteral("Accept"), QStringLiteral("application/json"));
    QVERIFY(network::FetchPolicy::isSimpleRequest(QStringLiteral("GET"), headers));

    // A header that needs permission does not.
    headers.set(QStringLiteral("X-Custom"), QStringLiteral("1"));
    QVERIFY(!network::FetchPolicy::isSimpleRequest(QStringLiteral("GET"), headers));

    // A method that needs permission does not, whatever the headers are.
    QVERIFY(!network::FetchPolicy::isSimpleRequest(QStringLiteral("DELETE"), {}));

    // A forbidden header can never be made simple: it is refused outright.
    network::HeaderList forbidden;
    forbidden.set(QStringLiteral("Host"), QStringLiteral("evil.example"));
    QVERIFY(!network::FetchPolicy::isSimpleRequest(QStringLiteral("GET"), forbidden));
}

void FetchTest::exposesSafelistedResponseHeaders()
{
    // A cross-origin response exposes these without naming them, because they
    // describe the response itself rather than its content.
    for (const QString &name : {QStringLiteral("Content-Type"), QStringLiteral("Content-Length"),
                               QStringLiteral("Cache-Control"), QStringLiteral("Expires"),
                               QStringLiteral("Last-Modified"), QStringLiteral("Pragma")}) {
        QVERIFY2(network::FetchPolicy::exposesHeader(name, {}),
                 qPrintable(QStringLiteral("%1 should be readable").arg(name)));
    }

    // The cache and content headers are case-insensitive.
    QVERIFY(network::FetchPolicy::exposesHeader(QStringLiteral("content-type"), {}));

    // A header that describes the resource rather than the reply is not exposed
    // without being named: that is what stops a page reading, say, an internal
    // request id.
    QVERIFY(!network::FetchPolicy::exposesHeader(QStringLiteral("X-Request-Id"), {}));
    QVERIFY(!network::FetchPolicy::exposesHeader(QStringLiteral("Set-Cookie"), {}));
}

void FetchTest::exposesNamedResponseHeaders()
{
    // Access-Control-Expose-Headers is what opens a header up.
    const QStringList exposed = {QStringLiteral("X-Request-Id"), QStringLiteral("X-Rate-Limit")};
    QVERIFY(network::FetchPolicy::exposesHeader(QStringLiteral("X-Request-Id"), exposed));
    QVERIFY(network::FetchPolicy::exposesHeader(QStringLiteral("x-rate-limit"), exposed));
    QVERIFY(!network::FetchPolicy::exposesHeader(QStringLiteral("X-Other"), exposed));

    // A wildcard exposes everything, which is how a server opts out of the rule.
    QVERIFY(network::FetchPolicy::exposesHeader(QStringLiteral("X-Other"),
                                               {QStringLiteral("*")}));

    // But not for a request that carried credentials: a server that answered "*"
    // has not decided to trust any particular origin with the session, so naming
    // no header means naming none of the sensitive ones.
    QVERIFY(!network::FetchPolicy::exposesHeader(QStringLiteral("X-Other"), {QStringLiteral("*")},
                                                true));
    // A named header is still exposed for a credentialed request.
    QVERIFY(network::FetchPolicy::exposesHeader(QStringLiteral("X-Other"),
                                               {QStringLiteral("X-Other")}, true));
}

void FetchTest::filtersHeadersOfACrossOriginResponse()
{
    network::HeaderList headers;
    headers.append(QStringLiteral("Content-Type"), QStringLiteral("text/plain"));
    headers.append(QStringLiteral("X-Secret"), QStringLiteral("internal-42"));
    headers.append(QStringLiteral("X-Request-Id"), QStringLiteral("abc"));
    headers.append(QStringLiteral("Access-Control-Allow-Origin"), QStringLiteral("*"));

    // A response's headers describe more than its content, so a cross-origin
    // response is stripped down to the safelisted ones and whatever it chose to
    // name. Without this filter any page that asked for the resource could read
    // an internal request id, a rate-limit bucket or an internal host name.
    const network::HeaderList visible
        = network::FetchPolicy::crossOriginReadableHeaders(headers, QStringLiteral("*"), {}, false);

    QCOMPARE(visible.value(QStringLiteral("Content-Type")), QStringLiteral("text/plain"));
    QVERIFY2(visible.value(QStringLiteral("X-Secret")).isEmpty(),
             "an unnamed header must not be exposed cross-origin");
    QVERIFY2(visible.value(QStringLiteral("X-Request-Id")).isEmpty(),
             "an unnamed header must not be exposed cross-origin");

    // What the server names is what becomes visible.
    const network::HeaderList named = network::FetchPolicy::crossOriginReadableHeaders(
        headers, QStringLiteral("*"), {QStringLiteral("X-Request-Id")}, false);
    QCOMPARE(named.value(QStringLiteral("X-Request-Id")), QStringLiteral("abc"));
    QVERIFY2(named.value(QStringLiteral("X-Secret")).isEmpty(),
             "naming one header must not expose the others");

    // And a wildcard grant does not open everything up for a request that
    // carried the user's session.
    const network::HeaderList credentialed = network::FetchPolicy::crossOriginReadableHeaders(
        headers, QStringLiteral("*"), {QStringLiteral("*")}, true);
    QVERIFY2(credentialed.value(QStringLiteral("X-Secret")).isEmpty(),
             "a wildcard must not expose arbitrary headers to a credentialed request");
    QCOMPARE(credentialed.value(QStringLiteral("Content-Type")), QStringLiteral("text/plain"));
}

void FetchTest::corsGrantMatchesTheSerialisedOrigin()
{
    // A server writes a default port out never, so the form a CORS grant is
    // compared against must not carry one either. The two are deliberately
    // different: origin() keeps the port so that an origin is a comparable
    // identity, while this is the form that goes in a header.
    QCOMPARE(network::Url::parse(QStringLiteral("http://example.com/page")).serialisedOrigin(),
             QStringLiteral("http://example.com"));
    QCOMPARE(network::Url::parse(QStringLiteral("https://example.com:443/x")).serialisedOrigin(),
             QStringLiteral("https://example.com"));

    // origin() still distinguishes them, which is what same-origin checks need.
    QCOMPARE(network::Url::parse(QStringLiteral("https://example.com/x")).origin(),
             QStringLiteral("https://example.com:443"));

    // A port that is not the scheme's default is part of the origin, because it
    // is a different server.
    QCOMPARE(network::Url::parse(QStringLiteral("http://example.com:8080/x")).serialisedOrigin(),
             QStringLiteral("http://example.com:8080"));

    // So a grant written the way a server writes it matches.
    network::FetchPolicy::CorsHeaders cors;
    cors.allowOrigin = QStringLiteral("http://example.com");
    QVERIFY(network::FetchPolicy::checkReadable(
                cors, network::Url::parse(QStringLiteral("http://example.com/p")).serialisedOrigin(),
                false)
                .readable);
}

void FetchTest::refusesCrossOriginWithoutGrant()
{
    network::FetchPolicy::CorsHeaders cors;
    const auto result = network::FetchPolicy::checkReadable(cors, QStringLiteral("https://a.example"), false);

    QVERIFY(!result.readable);
    QVERIFY(!result.reason.isEmpty());
}

void FetchTest::acceptsMatchingOrigin()
{
    network::FetchPolicy::CorsHeaders cors;
    cors.allowOrigin = QStringLiteral("https://a.example");

    const auto result = network::FetchPolicy::checkReadable(cors, QStringLiteral("https://a.example"), false);
    QVERIFY2(result.readable, qPrintable(result.reason));

    // A grant for a different origin does not authorise this one.
    const auto other = network::FetchPolicy::checkReadable(cors, QStringLiteral("https://b.example"), false);
    QVERIFY(!other.readable);
}

void FetchTest::acceptsWildcardForAnonymousRequest()
{
    network::FetchPolicy::CorsHeaders cors;
    cors.allowOrigin = QStringLiteral("*");

    // A wildcard is an answer to a request that carried no credentials: the
    // server said it does not care who reads it.
    const auto result = network::FetchPolicy::checkReadable(cors, QStringLiteral("https://a.example"), false);
    QVERIFY2(result.readable, qPrintable(result.reason));
}

void FetchTest::refusesWildcardWhenCredentialsSent()
{
    network::FetchPolicy::CorsHeaders cors;
    cors.allowOrigin = QStringLiteral("*");

    // *Once the request carried the user's cookies, "*" is not an answer: a
    // server that replies with it has not decided to trust any particular origin
    // with the session, so exposing the body would be exposing the user's data
    // to whoever asked.
    const auto result = network::FetchPolicy::checkReadable(cors, QStringLiteral("https://a.example"), true);
    QVERIFY(!result.readable);
    QVERIFY(result.reason.contains(QStringLiteral("credentials")));
}

void FetchTest::refusesNamedOriginWithoutCredentialsFlag()
{
    network::FetchPolicy::CorsHeaders cors;
    cors.allowOrigin = QStringLiteral("https://a.example");

    // A named origin with credentials also requires the explicit second header,
    // so that a server which echoes back whatever it is sent still has to opt in.
    const auto without = network::FetchPolicy::checkReadable(cors, QStringLiteral("https://a.example"), true);
    QVERIFY(!without.readable);
    QVERIFY(without.reason.contains(QStringLiteral("Allow-Credentials")));

    cors.allowCredentials = true;
    const auto with = network::FetchPolicy::checkReadable(cors, QStringLiteral("https://a.example"), true);
    QVERIFY2(with.readable, qPrintable(with.reason));
}

void FetchTest::refusesGrantForAnotherOrigin()
{
    network::FetchPolicy::CorsHeaders cors;
    cors.allowOrigin = QStringLiteral("https://a.example");

    // The value is compared as an origin, so a trailing slash or a default port
    // spelled out makes it not match - which is what a browser does, and what
    // stops a grant for one origin being read as a grant for another.
    QVERIFY(!network::FetchPolicy::checkReadable(cors, QStringLiteral("https://a.example/"), false).readable);
    QVERIFY(!network::FetchPolicy::checkReadable(cors, QStringLiteral("https://a.example:443"), false).readable);

    // The scheme matters: a grant for http is not a grant for https.
    QVERIFY(!network::FetchPolicy::checkReadable(cors, QStringLiteral("http://a.example"), false).readable);
}

void FetchTest::splitsHeaderLists()
{
    const QStringList parts = network::FetchPolicy::splitHeaderList(
        QStringLiteral(" X-One , X-Two,  ,X-Three "));
    QCOMPARE(parts.size(), 3);
    QCOMPARE(parts.at(0), QStringLiteral("X-One"));
    QCOMPARE(parts.at(1), QStringLiteral("X-Two"));
    QCOMPARE(parts.at(2), QStringLiteral("X-Three"));

    QVERIFY(network::FetchPolicy::splitHeaderList(QString()).isEmpty());
}

// ----------------------------------------------------------------- bindings

void FetchTest::exposesFetchAsAFunction()
{
    // A page feature detects this, so it has to be true or every modern site
    // takes its fallback path.
    QCOMPARE(eval(QStringLiteral("typeof fetch")), QStringLiteral("function"));
    QCOMPARE(eval(QStringLiteral("typeof Response")), QStringLiteral("function"));
    QCOMPARE(eval(QStringLiteral("typeof Headers")), QStringLiteral("function"));
    QCOMPARE(eval(QStringLiteral("fetch.length")), QStringLiteral("1"));
}

void FetchTest::rejectsWhenNoProviderIsInstalled()
{
    // A build with no loader still sees the global - a page feature detects it -
    // and calling it fails loudly rather than hanging or throwing out of the
    // call, because a rejection is what a page already knows how to handle.
    auto document = html::Parser::parse(QStringLiteral("<html><body></body></html>"),
                                        network::Url::parse(QStringLiteral("https://example.com/")));
    Engine engine(document.document.get());

    QCOMPARE(engine.evaluate(QStringLiteral("typeof fetch")).value, QStringLiteral("function"));

    // The handler runs on the job queue rather than during the call, so the
    // promise is still pending immediately afterwards ...
    const ExecutionResult immediate = engine.evaluate(QStringLiteral(
        "var out = 'pending';"
        "fetch('/x').catch(function (e) { out = String(e); });"
        "out"));
    QVERIFY(immediate.success);
    QCOMPARE(immediate.value, QStringLiteral("pending"));

    // ... and the engine's own queue drain is what settles it.
    const ExecutionResult after = engine.evaluate(QStringLiteral(
        "var seen = 'pending';"
        "fetch('/y').catch(function (e) { seen = String(e); });"
        "seen"));
    QVERIFY(after.success);

    // Reported as unavailable, and only after a turn.
    const QString finalValue = after.value;
    if (finalValue == QStringLiteral("pending")) {
        // The first call's rejection has now run, which is itself the proof that
        // the promise settles rather than hanging.
        QVERIFY(!engine.hasPendingWork());
    } else {
        QVERIFY(finalValue.contains(QStringLiteral("unavailable")));
    }
}

void FetchTest::resolvesWithStatusAndHeaders()
{
    FakeProvider::Answer answer;
    answer.status = 200;
    answer.statusText = QStringLiteral("OK");
    answer.headers.append({QStringLiteral("Content-Type"), QStringLiteral("application/json")});
    answer.headers.append({QStringLiteral("X-Trace"), QStringLiteral("abc123")});
    answer.body = "{\"ok\":true}";
    m_provider->fallback = answer;

    eval(QStringLiteral(
        "var seen = {};"
        "fetch('/data').then(function (r) {"
        "  seen.status = r.status;"
        "  seen.text = r.statusText;"
        "  seen.ok = r.ok;"
        "  seen.type = r.headers.get('content-type');"
        "  seen.trace = r.headers.get('x-trace');"
        "  seen.has = r.headers.has('X-Trace');"
        "  seen.missing = r.headers.get('X-Absent');"
        "});"));

    QVERIFY(waitFor([&] { return eval(QStringLiteral("typeof seen.status")) == QStringLiteral("number"); }));

    QCOMPARE(eval(QStringLiteral("seen.status")), QStringLiteral("200"));
    QCOMPARE(eval(QStringLiteral("seen.text")), QStringLiteral("OK"));
    QCOMPARE(eval(QStringLiteral("seen.ok")), QStringLiteral("true"));
    // Header names are case-insensitive, which is why the lookup above used a
    // different case from the one the server sent.
    QCOMPARE(eval(QStringLiteral("seen.type")), QStringLiteral("application/json"));
    QCOMPARE(eval(QStringLiteral("seen.trace")), QStringLiteral("abc123"));
    QCOMPARE(eval(QStringLiteral("seen.has")), QStringLiteral("true"));
}

void FetchTest::resolvesHttpErrorStatusInsteadOfRejecting()
{
    FakeProvider::Answer answer;
    answer.status = 404;
    answer.body = "not here";
    m_provider->fallback = answer;

    eval(QStringLiteral(
        "var code = 0, okValue = true, bodyText = '', rejected = false;"
        "fetch('/gone').then(function (r) {"
        "  code = r.status; okValue = r.ok;"
        "  return r.text();"
        "}).then(function (t) { bodyText = t; })"
        ".catch(function () { rejected = true; });"));

    QVERIFY(waitFor([&] { return eval(QStringLiteral("bodyText")) != QStringLiteral(""); }));

    // This is the distinction a page relies on to tell "the server said no" from
    // "the request never happened".
    QCOMPARE(eval(QStringLiteral("code")), QStringLiteral("404"));
    QCOMPARE(eval(QStringLiteral("okValue")), QStringLiteral("false"));
    QCOMPARE(eval(QStringLiteral("bodyText")), QStringLiteral("not here"));
    QCOMPARE(eval(QStringLiteral("rejected")), QStringLiteral("false"));
}

void FetchTest::rejectsOnTransportFailure()
{
    FakeProvider::Answer answer;
    answer.error = QStringLiteral("the connection was closed");
    m_provider->fallback = answer;

    eval(QStringLiteral(
        "var caught = '';"
        "fetch('/x').then(function () { caught = 'resolved'; })"
        ".catch(function (e) { caught = String(e); });"));

    QVERIFY(waitFor([&] { return eval(QStringLiteral("caught")) != QStringLiteral(""); }));

    const QString caught = eval(QStringLiteral("caught"));
    QVERIFY(caught != QStringLiteral("resolved"));
    QVERIFY(caught.contains(QStringLiteral("closed")));
}

void FetchTest::readsTextBody()
{
    FakeProvider::Answer answer;
    answer.body = "plain text body";
    m_provider->fallback = answer;

    eval(QStringLiteral(
        "var text = 'pending';"
        "fetch('/t').then(function (r) { return r.text(); }).then(function (t) { text = t; });"));

    QVERIFY(waitFor([&] { return eval(QStringLiteral("text")) != QStringLiteral("pending"); }));
    QCOMPARE(eval(QStringLiteral("text")), QStringLiteral("plain text body"));
}

void FetchTest::readsJsonBody()
{
    FakeProvider::Answer answer;
    answer.headers.append({QStringLiteral("Content-Type"), QStringLiteral("application/json")});
    answer.body = "{\"name\":\"openq\",\"items\":[1,2,3],\"nested\":{\"deep\":true}}";
    m_provider->fallback = answer;

    eval(QStringLiteral(
        "var parsed = null;"
        "fetch('/j').then(function (r) { return r.json(); }).then(function (d) { parsed = d; });"));

    QVERIFY(waitFor([&] { return eval(QStringLiteral("parsed !== null")) == QStringLiteral("true"); }));

    QCOMPARE(eval(QStringLiteral("parsed.name")), QStringLiteral("openq"));
    QCOMPARE(eval(QStringLiteral("parsed.items.length")), QStringLiteral("3"));
    QCOMPARE(eval(QStringLiteral("parsed.items[1]")), QStringLiteral("2"));
    QCOMPARE(eval(QStringLiteral("parsed.nested.deep")), QStringLiteral("true"));
}

void FetchTest::rejectsJsonThatIsNotJson()
{
    FakeProvider::Answer answer;
    answer.body = "<html>this is not json</html>";
    m_provider->fallback = answer;

    eval(QStringLiteral(
        "var outcome = 'pending';"
        "fetch('/notjson').then(function (r) { return r.json(); })"
        ".then(function () { outcome = 'resolved'; })"
        ".catch(function (e) { outcome = 'rejected:' + e.name; });"));

    QVERIFY(waitFor([&] { return eval(QStringLiteral("outcome")) != QStringLiteral("pending"); }));

    // A rejected promise, not a syntax error thrown out of the call: a page that
    // pipes a response through .json() expects to handle the failure.
    QCOMPARE(eval(QStringLiteral("outcome")), QStringLiteral("rejected:SyntaxError"));
}

void FetchTest::readsArrayBuffer()
{
    FakeProvider::Answer answer;
    answer.body = QByteArray("\x01\x02\x03\x04", 4);
    m_provider->fallback = answer;

    eval(QStringLiteral(
        "var size = -1, first = -1;"
        "fetch('/bin').then(function (r) { return r.arrayBuffer(); }).then(function (b) {"
        "  size = b.byteLength;"
        "  first = new Uint8Array(b)[0];"
        "});"));

    QVERIFY(waitFor([&] { return eval(QStringLiteral("size")) != QStringLiteral("-1"); }));
    QCOMPARE(eval(QStringLiteral("size")), QStringLiteral("4"));
    QCOMPARE(eval(QStringLiteral("first")), QStringLiteral("1"));
}

void FetchTest::reportsAnAbsentHeaderAsNull()
{
    FakeProvider::Answer answer;
    answer.headers.append({QStringLiteral("X-Present"), QStringLiteral("")});
    m_provider->fallback = answer;

    eval(QStringLiteral("var absent = 'pending', present = 'pending';"
                        "fetch('/h').then(function (r) {"
                        "  absent = r.headers.get('X-Missing');"
                        "  present = r.headers.get('X-Present');"
                        "});"));

    QVERIFY(waitFor([&] { return eval(QStringLiteral("absent")) != QStringLiteral("pending"); }));

    // A header that was not sent is null; one sent empty is an empty string. A
    // page that could not tell them apart would treat a deliberately blank header
    // as a failure.
    QCOMPARE(eval(QStringLiteral("absent")), QStringLiteral("null"));
    QCOMPARE(eval(QStringLiteral("present")), QString());
}

void FetchTest::reportsResponseUrlAndRedirected()
{
    FakeProvider::Answer answer;
    answer.url = QStringLiteral("https://cdn.example/final.json");
    answer.redirected = true;
    m_provider->fallback = answer;

    eval(QStringLiteral("var info = {};"
                        "fetch('/start').then(function (r) {"
                        "  info.url = r.url; info.redirected = r.redirected;"
                        "});"));

    QVERIFY(waitFor([&] { return eval(QStringLiteral("typeof info.url")) == QStringLiteral("string"); }));

    QCOMPARE(eval(QStringLiteral("info.url")), QStringLiteral("https://cdn.example/final.json"));
    QCOMPARE(eval(QStringLiteral("info.redirected")), QStringLiteral("true"));
}

void FetchTest::refusesForbiddenHeaderAtTheBinding()
{
    m_provider->fallback = FakeProvider::Answer{};

    eval(QStringLiteral(
        "fetch('/x', {headers: {'X-Allowed': 'yes', 'Host': 'evil.example',"
        " 'Content-Length': '999', 'Cookie': 'session=stolen'}});"));

    QVERIFY(waitFor([&] { return !m_provider->requests.isEmpty(); }));

    // The forbidden names never reach the request. They are dropped rather than
    // failing the call, which is what a browser does: the page asked for
    // something the browser will not do, not made a mistake it must hear about.
    const auto &request = m_provider->requests.first();
    QStringList sent;
    for (const auto &header : request.headers)
        sent.append(header.first);

    QVERIFY2(sent.contains(QStringLiteral("X-Allowed")), qPrintable(sent.join(QStringLiteral(","))));
    QVERIFY2(!sent.contains(QStringLiteral("Host")), qPrintable(sent.join(QStringLiteral(","))));
    QVERIFY2(!sent.contains(QStringLiteral("Content-Length")), qPrintable(sent.join(QStringLiteral(","))));
    QVERIFY2(!sent.contains(QStringLiteral("Cookie")), qPrintable(sent.join(QStringLiteral(","))));
}

void FetchTest::sendsPostBodyAndCustomHeader()
{
    m_provider->fallback = FakeProvider::Answer{};

    eval(QStringLiteral(
        "fetch('/submit', {method: 'post', body: 'name=openq',"
        " headers: {'X-Token': 't1'}});"));

    QVERIFY(waitFor([&] { return !m_provider->requests.isEmpty(); }));

    const auto &request = m_provider->requests.first();
    // The method is upper-cased, which is what goes on the wire.
    QCOMPARE(request.method, QStringLiteral("POST"));
    QCOMPARE(QString::fromUtf8(request.body), QStringLiteral("name=openq"));

    QString token;
    for (const auto &header : request.headers) {
        if (header.first.compare(QStringLiteral("X-Token"), Qt::CaseInsensitive) == 0)
            token = header.second;
    }
    QCOMPARE(token, QStringLiteral("t1"));

    // The document URL travels with the request: it supplies the referrer and the
    // origin a CORS grant has to match.
    QCOMPARE(request.documentUrl, QStringLiteral("https://example.com/page"));
}

void FetchTest::resolvesRelativeUrlAgainstDocument()
{
    m_provider->fallback = FakeProvider::Answer{};

    eval(QStringLiteral("fetch('relative/path.json');"));

    QVERIFY(waitFor([&] { return !m_provider->requests.isEmpty(); }));
    QCOMPARE(m_provider->requests.first().url, QStringLiteral("https://example.com/relative/path.json"));

    m_provider->requests.clear();
    eval(QStringLiteral("fetch('/rooted.json');"));
    QVERIFY(waitFor([&] { return !m_provider->requests.isEmpty(); }));
    QCOMPARE(m_provider->requests.first().url, QStringLiteral("https://example.com/rooted.json"));
}

void FetchTest::rejectsInvalidUrlWithoutARequest()
{
    eval(QStringLiteral("var message = 'pending';"
                        "fetch('http://[not a url').catch(function (e) { message = String(e); });"));

    QVERIFY(waitFor([&] { return eval(QStringLiteral("message")) != QStringLiteral("pending"); }));

    // Nothing was asked for, and the rejection says why.
    QVERIFY(m_provider->requests.isEmpty());
    QVERIFY(eval(QStringLiteral("message")).contains(QStringLiteral("not a valid URL")));
}

void FetchTest::runsThenHandlersAcrossTurns()
{
    m_provider->fallback = FakeProvider::Answer{};

    // A handler attached after the response arrived still runs. This is the whole
    // reason a fetch returns a promise rather than a value: a page that attaches
    // its handler in a later turn must not lose the response.
    eval(QStringLiteral("var order = [];"
                        "var p = fetch('/x').then(function () { order.push('first'); });"
                        "p.then(function () { order.push('second'); });"
                        "order.push('sync');"));

    QCOMPARE(eval(QStringLiteral("order.join(',')")), QStringLiteral("sync"));

    QVERIFY(waitFor([&] { return eval(QStringLiteral("order.length")) == QStringLiteral("3"); }));
    QCOMPARE(eval(QStringLiteral("order.join(',')")), QStringLiteral("sync,first,second"));
}

void FetchTest::keepsPendingWorkVisibleWhileInFlight()
{
    m_provider->neverAnswers = true;

    eval(QStringLiteral("fetch('/slow');"));

    // The page counts as still working while a request is outstanding. Without
    // this the frame clock would stop and the promise would never settle, so the
    // page would look frozen with no error to explain it.
    QVERIFY2(m_engine->hasPendingWork(), "an in-flight fetch must count as pending work");
}

void FetchTest::aRepeatingTimerNeverStopsBeingWork()
{
    // A page never has to stop, and this is the shape of that: an interval keeps
    // work outstanding however many frames are run. It is recorded here because
    // it is the reason a caller that waits for a page to go quiet needs a
    // deadline of its own - a headless dump that waited for this would hang on
    // any page with a polling loop, which is most of them.
    eval(QStringLiteral("setInterval(function () {}, 10);"));

    for (int frame = 0; frame < 5; ++frame)
        m_engine->runDueTimers(frame * 20);

    QVERIFY2(m_engine->hasPendingWork(),
             "a repeating timer keeps the page working indefinitely");

    // Clearing it is what lets the page finish, which is the other half of the
    // contract: the work is real, not a stuck flag.
    eval(QStringLiteral("for (var i = 1; i <= 100; ++i) clearInterval(i);"));

    // One more frame so the queue is drained after the clear.
    m_engine->runDueTimers(1000);
    QVERIFY2(!m_engine->hasPendingWork(), "clearing the interval must let the page finish");
}

void FetchTest::abortsWithoutSettling()
{
    m_provider->neverAnswers = true;

    eval(QStringLiteral("var settled = false;"
                        "var p = fetch('/slow');"
                        "p.then(function () { settled = true; }, function () { settled = true; });"));

    QVERIFY2(m_engine->hasPendingWork(), "the request should be in flight");

    // The engine's own cancel path is what a navigation uses. An aborted promise
    // stays pending rather than resolving or rejecting, which is what a browser
    // does and what a page's own abort handling is built on.
    m_provider->abortScriptFetch(1);
    QCOMPARE(eval(QStringLiteral("settled")), QStringLiteral("false"));
}

void FetchTest::cancelsOutstandingFetchesOnNavigation()
{
    m_provider->neverAnswers = true;

    eval(QStringLiteral("var outcome = 'pending';"
                        "fetch('/slow').then(function () { outcome = 'resolved'; },"
                        " function () { outcome = 'rejected'; });"));

    QVERIFY(m_engine->hasPendingWork());

    // Navigating away replaces the document. A response that arrives afterwards
    // must not resolve into the new page, or a stale answer would overwrite the
    // page the user actually navigated to.
    load(QStringLiteral("<html><body><div id='other'>new page</div></body></html>"));

    QCOMPARE(eval(QStringLiteral("outcome")), QStringLiteral("pending"));
    QVERIFY2(!m_engine->hasPendingWork(), "a replaced document must not leave fetches outstanding");
}

QTEST_MAIN(FetchTest)
#include "tst_fetch.moc"
