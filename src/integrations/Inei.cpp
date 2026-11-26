#include "Inei.h"
#include "SourceReplyGuard.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkCookieJar>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QTimer>

namespace zr {

static constexpr int kRequestTimeoutMs = 15000;

static void appendPhones(ImportResult &r, const QJsonArray &phones, const QString &group, const QString &mode,
                         const QString &sourceId, const QString &keyPrefix, QSet<QString> &seen)
{
    for (const QJsonValue &v : phones) {
        const QJsonObject p = v.toObject();
        const QString number = p.value(QStringLiteral("number")).toString().trimmed();
        const QString name = p.value(QStringLiteral("name")).toString().trimmed();
        const QString ip = p.value(QStringLiteral("ip")).toString().trimmed();
        if (number.isEmpty() && ip.isEmpty())
            continue;
        // Иней не проверяет поле ip при ручном добавлении — проверяем сами
        if (!isValidIpv4(ip)) {
            r.warnings << QStringLiteral("Номер %1: нет корректного IP («%2»)").arg(number, ip);
            continue;
        }
        const QString key = keyPrefix + number;
        if (seen.contains(key + QLatin1Char('@') + ip))
            continue;
        seen.insert(key + QLatin1Char('@') + ip);
        TargetConfig t;
        t.id = newTargetId();
        t.name = name.isEmpty() ? number : QStringLiteral("%1 · %2").arg(name, number);
        t.address = ip;
        t.group = group;
        if (mode == QLatin1String("sip")) {
            t.kind = ProbeKind::Sip;
            t.port = 5060;
        } else {
            t.kind = ProbeKind::Icmp;
        }
        t.sourceId = sourceId;
        t.sourceKey = key;
        t.notes = QStringLiteral("Иней, номер %1").arg(number);
        r.targets.append(t);
    }
}

ImportResult parseIneiPhones(const QJsonDocument &doc, const QString &group, const QString &mode,
                             const QString &sourceId, const QString &keyPrefix)
{
    ImportResult r;
    QSet<QString> seen;
    GroupInfo g;
    g.name = group;
    r.groups.append(g);
    if (doc.isArray()) {
        appendPhones(r, doc.array(), group, mode, sourceId, keyPrefix, seen);   // phones_<user>.json
    } else if (doc.isObject() && doc.object().contains(QStringLiteral("phones"))) {
        appendPhones(r, doc.object().value(QStringLiteral("phones")).toArray(), group, mode, sourceId, keyPrefix, seen);
    } else if (doc.isObject() && doc.object().contains(QStringLiteral("sets"))) {
        for (const QJsonValue &v : doc.object().value(QStringLiteral("sets")).toArray()) {
            const QJsonObject s = v.toObject();
            const QString user = s.value(QStringLiteral("username")).toString();
            appendPhones(r, s.value(QStringLiteral("phones")).toArray(), group, mode, sourceId,
                         keyPrefix + user + QLatin1Char('/'), seen);
        }
    } else {
        r.error = QStringLiteral("Не похоже на карточки Инея");
    }
    return r;
}

IneiClient::IneiClient(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<zr::ImportResult>("zr::ImportResult");
}

QUrl IneiClient::normalizeUrl(const QString &text)
{
    QString t = text.trimmed();
    if (t.isEmpty())
        return QUrl();
    if (!t.contains(QStringLiteral("://")))
        t = QStringLiteral("http://") + t;
    QUrl u(t);
    if (u.port() < 0 && u.scheme() == QLatin1String("http"))
        u.setPort(8000);
    u.setPath(QString());
    u.setQuery(QString());
    return u;
}

void IneiClient::fetch(const QUrl &url, const QString &username, const QString &password, bool includeShared,
                       const QString &group, const QString &mode, const QString &sourceId)
{
    abort();
    // своя корзина cookie на каждый запуск: cookie «session» привязана к хосту без порта,
    // и два Инея на одном IP иначе перезапишут друг друга
    delete m_nam;
    m_nam = new QNetworkAccessManager(this);
    m_nam->setProxy(QNetworkProxy::NoProxy);
    m_nam->setCookieJar(new QNetworkCookieJar(m_nam));
    m_base = url;
    m_user = username;
    m_pass = password;
    m_shared = includeShared;
    m_group = group;
    m_mode = mode;
    m_sourceId = sourceId;
    m_result = ImportResult();
    m_stage = 0;
    step();
}

void IneiClient::abort()
{
    if (m_reply) {
        disconnect(m_reply, nullptr, this, nullptr);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
}

QNetworkReply *IneiClient::send(const QString &method, const QString &path, const QByteArray &body)
{
    QUrl u = m_base;
    u.setPath(path);
    QNetworkRequest req(u);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    req.setRawHeader("User-Agent", "Zarnitsa/" + appVersion().toLatin1());
    req.setRawHeader("Accept", "application/json");
    // Origin не ставим: Иней сверяет его строго с Host, а без заголовка проверка не применяется
    if (!body.isEmpty() || method == QLatin1String("POST"))
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setTransferTimeout(kRequestTimeoutMs);
    QNetworkReply *r = method == QLatin1String("POST") ? m_nam->post(req, body) : m_nam->get(req);
    guardSourceReply(r, kRequestTimeoutMs);
    m_reply = r;
    return r;
}

void IneiClient::fail(const QString &error, bool authFailed)
{
    abort();
    ImportResult r;
    r.error = error;
    r.authFailed = authFailed;
    emit finished(r);
}

void IneiClient::step()
{
    if (m_stage == 0) {
        emit progress(QStringLiteral("Вход в Иней…"));
        QJsonObject o;
        o[QStringLiteral("username")] = m_user;
        o[QStringLiteral("password")] = m_pass;
        QNetworkReply *r = send(QStringLiteral("POST"), QStringLiteral("/login"),
                                QJsonDocument(o).toJson(QJsonDocument::Compact));
        connect(r, &QNetworkReply::finished, this, [this, r]() {
            m_reply = nullptr;
            r->deleteLater();
            const int code = sourceHttpStatus(r);
            const QJsonObject body = QJsonDocument::fromJson(r->readAll()).object();
            const QString serverError = body.value(QStringLiteral("error")).toString();
            if (code == 200 && body.value(QStringLiteral("ok")).toBool()) {
                m_stage = 1;
                step();
                return;
            }
            if (code == 0)
                fail(QStringLiteral("Иней не отвечает: %1").arg(sourceReplyError(r)));
            else if (code == 401)
                fail(QStringLiteral("Неверный логин или пароль. Иней считает попытки: после 7 неудач вход с этого адреса закрывается на 15 минут."), true);
            else if (code == 429)
                fail(serverError.isEmpty() ? QStringLiteral("Иней временно закрыл вход с этого адреса") : serverError, true);
            else if (code == 403)
                fail(serverError.isEmpty() ? QStringLiteral("Иней отклонил запрос") : serverError, true);
            else
                fail(QStringLiteral("Иней ответил %1 %2").arg(code).arg(serverError));
        });
        return;
    }
    if (m_stage == 1) {
        emit progress(QStringLiteral("Читаю карточки…"));
        QNetworkReply *r = send(QStringLiteral("GET"), QStringLiteral("/phones"));
        connect(r, &QNetworkReply::finished, this, [this, r]() {
            m_reply = nullptr;
            r->deleteLater();
            const int code = sourceHttpStatus(r);
            if (code != 200) {
                fail(code == 401 ? QStringLiteral("Иней не пустил к карточкам (сессия не принята)")
                                 : QStringLiteral("Иней ответил на /phones кодом %1").arg(code), code == 401 || code == 403 || code == 429);
                return;
            }
            const QJsonDocument doc = QJsonDocument::fromJson(r->readAll());
            m_result = parseIneiPhones(doc, m_group, m_mode, m_sourceId, QString());
            if (!m_result.ok()) {
                fail(m_result.error);
                return;
            }
            m_stage = m_shared ? 2 : 3;
            step();
        });
        return;
    }
    if (m_stage == 2) {
        emit progress(QStringLiteral("Читаю общие наборы…"));
        QNetworkReply *r = send(QStringLiteral("GET"), QStringLiteral("/shared_sets"));
        connect(r, &QNetworkReply::finished, this, [this, r]() {
            m_reply = nullptr;
            r->deleteLater();
            const int code = sourceHttpStatus(r);
            if (code == 200) {
                const ImportResult shared = parseIneiPhones(QJsonDocument::fromJson(r->readAll()), m_group, m_mode,
                                                            m_sourceId, QStringLiteral("shared/"));
                QSet<QString> ips;
                for (const TargetConfig &t : m_result.targets) ips.insert(t.address);
                for (const TargetConfig &t : shared.targets)
                    if (!ips.contains(t.address)) m_result.targets.append(t);
                m_result.warnings += shared.warnings;
            } else {
                m_result.warnings << QStringLiteral("Общие наборы недоступны (код %1)").arg(code);
            }
            m_stage = 3;
            step();
        });
        return;
    }
    // stage 3 — аккуратно выходим, сессию на сервере не оставляем
    QNetworkReply *r = send(QStringLiteral("POST"), QStringLiteral("/logout"), QByteArrayLiteral("{}"));
    connect(r, &QNetworkReply::finished, this, [this, r]() {
        m_reply = nullptr;
        r->deleteLater();
        const ImportResult res = m_result;
        emit finished(res);
    });
}

} // namespace zr
