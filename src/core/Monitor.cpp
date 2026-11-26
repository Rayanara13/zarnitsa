#include "Monitor.h"

#include "probes/Probe.h"
#include "probes/ProbeDispatcher.h"

#include <QDateTime>
#include <QRandomGenerator>
#include <QSet>
#include <algorithm>

namespace zr {

static constexpr int kTickMs = 50;
static constexpr int kStreakToDegrade = 3;

// ---------------------------------------------------------------------------
// События и сводка
// ---------------------------------------------------------------------------

QJsonObject StatusEvent::toJson() const
{
    QJsonObject o;
    o[QStringLiteral("t")] = double(t);
    o[QStringLiteral("id")] = id;
    o[QStringLiteral("name")] = name;
    o[QStringLiteral("address")] = address;
    o[QStringLiteral("group")] = group;
    o[QStringLiteral("from")] = statusKey(from);
    o[QStringLiteral("to")] = statusKey(to);
    if (rttMs >= 0) o[QStringLiteral("rtt_ms")] = qRound(rttMs * 100.0) / 100.0;
    o[QStringLiteral("loss_pct")] = qRound(lossPct * 10.0) / 10.0;
    if (!reason.isEmpty()) o[QStringLiteral("reason")] = reason;
    if (flapping) o[QStringLiteral("flapping")] = true;
    if (dependent) o[QStringLiteral("dependent")] = true;
    if (downForMs > 0) o[QStringLiteral("down_for_ms")] = double(downForMs);
    return o;
}

StatusEvent StatusEvent::fromJson(const QJsonObject &o)
{
    StatusEvent e;
    e.t = qint64(o.value(QStringLiteral("t")).toDouble());
    e.id = o.value(QStringLiteral("id")).toString();
    e.name = o.value(QStringLiteral("name")).toString();
    e.address = o.value(QStringLiteral("address")).toString();
    e.group = o.value(QStringLiteral("group")).toString();
    e.from = statusFromKey(o.value(QStringLiteral("from")).toString());
    e.to = statusFromKey(o.value(QStringLiteral("to")).toString());
    e.rttMs = o.contains(QStringLiteral("rtt_ms")) ? o.value(QStringLiteral("rtt_ms")).toDouble() : -1.0;
    e.lossPct = o.value(QStringLiteral("loss_pct")).toDouble();
    e.reason = o.value(QStringLiteral("reason")).toString();
    e.flapping = o.value(QStringLiteral("flapping")).toBool();
    e.dependent = o.value(QStringLiteral("dependent")).toBool();
    e.downForMs = qint64(o.value(QStringLiteral("down_for_ms")).toDouble());
    return e;
}

static QString durationText(qint64 ms)
{
    const qint64 s = ms / 1000;
    if (s < 60) return QStringLiteral("%1 с").arg(s);
    if (s < 3600) return QStringLiteral("%1 мин").arg(s / 60);
    if (s < 86400) return QStringLiteral("%1 ч %2 мин").arg(s / 3600).arg((s % 3600) / 60);
    return QStringLiteral("%1 д %2 ч").arg(s / 86400).arg((s % 86400) / 3600);
}

QString StatusEvent::text() const
{
    QStringList parts;
    parts << statusTitle(to);
    if (to == Status::Down && dependent) parts << QStringLiteral("лежит родитель");
    if (!reason.isEmpty() && to != Status::Up) parts << reason;
    if (to == Status::Up && downForMs > 0) parts << QStringLiteral("не было связи %1").arg(durationText(downForMs));
    if (flapping) parts << QStringLiteral("дребезг");
    return parts.join(QStringLiteral(" · "));
}

Status Summary::worst() const
{
    if (down) return Status::Down;
    if (degraded) return Status::Degraded;
    if (unknown) return Status::Unknown;
    if (up) return Status::Up;
    return Status::Paused;
}

// ---------------------------------------------------------------------------
// Монитор
// ---------------------------------------------------------------------------

Monitor::Monitor(ProbeDispatcher *dispatcher, QObject *parent)
    : QObject(parent)
    , m_dispatcher(dispatcher)
{
    qRegisterMetaType<zr::StatusEvent>("zr::StatusEvent");
    m_mono.start();
    m_tick.setInterval(kTickMs);
    connect(&m_tick, &QTimer::timeout, this, &Monitor::tick);
}

qint64 Monitor::nowMs() const
{
    return m_fakeNow ? m_fakeNow : QDateTime::currentMSecsSinceEpoch();
}

void Monitor::setSettings(const Settings &s)
{
    m_settings = s;
}

int Monitor::effInterval(const TargetConfig &t) const { return t.intervalMs > 0 ? t.intervalMs : m_settings.intervalMs; }
int Monitor::effTimeout(const TargetConfig &t) const { return t.timeoutMs > 0 ? t.timeoutMs : m_settings.timeoutMs; }
int Monitor::effDownAfter(const TargetConfig &t) const { return t.downAfter > 0 ? t.downAfter : m_settings.downAfter; }
int Monitor::effUpAfter(const TargetConfig &t) const { return t.upAfter > 0 ? t.upAfter : m_settings.upAfter; }
int Monitor::effLatencyWarn(const TargetConfig &t) const { return t.latencyWarnMs > 0 ? t.latencyWarnMs : m_settings.latencyWarnMs; }
double Monitor::effLossWarn(const TargetConfig &t) const { return t.lossWarnPct >= 0 ? t.lossWarnPct : m_settings.lossWarnPct; }

const TargetConfig *Monitor::target(const QString &id) const
{
    const int i = indexOf(id);
    return i >= 0 ? &m_targets[i] : nullptr;
}

const TargetRuntime *Monitor::runtime(const QString &id) const
{
    const auto it = m_rt.constFind(id);
    return it == m_rt.constEnd() ? nullptr : &it.value();
}

int Monitor::indexOf(const QString &id) const
{
    for (int i = 0; i < m_targets.size(); ++i)
        if (m_targets[i].id == id)
            return i;
    return -1;
}

TargetConfig *Monitor::findBySource(const QString &sourceId, const QString &key)
{
    for (TargetConfig &t : m_targets)
        if (t.sourceId == sourceId && t.sourceKey == key)
            return &t;
    return nullptr;
}

WindowStats Monitor::windowStats(const QString &id) const
{
    const TargetRuntime *rt = runtime(id);
    return rt ? rt->stats.window(m_settings.statsWindow) : WindowStats();
}

double Monitor::mos(const QString &id) const
{
    const TargetRuntime *rt = runtime(id);
    if (!rt || !rt->stats.hasData()) return -1.0;
    const WindowStats w = rt->stats.window(m_settings.statsWindow);
    if (w.recv == 0) return 1.0;
    return TargetStats::mos(w.avg, w.jitter, w.lossPct);
}

void Monitor::ensureGroupDeclared(const QString &name)
{
    for (const GroupInfo &g : m_groups)
        if (g.name == name)
            return;
    GroupInfo g;
    g.name = name;
    m_groups.append(g);
}

QVector<GroupInfo> Monitor::groups() const
{
    QVector<GroupInfo> out = m_groups;
    QSet<QString> known;
    for (const GroupInfo &g : out) known.insert(g.name);
    for (const TargetConfig &t : m_targets) {
        if (!known.contains(t.group)) {
            GroupInfo g;
            g.name = t.group;
            out.append(g);
            known.insert(t.group);
        }
    }
    return out;
}

void Monitor::normalizeOrder()
{
    const QVector<GroupInfo> gs = groups();
    QHash<QString, int> rank;
    for (int i = 0; i < gs.size(); ++i) rank.insert(gs[i].name, i);
    std::stable_sort(m_targets.begin(), m_targets.end(), [&rank](const TargetConfig &a, const TargetConfig &b) {
        return rank.value(a.group) < rank.value(b.group);
    });
    QHash<QString, int> counters;
    for (TargetConfig &t : m_targets)
        t.order = counters[t.group]++;
}

static TargetRuntime freshRuntime(const TargetConfig &t, qint64 now, qint64 mono, int interval)
{
    TargetRuntime rt;
    rt.status = t.enabled ? Status::Unknown : Status::Paused;
    rt.since = now;
    // разносим первые замеры, чтобы тысяча целей не стартовала в одну миллисекунду
    rt.nextDue = mono + QRandomGenerator::global()->bounded(qMax(1, qMin(interval, 1500)));
    return rt;
}

QString Monitor::addTarget(TargetConfig t)
{
    const QStringList ids = addTargets({t});
    return ids.isEmpty() ? QString() : ids.first();
}

QStringList Monitor::addTargets(QVector<TargetConfig> list)
{
    QStringList ids;
    if (list.isEmpty())
        return ids;
    const qint64 now = nowMs();
    for (TargetConfig &t : list) {
        if (t.id.isEmpty() || indexOf(t.id) >= 0)
            t.id = newTargetId();
        ensureGroupDeclared(t.group);
        m_targets.append(t);
        m_rt.insert(t.id, freshRuntime(t, now, mono(), effInterval(t)));
        ids << t.id;
    }
    normalizeOrder();
    emit targetsChanged();
    emit summaryChanged();
    return ids;
}

bool Monitor::updateTarget(const TargetConfig &t)
{
    const int i = indexOf(t.id);
    if (i < 0)
        return false;
    const TargetConfig old = m_targets[i];
    m_targets[i] = t;
    ensureGroupDeclared(t.group);
    TargetRuntime &rt = m_rt[t.id];
    const bool probeChanged = old.address != t.address || old.kind != t.kind || old.port != t.port
        || old.httpExpect != t.httpExpect || old.httpMatch != t.httpMatch || old.tlsPin != t.tlsPin
        || old.tlsInsecure != t.tlsInsecure || old.packetSize != t.packetSize || old.ttl != t.ttl
        || old.dontFragment != t.dontFragment;
    if (probeChanged) {
        if (rt.inFlight) m_dispatcher->cancel(rt.token);
        const quint32 gen = rt.generation + 1;
        rt = freshRuntime(t, nowMs(), mono(), effInterval(t));
        rt.generation = gen;
        rt.nextDue = mono();
    }
    if (old.enabled != t.enabled) {
        m_targets[i].enabled = old.enabled;
        setEnabled({t.id}, t.enabled);
    }
    if (old.group != t.group)
        normalizeOrder();
    emit targetsChanged();
    emit targetUpdated(t.id);
    emit summaryChanged();
    return true;
}

void Monitor::removeTargets(const QStringList &ids)
{
    if (ids.isEmpty())
        return;
    const QSet<QString> set(ids.begin(), ids.end());
    for (const QString &id : ids) {
        auto it = m_rt.find(id);
        if (it != m_rt.end()) {
            if (it->inFlight) m_dispatcher->cancel(it->token);
            m_rt.erase(it);
        }
    }
    m_targets.erase(std::remove_if(m_targets.begin(), m_targets.end(),
                                   [&set](const TargetConfig &t) { return set.contains(t.id); }),
                    m_targets.end());
    for (TargetConfig &t : m_targets)
        if (set.contains(t.parentId))
            t.parentId.clear();
    normalizeOrder();
    emit targetsChanged();
    emit summaryChanged();
}

void Monitor::setEnabled(const QStringList &ids, bool enabled)
{
    bool any = false;
    for (const QString &id : ids) {
        const int i = indexOf(id);
        if (i < 0 || m_targets[i].enabled == enabled)
            continue;
        m_targets[i].enabled = enabled;
        TargetRuntime &rt = m_rt[id];
        if (!enabled) {
            if (rt.inFlight) {
                m_dispatcher->cancel(rt.token);
                rt.inFlight = false;
            }
            ++rt.generation;
            transition(m_targets[i], rt, Status::Paused, QString());
        } else {
            rt.consecFail = rt.consecOk = rt.badStreak = rt.goodStreak = 0;
            rt.nextDue = mono();
            transition(m_targets[i], rt, Status::Unknown, QString());
        }
        emit targetUpdated(id);
        any = true;
    }
    if (any) {
        emit targetsChanged();
        emit summaryChanged();
    }
}

void Monitor::moveTargets(const QStringList &ids, const QString &group, const QString &beforeId)
{
    if (ids.isEmpty())
        return;
    const QSet<QString> set(ids.begin(), ids.end());
    if (set.contains(beforeId))
        return;
    QVector<TargetConfig> moving;
    for (const QString &id : ids) {
        const int i = indexOf(id);
        if (i >= 0) moving.append(m_targets[i]);
    }
    m_targets.erase(std::remove_if(m_targets.begin(), m_targets.end(),
                                   [&set](const TargetConfig &t) { return set.contains(t.id); }),
                    m_targets.end());
    for (TargetConfig &t : moving)
        t.group = group;
    ensureGroupDeclared(group);
    int pos = -1;
    if (!beforeId.isEmpty())
        pos = indexOf(beforeId);
    if (pos < 0) {
        // в конец группы
        pos = m_targets.size();
        for (int i = m_targets.size() - 1; i >= 0; --i) {
            if (m_targets[i].group == group) { pos = i + 1; break; }
        }
    }
    for (int k = 0; k < moving.size(); ++k)
        m_targets.insert(pos + k, moving[k]);
    normalizeOrder();
    emit targetsChanged();
}

void Monitor::setTargets(const QVector<TargetConfig> &list)
{
    for (auto it = m_rt.begin(); it != m_rt.end(); ++it)
        if (it->inFlight) m_dispatcher->cancel(it->token);
    m_rt.clear();
    m_targets.clear();
    QVector<TargetConfig> sorted = list;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const TargetConfig &a, const TargetConfig &b) { return a.order < b.order; });
    const qint64 now = nowMs();
    QSet<QString> seen;
    for (TargetConfig t : sorted) {
        if (t.id.isEmpty() || seen.contains(t.id)) t.id = newTargetId();
        seen.insert(t.id);
        ensureGroupDeclared(t.group);
        m_targets.append(t);
        m_rt.insert(t.id, freshRuntime(t, now, mono(), effInterval(t)));
    }
    normalizeOrder();
    emit targetsChanged();
    emit summaryChanged();
}

