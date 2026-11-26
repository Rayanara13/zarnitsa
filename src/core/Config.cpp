#include "Config.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

namespace zr {

static constexpr int kSchemaVersion = 1;

bool SourceConfig::autoSyncDue(qint64 now) const
{
    return !syncBlocked && syncMinutes > 0 && !password.isEmpty()
        && now - lastSync >= qint64(syncMinutes) * 60000;
}

QJsonObject SourceConfig::toJson() const
{
    QJsonObject o;
    o[QStringLiteral("id")] = id;
    o[QStringLiteral("kind")] = kind;
    o[QStringLiteral("title")] = title;
    o[QStringLiteral("url")] = url;
    o[QStringLiteral("username")] = username;
    o[QStringLiteral("remember")] = remember;
    o[QStringLiteral("group")] = group;
    o[QStringLiteral("mode")] = mode;
    o[QStringLiteral("include_shared")] = includeShared;
    o[QStringLiteral("rosa_sip")] = rosaSip;
    o[QStringLiteral("rosa_h323")] = rosaH323;
    o[QStringLiteral("rosa_server")] = rosaServer;
    o[QStringLiteral("tls_pin")] = tlsPin;
    o[QStringLiteral("sync_minutes")] = syncMinutes;
    o[QStringLiteral("sync_blocked")] = syncBlocked;
    o[QStringLiteral("last_sync")] = double(lastSync);
    o[QStringLiteral("last_result")] = lastResult;
    return o;
}

SourceConfig SourceConfig::fromJson(const QJsonObject &o)
{
    SourceConfig s;
    s.id = o.value(QStringLiteral("id")).toString();
    s.kind = o.value(QStringLiteral("kind")).toString();
    s.title = o.value(QStringLiteral("title")).toString();
    s.url = o.value(QStringLiteral("url")).toString();
    s.username = o.value(QStringLiteral("username")).toString();
    s.remember = o.value(QStringLiteral("remember")).toBool();
    s.group = o.value(QStringLiteral("group")).toString();
    s.mode = o.value(QStringLiteral("mode")).toString(QStringLiteral("icmp"));
    s.includeShared = o.value(QStringLiteral("include_shared")).toBool();
    s.rosaSip = o.value(QStringLiteral("rosa_sip")).toBool(true);
    s.rosaH323 = o.value(QStringLiteral("rosa_h323")).toBool(true);
    s.rosaServer = o.value(QStringLiteral("rosa_server")).toBool(true);
    s.tlsPin = o.value(QStringLiteral("tls_pin")).toString();
    s.syncMinutes = qBound(0, o.value(QStringLiteral("sync_minutes")).toInt(), 10080);
    s.syncBlocked = o.value(QStringLiteral("sync_blocked")).toBool();
    s.lastSync = qint64(o.value(QStringLiteral("last_sync")).toDouble());
    s.lastResult = o.value(QStringLiteral("last_result")).toString();
    return s;
}

bool writeFileAtomic(const QString &path, const QByteArray &data, bool privateMode, QString *err)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (err) *err = f.errorString();
        return false;
    }
    if (privateMode && !f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        if (err) *err = QStringLiteral("Не удалось ограничить права файла: %1").arg(path);
        f.cancelWriting();
        return false;
    }
    if (f.write(data) != data.size()) {
        if (err) *err = f.errorString();
        f.cancelWriting();
        return false;
    }
    if (!f.commit()) {
        if (err) *err = f.errorString();
        return false;
    }
    if (privateMode && !QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        if (err) *err = QStringLiteral("Не удалось ограничить права файла: %1").arg(path);
        return false;
    }
    return true;
}

ConfigStore::ConfigStore(const QString &explicitPath)
{
    const QString appDir = QCoreApplication::applicationDirPath();
    if (!explicitPath.isEmpty()) {
        m_path = QFileInfo(explicitPath).absoluteFilePath();
        m_dataDir = QFileInfo(m_path).absolutePath() + QStringLiteral("/zarnitsa-data");
        m_portable = true;
    } else if (QFileInfo::exists(appDir + QStringLiteral("/zarnitsa.json"))) {
        m_path = appDir + QStringLiteral("/zarnitsa.json");
        m_dataDir = appDir + QStringLiteral("/zarnitsa-data");
        m_portable = true;
    } else {
        QString cfg = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
        if (cfg.isEmpty()) cfg = QDir::homePath() + QStringLiteral("/.config");
        QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
        if (data.isEmpty()) data = QDir::homePath() + QStringLiteral("/.local/share");
        m_path = cfg + QStringLiteral("/zarnitsa/zarnitsa.json");
        m_dataDir = data + QStringLiteral("/zarnitsa");
    }
}

QString ConfigStore::dir() const
{
    return QFileInfo(m_path).absolutePath();
}

QString ConfigStore::secretsPath() const
{
    return dir() + QStringLiteral("/secrets.json");
}

