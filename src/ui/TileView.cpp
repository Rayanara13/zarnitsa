#include "TileView.h"

#include "Icons.h"
#include "Theme.h"
#include "core/Monitor.h"
#include "core/Stats.h"
#include "service/Engine.h"

#include <QApplication>
#include <QDateTime>
#include <QDrag>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QRegularExpression>
#include <QScrollBar>
#include <QToolTip>
#include <algorithm>

namespace zr {

static const char *kMime = "application/x-zarnitsa-ids";
static constexpr qint64 kFlashMs = 1500;

static QColor mix(const QColor &a, const QColor &b, qreal t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t, a.alphaF() + (b.alphaF() - a.alphaF()) * t);
}

static QColor alpha(QColor c, qreal a)
{
    c.setAlphaF(qBound<qreal>(0.0, a, 1.0));
    return c;
}

TileView::Density TileView::densityFromKey(const QString &k)
{
    if (k == QLatin1String("compact")) return Density::Compact;
    if (k == QLatin1String("list")) return Density::List;
    return Density::Comfortable;
}

QString TileView::densityKey(Density d)
{
    switch (d) {
    case Density::Compact: return QStringLiteral("compact");
    case Density::List: return QStringLiteral("list");
    default: return QStringLiteral("comfortable");
    }
}

TileView::Sort TileView::sortFromKey(const QString &k)
{
    if (k == QLatin1String("problems")) return Sort::Problems;
    if (k == QLatin1String("name")) return Sort::Name;
    if (k == QLatin1String("rtt")) return Sort::Rtt;
    return Sort::Manual;
}

QString TileView::sortKey(Sort s)
{
    switch (s) {
    case Sort::Problems: return QStringLiteral("problems");
    case Sort::Name: return QStringLiteral("name");
    case Sort::Rtt: return QStringLiteral("rtt");
    default: return QStringLiteral("manual");
    }
}

TileView::TileView(Engine *engine, QWidget *parent)
    : QAbstractScrollArea(parent)
    , m_engine(engine)
    , m_mon(engine->monitor())
{
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    viewport()->setMouseTracking(true);
    viewport()->setAcceptDrops(true);
    setAcceptDrops(true);
    setFocusPolicy(Qt::StrongFocus);
    verticalScrollBar()->setSingleStep(40);

    m_relayoutTimer.setSingleShot(true);
    m_relayoutTimer.setInterval(0);
    connect(&m_relayoutTimer, &QTimer::timeout, this, &TileView::relayout);
    m_repaintTimer.setSingleShot(true);
    m_repaintTimer.setInterval(80);
    connect(&m_repaintTimer, &QTimer::timeout, this, &TileView::flushDirty);
    m_animTimer.setInterval(33);
    connect(&m_animTimer, &QTimer::timeout, this, [this]() {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (auto it = m_flash.begin(); it != m_flash.end();) {
            const auto idx = m_itemIndex.constFind(it.key());
            if (idx != m_itemIndex.constEnd())
                viewport()->update(toViewport(m_items[*idx].rect.adjusted(-4, -4, 4, 4)));
            if (now - it.value() > kFlashMs) it = m_flash.erase(it);
            else ++it;
        }
        if (m_flash.isEmpty()) m_animTimer.stop();
    });
    m_secondTimer.setInterval(1000);
    connect(&m_secondTimer, &QTimer::timeout, this, [this]() {
        viewport()->update();   // «лежит N мин» и возраст замеров
        if (m_sort == Sort::Rtt) scheduleRelayout();
    });
    m_secondTimer.start();

    connect(m_mon, &Monitor::targetsChanged, this, &TileView::scheduleRelayout);
    connect(m_mon, &Monitor::targetUpdated, this, &TileView::onTargetUpdated);
    connect(m_mon, &Monitor::statusChanged, this, &TileView::onStatusChanged);
    relayout();
}

void TileView::setDensity(Density d) { m_density = d; relayout(); }
void TileView::setSort(Sort s) { m_sort = s; relayout(); }
void TileView::setFilterText(const QString &text) { m_filter = text.trimmed(); relayout(); }
void TileView::setStatusFilter(const QSet<Status> &f) { m_statusFilter = f; relayout(); }
void TileView::setOnlyProblems(bool on) { m_onlyProblems = on; relayout(); }
void TileView::setColumns(int c) { m_columns = qMax(0, c); relayout(); }

void TileView::setWall(bool on)
{
    m_wall = on;
    m_scale = on ? 1.22 : 1.0;
    relayout();
}

QStringList TileView::selection() const
{
    QStringList out;
    for (const QString &id : m_order)
        if (m_sel.contains(id)) out << id;
    return out;
}

void TileView::setSelection(const QStringList &ids)
{
    m_sel = QSet<QString>(ids.begin(), ids.end());
    viewport()->update();
    emit selectionChanged();
}

void TileView::selectAll() { setSelection(m_order); }
void TileView::clearSelection() { setSelection({}); }

void TileView::flash(const QString &id)
{
    m_flash.insert(id, QDateTime::currentMSecsSinceEpoch());
    if (!m_animTimer.isActive()) m_animTimer.start();
}

void TileView::ensureVisible(const QString &id)
{
    const auto it = m_itemIndex.constFind(id);
    if (it == m_itemIndex.constEnd()) return;
    const QRect r = m_items[*it].rect;
    QScrollBar *sb = verticalScrollBar();
    if (r.top() < sb->value()) sb->setValue(r.top() - 12);
    else if (r.bottom() > sb->value() + viewport()->height()) sb->setValue(r.bottom() - viewport()->height() + 12);
}

void TileView::scheduleRelayout()
{
    if (!m_relayoutTimer.isActive()) m_relayoutTimer.start();
}

bool TileView::passesFilter(const TargetConfig &t, const TargetRuntime *rt) const
{
    const Status st = rt ? rt->status : Status::Unknown;
    if (m_onlyProblems && st != Status::Down && st != Status::Degraded)
        return false;
    if (!m_statusFilter.isEmpty() && !m_statusFilter.contains(st))
        return false;
    if (!m_filter.isEmpty()) {
        const Qt::CaseSensitivity cs = Qt::CaseInsensitive;
        if (!t.displayName().contains(m_filter, cs) && !t.address.contains(m_filter, cs) && !t.group.contains(m_filter, cs)
            && !t.notes.contains(m_filter, cs) && !t.endpointText().contains(m_filter, cs))
            return false;
    }
    return true;
}

