#include "Engine.h"

#include "Alerter.h"
#include "Sound.h"
#include "api/ApiController.h"
#include "core/EventLog.h"
#include "integrations/Inei.h"
#include "integrations/Rosa.h"
#include "integrations/SourceSync.h"
#include "probes/IcmpEngine.h"
#include "probes/NetProbers.h"
#include "probes/ProbeDispatcher.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QRandomGenerator>

namespace zr {

Engine::Engine(const QString &configPath, bool demo, QObject *parent)
    : QObject(parent)
    , m_store(configPath)
    , m_demo(demo)
{
    m_dispatcher = new ProbeDispatcher(this);
    m_icmp = new IcmpEngine(this);
    m_dispatcher->setIcmpEngine(m_icmp);
    m_dispatcher->setBackend(ProbeKind::Tcp, new TcpProber(this));
    m_dispatcher->setBackend(ProbeKind::Http, new HttpProber(this));
    m_dispatcher->setBackend(ProbeKind::Sip, new SipProber(this));
    m_dispatcher->setBackend(ProbeKind::Dns, new DnsProber(this));
    m_dispatcher->setBackend(ProbeKind::Demo, new DemoProber(this));
    if (demo)
        m_dispatcher->setDemo(new DemoProber(this));

    m_monitor = new Monitor(m_dispatcher, this);
    const QString logDir = demo ? QDir::tempPath() + QStringLiteral("/zarnitsa-demo-%1").arg(QCoreApplication::applicationPid())
                                : m_store.dataDir();
    m_log = new EventLog(logDir, this);
    connect(m_monitor, &Monitor::statusChanged, this, [this](const StatusEvent &e) {
        // обычный старт и снятие с паузы в журнал не пишем — тысяча строк при каждом запуске никому не нужна
        if ((e.from == Status::Unknown && e.to == Status::Up) || (e.from == Status::Paused && e.to == Status::Unknown))
            return;
        m_log->append(e);
    });
    m_api = new ApiController(m_monitor, m_log, m_icmp, this);
    m_alerter = new Alerter(m_monitor, this);
    m_sound = new Sound(this);

    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(800);
    connect(&m_saveTimer, &QTimer::timeout, this, &Engine::save);
    connect(m_monitor, &Monitor::targetsChanged, this, &Engine::scheduleSave);
    m_syncTimer.setInterval(60000);
    connect(&m_syncTimer, &QTimer::timeout, this, &Engine::syncDue);
}

Engine::~Engine()
{
    if (m_saveTimer.isActive())
        save();
    if (m_demo)
        QDir(m_log->dir()).removeRecursively();
}

bool Engine::load(QString *err)
{
    if (m_demo) {
        m_cfg = AppConfig();
    } else if (!m_store.load(m_cfg, err)) {
        return false;
    }
    m_monitor->setSettings(m_cfg.settings);
    m_alerter->setSettings(m_cfg.settings);
    m_monitor->setGroups(m_cfg.groups);
    m_monitor->setTargets(m_cfg.targets);
    m_log->loadRecent();
    m_log->prune(m_cfg.settings.eventsKeepMonths);
    m_icmp->configure(m_cfg.settings.icmpMode, m_cfg.settings.helperPath, m_cfg.settings.pingMaxParallel);
    if (m_demo)
        loadDemo();
    return true;
}

void Engine::start()
{
    m_monitor->start();
    m_syncTimer.start();
}

void Engine::scheduleSave()
{
    if (!m_demo)
        m_saveTimer.start();
}

void Engine::save()
{
    m_saveTimer.stop();
    if (m_demo)
        return;
    m_cfg.targets = m_monitor->targets();
    m_cfg.groups = m_monitor->groups();
    QString err;
    if (!m_store.save(m_cfg, &err))
        qWarning("Не удалось сохранить конфиг: %s", qPrintable(err));
    emit saved();
}

void Engine::applySettings(const Settings &s)
{
    const Settings old = m_cfg.settings;
    m_cfg.settings = s;
    m_monitor->setSettings(s);
    m_alerter->setSettings(s);
    if (old.icmpMode != s.icmpMode || old.helperPath != s.helperPath || old.pingMaxParallel != s.pingMaxParallel)
        m_icmp->configure(s.icmpMode, s.helperPath, s.pingMaxParallel);
    const bool apiChanged = old.apiEnabled != s.apiEnabled || old.apiListen != s.apiListen
        || old.apiToken != s.apiToken || old.apiTlsCert != s.apiTlsCert || old.apiTlsKey != s.apiTlsKey;
    if (apiChanged) {
        if (s.apiEnabled) {
            QString err;
            startApi(&err);
        } else {
            stopApi();
        }
    }
    scheduleSave();
    emit settingsApplied();
}

QString Engine::ensureApiToken()
{
    if (m_cfg.settings.apiToken.size() < 16) {
        m_cfg.settings.apiToken = ApiController::generateToken();
        scheduleSave();
    }
    return m_cfg.settings.apiToken;
}

bool Engine::startApi(QString *err)
{
    ensureApiToken();
    QString e;
    const bool ok = m_api->start(m_cfg.settings, &e);
    m_apiError = ok ? QString() : e;
    if (err) *err = e;
    emit apiStateChanged();
    return ok;
}

void Engine::stopApi()
{
    m_api->stop();
    m_apiError.clear();
    emit apiStateChanged();
}

void Engine::saveSet(const QString &name, const QVector<TargetConfig> &targets)
{
    for (TargetSet &s : m_cfg.sets) {
        if (s.name == name) {
            s.targets = targets;
            scheduleSave();
            return;
        }
    }
    TargetSet s;
    s.name = name;
    s.targets = targets;
    m_cfg.sets.append(s);
    scheduleSave();
}

bool Engine::openSet(const QString &name, bool replace)
{
    for (const TargetSet &s : m_cfg.sets) {
        if (s.name != name) continue;
        QVector<TargetConfig> copy = s.targets;
        for (TargetConfig &t : copy) t.id = newTargetId();
        if (replace) m_monitor->setTargets(copy);
        else m_monitor->addTargets(copy);
        scheduleSave();
        return true;
    }
    return false;
}

void Engine::removeSet(const QString &name)
{
    for (int i = 0; i < m_cfg.sets.size(); ++i) {
        if (m_cfg.sets[i].name == name) {
            m_cfg.sets.removeAt(i);
            scheduleSave();
            return;
        }
    }
}

SourceConfig *Engine::source(const QString &id)
{
    for (SourceConfig &s : m_cfg.sources)
        if (s.id == id) return &s;
    return nullptr;
}

void Engine::upsertSource(const SourceConfig &src)
{
    SourceConfig s = src;
    if (s.id.isEmpty())
        s.id = QStringLiteral("s") + QString::number(QRandomGenerator::global()->generate64(), 36).left(10);
    cancelSourceSync(s.id);
    s.syncBlocked = false;
    if (SourceConfig *old = source(s.id)) *old = s;
    else m_cfg.sources.append(s);
    scheduleSave();
}

void Engine::cancelSourceSync(const QString &id)
{
    ++m_sourceGenerations[id];
    m_syncing.remove(id);
    QObject *client = m_syncClients.take(id);
    if (!client) return;
    client->disconnect(this);
    if (auto *c = qobject_cast<IneiClient *>(client)) c->abort();
    if (auto *c = qobject_cast<RosaClient *>(client)) c->abort();
    client->deleteLater();
}

void Engine::removeSource(QString id, bool withTargets)
{
    cancelSourceSync(id);
    for (int i = 0; i < m_cfg.sources.size(); ++i)
        if (m_cfg.sources[i].id == id) { m_cfg.sources.removeAt(i); break; }
    QStringList remove;
    for (const TargetConfig &t : m_monitor->targets()) {
        if (t.sourceId != id) continue;
        if (withTargets) {
            remove << t.id;
        } else {
            TargetConfig copy = t;
            copy.sourceId.clear();
            copy.sourceKey.clear();
            copy.sourceMissing = false;
            m_monitor->updateTarget(copy);
        }
    }
    m_monitor->removeTargets(remove);
    scheduleSave();
}

void Engine::syncSource(const QString &id, const QString &passwordOnce)
{
    SourceConfig *s = source(id);
    if (!s || m_syncing.contains(id))
        return;
    const QString pass = passwordOnce.isEmpty() ? s->password : passwordOnce;
    if (pass.isEmpty()) {
        emit sourceSynced(id, false, QStringLiteral("Нужен пароль"));
        return;
    }
    s->syncBlocked = false;
    m_syncing.insert(id);
    const quint64 generation = ++m_sourceGenerations[id];
    auto handle = [this, id, generation](const ImportResult &r) {
        if (m_sourceGenerations.value(id) != generation) return;
        m_syncing.remove(id);
        m_syncClients.remove(id);
        SourceConfig *src = source(id);
        if (!src) return;
        src->lastSync = QDateTime::currentMSecsSinceEpoch();
        if (!r.ok()) {
            src->syncBlocked = r.authFailed;
            src->lastResult = QStringLiteral("Ошибка: %1").arg(r.error);
            if (r.authFailed)
                src->lastResult += QStringLiteral(" Автосинхронизация остановлена; исправьте учётные данные и запустите вручную.");
            scheduleSave();
            emit sourceSynced(id, false, src->lastResult);
            return;
        }
        const SyncReport rep = mergeImport(m_monitor, id, src->group, r);
        src->lastResult = rep.text();
        scheduleSave();
        QString text = rep.text();
        if (!rep.warnings.isEmpty())
            text += QStringLiteral("\n") + rep.warnings.mid(0, 8).join(QStringLiteral("\n"));
        emit sourceSynced(id, true, text);
    };
    if (s->kind == QLatin1String("inei")) {
        auto *c = new IneiClient(this);
        m_syncClients.insert(id, c);
        connect(c, &IneiClient::finished, this, [c, handle](const ImportResult &r) {
            c->deleteLater();
            handle(r);
        });
        c->fetch(IneiClient::normalizeUrl(s->url), s->username, pass, s->includeShared, s->group, s->mode, s->id);
    } else {
        auto *c = new RosaClient(this);
        m_syncClients.insert(id, c);
        connect(c, &RosaClient::finished, this, [c, handle](const ImportResult &r) {
            c->deleteLater();
            handle(r);
        });
        RosaOptions o;
        o.sip = s->rosaSip;
        o.h323 = s->rosaH323;
        o.server = s->rosaServer;
        o.mode = s->mode;
        c->fetch(RosaClient::normalizeUrl(s->url), s->username, pass, s->tlsPin, o, s->group, s->id);
    }
}

void Engine::syncDue()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const SourceConfig &s : m_cfg.sources)
        if (s.autoSyncDue(now))
            syncSource(s.id);
}

