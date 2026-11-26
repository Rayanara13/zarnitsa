// Слияние импорта из источника с текущими целями: новое добавить, изменённое подтянуть,
// пропавшее пометить «нет в источнике», но не удалять — человек решает сам.
#pragma once

#include "Importers.h"

namespace zr {

class Monitor;

struct SyncReport {
    int added = 0;
    int updated = 0;
    int unchanged = 0;
    int missing = 0;
    QStringList warnings;
    QString text() const;
};

SyncReport mergeImport(Monitor *monitor, const QString &sourceId, const QString &group, const ImportResult &r);

} // namespace zr