void TileView::relayout()
{
    m_items.clear();
    m_itemIndex.clear();
    m_order.clear();
    const qreal S = m_scale;
    const int W = viewport()->width();
    const int pad = int(16 * S);
    const int gap = int(10 * S);
    int y = pad;
    const bool filtering = !m_filter.isEmpty() || !m_statusFilter.isEmpty() || m_onlyProblems;

    if (m_density == Density::List && !m_mon->targets().isEmpty()) {
        Item h;
        h.type = Item::ListHead;
        h.rect = QRect(pad, y, W - 2 * pad, int(30 * S));
        m_items.append(h);
        y += h.rect.height() + 4;
    }

    const QVector<GroupInfo> groups = m_mon->groups();
    for (const GroupInfo &g : groups) {
        QVector<const TargetConfig *> list;
        for (const TargetConfig &t : m_mon->targets())
            if (t.group == g.name && passesFilter(t, m_mon->runtime(t.id))) list.append(&t);
        if (list.isEmpty() && (filtering || (g.name.isEmpty() && groups.size() > 1)))
            continue;
        auto rank = [this](const TargetConfig *t) {
            const TargetRuntime *rt = m_mon->runtime(t->id);
            return int(rt ? rt->status : Status::Unknown);
        };
        switch (m_wall ? Sort::Problems : m_sort) {
        case Sort::Problems:
            std::stable_sort(list.begin(), list.end(), [&rank](const TargetConfig *a, const TargetConfig *b) {
                return rank(a) > rank(b);
            });
            break;
        case Sort::Name:
            std::stable_sort(list.begin(), list.end(), [](const TargetConfig *a, const TargetConfig *b) {
                return QString::localeAwareCompare(a->displayName(), b->displayName()) < 0;
            });
            break;
        case Sort::Rtt:
            std::stable_sort(list.begin(), list.end(), [this](const TargetConfig *a, const TargetConfig *b) {
                const double ra = m_mon->windowStats(a->id).last, rb = m_mon->windowStats(b->id).last;
                return (ra < 0 ? 1e9 : ra) > (rb < 0 ? 1e9 : rb);
            });
            break;
        default:
            break;
        }

        Item head;
        head.type = Item::Group;
        head.group = g.name;
        head.rect = QRect(pad, y, W - 2 * pad, int(36 * S));
        m_items.append(head);
        y += head.rect.height() + int(6 * S);
        if (g.collapsed && !filtering) {
            y += int(6 * S);
            continue;
        }
        if (list.isEmpty()) {
            Item e;
            e.type = Item::Empty;
            e.group = g.name;
            e.rect = QRect(pad, y, W - 2 * pad, int(46 * S));
            m_items.append(e);
            y += e.rect.height() + int(16 * S);
            continue;
        }
        if (m_density == Density::List) {
            const int rh = int(32 * S);
            for (const TargetConfig *t : list) {
                Item it;
                it.type = Item::Tile;
                it.id = t->id;
                it.group = g.name;
                it.rect = QRect(pad, y, W - 2 * pad, rh);
                m_itemIndex.insert(t->id, m_items.size());
                m_items.append(it);
                m_order << t->id;
                y += rh + 2;
            }
            y += int(14 * S);
            continue;
        }
        const bool compact = m_density == Density::Compact;
        const int minW = int((compact ? 178 : 238) * S);
        const int h = int((compact ? 58 : 122) * S);
        const int cols = m_columns > 0 ? m_columns : qMax(1, (W - 2 * pad + gap) / (minW + gap));
        const int tileW = qMax(80, (W - 2 * pad - (cols - 1) * gap) / cols);
        int col = 0;
        for (const TargetConfig *t : list) {
            Item it;
            it.type = Item::Tile;
            it.id = t->id;
            it.group = g.name;
            it.rect = QRect(pad + col * (tileW + gap), y, tileW, h);
            m_itemIndex.insert(t->id, m_items.size());
            m_items.append(it);
            m_order << t->id;
            if (++col == cols) {
                col = 0;
                y += h + gap;
            }
        }
        if (col) y += h + gap;
        y += int(12 * S);
    }
    m_contentH = y + pad;
    // выделение не должно держать исчезнувшие цели
    for (auto it = m_sel.begin(); it != m_sel.end();) {
        if (!m_mon->target(*it)) it = m_sel.erase(it);
        else ++it;
    }
    updateScrollBar();
    viewport()->update();
}

void TileView::updateScrollBar()
{
    QScrollBar *sb = verticalScrollBar();
    sb->setPageStep(viewport()->height());
    sb->setRange(0, qMax(0, m_contentH - viewport()->height()));
}

void TileView::resizeEvent(QResizeEvent *e)
{
    QAbstractScrollArea::resizeEvent(e);
    relayout();
}

void TileView::scrollContentsBy(int, int)
{
    viewport()->update();
}

QPoint TileView::toContent(const QPoint &vp) const
{
    return vp + QPoint(0, verticalScrollBar()->value());
}

QRect TileView::toViewport(const QRect &c) const
{
    return c.translated(0, -verticalScrollBar()->value());
}

int TileView::itemAt(const QPoint &cp) const
{
    for (int i = 0; i < m_items.size(); ++i)
        if (m_items[i].rect.contains(cp)) return i;
    return -1;
}

void TileView::onTargetUpdated(const QString &id)
{
    m_dirty.insert(id);
    if (!m_repaintTimer.isActive()) m_repaintTimer.start();
}

void TileView::flushDirty()
{
    const QRect vis(0, verticalScrollBar()->value(), viewport()->width(), viewport()->height());
    for (const QString &id : qAsConst(m_dirty)) {
        const auto it = m_itemIndex.constFind(id);
        if (it == m_itemIndex.constEnd()) continue;
        const QRect r = m_items[*it].rect;
        if (r.intersects(vis)) viewport()->update(toViewport(r.adjusted(-3, -3, 3, 3)));
    }
    m_dirty.clear();
    // в шапках групп сводка тоже меняется — они узкие, перерисовать дёшево
    for (const Item &it : m_items)
        if (it.type == Item::Group && it.rect.intersects(vis)) viewport()->update(toViewport(it.rect));
}

void TileView::onStatusChanged(const StatusEvent &ev)
{
    if (ev.from != Status::Unknown && ev.from != Status::Paused && ev.to != Status::Paused && ev.to != Status::Unknown)
        flash(ev.id);
    if (m_sort == Sort::Problems || m_wall || m_onlyProblems || !m_statusFilter.isEmpty())
        scheduleRelayout();
}

// ---------------------------------------------------------------------------
// Рисование
// ---------------------------------------------------------------------------

void TileView::paintEvent(QPaintEvent *e)
{
    QPainter p(viewport());
    const Palette &pal = Theme::p();
    p.fillRect(e->rect(), pal.canvas);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    if (m_mon->targets().isEmpty()) {
        paintEmptyState(p);
        return;
    }
    const int off = verticalScrollBar()->value();
    p.translate(0, -off);
    const QRect vis = e->rect().translated(0, off);
    for (const Item &it : qAsConst(m_items)) {
        if (!it.rect.adjusted(-6, -6, 6, 6).intersects(vis)) continue;
        switch (it.type) {
        case Item::Group: paintGroup(p, it); break;
        case Item::Empty: paintEmptyGroup(p, it); break;
        case Item::ListHead: paintListHead(p, it.rect); break;
        case Item::Tile:
            if (m_density == Density::List) paintRow(p, it.rect, it.id);
            else if (m_density == Density::Compact) paintCompact(p, it.rect, it.id);
            else paintTile(p, it.rect, it.id);
            break;
        }
    }
    if (m_dropActive && !m_dropLine.isNull()) {
        p.setPen(Qt::NoPen);
        p.setBrush(pal.accent);
        p.drawRoundedRect(m_dropLine, 1.5, 1.5);
    }
}