void Engine::loadDemo()
{
    struct D { const char *group, *name, *addr; ProbeKind kind; int port; const char *scenario; const char *parent; bool enabled; };
    // Адреса RFC 5737 и вымышленные номера: демонстрация не содержит данных рабочей сети.
    const D items[] = {
        {"Ядро сети", "Шлюз", "192.0.2.1", ProbeKind::Icmp, 0, "ok:0.4", nullptr, true},
        {"Ядро сети", "Коммутатор ядра", "192.0.2.2", ProbeKind::Icmp, 0, "ok:0.6", nullptr, true},
        {"Ядро сети", "Иней · сервер", "http://192.0.2.10:8000/login", ProbeKind::Http, 0, "http:210", nullptr, true},
        {"Ядро сети", "Роса · ВКС", "https://192.0.2.20:18443/healthz", ProbeKind::Http, 0, "http:340", nullptr, true},
        {"Ядро сети", "Роса · SIP", "192.0.2.20", ProbeKind::Sip, 5060, "sip", nullptr, true},
        {"Офис А · Приёмная", "Приёмная · 1001", "198.51.100.21", ProbeKind::Icmp, 0, "ok:1.8", nullptr, true},
        {"Офис А · Приёмная", "Бухгалтерия · 1002", "198.51.100.22", ProbeKind::Icmp, 0, "ok:2.1", nullptr, true},
        {"Офис А · Приёмная", "Кабинет · 1010", "198.51.100.30", ProbeKind::Sip, 5060, "sip", nullptr, true},
        {"Офис А · Приёмная", "Склад · 1015", "198.51.100.35", ProbeKind::Icmp, 0, "down", nullptr, true},
        {"Офис А · Приёмная", "Переговорная · 1020", "198.51.100.40", ProbeKind::Icmp, 0, "flap:20", nullptr, true},
        {"Офис А · Приёмная", "Принтер", "198.51.100.130", ProbeKind::Icmp, 0, "ok:0.9", nullptr, false},
        {"Филиал Б", "Канал до филиала", "203.0.113.1", ProbeKind::Icmp, 0, "slow:185", nullptr, true},
        {"Филиал Б", "Телефон · 2001", "203.0.113.84", ProbeKind::Icmp, 0, "lossy:35", nullptr, true},
        {"Филиал Б", "Видеотерминал H.323", "203.0.113.90", ProbeKind::Tcp, 1720, "ok:3", nullptr, true},
        {"Филиал Б", "Камера на входе", "203.0.113.120", ProbeKind::Icmp, 0, "recover:25", nullptr, true},
        {"Филиал Б", "Радиомост до склада", "203.0.113.200", ProbeKind::Icmp, 0, "jitter:45", nullptr, true},
        {"Офис В", "Канал до офиса В", "192.0.2.129", ProbeKind::Icmp, 0, "down", nullptr, true},
        {"Офис В", "Телефон · 3001", "192.0.2.130", ProbeKind::Icmp, 0, "down", "Канал до офиса В", true},
        {"Офис В", "Телефон · 3002", "192.0.2.131", ProbeKind::Icmp, 0, "down", "Канал до офиса В", true},
    };
    QVector<TargetConfig> list;
    QHash<QString, QString> idByName;
    for (const D &d : items) {
        TargetConfig t;
        t.id = newTargetId();
        t.group = QString::fromUtf8(d.group);
        t.name = QString::fromUtf8(d.name);
        t.address = QString::fromUtf8(d.addr);
        t.kind = d.kind;
        t.port = d.port;
        t.demoScenario = QString::fromUtf8(d.scenario);
        t.enabled = d.enabled;
        idByName.insert(t.name, t.id);
        list.append(t);
    }
    for (int i = 0; i < list.size(); ++i)
        if (items[i].parent) list[i].parentId = idByName.value(QString::fromUtf8(items[i].parent));
    m_monitor->setGroups({});
    m_monitor->setTargets(list);

    // предыстория за 10 минут, чтобы графики и «лежит 7 мин» выглядели как на живом посту
    m_alerter->setMuted(true);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    auto *rng = QRandomGenerator::global();
    for (int sec = -600; sec <= 0; ++sec) {
        m_monitor->setFakeNow(now + qint64(sec) * 1000);
        for (const TargetConfig &t : list) {
            if (!t.enabled) continue;
            const DemoProber::Sim s = DemoProber::simulate(t.demoScenario, sec, rng->generateDouble(), rng->generateDouble());
            m_monitor->injectResult(t.id, s.ok, s.rtt, s.ok ? QString() : QStringLiteral("Таймаут"));
        }
    }
    m_monitor->setFakeNow(0);
    m_alerter->setMuted(false);
}

} // namespace zr
