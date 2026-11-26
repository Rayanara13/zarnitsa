// Живые проверки на loopback: TCP, HTTP, SIP, DNS, все пути ICMP и API с токеном и SSE.
#include "api/ApiController.h"
#include "api/HttpServer.h"
#include "core/EventLog.h"
#include "core/Monitor.h"
#include "probes/IcmpEngine.h"
#include "probes/NetProbers.h"
#include "probes/ProbeDispatcher.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUdpSocket>
#include <QtTest>

#include <memory>

using namespace zr;

namespace {

// Запустить одну проверку и дождаться ответа
ProbeResult run(ProbeBackend *b, ProbeRequest req, int waitMs = 5000)
{
    static quint64 token = 1000;
    req.token = ++token;
    ProbeResult out;
    bool done = false;
    QMetaObject::Connection c = QObject::connect(b, &ProbeBackend::finished, [&](const ProbeResult &r) {
        if (r.token == req.token) { out = r; done = true; }
    });
    b->start(req);
    QElapsedTimer t;
    t.start();
    while (!done && t.elapsed() < waitMs) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QObject::disconnect(c);
    if (!done) out.error = QStringLiteral("нет ответа в тесте");
    return out;
}

struct Http {
    int code = 0;
    QByteArray body;
    QByteArray type;
};

Http request(const QByteArray &verb, const QUrl &url, const QByteArray &token, const QByteArray &body = QByteArray(),
             const QByteArray &ctype = "application/json")
{
    QNetworkAccessManager nam;
    QNetworkRequest req(url);
    if (!token.isEmpty()) req.setRawHeader("Authorization", "Bearer " + token);
    if (!body.isEmpty()) req.setRawHeader("Content-Type", ctype);
    QNetworkReply *r = nam.sendCustomRequest(req, verb, body);
    QEventLoop loop;
    QObject::connect(r, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    loop.exec();
    Http h;
    h.code = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    h.body = r->readAll();
    h.type = r->header(QNetworkRequest::ContentTypeHeader).toByteArray();
    r->deleteLater();
    return h;
}

// TLS-сервер с keep-alive, как у Росы на Go: соединения живут между запросами
class KeepAliveTls : public QTcpServer {
public:
    QSslCertificate cert;
    QSslKey key;
    int connections = 0;

protected:
    void incomingConnection(qintptr fd) override
    {
        auto *s = new QSslSocket(this);
        s->setSocketDescriptor(fd);
        s->setLocalCertificate(cert);
        s->setPrivateKey(key);
        ++connections;
        auto buf = std::make_shared<QByteArray>();
        connect(s, &QSslSocket::readyRead, s, [s, buf]() {
            *buf += s->readAll();
            int end;
            static const QByteArray kEnd("\r\n\r\n");
            static const QByteArray kReply("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 15\r\n"
                                           "Connection: keep-alive\r\n\r\n{\"status\":\"ok\"}");
            while ((end = buf->indexOf(kEnd)) >= 0) {
                buf->remove(0, end + 4);
                s->write(kReply);
            }
        });
        connect(s, &QSslSocket::disconnected, s, &QObject::deleteLater);
        s->startServerEncryption();
    }
};

} // namespace

class TestNetwork : public QObject {
    Q_OBJECT
private slots:
    void tcp();
    void http();
    void httpPartialBody_data();
    void httpPartialBody();
    void httpsPinningNoReuse();
    void httpsPinOverridesInsecure();
    void httpRequestFraming_data();
    void httpRequestFraming();
    void eventStreamWriteLimit_data();
    void eventStreamWriteLimit();
    void sip();
    void sipSourcePeer_data();
    void sipSourcePeer();
    void dns();
    void icmpSocket();
    void icmpSystemPing();
    void icmpHelper();
    void icmpEngineAuto();
    void dispatcherNameError();
    void api();
    void apiEvents();
    void apiRequiresTlsOutside();
};

void TestNetwork::tcp()
{
    QTcpServer srv;
    QVERIFY(srv.listen(QHostAddress::LocalHost, 0));
    TcpProber p;
    ProbeRequest req;
    req.kind = ProbeKind::Tcp;
    req.host = QStringLiteral("127.0.0.1");
    req.port = srv.serverPort();
    req.timeoutMs = 1000;
    ProbeResult r = run(&p, req);
    QVERIFY2(r.ok, qPrintable(r.error));
    QVERIFY(r.rttMs >= 0);
    const quint16 closed = srv.serverPort();
    srv.close();
    req.port = closed;
    r = run(&p, req);
    QCOMPARE(int(r.outcome), int(ProbeOutcome::Refused));
}

void TestNetwork::http()
{
    HttpServer srv;
    srv.setHandler([](const HttpRequest &req, HttpConnection *c) {
        HttpResponse resp;
        if (req.path == QLatin1String("/ok")) { resp.body = "всё хорошо"; resp.contentType = "text/plain; charset=utf-8"; }
        else if (req.path == QLatin1String("/slow")) { QTimer::singleShot(1500, c, [c]() { HttpResponse x; x.body = "late"; c->respond(x); }); return; }
        else if (req.path == QLatin1String("/error")) { resp.status = 500; resp.body = "{}"; }
        else { resp.status = 404; resp.body = "{}"; }
        c->respond(resp);
    });
    QVERIFY(srv.listen(QHostAddress::LocalHost, 0));
    const QString base = QStringLiteral("http://127.0.0.1:%1").arg(srv.serverPort());
    HttpProber p;
    ProbeRequest req;
    req.kind = ProbeKind::Http;
    req.timeoutMs = 2000;
    req.host = base + QStringLiteral("/ok");
    ProbeResult r = run(&p, req);
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(r.code, 200);
    req.httpMatch = QStringLiteral("хорошо");
    QVERIFY(run(&p, req).ok);
    req.httpMatch = QStringLiteral("плохо");
    QCOMPARE(int(run(&p, req).outcome), int(ProbeOutcome::BadResponse));
    req.httpMatch.clear();
    req.host = base + QStringLiteral("/missing");
    r = run(&p, req);
    QCOMPARE(int(r.outcome), int(ProbeOutcome::BadResponse));
    QCOMPARE(r.code, 404);
    req.httpExpect = 404;
    QVERIFY(run(&p, req).ok);
    req.httpExpect = 500;
    req.host = base + QStringLiteral("/error");
    QVERIFY(run(&p, req).ok);
    req.httpExpect = 0;
    req.host = base + QStringLiteral("/slow");
    req.timeoutMs = 400;
    r = run(&p, req);
    QVERIFY2(r.outcome == ProbeOutcome::Timeout, qPrintable(r.error));
}

void TestNetwork::httpPartialBody_data()
{
    QTest::addColumn<bool>("closeEarly");
    QTest::addColumn<bool>("trickle");
    QTest::addColumn<int>("outcome");
    QTest::newRow("stalled-body") << false << false << int(ProbeOutcome::Timeout);
    QTest::newRow("truncated-body") << true << false << int(ProbeOutcome::Error);
    QTest::newRow("body-past-deadline") << false << true << int(ProbeOutcome::Timeout);
}

void TestNetwork::httpPartialBody()
{
    QFETCH(bool, closeEarly);
    QFETCH(bool, trickle);
    QFETCH(int, outcome);
    QTcpServer srv;
    QVERIFY(srv.listen(QHostAddress::LocalHost, 0));
    connect(&srv, &QTcpServer::newConnection, &srv, [&srv, closeEarly, trickle]() {
        auto *s = srv.nextPendingConnection();
        auto buf = std::make_shared<QByteArray>();
        auto replied = std::make_shared<bool>(false);
        connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
        connect(s, &QTcpSocket::readyRead, s, [s, buf, replied, closeEarly, trickle]() {
            *buf += s->readAll();
            if (*replied || !buf->contains("\r\n\r\n")) return;
            *replied = true;
            s->write("HTTP/1.1 200 OK\r\nContent-Length: 1000000\r\nConnection: keep-alive\r\n\r\npartial");
            if (closeEarly) {
                s->disconnectFromHost();
            } else if (trickle) {
                // Данные приходят чаще idle-таймаута, но тело не успеет к общему дедлайну.
                auto *timer = new QTimer(s);
                timer->setInterval(30);
                connect(timer, &QTimer::timeout, s, [s]() { s->write("x"); });
                connect(s, &QTcpSocket::disconnected, timer, &QTimer::stop);
                timer->start();
            }
        });
    });
    HttpProber p;
    ProbeRequest req;
    req.kind = ProbeKind::Http;
    req.host = QStringLiteral("http://127.0.0.1:%1/partial").arg(srv.serverPort());
    req.timeoutMs = 150;
    req.httpMatch = QStringLiteral("partial");
    const ProbeResult r = run(&p, req);
    QVERIFY2(!r.ok, "HTTP 200 с неполным телом ошибочно принят за успех");
    QCOMPARE(int(r.outcome), outcome);
    QCOMPARE(r.code, 200);
}

void TestNetwork::httpsPinningNoReuse()
{
    const QString data = QStringLiteral(ZR_TESTDATA);
    QFile cf(data + QStringLiteral("/test-cert.pem")), kf(data + QStringLiteral("/test-key.pem")), sf(data + QStringLiteral("/test-cert.sha256"));
    QVERIFY(cf.open(QIODevice::ReadOnly) && kf.open(QIODevice::ReadOnly) && sf.open(QIODevice::ReadOnly));
    KeepAliveTls srv;
    srv.cert = QSslCertificate(cf.readAll(), QSsl::Pem);
    srv.key = QSslKey(kf.readAll(), QSsl::Ec, QSsl::Pem);
    QVERIFY(!srv.cert.isNull() && !srv.key.isNull());
    const QString pin = QString::fromLatin1(sf.readAll()).trimmed();
    QVERIFY(srv.listen(QHostAddress::LocalHost, 0));
    HttpProber p;
    ProbeRequest req;
    req.kind = ProbeKind::Http;
    req.timeoutMs = 3000;
    req.host = QStringLiteral("https://127.0.0.1:%1/healthz").arg(srv.serverPort());

    ProbeRequest pinned = req;
    pinned.tlsPin = pin;
    ProbeResult r = run(&p, pinned);
    QVERIFY2(r.ok, qPrintable(r.error));
    QVERIFY(r.certExpiry > 0);
    // без отпечатка самоподписанный сертификат не принимается, даже если рядом открыто соединение по отпечатку
    r = run(&p, req);
    QVERIFY2(!r.ok, "проверка сертификата обойдена через чужое соединение");
    ProbeRequest wrong = req;
    wrong.tlsPin = QStringLiteral("00ff00ff");
    r = run(&p, wrong);
    QVERIFY(!r.ok);
    QVERIFY2(r.error.contains(QStringLiteral("не совпал")), qPrintable(r.error));
    ProbeRequest insecure = req;
    insecure.tlsInsecure = true;
    QVERIFY(run(&p, insecure).ok);
    QVERIFY(run(&p, pinned).ok);
    QVERIFY(!run(&p, req).ok);
}

void TestNetwork::httpsPinOverridesInsecure()
{
    const QString data = QStringLiteral(ZR_TESTDATA);
    QFile cf(data + QStringLiteral("/test-cert.pem")), kf(data + QStringLiteral("/test-key.pem")), sf(data + QStringLiteral("/test-cert.sha256"));
    QVERIFY(cf.open(QIODevice::ReadOnly) && kf.open(QIODevice::ReadOnly) && sf.open(QIODevice::ReadOnly));
    KeepAliveTls srv;
    srv.cert = QSslCertificate(cf.readAll(), QSsl::Pem);
    srv.key = QSslKey(kf.readAll(), QSsl::Ec, QSsl::Pem);
    QVERIFY(!srv.cert.isNull() && !srv.key.isNull());
    const QString pin = QString::fromLatin1(sf.readAll()).trimmed();
    QVERIFY(srv.listen(QHostAddress::LocalHost, 0));
    HttpProber p;
    ProbeRequest insecure;
    insecure.kind = ProbeKind::Http;
    insecure.host = QStringLiteral("https://127.0.0.1:%1/healthz").arg(srv.serverPort());
    insecure.timeoutMs = 3000;
    insecure.tlsInsecure = true;
    QVERIFY(run(&p, insecure).ok);
    QVERIFY(run(&p, insecure).ok);
    QCOMPARE(srv.connections, 1); // Проверяем, что сервер действительно сохраняет соединение.

    ProbeRequest wrong = insecure;
    wrong.tlsPin = QString(64, QLatin1Char('0'));
    const ProbeResult r = run(&p, wrong);
    QVERIFY2(!r.ok, "insecure обошёл проверку закреплённого сертификата");
    QCOMPARE(int(r.outcome), int(ProbeOutcome::Error));
    QVERIFY2(r.error.contains(QStringLiteral("не совпал")), qPrintable(r.error));
    QCOMPARE(srv.connections, 2);

    ProbeRequest pinned = insecure;
    pinned.tlsPin = pin;
    QVERIFY(run(&p, pinned).ok);
    QCOMPARE(srv.connections, 3);
    pinned.tlsInsecure = false;
    QVERIFY(run(&p, pinned).ok);
    QCOMPARE(srv.connections, 3); // Одна и та же pin-политика независимо от флага insecure.
    QVERIFY(run(&p, insecure).ok);
    QCOMPARE(srv.connections, 3);
}

void TestNetwork::httpRequestFraming_data()
{
    QTest::addColumn<QByteArray>("wire");
    QTest::addColumn<int>("status");
    QTest::addColumn<QByteArray>("body");
    const QByteArray first("POST /echo HTTP/1.1\r\nHost: localhost\r\n");
    const auto row = [&first](const char *name, const QByteArray &headers, const QByteArray &body, int status) {
        QTest::newRow(name) << first + headers + "\r\n" + body << status << body;
    };
    row("valid-length", "Content-Length: 4\r\n", "data", 200);
    row("valid-ows", "Content-Length:\t0004 \t\r\n", "data", 200);
    row("no-length", QByteArray(), QByteArray(), 200);
    row("duplicate-equal", "Content-Length: 0\r\ncontent-length: 0\r\n", QByteArray(), 400);
    row("duplicate-conflicting", "Content-Length: 4\r\nContent-Length: 0\r\n", "data", 400);
    row("duplicate-reversed", "Content-Length: 0\r\nContent-Length: 4\r\n", "data", 400);
    row("length-list", "Content-Length: 0, 0\r\n", QByteArray(), 400);
    row("chunked", "Transfer-Encoding: chunked\r\n", "0\r\n\r\n", 400);
    row("transfer-before-length", "Transfer-Encoding: chunked\r\nContent-Length: 0\r\n", "0\r\n\r\n", 400);
    row("length-before-transfer", "Content-Length: 0\r\nTransfer-Encoding: chunked\r\n", "0\r\n\r\n", 400);
    row("identity-transfer", "Transfer-Encoding: identity\r\n", QByteArray(), 400);
    row("signed-length", "Content-Length: +0\r\n", QByteArray(), 400);
    row("negative-length", "Content-Length: -1\r\n", QByteArray(), 400);
    row("empty-length", "Content-Length:\r\n", QByteArray(), 400);
    row("overflow-length", "Content-Length: 9223372036854775808\r\n", QByteArray(), 400);
    row("body-over-limit", "Content-Length: 1048577\r\n", QByteArray(), 413);
    row("space-before-colon", "Content-Length : 0\r\n", QByteArray(), 400);
    row("folded-header", " Content-Length: 0\r\n", QByteArray(), 400);
    row("missing-colon", "Content-Length 0\r\n", QByteArray(), 400);
    row("bare-lf", "X-Test: value\nContent-Length: 0\r\n", QByteArray(), 400);

    const QByteArray padded = first + "Content-Length: 4\r\nX-Pad: ";
    const QByteArray atLimit = padded + QByteArray(16 * 1024 - padded.size() - 4, 'x') + "\r\n\r\n";
    QTest::newRow("header-at-limit-with-body") << atLimit + "data" << 200 << QByteArray("data");
    const QByteArray overLimit = padded + QByteArray(16 * 1024 - padded.size() - 3, 'x') + "\r\n\r\n";
    QTest::newRow("complete-header-over-limit") << overLimit + "data" << 413 << QByteArray();
    QTest::newRow("unfinished-header-over-limit") << first + "X-Pad: " + QByteArray(16 * 1024, 'x') << 413 << QByteArray();
}

void TestNetwork::httpRequestFraming()
{
    QFETCH(QByteArray, wire);
    QFETCH(int, status);
    QFETCH(QByteArray, body);
    QTcpServer listener;
    QVERIFY(listener.listen(QHostAddress::LocalHost, 0));
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, listener.serverPort());
    QVERIFY(client.waitForConnected(2000));
    QTRY_VERIFY_WITH_TIMEOUT(listener.hasPendingConnections(), 2000);
    QTcpSocket *sock = listener.nextPendingConnection();
    QCOMPARE(client.write(wire), qint64(wire.size()));
    // Целый блок уже принят до установки парсера: случайная TCP-фрагментация не скроет дефект.
    QTRY_COMPARE_WITH_TIMEOUT(sock->bytesAvailable(), qint64(wire.size()), 2000);
    auto *conn = new HttpConnection(sock, &listener);
    int dispatched = 0;
    HttpRequest seen;
    connect(conn, &HttpConnection::request, &listener, [&](const HttpRequest &req, HttpConnection *c) {
        ++dispatched;
        seen = req;
        HttpResponse r;
        r.body = req.body;
        c->respond(r);
    });
    QVERIFY(QMetaObject::invokeMethod(sock, "readyRead", Qt::DirectConnection));
    QByteArray response;
    QTRY_VERIFY_WITH_TIMEOUT((response += client.readAll()).contains("\r\n\r\n"), 2000);
    QTRY_COMPARE_WITH_TIMEOUT(client.state(), QAbstractSocket::UnconnectedState, 2000);
    response += client.readAll();
    QCOMPARE(response.mid(9, 3).toInt(), status);
    QCOMPARE(response.count("HTTP/1.1 "), 1);
    QCOMPARE(dispatched, status == 200 ? 1 : 0);
    if (status == 200) {
        QCOMPARE(seen.body, body);
        QCOMPARE(response.mid(response.indexOf("\r\n\r\n") + 4), body);
    }
}

