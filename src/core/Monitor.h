// Монитор: расписание проверок, автомат статусов, дребезг, зависимости, группы.
#pragma once

#include "Settings.h"
#include "Stats.h"
#include "Target.h"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QTimer>
#include <QVector>

namespace zr {

class ProbeDispatcher;
struct ProbeResult;

struct GroupInfo {
    QString name;
    bool collapsed = false;
};

struct TargetRuntime {
    Status status = Status::Unknown;
    qint64 since = 0;            // мс от эпохи — когда вошли в текущий статус
    int consecFail = 0;
    int consecOk = 0;
    int badStreak = 0;           // подряд оценок «плохо» — вход в Degraded
    int goodStreak = 0;
    bool flapping = false;
    QVector<qint64> changes;     // моменты смен статуса с участием «Нет связи»
    bool dependent = false;      // лежит родитель
    QString reason;              // почему «Плохо» / «Нет связи»
    QString lastError;
    QString lastDetail;
    QString lastFrom;
    int lastCode = 0;
    int lastTtl = -1;
    qint64 certExpiry = 0;
    QString resolved;
    qint64 lastProbe = 0;
    qint64 downSince = 0;
    // расписание
    bool inFlight = false;
    qint64 nextDue = 0;          // монотонные мс
    quint64 token = 0;
    quint32 generation = 0;
    TargetStats stats;
};

struct StatusEvent {
    qint64 t = 0;
    QString id;
    QString name;
    QString address;
    QString group;
    Status from = Status::Unknown;
    Status to = Status::Unknown;
    double rttMs = -1.0;
    double lossPct = 0.0;
    QString reason;
    bool flapping = false;
    bool dependent = false;
    qint64 downForMs = 0;        // при восстановлении — сколько лежала

    QJsonObject toJson() const;
    static StatusEvent fromJson(const QJsonObject &o);
    QString text() const;        // «Нет связи · 10.0.0.1 · таймаут»
};

struct Summary {
    int total = 0, up = 0, degraded = 0, down = 0, unknown = 0, paused = 0;
    Status worst() const;
};

class Monitor : public QObject {
    Q_OBJECT
public:
    explicit Monitor(ProbeDispatcher *dispatcher, QObject *parent = nullptr);

    void setSettings(const Settings &s);
    const Settings &settings() const { return m_settings; }

    // цели
    const QVector<TargetConfig> &targets() const { return m_targets; }
    const TargetConfig *target(const QString &id) const;
    const TargetRuntime *runtime(const QString &id) const;
    int indexOf(const QString &id) const;
    QString addTarget(TargetConfig t);
    QStringList addTargets(QVector<TargetConfig> list);
    bool updateTarget(const TargetConfig &t);
    void removeTargets(const QStringList &ids);
    void setEnabled(const QStringList &ids, bool enabled);
    void moveTargets(const QStringList &ids, const QString &group, const QString &beforeId);
    void setTargets(const QVector<TargetConfig> &list);
    void resetStats(const QStringList &ids);
    TargetConfig *findBySource(const QString &sourceId, const QString &key);

    // группы
    QVector<GroupInfo> groups() const;            // в порядке показа, включая пустые объявленные
    void setGroups(const QVector<GroupInfo> &g);
    void setGroupCollapsed(const QString &name, bool collapsed);
    void renameGroup(const QString &from, const QString &to);
    void removeGroup(const QString &name, bool withTargets);
    void addGroup(const QString &name);
    void moveGroup(const QString &name, int newIndex);

    // работа
    void start();
    void stop();
    bool isRunning() const { return m_running; }
    Summary summary() const;
    Summary groupSummary(const QString &group) const;

    // действующие значения с учётом общих настроек
    int effInterval(const TargetConfig &t) const;
    int effTimeout(const TargetConfig &t) const;
    int effDownAfter(const TargetConfig &t) const;
    int effUpAfter(const TargetConfig &t) const;
    int effLatencyWarn(const TargetConfig &t) const;
    double effLossWarn(const TargetConfig &t) const;
    WindowStats windowStats(const QString &id) const;
    double mos(const QString &id) const;
    qint64 nowMs() const;

    // для тестов и демо: подать результат напрямую и подменить часы
    void injectResult(const QString &id, bool ok, double rttMs, const QString &error = QString());
    void setFakeNow(qint64 ms) { m_fakeNow = ms; }

signals:
    void targetsChanged();                 // состав, порядок, группы
    void targetUpdated(const QString &id); // новый замер
    void statusChanged(const zr::StatusEvent &ev);
    void summaryChanged();

private:
    void tick();
    void dispatch(TargetConfig &t, TargetRuntime &rt, qint64 mono);
    void onResult(const QString &id, quint32 generation, const ProbeResult &r);
    void apply(const TargetConfig &t, TargetRuntime &rt, bool ok, double rttMs, const QString &error);
    void transition(const TargetConfig &t, TargetRuntime &rt, Status next, const QString &reason);
    void normalizeOrder();
    void ensureGroupDeclared(const QString &name);
    qint64 mono() const { return m_mono.elapsed(); }

    ProbeDispatcher *m_dispatcher;
    Settings m_settings;
    QVector<TargetConfig> m_targets;
    QHash<QString, TargetRuntime> m_rt;
    QVector<GroupInfo> m_groups;
    QTimer m_tick;
    QElapsedTimer m_mono;
    bool m_running = false;
    qint64 m_fakeNow = 0;
};

} // namespace zr

Q_DECLARE_METATYPE(zr::StatusEvent)
