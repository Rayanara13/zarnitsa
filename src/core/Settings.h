// Общие настройки: значения по умолчанию для целей, вид, оповещения, ICMP и API.
#pragma once

#include <QJsonObject>
#include <QString>

namespace zr {

struct Settings {
    // проверки по умолчанию
    int intervalMs = 1000;
    int timeoutMs = 1000;
    int packetSize = 56;        // как у ping в Linux
    int ttl = 64;
    int downAfter = 3;          // как в Инее: OFFLINE после трёх неудач
    int upAfter = 2;
    int latencyWarnMs = 150;
    bool latencyDynamic = false;    // «выше минимума на N мс»
    int latencyDynamicDeltaMs = 60;
    double lossWarnPct = 10.0;
    int statsWindow = 60;           // замеров в окне потерь/джиттера
    int flapChanges = 6;            // смен статуса за окно — считается дребезгом
    int flapWindowSec = 600;

    // вид
    QString theme = QStringLiteral("dark");        // dark | light
    QString density = QStringLiteral("comfortable"); // comfortable | compact | list
    QString sortMode = QStringLiteral("manual");   // manual | problems | name | rtt
    int columns = 0;                // 0 — по ширине окна
    bool alwaysOnTop = false;
    bool minimizeToTray = true;
    bool closeToTray = false;
    bool startMinimized = false;
    bool onlyProblems = false;
    bool showSparkline = true;
    bool showMos = true;

    // оповещения
    bool alertTray = true;
    bool alertToast = true;
    bool alertCorner = true;
    bool alertSound = true;
    bool alertOnDegraded = false;
    bool alertOnRecover = true;
    bool alertQuietFlapping = true;
    int alertMinDownSec = 0;        // не шуметь, если лежит меньше N секунд
    QString alertCommand;
    bool alertCommandShell = false;
    QString webhookUrl;

    // ICMP
    QString icmpMode = QStringLiteral("auto");   // auto | socket | helper | ping
    QString helperPath;
    int pingMaxParallel = 64;

    // API
    bool apiEnabled = false;
    QString apiListen = QStringLiteral("127.0.0.1:8790");
    QString apiToken;
    QString apiTlsCert;
    QString apiTlsKey;

    // журнал
    int eventsKeepMonths = 12;

    QJsonObject toJson() const;
    static Settings fromJson(const QJsonObject &o);
};

} // namespace zr