void TestNetwork::eventStreamWriteLimit_data()
{
    QTest::addColumn<int>("mode");
    QTest::newRow("event-over-pending-limit") << 0;
    QTest::newRow("comment-over-pending-limit") << 1;
    QTest::newRow("single-event-over-limit") << 2;
    QTest::newRow("headers-over-limit") << 3;
}

void TestNetwork::eventStreamWriteLimit()
{
    QFETCH(int, mode);
    QTcpServer listener;
    QVERIFY(listener.listen(QHostAddress::LocalHost, 0));
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, listener.serverPort());
    QVERIFY(client.waitForConnected(2000));
    QTRY_VERIFY_WITH_TIMEOUT(listener.hasPendingConnections(), 2000);
    QTcpSocket *sock = listener.nextPendingConnection();
    const qint64 limit = mode == 3 ? 32 : 1024;
    QPointer<HttpConnection> conn = new HttpConnection(sock, &listener, limit);
    QSignalSpy closed(conn, &HttpConnection::closed);
    conn->startEventStream();
    if (mode != 3) {
        QVERIFY(conn->isStream());
        const qint64 headers = sock->bytesToWrite();
        QVERIFY(headers > 0 && headers < limit);
        conn->sendEvent("status", "one\ntwo");
        QCOMPARE(sock->bytesToWrite(), headers + QByteArray("event: status\ndata: one\ndata: two\n\n").size());
        if (mode == 2) {
            conn->sendEvent("status", QByteArray(int(limit) + 1, 'x'));
        } else {
            // Без прокрутки event loop очередь не успевает уйти в ядро: достаточно трёх малых записей.
            conn->sendComment(QByteArray(int(limit - sock->bytesToWrite() - 4), 'x'));
            QCOMPARE(sock->bytesToWrite(), limit);
            QVERIFY(conn->isStream());
            if (mode == 0) conn->sendEvent("status", "overflow");
            else conn->sendComment("overflow");
        }
    }
    QVERIFY(conn);
    QVERIFY(!conn->isStream());
    QCOMPARE(sock->state(), QAbstractSocket::UnconnectedState);
    QCOMPARE(sock->bytesToWrite(), qint64(0));
    QCOMPARE(closed.count(), 1);
    // abort() уже закрыл сокет, а поздние рассылки до deleteLater() ничего не пишут.
    conn->sendEvent("status", "late");
    conn->sendComment("late");
    QCOMPARE(sock->bytesToWrite(), qint64(0));
    QCOMPARE(closed.count(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(conn.isNull(), 2000);
}

void TestNetwork::sip()
{
    QUdpSocket phone;
    QVERIFY(phone.bind(QHostAddress(QHostAddress::LocalHost), 0));
    connect(&phone, &QUdpSocket::readyRead, &phone, [&phone]() {
        while (phone.hasPendingDatagrams()) {
            QByteArray d(int(phone.pendingDatagramSize()), '\0');
            QHostAddress from;
            quint16 port = 0;
            phone.readDatagram(d.data(), d.size(), &from, &port);
            QByteArray callId;
            for (const QByteArray &l : d.split('\n'))
                if (l.toLower().startsWith("call-id:")) callId = l.mid(8).trimmed();
            phone.writeDatagram("SIP/2.0 200 OK\r\nCall-ID: " + callId + "\r\nCSeq: 1 OPTIONS\r\nContent-Length: 0\r\n\r\n", from, port);
        }
    });
    SipProber p;
    ProbeRequest req;
    req.kind = ProbeKind::Sip;
    req.host = QStringLiteral("127.0.0.1");
    req.port = phone.localPort();
    req.timeoutMs = 1000;
    ProbeResult r = run(&p, req);
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(r.code, 200);
    QVERIFY(r.detail.startsWith(QStringLiteral("SIP 200")));
    QUdpSocket silent;
    QVERIFY(silent.bind(QHostAddress(QHostAddress::LocalHost), 0));
    req.port = silent.localPort();
    req.timeoutMs = 300;
    QCOMPARE(int(run(&p, req).outcome), int(ProbeOutcome::Timeout));
}

void TestNetwork::sipSourcePeer_data()
{
    QTest::addColumn<bool>("wrongAddress");
    QTest::newRow("wrong-port") << false;
    QTest::newRow("wrong-address") << true;
}

void TestNetwork::sipSourcePeer()
{
    QFETCH(bool, wrongAddress);
    QUdpSocket phone;
    QVERIFY(phone.bind(QHostAddress(QHostAddress::LocalHost), 0));
    QUdpSocket impostor;
    const QHostAddress fakeAddress = wrongAddress ? QHostAddress(QStringLiteral("127.0.0.2"))
                                                  : QHostAddress(QHostAddress::LocalHost);
    QVERIFY(impostor.bind(fakeAddress, wrongAddress ? phone.localPort() : quint16(0)));
    SipProber p;
    ProbeRequest req;
    req.token = 9001;
    req.kind = ProbeKind::Sip;
    req.host = QStringLiteral("127.0.0.1");
    req.port = phone.localPort();
    req.timeoutMs = 5000;
    qRegisterMetaType<ProbeResult>();
    QSignalSpy finished(&p, &ProbeBackend::finished);
    p.start(req);
    QTRY_VERIFY_WITH_TIMEOUT(phone.hasPendingDatagrams(), 2000);
    QByteArray packet(int(phone.pendingDatagramSize()), '\0');
    QHostAddress from;
    quint16 port = 0;
    QCOMPARE(phone.readDatagram(packet.data(), packet.size(), &from, &port), qint64(packet.size()));
    QByteArray callId;
    for (const QByteArray &line : packet.split('\n'))
        if (line.toLower().startsWith("call-id:")) callId = line.mid(8).trimmed();
    QVERIFY(!callId.isEmpty());
    const QByteArray reply = "SIP/2.0 200 OK\r\nCall-ID: " + callId + "\r\nCSeq: 1 OPTIONS\r\nContent-Length: 0\r\n\r\n";
    auto *receiver = p.findChild<QUdpSocket *>();
    QVERIFY(receiver);
    // Этот spy вызывается после обработчика пробы: пакет точно разобран, ждать «тишину» не надо.
    QSignalSpy read(receiver, &QUdpSocket::readyRead);
    QCOMPARE(impostor.writeDatagram(reply, from, port), qint64(reply.size()));
    QTRY_VERIFY_WITH_TIMEOUT(!read.isEmpty(), 2000);
    QVERIFY2(finished.isEmpty(), "SIP-ответ с чужого адреса или порта завершил проверку");
    QCOMPARE(phone.writeDatagram(reply, from, port), qint64(reply.size()));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 2000);
    const ProbeResult r = qvariant_cast<ProbeResult>(finished.first().first());
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(r.token, req.token);
    QCOMPARE(r.code, 200);
}

