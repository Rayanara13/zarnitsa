// Разбор вывода iputils ping (Astra, Debian) — для запасного пути через системный ping.
#pragma once

#include <QString>

namespace zr {

struct PingParse {
    enum Kind { Reply, TtlExceeded, Unreachable, NoReply, Error };
    Kind kind = NoReply;
    double rttMs = -1.0;
    int ttl = -1;
    QString from;
    QString error;
};

PingParse parsePingOutput(const QString &text);

} // namespace zr