void Monitor::resetStats(const QStringList &ids)
{
    for (const QString &id : ids) {
        auto it = m_rt.find(id);
        if (it == m_rt.end()) continue;
        it->stats.clear();
        it->changes.clear();
        it->flapping = false;
        emit targetUpdated(id);
    }
}

void Monitor::setGroups(const QVector<GroupInfo> &g)
{
    m_groups = g;
    normalizeOrder();
    emit targetsChanged();
}

void Monitor::setGroupCollapsed(const QString &name, bool collapsed)
{
    ensureGroupDeclared(name);
    for (GroupInfo &g : m_groups)
        if (g.name == name) g.collapsed = collapsed;
    emit targetsChanged();
}

void Monitor::addGroup(const QString &name)
{
    ensureGroupDeclared(name);
    emit targetsChanged();
}

void Monitor::renameGroup(const QString &from, const QString &to)
{
    if (from == to)
        return;
    bool merged = false;
    for (const GroupInfo &g : m_groups)
        if (g.name == to) merged = true;
    for (int i = 0; i < m_groups.size(); ++i) {
        if (m_groups[i].name == from) {
            if (merged) m_groups.removeAt(i);
            else m_groups[i].name = to;
            break;
        }
    }
    for (TargetConfig &t : m_targets)
        if (t.group == from) t.group = to;
    normalizeOrder();
    emit targetsChanged();
}

