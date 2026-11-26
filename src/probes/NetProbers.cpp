#include "NetProbers.h"

#include "core/Target.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QHostAddress>
#include <QHostInfo>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QtMath>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace zr {

static QByteArray randomToken(int bytes)
{
    QByteArray b(bytes, '\0');
    for (int i = 0; i < bytes; ++i)
        b[i] = char(QRandomGenerator::global()->bounded(256));
    return b.toHex();
}

// ===========================================================================
// TCP
// ===========================================================================

TcpProber::TcpProber(QObject *parent)
    : ProbeBackend(parent)
{
}

void TcpProber::start(const ProbeRequest &req)
{
    QHostAddress addr;
    if (!addr.setAddress(req.host) || req.port == 0) {
        finishLater(makeResult(req, ProbeOutcome::Error, QStringLiteral("Нужен адрес и порт")));
        return;
    }
    Run run;
    run.req = req;
    run.sock = new QTcpSocket(this);
    run.sock->setProxy(QNetworkProxy::NoProxy);
    run.timer = new QTimer(run.sock);
    run.timer->setSingleShot(true);
    const quint64 token = req.token;
    connect(run.sock, &QTcpSocket::connected, this, [this, token]() { done(token, ProbeOutcome::Ok); });
    connect(run.sock, &QAbstractSocket::errorOccurred, this, [this, token](QAbstractSocket::SocketError e) {
        switch (e) {
        case QAbstractSocket::ConnectionRefusedError:
            done(token, ProbeOutcome::Refused, QStringLiteral("Порт закрыт"));
            break;
        case QAbstractSocket::HostNotFoundError:
            done(token, ProbeOutcome::NameError);
            break;
        case QAbstractSocket::NetworkError:
            done(token, ProbeOutcome::Unreachable, QStringLiteral("Сеть недоступна"));
            break;
        case QAbstractSocket::SocketTimeoutError:
            done(token, ProbeOutcome::Timeout);
            break;
        default: {
            const auto it = m_runs.constFind(token);
            done(token, ProbeOutcome::Error, it != m_runs.constEnd() && it->sock ? it->sock->errorString() : QString());
        }
        }
    });
    connect(run.timer, &QTimer::timeout, this, [this, token]() { done(token, ProbeOutcome::Timeout); });
    run.clock.start();
    run.timer->start(qMax(10, req.timeoutMs));
    QTcpSocket *sock = run.sock;
    m_runs.insert(token, run);
    sock->connectToHost(addr, req.port);
}

void TcpProber::done(quint64 token, ProbeOutcome o, const QString &err)
{
    auto it = m_runs.find(token);
    if (it == m_runs.end())
        return;
    Run run = it.value();
    m_runs.erase(it);
    ProbeResult r = makeResult(run.req, o, err);
    if (o == ProbeOutcome::Ok) {
        r.rttMs = double(run.clock.nsecsElapsed()) / 1e6;
        r.detail = QStringLiteral("порт %1 открыт").arg(run.req.port);
    }
    disconnect(run.sock, nullptr, this, nullptr);
    run.sock->abort();
    run.sock->deleteLater();
    emit finished(r);
}

void TcpProber::cancel(quint64 token)
{
    auto it = m_runs.find(token);
    if (it == m_runs.end())
        return;
    disconnect(it->sock, nullptr, this, nullptr);
    it->sock->abort();
    it->sock->deleteLater();
    m_runs.erase(it);
}

// ===========================================================================
// HTTP(S)
// ===========================================================================

HttpProber::HttpProber(QObject *parent)
    : ProbeBackend(parent)
{
}

QString HttpProber::certSha256(const QByteArray &der)
{
    return QString::fromLatin1(QCryptographicHash::hash(der, QCryptographicHash::Sha256).toHex());
}

static QString normalizedPin(QString pin)
{
    pin.remove(QLatin1Char(':'));
    pin.remove(QLatin1Char(' '));
    return pin.trimmed().toLower();
}

