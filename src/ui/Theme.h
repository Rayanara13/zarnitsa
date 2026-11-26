// Оформление Зарницы: светлая и тёмная темы, шрифты Golos Text и JetBrains Mono, общая таблица стилей.
// Цвета статусов те же, что в Инее; акцент свой — сиреневый «грозовой».
#pragma once

#include "core/Types.h"

#include <QColor>
#include <QFont>
#include <QString>

class QApplication;

namespace zr {

struct Palette {
    bool dark = true;
    QColor canvas, surface, tile, raised, line, lineStrong;
    QColor ink, muted, faint;
    QColor accent, accentStrong, accentWash, onAccent;
    QColor ok, okText, okWash;
    QColor warn, warnText, warnWash;
    QColor bad, badText, badWash;
    QColor unknown, unknownText, unknownWash;
    QColor paused;
    QColor shadow;
};

class Theme {
public:
    static void init();                         // шрифты из ресурсов
    static void apply(QApplication *app, const QString &name);
    static const Palette &p();
    static bool isDark();
    static QString name();

    static QFont ui(qreal px, int weight = QFont::Normal);
    static QFont mono(qreal px, int weight = QFont::Normal);
    static QString uiFamily();
    static QString monoFamily();

    static QColor statusColor(Status s);
    static QColor statusText(Status s);
    static QColor statusWash(Status s);

    static QString styleSheet();
};

QString fmtMs(double ms);                 // «0,42», «3,4», «185»
QString fmtPct(double pct);               // «0», «2,5», «35»
QString fmtDuration(qint64 ms);           // «12 с», «7 мин», «2 ч 14 мин»
QString fmtNum(double v, int prec);

} // namespace zr