void Monitor::removeGroup(const QString &name, bool withTargets)
{
    if (withTargets) {
        QStringList ids;
        for (const TargetConfig &t : m_targets)
            if (t.group == name) ids << t.id;
        removeTargets(ids);
    } else {
        for (TargetConfig &t : m_targets)
            if (t.group == name) t.group.clear();
        ensureGroupDeclared(QString());
    }
    for (int i = 0; i < m_groups.size(); ++i) {
        if (m_groups[i].name == name) { m_groups.removeAt(i); break; }
    }
    normalizeOrder();
    emit targetsChanged();
}

void Monitor::moveGroup(const QString &name, int newIndex)
{
    QVector<GroupInfo> gs = groups();
    int from = -1;
    for (int i = 0; i < gs.size(); ++i)
        if (gs[i].name == name) from = i;
    if (from < 0)
        return;
    const GroupInfo g = gs.takeAt(from);
    newIndex = qBound(0, newIndex, gs.size());
    gs.insert(newIndex, g);
    m_groups = gs;
    normalizeOrder();
    emit targetsChanged();
}

void Monitor::start()
{
    m_running = true;
    m_tick.start();
}

void Monitor::stop()
{
    m_running = false;
    m_tick.stop();
}

Summary Monitor::summary() const
{
    Summary s;
    for (const TargetConfig &t : m_targets) {
        ++s.total;
        const TargetRuntime *rt = runtime(t.id);
        switch (rt ? rt->status : Status::Unknown) {
        case Status::Up: ++s.up; break;
        case Status::Degraded: ++s.degraded; break;
        case Status::Down: ++s.down; break;
        case Status::Unknown: ++s.unknown; break;
        case Status::Paused: ++s.paused; break;
        }
    }
    return s;
}