void TileView::paintEmptyState(QPainter &p)
{
    const Palette &pal = Theme::p();
    const QRect r = viewport()->rect();
    const int logo = 76;
    const QPoint c = r.center() - QPoint(0, 70);
    Icons::paintLogo(p, QRectF(c.x() - logo / 2, c.y() - logo / 2, logo, logo));
    p.setPen(pal.ink);
    p.setFont(Theme::ui(20, QFont::DemiBold));
    p.drawText(QRect(r.left(), c.y() + 54, r.width(), 30), Qt::AlignHCenter, QStringLiteral("Добавьте первый адрес"));
    p.setPen(pal.muted);
    p.setFont(Theme::ui(13.5));
    p.drawText(QRect(r.left() + 40, c.y() + 90, r.width() - 80, 60), Qt::AlignHCenter | Qt::TextWordWrap,
               QStringLiteral("Введите IP, хост:порт, https://… или sip:… в строке сверху и нажмите Enter.\n"
                              "Можно вставить сразу список или импортировать его из vmPing, Инея или Росы."));
}

void TileView::paintGroup(QPainter &p, const Item &it)
{
    const Palette &pal = Theme::p();
    const qreal S = m_scale;
    const QRect r = it.rect;
    bool collapsed = false;
    for (const GroupInfo &g : m_mon->groups())
        if (g.name == it.group) collapsed = g.collapsed;
    const Summary s = m_mon->groupSummary(it.group);

    // шеврон
    p.save();
    p.setPen(QPen(pal.muted, 1.8 * S, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const QPointF cc(r.left() + 9 * S, r.center().y());
    QPolygonF chev = collapsed ? QPolygonF({cc + QPointF(-2.5 * S, -4.5 * S), cc + QPointF(2.5 * S, 0), cc + QPointF(-2.5 * S, 4.5 * S)})
                               : QPolygonF({cc + QPointF(-4.5 * S, -2 * S), cc + QPointF(0, 2.8 * S), cc + QPointF(4.5 * S, -2 * S)});
    p.drawPolyline(chev);
    p.restore();

    const QString title = it.group.isEmpty() ? QStringLiteral("Без группы") : it.group;
    p.setFont(Theme::ui(14 * S, QFont::DemiBold));
    p.setPen(pal.ink);
    const QFontMetrics fm(p.font());
    const int tx = r.left() + int(24 * S);
    const QString shown = fm.elidedText(title, Qt::ElideRight, r.width() / 2);
    p.drawText(QRect(tx, r.top(), fm.horizontalAdvance(shown) + 2, r.height()), Qt::AlignVCenter, shown);
    int x = tx + fm.horizontalAdvance(shown) + int(10 * S);

    p.setFont(Theme::ui(12 * S));
    p.setPen(pal.muted);
    const int okCount = s.up + s.degraded;
    const QString cnt = s.total ? QStringLiteral("%1 из %2 на связи").arg(okCount).arg(s.total - s.paused) : QStringLiteral("пусто");
    const QFontMetrics fm2(p.font());
    p.drawText(QRect(x, r.top(), fm2.horizontalAdvance(cnt) + 2, r.height()), Qt::AlignVCenter, cnt);
    x += fm2.horizontalAdvance(cnt) + int(12 * S);

    if (s.down) {
        p.setFont(Theme::ui(11.5 * S, QFont::DemiBold));
        const QString t = QStringLiteral("%1 нет связи").arg(s.down);
        const QFontMetrics fm3(p.font());
        const QRectF pill(x, r.center().y() - 10 * S, fm3.horizontalAdvance(t) + 16 * S, 20 * S);
        p.setPen(Qt::NoPen);
        p.setBrush(pal.badWash);
        p.drawRoundedRect(pill, 10 * S, 10 * S);
        p.setPen(pal.badText);
        p.drawText(pill, Qt::AlignCenter, t);
        x = int(pill.right() + 10 * S);
    }

    // полоса состава группы
    const QRect menu = groupMenuRect(r);
    const int barW = int(150 * S);
    const QRectF bar(menu.left() - 12 * S - barW, r.center().y() - 3 * S, barW, 6 * S);
    if (s.total && bar.left() > x + 10) {
        QPainterPath clip;
        clip.addRoundedRect(bar, 3 * S, 3 * S);
        p.save();
        p.setClipPath(clip);
        p.fillRect(bar, pal.raised);
        qreal bx = bar.left();
        const struct { int n; Status st; } segs[] = {{s.up, Status::Up}, {s.degraded, Status::Degraded}, {s.down, Status::Down},
                                                     {s.unknown, Status::Unknown}, {s.paused, Status::Paused}};
        for (const auto &sg : segs) {
            if (!sg.n) continue;
            const qreal w = bar.width() * sg.n / s.total;
            p.fillRect(QRectF(bx, bar.top(), w, bar.height()), Theme::statusColor(sg.st));
            bx += w;
        }
        p.restore();
    }
    // «⋯»
    Icons::paintGlyph(p, Glyph::Menu, menu, pal.faint);
    // тонкая линия под шапкой
    p.setPen(QPen(pal.line, 1));
    p.drawLine(QPointF(r.left(), r.bottom() + 0.5), QPointF(r.right(), r.bottom() + 0.5));
}

QRect TileView::groupMenuRect(const QRect &h) const
{
    const int s = int(24 * m_scale);
    return QRect(h.right() - s, h.center().y() - s / 2, s, s);
}

void TileView::paintEmptyGroup(QPainter &p, const Item &it)
{
    const Palette &pal = Theme::p();
    QPen pen(pal.lineStrong, 1.2, Qt::DashLine);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(it.rect).adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);
    p.setPen(pal.faint);
    p.setFont(Theme::ui(12.5 * m_scale));
    p.drawText(it.rect, Qt::AlignCenter, QStringLiteral("Пусто — перетащите сюда цели или выберите эту группу при добавлении"));
}

void TileView::paintBackground(QPainter &p, const QRectF &r, const QString &id, Status st, bool dependent, qreal radius)
{
    const Palette &pal = Theme::p();
    const bool sel = m_sel.contains(id);
    const bool hov = m_hover == id;
    QPainterPath path;
    path.addRoundedRect(r, radius, radius);
    if (!pal.dark) {
        p.setPen(Qt::NoPen);
        p.setBrush(alpha(pal.shadow, 0.6));
        p.drawRoundedRect(r.translated(0, 1.2), radius, radius);
    }
    QColor base = pal.tile;
    if (hov) base = mix(base, pal.raised, pal.dark ? 0.8 : 0.6);
    p.fillPath(path, base);
    if (st == Status::Down) p.fillPath(path, dependent ? pal.unknownWash : pal.badWash);
    else if (st == Status::Degraded) p.fillPath(path, alpha(pal.warnWash, pal.warnWash.alphaF() * 0.8));

    p.save();
    p.setClipPath(path);
    if (pal.dark) {
        // лёгкий блик сверху — «стекло», как у Инея, только сдержаннее
        QLinearGradient sheen(r.topLeft(), r.bottomLeft());
        sheen.setColorAt(0.0, QColor(255, 255, 255, 12));
        sheen.setColorAt(0.5, QColor(255, 255, 255, 0));
        p.fillRect(r, sheen);
    }
    if (st == Status::Paused) {
        p.setPen(QPen(alpha(pal.lineStrong, 0.35), 1));
        for (qreal x = r.left() - r.height(); x < r.right(); x += 9)
            p.drawLine(QPointF(x, r.bottom()), QPointF(x + r.height(), r.top()));
    }
    // полоса статуса слева
    QColor stripe = Theme::statusColor(st);
    if (dependent && st == Status::Down) stripe = pal.unknown;
    p.fillRect(QRectF(r.left(), r.top(), 4 * m_scale, r.height()), stripe);

    // вспышка-«зарница» при смене статуса
    const auto fl = m_flash.constFind(id);
    if (fl != m_flash.constEnd()) {
        const qreal age = qreal(QDateTime::currentMSecsSinceEpoch() - fl.value()) / kFlashMs;
        if (age < 1.0) {
            const qreal a = (1.0 - age) * (1.0 - age);
            QRadialGradient g(r.topLeft() + QPointF(r.width() * 0.18, r.height() * 0.1), r.width() * 0.95);
            QColor c0 = Theme::statusColor(st);
            g.setColorAt(0.0, alpha(c0, 0.55 * a));
            g.setColorAt(0.6, alpha(c0, 0.12 * a));
            g.setColorAt(1.0, alpha(c0, 0.0));
            p.fillRect(r, g);
        }
    }
    p.restore();

    QPen border(pal.line, 1);
    if (fl != m_flash.constEnd()) {
        const qreal age = qreal(QDateTime::currentMSecsSinceEpoch() - fl.value()) / kFlashMs;
        if (age < 1.0) border = QPen(alpha(Theme::statusColor(st), 0.9 * (1.0 - age)), 2 * m_scale);
    }
    if (sel) border = QPen(pal.accent, 2 * m_scale);
    p.setPen(border);
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r, radius, radius);
}