void TestNetwork::dns()
{
    DnsProber p;
    ProbeRequest req;
    req.kind = ProbeKind::Dns;
    req.host = QStringLiteral("localhost");
    req.timeoutMs = 3000;
    const ProbeResult r = run(&p, req);
    QVERIFY2(r.ok, qPrintable(r.error));
    QVERIFY(!r.detail.isEmpty());
}

void TestNetwork::icmpSocket()
{
    IcmpSocketBackend b;
    QString err;
    if (!b.open(true, &err))
        QSKIP(qPrintable(QStringLiteral("нет ICMP-сокета: %1").arg(err)));
    ProbeRequest req;
    req.kind = ProbeKind::Icmp;
    req.host = QStringLiteral("127.0.0.1");
    req.timeoutMs = 1000;
    req.size = 56;
    req.ttl = 64;
    ProbeResult r = run(&b, req);
    QVERIFY2(r.ok, qPrintable(r.error));
    QVERIFY(r.rttMs >= 0 && r.rttMs < 100);
    QCOMPARE(r.from, QStringLiteral("127.0.0.1"));
    req.df = true;
    req.size = 1400;
    QVERIFY(run(&b, req).ok);
    req.df = false;
    req.size = 56;
    req.host = QStringLiteral("10.255.255.1");
    req.timeoutMs = 300;
    r = run(&b, req);
    QVERIFY(!r.ok);
}