Summary Monitor::groupSummary(const QString &group) const
{
    Summary s;
    for (const TargetConfig &t : m_targets) {
        if (t.group != group) continue;
        ++s.total;
        const TargetRuntime *rt = runtime(t.id);
        switch (rt ? rt->status : Status::Unknown) {
        case Status::Up: ++s.up; break;
        case Status::Degraded: ++s.degraded; break;
        case Status::Down: ++s.down; break;
        case Status::Unknown: ++s.unknown; break;
        case Status::Paused: ++s.paused; break;
        }
    }
    return s;
}

void Monitor::tick()
{
    if (!m_running)
        return;
    const qint64 m = mono();
    // обходим по снимку id: слоты событий могут менять список целей
    QStringList due;
    for (const TargetConfig &t : m_targets) {
        if (!t.enabled) continue;
        const auto it = m_rt.constFind(t.id);
        if (it == m_rt.constEnd() || it->inFlight || m < it->nextDue) continue;
        due << t.id;
    }
    for (const QString &id : due) {
        const int i = indexOf(id);
        if (i < 0) continue;
        auto it = m_rt.find(id);
        if (it == m_rt.end() || it->inFlight) continue;
        dispatch(m_targets[i], it.value(), m);
    }

    // раз в пару секунд — гасим «дребезг», если цель успокоилась
    static qint64 lastSweep = 0;
    if (m - lastSweep > 2000) {
        lastSweep = m;
        const qint64 now = nowMs();
        const qint64 horizon = now - qint64(m_settings.flapWindowSec) * 1000;
        for (auto it = m_rt.begin(); it != m_rt.end(); ++it) {
            TargetRuntime &rt = it.value();
            if (rt.changes.isEmpty()) continue;
            while (!rt.changes.isEmpty() && rt.changes.first() < horizon)
                rt.changes.removeFirst();
            if (rt.flapping && rt.changes.size() < qMax(1, m_settings.flapChanges / 2)) {
                rt.flapping = false;
                emit targetUpdated(it.key());
            }
        }
    }
}

