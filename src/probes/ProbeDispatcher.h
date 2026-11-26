// Раздаёт проверки исполнителям по виду, разрешает имена и гарантирует ровно один ответ на запрос.
#pragma once

#include "Probe.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QTimer>
#include <functional>

namespace zr {

class Resolver;
class IcmpEngine;

class ProbeDispatcher : public QObject {
    Q_OBJECT
public:
    using Callback = std::function<void(const ProbeResult &)>;

    explicit ProbeDispatcher(QObject *parent = nullptr);

    void setBackend(ProbeKind kind, ProbeBackend *backend);
    ProbeBackend *backend(ProbeKind kind) const { return m_backends.value(int(kind)); }
    void setIcmpEngine(IcmpEngine *engine);
    IcmpEngine *icmp() const { return m_icmp; }
    Resolver *resolver() const { return m_resolver; }

    quint64 start(ProbeRequest req, Callback cb);
    void cancel(quint64 token);
    // --demo: все запросы уходят демо-исполнителю, вид цели остаётся настоящим
    void setDemo(ProbeBackend *demo) { m_demo = demo; if (demo) connect(demo, &ProbeBackend::finished, this, &ProbeDispatcher::onFinished); }
    bool isDemo() const { return m_demo != nullptr; }
    int inFlight() const { return m_pending.size(); }
    double probesPerSecond() const { return m_rate; }

private:
    struct Pending {
        Callback cb;
        ProbeRequest req;
        qint64 deadline = 0;   // страховочный срок по монотонным часам
        bool sent = false;
    };
    void dispatch(quint64 token);
    void onFinished(const ProbeResult &r);
    void sweep();
    void complete(quint64 token, ProbeResult r);

    QHash<int, ProbeBackend *> m_backends;
    QHash<quint64, Pending> m_pending;
    ProbeBackend *m_demo = nullptr;
    IcmpEngine *m_icmp = nullptr;
    Resolver *m_resolver = nullptr;
    quint64 m_next = 1;
    QTimer m_sweep;
    QElapsedTimer m_clock;
    qint64 m_rateWindowStart = 0;
    int m_rateCount = 0;
    double m_rate = 0.0;
};

} // namespace zr