QNetworkAccessManager *HttpProber::namFor(const ProbeRequest &req)
{
    const QString key = !req.tlsPin.trimmed().isEmpty() ? QStringLiteral("pin:") + normalizedPin(req.tlsPin)
        : req.tlsInsecure ? QStringLiteral("insecure") : QStringLiteral("verify");
    auto it = m_nams.find(key);
    if (it != m_nams.end())
        return it.value();
    auto *nam = new QNetworkAccessManager(this);
    nam->setProxy(QNetworkProxy::NoProxy);
    nam->setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
    m_nams.insert(key, nam);
    return nam;
}

void HttpProber::start(const ProbeRequest &req)
{
    const QUrl url(req.host);
    if (!url.isValid() || url.host().isEmpty()) {
        finishLater(makeResult(req, ProbeOutcome::Error, QStringLiteral("Неверный URL")));
        return;
    }
    QNetworkRequest nr(url);
    nr.setRawHeader("User-Agent", "Zarnitsa/" + appVersion().toLatin1());
    nr.setRawHeader("Cache-Control", "no-cache");
    nr.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    nr.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    nr.setTransferTimeout(qMax(50, req.timeoutMs));
    const bool pinMode = !req.tlsPin.trimmed().isEmpty();
    if (url.scheme() == QLatin1String("https")) {
        QSslConfiguration conf = QSslConfiguration::defaultConfiguration();
        if (req.tlsInsecure && !pinMode)
            conf.setPeerVerifyMode(QSslSocket::VerifyNone);
        nr.setSslConfiguration(conf);
    }

    Run run;
    run.req = req;
    run.clock.start();
    QNetworkReply *reply = namFor(req)->get(nr);
    run.reply = reply;
    run.timer = new QTimer(reply);
    run.timer->setSingleShot(true);
    const quint64 token = req.token;
    connect(run.timer, &QTimer::timeout, this, [this, token]() {
        auto it = m_runs.find(token);
        if (it == m_runs.end() || !it->reply) return;
        it->timedOut = true;
        it->reply->abort();
    });
    connect(reply, &QNetworkReply::metaDataChanged, this, [this, token]() {
        auto it = m_runs.find(token);
        if (it != m_runs.end() && it->headersMs < 0)
            it->headersMs = double(it->clock.nsecsElapsed()) / 1e6;
    });
    connect(reply, &QNetworkReply::readyRead, this, [this, token]() {
        auto it = m_runs.find(token);
        if (it == m_runs.end() || !it->reply) return;
        const QByteArray chunk = it->reply->readAll();
        if (it->body.size() < 65536)
            it->body.append(chunk.left(65536 - it->body.size()));
    });
    if (pinMode) {
        const QString want = normalizedPin(req.tlsPin);
        const auto checkPin = [this, token, want]() {
            auto it = m_runs.find(token);
            if (it == m_runs.end() || !it->reply) return false;
            QNetworkReply *reply = it->reply;
            const QSslCertificate cert = reply->sslConfiguration().peerCertificate();
            it->pinSeen = certSha256(cert.toDer());
            if (!cert.isNull() && it->pinSeen == want)
                return true;
            it->pinFailed = true;
            // abort() может синхронно вызвать onFinished() и удалить запись из m_runs.
            reply->abort();
            return false;
        };
        connect(reply, &QNetworkReply::encrypted, this, [checkPin]() { checkPin(); });
        // Закреплённый сертификат важнее insecure: ошибки цепочки игнорируем только после сверки.
        connect(reply, &QNetworkReply::sslErrors, this, [reply, checkPin](const QList<QSslError> &) {
            if (checkPin()) reply->ignoreSslErrors();
        });
    } else if (req.tlsInsecure) {
        connect(reply, &QNetworkReply::sslErrors, reply, [reply](const QList<QSslError> &) { reply->ignoreSslErrors(); });
    }
    connect(reply, &QNetworkReply::finished, this, [this, token]() { onFinished(token); });
    run.timer->start(qMax(50, req.timeoutMs) + 250);
    m_runs.insert(token, run);
}

