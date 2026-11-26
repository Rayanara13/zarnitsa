// Описание цели наблюдения и разбор строки ввода «как в vmPing».
#pragma once

#include "Types.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace zr {

struct TargetConfig {
    QString id;
    QString name;          // подпись на плитке; пусто — показываем адрес
    QString address;       // IP, имя хоста или URL для HTTP
    ProbeKind kind = ProbeKind::Icmp;
    int port = 0;          // TCP/SIP
    QString group;
    int order = 0;         // порядок внутри группы
    bool enabled = true;   // false — пауза

    // 0 или отрицательное — взять общее значение из настроек
    int intervalMs = 0;
    int timeoutMs = 0;
    int packetSize = 0;
    int ttl = 0;
    bool dontFragment = false;
    int downAfter = 0;
    int upAfter = 0;
    int latencyWarnMs = 0;
    double lossWarnPct = -1.0;

    QString parentId;
    QString notes;
    QString demoScenario;   // только --demo: «ok:0.4», «down», «flap:25»…

    // HTTP(S)
    int httpExpect = 0;     // 0 — любой 2xx/3xx
    QString httpMatch;      // подстрока в теле ответа
    bool tlsInsecure = false;
    QString tlsPin;         // SHA-256 сертификата, hex без двоеточий

    // откуда пришла цель при импорте
    QString sourceId;       // id источника (Иней/Роса), пусто — добавлена вручную
    QString sourceKey;      // устойчивый ключ внутри источника
    bool sourceMissing = false;

    QString displayName() const;
    QString endpointText() const;   // «10.0.0.1:5060», URL и т.п.
    QString publicEndpointText() const;   // без userinfo, значений query и фрагмента URL
    bool isNetworkHost() const;     // есть ли у цели хост для ICMP/трассировки
    QString hostPart() const;       // хост без порта и схемы

    QJsonObject toJson() const;
    static TargetConfig fromJson(const QJsonObject &o);
};

QString newTargetId();

// Разбор строки из поля «добавить» и строк списков vmPing:
//   10.0.0.1 · host:80 · https://host/path · sip:host[:port] · dns:name · tcp:host:port
//   хвост «# подпись» становится именем цели
struct ParsedInput {
    bool ok = false;
    QString error;
    TargetConfig target;
};
ParsedInput parseTargetInput(const QString &line);

// Готовые шаблоны для диалога добавления
struct TargetPreset {
    QString key;
    QString title;
    ProbeKind kind;
    int port;
    QString pathOrScheme;   // для HTTP: «http://{host}:8000/login»
};
QVector<TargetPreset> targetPresets();
TargetConfig applyPreset(const TargetPreset &p, const QString &host, const QString &name);

bool isValidIpv4(const QString &s);
bool isValidHostName(const QString &s);

} // namespace zr
