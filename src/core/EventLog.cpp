#include "EventLog.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTextStream>

namespace zr {

static constexpr int kMemoryEvents = 20000;

EventLog::EventLog(const QString &dir, QObject *parent)
    : QObject(parent)
    , m_dir(dir)
{
    QDir().mkpath(m_dir);
}

QString EventLog::fileFor(qint64 t) const
{
    const QDateTime dt = QDateTime::fromMSecsSinceEpoch(t);
    return m_dir + QStringLiteral("/events-%1.jsonl").arg(dt.toString(QStringLiteral("yyyy-MM")));
}

void EventLog::append(const StatusEvent &e)
{
    m_recent.append(e);
    if (m_recent.size() > kMemoryEvents)
        m_recent.remove(0, m_recent.size() - kMemoryEvents);
    QFile f(fileFor(e.t));
    if (f.open(QIODevice::WriteOnly | QIODevice::Append)) {
        f.write(QJsonDocument(e.toJson()).toJson(QJsonDocument::Compact));
        f.write("\n");
    }
    emit appended(e);
}

QVector<StatusEvent> EventLog::forTarget(const QString &id, int max) const
{
    QVector<StatusEvent> out;
    for (int i = m_recent.size() - 1; i >= 0 && out.size() < max; --i)
        if (m_recent[i].id == id)
            out.prepend(m_recent[i]);
    return out;
}

QVector<StatusEvent> EventLog::since(qint64 t, int max) const
{
    QVector<StatusEvent> out;
    for (int i = m_recent.size() - 1; i >= 0 && out.size() < max; --i) {
        if (m_recent[i].t <= t) break;
        out.prepend(m_recent[i]);
    }
    return out;
}

void EventLog::loadRecent()
{
    m_recent.clear();
    const QDateTime now = QDateTime::currentDateTime();
    // прошлый и текущий месяц — этого хватает окну журнала
    const QStringList files{fileFor(now.addMonths(-1).toMSecsSinceEpoch()), fileFor(now.toMSecsSinceEpoch())};
    for (const QString &path : files) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        while (!f.atEnd()) {
            const QByteArray line = f.readLine().trimmed();
            if (line.isEmpty()) continue;
            const QJsonDocument d = QJsonDocument::fromJson(line);
            if (!d.isObject()) continue;
            m_recent.append(StatusEvent::fromJson(d.object()));
        }
    }
    if (m_recent.size() > kMemoryEvents)
        m_recent.remove(0, m_recent.size() - kMemoryEvents);
}

void EventLog::prune(int keepMonths)
{
    const QString cutoff = QDateTime::currentDateTime().addMonths(-keepMonths).toString(QStringLiteral("yyyy-MM"));
    const QDir d(m_dir);
    const QStringList files = d.entryList({QStringLiteral("events-*.jsonl")}, QDir::Files);
    for (const QString &name : files) {
        const QString ym = name.mid(7, 7);
        if (ym < cutoff)
            QFile::remove(d.filePath(name));
    }
}

bool EventLog::exportCsv(const QVector<StatusEvent> &events, const QString &path, QChar sep, QString *err)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (err) *err = f.errorString();
        return false;
    }
    QTextStream out(&f);
    out.setCodec("UTF-8");
    out.setGenerateByteOrderMark(true);   // Excel и LibreOffice сразу понимают кириллицу
    auto q = [sep](QString s) {
        if (s.contains(sep) || s.contains(QLatin1Char('"')) || s.contains(QLatin1Char('\n'))) {
            s.replace(QLatin1Char('"'), QStringLiteral("\"\""));
            return QStringLiteral("\"%1\"").arg(s);
        }
        return s;
    };
    const QStringList head{QStringLiteral("Время"), QStringLiteral("Цель"), QStringLiteral("Адрес"),
                           QStringLiteral("Группа"), QStringLiteral("Было"), QStringLiteral("Стало"),
                           QStringLiteral("Задержка, мс"), QStringLiteral("Потери, %"), QStringLiteral("Причина")};
    out << head.join(sep) << "\r\n";
    for (const StatusEvent &e : events) {
        QStringList row;
        row << QDateTime::fromMSecsSinceEpoch(e.t).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
            << q(e.name) << q(e.address) << q(e.group) << statusTitle(e.from) << statusTitle(e.to)
            << (e.rttMs >= 0 ? QString::number(e.rttMs, 'f', 2) : QString())
            << QString::number(e.lossPct, 'f', 1) << q(e.reason);
        out << row.join(sep) << "\r\n";
    }
    out.flush();
    if (!f.commit()) {
        if (err) *err = f.errorString();
        return false;
    }
    return true;
}

} // namespace zr