void TestNetwork::icmpSystemPing()
{
    SystemPingBackend b;
    if (!b.available())
        QSKIP("нет ping");
    ProbeRequest req;
    req.kind = ProbeKind::Icmp;
    req.host = QStringLiteral("127.0.0.1");
    req.timeoutMs = 1000;
    req.size = 56;
    req.ttl = 64;
    const ProbeResult r = run(&b, req);
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(r.ttl, 64);
    QVERIFY(r.rttMs >= 0);
}

void TestNetwork::icmpHelper()
{
    const QString path = QStringLiteral(ZR_HELPER_PATH);
    if (!QFileInfo::exists(path))
        QSKIP("помощник не собран");
    IcmpHelperBackend b;
    QSignalSpy ready(&b, &IcmpHelperBackend::ready);
    QSignalSpy failed(&b, &IcmpHelperBackend::failed);
    b.startHelper(path);
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() + failed.count() > 0, 3000);
    if (failed.count())
        QSKIP(qPrintable(QStringLiteral("помощник без прав: %1").arg(failed.first().first().toString())));
    ProbeRequest req;
    req.kind = ProbeKind::Icmp;
    req.host = QStringLiteral("127.0.0.1");
    req.timeoutMs = 1000;
    req.size = 56;
    req.ttl = 64;
    const ProbeResult r = run(&b, req);
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(r.ttl, 64);
    req.host = QStringLiteral("10.255.255.1");
    req.timeoutMs = 300;
    const ProbeResult unavailable = run(&b, req);
    QVERIFY(!unavailable.ok);
    // В изолированном --network none маршрута нет: корректен немедленный Unreachable.
    QVERIFY(unavailable.outcome == ProbeOutcome::Timeout || unavailable.outcome == ProbeOutcome::Unreachable);
    b.stopHelper();
}

