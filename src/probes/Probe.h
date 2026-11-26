// Запрос и результат одной проверки, базовый класс исполнителя.
#pragma once

#include "core/Types.h"

#include <QObject>
#include <QString>

namespace zr {

enum class ProbeOutcome {
    Ok,
    Timeout,
    Refused,        // TCP RST, порт закрыт
    Unreachable,    // ICMP destination unreachable
    TtlExceeded,    // ICMP time exceeded — для трассировки
    NameError,      // имя не разрешилось
    BadResponse,    // HTTP-код или содержимое не те
    Error,
};

struct ProbeRequest {
    quint64 token = 0;
    QString targetId;
    ProbeKind kind = ProbeKind::Icmp;
    QString host;           // IP или имя; для HTTP — URL
    quint16 port = 0;
    int timeoutMs = 1000;
    int size = 56;
    int ttl = 64;
    bool df = false;
    int httpExpect = 0;
    QString httpMatch;
    bool tlsInsecure = false;
    QString tlsPin;
    QString demoScenario;   // только для демо-режима
};

struct ProbeResult {
    quint64 token = 0;
    QString targetId;
    bool ok = false;
    ProbeOutcome outcome = ProbeOutcome::Error;
    double rttMs = -1.0;
    int ttl = -1;
    QString from;           // кто ответил (ICMP)
    QString error;          // текст для человека
    QString detail;         // «HTTP 200», «SIP 200 OK», список адресов DNS
    int code = 0;           // код HTTP/SIP
    qint64 certExpiry = 0;  // мс от эпохи, для HTTPS
    QString resolved;       // IP, по которому шла проверка
};

QString outcomeText(ProbeOutcome o);

class ProbeBackend : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    ~ProbeBackend() override = default;
    virtual void start(const ProbeRequest &req) = 0;
    virtual void cancel(quint64 token) { Q_UNUSED(token); }

signals:
    void finished(const zr::ProbeResult &r);

protected:
    // Немедленные отказы отдаём через очередь событий: вызывающий может ещё обходить свои списки.
    void finishLater(const ProbeResult &r)
    {
        QMetaObject::invokeMethod(this, [this, r]() { emit finished(r); }, Qt::QueuedConnection);
    }
};

ProbeResult makeResult(const ProbeRequest &req, ProbeOutcome o, const QString &error = QString());

} // namespace zr

Q_DECLARE_METATYPE(zr::ProbeResult)
