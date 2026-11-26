// Связка с Инеем: карточки телефонов по сети (POST /login → GET /phones) или из phones_<user>.json.
#pragma once

#include "Importers.h"

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QObject>
#include <QUrl>

class QNetworkReply;

namespace zr {

// mode: icmp — пинговать телефоны; sip — SIP OPTIONS на 5060
ImportResult parseIneiPhones(const QJsonDocument &doc, const QString &group, const QString &mode,
                             const QString &sourceId, const QString &keyPrefix);

class IneiClient : public QObject {
    Q_OBJECT
public:
    explicit IneiClient(QObject *parent = nullptr);

    // url — адрес панели, например http://10.0.0.5:8000
    void fetch(const QUrl &url, const QString &username, const QString &password, bool includeShared,
               const QString &group, const QString &mode, const QString &sourceId);
    void abort();

    static QUrl normalizeUrl(const QString &text);

signals:
    void finished(const zr::ImportResult &r);
    void progress(const QString &text);

private:
    QNetworkReply *send(const QString &method, const QString &path, const QByteArray &body = QByteArray());
    void fail(const QString &error, bool authFailed = false);
    void step();

    QNetworkAccessManager *m_nam = nullptr;
    QUrl m_base;
    QString m_user, m_pass, m_group, m_mode, m_sourceId;
    bool m_shared = false;
    int m_stage = 0;
    ImportResult m_result;
    QNetworkReply *m_reply = nullptr;
};

} // namespace zr

Q_DECLARE_METATYPE(zr::ImportResult)
