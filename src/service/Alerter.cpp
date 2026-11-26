#include "Alerter.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>

namespace zr {

Alerter::Alerter(Monitor *monitor, QObject *parent)
    : QObject(parent)
    , m_monitor(monitor)
{
    m_nam.setProxy(QNetworkProxy::NoProxy);
    connect(m_monitor, &Monitor::statusChanged, this, &Alerter::onStatus);
}

bool Alerter::shouldAlert(const StatusEvent &e, const Settings &s)
{
    if (e.to == Status::Paused || e.from == Status::Paused || e.to == Status::Unknown)
        return false;
    if (e.from == Status::Unknown && e.to == Status::Up)
        return false;                      // обычный старт — не новость
    if (e.dependent)
        return false;                      // лежит родитель — о нём уже сказали
    if (s.alertQuietFlapping && e.flapping)
        return false;
    if (e.to == Status::Down)
        return true;
    if (e.to == Status::Degraded)
        return s.alertOnDegraded;
    if (e.to == Status::Up && e.from == Status::Down)
        return s.alertOnRecover;
    if (e.to == Status::Up && e.from == Status::Degraded)
        return s.alertOnDegraded && s.alertOnRecover;
    return false;
}

QString Alerter::expand(const QString &tpl, const StatusEvent &e)
{
    const QHash<QString, QString> fields = {
        {QStringLiteral("name"), e.name},
        {QStringLiteral("address"), e.address},
        {QStringLiteral("group"), e.group},
        {QStringLiteral("status"), statusKey(e.to)},
        {QStringLiteral("status_title"), statusTitle(e.to)},
        {QStringLiteral("prev"), statusKey(e.from)},
        {QStringLiteral("rtt"), e.rttMs >= 0 ? QString::number(e.rttMs, 'f', 1) : QString()},
        {QStringLiteral("loss"), QString::number(e.lossPct, 'f', 1)},
        {QStringLiteral("reason"), e.reason},
        {QStringLiteral("text"), e.text()},
        {QStringLiteral("time"), QDateTime::fromMSecsSinceEpoch(e.t).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))},
    };
    static const QRegularExpression placeholder(QStringLiteral(
        "\\{(name|address|group|status|status_title|prev|rtt|loss|reason|text|time)\\}"));
    QString out;
    out.reserve(tpl.size());
    int pos = 0;
    auto matches = placeholder.globalMatch(tpl);
    while (matches.hasNext()) {
        const auto match = matches.next();
        out += tpl.mid(pos, match.capturedStart() - pos);
        out += fields.value(match.captured(1));
        pos = match.capturedEnd();
    }
    // Подставленные данные не разбираем повторно: {reason} в имени остаётся частью имени.
    out += tpl.mid(pos);
    return out;
}

void Alerter::onStatus(const StatusEvent &e)
{
    // восстановление до истечения отсрочки — о падении и подъёме молчим
    if (QTimer *t = m_delayed.take(e.id)) {
        t->stop();
        t->deleteLater();
        if (e.to != Status::Down)
            return;
    }
    if (!shouldAlert(e, m_s))
        return;
    if (e.to == Status::Up && !m_firedDown.value(e.id) && e.from == Status::Down && m_s.alertMinDownSec > 0)
        return;   // о падении не сообщали — и о подъёме не надо
    if (e.to == Status::Down && m_s.alertMinDownSec > 0) {
        auto *t = new QTimer(this);
        t->setSingleShot(true);
        const StatusEvent copy = e;
        connect(t, &QTimer::timeout, this, [this, copy, t]() {
            m_delayed.remove(copy.id);
            t->deleteLater();
            const TargetRuntime *rt = m_monitor->runtime(copy.id);
            if (rt && rt->status == Status::Down)
                fire(copy);
        });
        m_delayed.insert(e.id, t);
        t->start(m_s.alertMinDownSec * 1000);
        return;
    }
    fire(e);
}

void Alerter::fire(const StatusEvent &e)
{
    m_firedDown[e.id] = e.to == Status::Down;
    if (m_muted)
        return;
    emit alert(e);
    if (!m_s.alertCommand.trimmed().isEmpty())
        runCommand(e);
    if (!m_s.webhookUrl.trimmed().isEmpty())
        sendWebhook(e);
}

void Alerter::runCommand(const StatusEvent &e)
{
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("ZARNITSA_NAME"), e.name);
    env.insert(QStringLiteral("ZARNITSA_ADDRESS"), e.address);
    env.insert(QStringLiteral("ZARNITSA_GROUP"), e.group);
    env.insert(QStringLiteral("ZARNITSA_STATUS"), statusKey(e.to));
    env.insert(QStringLiteral("ZARNITSA_PREV"), statusKey(e.from));
    env.insert(QStringLiteral("ZARNITSA_TEXT"), e.text());
    env.insert(QStringLiteral("ZARNITSA_RTT"), e.rttMs >= 0 ? QString::number(e.rttMs, 'f', 1) : QString());
    env.insert(QStringLiteral("ZARNITSA_LOSS"), QString::number(e.lossPct, 'f', 1));
    env.insert(QStringLiteral("ZARNITSA_REASON"), e.reason);
    env.insert(QStringLiteral("ZARNITSA_TIME"), QDateTime::fromMSecsSinceEpoch(e.t).toString(Qt::ISODate));
    p.setProcessEnvironment(env);
    if (m_s.alertCommandShell) {
        // Скрипт доверенный и буквальный; данные доступны только через ZARNITSA_*.
        p.setProgram(QStringLiteral("/bin/sh"));
        p.setArguments({QStringLiteral("-c"), m_s.alertCommand});
    } else {
        // Сначала отделяем программу и argv, иначе кавычки из имени меняют аргументы.
        QStringList parts = QProcess::splitCommand(m_s.alertCommand);
        if (parts.isEmpty()) return;
        p.setProgram(parts.takeFirst());   // путь к программе не зависит от данных события
        for (QString &arg : parts)
            arg = expand(arg, e);
        p.setArguments(parts);
    }
    p.setStandardOutputFile(QProcess::nullDevice());
    p.setStandardErrorFile(QProcess::nullDevice());
    if (!p.startDetached())
        emit actionFailed(QStringLiteral("Команда оповещения не запустилась: %1").arg(p.program()));
}

void Alerter::sendWebhook(const StatusEvent &e, int attempt)
{
    QNetworkRequest req{QUrl(m_s.webhookUrl.trimmed())};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("User-Agent", "Zarnitsa/" + appVersion().toLatin1());
    req.setTransferTimeout(10000);
    QJsonObject o;
    o[QStringLiteral("app")] = QStringLiteral("zarnitsa");
    o[QStringLiteral("text")] = QStringLiteral("%1 — %2").arg(e.name, e.text());
    o[QStringLiteral("event")] = e.toJson();
    QNetworkReply *r = m_nam.post(req, QJsonDocument(o).toJson(QJsonDocument::Compact));
    connect(r, &QNetworkReply::finished, this, [this, r, e, attempt]() {
        r->deleteLater();
        const int code = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (code >= 200 && code < 300)
            return;
        static const int delays[] = {1000, 5000, 15000};
        if (attempt < 3) {
            QTimer::singleShot(delays[attempt], this, [this, e, attempt]() { sendWebhook(e, attempt + 1); });
        } else {
            emit actionFailed(QStringLiteral("Вебхук не принят: %1").arg(code ? QString::number(code) : r->errorString()));
        }
    });
}

} // namespace zr
