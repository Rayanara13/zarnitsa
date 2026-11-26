#include "Rosa.h"
#include "SourceReplyGuard.h"

#include "probes/NetProbers.h"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslSocket>

#include <memory>

namespace zr {

static constexpr int kRequestTimeoutMs = 15000;

static QString hostOf(const QString &hostPort)
{
    const int colon = hostPort.lastIndexOf(QLatin1Char(':'));
    return colon > 0 ? hostPort.left(colon) : hostPort;
}

static int portOf(const QString &hostPort)
{
    const int colon = hostPort.lastIndexOf(QLatin1Char(':'));
    return colon > 0 ? hostPort.mid(colon + 1).toInt() : 0;
}

ImportResult parseRosaSipStatus(const QJsonObject &status, const QString &group, const QString &mode,
                                const QString &sourceId, QString *serverSipAddress)
{
    ImportResult r;
    if (serverSipAddress)
        *serverSipAddress = status.value(QStringLiteral("address")).toString();
    if (!status.value(QStringLiteral("enabled")).toBool()) {
        r.warnings << QStringLiteral("SIP в Росе выключен");
        return r;
    }
    const QJsonArray regs = status.value(QStringLiteral("state")).toObject().value(QStringLiteral("registrations")).toArray();
    for (const QJsonValue &v : regs) {
        const QJsonObject reg = v.toObject();
        const QString number = reg.value(QStringLiteral("number")).toString();
        const QString username = reg.value(QStringLiteral("username")).toString();
        const QString peer = reg.value(QStringLiteral("peer")).toString();   // строго «IPv4:порт»
        const QString ip = hostOf(peer);
        if (!isValidIpv4(ip)) {
            r.warnings << QStringLiteral("SIP %1: непонятный адрес «%2»").arg(number, peer);
            continue;
        }
        TargetConfig t;
        t.id = newTargetId();
        t.name = username.isEmpty() || username == number ? QStringLiteral("SIP %1").arg(number)
                                                          : QStringLiteral("SIP %1 · %2").arg(number, username);
        t.address = ip;
        t.group = group;
        if (mode == QLatin1String("sip")) {
            t.kind = ProbeKind::Sip;
            t.port = portOf(peer) > 0 ? portOf(peer) : 5060;
        } else {
            t.kind = ProbeKind::Icmp;
        }
        t.sourceId = sourceId;
        t.sourceKey = QStringLiteral("sip/") + number;
        t.notes = QStringLiteral("Роса, регистрация SIP %1").arg(number);
        r.targets.append(t);
    }
    return r;
}

ImportResult parseRosaH323Status(const QJsonObject &status, const QString &group, const QString &mode,
                                 const QString &sourceId)
{
    ImportResult r;
    if (!status.value(QStringLiteral("enabled")).toBool()) {
        r.warnings << QStringLiteral("H.323 в Росе выключен");
        return r;
    }
    const QString peer = status.value(QStringLiteral("peer_ip")).toString();
    if (peer.isEmpty())
        return r;
    if (!isValidIpv4(peer)) {
        r.warnings << QStringLiteral("H.323: непонятный адрес «%1»").arg(peer);
        return r;
    }
    TargetConfig t;
    t.id = newTargetId();
    const QString ext = status.value(QStringLiteral("extension")).toString();
    t.name = ext.isEmpty() ? QStringLiteral("H.323-терминал") : QStringLiteral("H.323-терминал · %1").arg(ext);
    t.address = peer;
    t.group = group;
    if (mode == QLatin1String("sip")) {   // «проверять службу»: у H.323 это TCP 1720
        t.kind = ProbeKind::Tcp;
        t.port = 1720;
    } else {
        t.kind = ProbeKind::Icmp;
    }
    t.sourceId = sourceId;
    t.sourceKey = QStringLiteral("h323/") + peer;
    t.notes = QStringLiteral("Роса, терминал H.323");
    r.targets.append(t);
    return r;
}

QVector<TargetConfig> rosaServerTargets(const QUrl &base, const QString &tlsPin, const QString &sipAddress,
                                        const QString &group, const QString &sourceId)
{
    QVector<TargetConfig> out;
    TargetConfig http;
    http.id = newTargetId();
    http.name = QStringLiteral("Роса · сервер");
    QUrl u = base;
    u.setPath(QStringLiteral("/healthz"));
    http.address = u.toString();
    http.kind = ProbeKind::Http;
    http.tlsPin = tlsPin;
    http.group = group;
    http.sourceId = sourceId;
    http.sourceKey = QStringLiteral("server/http");
    out.append(http);

    if (!sipAddress.isEmpty()) {
        QString host = hostOf(sipAddress);
        const int port = portOf(sipAddress);
        const QHostAddress ha(host);
        if (host.isEmpty() || ha.isNull() || ha == QHostAddress::AnyIPv4 || ha.isLoopback())
            host = base.host();
        if (port > 0 && isValidHostName(host)) {
            TargetConfig sip;
            sip.id = newTargetId();
            sip.name = QStringLiteral("Роса · SIP");
            sip.address = host;
            sip.port = port;
            sip.kind = ProbeKind::Sip;
            sip.group = group;
            sip.sourceId = sourceId;
            sip.sourceKey = QStringLiteral("server/sip");
            out.append(sip);
        }
    }
    return out;
}

RosaClient::RosaClient(QObject *parent)
    : QObject(parent)
{
    m_nam.setProxy(QNetworkProxy::NoProxy);
}

QUrl RosaClient::normalizeUrl(const QString &text)
{
    QString t = text.trimmed();
    if (t.isEmpty())
        return QUrl();
    if (!t.contains(QStringLiteral("://")))
        t = QStringLiteral("https://") + t;
    QUrl u(t);
    u.setPath(QString());
    u.setQuery(QString());
    return u;
}

void RosaClient::abort()
{
    if (m_reply) {
        disconnect(m_reply, nullptr, this, nullptr);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
}

void RosaClient::probeCertificate(const QUrl &url)
{
    abort();
    QUrl u = url;
    u.setPath(QStringLiteral("/healthz"));
    QNetworkRequest req(u);
    QSslConfiguration conf = QSslConfiguration::defaultConfiguration();
    conf.setPeerVerifyMode(QSslSocket::VerifyNone);
    req.setSslConfiguration(conf);
    req.setTransferTimeout(kRequestTimeoutMs);
    QNetworkReply *r = m_nam.get(req);
    guardSourceReply(r, kRequestTimeoutMs);
    m_reply = r;
    auto trusted = std::make_shared<bool>(true);
    // проверяем отдельно по системному хранилищу: VerifyNone не даст сообщить об ошибке
    connect(r, &QNetworkReply::encrypted, this, [r, trusted]() {
        const QList<QSslCertificate> chain = r->sslConfiguration().peerCertificateChain();
        const QList<QSslError> errs = QSslCertificate::verify(chain, r->url().host());
        *trusted = errs.isEmpty();
    });
    connect(r, &QNetworkReply::finished, this, [this, r, trusted]() {
        m_reply = nullptr;
        r->deleteLater();
        const QSslCertificate cert = r->sslConfiguration().peerCertificate();
        if (cert.isNull()) {
            emit certificate(QString(), QString(), QDateTime(), false,
                             r->error() == QNetworkReply::NoError ? QStringLiteral("Сервер ответил без TLS")
                                                                  : r->errorString());
            return;
        }
        const QString subject = cert.subjectInfo(QSslCertificate::CommonName).join(QStringLiteral(", "));
        emit certificate(HttpProber::certSha256(cert.toDer()), subject, cert.expiryDate(), *trusted, QString());
    });
}

bool RosaClient::checkPin(QNetworkReply *r)
{
    if (m_pin.isEmpty())
        return true;
    QString want = m_pin;
    want.remove(QLatin1Char(':'));
    const QSslCertificate cert = r->sslConfiguration().peerCertificate();
    return !cert.isNull() && HttpProber::certSha256(cert.toDer()) == want.toLower();
}

QNetworkReply *RosaClient::send(const QByteArray &verb, const QString &path, const QByteArray &body)
{
    QUrl u = m_base;
    u.setPath(path);
    QNetworkRequest req(u);
    req.setRawHeader("User-Agent", "Zarnitsa/" + appVersion().toLatin1());
    req.setRawHeader("Accept", "application/json");
    if (!m_token.isEmpty())
        req.setRawHeader("Authorization", "Bearer " + m_token.toLatin1());
    if (!body.isEmpty())
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setTransferTimeout(kRequestTimeoutMs);
    if (u.scheme() == QLatin1String("https") && !m_pin.isEmpty()) {
        QSslConfiguration conf = QSslConfiguration::defaultConfiguration();
        conf.setPeerVerifyMode(QSslSocket::VerifyNone);
        req.setSslConfiguration(conf);
    }
    QNetworkReply *r = m_nam.sendCustomRequest(req, verb, body);
    guardSourceReply(r, kRequestTimeoutMs);
    if (!m_pin.isEmpty()) {
        connect(r, &QNetworkReply::encrypted, this, [this, r]() {
            if (!checkPin(r)) {
                r->setProperty("pinFailed", true);
                r->abort();
            }
        });
    }
    m_reply = r;
    return r;
}

void RosaClient::fail(const QString &error, bool authFailed)
{
    abort();
    // токен, если уже выдан, отзываем без ожидания ответа
    if (!m_token.isEmpty()) {
        QNetworkReply *r = send("DELETE", QStringLiteral("/api/v1/session"));
        m_reply = nullptr;
        connect(r, &QNetworkReply::finished, r, &QObject::deleteLater);
        m_token.clear();
    }
    ImportResult res;
    res.error = error;
    res.authFailed = authFailed;
    emit finished(res);
}

void RosaClient::fetch(const QUrl &url, const QString &username, const QString &password, const QString &pin,
                       const RosaOptions &opts, const QString &group, const QString &sourceId)
{
    abort();
    m_base = url;
    m_user = username;
    m_pass = password;
    m_pin = pin.trimmed().toLower();
    m_opts = opts;
    m_group = group;
    m_sourceId = sourceId;
    m_token.clear();
    m_sipAddress.clear();
    m_result = ImportResult();
    GroupInfo g;
    g.name = group;
    m_result.groups.append(g);
    m_stage = 0;
    step();
}

static QString rosaError(QNetworkReply *r, int code)
{
    if (r->property("pinFailed").toBool())
        return QStringLiteral("Сертификат Росы не совпал с закреплённым отпечатком");
    if (code == 0)
        return QStringLiteral("Роса не отвечает: %1").arg(sourceReplyError(r));
    const QString err = QJsonDocument::fromJson(r->readAll()).object().value(QStringLiteral("error")).toString();
    if (code == 401) return QStringLiteral("Неверный логин или пароль");
    if (code == 403) return QStringLiteral("Нужна учётная запись администратора Росы");
    if (code == 429) return QStringLiteral("Роса занята другими входами, повторите через пару секунд");
    return QStringLiteral("Роса ответила %1 %2").arg(code).arg(err);
}

void RosaClient::step()
{
    switch (m_stage) {
    case 0: {
        emit progress(QStringLiteral("Вход в Росу…"));
        QJsonObject o;
        o[QStringLiteral("username")] = m_user;
        o[QStringLiteral("password")] = m_pass;
        QNetworkReply *r = send("POST", QStringLiteral("/api/v1/sessions"), QJsonDocument(o).toJson(QJsonDocument::Compact));
        connect(r, &QNetworkReply::finished, this, [this, r]() {
            m_reply = nullptr;
            r->deleteLater();
            const int code = sourceHttpStatus(r);
            if (code != 201) {
                fail(rosaError(r, code), code == 401 || code == 403 || code == 429);
                return;
            }
            m_token = QJsonDocument::fromJson(r->readAll()).object().value(QStringLiteral("token")).toString();
            if (m_token.isEmpty()) {
                fail(QStringLiteral("Роса не выдала токен"));
                return;
            }
            m_stage = 1;
            step();
        });
        return;
    }
    case 1: {
        if (!m_opts.sip && !m_opts.server) { m_stage = 2; step(); return; }
        emit progress(QStringLiteral("Читаю регистрации SIP…"));
        QNetworkReply *r = send("GET", QStringLiteral("/api/v1/admin/sip/status"));
        connect(r, &QNetworkReply::finished, this, [this, r]() {
            m_reply = nullptr;
            r->deleteLater();
            const int code = sourceHttpStatus(r);
            if (code != 200) {
                fail(rosaError(r, code), code == 401 || code == 403 || code == 429);
                return;
            }
            const ImportResult sip = parseRosaSipStatus(QJsonDocument::fromJson(r->readAll()).object(), m_group,
                                                        m_opts.mode, m_sourceId, &m_sipAddress);
            if (m_opts.sip) m_result.targets += sip.targets;
            m_result.warnings += sip.warnings;
            m_stage = 2;
            step();
        });
        return;
    }
    case 2: {
        if (!m_opts.h323) { m_stage = 3; step(); return; }
        emit progress(QStringLiteral("Читаю H.323…"));
        QNetworkReply *r = send("GET", QStringLiteral("/api/v1/admin/h323/status"));
        connect(r, &QNetworkReply::finished, this, [this, r]() {
            m_reply = nullptr;
            r->deleteLater();
            const int code = sourceHttpStatus(r);
            if (code == 200) {
                const ImportResult h = parseRosaH323Status(QJsonDocument::fromJson(r->readAll()).object(), m_group,
                                                           m_opts.mode, m_sourceId);
                m_result.targets += h.targets;
                m_result.warnings += h.warnings;
            } else {
                m_result.warnings << rosaError(r, code);
            }
            m_stage = 3;
            step();
        });
        return;
    }
    default: {
        if (m_opts.server)
            m_result.targets += rosaServerTargets(m_base, m_pin, m_sipAddress, m_group, m_sourceId);
        QNetworkReply *r = send("DELETE", QStringLiteral("/api/v1/session"));
        connect(r, &QNetworkReply::finished, this, [this, r]() {
            m_reply = nullptr;
            r->deleteLater();
            m_token.clear();
            const ImportResult res = m_result;
            emit finished(res);
        });
        return;
    }
    }
}

} // namespace zr
