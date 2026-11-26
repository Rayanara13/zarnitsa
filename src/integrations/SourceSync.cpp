#include "SourceSync.h"

#include "core/Monitor.h"

#include <QSet>

namespace zr {

QString SyncReport::text() const
{
    QStringList parts;
    parts << QStringLiteral("новых %1").arg(added);
    if (updated) parts << QStringLiteral("обновлено %1").arg(updated);
    if (unchanged) parts << QStringLiteral("без изменений %1").arg(unchanged);
    if (missing) parts << QStringLiteral("нет в источнике %1").arg(missing);
    if (!warnings.isEmpty()) parts << QStringLiteral("предупреждений %1").arg(warnings.size());
    return parts.join(QStringLiteral(", "));
}

SyncReport mergeImport(Monitor *monitor, const QString &sourceId, const QString &group, const ImportResult &r)
{
    SyncReport rep;
    rep.warnings = r.warnings;
    QSet<QString> keys;
    QVector<TargetConfig> fresh;
    for (TargetConfig t : r.targets) {
        keys.insert(t.sourceKey);
        if (!group.isEmpty()) t.group = group;
        const TargetConfig *existing = sourceId.isEmpty() ? nullptr : monitor->findBySource(sourceId, t.sourceKey);
        if (!existing) {
            fresh.append(t);
            continue;
        }
        TargetConfig upd = *existing;
        // берём из источника только то, что источник знает; пороги и заметки человека не трогаем
        const bool changed = upd.address != t.address || upd.name != t.name || upd.port != t.port
            || upd.kind != t.kind || upd.tlsPin != t.tlsPin || upd.sourceMissing;
        if (!changed) {
            ++rep.unchanged;
            continue;
        }
        upd.address = t.address;
        upd.name = t.name;
        upd.port = t.port;
        upd.kind = t.kind;
        upd.tlsPin = t.tlsPin;
        upd.sourceMissing = false;
        monitor->updateTarget(upd);
        ++rep.updated;
    }
    if (!fresh.isEmpty()) {
        monitor->addTargets(fresh);
        rep.added = fresh.size();
    }
    if (!sourceId.isEmpty()) {
        const QVector<TargetConfig> all = monitor->targets();
        for (const TargetConfig &t : all) {
            if (t.sourceId != sourceId || keys.contains(t.sourceKey) || t.sourceMissing)
                continue;
            TargetConfig upd = t;
            upd.sourceMissing = true;
            monitor->updateTarget(upd);
            ++rep.missing;
        }
    }
    return rep;
}

} // namespace zr
