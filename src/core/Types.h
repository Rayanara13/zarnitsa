// Общие типы Зарницы: виды проверок, статусы и их подписи.
#pragma once

#include <QHashFunctions>
#include <QMetaType>
#include <QString>

namespace zr {

enum class ProbeKind { Icmp, Tcp, Http, Sip, Dns, Demo };

// Порядок важен: по нему сортируется «проблемные сверху» (больше — тревожнее).
enum class Status { Paused = 0, Up = 1, Unknown = 2, Degraded = 3, Down = 4 };

QString kindToKey(ProbeKind k);
ProbeKind kindFromKey(const QString &key, bool *ok = nullptr);
QString kindTitle(ProbeKind k);

QString statusKey(Status s);          // up | degraded | down | unknown | paused
Status statusFromKey(const QString &key);
QString statusTitle(Status s);        // «В норме», «Плохо», ...
QString statusIneiColor(Status s);    // green | yellow | red | gray — словарь «Виктории»

constexpr const char *kAppName = "zarnitsa";
constexpr const char *kAppTitle = "Зарница";
QString appVersion();

inline uint qHash(Status s, uint seed = 0) noexcept { return ::qHash(int(s), seed); }
inline uint qHash(ProbeKind k, uint seed = 0) noexcept { return ::qHash(int(k), seed); }

} // namespace zr

Q_DECLARE_METATYPE(zr::Status)
Q_DECLARE_METATYPE(zr::ProbeKind)
