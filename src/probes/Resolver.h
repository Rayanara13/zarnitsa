// Разрешение имён с кэшем: у заказчика DNS часто нет, поэтому IP-литералы обходят его совсем.
#pragma once

#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QString>
#include <QVector>
#include <functional>

namespace zr {

class Resolver : public QObject {
    Q_OBJECT
public:
    using Callback = std::function<void(const QHostAddress &addr, const QString &error)>;

    explicit Resolver(QObject *parent = nullptr);
    // IPv4 предпочтительнее: ICMP-помощник и SIP работают по IPv4
    void resolve(const QString &host, Callback cb);
    void clear();

private:
    struct Entry {
        QHostAddress addr;
        QString error;
        qint64 expires = 0;
    };
    QHash<QString, Entry> m_cache;
    QHash<QString, QVector<Callback>> m_waiting;
};

} // namespace zr
