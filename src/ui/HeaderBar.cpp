#include "HeaderBar.h"

#include "Icons.h"
#include "Theme.h"
#include "core/Monitor.h"
#include "probes/IcmpEngine.h"
#include "service/Engine.h"

#include <QAction>
#include <QDateTime>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QToolButton>

namespace zr {

static QToolButton *makeTool(QWidget *parent, const QString &tip)
{
    auto *b = new QToolButton(parent);
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setIconSize(QSize(18, 18));
    b->setFixedSize(34, 34);
    b->setCursor(Qt::PointingHandCursor);
    return b;
}

HeaderBar::HeaderBar(Engine *engine, QWidget *parent)
    : QWidget(parent)
    , m_engine(engine)
{
    setMouseTracking(true);
    setAttribute(Qt::WA_StyledBackground, false);
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(QStringLiteral("Поиск по имени, адресу, группе"));
    m_search->setClearButtonEnabled(true);
    m_search->setFixedWidth(250);
    m_icmp = new QToolButton(this);
    m_icmp->setCursor(Qt::PointingHandCursor);
    m_icmp->setToolButtonStyle(Qt::ToolButtonTextOnly);
    connect(m_icmp, &QToolButton::clicked, this, &HeaderBar::icmpClicked);
    m_pause = makeTool(this, QStringLiteral("Пауза всех проверок"));
    m_pause->setCheckable(true);
    m_sound = makeTool(this, QStringLiteral("Звук оповещений"));
    m_sound->setCheckable(true);
    m_density = makeTool(this, QStringLiteral("Вид: плитки, компактно, таблица"));
    m_theme = makeTool(this, QStringLiteral("Светлая или тёмная тема"));
    m_wall = makeTool(this, QStringLiteral("Экран дежурного (F11)"));
    m_menu = makeTool(this, QStringLiteral("Меню"));
    m_menu->setPopupMode(QToolButton::InstantPopup);
    connect(m_engine->monitor(), &Monitor::summaryChanged, this, [this]() { update(); });
    auto *clock = new QTimer(this);
    connect(clock, &QTimer::timeout, this, [this]() { if (m_wallMode) update(); });
    clock->start(1000);
    connect(m_engine->icmp(), &IcmpEngine::modeChanged, this, &HeaderBar::updateIcmp);
    refreshIcons();
    updateIcmp();
}

QSize HeaderBar::sizeHint() const
{
    return QSize(900, m_wallMode ? 76 : 62);
}

void HeaderBar::setWall(bool on)
{
    m_wallMode = on;
    m_search->setVisible(!on);
    m_density->setVisible(!on);
    m_icmp->setVisible(!on);
    setFixedHeight(sizeHint().height());
    layoutChildren();
    update();
}

void HeaderBar::refreshIcons()
{
    const Palette &p = Theme::p();
    m_pause->setIcon(Icons::glyph(m_pause->isChecked() ? Glyph::Play : Glyph::Pause, p.muted));
    m_sound->setIcon(Icons::glyph(m_sound->isChecked() ? Glyph::Bell : Glyph::BellOff, p.muted));
    m_theme->setIcon(Icons::glyph(Theme::isDark() ? Glyph::Sun : Glyph::Moon, p.muted));
    m_wall->setIcon(Icons::glyph(Glyph::Wall, p.muted));
    m_menu->setIcon(Icons::glyph(Glyph::Menu, p.muted));
    const QString d = m_engine->settings().density;
    m_density->setIcon(Icons::glyph(d == QLatin1String("list") ? Glyph::List : (d == QLatin1String("compact") ? Glyph::Compact : Glyph::Grid), p.muted));
    if (!m_searchIcon)
        m_searchIcon = m_search->addAction(Icons::glyph(Glyph::Search, p.faint, 16), QLineEdit::LeadingPosition);
    else
        m_searchIcon->setIcon(Icons::glyph(Glyph::Search, p.faint, 16));
    updateIcmp();
}

void HeaderBar::updateIcmp()
{
    IcmpEngine *e = m_engine->icmp();
    const Palette &p = Theme::p();
    const QColor dot = m_engine->isDemo() ? p.accent : (e->isFast() ? p.ok : (e->mode() == IcmpEngine::Mode::None ? p.bad : p.warn));
    m_icmp->setText(m_engine->isDemo() ? QStringLiteral("демо-режим") : QStringLiteral("ICMP: %1").arg(e->modeTitle()));
    m_icmp->setToolTip(m_engine->isDemo() ? QStringLiteral("Синтетические данные: сеть не опрашивается") : e->modeHint());
    m_icmp->setStyleSheet(QStringLiteral("QToolButton { color: %1; border: 1px solid %2; border-radius: 13px; padding: 3px 11px; font-size: 12px; }"
                                         "QToolButton:hover { background: %3; }")
                              .arg(dot.name(), p.line.name(), p.raised.name()));
    m_icmp->adjustSize();
    layoutChildren();
}

void HeaderBar::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    layoutChildren();
}