void TestNetwork::icmpEngineAuto()
{
    IcmpEngine e;
    e.configure(QStringLiteral("auto"), QStringLiteral(ZR_HELPER_PATH), 8);
    QTRY_VERIFY_WITH_TIMEOUT(e.mode() != IcmpEngine::Mode::Helper || e.modeTitle() == QStringLiteral("помощник"), 3000);
    if (e.mode() == IcmpEngine::Mode::None)
        QSKIP("ICMP недоступен совсем");
    ProbeRequest req;
    req.kind = ProbeKind::Icmp;
    req.host = QStringLiteral("127.0.0.1");
    req.timeoutMs = 1000;
    req.size = 56;
    req.ttl = 64;
    const ProbeResult r = run(&e, req);
    QVERIFY2(r.ok, qPrintable(QStringLiteral("%1: %2").arg(e.modeTitle(), r.error)));
    // принудительно системный ping
    e.configure(QStringLiteral("ping"), QString(), 4);
    if (e.mode() == IcmpEngine::Mode::SystemPing)
        QVERIFY(run(&e, req).ok);
}

void TestNetwork::dispatcherNameError()
{
    ProbeDispatcher d;
    TcpProber tcp;
    d.setBackend(ProbeKind::Tcp, &tcp);
    ProbeRequest req;
    req.kind = ProbeKind::Tcp;
    req.host = QStringLiteral("нет-такого-хоста.invalid");
    req.port = 80;
    req.timeoutMs = 500;
    bool done = false;
    ProbeResult out;
    d.start(req, [&](const ProbeResult &r) { out = r; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 8000);
    QCOMPARE(int(out.outcome), int(ProbeOutcome::NameError));
}

void TestNetwork::api()
{
    QTemporaryDir dir;
    ProbeDispatcher d;
    Monitor mon(&d);
    mon.setSettings(Settings());
    EventLog log(dir.path());
    connect(&mon, &Monitor::statusChanged, &log, &EventLog::append);
    TargetConfig t = parseTargetInput(QStringLiteral("10.0.0.1 # Шлюз")).target;
    t.group = QStringLiteral("Ядро");
    const QString id = mon.addTarget(t);
    mon.injectResult(id, true, 1.5);
    TargetConfig t2 = parseTargetInput(QStringLiteral("10.0.0.2")).target;
    const QString id2 = mon.addTarget(t2);
    for (int i = 0; i < 3; ++i) mon.injectResult(id2, false, -1, QStringLiteral("Таймаут"));

    ApiController api(&mon, &log, nullptr);
    Settings s;
    s.apiListen = QStringLiteral("127.0.0.1:0");
    s.apiToken = ApiController::generateToken();
    QString err;
    QVERIFY2(api.start(s, &err), qPrintable(err));
    const QString base = QStringLiteral("http://127.0.0.1:%1").arg(api.port());
    const QByteArray tok = s.apiToken.toLatin1();

    Http h = request("GET", QUrl(base + QStringLiteral("/healthz")), QByteArray());
    QCOMPARE(h.code, 200);
    QCOMPARE(QJsonDocument::fromJson(h.body).object().value(QStringLiteral("status")).toString(), QStringLiteral("ok"));

    h = request("GET", QUrl(base + QStringLiteral("/api/v1/targets")), QByteArray());
    QCOMPARE(h.code, 401);
    QCOMPARE(QJsonDocument::fromJson(h.body).object().value(QStringLiteral("error")).toString(), QStringLiteral("unauthenticated"));
    h = request("GET", QUrl(base + QStringLiteral("/api/v1/targets")), "неправильный");
    QCOMPARE(h.code, 401);

    h = request("GET", QUrl(base + QStringLiteral("/api/v1/targets")), tok);
    QCOMPARE(h.code, 200);
    const QJsonArray arr = QJsonDocument::fromJson(h.body).array();
    QCOMPARE(arr.size(), 2);
    QCOMPARE(arr[0].toObject().value(QStringLiteral("name")).toString(), QStringLiteral("Шлюз"));
    QCOMPARE(arr[0].toObject().value(QStringLiteral("status")).toString(), QStringLiteral("up"));
    QCOMPARE(arr[1].toObject().value(QStringLiteral("status")).toString(), QStringLiteral("down"));

    h = request("GET", QUrl(base + QStringLiteral("/api/v1/targets?status=down")), tok);
    QCOMPARE(QJsonDocument::fromJson(h.body).array().size(), 1);

    h = request("GET", QUrl(base + QStringLiteral("/api/v1/targets/") + id), tok);
    QCOMPARE(h.code, 200);
    QVERIFY(QJsonDocument::fromJson(h.body).object().value(QStringLiteral("samples")).toArray().size() >= 1);

    // формат «Виктории» Инея: [id, green|yellow|red|gray, rtt_ms|null, возраст_мс]
    h = request("GET", QUrl(base + QStringLiteral("/api/v1/availability")), tok);
    const QJsonObject av = QJsonDocument::fromJson(h.body).object();
    QVERIFY(av.contains(QStringLiteral("revision")));
    const QJsonArray rows = av.value(QStringLiteral("results")).toArray();
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows[0].toArray()[1].toString(), QStringLiteral("green"));
    QCOMPARE(rows[1].toArray()[1].toString(), QStringLiteral("red"));
    QVERIFY(rows[1].toArray()[2].isNull());

    h = request("GET", QUrl(base + QStringLiteral("/metrics")), tok);
    QCOMPARE(h.code, 200);
    QVERIFY(h.body.contains("zarnitsa_up{"));
    QVERIFY(h.body.contains("name=\"Шлюз\""));

    h = request("GET", QUrl(base + QStringLiteral("/api/v1/summary")), tok);
    QCOMPARE(QJsonDocument::fromJson(h.body).object().value(QStringLiteral("down")).toInt(), 1);

    h = request("GET", QUrl(base + QStringLiteral("/api/v1/history")), tok);
    QVERIFY(QJsonDocument::fromJson(h.body).array().size() >= 1);

    h = request("POST", QUrl(base + QStringLiteral("/api/v1/targets")), tok, "{\"address\":\"10.0.0.3\",\"name\":\"Новый\",\"group\":\"API\"}", "text/plain");
    QCOMPARE(h.code, 415);
    h = request("POST", QUrl(base + QStringLiteral("/api/v1/targets")), tok, "{\"address\":\"10.0.0.3\",\"name\":\"Новый\",\"group\":\"API\"}");
    QCOMPARE(h.code, 201);
    const QString newId = QJsonDocument::fromJson(h.body).object().value(QStringLiteral("id")).toString();
    QVERIFY(mon.target(newId));
    QCOMPARE(mon.target(newId)->group, QStringLiteral("API"));
    h = request("POST", QUrl(base + QStringLiteral("/api/v1/targets")), tok, "[{\"line\":\"sip:10.0.0.4\"},{\"line\":\"10.0.0.5:22\"}]");
    QCOMPARE(h.code, 201);
    QCOMPARE(QJsonDocument::fromJson(h.body).array().size(), 2);
    h = request("POST", QUrl(base + QStringLiteral("/api/v1/targets")), tok, "{\"address\":\"не адрес\"}");
    QCOMPARE(h.code, 400);
    h = request("POST", QUrl(base + QStringLiteral("/api/v1/targets")), tok, "{сломано");
    QCOMPARE(h.code, 400);

    h = request("DELETE", QUrl(base + QStringLiteral("/api/v1/targets/") + newId), tok);
    QCOMPARE(h.code, 204);
    QVERIFY(!mon.target(newId));
    h = request("DELETE", QUrl(base + QStringLiteral("/api/v1/targets/") + newId), tok);
    QCOMPARE(h.code, 404);
    h = request("PUT", QUrl(base + QStringLiteral("/api/v1/summary")), tok, "{}");
    QCOMPARE(h.code, 405);
    h = request("GET", QUrl(base + QStringLiteral("/нет")), tok);
    QCOMPARE(h.code, 404);
}

