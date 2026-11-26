// Импорт и экспорт списков: TXT в духе vmPing, CSV, vmPing.xml, файлы Инея, собственный JSON.
#pragma once

#include "core/Monitor.h"
#include "core/Target.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace zr {

struct ImportResult {
    QVector<TargetConfig> targets;
    QVector<GroupInfo> groups;
    QStringList warnings;
    QString error;
    bool authFailed = false;
    bool ok() const { return error.isEmpty(); }
};

ImportResult parseTextList(const QString &text, const QString &defaultGroup);
ImportResult parseCsv(const QString &text, const QString &defaultGroup);
ImportResult parseVmPingXml(const QByteArray &xml);
ImportResult parseZarnitsaJson(const QByteArray &json);
// Определяет формат по расширению и содержимому
ImportResult importFile(const QString &path, const QString &defaultGroup);

QString exportCsv(const QVector<TargetConfig> &targets);
QByteArray exportJson(const QVector<TargetConfig> &targets, const QVector<GroupInfo> &groups);
QString exportTextList(const QVector<TargetConfig> &targets);

} // namespace zr
