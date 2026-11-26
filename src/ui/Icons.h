// Значки рисуются кодом (без QtSvg): логотип-молния над горизонтом и набор контурных глифов.
#pragma once

#include "core/Types.h"

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>

class QPainter;
class QRectF;

namespace zr {

enum class Glyph {
    Plus, Search, Menu, Play, Pause, Moon, Sun, Wall, Bell, BellOff, Grid, Compact, List,
    Trash, Edit, Copy, Route, Radar, Import, Export, Close, Check, Warning, Link, Bolt, Globe, Clock, Sort
};

class Icons {
public:
    // Логотип; dot — цвет точки состояния в углу (для трея), невалидный — без точки
    static QPixmap logo(int size, qreal dpr = 1.0, const QColor &dot = QColor());
    static QIcon appIcon(const QColor &dot = QColor());
    static void paintLogo(QPainter &p, const QRectF &r);
    static QIcon glyph(Glyph g, const QColor &color, int size = 18);
    static void paintGlyph(QPainter &p, Glyph g, const QRectF &r, const QColor &color, qreal stroke = 1.7);
};

} // namespace zr