void TileView::paintSparkline(QPainter &p, const QRectF &area, const TargetRuntime &rt, Status st)
{
    const Palette &pal = Theme::p();
    const QVector<Sample> s = rt.stats.lastSamples(60);
    if (s.size() < 2) return;
    double maxR = 0;
    for (const Sample &x : s) if (x.ok) maxR = qMax(maxR, double(x.rtt));
    const int warn = m_mon->settings().latencyWarnMs;
    maxR = qMax(maxR * 1.15, qMin<double>(warn > 0 ? warn * 0.25 : 1.0, 5.0));
    if (maxR <= 0) maxR = 1;
    const qreal step = area.width() / 59.0;
    const qreal x0 = area.right() - step * (s.size() - 1);
    QColor line = Theme::statusColor(st == Status::Paused ? Status::Unknown : st);
    if (st == Status::Down) line = pal.faint;

    QPainterPath path, fill;
    bool open = false;
    qreal lastX = x0;
    for (int i = 0; i < s.size(); ++i) {
        const qreal x = x0 + i * step;
        if (!s[i].ok) {
            // потеря — красная засечка снизу
            p.setPen(QPen(alpha(pal.bad, 0.85), qMax<qreal>(1.5, step * 0.55), Qt::SolidLine, Qt::FlatCap));
            p.drawLine(QPointF(x, area.bottom()), QPointF(x, area.bottom() - area.height() * 0.32));
            if (open) {
                fill.lineTo(lastX, area.bottom());
                fill.closeSubpath();
            }
            open = false;
            continue;
        }
        const qreal y = area.bottom() - (s[i].rtt / maxR) * area.height();
        if (!open) {
            path.moveTo(x, y);
            fill.moveTo(x, area.bottom());
            fill.lineTo(x, y);
            open = true;
        } else {
            path.lineTo(x, y);
            fill.lineTo(x, y);
        }
        lastX = x;
    }
    if (open) {
        fill.lineTo(lastX, area.bottom());
        fill.closeSubpath();
    }
    QLinearGradient g(area.topLeft(), area.bottomLeft());
    g.setColorAt(0.0, alpha(line, pal.dark ? 0.30 : 0.22));
    g.setColorAt(1.0, alpha(line, 0.0));
    p.setPen(Qt::NoPen);
    p.fillPath(fill, g);
    p.setPen(QPen(line, 1.6 * m_scale, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
    // порог задержки пунктиром, если он в кадре
    if (warn > 0 && warn < maxR) {
        const qreal y = area.bottom() - (warn / maxR) * area.height();
        p.setPen(QPen(alpha(pal.warn, 0.5), 1, Qt::DashLine));
        p.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
    }
}

static QString kindBadge(const TargetConfig &t)
{
    switch (t.kind) {
    case ProbeKind::Http: return t.address.startsWith(QLatin1String("https"), Qt::CaseInsensitive) ? QStringLiteral("HTTPS") : QStringLiteral("HTTP");
    default: return kindTitle(t.kind);
    }
}

static QString shortEndpoint(const TargetConfig &t)
{
    if (t.kind == ProbeKind::Http) {
        QString a = t.publicEndpointText();
        a.remove(QRegularExpression(QStringLiteral("^https?://"), QRegularExpression::CaseInsensitiveOption));
        return a;
    }
    if (t.kind == ProbeKind::Sip) return t.port && t.port != 5060 ? QStringLiteral("%1:%2").arg(t.address).arg(t.port) : t.address;
    return t.endpointText();
}

void TileView::paintTile(QPainter &p, const QRect &rect, const QString &id)
{
    const TargetConfig *t = m_mon->target(id);
    const TargetRuntime *rt = m_mon->runtime(id);
    if (!t || !rt) return;
    const Palette &pal = Theme::p();
    const qreal S = m_scale;
    const QRectF r = QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5);
    const Status st = rt->status;
    const bool dep = rt->dependent && st == Status::Down;
    paintBackground(p, r, id, st, dep, 11 * S);

    const qreal x0 = r.left() + 17 * S;
    const qreal x1 = r.right() - 13 * S;
    const qreal paused = st == Status::Paused;

    // строка 1: имя и плашка статуса
    QString pillText = statusTitle(st);
    if (dep) pillText = QStringLiteral("За родителем");
    p.setFont(Theme::ui(11 * S, QFont::DemiBold));
    const QFontMetricsF pfm(p.font());
    const QRectF pill(x1 - pfm.horizontalAdvance(pillText) - 16 * S, r.top() + 12 * S, pfm.horizontalAdvance(pillText) + 16 * S, 20 * S);
    p.setPen(Qt::NoPen);
    p.setBrush(dep ? pal.unknownWash : Theme::statusWash(st));
    p.drawRoundedRect(pill, 10 * S, 10 * S);
    p.setPen(dep ? pal.unknownText : Theme::statusText(st));
    p.drawText(pill, Qt::AlignCenter, pillText);

    p.setFont(Theme::ui(14.5 * S, QFont::DemiBold));
    p.setPen(paused ? pal.muted : pal.ink);
    const QFontMetricsF nfm(p.font());
    p.drawText(QRectF(x0, r.top() + 10 * S, pill.left() - x0 - 8 * S, 24 * S), Qt::AlignVCenter | Qt::AlignLeft,
               nfm.elidedText(t->displayName(), Qt::ElideRight, pill.left() - x0 - 8 * S));

    // строка 2: адрес и вид проверки
    p.setFont(Theme::mono(11.5 * S));
    p.setPen(pal.faint);
    QString sub = shortEndpoint(*t) + QStringLiteral("  ·  ") + kindBadge(*t);
    if (dep) {
        const TargetConfig *parent = m_mon->target(t->parentId);
        if (parent) sub = QStringLiteral("за «%1»").arg(parent->displayName());
        p.setFont(Theme::ui(11.5 * S));
    }
    const QFontMetricsF sfm(p.font());
    p.drawText(QRectF(x0, r.top() + 33 * S, x1 - x0, 18 * S), Qt::AlignVCenter | Qt::AlignLeft,
               sfm.elidedText(sub, Qt::ElideRight, x1 - x0));

    // крупное значение
    const WindowStats w = rt->stats.window(m_mon->settings().statsWindow);
    const qreal vy = r.top() + 55 * S;
    const qreal vh = 34 * S;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    qreal textRight = x0;
    if (st == Status::Up || st == Status::Degraded) {
        const double v = w.last >= 0 ? w.last : w.avg;
        p.setFont(Theme::ui(29 * S, QFont::DemiBold));
        p.setPen(st == Status::Degraded ? pal.warnText : pal.ink);
        const QString num = fmtMs(v);
        const QFontMetricsF vfm(p.font());
        const qreal nw = vfm.horizontalAdvance(num);
        p.drawText(QRectF(x0, vy, nw + 2, vh), Qt::AlignVCenter | Qt::AlignLeft, num);
        p.setFont(Theme::ui(12.5 * S));
        p.setPen(pal.muted);
        p.drawText(QRectF(x0 + nw + 5 * S, vy + 5 * S, 40 * S, vh), Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("мс"));
        textRight = x0 + nw + 5 * S + QFontMetricsF(p.font()).horizontalAdvance(QStringLiteral("мс"));
    } else if (st == Status::Down) {
        p.setFont(Theme::ui(18 * S, QFont::Bold));
        p.setPen(dep ? pal.unknownText : pal.badText);
        const QString big = QStringLiteral("Нет связи");
        const QFontMetricsF bfm(p.font());
        p.drawText(QRectF(x0, vy, bfm.horizontalAdvance(big) + 2, vh), Qt::AlignVCenter | Qt::AlignLeft, big);
        p.setFont(Theme::ui(13 * S, QFont::Medium));
        p.setPen(pal.muted);
        const QString dur = fmtDuration(now - rt->since);
        p.drawText(QRectF(x0 + bfm.horizontalAdvance(big) + 8 * S, vy + 1 * S, 120 * S, vh), Qt::AlignVCenter | Qt::AlignLeft, dur);
        textRight = x0 + bfm.horizontalAdvance(big) + 8 * S + QFontMetricsF(p.font()).horizontalAdvance(dur);
    } else {
        p.setFont(Theme::ui(15 * S));
        p.setPen(pal.faint);
        p.drawText(QRectF(x0, vy, x1 - x0, vh), Qt::AlignVCenter | Qt::AlignLeft,
                   st == Status::Paused ? QStringLiteral("на паузе") : QStringLiteral("проверка…"));
    }
    // график — справа от крупного значения и не залезая на него
    const qreal sx = qMax(r.left() + r.width() * 0.47, textRight + 14 * S);
    if (m_sparkline && st != Status::Paused && x1 - sx > 40 * S)
        paintSparkline(p, QRectF(sx, vy + 1 * S, x1 - sx, vh - 4 * S), *rt, st);

    // подвал: потери, джиттер, MOS или HTTP-подробности
    p.setFont(Theme::ui(11.5 * S));
    const qreal fy = r.bottom() - 27 * S;
    const qreal is = 15 * S;
    const int icons = (rt->flapping ? 1 : 0) + (t->sourceMissing ? 1 : 0) + (!t->parentId.isEmpty() && !dep ? 1 : 0);
    const qreal textLimit = x1 - icons * (is + 6 * S);
    qreal fx = x0;
    auto put = [&](const QString &text, const QColor &c) {
        const QFontMetricsF fm(p.font());
        const qreal tw = fm.horizontalAdvance(text);
        if (fx + tw > textLimit) return;
        p.setPen(c);
        p.drawText(QRectF(fx, fy, tw + 2, 18 * S), Qt::AlignVCenter | Qt::AlignLeft, text);
        fx += tw + 12 * S;
    };
    if (st == Status::Paused) {
        put(QStringLiteral("замеры остановлены"), pal.faint);
    } else if (st == Status::Down) {
        const QString why = rt->reason.isEmpty() ? rt->lastError : rt->reason;
        if (!why.isEmpty()) put(why, pal.muted);
        if (rt->stats.lastOkTime() > 0)
            put(QStringLiteral("ответ был %1 назад").arg(fmtDuration(now - rt->stats.lastOkTime())), pal.faint);
    } else if (t->kind == ProbeKind::Http) {
        if (!rt->lastDetail.isEmpty()) put(rt->lastDetail, pal.muted);
        if (rt->certExpiry > 0) {
            const qint64 days = (rt->certExpiry - now) / 86400000;
            put(QStringLiteral("серт. %1 дн.").arg(days), days < 14 ? pal.warnText : pal.muted);
        }
        if (w.sent) put(QStringLiteral("потери %1%").arg(fmtPct(w.lossPct)), w.lossPct > 0 ? pal.warnText : pal.faint);
    } else {
        const double lossWarn = m_mon->effLossWarn(*t);
        put(QStringLiteral("потери %1%").arg(fmtPct(w.lossPct)),
            w.lossPct >= lossWarn && lossWarn > 0 ? pal.warnText : (w.lossPct > 0 ? pal.muted : pal.faint));
        if (w.jitter >= 0) put(QStringLiteral("джиттер %1").arg(fmtMs(w.jitter)), pal.faint);
        if (m_showMos && (t->kind == ProbeKind::Icmp || t->kind == ProbeKind::Sip) && w.recv > 0) {
            const double m = TargetStats::mos(w.avg, w.jitter, w.lossPct);
            put(QStringLiteral("MOS %1").arg(fmtNum(m, 1)), m >= 4.0 ? pal.okText : (m >= 3.6 ? pal.muted : (m >= 3.1 ? pal.warnText : pal.badText)));
        }
        if (t->kind == ProbeKind::Sip && !rt->lastDetail.isEmpty()) put(rt->lastDetail.section(QLatin1Char(' '), 0, 1), pal.faint);
    }
    // значки справа внизу: дребезг, источник, зависимость
    qreal ix = x1;
    if (rt->flapping) {
        Icons::paintGlyph(p, Glyph::Bolt, QRectF(ix - is, fy + 1.5 * S, is, is), pal.warn, 1.6);
        ix -= is + 6 * S;
    }
    if (t->sourceMissing) {
        Icons::paintGlyph(p, Glyph::Warning, QRectF(ix - is, fy + 1.5 * S, is, is), pal.faint, 1.5);
        ix -= is + 6 * S;
    }
    if (!t->parentId.isEmpty() && !dep) {
        Icons::paintGlyph(p, Glyph::Link, QRectF(ix - is, fy + 1.5 * S, is, is), pal.faint, 1.5);
    }
}

void TileView::paintCompact(QPainter &p, const QRect &rect, const QString &id)
{
    const TargetConfig *t = m_mon->target(id);
    const TargetRuntime *rt = m_mon->runtime(id);
    if (!t || !rt) return;
    const Palette &pal = Theme::p();
    const qreal S = m_scale;
    const QRectF r = QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5);
    const Status st = rt->status;
    const bool dep = rt->dependent && st == Status::Down;
    paintBackground(p, r, id, st, dep, 9 * S);
    const qreal x0 = r.left() + 14 * S, x1 = r.right() - 10 * S;
    const WindowStats w = rt->stats.window(m_mon->settings().statsWindow);

    QString value;
    QColor vc = pal.ink;
    if (st == Status::Up || st == Status::Degraded) {
        value = fmtMs(w.last >= 0 ? w.last : w.avg) + QStringLiteral(" мс");
        if (st == Status::Degraded) vc = pal.warnText;
    } else if (st == Status::Down) {
        value = fmtDuration(QDateTime::currentMSecsSinceEpoch() - rt->since);
        vc = dep ? pal.unknownText : pal.badText;
    } else {
        value = st == Status::Paused ? QStringLiteral("пауза") : QStringLiteral("…");
        vc = pal.faint;
    }
    p.setFont(Theme::ui(14 * S, QFont::DemiBold));
    const QFontMetricsF vfm(p.font());
    const qreal vw = vfm.horizontalAdvance(value);
    p.setPen(vc);
    p.drawText(QRectF(x1 - vw, r.top() + 8 * S, vw + 1, 20 * S), Qt::AlignVCenter | Qt::AlignRight, value);

    p.setFont(Theme::ui(13 * S, QFont::DemiBold));
    p.setPen(st == Status::Paused ? pal.muted : pal.ink);
    const QFontMetricsF nfm(p.font());
    p.drawText(QRectF(x0, r.top() + 8 * S, x1 - vw - x0 - 8 * S, 20 * S), Qt::AlignVCenter | Qt::AlignLeft,
               nfm.elidedText(t->displayName(), Qt::ElideRight, x1 - vw - x0 - 8 * S));

    p.setFont(st == Status::Down ? Theme::ui(11 * S, QFont::DemiBold) : Theme::mono(11 * S));
    p.setPen(st == Status::Down ? (dep ? pal.unknownText : pal.badText) : pal.faint);
    QString sub = st == Status::Down ? (dep ? QStringLiteral("за родителем") : QStringLiteral("Нет связи")) : shortEndpoint(*t);
    if (st != Status::Down && w.sent && w.lossPct > 0)
        sub += QStringLiteral("  ·  потери %1%").arg(fmtPct(w.lossPct));
    const QFontMetricsF sfm(p.font());
    const qreal subW = x1 - x0 - (rt->flapping ? 18 * S : 0);
    p.drawText(QRectF(x0, r.top() + 30 * S, subW, 18 * S), Qt::AlignVCenter | Qt::AlignLeft,
               sfm.elidedText(sub, Qt::ElideRight, subW));
    if (rt->flapping)
        Icons::paintGlyph(p, Glyph::Bolt, QRectF(x1 - 14 * S, r.top() + 31 * S, 14 * S, 14 * S), pal.warn, 1.6);
}