void HttpProber::onFinished(quint64 token)
{
    auto it = m_runs.find(token);
    if (it == m_runs.end())
        return;
    Run run = it.value();
    m_runs.erase(it);
    QNetworkReply *reply = run.reply;
    if (!reply)
        return;
    run.timer->stop();
    reply->deleteLater();
    if (reply->isOpen() && run.body.size() < 65536)
        run.body.append(reply->readAll().left(65536 - run.body.size()));

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError error = reply->error();
    // Qt представляет HTTP 4xx/5xx как ошибки ответа: ожидаемый 404 всё ещё допустим.
    const bool httpError = status >= 400
        && ((error >= QNetworkReply::ContentAccessDenied && error <= QNetworkReply::UnknownContentError)
            || (error >= QNetworkReply::InternalServerError && error <= QNetworkReply::UnknownServerError));
    const double total = double(run.clock.nsecsElapsed()) / 1e6;
    ProbeResult r;
    if (run.pinFailed) {
        r = makeResult(run.req, ProbeOutcome::Error, QStringLiteral("Сертификат не совпал с закреплённым"));
        r.detail = run.pinSeen;
    } else if (run.timedOut || error == QNetworkReply::TimeoutError || error == QNetworkReply::OperationCanceledError) {
        r = makeResult(run.req, ProbeOutcome::Timeout);
    } else if (status == 0 || (error != QNetworkReply::NoError && !httpError)) {
        // Наличие заголовков HTTP 200 не означает, что тело получено без транспортной ошибки.
        switch (error) {
        case QNetworkReply::ConnectionRefusedError:
            r = makeResult(run.req, ProbeOutcome::Refused, QStringLiteral("Порт закрыт"));
            break;
        case QNetworkReply::HostNotFoundError:
            r = makeResult(run.req, ProbeOutcome::NameError);
            break;
        case QNetworkReply::SslHandshakeFailedError:
            r = makeResult(run.req, ProbeOutcome::Error, QStringLiteral("Ошибка TLS: %1").arg(reply->errorString()));
            break;
        default:
            r = makeResult(run.req, ProbeOutcome::Error, reply->errorString());
            break;
        }
    } else {
        const bool codeOk = run.req.httpExpect > 0 ? status == run.req.httpExpect : (status >= 200 && status < 400);
        if (!codeOk) {
            r = makeResult(run.req, ProbeOutcome::BadResponse, QStringLiteral("HTTP %1").arg(status));
        } else if (!run.req.httpMatch.isEmpty() && !QString::fromUtf8(run.body).contains(run.req.httpMatch)) {
            r = makeResult(run.req, ProbeOutcome::BadResponse, QStringLiteral("Нет текста «%1»").arg(run.req.httpMatch));
        } else {
            r = makeResult(run.req, ProbeOutcome::Ok);
        }
        r.detail = QStringLiteral("HTTP %1").arg(status);
        r.rttMs = run.headersMs >= 0 ? run.headersMs : total;
        const QSslCertificate cert = reply->sslConfiguration().peerCertificate();
        if (!cert.isNull())
            r.certExpiry = cert.expiryDate().toMSecsSinceEpoch();
    }
    r.code = status;
    if (!r.error.isEmpty()) {
        TargetConfig target;
        target.kind = ProbeKind::Http;
        target.address = reply->url().toString();
        const QString publicUrl = target.publicEndpointText();
        // Меняем только URL этой пробы в тексте Qt, не трогая адрес запроса и чужой текст.
        for (const QUrl &url : {reply->url(), reply->url().adjusted(QUrl::RemoveFragment)}) {
            for (const QString &text : {url.toString(QUrl::FullyEncoded), url.toString(), url.toDisplayString()})
                if (!text.isEmpty()) r.error.replace(text, publicUrl);
        }
    }
    emit finished(r);
}

