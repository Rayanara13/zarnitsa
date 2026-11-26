#include "Target.h"

#include <QHostAddress>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

namespace zr {

QString newTargetId()
{
    const quint64 r = QRandomGenerator::global()->generate64();
    return QStringLiteral("t") + QString::number(r, 36).rightJustified(13, QLatin1Char('0')).left(12);
}

bool isValidIpv4(const QString &s)
{
    static const QRegularExpression re(
        QStringLiteral("^(25[0-5]|2[0-4]\\d|1\\d\\d|[1-9]?\\d)(\\.(25[0-5]|2[0-4]\\d|1\\d\\d|[1-9]?\\d)){3}$"));
    return re.match(s).hasMatch();
}

bool isValidHostName(const QString &s)
{
    if (s.isEmpty() || s.size() > 253)
        return false;
    QHostAddress a;
    if (a.setAddress(s))
        return true;
    static const QRegularExpression re(
        QStringLiteral("^(?=.{1,253}$)([A-Za-z0-9_]([A-Za-z0-9_-]{0,61}[A-Za-z0-9_])?)(\\.[A-Za-z0-9_]([A-Za-z0-9_-]{0,61}[A-Za-z0-9_])?)*\\.?$"));
    return re.match(s).hasMatch();
}

QString TargetConfig::displayName() const
{
    return name.trimmed().isEmpty() ? publicEndpointText() : name.trimmed();
}

QString TargetConfig::hostPart() const
{
    if (kind == ProbeKind::Http) {
        const QUrl u(address);
        return u.host();
    }
    return address;
}

QString TargetConfig::endpointText() const
{
    switch (kind) {
    case ProbeKind::Tcp:
        return port > 0 ? QStringLiteral("%1:%2").arg(address).arg(port) : address;
    case ProbeKind::Sip:
        return port > 0 && port != 5060 ? QStringLiteral("sip:%1:%2").arg(address).arg(port)
                                        : QStringLiteral("sip:%1").arg(address);
    case ProbeKind::Dns:
        return QStringLiteral("dns:%1").arg(address);
    default:
        return address;
    }
}

QString TargetConfig::publicEndpointText() const
{
    if (kind != ProbeKind::Http) return endpointText();
    QUrl url(address);
    if (!url.isValid() || url.host().isEmpty()) return QStringLiteral("<неверный URL>");
    url.setUserInfo(QString());
    url.setFragment(QString());
    QUrlQuery query;
    for (const auto &item : QUrlQuery(url).queryItems())
        query.addQueryItem(item.first, QStringLiteral("…"));
    url.setQuery(query);
    return url.toDisplayString();
}

bool TargetConfig::isNetworkHost() const
{
    return kind != ProbeKind::Demo && !hostPart().isEmpty();
}

QJsonObject TargetConfig::toJson() const
{
    QJsonObject o;
    o[QStringLiteral("id")] = id;
    o[QStringLiteral("name")] = name;
    o[QStringLiteral("address")] = address;
    o[QStringLiteral("kind")] = kindToKey(kind);
    if (port) o[QStringLiteral("port")] = port;
    o[QStringLiteral("group")] = group;
    o[QStringLiteral("order")] = order;
    o[QStringLiteral("enabled")] = enabled;
    auto putInt = [&o](const char *k, int v) { if (v > 0) o[QLatin1String(k)] = v; };
    putInt("interval_ms", intervalMs);
    putInt("timeout_ms", timeoutMs);
    putInt("packet_size", packetSize);
    putInt("ttl", ttl);
    putInt("down_after", downAfter);
    putInt("up_after", upAfter);
    putInt("latency_warn_ms", latencyWarnMs);
    if (lossWarnPct >= 0) o[QStringLiteral("loss_warn_pct")] = lossWarnPct;
    if (dontFragment) o[QStringLiteral("df")] = true;
    if (!parentId.isEmpty()) o[QStringLiteral("parent")] = parentId;
    if (!notes.isEmpty()) o[QStringLiteral("notes")] = notes;
    if (!demoScenario.isEmpty()) o[QStringLiteral("demo")] = demoScenario;
    if (httpExpect) o[QStringLiteral("http_expect")] = httpExpect;
    if (!httpMatch.isEmpty()) o[QStringLiteral("http_match")] = httpMatch;
    if (tlsInsecure) o[QStringLiteral("tls_insecure")] = true;
    if (!tlsPin.isEmpty()) o[QStringLiteral("tls_pin")] = tlsPin;
    if (!sourceId.isEmpty()) {
        o[QStringLiteral("source")] = sourceId;
        o[QStringLiteral("source_key")] = sourceKey;
        if (sourceMissing) o[QStringLiteral("source_missing")] = true;
    }
    return o;
}

TargetConfig TargetConfig::fromJson(const QJsonObject &o)
{
    TargetConfig t;
    t.id = o.value(QStringLiteral("id")).toString();
    if (t.id.isEmpty()) t.id = newTargetId();
    t.name = o.value(QStringLiteral("name")).toString();
    t.address = o.value(QStringLiteral("address")).toString().trimmed();
    t.kind = kindFromKey(o.value(QStringLiteral("kind")).toString(QStringLiteral("icmp")));
    t.port = qBound(0, o.value(QStringLiteral("port")).toInt(), 65535);
    t.group = o.value(QStringLiteral("group")).toString();
    t.order = o.value(QStringLiteral("order")).toInt();
    t.enabled = o.value(QStringLiteral("enabled")).toBool(true);
    t.intervalMs = qBound(0, o.value(QStringLiteral("interval_ms")).toInt(), 86400000);
    t.timeoutMs = qBound(0, o.value(QStringLiteral("timeout_ms")).toInt(), 60000);
    t.packetSize = qBound(0, o.value(QStringLiteral("packet_size")).toInt(), 65000);
    t.ttl = qBound(0, o.value(QStringLiteral("ttl")).toInt(), 255);
    t.dontFragment = o.value(QStringLiteral("df")).toBool();
    t.downAfter = qBound(0, o.value(QStringLiteral("down_after")).toInt(), 100);
    t.upAfter = qBound(0, o.value(QStringLiteral("up_after")).toInt(), 100);
    t.latencyWarnMs = qBound(0, o.value(QStringLiteral("latency_warn_ms")).toInt(), 60000);
    t.lossWarnPct = o.contains(QStringLiteral("loss_warn_pct")) ? o.value(QStringLiteral("loss_warn_pct")).toDouble() : -1.0;
    t.parentId = o.value(QStringLiteral("parent")).toString();
    t.notes = o.value(QStringLiteral("notes")).toString();
    t.demoScenario = o.value(QStringLiteral("demo")).toString();
    t.httpExpect = o.value(QStringLiteral("http_expect")).toInt();
    t.httpMatch = o.value(QStringLiteral("http_match")).toString();
    t.tlsInsecure = o.value(QStringLiteral("tls_insecure")).toBool();
    t.tlsPin = o.value(QStringLiteral("tls_pin")).toString();
    t.sourceId = o.value(QStringLiteral("source")).toString();
    t.sourceKey = o.value(QStringLiteral("source_key")).toString();
    t.sourceMissing = o.value(QStringLiteral("source_missing")).toBool();
    return t;
}

ParsedInput parseTargetInput(const QString &lineIn)
{
    ParsedInput r;
    QString line = lineIn.trimmed();
    QString label;
    // «адрес # подпись» — как в избранном vmPing
    const int hash = line.indexOf(QStringLiteral(" #"));
    if (hash >= 0) {
        label = line.mid(hash + 2).trimmed();
        line = line.left(hash).trimmed();
    } else if (line.contains(QLatin1Char('\t'))) {
        const int tab = line.indexOf(QLatin1Char('\t'));
        label = line.mid(tab + 1).trimmed();
        line = line.left(tab).trimmed();
    }
    if (line.isEmpty()) {
        r.error = QStringLiteral("Пустой адрес");
        return r;
    }
    TargetConfig &t = r.target;
    t.id = newTargetId();
    t.name = label;

    const QString lower = line.toLower();
    if (lower.startsWith(QLatin1String("http://")) || lower.startsWith(QLatin1String("https://"))) {
        const QUrl u(line, QUrl::StrictMode);
        if (!u.isValid() || u.host().isEmpty()) {
            r.error = QStringLiteral("Неверный адрес URL");
            return r;
        }
        t.kind = ProbeKind::Http;
        t.address = u.toString();
        r.ok = true;
        return r;
    }

    auto splitHostPort = [](const QString &s, QString &host, int &port) -> bool {
        // [v6]:port | host:port | host
        if (s.startsWith(QLatin1Char('['))) {
            const int close = s.indexOf(QLatin1Char(']'));
            if (close < 0) return false;
            host = s.mid(1, close - 1);
            const QString rest = s.mid(close + 1);
            if (rest.startsWith(QLatin1Char(':'))) {
                bool ok = false;
                port = rest.mid(1).toInt(&ok);
                return ok && port > 0 && port < 65536;
            }
            port = 0;
            return rest.isEmpty();
        }
        const int colons = s.count(QLatin1Char(':'));
        if (colons == 1) {
            host = s.section(QLatin1Char(':'), 0, 0);
            bool ok = false;
            port = s.section(QLatin1Char(':'), 1).toInt(&ok);
            return ok && port > 0 && port < 65536;
        }
        host = s;   // IPv6 без скобок или просто хост
        port = 0;
        return true;
    };

    QString rest = line;
    ProbeKind kind = ProbeKind::Icmp;
    bool explicitKind = false;
    static const QRegularExpression schemeRe(QStringLiteral("^(icmp|ping|tcp|sip|dns):(?!//)"),
                                             QRegularExpression::CaseInsensitiveOption);
    const auto m = schemeRe.match(rest);
    if (m.hasMatch()) {
        kind = kindFromKey(m.captured(1));
        rest = rest.mid(m.capturedLength());
        explicitKind = true;
    }

    QString host;
    int port = 0;
    if (!splitHostPort(rest, host, port)) {
        r.error = QStringLiteral("Неверный порт");
        return r;
    }
    if (!isValidHostName(host)) {
        r.error = QStringLiteral("Неверный адрес: %1").arg(host);
        return r;
    }
    if (!explicitKind && port > 0)
        kind = ProbeKind::Tcp;            // host:port — «порт-пинг» как в vmPing
    if (kind == ProbeKind::Tcp && port == 0) {
        r.error = QStringLiteral("Для TCP нужен порт: хост:порт");
        return r;
    }
    if (kind == ProbeKind::Sip && port == 0)
        port = 5060;
    if (kind == ProbeKind::Icmp || kind == ProbeKind::Dns)
        port = 0;
    t.kind = kind;
    t.address = host;
    t.port = port;
    r.ok = true;
    return r;
}

QVector<TargetPreset> targetPresets()
{
    return {
        {QStringLiteral("icmp"), QStringLiteral("Узел (ICMP)"), ProbeKind::Icmp, 0, QString()},
        {QStringLiteral("phone"), QStringLiteral("Телефон (ICMP)"), ProbeKind::Icmp, 0, QString()},
        {QStringLiteral("sip"), QStringLiteral("SIP-телефон или АТС (OPTIONS, 5060)"), ProbeKind::Sip, 5060, QString()},
        {QStringLiteral("inei"), QStringLiteral("Сервер Инея (HTTP 8000, /login)"), ProbeKind::Http, 8000,
         QStringLiteral("http://{host}:8000/login")},
        {QStringLiteral("rosa"), QStringLiteral("Роса (HTTPS, /healthz)"), ProbeKind::Http, 18443,
         QStringLiteral("https://{host}:18443/healthz")},
        {QStringLiteral("h323"), QStringLiteral("H.323-терминал (TCP 1720)"), ProbeKind::Tcp, 1720, QString()},
        {QStringLiteral("web"), QStringLiteral("Веб-сервер (HTTPS)"), ProbeKind::Http, 443, QStringLiteral("https://{host}/")},
        {QStringLiteral("ssh"), QStringLiteral("SSH (TCP 22)"), ProbeKind::Tcp, 22, QString()},
        {QStringLiteral("dns"), QStringLiteral("Имя в DNS"), ProbeKind::Dns, 0, QString()},
    };
}

TargetConfig applyPreset(const TargetPreset &p, const QString &host, const QString &name)
{
    TargetConfig t;
    t.id = newTargetId();
    t.name = name;
    t.kind = p.kind;
    if (p.kind == ProbeKind::Http) {
        QString h = host;
        if (h.contains(QLatin1Char(':')) && !h.startsWith(QLatin1Char('[')))
            h = QLatin1Char('[') + h + QLatin1Char(']');
        t.address = QString(p.pathOrScheme).replace(QStringLiteral("{host}"), h);
        t.tlsInsecure = false;
    } else {
        t.address = host;
        t.port = p.port;
    }
    return t;
}

} // namespace zr