QVector<int> TileView::listColumns(int width) const
{
    // точки отсчёта столбцов: статус, имя, адрес, вид, задержка, среднее, потери, джиттер, MOS, сутки, состояние
    const qreal S = m_scale;
    const int fixed = int(24 * S);
    const int rest = width - fixed;
    const double parts[] = {0.21, 0.16, 0.05, 0.065, 0.065, 0.06, 0.06, 0.045, 0.065, 0.22};
    QVector<int> xs;
    int x = fixed;
    xs << 0 << x;
    for (double f : parts) {
        x += int(rest * f);
        xs << x;
    }
    return xs;
}

void TileView::paintListHead(QPainter &p, const QRect &r)
{
    const Palette &pal = Theme::p();
    const QVector<int> xs = listColumns(r.width());
    const QStringList titles{QString(), QStringLiteral("Цель"), QStringLiteral("Адрес"), QStringLiteral("Вид"),
                             QStringLiteral("Сейчас"), QStringLiteral("Среднее"), QStringLiteral("Потери"),
                             QStringLiteral("Джиттер"), QStringLiteral("MOS"), QStringLiteral("Сутки"), QStringLiteral("Состояние")};
    p.setFont(Theme::ui(11.5 * m_scale, QFont::DemiBold));
    for (int i = 1; i < titles.size(); ++i) {
        const int x = r.left() + xs[i] + (i == 10 ? 18 : 6);
        const int w = (i + 1 < xs.size() ? xs[i + 1] : r.width()) - xs[i] - 8;
        const bool active = (i == 1 && m_sort == Sort::Name) || (i == 4 && m_sort == Sort::Rtt) || (i == 10 && m_sort == Sort::Problems);
        p.setPen(active ? pal.accent : pal.muted);
        p.drawText(QRect(x, r.top(), w, r.height()), Qt::AlignVCenter | (i >= 4 && i <= 9 ? Qt::AlignRight : Qt::AlignLeft), titles[i]);
    }
    p.setPen(QPen(pal.line, 1));
    p.drawLine(QPointF(r.left(), r.bottom() + 0.5), QPointF(r.right(), r.bottom() + 0.5));
}