void HttpProber::cancel(quint64 token)
{
    auto it = m_runs.find(token);
    if (it == m_runs.end())
        return;
    QPointer<QNetworkReply> reply = it->reply;
    if (reply) it->timer->stop();
    m_runs.erase(it);
    if (reply) {
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
}

// ===========================================================================
// SIP OPTIONS
// ===========================================================================

SipProber::SipProber(QObject *parent)
    : ProbeBackend(parent)
{
    m_clock.start();
    m_timer.setInterval(50);
    connect(&m_timer, &QTimer::timeout, this, &SipProber::expire);
}

bool SipProber::ensureSocket(QString *err)
{
    if (m_sock && m_sock->state() == QAbstractSocket::BoundState)
        return true;
    delete m_sock;
    m_sock = new QUdpSocket(this);
    if (!m_sock->bind(QHostAddress(QHostAddress::AnyIPv4), 0)) {
        if (err) *err = m_sock->errorString();
        delete m_sock;
        m_sock = nullptr;
        return false;
    }
    connect(m_sock, &QUdpSocket::readyRead, this, &SipProber::onReadyRead);
    m_timer.start();
    return true;
}

QString SipProber::localIpFor(const QString &target, int port)
{
    const qint64 now = m_clock.elapsed();
    const auto it = m_localIps.constFind(target);
    if (it != m_localIps.constEnd() && it->second > now)
        return it->first;
    // connect() у UDP ничего не отправляет, но выбирает маршрут — так узнаём свой адрес к цели
    QString ip = QStringLiteral("0.0.0.0");
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd >= 0) {
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(quint16(port));
        sa.sin_addr.s_addr = htonl(QHostAddress(target).toIPv4Address());
        if (::connect(fd, reinterpret_cast<sockaddr *>(&sa), sizeof sa) == 0) {
            sockaddr_in local{};
            socklen_t len = sizeof local;
            if (::getsockname(fd, reinterpret_cast<sockaddr *>(&local), &len) == 0)
                ip = QHostAddress(ntohl(local.sin_addr.s_addr)).toString();
        }
        ::close(fd);
    }
    m_localIps.insert(target, qMakePair(ip, now + 60000));
    return ip;
}

QByteArray SipProber::buildOptions(const QString &host, int port, const QString &localIp, int localPort,
                                   const QByteArray &callId, const QByteArray &branch, const QByteArray &tag)
{
    const QByteArray h = host.toLatin1();
    const QByteArray p = QByteArray::number(port);
    const QByteArray l = localIp.toLatin1();
    const QByteArray lp = QByteArray::number(localPort);
    QByteArray m;
    m += "OPTIONS sip:" + h + ':' + p + " SIP/2.0\r\n";
    m += "Via: SIP/2.0/UDP " + l + ':' + lp + ";branch=z9hG4bK" + branch + ";rport\r\n";
    m += "Max-Forwards: 70\r\n";
    m += "From: <sip:zarnitsa@" + l + ">;tag=" + tag + "\r\n";
    m += "To: <sip:" + h + ':' + p + ">\r\n";
    m += "Call-ID: " + callId + "\r\n";
    m += "CSeq: 1 OPTIONS\r\n";
    m += "Contact: <sip:zarnitsa@" + l + ':' + lp + ">\r\n";
    m += "Accept: application/sdp\r\n";
    m += "User-Agent: Zarnitsa/" + appVersion().toLatin1() + "\r\n";
    m += "Content-Length: 0\r\n\r\n";
    return m;
}

int SipProber::parseResponse(const QByteArray &d, QByteArray *callId, QString *reason)
{
    if (!d.startsWith("SIP/2.0 "))
        return 0;
    const int eol = d.indexOf("\r\n");
    const QByteArray first = d.left(eol < 0 ? d.size() : eol);
    const int code = first.mid(8, 3).toInt();
    if (code < 100 || code > 699)
        return 0;
    if (reason)
        *reason = QString::fromUtf8(first.mid(12)).trimmed();
    if (callId) {
        callId->clear();
        const QList<QByteArray> lines = d.split('\n');
        for (const QByteArray &raw : lines) {
            const QByteArray line = raw.trimmed();
            const int colon = line.indexOf(':');
            if (colon <= 0) continue;
            const QByteArray name = line.left(colon).trimmed().toLower();
            if (name == "call-id" || name == "i") {
                *callId = line.mid(colon + 1).trimmed();
                break;
            }
        }
    }
    return code;
}

