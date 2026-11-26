#include "ProbeDispatcher.h"

#include "IcmpEngine.h"
#include "Resolver.h"

namespace zr {

// Исполнитель обязан ответить сам; страховка срабатывает только при его поломке,
// поэтому запас большой: очередь системного ping может стоять долго.
static constexpr qint64 kWatchdogMarginMs = 60000;

ProbeDispatcher::ProbeDispatcher(QObject *parent)
    : QObject(parent)
    , m_resolver(new Resolver(this))
{
    qRegisterMetaType<zr::ProbeResult>("zr::ProbeResult");
    m_clock.start();
    m_sweep.setInterval(1000);
    connect(&m_sweep, &QTimer::timeout, this, &ProbeDispatcher::sweep);
    m_sweep.start();
}

void ProbeDispatcher::setBackend(ProbeKind kind, ProbeBackend *backend)
{
    if (ProbeBackend *old = m_backends.value(int(kind)))
        disconnect(old, nullptr, this, nullptr);
    m_backends.insert(int(kind), backend);
    if (backend)
        connect(backend, &ProbeBackend::finished, this, &ProbeDispatcher::onFinished);
}

void ProbeDispatcher::setIcmpEngine(IcmpEngine *engine)
{
    m_icmp = engine;
    setBackend(ProbeKind::Icmp, engine);
}

quint64 ProbeDispatcher::start(ProbeRequest req, Callback cb)
{
    const quint64 token = m_next++;
    req.token = token;
    Pending p;
    p.cb = std::move(cb);
    p.req = req;
    p.deadline = m_clock.elapsed() + req.timeoutMs + kWatchdogMarginMs;
    m_pending.insert(token, std::move(p));

    const bool needsIp = !m_demo
        && (req.kind == ProbeKind::Icmp || req.kind == ProbeKind::Tcp || req.kind == ProbeKind::Sip);
    if (!needsIp) {
        dispatch(token);
        return token;
    }
    m_resolver->resolve(req.host, [this, token](const QHostAddress &addr, const QString &error) {
        auto it = m_pending.find(token);
        if (it == m_pending.end())
            return;
        if (!error.isEmpty() || addr.isNull()) {
            ProbeResult r = makeResult(it->req, ProbeOutcome::NameError, error.isEmpty() ? QStringLiteral("Имя не найдено") : error);
            // ответ из кэша приходит синхронно — отдаём его через очередь, вызывающий ещё обходит свои цели
            QMetaObject::invokeMethod(this, [this, token, r]() { complete(token, r); }, Qt::QueuedConnection);
            return;
        }
        it->req.host = addr.toString();
        dispatch(token);
    });
    return token;
}

void ProbeDispatcher::dispatch(quint64 token)
{
    auto it = m_pending.find(token);
    if (it == m_pending.end())
        return;
    ProbeBackend *b = m_demo ? m_demo : m_backends.value(int(it->req.kind));
    if (!b) {
        const ProbeResult r = makeResult(it->req, ProbeOutcome::Error, QStringLiteral("Вид проверки недоступен"));
        QMetaObject::invokeMethod(this, [this, token, r]() { complete(token, r); }, Qt::QueuedConnection);
        return;
    }
    it->sent = true;
    const ProbeRequest req = it->req;   // исполнитель может ответить синхронно и стереть запись
    b->start(req);
}

void ProbeDispatcher::cancel(quint64 token)
{
    auto it = m_pending.find(token);
    if (it == m_pending.end())
        return;
    if (it->sent)
        if (ProbeBackend *b = m_backends.value(int(it->req.kind)))
            b->cancel(token);
    m_pending.erase(it);
}

void ProbeDispatcher::onFinished(const ProbeResult &r)
{
    complete(r.token, r);
}

void ProbeDispatcher::complete(quint64 token, ProbeResult r)
{
    auto it = m_pending.find(token);
    if (it == m_pending.end())
        return;
    Callback cb = std::move(it->cb);
    r.token = token;
    r.targetId = it->req.targetId;
    if (r.resolved.isEmpty() && it->req.kind != ProbeKind::Http && it->req.kind != ProbeKind::Dns)
        r.resolved = it->req.host;
    m_pending.erase(it);

    ++m_rateCount;
    const qint64 now = m_clock.elapsed();
    if (now - m_rateWindowStart >= 2000) {
        m_rate = double(m_rateCount) * 1000.0 / double(qMax<qint64>(1, now - m_rateWindowStart));
        m_rateCount = 0;
        m_rateWindowStart = now;
    }
    if (cb)
        cb(r);
}

void ProbeDispatcher::sweep()
{
    const qint64 now = m_clock.elapsed();
    QVector<quint64> expired;
    for (auto it = m_pending.constBegin(); it != m_pending.constEnd(); ++it)
        if (it->deadline <= now)
            expired.append(it.key());
    for (quint64 token : expired) {
        auto it = m_pending.find(token);
        if (it == m_pending.end())
            continue;
        if (it->sent)
            if (ProbeBackend *b = m_backends.value(int(it->req.kind)))
                b->cancel(token);
        complete(token, makeResult(it->req, ProbeOutcome::Timeout, QStringLiteral("Нет ответа исполнителя")));
    }
    if (now - m_rateWindowStart >= 4000) {
        m_rate = 0.0;
        m_rateCount = 0;
        m_rateWindowStart = now;
    }
}

QString outcomeText(ProbeOutcome o)
{
    switch (o) {
    case ProbeOutcome::Ok: return QStringLiteral("Ответ");
    case ProbeOutcome::Timeout: return QStringLiteral("Таймаут");
    case ProbeOutcome::Refused: return QStringLiteral("Порт закрыт");
    case ProbeOutcome::Unreachable: return QStringLiteral("Узел недоступен");
    case ProbeOutcome::TtlExceeded: return QStringLiteral("Истёк TTL");
    case ProbeOutcome::NameError: return QStringLiteral("Имя не найдено");
    case ProbeOutcome::BadResponse: return QStringLiteral("Неверный ответ");
    case ProbeOutcome::Error: return QStringLiteral("Ошибка");
    }
    return QString();
}

ProbeResult makeResult(const ProbeRequest &req, ProbeOutcome o, const QString &error)
{
    ProbeResult r;
    r.token = req.token;
    r.targetId = req.targetId;
    r.outcome = o;
    r.ok = o == ProbeOutcome::Ok;
    r.error = error.isEmpty() && !r.ok ? outcomeText(o) : error;
    return r;
}

} // namespace zr