void Monitor::dispatch(TargetConfig &t, TargetRuntime &rt, qint64 m)
{
    ProbeRequest req;
    req.targetId = t.id;
    req.kind = t.kind;
    req.host = t.address;
    req.port = quint16(t.port);
    req.timeoutMs = effTimeout(t);
    req.size = t.packetSize > 0 ? t.packetSize : m_settings.packetSize;
    req.ttl = t.ttl > 0 ? t.ttl : m_settings.ttl;
    req.df = t.dontFragment;
    req.httpExpect = t.httpExpect;
    req.httpMatch = t.httpMatch;
    req.tlsInsecure = t.tlsInsecure;
    req.tlsPin = t.tlsPin;
    req.demoScenario = t.demoScenario;
    rt.inFlight = true;
    rt.nextDue = m + effInterval(t);
    const QString id = t.id;
    const quint32 gen = rt.generation;
    rt.token = m_dispatcher->start(req, [this, id, gen](const ProbeResult &r) { onResult(id, gen, r); });
}

void Monitor::onResult(const QString &id, quint32 generation, const ProbeResult &r)
{
    auto it = m_rt.find(id);
    if (it == m_rt.end() || it->generation != generation)
        return;
    TargetRuntime &rt = it.value();
    rt.inFlight = false;
    const TargetConfig *t = target(id);
    if (!t || !t->enabled)
        return;
    rt.lastDetail = r.detail;
    rt.lastCode = r.code;
    rt.lastTtl = r.ttl;
    rt.lastFrom = r.from;
    if (r.certExpiry) rt.certExpiry = r.certExpiry;
    if (!r.resolved.isEmpty()) rt.resolved = r.resolved;
    apply(*t, rt, r.ok, r.rttMs, r.ok ? QString() : r.error);
}

void Monitor::injectResult(const QString &id, bool ok, double rttMs, const QString &error)
{
    auto it = m_rt.find(id);
    const TargetConfig *t = target(id);
    if (it == m_rt.end() || !t)
        return;
    apply(*t, it.value(), ok, rttMs, error);
}