void SipProber::start(const ProbeRequest &req)
{
    QHostAddress addr;
    if (!addr.setAddress(req.host) || addr.protocol() != QAbstractSocket::IPv4Protocol) {
        finishLater(makeResult(req, ProbeOutcome::Error, QStringLiteral("Нужен IPv4-адрес")));
        return;
    }
    QString err;
    if (!ensureSocket(&err)) {
        finishLater(makeResult(req, ProbeOutcome::Error, QStringLiteral("UDP-сокет: %1").arg(err)));
        return;
    }
    const int port = req.port ? req.port : 5060;
    const QString lip = localIpFor(req.host, port);
    const QByteArray callId = randomToken(10) + '@' + lip.toLatin1();
    const QByteArray msg = buildOptions(req.host, port, lip, m_sock->localPort(), callId, randomToken(8), randomToken(5));
    Pend p;
    p.req = req;
    p.peer = addr;
    p.port = quint16(port);
    p.clock.start();
    p.deadline = m_clock.elapsed() + qMax(10, req.timeoutMs);
    if (m_sock->writeDatagram(msg, p.peer, p.port) < 0) {
        finishLater(makeResult(req, ProbeOutcome::Unreachable, m_sock->errorString()));
        return;
    }
    m_pending.insert(callId, p);
    m_byToken.insert(req.token, callId);
}

void SipProber::onReadyRead()
{
    while (m_sock && m_sock->hasPendingDatagrams()) {
        QByteArray d;
        d.resize(int(qMax<qint64>(0, m_sock->pendingDatagramSize())));
        QHostAddress peer;
        quint16 port = 0;
        if (m_sock->readDatagram(d.data(), d.size(), &peer, &port) < 0)
            break;
        QByteArray callId;
        QString reason;
        const int code = parseResponse(d, &callId, &reason);
        if (!code)
            continue;
        auto it = m_pending.find(callId);
        if (it == m_pending.end() || it->peer != peer || it->port != port)
            continue;
        ProbeResult r = makeResult(it->req, ProbeOutcome::Ok);
        r.rttMs = double(it->clock.nsecsElapsed()) / 1e6;
        r.code = code;
        r.detail = QStringLiteral("SIP %1 %2").arg(code).arg(reason);
        m_byToken.remove(it->req.token);
        m_pending.erase(it);
        emit finished(r);
    }
}

void SipProber::expire()
{
    if (m_pending.isEmpty())
        return;
    const qint64 now = m_clock.elapsed();
    QVector<QByteArray> due;
    for (auto it = m_pending.constBegin(); it != m_pending.constEnd(); ++it)
        if (it->deadline <= now)
            due.append(it.key());
    for (const QByteArray &id : due) {
        auto it = m_pending.find(id);
        if (it == m_pending.end()) continue;
        ProbeResult r = makeResult(it->req, ProbeOutcome::Timeout, QStringLiteral("SIP не отвечает"));
        m_byToken.remove(it->req.token);
        m_pending.erase(it);
        emit finished(r);
    }
}

void SipProber::cancel(quint64 token)
{
    const auto it = m_byToken.find(token);
    if (it == m_byToken.end())
        return;
    m_pending.remove(it.value());
    m_byToken.erase(it);
}

// ===========================================================================
// DNS
// ===========================================================================

DnsProber::DnsProber(QObject *parent)
    : ProbeBackend(parent)
{
}

void DnsProber::start(const ProbeRequest &req)
{
    Run run;
    run.req = req;
    run.clock.start();
    run.timer = new QTimer(this);
    run.timer->setSingleShot(true);
    const quint64 token = req.token;
    connect(run.timer, &QTimer::timeout, this, [this, token]() {
        auto it = m_runs.find(token);
        if (it == m_runs.end()) return;
        Run r = it.value();
        m_runs.erase(it);
        QHostInfo::abortHostLookup(r.lookupId);
        r.timer->deleteLater();
        emit finished(makeResult(r.req, ProbeOutcome::Timeout, QStringLiteral("DNS не ответил")));
    });
    run.timer->start(qMax(50, req.timeoutMs));
    m_runs.insert(token, run);
    const int id = QHostInfo::lookupHost(req.host, this, [this, token](const QHostInfo &info) {
        auto it = m_runs.find(token);
        if (it == m_runs.end()) return;
        Run r = it.value();
        m_runs.erase(it);
        r.timer->stop();
        r.timer->deleteLater();
        ProbeResult res;
        if (info.error() != QHostInfo::NoError || info.addresses().isEmpty()) {
            res = makeResult(r.req, ProbeOutcome::NameError);
        } else {
            res = makeResult(r.req, ProbeOutcome::Ok);
            QStringList list;
            for (const QHostAddress &a : info.addresses()) list << a.toString();
            res.detail = list.join(QStringLiteral(", "));
            res.resolved = list.first();
        }
        res.rttMs = double(r.clock.nsecsElapsed()) / 1e6;
        emit finished(res);
    });
    auto it = m_runs.find(token);
    if (it != m_runs.end())
        it->lookupId = id;
}

