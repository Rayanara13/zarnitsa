// Журнал смен статусов: JSONL по месяцам и свежая часть в памяти для окна F12 и API.
#pragma once

#include "Monitor.h"

#include <QObject>
#include <QVector>

namespace zr {

class EventLog : public QObject {
    Q_OBJECT
public:
    explicit EventLog(const QString &dir, QObject *parent = nullptr);

    void append(const StatusEvent &e);
    const QVector<StatusEvent> &recent() const { return m_recent; }
    QVector<StatusEvent> forTarget(const QString &id, int max) const;
    QVector<StatusEvent> since(qint64 t, int max) const;
    void loadRecent();
    void prune(int keepMonths);
    QString dir() const { return m_dir; }
    void clearMemory() { m_recent.clear(); }

    static bool exportCsv(const QVector<StatusEvent> &events, const QString &path, QChar sep, QString *err);

signals:
    void appended(const zr::StatusEvent &e);

private:
    QString fileFor(qint64 t) const;
    QString m_dir;
    QVector<StatusEvent> m_recent;
};

} // namespace zr