void TileView::paintRow(QPainter &p, const QRect &rect, const QString &id)
{
    const TargetConfig *t = m_mon->target(id);
    const TargetRuntime *rt = m_mon->runtime(id);
    if (!t || !rt) return;
    const Palette &pal = Theme::p();
    const qreal S = m_scale;
    const Status st = rt->status;
    const bool dep = rt->dependent && st == Status::Down;
    const QRectF r = QRectF(rect);
    QColor bg = pal.surface;
    if (m_hover == id) bg = pal.raised;
    if (st == Status::Down) bg = mix(bg, dep ? pal.unknown : pal.bad, 0.10);
    if (m_sel.contains(id)) bg = pal.accentWash;
    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawRoundedRect(r, 6 * S, 6 * S);
    const auto fl = m_flash.constFind(id);
    if (fl != m_flash.constEnd()) {
        const qreal age = qreal(QDateTime::currentMSecsSinceEpoch() - fl.value()) / kFlashMs;
        if (age < 1.0) {
            p.setBrush(alpha(Theme::statusColor(st), 0.35 * (1 - age)));
            p.drawRoundedRect(r, 6 * S, 6 * S);
        }
    }
    const QVector<int> xs = listColumns(rect.width());
    p.setBrush(dep ? pal.unknown : Theme::statusColor(st));
    p.drawEllipse(QPointF(r.left() + 12 * S, r.center().y()), 4.5 * S, 4.5 * S);

    const WindowStats w = rt->stats.window(m_mon->settings().statsWindow);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    auto cell = [&](int i, const QString &text, const QColor &c, const QFont &f, Qt::Alignment al) {
        const int pad = i == 10 ? 18 : 6;
        const int x = rect.left() + xs[i] + pad;
        const int wdt = (i + 1 < xs.size() ? xs[i + 1] : rect.width()) - xs[i] - 8 - (pad - 6);
        p.setFont(f);
        p.setPen(c);
        const QFontMetrics fm(f);
        p.drawText(QRect(x, rect.top(), wdt, rect.height()), Qt::AlignVCenter | al, fm.elidedText(text, Qt::ElideRight, wdt));
    };
    const QFont uiF = Theme::ui(12.5 * S);
    const QFont nameF = Theme::ui(12.5 * S, QFont::DemiBold);
    const QFont monoF = Theme::mono(12 * S);
    cell(1, t->displayName(), st == Status::Paused ? pal.muted : pal.ink, nameF, Qt::AlignLeft);
    cell(2, shortEndpoint(*t), pal.muted, monoF, Qt::AlignLeft);
    cell(3, kindBadge(*t), pal.faint, Theme::ui(11.5 * S), Qt::AlignLeft);
    const bool live = st == Status::Up || st == Status::Degraded;
    cell(4, live ? fmtMs(w.last) : QStringLiteral("—"), st == Status::Degraded ? pal.warnText : pal.ink, Theme::mono(12 * S, QFont::Medium), Qt::AlignRight);
    cell(5, fmtMs(w.avg), pal.muted, monoF, Qt::AlignRight);
    cell(6, w.sent ? fmtPct(w.lossPct) + QLatin1Char('%') : QStringLiteral("—"), w.lossPct > 0 ? pal.warnText : pal.faint, monoF, Qt::AlignRight);
    cell(7, fmtMs(w.jitter), pal.faint, monoF, Qt::AlignRight);
    if (w.recv > 0 && (t->kind == ProbeKind::Icmp || t->kind == ProbeKind::Sip)) {
        const double m = TargetStats::mos(w.avg, w.jitter, w.lossPct);
        cell(8, fmtNum(m, 1), m >= 4.0 ? pal.okText : (m >= 3.6 ? pal.muted : pal.warnText), monoF, Qt::AlignRight);
    } else {
        cell(8, QStringLiteral("—"), pal.faint, monoF, Qt::AlignRight);
    }
    const double up = rt->stats.uptimePct(now);
    cell(9, up >= 0 ? fmtNum(up, up >= 99.95 || up < 10 ? 1 : 2) + QLatin1Char('%') : QStringLiteral("—"), pal.muted, monoF, Qt::AlignRight);
    QString state = dep ? QStringLiteral("за родителем") : statusTitle(st);
    if (st == Status::Down || st == Status::Degraded) state += QStringLiteral(" · %1").arg(fmtDuration(now - rt->since));
    if (rt->flapping) state += QStringLiteral(" · дребезг");
    cell(10, state, dep ? pal.unknownText : Theme::statusText(st), uiF, Qt::AlignLeft);
}