void DnsProber::cancel(quint64 token)
{
    auto it = m_runs.find(token);
    if (it == m_runs.end())
        return;
    QHostInfo::abortHostLookup(it->lookupId);
    it->timer->deleteLater();
    m_runs.erase(it);
}

// ===========================================================================
// Демо
// ===========================================================================

DemoProber::DemoProber(QObject *parent)
    : ProbeBackend(parent)
{
    m_clock.start();
}

DemoProber::Sim DemoProber::simulate(const QString &scenario, double t, double noise, double noise2)
{
    // сценарий: «вид:параметр» — ok:0.4, slow:190, lossy:18, down, flap:25, recover:30, http:210, sip, jitter:40
    const QString sc = scenario.isEmpty() ? QStringLiteral("ok:1") : scenario;
    const QString kind = sc.section(QLatin1Char(':'), 0, 0);
    const double param = sc.section(QLatin1Char(':'), 1).toDouble();
    Sim s;
    s.rtt = param > 0 ? param : 1.0;
    if (kind == QLatin1String("ok")) {
        s.rtt = s.rtt * (0.85 + 0.3 * noise) + (noise2 > 0.97 ? s.rtt * 2.5 : 0.0);
    } else if (kind == QLatin1String("slow")) {
        s.rtt = s.rtt * (0.9 + 0.25 * noise) + 20.0 * qSin(t / 3.0);
    } else if (kind == QLatin1String("lossy")) {
        s.ok = noise * 100.0 >= param;
        s.rtt = 18.0 + 9.0 * noise2;
    } else if (kind == QLatin1String("down")) {
        s.ok = false;
    } else if (kind == QLatin1String("flap")) {
        const double period = param > 0 ? param : 25.0;
        s.ok = std::fmod(t + 100000.0, period * 2.0) < period;
        s.rtt = 3.0 + 2.0 * noise;
    } else if (kind == QLatin1String("recover")) {
        s.ok = t > (param > 0 ? param : 30.0);
        s.rtt = 2.0 + noise;
    } else if (kind == QLatin1String("http")) {
        s.rtt = 11.0 + 6.0 * noise;
        s.code = 200;
        s.detail = QStringLiteral("HTTP 200");
        s.certExpiry = QDateTime::currentDateTimeUtc().addDays(int(param > 0 ? param : 210)).toMSecsSinceEpoch();
    } else if (kind == QLatin1String("sip")) {
        s.rtt = 2.0 + 1.5 * noise;
        s.code = 200;
        s.detail = QStringLiteral("SIP 200 OK");
    } else if (kind == QLatin1String("jitter")) {
        s.rtt = param * (0.3 + 1.6 * noise);
    }
    return s;
}

void DemoProber::start(const ProbeRequest &req)
{
    auto *rng = QRandomGenerator::global();
    const double t = m_clock.elapsed() / 1000.0;
    const Sim s = simulate(req.demoScenario, t, rng->generateDouble(), rng->generateDouble());
    ProbeResult r = makeResult(req, s.ok ? ProbeOutcome::Ok : ProbeOutcome::Timeout);
    if (s.ok) {
        r.rttMs = s.rtt;
        r.ttl = 63;
        r.code = s.code;
        r.detail = s.detail;
        r.certExpiry = s.certExpiry;
    }
    const int delay = s.ok ? qMax(1, int(s.rtt)) : qMax(10, req.timeoutMs);
    QTimer::singleShot(qMin(delay, 2000), this, [this, r]() { emit finished(r); });
}

} // namespace zr
