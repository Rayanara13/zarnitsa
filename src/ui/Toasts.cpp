#include "Toasts.h"

#include "Icons.h"
#include "Theme.h"

#include <QApplication>
#include <QDateTime>
#include <QDesktopWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QTimer>

namespace zr {

static constexpr int kToastW = 360;
static constexpr int kToastH = 66;
static constexpr int kToastLife = 7000;

static void paintCard(QPainter &p, const QRectF &r, const StatusEvent &e, qreal opacity)
{
    const Palette &pal = Theme::p();
    p.setOpacity(opacity);
    QPainterPath path;
    path.addRoundedRect(r, 10, 10);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, pal.dark ? 90 : 30));
    p.drawRoundedRect(r.translated(0, 3), 10, 10);
    p.fillPath(path, pal.raised);
    p.fillPath(path, Theme::statusWash(e.to));
    p.save();
    p.setClipPath(path);
    p.fillRect(QRectF(r.left(), r.top(), 4, r.height()), Theme::statusColor(e.to));
    p.restore();
    p.setPen(QPen(pal.lineStrong, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r, 10, 10);
    p.setFont(Theme::ui(13.5, QFont::DemiBold));
    p.setPen(pal.ink);
    const QFontMetrics fm(p.font());
    const QString time = QDateTime::fromMSecsSinceEpoch(e.t).toString(QStringLiteral("HH:mm:ss"));
    p.drawText(QRectF(r.left() + 16, r.top() + 9, r.width() - 90, 20), Qt::AlignLeft | Qt::AlignVCenter,
               fm.elidedText(e.name, Qt::ElideRight, int(r.width() - 90)));
    p.setFont(Theme::mono(11));
    p.setPen(pal.faint);
    p.drawText(QRectF(r.right() - 72, r.top() + 9, 60, 20), Qt::AlignRight | Qt::AlignVCenter, time);
    p.setFont(Theme::ui(12));
    p.setPen(Theme::statusText(e.to));
    const QFontMetrics fm2(p.font());
    p.drawText(QRectF(r.left() + 16, r.top() + 33, r.width() - 28, 20), Qt::AlignLeft | Qt::AlignVCenter,
               fm2.elidedText(QStringLiteral("%1 · %2").arg(e.text(), e.address), Qt::ElideRight, int(r.width() - 28)));
    p.setOpacity(1.0);
}

ToastHost::ToastHost(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setAttribute(Qt::WA_NoSystemBackground);
    m_timer = new QTimer(this);
    m_timer->setInterval(50);
    connect(m_timer, &QTimer::timeout, this, [this]() {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (int i = m_toasts.size() - 1; i >= 0; --i)
            if (now - m_toasts[i].born > kToastLife) m_toasts.removeAt(i);
        if (m_toasts.isEmpty()) {
            m_timer->stop();
            hide();
        }
        update();
    });
    hide();
}

void ToastHost::push(const StatusEvent &e)
{
    m_toasts.prepend({e, QDateTime::currentMSecsSinceEpoch()});
    while (m_toasts.size() > 4) m_toasts.removeLast();
    reposition();
    show();
    raise();
    if (!m_timer->isActive()) m_timer->start();
    update();
}

void ToastHost::reposition()
{
    if (!parentWidget()) return;
    const int h = m_toasts.size() * (kToastH + 8) + 8;
    setGeometry(parentWidget()->width() - kToastW - 20, 8, kToastW + 8, h);
}

void ToastHost::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (int i = 0; i < m_toasts.size(); ++i) {
        const qint64 age = now - m_toasts[i].born;
        const qreal in = qMin<qreal>(1.0, age / 180.0);
        const qreal out = age > kToastLife - 400 ? qMax<qreal>(0.0, (kToastLife - age) / 400.0) : 1.0;
        const QRectF r(4 + (1.0 - in) * 30, 4 + i * (kToastH + 8), kToastW, kToastH);
        paintCard(p, r, m_toasts[i].e, qMin(in, out));
    }
}

void ToastHost::mousePressEvent(QMouseEvent *e)
{
    const int i = (e->pos().y() - 4) / (kToastH + 8);
    if (i >= 0 && i < m_toasts.size()) {
        emit clicked(m_toasts[i].e.id);
        m_toasts.removeAt(i);
        reposition();
        update();
    }
}

CornerPopup::CornerPopup()
    : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setWindowTitle(QStringLiteral("Зарница — оповещения"));
    m_hide = new QTimer(this);
    m_hide->setSingleShot(true);
    connect(m_hide, &QTimer::timeout, this, &QWidget::hide);
}

void CornerPopup::push(const StatusEvent &e)
{
    m_events.prepend(e);
    while (m_events.size() > 5) m_events.removeLast();
    place();
    show();
    raise();
    update();
    m_hide->start(10000);
}

void CornerPopup::place()
{
    QScreen *s = QApplication::primaryScreen();
    const QRect avail = s ? s->availableGeometry() : QRect(0, 0, 1280, 800);
    const int h = 34 + m_events.size() * (kToastH + 8);
    const int w = kToastW + 16;
    setGeometry(avail.right() - w - 12, avail.bottom() - h - 12, w, h);
}

void CornerPopup::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const Palette &pal = Theme::p();
    Icons::paintLogo(p, QRectF(10, 6, 20, 20));
    p.setFont(Theme::ui(12, QFont::DemiBold));
    p.setPen(pal.ink);
    // подложка под заголовок, чтобы читался на любом фоне рабочего стола
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(pal.surface.red(), pal.surface.green(), pal.surface.blue(), 230));
    p.drawRoundedRect(QRectF(6, 3, 130, 26), 8, 8);
    Icons::paintLogo(p, QRectF(10, 6, 20, 20));
    p.setPen(pal.ink);
    p.drawText(QRectF(36, 3, 100, 26), Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("Зарница"));
    for (int i = 0; i < m_events.size(); ++i)
        paintCard(p, QRectF(8, 34 + i * (kToastH + 8), kToastW, kToastH), m_events[i], 1.0);
}

void CornerPopup::mousePressEvent(QMouseEvent *e)
{
    const int i = (e->pos().y() - 34) / (kToastH + 8);
    if (e->button() == Qt::LeftButton && i >= 0 && i < m_events.size())
        emit clicked(m_events[i].id);
    hide();
}

void CornerPopup::enterEvent(QEvent *)
{
    m_hide->stop();
}

void CornerPopup::leaveEvent(QEvent *)
{
    m_hide->start(4000);
}

} // namespace zr
