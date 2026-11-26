#include "ApiController.h"

#include "core/EventLog.h"
#include "probes/IcmpEngine.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>

namespace zr {

ApiController::ApiController(Monitor *monitor, EventLog *log, IcmpEngine *icmp, QObject *parent)
    : QObject(parent)
    , m_monitor(monitor)
    , m_log(log)
    , m_icmp(icmp)
{
    connect(m_monitor, &Monitor::statusChanged, this, &ApiController::onStatus);
}

QString ApiController::generateToken()
{
    QByteArray b(32, '\0');
    for (int i = 0; i < b.size(); ++i)
        b[i] = char(QRandomGenerator::system()->bounded(256));
    return QString::fromLatin1(b.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

bool ApiController::parseListen(const QString &text, QHostAddress *addr, quint16 *port)
{
    QString t = text.trimmed();
    if (t.isEmpty()) return false;
    QString host;
    QString portText;
    if (t.startsWith(QLatin1Char('['))) {
        const int close = t.indexOf(QLatin1Char(']'));
        if (close < 0) return false;
        host = t.mid(1, close - 1);
        portText = t.mid(close + 2);
    } else if (t.count(QLatin1Char(':')) == 1) {
        host = t.section(QLatin1Char(':'), 0, 0);
        portText = t.section(QLatin1Char(':'), 1);
    } else {
        portText = t;
    }
    bool ok = false;
    const int p = portText.toInt(&ok);
    if (!ok || p < 0 || p > 65535) return false;
    QHostAddress a;
    if (host.isEmpty() || host == QLatin1String("*")) a = QHostAddress(QHostAddress::Any);
    else if (host == QLatin1String("localhost")) a = QHostAddress(QHostAddress::LocalHost);
    else if (!a.setAddress(host)) return false;
    *addr = a;
    *port = quint16(p);
    return true;
}

bool ApiController::start(const Settings &s, QString *err)
{
    stop();
    QHostAddress addr;
    quint16 port = 0;
    if (!parseListen(s.apiListen, &addr, &port)) {
        if (err) *err = QStringLiteral("Неверный адрес API: %1").arg(s.apiListen);
        return false;
    }
    m_token = s.apiToken.toLatin1();
    if (m_token.size() < 16) {
        if (err) *err = QStringLiteral("Нет токена API");
        return false;
    }
    m_server = new HttpServer(this);
    const bool loopback = addr.isLoopback();
    if (!s.apiTlsCert.isEmpty() || !s.apiTlsKey.isEmpty()) {
        QFile cf(s.apiTlsCert), kf(s.apiTlsKey);
        if (!cf.open(QIODevice::ReadOnly) || !kf.open(QIODevice::ReadOnly)) {
            if (err) *err = QStringLiteral("Не читается сертификат или ключ API");
            stop();
            return false;
        }
        const QSslCertificate cert(cf.readAll(), QSsl::Pem);
        const QByteArray keyPem = kf.readAll();
        QSslKey key(keyPem, QSsl::Ec, QSsl::Pem);
        if (key.isNull()) key = QSslKey(keyPem, QSsl::Rsa, QSsl::Pem);
        if (cert.isNull() || key.isNull()) {
            if (err) *err = QStringLiteral("Сертификат или ключ API не в формате PEM");
            stop();
            return false;
        }
        m_server->setTls(cert, key);
    } else if (!loopback) {
        // то же правило, что у Росы: наружу — только по TLS
        if (err) *err = QStringLiteral("Для адреса вне 127.0.0.1 нужен TLS: укажите сертификат и ключ (make-cert.sh)");
        stop();
        return false;
    }
    m_server->setHandler([this](const HttpRequest &r, HttpConnection *c) { handle(r, c); });
    if (!m_server->listen(addr, port)) {
        if (err) *err = QStringLiteral("API не слушает %1: %2").arg(s.apiListen, m_server->errorString());
        stop();
        return false;
    }
    m_address = QStringLiteral("%1://%2:%3")
                    .arg(m_server->tls() ? QStringLiteral("https") : QStringLiteral("http"),
                         addr.protocol() == QAbstractSocket::IPv6Protocol ? QStringLiteral("[%1]").arg(addr.toString())
                                                                         : addr.toString())
                    .arg(m_server->serverPort());
    return true;
}

void ApiController::stop()
{
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
    m_address.clear();
}

quint16 ApiController::port() const
{
    return m_server ? m_server->serverPort() : 0;
}

bool ApiController::authorized(const HttpRequest &req) const
{
    const QByteArray h = req.header("authorization");
    const int sp = h.indexOf(' ');
    if (sp <= 0 || h.left(sp).toLower() != "bearer")
        return false;
    const QByteArray got = h.mid(sp + 1).trimmed();
    // сравнение за постоянное время
    const QByteArray a = QCryptographicHash::hash(got, QCryptographicHash::Sha256);
    const QByteArray b = QCryptographicHash::hash(m_token, QCryptographicHash::Sha256);
    unsigned char diff = 0;
    for (int i = 0; i < a.size(); ++i) diff |= uchar(a[i]) ^ uchar(b[i]);
    return diff == 0 && !m_token.isEmpty();
}

void ApiController::json(HttpConnection *c, int status, const QJsonValue &v)
{
    HttpResponse r;
    r.status = status;
    if (v.isObject()) r.body = QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact);
    else if (v.isArray()) r.body = QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact);
    c->respond(r);
}

void ApiController::error(HttpConnection *c, int status, const char *code)
{
    QJsonObject o;
    o[QStringLiteral("error")] = QLatin1String(code);
    json(c, status, o);
}

static QJsonValue num(double v, int prec = 2)
{
    if (v < 0) return QJsonValue(QJsonValue::Null);
    const double f = std::pow(10.0, prec);
    return std::round(v * f) / f;
}

QJsonObject ApiController::targetJson(const TargetConfig &t, bool detail) const
{
    const TargetRuntime *rt = m_monitor->runtime(t.id);
    QJsonObject o;
    o[QStringLiteral("id")] = t.id;
    o[QStringLiteral("name")] = t.displayName();
    o[QStringLiteral("address")] = t.kind == ProbeKind::Http ? t.publicEndpointText() : t.address;
    o[QStringLiteral("endpoint")] = t.publicEndpointText();
    o[QStringLiteral("kind")] = kindToKey(t.kind);
    if (t.port) o[QStringLiteral("port")] = t.port;
    o[QStringLiteral("group")] = t.group;
    o[QStringLiteral("enabled")] = t.enabled;
    if (!rt) return o;
    const WindowStats w = rt->stats.window(m_monitor->settings().statsWindow);
    o[QStringLiteral("status")] = statusKey(rt->status);
    o[QStringLiteral("status_title")] = statusTitle(rt->status);
    o[QStringLiteral("since")] = QDateTime::fromMSecsSinceEpoch(rt->since).toUTC().toString(Qt::ISODateWithMs);
    o[QStringLiteral("rtt_ms")] = num(w.last);
    o[QStringLiteral("avg_ms")] = num(w.avg);
    o[QStringLiteral("loss_pct")] = num(w.sent ? w.lossPct : -1.0, 1);
    o[QStringLiteral("jitter_ms")] = num(w.jitter);
    const double m = m_monitor->mos(t.id);
    o[QStringLiteral("mos")] = num(m, 2);
    o[QStringLiteral("uptime_24h")] = num(rt->stats.uptimePct(m_monitor->nowMs()), 2);
    if (!rt->reason.isEmpty()) o[QStringLiteral("reason")] = rt->reason;
    if (!rt->lastError.isEmpty()) o[QStringLiteral("last_error")] = rt->lastError;
    if (!rt->lastDetail.isEmpty()) o[QStringLiteral("detail")] = rt->lastDetail;
    if (rt->flapping) o[QStringLiteral("flapping")] = true;
    if (rt->dependent) o[QStringLiteral("dependent")] = true;
    if (!t.parentId.isEmpty()) o[QStringLiteral("parent")] = t.parentId;
    if (!t.sourceId.isEmpty()) {
        o[QStringLiteral("source")] = t.sourceId;
        if (t.sourceMissing) o[QStringLiteral("source_missing")] = true;
    }
    if (rt->certExpiry) o[QStringLiteral("cert_expires")] = QDateTime::fromMSecsSinceEpoch(rt->certExpiry).toUTC().toString(Qt::ISODate);
    if (rt->lastProbe) o[QStringLiteral("checked_at")] = QDateTime::fromMSecsSinceEpoch(rt->lastProbe).toUTC().toString(Qt::ISODateWithMs);
    if (detail) {
        QJsonArray samples;
        for (const Sample &s : rt->stats.lastSamples(120)) {
            QJsonArray a;
            a.append(double(s.t));
            a.append(s.ok ? QJsonValue(qRound(s.rtt * 100.0) / 100.0) : QJsonValue(QJsonValue::Null));
            samples.append(a);
        }
        o[QStringLiteral("samples")] = samples;
        QJsonArray events;
        for (const StatusEvent &e : m_log->forTarget(t.id, 50))
            events.append(e.toJson());
        o[QStringLiteral("events")] = events;
        o[QStringLiteral("min_ms")] = num(w.min);
        o[QStringLiteral("max_ms")] = num(w.max);
        o[QStringLiteral("sent")] = double(rt->stats.totalSent());
        o[QStringLiteral("received")] = double(rt->stats.totalRecv());
    }
    return o;
}

QByteArray ApiController::availabilityJson() const
{
    // словарь и форма отчёта «Виктории»: [id, green|yellow|red|gray, rtt_ms|null, возраст_мс]
    const qint64 now = m_monitor->nowMs();
    QJsonArray results;
    for (const TargetConfig &t : m_monitor->targets()) {
        const TargetRuntime *rt = m_monitor->runtime(t.id);
        QJsonArray row;
        row.append(t.id);
        row.append(statusIneiColor(rt ? rt->status : Status::Unknown));
        const WindowStats w = rt ? rt->stats.window(m_monitor->settings().statsWindow) : WindowStats();
        row.append(w.last >= 0 ? QJsonValue(qRound(w.last)) : QJsonValue(QJsonValue::Null));
        row.append(rt && rt->lastProbe ? QJsonValue(double(now - rt->lastProbe)) : QJsonValue(QJsonValue::Null));
        results.append(row);
    }
    QJsonObject o;
    o[QStringLiteral("revision")] = double(m_revision);
    o[QStringLiteral("results")] = results;
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

static QByteArray promLabel(const QString &v)
{
    QByteArray b = v.toUtf8();
    b.replace('\\', "\\\\");
    b.replace('"', "\\\"");
    b.replace('\n', "\\n");
    return b;
}

QByteArray ApiController::metricsText() const
{
    QByteArray up, status, rtt, loss, jitter, mos, uptime;
    for (const TargetConfig &t : m_monitor->targets()) {
        const TargetRuntime *rt = m_monitor->runtime(t.id);
        if (!rt) continue;
        const QByteArray labels = "{id=\"" + promLabel(t.id) + "\",name=\"" + promLabel(t.displayName()) + "\",address=\""
            + promLabel(t.publicEndpointText()) + "\",group=\"" + promLabel(t.group) + "\",kind=\"" + kindToKey(t.kind).toLatin1() + "\"}";
        const WindowStats w = rt->stats.window(m_monitor->settings().statsWindow);
        up += "zarnitsa_up" + labels + ' ' + (rt->status == Status::Up || rt->status == Status::Degraded ? "1" : "0") + '\n';
        status += "zarnitsa_status" + labels + ' ' + QByteArray::number(int(rt->status)) + '\n';
        if (w.last >= 0) rtt += "zarnitsa_rtt_ms" + labels + ' ' + QByteArray::number(w.last, 'f', 3) + '\n';
        if (w.sent) loss += "zarnitsa_loss_ratio" + labels + ' ' + QByteArray::number(w.lossPct / 100.0, 'f', 4) + '\n';
        if (w.jitter >= 0) jitter += "zarnitsa_jitter_ms" + labels + ' ' + QByteArray::number(w.jitter, 'f', 3) + '\n';
        const double m = m_monitor->mos(t.id);
        if (m > 0) mos += "zarnitsa_mos" + labels + ' ' + QByteArray::number(m, 'f', 2) + '\n';
        const double u = rt->stats.uptimePct(m_monitor->nowMs());
        if (u >= 0) uptime += "zarnitsa_uptime_ratio_24h" + labels + ' ' + QByteArray::number(u / 100.0, 'f', 4) + '\n';
    }
    QByteArray out;
    out += "# HELP zarnitsa_up 1, если цель отвечает (в норме или плохо)\n# TYPE zarnitsa_up gauge\n" + up;
    out += "# HELP zarnitsa_status 0 пауза, 1 норма, 2 нет данных, 3 плохо, 4 нет связи\n# TYPE zarnitsa_status gauge\n" + status;
    out += "# HELP zarnitsa_rtt_ms последняя задержка\n# TYPE zarnitsa_rtt_ms gauge\n" + rtt;
    out += "# HELP zarnitsa_loss_ratio потери в окне\n# TYPE zarnitsa_loss_ratio gauge\n" + loss;
    out += "# HELP zarnitsa_jitter_ms джиттер в окне\n# TYPE zarnitsa_jitter_ms gauge\n" + jitter;
    out += "# HELP zarnitsa_mos приближённая оценка по сетевым пробам 1..4.5, не замер RTP\n# TYPE zarnitsa_mos gauge\n" + mos;
    out += "# HELP zarnitsa_uptime_ratio_24h доступность за сутки\n# TYPE zarnitsa_uptime_ratio_24h gauge\n" + uptime;
    return out;
}

void ApiController::onStatus(const StatusEvent &ev)
{
    ++m_revision;
    if (m_server && m_server->streamCount() > 0)
        m_server->broadcast("status", QJsonDocument(ev.toJson()).toJson(QJsonDocument::Compact));
}

void ApiController::handle(const HttpRequest &req, HttpConnection *c)
{
    const QString path = req.path.endsWith(QLatin1Char('/')) && req.path.size() > 1 ? req.path.chopped(1) : req.path;
    const bool get = req.method == "GET" || req.method == "HEAD";

    if (path == QLatin1String("/healthz")) {
        if (!get) return error(c, 405, "method_not_allowed");
        QJsonObject o;
        o[QStringLiteral("status")] = QStringLiteral("ok");
        o[QStringLiteral("app")] = QStringLiteral("zarnitsa");
        o[QStringLiteral("version")] = appVersion();
        return json(c, 200, o);
    }
    if (!path.startsWith(QLatin1String("/api/v1")) && path != QLatin1String("/metrics"))
        return error(c, 404, "not_found");
    if (!authorized(req))
        return error(c, 401, "unauthenticated");

    if (path == QLatin1String("/metrics")) {
        if (!get) return error(c, 405, "method_not_allowed");
        HttpResponse r;
        r.contentType = "text/plain; version=0.0.4; charset=utf-8";
        r.body = metricsText();
        return c->respond(r);
    }
    if (path == QLatin1String("/api/v1/summary")) {
        if (!get) return error(c, 405, "method_not_allowed");
        const Summary s = m_monitor->summary();
        QJsonObject o;
        o[QStringLiteral("total")] = s.total;
        o[QStringLiteral("up")] = s.up;
        o[QStringLiteral("degraded")] = s.degraded;
        o[QStringLiteral("down")] = s.down;
        o[QStringLiteral("unknown")] = s.unknown;
        o[QStringLiteral("paused")] = s.paused;
        o[QStringLiteral("worst")] = statusKey(s.worst());
        o[QStringLiteral("icmp")] = m_icmp ? m_icmp->modeTitle() : QString();
        return json(c, 200, o);
    }
    if (path == QLatin1String("/api/v1/availability")) {
        if (!get) return error(c, 405, "method_not_allowed");
        HttpResponse r;
        r.body = availabilityJson();
        return c->respond(r);
    }
    if (path == QLatin1String("/api/v1/history")) {
        if (!get) return error(c, 405, "method_not_allowed");
        const qint64 since = req.query.queryItemValue(QStringLiteral("since")).toLongLong();
        const int limit = qBound(1, req.query.queryItemValue(QStringLiteral("limit")).toInt() ?: 200, 5000);
        const QString id = req.query.queryItemValue(QStringLiteral("id"));
        const QVector<StatusEvent> evs = id.isEmpty() ? m_log->since(since, limit) : m_log->forTarget(id, limit);
        QJsonArray a;
        for (const StatusEvent &e : evs)
            if (e.t > since) a.append(e.toJson());
        return json(c, 200, a);
    }
    if (path == QLatin1String("/api/v1/events")) {
        if (!get) return error(c, 405, "method_not_allowed");
        c->startEventStream();
        const Summary s = m_monitor->summary();
        QJsonObject o;
        o[QStringLiteral("total")] = s.total;
        o[QStringLiteral("down")] = s.down;
        o[QStringLiteral("degraded")] = s.degraded;
        c->sendEvent("hello", QJsonDocument(o).toJson(QJsonDocument::Compact));
        return;
    }
    if (path == QLatin1String("/api/v1/targets")) {
        if (get) {
            const QString group = req.query.queryItemValue(QStringLiteral("group"));
            const QString status = req.query.queryItemValue(QStringLiteral("status"));
            QJsonArray a;
            for (const TargetConfig &t : m_monitor->targets()) {
                if (req.query.hasQueryItem(QStringLiteral("group")) && t.group != group) continue;
                if (!status.isEmpty()) {
                    const TargetRuntime *rt = m_monitor->runtime(t.id);
                    if (!rt || statusKey(rt->status) != status) continue;
                }
                a.append(targetJson(t, false));
            }
            return json(c, 200, a);
        }
        if (req.method == "POST") {
            if (!req.header("content-type").toLower().startsWith("application/json"))
                return error(c, 415, "expected_json");
            QJsonParseError pe{};
            const QJsonDocument d = QJsonDocument::fromJson(req.body, &pe);
            if (pe.error != QJsonParseError::NoError)
                return error(c, 400, "invalid_json");
            QJsonArray items = d.isArray() ? d.array() : QJsonArray{d.object()};
            if (items.size() > 5000)
                return error(c, 413, "request_too_large");
            QVector<TargetConfig> add;
            for (const QJsonValue &v : items) {
                const QJsonObject o = v.toObject();
                QString line = o.value(QStringLiteral("line")).toString();
                if (line.isEmpty()) {
                    const QString addr = o.value(QStringLiteral("address")).toString();
                    const QString kind = o.value(QStringLiteral("kind")).toString(QStringLiteral("icmp"));
                    const int port = o.value(QStringLiteral("port")).toInt();
                    if (addr.isEmpty()) return error(c, 400, "invalid_request");
                    if (kind == QLatin1String("http") || addr.contains(QStringLiteral("://"))) line = addr;
                    else if (kind == QLatin1String("icmp")) line = addr;
                    else line = kind + QLatin1Char(':') + addr + (port ? QStringLiteral(":%1").arg(port) : QString());
                }
                ParsedInput p = parseTargetInput(line);
                if (!p.ok) return error(c, 400, "invalid_request");
                if (o.contains(QStringLiteral("name"))) p.target.name = o.value(QStringLiteral("name")).toString();
                p.target.group = o.value(QStringLiteral("group")).toString();
                p.target.tlsPin = o.value(QStringLiteral("tls_pin")).toString().remove(QLatin1Char(':')).toLower();
                p.target.tlsInsecure = o.value(QStringLiteral("tls_insecure")).toBool();
                p.target.parentId = o.value(QStringLiteral("parent")).toString();
                add.append(p.target);
            }
            const QStringList ids = m_monitor->addTargets(add);
            emit targetsAddedByApi(ids.size());
            QJsonArray out;
            for (const QString &id : ids)
                if (const TargetConfig *t = m_monitor->target(id)) out.append(targetJson(*t, false));
            return json(c, 201, d.isArray() ? QJsonValue(out) : out.first());
        }
        return error(c, 405, "method_not_allowed");
    }
    if (path.startsWith(QLatin1String("/api/v1/targets/"))) {
        const QString id = path.mid(16);
        const TargetConfig *t = m_monitor->target(id);
        if (!t) return error(c, 404, "not_found");
        if (get) return json(c, 200, targetJson(*t, true));
        if (req.method == "DELETE") {
            m_monitor->removeTargets({id});
            HttpResponse r;
            r.status = 204;
            return c->respond(r);
        }
        return error(c, 405, "method_not_allowed");
    }
    error(c, 404, "not_found");
}

} // namespace zr