void HeaderBar::layoutChildren()
{
    const int h = height();
    m_search->setFixedWidth(qBound(150, width() / 5, 250));
    m_icmp->setVisible(!m_wallMode && width() >= 1100);
    int x = width() - 14;
    for (QToolButton *b : {m_menu, m_wall, m_theme, m_density, m_sound, m_pause}) {
        if (b->isHidden()) continue;
        x -= b->width();
        b->move(x, (h - b->height()) / 2);
        x -= 4;
    }
    x -= 8;
    if (!m_wallMode) {
        x -= m_search->width();
        m_search->move(x, (h - m_search->sizeHint().height()) / 2);
        x -= 10;
        if (!m_icmp->isHidden()) {
            x -= m_icmp->width();
            m_icmp->move(x, (h - m_icmp->height()) / 2);
            x -= 10;
        }
    }
    m_countersRight = x;
}

void HeaderBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const Palette &pal = Theme::p();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), pal.surface);
    p.setPen(QPen(pal.line, 1));
    p.drawLine(QPointF(0, height() - 0.5), QPointF(width(), height() - 0.5));

    const int logo = m_wallMode ? 40 : 32;
    Icons::paintLogo(p, QRectF(16, (height() - logo) / 2.0, logo, logo));
    p.setPen(pal.ink);
    p.setFont(Theme::ui(m_wallMode ? 21 : 17.5, QFont::DemiBold));
    const QString title = QString::fromUtf8(kAppTitle);
    const QFontMetrics fm(p.font());
    const int tx = 16 + logo + 11;
    p.drawText(QRect(tx, 0, fm.horizontalAdvance(title) + 4, height()), Qt::AlignVCenter, title);
    int x = tx + fm.horizontalAdvance(title) + (m_wallMode ? 26 : 22);

    if (m_wallMode) {
        p.setFont(Theme::mono(26, QFont::Medium));
        p.setPen(pal.muted);
        const QString clock = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
        const QFontMetrics cfm(p.font());
        p.drawText(QRect(x, 0, cfm.horizontalAdvance(clock) + 4, height()), Qt::AlignVCenter, clock);
        x += cfm.horizontalAdvance(clock) + 28;
    }

    const Summary s = m_engine->monitor()->summary();
    const struct { Status st; int n; const char *word; } items[] = {
        {Status::Up, s.up, "в норме"}, {Status::Degraded, s.degraded, "плохо"}, {Status::Down, s.down, "нет связи"},
        {Status::Unknown, s.unknown, "нет данных"}, {Status::Paused, s.paused, "пауза"},
    };
    m_counters.clear();
    const qreal fs = m_wallMode ? 16 : 12.5;
    p.setFont(Theme::mono(fs + 1, QFont::Bold));
    const QFontMetrics nfm(p.font());
    p.setFont(Theme::ui(fs));
    const QFontMetrics wfm(p.font());
    int fullWidth = 0;
    for (const auto &it : items) {
        if (it.n == 0 && it.st != Status::Up && it.st != Status::Down) continue;
        fullWidth += 14 + 8 + nfm.horizontalAdvance(QString::number(it.n)) + 6
            + wfm.horizontalAdvance(QString::fromUtf8(it.word)) + 14 + 8;
    }
    const bool compact = x + fullWidth > m_countersRight;
    for (const auto &it : items) {
        if (it.n == 0 && it.st != Status::Up && it.st != Status::Down) continue;
        const QString num = QString::number(it.n);
        const QString word = compact ? QString() : QString::fromUtf8(it.word);
        const int w = 22 + nfm.horizontalAdvance(num) + (compact ? 10 : 6 + wfm.horizontalAdvance(word) + 14);
        if (x + w > m_countersRight) continue;
        const int ph = m_wallMode ? 36 : 28;
        const QRect pill(x, (height() - ph) / 2, w, ph);
        const bool on = m_filter.contains(it.st);
        const bool alarm = it.st == Status::Down && it.n > 0;
        p.setPen(on ? QPen(Theme::statusColor(it.st), 1.5) : QPen(pal.line, 1));
        p.setBrush(on || alarm ? Theme::statusWash(it.st) : pal.surface);
        p.drawRoundedRect(QRectF(pill).adjusted(0.5, 0.5, -0.5, -0.5), ph / 2.0, ph / 2.0);
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::statusColor(it.st));
        p.drawEllipse(QPointF(pill.left() + 14, pill.center().y() + 0.5), 4, 4);
        p.setFont(Theme::mono(fs + 1, QFont::Bold));
        p.setPen(alarm ? pal.badText : pal.ink);
        p.drawText(QRect(pill.left() + 22, pill.top(), nfm.horizontalAdvance(num) + 2, ph), Qt::AlignVCenter, num);
        p.setFont(Theme::ui(fs));
        p.setPen(alarm ? pal.badText : pal.muted);
        p.drawText(QRect(pill.left() + 22 + nfm.horizontalAdvance(num) + 6, pill.top(), wfm.horizontalAdvance(word) + 2, ph),
                   Qt::AlignVCenter, word);
        m_counters.append({it.st, pill});
        x += w + 8;
    }
}

void HeaderBar::mousePressEvent(QMouseEvent *e)
{
    for (const Counter &c : m_counters) {
        if (c.rect.contains(e->pos())) {
            emit statusFilterToggled(c.st);
            return;
        }
    }
    QWidget::mousePressEvent(e);
}

void HeaderBar::mouseMoveEvent(QMouseEvent *e)
{
    bool over = false;
    for (const Counter &c : m_counters)
        if (c.rect.contains(e->pos())) over = true;
    setCursor(over ? Qt::PointingHandCursor : Qt::ArrowCursor);
    if (over) setToolTip(QStringLiteral("Показать только эти цели"));
    else setToolTip(QString());
}

} // namespace zr