QJsonObject ConfigStore::toJson(const AppConfig &cfg)
{
    QJsonObject o;
    o[QStringLiteral("version")] = kSchemaVersion;
    o[QStringLiteral("app")] = QStringLiteral("zarnitsa");
    o[QStringLiteral("settings")] = cfg.settings.toJson();
    QJsonArray groups;
    for (const GroupInfo &g : cfg.groups) {
        QJsonObject go;
        go[QStringLiteral("name")] = g.name;
        if (g.collapsed) go[QStringLiteral("collapsed")] = true;
        groups.append(go);
    }
    o[QStringLiteral("groups")] = groups;
    QJsonArray targets;
    for (const TargetConfig &t : cfg.targets)
        targets.append(t.toJson());
    o[QStringLiteral("targets")] = targets;
    QJsonArray sets;
    for (const TargetSet &s : cfg.sets) {
        QJsonObject so;
        so[QStringLiteral("name")] = s.name;
        QJsonArray st;
        for (const TargetConfig &t : s.targets) st.append(t.toJson());
        so[QStringLiteral("targets")] = st;
        sets.append(so);
    }
    o[QStringLiteral("sets")] = sets;
    QJsonArray sources;
    for (const SourceConfig &s : cfg.sources)
        sources.append(s.toJson());
    o[QStringLiteral("sources")] = sources;
    o[QStringLiteral("ui")] = cfg.ui;
    return o;
}

AppConfig ConfigStore::fromJson(const QJsonObject &o)
{
    AppConfig c;
    c.settings = Settings::fromJson(o.value(QStringLiteral("settings")).toObject());
    for (const QJsonValue &v : o.value(QStringLiteral("groups")).toArray()) {
        const QJsonObject go = v.toObject();
        GroupInfo g;
        g.name = go.value(QStringLiteral("name")).toString();
        g.collapsed = go.value(QStringLiteral("collapsed")).toBool();
        c.groups.append(g);
    }
    for (const QJsonValue &v : o.value(QStringLiteral("targets")).toArray()) {
        const TargetConfig t = TargetConfig::fromJson(v.toObject());
        if (!t.address.isEmpty())
            c.targets.append(t);
    }
    for (const QJsonValue &v : o.value(QStringLiteral("sets")).toArray()) {
        const QJsonObject so = v.toObject();
        TargetSet s;
        s.name = so.value(QStringLiteral("name")).toString();
        for (const QJsonValue &tv : so.value(QStringLiteral("targets")).toArray())
            s.targets.append(TargetConfig::fromJson(tv.toObject()));
        c.sets.append(s);
    }
    for (const QJsonValue &v : o.value(QStringLiteral("sources")).toArray())
        c.sources.append(SourceConfig::fromJson(v.toObject()));
    c.ui = o.value(QStringLiteral("ui")).toObject();
    return c;
}

static bool readSecretsFile(const QString &path, QJsonObject &out, QString *err)
{
    QFile file(path);
    if (!file.exists()) return true;
    if (!file.open(QIODevice::ReadOnly)) {
        if (err) *err = QStringLiteral("Не удалось прочитать файл паролей %1: %2").arg(path, file.errorString());
        return false;
    }
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        if (err) *err = QStringLiteral("Файл паролей повреждён: %1. Сохраните его копию перед восстановлением.").arg(path);
        return false;
    }
    out = doc.object();
    for (auto it = out.constBegin(); it != out.constEnd(); ++it) {
        if (!it.value().isString()) {
            if (err) *err = QStringLiteral("Неверная запись в файле паролей: %1").arg(path);
            return false;
        }
    }
    return true;
}

bool ConfigStore::load(AppConfig &out, QString *err) const
{
    QJsonObject secrets;
    if (!readSecretsFile(secretsPath(), secrets, err)) return false;
    QFile f(m_path);
    if (!f.exists()) {
        if (!secrets.isEmpty()) {
            if (err) *err = QStringLiteral("Конфиг отсутствует, но рядом остался файл паролей. Восстановите конфиг из копии: %1").arg(m_path);
            return false;
        }
        out = AppConfig();
        return true;
    }
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = f.errorString();
        return false;
    }
    QJsonParseError pe{};
    const QJsonDocument d = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !d.isObject()) {
        if (err) *err = QStringLiteral("Конфиг повреждён: %1").arg(pe.errorString());
        return false;
    }
    const QJsonObject root = d.object();
    if (root.value(QStringLiteral("app")).toString() != QLatin1String("zarnitsa")
        || root.value(QStringLiteral("version")).toDouble(-1) != kSchemaVersion) {
        if (err) *err = QStringLiteral("Неизвестный формат или версия конфига; файл не изменён: %1").arg(m_path);
        return false;
    }
    out = fromJson(root);
    for (SourceConfig &s : out.sources)
        if (s.remember)
            s.password = secrets.value(s.id).toString();
    return true;
}

bool ConfigStore::save(const AppConfig &cfg, QString *err) const
{
    // Не затираем повреждённый/недоступный файл паролей даже при позднем изменении на диске.
    QJsonObject previousSecrets;
    if (!readSecretsFile(secretsPath(), previousSecrets, err)) return false;
    const QByteArray data = QJsonDocument(toJson(cfg)).toJson(QJsonDocument::Indented);
    if (!writeFileAtomic(m_path, data, true, err))
        return false;
    QJsonObject secrets;
    for (const SourceConfig &s : cfg.sources)
        if (s.remember && !s.password.isEmpty())
            secrets[s.id] = s.password;
    if (secrets.isEmpty()) {
        QFile::remove(secretsPath());
        return true;
    }
    return writeFileAtomic(secretsPath(), QJsonDocument(secrets).toJson(QJsonDocument::Compact), true, err);
}

} // namespace zr
