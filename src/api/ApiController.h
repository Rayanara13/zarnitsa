// API Зарницы в стиле Росы: /api/v1, ошибки {"error":"код"}, Bearer-токен, /healthz, SSE и формат Инея.
#pragma once

#include "HttpServer.h"

#include "core/Monitor.h"
#include "core/Settings.h"

#include <QJsonObject>
#include <QObject>

namespace zr {

class EventLog;
class IcmpEngine;

class ApiController : public QObject {
    Q_OBJECT
public:
    ApiController(Monitor *monitor, EventLog *log, IcmpEngine *icmp, QObject *parent = nullptr);

    bool start(const Settings &s, QString *err);
    void stop();
    bool isRunning() const { return m_server && m_server->isListening(); }
    QString address() const { return m_address; }
    quint16 port() const;
    void setToken(const QString &t) { m_token = t.toLatin1(); }

    static QString generateToken();
    static bool parseListen(const QString &text, QHostAddress *addr, quint16 *port);

    QJsonObject targetJson(const TargetConfig &t, bool detail) const;
    QByteArray availabilityJson() const;
    QByteArray metricsText() const;

signals:
    void targetsAddedByApi(int count);

private:
    void handle(const HttpRequest &req, HttpConnection *conn);
    bool authorized(const HttpRequest &req) const;
    void json(HttpConnection *c, int status, const QJsonValue &v);
    void error(HttpConnection *c, int status, const char *code);
    void onStatus(const StatusEvent &ev);

    Monitor *m_monitor;
    EventLog *m_log;
    IcmpEngine *m_icmp;
    HttpServer *m_server = nullptr;
    QByteArray m_token;
    QString m_address;
    quint64 m_revision = 1;
};

} // namespace zr
