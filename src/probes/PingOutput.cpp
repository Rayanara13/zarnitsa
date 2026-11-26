#include "PingOutput.h"

#include <QRegularExpression>
#include <QStringList>

namespace zr {

PingParse parsePingOutput(const QString &text)
{
    PingParse p;
    static const QRegularExpression reply(QStringLiteral(
        "bytes from ([^\\s:]+)(?: \\(([^)]+)\\))?:\\s+(?:icmp_)?seq=\\d+\\s+ttl=(\\d+)(?:\\s+time[=<]\\s*([0-9]+(?:[.,][0-9]+)?)\\s*ms)?"));
    static const QRegularExpression ttlx(QStringLiteral(
        "^From ([^\\s:]+)(?: \\(([^)]+)\\))?:?\\s+icmp_seq=\\d+\\s+Time to live exceeded"),
        QRegularExpression::MultilineOption);
    static const QRegularExpression unreach(QStringLiteral(
        "^From ([^\\s:]+)(?: \\(([^)]+)\\))?:?\\s+icmp_seq=\\d+\\s+(Destination .*Unreachable|Packet filtered|Destination .*Prohibited)"),
        QRegularExpression::MultilineOption);

    const auto m = reply.match(text);
    if (m.hasMatch()) {
        p.kind = PingParse::Reply;
        p.from = m.captured(2).isEmpty() ? m.captured(1) : m.captured(2);
        p.ttl = m.captured(3).toInt();
        if (!m.captured(4).isEmpty())
            p.rttMs = QString(m.captured(4)).replace(QLatin1Char(','), QLatin1Char('.')).toDouble();
        return p;
    }
    const auto t = ttlx.match(text);
    if (t.hasMatch()) {
        p.kind = PingParse::TtlExceeded;
        p.from = t.captured(2).isEmpty() ? t.captured(1) : t.captured(2);
        return p;
    }
    const auto u = unreach.match(text);
    if (u.hasMatch()) {
        p.kind = PingParse::Unreachable;
        p.from = u.captured(2).isEmpty() ? u.captured(1) : u.captured(2);
        p.error = u.captured(3);
        return p;
    }
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString l = line.trimmed();
        if (l.startsWith(QLatin1String("ping:")) || l.startsWith(QLatin1String("ping6:"))) {
            p.kind = PingParse::Error;
            p.error = l.section(QLatin1Char(':'), 1).trimmed();
            return p;
        }
    }
    p.kind = PingParse::NoReply;
    return p;
}

} // namespace zr
