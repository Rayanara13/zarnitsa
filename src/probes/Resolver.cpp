#include "Resolver.h"

#include <QDateTime>
#include <QHostInfo>

namespace zr {

static constexpr qint64 kPositiveTtlMs = 300000;
static constexpr qint64 kNegativeTtlMs = 30000;

Resolver::Resolver(QObject *parent)
    : QObject(parent)
{
}

void Resolver::clear()
{
    m_cache.clear();
}

void Resolver::resolve(const QString &hostIn, Callback cb)
{
    const QString host = hostIn.trimmed();
    QHostAddress literal;
    if (literal.setAddress(host)) {
        cb(literal, QString());
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto it = m_cache.constFind(host.toLower());
    if (it != m_cache.constEnd() && it->expires > now) {
        cb(it->addr, it->error);
        return;
    }
    const QString key = host.toLower();
    auto &queue = m_waiting[key];
    queue.append(std::move(cb));
    if (queue.size() > 1)
        return;   // запрос уже в пути

    QHostInfo::lookupHost(host, this, [this, key](const QHostInfo &info) {
        Entry e;
        const qint64 now2 = QDateTime::currentMSecsSinceEpoch();
        if (info.error() != QHostInfo::NoError || info.addresses().isEmpty()) {
            e.error = QStringLiteral("Имя не найдено");
            e.expires = now2 + kNegativeTtlMs;
        } else {
            e.addr = info.addresses().first();
            for (const QHostAddress &a : info.addresses()) {
                if (a.protocol() == QAbstractSocket::IPv4Protocol) { e.addr = a; break; }
            }
            e.expires = now2 + kPositiveTtlMs;
        }
        m_cache.insert(key, e);
        const QVector<Callback> waiting = m_waiting.take(key);
        for (const Callback &c : waiting)
            c(e.addr, e.error);
    });
}

} // namespace zr