// ---------------------------------------------------------------------------
// Мышь, клавиатура, перетаскивание
// ---------------------------------------------------------------------------

void TileView::mousePressEvent(QMouseEvent *e)
{
    setFocus();
    const QPoint cp = toContent(e->pos());
    const int idx = itemAt(cp);
    m_dragArmed = false;
    if (idx < 0) {
        if (e->button() == Qt::LeftButton && !(e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)))
            clearSelection();
        return;
    }
    const Item &it = m_items[idx];
    if (it.type == Item::Group) {
        if (e->button() == Qt::LeftButton) {
            if (groupMenuRect(it.rect).contains(cp)) {
                emit groupMenuRequested(it.group, e->globalPos());
                return;
            }
            bool collapsed = false;
            for (const GroupInfo &g : m_mon->groups())
                if (g.name == it.group) collapsed = g.collapsed;
            m_mon->setGroupCollapsed(it.group, !collapsed);
        }
        return;
    }
    if (it.type == Item::ListHead) {
        const QVector<int> xs = listColumns(it.rect.width());
        const int x = cp.x() - it.rect.left();
        int col = 0;
        for (int i = 0; i < xs.size(); ++i) if (x >= xs[i]) col = i;
        if (col == 1) setSort(m_sort == Sort::Name ? Sort::Manual : Sort::Name);
        else if (col == 4) setSort(m_sort == Sort::Rtt ? Sort::Manual : Sort::Rtt);
        else if (col == 10) setSort(m_sort == Sort::Problems ? Sort::Manual : Sort::Problems);
        return;
    }
    if (it.type != Item::Tile)
        return;
    const QString id = it.id;
    if (e->button() == Qt::LeftButton) {
        if (e->modifiers() & Qt::ControlModifier) {
            if (m_sel.contains(id)) m_sel.remove(id);
            else m_sel.insert(id);
            m_anchor = id;
        } else if ((e->modifiers() & Qt::ShiftModifier) && !m_anchor.isEmpty()) {
            const int a = m_order.indexOf(m_anchor), b = m_order.indexOf(id);
            if (a >= 0 && b >= 0) {
                m_sel.clear();
                for (int i = qMin(a, b); i <= qMax(a, b); ++i) m_sel.insert(m_order[i]);
            }
        } else {
            if (!m_sel.contains(id)) {
                m_sel = {id};
            }
            m_anchor = id;
        }
        m_pressPos = e->pos();
        m_pressId = id;
        m_dragArmed = true;
        viewport()->update();
        emit selectionChanged();
    } else if (e->button() == Qt::RightButton && !m_sel.contains(id)) {
        m_sel = {id};
        m_anchor = id;
        viewport()->update();
        emit selectionChanged();
    }
}

void TileView::mouseMoveEvent(QMouseEvent *e)
{
    const QPoint cp = toContent(e->pos());
    if (m_dragArmed && (e->buttons() & Qt::LeftButton)
        && (e->pos() - m_pressPos).manhattanLength() >= QApplication::startDragDistance()) {
        m_dragArmed = false;
        QStringList ids = selection();
        if (ids.isEmpty()) ids << m_pressId;
        auto *mime = new QMimeData;
        mime->setData(QString::fromLatin1(kMime), ids.join(QLatin1Char('\n')).toUtf8());
        auto *drag = new QDrag(this);
        drag->setMimeData(mime);
        const auto idx = m_itemIndex.constFind(m_pressId);
        if (idx != m_itemIndex.constEnd()) {
            const QRect r = m_items[*idx].rect;
            QPixmap pm(r.size() * devicePixelRatioF());
            pm.setDevicePixelRatio(devicePixelRatioF());
            pm.fill(Qt::transparent);
            QPainter pp(&pm);
            pp.setRenderHint(QPainter::Antialiasing, true);
            pp.setOpacity(0.9);
            pp.translate(-r.topLeft());
            if (m_density == Density::List) paintRow(pp, r, m_pressId);
            else if (m_density == Density::Compact) paintCompact(pp, r, m_pressId);
            else paintTile(pp, r, m_pressId);
            if (ids.size() > 1) {
                pp.resetTransform();
                pp.setOpacity(1.0);
                const QRectF badge(r.width() - 34, 6, 28, 20);
                pp.setPen(Qt::NoPen);
                pp.setBrush(Theme::p().accent);
                pp.drawRoundedRect(badge, 10, 10);
                pp.setPen(Theme::p().onAccent);
                pp.setFont(Theme::ui(12, QFont::Bold));
                pp.drawText(badge, Qt::AlignCenter, QString::number(ids.size()));
            }
            pp.end();
            drag->setPixmap(pm);
            drag->setHotSpot(m_pressPos - toViewport(r).topLeft());
        }
        drag->exec(Qt::MoveAction);
        m_dropActive = false;
        viewport()->update();
        return;
    }
    const int idx = itemAt(cp);
    const QString hov = idx >= 0 && m_items[idx].type == Item::Tile ? m_items[idx].id : QString();
    if (hov != m_hover) {
        for (const QString &id : {m_hover, hov}) {
            const auto it = m_itemIndex.constFind(id);
            if (it != m_itemIndex.constEnd()) viewport()->update(toViewport(m_items[*it].rect.adjusted(-3, -3, 3, 3)));
        }
        m_hover = hov;
    }
    viewport()->setCursor(idx >= 0 && m_items[idx].type == Item::Group ? Qt::PointingHandCursor : Qt::ArrowCursor);
}

void TileView::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_dragArmed && e->button() == Qt::LeftButton && !(e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))) {
        // щелчок без перетаскивания по уже выделенной группе — оставляем одну
        m_sel = {m_pressId};
        viewport()->update();
        emit selectionChanged();
    }
    m_dragArmed = false;
}

void TileView::mouseDoubleClickEvent(QMouseEvent *e)
{
    const int idx = itemAt(toContent(e->pos()));
    if (idx >= 0 && m_items[idx].type == Item::Tile)
        emit openDetails(m_items[idx].id);
}

