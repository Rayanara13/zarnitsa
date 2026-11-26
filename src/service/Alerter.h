// Решает, о каких сменах статуса шуметь, и выполняет внешние действия: команду и вебхук.
// Оповещения на экране (трей, всплывашки, звук) делает интерфейс по сигналу alert().
#pragma once

#include "core/Monitor.h"
#include "core/Settings.h"

#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>

namespace zr {

class Alerter : public QObject {
    Q_OBJECT
public:
    explicit Alerter(Monitor *monitor, QObject *parent = nullptr);
    void setSettings(const Settings &s) { m_s = s; }
    void setMuted(bool muted) { m_muted = muted; }
    bool isMuted() const { return m_muted; }

    static bool shouldAlert(const StatusEvent &e, const Settings &s);
    static QString expand(const QString &tpl, const StatusEvent &e);

    void runCommand(const StatusEvent &e);
    void sendWebhook(const StatusEvent &e, int attempt = 0);

signals:
    void alert(const zr::StatusEvent &e);
    void actionFailed(const QString &what);

private:
    void onStatus(const StatusEvent &e);
    void fire(const StatusEvent &e);

    Monitor *m_monitor;
    Settings m_s;
    bool m_muted = false;
    QHash<QString, QTimer *> m_delayed;   // отложенные «Нет связи» при alertMinDownSec
    QHash<QString, bool> m_firedDown;     // по какой цели уже сказали «лежит»
    QNetworkAccessManager m_nam;
};

} // namespace zr
