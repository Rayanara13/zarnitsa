// Сборка всех частей Зарницы: конфиг, исполнители, монитор, журнал, API, оповещения, источники.
// Одинаково служит и окну, и режиму --headless.
#pragma once

#include "core/Config.h"
#include "core/Monitor.h"

#include <QObject>
#include <QSet>
#include <QTimer>
#include <functional>

namespace zr {

class ProbeDispatcher;
class IcmpEngine;
class EventLog;
class ApiController;
class Alerter;
class Sound;

class Engine : public QObject {
    Q_OBJECT
public:
    explicit Engine(const QString &configPath, bool demo, QObject *parent = nullptr);
    ~Engine() override;

    bool load(QString *err);
    void save();
    void scheduleSave();
    void start();

    ConfigStore &store() { return m_store; }
    AppConfig &config() { return m_cfg; }
    const Settings &settings() const { return m_cfg.settings; }
    void applySettings(const Settings &s);

    Monitor *monitor() const { return m_monitor; }
    ProbeDispatcher *dispatcher() const { return m_dispatcher; }
    IcmpEngine *icmp() const { return m_icmp; }
    EventLog *log() const { return m_log; }
    ApiController *api() const { return m_api; }
    Alerter *alerter() const { return m_alerter; }
    Sound *sound() const { return m_sound; }
    bool isDemo() const { return m_demo; }

    bool startApi(QString *err);
    void stopApi();
    QString apiError() const { return m_apiError; }
    QString ensureApiToken();

    // наборы — аналог «Избранного» vmPing
    void saveSet(const QString &name, const QVector<TargetConfig> &targets);
    bool openSet(const QString &name, bool replace);
    void removeSet(const QString &name);

    // источники Инея и Росы
    SourceConfig *source(const QString &id);
    void upsertSource(const SourceConfig &s);
    void removeSource(QString id, bool withTargets);
    void syncSource(const QString &id, const QString &passwordOnce = QString());

    void loadDemo();

signals:
    void sourceSynced(const QString &id, bool ok, const QString &report);
    void apiStateChanged();
    void settingsApplied();
    void saved();

private:
    void syncDue();
    void cancelSourceSync(const QString &id);

    ConfigStore m_store;
    AppConfig m_cfg;
    bool m_demo;
    ProbeDispatcher *m_dispatcher;
    IcmpEngine *m_icmp;
    Monitor *m_monitor;
    EventLog *m_log;
    ApiController *m_api;
    Alerter *m_alerter;
    Sound *m_sound;
    QTimer m_saveTimer;
    QTimer m_syncTimer;
    QString m_apiError;
    QSet<QString> m_syncing;
    QHash<QString, quint64> m_sourceGenerations;
    QHash<QString, QObject *> m_syncClients;
};

} // namespace zr
