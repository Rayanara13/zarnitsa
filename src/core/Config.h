// Конфиг на диске: настройки, группы, цели, наборы, источники Инея/Росы. Атомарная запись, права 0600.
#pragma once

#include "Monitor.h"
#include "Settings.h"
#include "Target.h"

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace zr {

struct SourceConfig {
    QString id;
    QString kind;            // inei | rosa
    QString title;
    QString url;
    QString username;
    QString password;        // в основной конфиг не пишется — только в secrets.json, если remember
    bool remember = false;
    QString group;           // куда складывать цели
    QString mode = QStringLiteral("icmp");   // Иней: icmp | sip
    bool includeShared = false;               // Иней: общие наборы других пользователей
    bool rosaSip = true;
    bool rosaH323 = true;
    bool rosaServer = true;
    QString tlsPin;          // SHA-256 сертификата Росы
    int syncMinutes = 0;     // 0 — только по кнопке
    bool syncBlocked = false;   // ошибка входа: до ручного запуска или правки источника
    qint64 lastSync = 0;
    QString lastResult;

    bool autoSyncDue(qint64 now) const;
    QJsonObject toJson() const;
    static SourceConfig fromJson(const QJsonObject &o);
};

struct TargetSet {
    QString name;
    QVector<TargetConfig> targets;
};

struct AppConfig {
    Settings settings;
    QVector<GroupInfo> groups;
    QVector<TargetConfig> targets;
    QVector<TargetSet> sets;
    QVector<SourceConfig> sources;
    QJsonObject ui;          // геометрия окна и прочее состояние интерфейса
};

class ConfigStore {
public:
    // explicitPath — из --config; иначе переносной zarnitsa.json рядом с программой или ~/.config/zarnitsa/
    explicit ConfigStore(const QString &explicitPath = QString());

    QString path() const { return m_path; }
    QString dir() const;
    QString dataDir() const { return m_dataDir; }
    bool isPortable() const { return m_portable; }

    bool load(AppConfig &out, QString *err) const;
    bool save(const AppConfig &cfg, QString *err) const;

    static QJsonObject toJson(const AppConfig &cfg);
    static AppConfig fromJson(const QJsonObject &o);

private:
    QString secretsPath() const;
    QString m_path;
    QString m_dataDir;
    bool m_portable = false;
};

bool writeFileAtomic(const QString &path, const QByteArray &data, bool privateMode, QString *err);

} // namespace zr
