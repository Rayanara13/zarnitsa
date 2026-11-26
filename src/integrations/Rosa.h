// Связка с Росой: абоненты SIP и H.323 по API администратора, сам сервер — /healthz и SIP OPTIONS.
// TLS у Росы самоподписанный, поэтому сертификат закрепляем по отпечатку SHA-256 при первом подключении.
#pragma once

#include "Importers.h"

#include <QDateTime>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QUrl>

class QNetworkReply;

namespace zr {

struct RosaOptions {
    bool sip = true;
    bool h323 = true;
    bool server = true;
    QString mode = QStringLiteral("icmp");   // абонентов: icmp | sip
};

ImportResult parseRosaSipStatus(const QJsonObject &status, const QString &group, const QString &mode,
                                const QString &sourceId, QString *serverSipAddress);
ImportResult parseRosaH323Status(const QJsonObject &status, const QString &group, const QString &mode,
                                 const QString &sourceId);
QVector<TargetConfig> rosaServerTargets(const QUrl &base, const QString &tlsPin, const QString &sipAddress,
                                        const QString &group, const QString &sourceId);

class RosaClient : public QObject {
    Q_OBJECT
public:
    explicit RosaClient(QObject *parent = nullptr);

    static QUrl normalizeUrl(const QString &text);
    // Первое подключение: показать сертификат и отпечаток, чтобы человек его подтвердил
    void probeCertificate(const QUrl &url);
    void fetch(const QUrl &url, const QString &username, const QString &password, const QString &pin,
               const RosaOptions &opts, const QString &group, const QString &sourceId);
    void abort();

signals:
    void certificate(const QString &sha256, const QString &subject, const QDateTime &expiry, bool systemTrusted,
                     const QString &error);
    void finished(const zr::ImportResult &r);
    void progress(const QString &text);

private:
    QNetworkReply *send(const QByteArray &verb, const QString &path, const QByteArray &body = QByteArray());
    void fail(const QString &error, bool authFailed = false);
    void step();
    bool checkPin(QNetworkReply *r);

    QNetworkAccessManager m_nam;
    QUrl m_base;
    QString m_user, m_pass, m_pin, m_group, m_sourceId, m_token, m_sipAddress;
    RosaOptions m_opts;
    int m_stage = 0;
    ImportResult m_result;
    QNetworkReply *m_reply = nullptr;
};

} // namespace zr
