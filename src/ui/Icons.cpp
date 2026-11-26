#include "Icons.h"

#include <QGuiApplication>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QtMath>

namespace zr {

void Icons::paintLogo(QPainter &p, const QRectF &r)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    const qreal s = r.width();
    const qreal rad = s * 0.24;
    // ночное небо
    QLinearGradient sky(r.topLeft(), r.bottomLeft());
    sky.setColorAt(0.0, QColor(0x1c, 0x17, 0x45));
    sky.setColorAt(0.62, QColor(0x2c, 0x22, 0x66));
    sky.setColorAt(1.0, QColor(0x46, 0x2a, 0x6e));
    QPainterPath box;
    box.addRoundedRect(r, rad, rad);
    p.fillPath(box, sky);
    p.setClipPath(box);
    // зарево у горизонта
    QRadialGradient glow(QPointF(r.center().x(), r.top() + s * 0.84), s * 0.62);
    glow.setColorAt(0.0, QColor(0xff, 0xa8, 0x6b, 150));
    glow.setColorAt(0.45, QColor(0xc0, 0x6c, 0xd8, 70));
    glow.setColorAt(1.0, QColor(0x2c, 0x22, 0x66, 0));
    p.fillRect(r, glow);
    // горизонт
    const qreal hy = r.top() + s * 0.80;
    p.setPen(QPen(QColor(0xf3, 0xd9, 0xff, 120), qMax<qreal>(1.0, s * 0.03), Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(r.left() + s * 0.12, hy), QPointF(r.right() - s * 0.12, hy));
    // молния
    QPainterPath bolt;
    auto pt = [&r, s](qreal x, qreal y) { return QPointF(r.left() + x * s, r.top() + y * s); };
    bolt.moveTo(pt(0.585, 0.12));
    bolt.lineTo(pt(0.315, 0.50));
    bolt.lineTo(pt(0.485, 0.50));
    bolt.lineTo(pt(0.395, 0.86));
    bolt.lineTo(pt(0.705, 0.42));
    bolt.lineTo(pt(0.525, 0.42));
    bolt.lineTo(pt(0.615, 0.12));
    bolt.closeSubpath();
    for (int i = 3; i >= 1; --i) {
        p.setPen(QPen(QColor(0xc9, 0xb8, 0xff, 34 + 10 * (3 - i)), s * 0.035 * i, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(bolt);
    }
    QLinearGradient bg(pt(0.5, 0.12), pt(0.5, 0.86));
    bg.setColorAt(0.0, QColor(0xff, 0xff, 0xff));
    bg.setColorAt(0.55, QColor(0xe2, 0xd9, 0xff));
    bg.setColorAt(1.0, QColor(0xb4, 0xa2, 0xff));
    p.setPen(Qt::NoPen);
    p.fillPath(bolt, bg);
    p.restore();
}

QPixmap Icons::logo(int size, qreal dpr, const QColor &dot)
{
    QPixmap pm(qRound(size * dpr), qRound(size * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    paintLogo(p, QRectF(0, 0, size, size));
    if (dot.isValid()) {
        p.setRenderHint(QPainter::Antialiasing, true);
        const qreal d = size * 0.42;
        const QRectF dr(size - d - size * 0.02, size - d - size * 0.02, d, d);
        p.setPen(QPen(QColor(0x0f, 0x11, 0x17), qMax<qreal>(1.0, size * 0.07)));
        p.setBrush(dot);
        p.drawEllipse(dr);
    }
    return pm;
}

QIcon Icons::appIcon(const QColor &dot)
{
    QIcon icon;
    for (int s : {16, 22, 24, 32, 48, 64, 128, 256})
        icon.addPixmap(logo(s, 1.0, dot));
    return icon;
}

void Icons::paintGlyph(QPainter &p, Glyph g, const QRectF &r, const QColor &c, qreal stroke)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    const qreal s = qMin(r.width(), r.height()) / 24.0;   // рисуем в сетке 24×24, как Lucide
    p.translate(r.center().x() - 12 * s, r.center().y() - 12 * s);
    p.scale(s, s);
    // толщина задана в единицах сетки 24×24: при 18 px выходит ~1,4 px, как у Lucide
    p.setPen(QPen(c, stroke * 1.12, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    QPainterPath path;
    switch (g) {
    case Glyph::Plus:
        p.drawLine(QPointF(12, 5), QPointF(12, 19));
        p.drawLine(QPointF(5, 12), QPointF(19, 12));
        break;
    case Glyph::Search:
        p.drawEllipse(QPointF(10.5, 10.5), 6.5, 6.5);
        p.drawLine(QPointF(15.5, 15.5), QPointF(20, 20));
        break;
    case Glyph::Menu:
        p.setBrush(c);
        p.setPen(Qt::NoPen);
        for (qreal x : {5.5, 12.0, 18.5}) p.drawEllipse(QPointF(x, 12), 1.8, 1.8);
        break;
    case Glyph::Play:
        path.moveTo(7, 5); path.lineTo(19, 12); path.lineTo(7, 19); path.closeSubpath();
        p.drawPath(path);
        break;
    case Glyph::Pause:
        p.drawLine(QPointF(9, 5.5), QPointF(9, 18.5));
        p.drawLine(QPointF(15, 5.5), QPointF(15, 18.5));
        break;
    case Glyph::Moon: {
        QPainterPath disk, bite;
        disk.addEllipse(QPointF(11.5, 12.5), 8, 8);
        bite.addEllipse(QPointF(16.5, 8.0), 6.8, 6.8);
        p.drawPath(disk.subtracted(bite));
        break;
    }
    case Glyph::Sun:
        p.drawEllipse(QPointF(12, 12), 4, 4);
        for (int i = 0; i < 8; ++i) {
            const qreal a = i * M_PI / 4.0;
            p.drawLine(QPointF(12 + 7 * qCos(a), 12 + 7 * qSin(a)), QPointF(12 + 9.5 * qCos(a), 12 + 9.5 * qSin(a)));
        }
        break;
    case Glyph::Wall:
        p.drawRoundedRect(QRectF(3, 4.5, 18, 12), 2, 2);
        p.drawLine(QPointF(8, 20), QPointF(16, 20));
        p.drawLine(QPointF(12, 16.5), QPointF(12, 20));
        break;
    case Glyph::Bell:
    case Glyph::BellOff:
        path.moveTo(6, 16.5);
        path.lineTo(6, 10.5);
        path.cubicTo(QPointF(6, 7), QPointF(8.7, 4.5), QPointF(12, 4.5));
        path.cubicTo(QPointF(15.3, 4.5), QPointF(18, 7), QPointF(18, 10.5));
        path.lineTo(18, 16.5);
        path.lineTo(19.5, 17.5);
        path.lineTo(4.5, 17.5);
        path.closeSubpath();
        p.drawPath(path);
        p.drawLine(QPointF(10.2, 20.2), QPointF(13.8, 20.2));
        if (g == Glyph::BellOff) p.drawLine(QPointF(3.5, 3.5), QPointF(20.5, 20.5));
        break;
    case Glyph::Grid:
        for (qreal x : {4.0, 13.0})
            for (qreal y : {4.0, 13.0}) p.drawRoundedRect(QRectF(x, y, 7, 7), 1.5, 1.5);
        break;
    case Glyph::Compact:
        for (qreal y : {4.5, 10.5, 16.5}) {
            p.drawRoundedRect(QRectF(3.5, y, 7.5, 3.5), 1, 1);
            p.drawRoundedRect(QRectF(13, y, 7.5, 3.5), 1, 1);
        }
        break;
    case Glyph::List:
        for (qreal y : {6.0, 12.0, 18.0}) {
            p.drawLine(QPointF(9, y), QPointF(20, y));
            p.drawPoint(QPointF(4.5, y));
        }
        break;
    case Glyph::Trash:
        p.drawLine(QPointF(4, 6.5), QPointF(20, 6.5));
        p.drawLine(QPointF(9.5, 6.5), QPointF(10, 4));
        p.drawLine(QPointF(10, 4), QPointF(14, 4));
        p.drawLine(QPointF(14, 4), QPointF(14.5, 6.5));
        path.moveTo(6, 6.5); path.lineTo(7, 20); path.lineTo(17, 20); path.lineTo(18, 6.5);
        p.drawPath(path);
        break;
    case Glyph::Edit:
        path.moveTo(15.5, 4.5); path.lineTo(19.5, 8.5); path.lineTo(8.5, 19.5); path.lineTo(4.5, 19.5);
        path.lineTo(4.5, 15.5); path.closeSubpath();
        p.drawPath(path);
        p.drawLine(QPointF(13, 7), QPointF(17, 11));
        break;
    case Glyph::Copy:
        p.drawRoundedRect(QRectF(8.5, 8.5, 11.5, 11.5), 2, 2);
        path.moveTo(15.5, 8.5); path.lineTo(15.5, 5.5); path.quadTo(15.5, 4, 14, 4);
        path.lineTo(5.5, 4); path.quadTo(4, 4, 4, 5.5); path.lineTo(4, 14); path.quadTo(4, 15.5, 5.5, 15.5);
        path.lineTo(8.5, 15.5);
        p.drawPath(path);
        break;
    case Glyph::Route:
        p.drawEllipse(QPointF(6, 18), 2.2, 2.2);
        p.drawEllipse(QPointF(18, 6), 2.2, 2.2);
        path.moveTo(8.2, 18); path.lineTo(15, 18); path.quadTo(18.5, 18, 18.5, 14.5); path.quadTo(18.5, 11.5, 15, 11.5);
        path.lineTo(9, 11.5); path.quadTo(5.5, 11.5, 5.5, 8.5); path.quadTo(5.5, 6, 9, 6); path.lineTo(15.8, 6);
        p.drawPath(path);
        break;
    case Glyph::Radar:
        p.drawEllipse(QPointF(12, 12), 8.5, 8.5);
        p.drawEllipse(QPointF(12, 12), 4.5, 4.5);
        p.drawLine(QPointF(12, 12), QPointF(18, 6));
        break;
    case Glyph::Import:
        p.drawLine(QPointF(12, 3.5), QPointF(12, 14.5));
        p.drawPolyline(QPolygonF({QPointF(7.5, 10.5), QPointF(12, 15), QPointF(16.5, 10.5)}));
        p.drawPolyline(QPolygonF({QPointF(4.5, 15.5), QPointF(4.5, 20), QPointF(19.5, 20), QPointF(19.5, 15.5)}));
        break;
    case Glyph::Export:
        p.drawLine(QPointF(12, 15), QPointF(12, 4));
        p.drawPolyline(QPolygonF({QPointF(7.5, 8.5), QPointF(12, 4), QPointF(16.5, 8.5)}));
        p.drawPolyline(QPolygonF({QPointF(4.5, 15.5), QPointF(4.5, 20), QPointF(19.5, 20), QPointF(19.5, 15.5)}));
        break;
    case Glyph::Close:
        p.drawLine(QPointF(6, 6), QPointF(18, 18));
        p.drawLine(QPointF(18, 6), QPointF(6, 18));
        break;
    case Glyph::Check:
        p.drawPolyline(QPolygonF({QPointF(5, 12.5), QPointF(10, 17.5), QPointF(19, 7)}));
        break;
    case Glyph::Warning:
        path.moveTo(12, 3.8); path.lineTo(21, 19.5); path.lineTo(3, 19.5); path.closeSubpath();
        p.drawPath(path);
        p.drawLine(QPointF(12, 9.5), QPointF(12, 13.5));
        p.drawPoint(QPointF(12, 16.6));
        break;
    case Glyph::Link:
        p.drawRoundedRect(QRectF(3, 8.5, 10, 7), 3.5, 3.5);
        p.drawRoundedRect(QRectF(11, 8.5, 10, 7), 3.5, 3.5);
        break;
    case Glyph::Bolt:
        path.moveTo(13.5, 3); path.lineTo(5.5, 13.5); path.lineTo(11.5, 13.5); path.lineTo(10.5, 21);
        path.lineTo(18.5, 10.5); path.lineTo(12.5, 10.5); path.closeSubpath();
        p.drawPath(path);
        break;
    case Glyph::Globe:
        p.drawEllipse(QPointF(12, 12), 8.5, 8.5);
        p.drawEllipse(QRectF(8, 3.5, 8, 17));
        p.drawLine(QPointF(3.5, 12), QPointF(20.5, 12));
        break;
    case Glyph::Clock:
        p.drawEllipse(QPointF(12, 12), 8.5, 8.5);
        p.drawPolyline(QPolygonF({QPointF(12, 7), QPointF(12, 12), QPointF(15.5, 14)}));
        break;
    case Glyph::Sort:
        p.drawLine(QPointF(7, 5), QPointF(7, 19));
        p.drawPolyline(QPolygonF({QPointF(4, 16), QPointF(7, 19), QPointF(10, 16)}));
        p.drawLine(QPointF(13, 7), QPointF(20, 7));
        p.drawLine(QPointF(13, 12), QPointF(18, 12));
        p.drawLine(QPointF(13, 17), QPointF(16, 17));
        break;
    }
    p.restore();
}

QIcon Icons::glyph(Glyph g, const QColor &color, int size)
{
    QIcon icon;
    const qreal dpr = qMax<qreal>(1.0, qApp ? qApp->devicePixelRatio() : 1.0);
    for (int sz : {size, size * 2}) {
        QPixmap pm(qRound(sz * dpr), qRound(sz * dpr));
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        paintGlyph(p, g, QRectF(0, 0, sz, sz), color);
        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}

} // namespace zr