void Monitor::apply(const TargetConfig &t, TargetRuntime &rt, bool ok, double rttMs, const QString &error)
{
    const qint64 now = nowMs();
    rt.lastProbe = now;
    rt.stats.add(now, ok, rttMs);
    if (ok) {
        ++rt.consecOk;
        rt.consecFail = 0;
        rt.lastError.clear();
    } else {
        ++rt.consecFail;
        rt.consecOk = 0;
        rt.lastError = error;
    }

    Status next = rt.status;
    QString reason = rt.reason;
    switch (rt.status) {
    case Status::Unknown:
        if (ok) { next = Status::Up; reason.clear(); }
        else if (rt.consecFail >= effDownAfter(t)) { next = Status::Down; reason = error; }
        break;
    case Status::Up:
    case Status::Degraded:
        if (!ok && rt.consecFail >= effDownAfter(t)) { next = Status::Down; reason = error; }
        break;
    case Status::Down:
        if (ok && rt.consecOk >= effUpAfter(t)) { next = Status::Up; reason.clear(); }
        else if (!ok) reason = error;
        break;
    case Status::Paused:
        return;
    }

    if (next == Status::Up || next == Status::Degraded) {
        bool bad = false;
        QString why;
        const WindowStats w = rt.stats.window(m_settings.statsWindow);
        const double lossWarn = effLossWarn(t);
        if (lossWarn > 0 && w.sent >= 5 && w.lossPct >= lossWarn) {
            bad = true;
            why = QStringLiteral("потери %1%").arg(QString::number(w.lossPct, 'f', w.lossPct < 10 ? 1 : 0));
        }
        int thr = 0;
        if (m_settings.latencyDynamic && t.latencyWarnMs <= 0) {
            if (rt.stats.bestEver() >= 0)
                thr = int(rt.stats.bestEver() + m_settings.latencyDynamicDeltaMs);
        } else {
            thr = effLatencyWarn(t);
        }
        const double avg5 = rt.stats.avgOfLastOk(5);
        if (thr > 0 && avg5 > thr) {
            bad = true;
            why = QStringLiteral("задержка %1 мс").arg(QString::number(avg5, 'f', avg5 < 10 ? 1 : 0));
        }
        if (t.kind == ProbeKind::Http && rt.certExpiry > 0) {
            const qint64 days = (rt.certExpiry - now) / 86400000;
            if (days < 14) {
                bad = true;
                why = rt.certExpiry < now ? QStringLiteral("сертификат истёк")
                                          : QStringLiteral("сертификат истекает через %1 дн.").arg(days);
            }
        }
        if (bad) { ++rt.badStreak; rt.goodStreak = 0; }
        else { ++rt.goodStreak; rt.badStreak = 0; }
        if (next == Status::Up && rt.status != Status::Down && bad && rt.badStreak >= kStreakToDegrade) {
            next = Status::Degraded;
            reason = why;
        } else if (next == Status::Degraded) {
            if (!bad && rt.goodStreak >= kStreakToDegrade) { next = Status::Up; reason.clear(); }
            else if (bad) reason = why;
        }
    }

    if (next != rt.status)
        transition(t, rt, next, reason);
    else
        rt.reason = reason;
    emit targetUpdated(t.id);
}

void Monitor::transition(const TargetConfig &t, TargetRuntime &rt, Status next, const QString &reason)
{
    const qint64 now = nowMs();
    StatusEvent ev;
    ev.t = now;
    ev.id = t.id;
    ev.name = t.displayName();
    ev.address = t.publicEndpointText();
    ev.group = t.group;
    ev.from = rt.status;
    ev.to = next;
    const WindowStats w = rt.stats.window(m_settings.statsWindow);
    ev.rttMs = w.last;
    ev.lossPct = w.lossPct;
    ev.reason = reason;

    if (rt.status == Status::Down || next == Status::Down) {
        rt.changes.append(now);
        const qint64 horizon = now - qint64(m_settings.flapWindowSec) * 1000;
        while (!rt.changes.isEmpty() && rt.changes.first() < horizon)
            rt.changes.removeFirst();
        rt.flapping = rt.changes.size() >= m_settings.flapChanges;
    }
    rt.dependent = false;
    if (!t.parentId.isEmpty()) {
        const TargetRuntime *prt = runtime(t.parentId);
        rt.dependent = prt && prt->status == Status::Down;
    }
    if (next == Status::Down)
        rt.downSince = now;
    if (rt.status == Status::Down && next != Status::Down && rt.downSince > 0)
        ev.downForMs = now - rt.downSince;
    ev.flapping = rt.flapping;
    ev.dependent = rt.dependent && next == Status::Down;

    rt.status = next;
    rt.since = now;
    rt.reason = reason;

    // дочерние цели помечаем «за родителем», когда родитель падает или поднимается
    for (const TargetConfig &child : m_targets) {
        if (child.parentId != t.id) continue;
        auto cit = m_rt.find(child.id);
        if (cit == m_rt.end()) continue;
        const bool dep = next == Status::Down;
        if (cit->dependent != dep) {
            cit->dependent = dep && cit->status == Status::Down ? true : dep;
            emit targetUpdated(child.id);
        }
    }

    emit statusChanged(ev);
    emit summaryChanged();
}

} // namespace zr