void TileView::contextMenuEvent(QContextMenuEvent *e)
{
    const QPoint cp = toContent(e->pos());
    const int idx = itemAt(cp);
    if (idx >= 0 && m_items[idx].type == Item::Group) {
        emit groupMenuRequested(m_items[idx].group, e->globalPos());
        return;
    }
    if (idx >= 0 && m_items[idx].type == Item::Tile) {
        if (!m_sel.contains(m_items[idx].id)) {
            m_sel = {m_items[idx].id};
            viewport()->update();
        }
        emit contextMenuRequested(selection(), e->globalPos());
    }
}

void TileView::keyPressEvent(QKeyEvent *e)
{
    if (e->matches(QKeySequence::Delete) || e->key() == Qt::Key_Delete) {
        if (!m_sel.isEmpty()) emit deleteRequested(selection());
        return;
    }
    if (e->matches(QKeySequence::SelectAll)) {
        selectAll();
        return;
    }
    switch (e->key()) {
    case Qt::Key_Space:
        if (!m_sel.isEmpty()) emit togglePauseRequested(selection());
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (!m_sel.isEmpty()) emit openDetails(selection().first());
        return;
    case Qt::Key_Escape:
        if (!m_sel.isEmpty()) { clearSelection(); return; }
        break;
    case Qt::Key_Home:
        verticalScrollBar()->setValue(0);
        return;
    case Qt::Key_End:
        verticalScrollBar()->setValue(verticalScrollBar()->maximum());
        return;
    default:
        break;
    }
    QAbstractScrollArea::keyPressEvent(e);
}

void TileView::leaveEvent(QEvent *e)
{
    QAbstractScrollArea::leaveEvent(e);
    if (!m_hover.isEmpty()) {
        m_hover.clear();
        viewport()->update();
    }
}

QString TileView::tooltipFor(const QString &id) const
{
    const TargetConfig *t = m_mon->target(id);
    const TargetRuntime *rt = m_mon->runtime(id);
    if (!t || !rt) return QString();
    const WindowStats w = rt->stats.window(m_mon->settings().statsWindow);
    QStringList lines;
    lines << QStringLiteral("<b>%1</b>").arg(t->displayName().toHtmlEscaped());
    lines << QStringLiteral("%1 · %2").arg(t->publicEndpointText().toHtmlEscaped(), kindBadge(*t));
    QString st = statusTitle(rt->status);
    if (!rt->reason.isEmpty()) st += QStringLiteral(": ") + rt->reason.toHtmlEscaped();
    lines << st + QStringLiteral(" · %1").arg(fmtDuration(QDateTime::currentMSecsSinceEpoch() - rt->since));
    if (w.recv)
        lines << QStringLiteral("мин %1 · сред %2 · макс %3 мс").arg(fmtMs(w.min), fmtMs(w.avg), fmtMs(w.max));
    if (!rt->lastError.isEmpty() && rt->status != Status::Up) lines << QStringLiteral("последняя ошибка: %1").arg(rt->lastError.toHtmlEscaped());
    if (!t->notes.isEmpty()) lines << t->notes.toHtmlEscaped();
    if (t->sourceMissing) lines << QStringLiteral("<i>нет в источнике при последней синхронизации</i>");
    lines << QStringLiteral("<span style='color:%1'>двойной щелчок — карточка</span>").arg(Theme::p().faint.name());
    return lines.join(QStringLiteral("<br>"));
}

bool TileView::viewportEvent(QEvent *e)
{
    if (e->type() == QEvent::ToolTip) {
        auto *he = static_cast<QHelpEvent *>(e);
        const int idx = itemAt(toContent(he->pos()));
        if (idx >= 0 && m_items[idx].type == Item::Tile) {
            QToolTip::showText(he->globalPos(), tooltipFor(m_items[idx].id), viewport(), toViewport(m_items[idx].rect));
        } else {
            QToolTip::hideText();
            e->ignore();
        }
        return true;
    }
    return QAbstractScrollArea::viewportEvent(e);
}

void TileView::updateDrop(const QPoint &cp)
{
    m_dropGroup.clear();
    m_dropBefore.clear();
    m_dropLine = QRect();
    const int idx = itemAt(cp);
    const bool list = m_density == Density::List;
    if (idx >= 0) {
        const Item &it = m_items[idx];
        if (it.type == Item::Group || it.type == Item::Empty) {
            m_dropGroup = it.group;
            m_dropLine = it.type == Item::Empty ? it.rect.adjusted(0, it.rect.height() / 2 - 1, 0, -(it.rect.height() / 2 - 2))
                                                : QRect(it.rect.left(), it.rect.bottom() + 2, it.rect.width(), 3);
            return;
        }
        if (it.type == Item::Tile) {
            m_dropGroup = it.group;
            const bool before = list ? cp.y() < it.rect.center().y() : cp.x() < it.rect.center().x();
            if (before) {
                m_dropBefore = it.id;
                m_dropLine = list ? QRect(it.rect.left(), it.rect.top() - 2, it.rect.width(), 3)
                                  : QRect(it.rect.left() - 6, it.rect.top(), 3, it.rect.height());
            } else {
                const int pos = m_order.indexOf(it.id);
                const QString next = pos + 1 < m_order.size() ? m_order[pos + 1] : QString();
                const TargetConfig *nt = m_mon->target(next);
                m_dropBefore = nt && nt->group == it.group ? next : QString();
                m_dropLine = list ? QRect(it.rect.left(), it.rect.bottom(), it.rect.width(), 3)
                                  : QRect(it.rect.right() + 4, it.rect.top(), 3, it.rect.height());
            }
            return;
        }
    }
    // промежуток — к ближайшей группе выше
    for (int i = m_items.size() - 1; i >= 0; --i) {
        if (m_items[i].type != Item::ListHead && m_items[i].rect.top() <= cp.y()) {
            m_dropGroup = m_items[i].group;
            return;
        }
    }
}

void TileView::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasFormat(QString::fromLatin1(kMime))) {
        e->acceptProposedAction();
        m_dropActive = true;
    }
}

void TileView::dragMoveEvent(QDragMoveEvent *e)
{
    if (!e->mimeData()->hasFormat(QString::fromLatin1(kMime)))
        return;
    e->acceptProposedAction();
    const QPoint vp = e->pos();
    // прокрутка у краёв
    if (vp.y() < 30) verticalScrollBar()->setValue(verticalScrollBar()->value() - 18);
    else if (vp.y() > viewport()->height() - 30) verticalScrollBar()->setValue(verticalScrollBar()->value() + 18);
    updateDrop(toContent(vp));
    viewport()->update();
}

void TileView::dragLeaveEvent(QDragLeaveEvent *)
{
    m_dropActive = false;
    viewport()->update();
}

void TileView::dropEvent(QDropEvent *e)
{
    m_dropActive = false;
    const QStringList ids = QString::fromUtf8(e->mimeData()->data(QString::fromLatin1(kMime))).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    updateDrop(toContent(e->pos()));
    if (!ids.isEmpty() && !ids.contains(m_dropBefore)) {
        m_mon->moveTargets(ids, m_dropGroup, m_dropBefore);
        if (m_sort != Sort::Manual) m_sort = Sort::Manual;   // ручной порядок виден только в ручной сортировке
    }
    e->acceptProposedAction();
    viewport()->update();
}

} // namespace zr