void TestNetwork::apiEvents()
{
    QTemporaryDir dir;
    ProbeDispatcher d;
    Monitor mon(&d);
    mon.setSettings(Settings());
    EventLog log(dir.path());
    const QString id = mon.addTarget(parseTargetInput(QStringLiteral("10.0.0.9")).target);
    ApiController api(&mon, &log, nullptr);
    Settings s;
    s.apiListen = QStringLiteral("127.0.0.1:0");
    s.apiToken = ApiController::generateToken();
    QString err;
    QVERIFY(api.start(s, &err));
    QTcpSocket sock;
    sock.connectToHost(QHostAddress::LocalHost, api.port());
    QVERIFY(sock.waitForConnected(2000));
    sock.write("GET /api/v1/events HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer " + s.apiToken.toLatin1() + "\r\n\r\n");
    QByteArray got;
    QTRY_VERIFY_WITH_TIMEOUT((got += sock.readAll()).contains("event: hello"), 3000);
    QVERIFY(got.startsWith("HTTP/1.1 200 OK"));
    QVERIFY(got.contains("text/event-stream"));
    mon.injectResult(id, true, 2.0);   // нет данных → в норме
    QTRY_VERIFY_WITH_TIMEOUT((got += sock.readAll()).contains("event: status"), 3000);
    QVERIFY(got.contains("\"to\":\"up\""));
}

void TestNetwork::apiRequiresTlsOutside()
{
    QTemporaryDir dir;
    ProbeDispatcher d;
    Monitor mon(&d);
    EventLog log(dir.path());
    ApiController api(&mon, &log, nullptr);
    Settings s;
    s.apiListen = QStringLiteral("0.0.0.0:0");
    s.apiToken = ApiController::generateToken();
    QString err;
    QVERIFY(!api.start(s, &err));
    QVERIFY(err.contains(QStringLiteral("TLS")));
    s.apiListen = QStringLiteral("не-адрес");
    QVERIFY(!api.start(s, &err));
}

QTEST_GUILESS_MAIN(TestNetwork)
#include "test_network.moc"
