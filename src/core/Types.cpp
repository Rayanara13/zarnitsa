#include "Types.h"

#ifndef ZARNITSA_VERSION
#define ZARNITSA_VERSION "1.0.0"
#endif

namespace zr {

QString appVersion() { return QStringLiteral(ZARNITSA_VERSION); }

QString kindToKey(ProbeKind k)
{
    switch (k) {
    case ProbeKind::Icmp: return QStringLiteral("icmp");
    case ProbeKind::Tcp: return QStringLiteral("tcp");
    case ProbeKind::Http: return QStringLiteral("http");
    case ProbeKind::Sip: return QStringLiteral("sip");
    case ProbeKind::Dns: return QStringLiteral("dns");
    case ProbeKind::Demo: return QStringLiteral("demo");
    }
    return QStringLiteral("icmp");
}

ProbeKind kindFromKey(const QString &key, bool *ok)
{
    const QString k = key.trimmed().toLower();
    if (ok) *ok = true;
    if (k == QLatin1String("icmp") || k == QLatin1String("ping")) return ProbeKind::Icmp;
    if (k == QLatin1String("tcp")) return ProbeKind::Tcp;
    if (k == QLatin1String("http") || k == QLatin1String("https")) return ProbeKind::Http;
    if (k == QLatin1String("sip")) return ProbeKind::Sip;
    if (k == QLatin1String("dns")) return ProbeKind::Dns;
    if (k == QLatin1String("demo")) return ProbeKind::Demo;
    if (ok) *ok = false;
    return ProbeKind::Icmp;
}

QString kindTitle(ProbeKind k)
{
    switch (k) {
    case ProbeKind::Icmp: return QStringLiteral("ICMP");
    case ProbeKind::Tcp: return QStringLiteral("TCP");
    case ProbeKind::Http: return QStringLiteral("HTTP");
    case ProbeKind::Sip: return QStringLiteral("SIP");
    case ProbeKind::Dns: return QStringLiteral("DNS");
    case ProbeKind::Demo: return QStringLiteral("Демо");
    }
    return QString();
}

QString statusKey(Status s)
{
    switch (s) {
    case Status::Up: return QStringLiteral("up");
    case Status::Degraded: return QStringLiteral("degraded");
    case Status::Down: return QStringLiteral("down");
    case Status::Unknown: return QStringLiteral("unknown");
    case Status::Paused: return QStringLiteral("paused");
    }
    return QStringLiteral("unknown");
}

Status statusFromKey(const QString &key)
{
    if (key == QLatin1String("up")) return Status::Up;
    if (key == QLatin1String("degraded")) return Status::Degraded;
    if (key == QLatin1String("down")) return Status::Down;
    if (key == QLatin1String("paused")) return Status::Paused;
    return Status::Unknown;
}

QString statusTitle(Status s)
{
    switch (s) {
    case Status::Up: return QStringLiteral("В норме");
    case Status::Degraded: return QStringLiteral("Плохо");
    case Status::Down: return QStringLiteral("Нет связи");
    case Status::Unknown: return QStringLiteral("Нет данных");
    case Status::Paused: return QStringLiteral("Пауза");
    }
    return QString();
}

QString statusIneiColor(Status s)
{
    switch (s) {
    case Status::Up: return QStringLiteral("green");
    case Status::Degraded: return QStringLiteral("yellow");
    case Status::Down: return QStringLiteral("red");
    default: return QStringLiteral("gray");
    }
}

} // namespace zr
