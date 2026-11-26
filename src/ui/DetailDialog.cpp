#include "DetailDialog.h"

#include "Icons.h"
#include "Theme.h"
#include "core/EventLog.h"
#include "core/Monitor.h"
#include "service/Engine.h"

#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QTimer>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>
#include <cmath>

namespace zr {

// ===========================================================================
// График
// ===========================================================================

RttChart::RttChart(Engine *engine, const QString &id, QWidget *parent)
    : QWidget(parent)
    , m_engine(engine)
    , m_id(id)
{
    setMouseTracking(true);
    setMinimumHeight(190);
    connect(engine->monitor(), &Monitor::targetUpdated, this, [this](const QString &tid) {
        if (tid == m_id) update();
    });
}

static double niceCeil(double v)
{
    if (v <= 0) return 1;
    const double e = std::pow(10.0, std::floor(std::log10(v)));
    for (double m : {1.0, 2.0, 2.5, 5.0, 10.0})
        if (v <= m * e) return m * e;
    return 10 * e;
}

void RttChart::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const Palette &pal = Theme::p();
    const QRectF full = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(pal.line, 1));
    p.setBrush(pal.tile);
    p.drawRoundedRect(full, 10, 10);

    const TargetRuntime *rt = m_engine->monitor()->runtime(m_id);
    if (!rt) return;
    const QRectF area = full.adjusted(52, 16, -16, -30);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    struct Pt { qint64 t; double avg, lo, hi; double loss; bool has; };
    QVector<Pt> pts;
    qint64 span = 600000;
    if (m_range == Range::Minutes10) {
        for (const Sample &s : rt->stats.lastSamples(600)) {
            if (s.t < now - span) continue;
            pts.append({s.t, s.ok ? s.rtt : -1.0, s.rtt, s.rtt, s.ok ? 0.0 : 1.0, s.ok});
        }
    } else {
        const int minutes = m_range == Range::Hour ? 60 : 1440;
        span = qint64(minutes) * 60000;
        for (const MinuteBucket &b : rt->stats.minuteSeries(now, minutes)) {
            const bool has = b.sent > 0;
            pts.append({b.minute * 60000 + 30000, b.avg(), b.min, b.max,
                        has ? double(b.sent - b.recv) / b.sent : 0.0, has && b.recv > 0});
        }
    }
    double maxV = 0;
    for (const Pt &x : pts) if (x.has) maxV = qMax(maxV, x.hi);
    const int warn = m_engine->monitor()->settings().latencyWarnMs;
    maxV = niceCeil(qMax(maxV * 1.1, 1.0));
    auto X = [&](qint64 t) { return area.left() + area.width() * double(t - (now - span)) / double(span); };
    auto Y = [&](double v) { return area.bottom() - area.height() * qBound(0.0, v / maxV, 1.0); };

    // сетка и подписи
    p.setFont(Theme::mono(10.5));
    for (int i = 0; i <= 4; ++i) {
        const double v = maxV * i / 4.0;
        const qreal y = Y(v);
        p.setPen(QPen(i == 0 ? pal.lineStrong : pal.line, 1, i == 0 ? Qt::SolidLine : Qt::DotLine));
        p.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
        p.setPen(pal.faint);
        p.drawText(QRectF(full.left() + 4, y - 9, 42, 18), Qt::AlignRight | Qt::AlignVCenter, fmtMs(v));
    }
    const int ticks = 6;
    for (int i = 0; i <= ticks; ++i) {
        const qint64 t = now - span + span * i / ticks;
        const qreal x = X(t);
        p.setPen(pal.faint);
        const QString lab = m_range == Range::Day ? QDateTime::fromMSecsSinceEpoch(t).toString(QStringLiteral("HH:mm"))
                                                  : QDateTime::fromMSecsSinceEpoch(t).toString(QStringLiteral("HH:mm:ss"));
        p.drawText(QRectF(x - 40, area.bottom() + 6, 80, 18), Qt::AlignCenter, lab);
    }
    if (warn > 0 && warn < maxV) {
        p.setPen(QPen(pal.warn, 1, Qt::DashLine));
        p.drawLine(QPointF(area.left(), Y(warn)), QPointF(area.right(), Y(warn)));
    }
    if (pts.isEmpty()) {
        p.setPen(pal.faint);
        p.setFont(Theme::ui(13));
        p.drawText(area, Qt::AlignCenter, QStringLiteral("Замеров пока нет"));
        return;
    }
    const QColor line = pal.accent;
    // коридор мин–макс по минутам
    if (m_range != Range::Minutes10) {
        QPainterPath band;
        bool open = false;
        QVector<QPointF> lower;
        for (const Pt &x : pts) {
            if (!x.has) {
                if (open) {
                    for (int i = lower.size() - 1; i >= 0; --i) band.lineTo(lower[i]);
                    band.closeSubpath();
                    lower.clear();
                    open = false;
                }
                continue;
            }
            const QPointF hi(X(x.t), Y(x.hi));
            if (!open) { band.moveTo(hi); open = true; }
            else band.lineTo(hi);
            lower.append(QPointF(X(x.t), Y(x.lo)));
        }
        if (open) {
            for (int i = lower.size() - 1; i >= 0; --i) band.lineTo(lower[i]);
            band.closeSubpath();
        }
        QColor bc = line;
        bc.setAlphaF(0.18);
        p.fillPath(band, bc);
    }
    // потери снизу
    for (const Pt &x : pts) {
        if (x.loss <= 0) continue;
        const qreal xx = X(x.t);
        const qreal w = qMax<qreal>(1.5, area.width() / qMax(60, pts.size()));
        QColor c = pal.bad;
        c.setAlphaF(0.35 + 0.6 * x.loss);
        p.fillRect(QRectF(xx - w / 2, area.bottom() - 10 * (0.4 + 0.6 * x.loss), w, 10 * (0.4 + 0.6 * x.loss)), c);
    }
    QPainterPath path;
    bool open = false;
    for (const Pt &x : pts) {
        if (!x.has || x.avg < 0) { open = false; continue; }
        const QPointF pt(X(x.t), Y(x.avg));
        if (!open) { path.moveTo(pt); open = true; }
        else path.lineTo(pt);
    }
    p.setPen(QPen(line, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);

    // перекрестие под мышью
    if (m_hoverX >= area.left() && m_hoverX <= area.right()) {
        const qint64 t = now - span + qint64(double(m_hoverX - area.left()) / area.width() * span);
        const Pt *best = nullptr;
        for (const Pt &x : pts)
            if (!best || qAbs(x.t - t) < qAbs(best->t - t)) best = &x;
        if (best) {
            const qreal xx = X(best->t);
            p.setPen(QPen(pal.lineStrong, 1));
            p.drawLine(QPointF(xx, area.top()), QPointF(xx, area.bottom()));
            QString label = QDateTime::fromMSecsSinceEpoch(best->t).toString(QStringLiteral("HH:mm:ss")) + QStringLiteral("  ");
            if (best->has) {
                label += m_range == Range::Minutes10 ? QStringLiteral("%1 мс").arg(fmtMs(best->avg))
                                                     : QStringLiteral("сред %1 · мин %2 · макс %3 мс").arg(fmtMs(best->avg), fmtMs(best->lo), fmtMs(best->hi));
                p.setPen(Qt::NoPen);
                p.setBrush(line);
                p.drawEllipse(QPointF(xx, Y(best->avg)), 3.5, 3.5);
            } else {
                label += QStringLiteral("нет ответа");
            }
            if (best->loss > 0 && m_range != Range::Minutes10) label += QStringLiteral(" · потери %1%").arg(fmtPct(best->loss * 100));
            p.setFont(Theme::ui(11.5));
            const QFontMetrics fm(p.font());
            QRectF box(xx + 8, area.top() + 2, fm.horizontalAdvance(label) + 14, 22);
            if (box.right() > area.right()) box.moveRight(xx - 8);
            p.setPen(QPen(pal.lineStrong, 1));
            p.setBrush(pal.raised);
            p.drawRoundedRect(box, 6, 6);
            p.setPen(pal.ink);
            p.drawText(box, Qt::AlignCenter, label);
        }
    }
}

void RttChart::mouseMoveEvent(QMouseEvent *e)
{
    m_hoverX = e->pos().x();
    update();
}

void RttChart::leaveEvent(QEvent *)
{
    m_hoverX = -1;
    update();
}

// ===========================================================================
// Карточка
// ===========================================================================

DetailDialog::DetailDialog(Engine *engine, const QString &id, QWidget *parent)
    : QDialog(parent)
    , m_engine(engine)
    , m_id(id)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setMinimumSize(720, 640);
    auto *root = new QVBoxLayout(this);
    root->setSpacing(10);

    auto *head = new QHBoxLayout;
    auto *col = new QVBoxLayout;
    m_title = new QLabel;
    m_title->setTextFormat(Qt::PlainText);
    m_title->setFont(Theme::ui(21, QFont::DemiBold));
    m_addr = new QLabel;
    m_addr->setTextFormat(Qt::PlainText);
    m_addr->setFont(Theme::mono(12.5));
    m_addr->setProperty("role", "muted");
    m_addr->setTextInteractionFlags(Qt::TextSelectableByMouse);
    col->addWidget(m_title);
    col->addWidget(m_addr);
    head->addLayout(col, 1);
    m_status = new QLabel;
    m_status->setFont(Theme::ui(13, QFont::DemiBold));
    head->addWidget(m_status, 0, Qt::AlignTop);
    root->addLayout(head);

    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(18);
    grid->setVerticalSpacing(6);
    const QStringList captions{QStringLiteral("Сейчас"), QStringLiteral("Среднее"), QStringLiteral("Мин / макс"),
                               QStringLiteral("Джиттер"), QStringLiteral("Потери (окно)"), QStringLiteral("MOS · оценка"),
                               QStringLiteral("Успешные замеры 24 ч"), QStringLiteral("Отправлено / получено"),
                               QStringLiteral("В статусе"), QStringLiteral("Последний ответ"), QStringLiteral("Подробности"),
                               QStringLiteral("Ошибка")};
    for (int i = 0; i < captions.size(); ++i) {
        auto *cap = new QLabel(captions[i]);
        cap->setProperty("role", "faint");
        cap->setFont(Theme::ui(11.5));
        auto *val = new QLabel(QStringLiteral("—"));
        val->setFont(i < 8 ? Theme::mono(15, QFont::Medium) : Theme::ui(13));
        val->setTextInteractionFlags(Qt::TextSelectableByMouse);
        val->setWordWrap(i >= 8);
        const int r = (i / 4) * 2, c = i % 4;
        grid->addWidget(cap, r, c);
        grid->addWidget(val, r + 1, c);
        if (i == 5) {
            const QString tip = QStringLiteral("Приближённая оценка по RTT, вариации задержки и потерям сетевых проб. Не замер RTP, качества речи или звонка.");
            cap->setToolTip(tip);
            val->setToolTip(tip);
        } else if (i == 6) {
            const QString tip = QStringLiteral("Доля успешных замеров в доступной истории за последние 24 часа, не доля времени и не SLA. История замеров хранится в памяти.");
            cap->setToolTip(tip);
            val->setToolTip(tip);
        }
        val->setTextFormat(Qt::PlainText);
        m_values.append(val);
    }
    root->addLayout(grid);

    auto *rangeRow = new QHBoxLayout;
    auto *rangeGroup = new QButtonGroup(this);
    const QStringList ranges{QStringLiteral("10 минут"), QStringLiteral("Час"), QStringLiteral("Сутки")};
    for (int i = 0; i < ranges.size(); ++i) {
        auto *b = new QPushButton(ranges[i]);
        b->setCheckable(true);
        b->setChecked(i == 0);
        rangeGroup->addButton(b, i);
        rangeRow->addWidget(b);
    }
    rangeRow->addStretch();
    root->addLayout(rangeRow);
    m_chart = new RttChart(engine, id);
    root->addWidget(m_chart, 1);
    connect(rangeGroup, &QButtonGroup::idClicked, this, [this](int i) {
        m_chart->setRange(i == 0 ? RttChart::Range::Minutes10 : (i == 1 ? RttChart::Range::Hour : RttChart::Range::Day));
    });

    auto *evCap = new QLabel(QStringLiteral("Журнал цели"));
    evCap->setProperty("role", "section");
    root->addWidget(evCap);
    m_events = new QListWidget;
    m_events->setMaximumHeight(150);
    root->addWidget(m_events);

    auto *buttons = new QHBoxLayout;
    m_pause = new QPushButton;
    auto *edit = new QPushButton(QStringLiteral("Изменить…"));
    auto *trace = new QPushButton(QStringLiteral("Трассировка"));
    auto *copy = new QPushButton(QStringLiteral("Копировать адрес"));
    auto *reset = new QPushButton(QStringLiteral("Сбросить статистику"));
    auto *closeBtn = new QPushButton(QStringLiteral("Закрыть"));
    buttons->addWidget(m_pause);
    buttons->addWidget(edit);
    buttons->addWidget(trace);
    buttons->addWidget(copy);
    const TargetConfig *t = engine->monitor()->target(id);
    if (t && t->kind == ProbeKind::Http) {
        auto *open = new QPushButton(QStringLiteral("Открыть в браузере"));
        buttons->addWidget(open);
        connect(open, &QPushButton::clicked, this, [this]() {
            if (const TargetConfig *tt = m_engine->monitor()->target(m_id)) QDesktopServices::openUrl(QUrl(tt->address));
        });
    }
    buttons->addWidget(reset);
    buttons->addStretch();
    buttons->addWidget(closeBtn);
    root->addLayout(buttons);

    connect(m_pause, &QPushButton::clicked, this, [this]() {
        const TargetConfig *tt = m_engine->monitor()->target(m_id);
        if (tt) m_engine->monitor()->setEnabled({m_id}, !tt->enabled);
    });
    connect(edit, &QPushButton::clicked, this, [this]() { emit editRequested(m_id); });
    connect(trace, &QPushButton::clicked, this, [this]() { emit traceRequested(m_id); });
    connect(copy, &QPushButton::clicked, this, [this]() {
        if (const TargetConfig *tt = m_engine->monitor()->target(m_id))
            QApplication::clipboard()->setText(tt->kind == ProbeKind::Http ? tt->address : tt->hostPart());
    });
    connect(reset, &QPushButton::clicked, this, [this]() { m_engine->monitor()->resetStats({m_id}); });
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);

    connect(engine->monitor(), &Monitor::targetUpdated, this, [this](const QString &tid) {
        if (tid == m_id) refresh();
    });
    connect(engine->monitor(), &Monitor::targetsChanged, this, [this]() {
        if (!m_engine->monitor()->target(m_id)) close();
        else refresh();
    });
    connect(engine->log(), &EventLog::appended, this, [this](const StatusEvent &e) {
        if (e.id == m_id) refreshEvents();
    });
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &DetailDialog::refresh);
    timer->start(1000);
    refresh();
    refreshEvents();
}

void DetailDialog::refresh()
{
    Monitor *mon = m_engine->monitor();
    const TargetConfig *t = mon->target(m_id);
    const TargetRuntime *rt = mon->runtime(m_id);
    if (!t || !rt) return;
    const Palette &pal = Theme::p();
    setWindowTitle(QStringLiteral("%1 — Зарница").arg(t->displayName()));
    m_title->setText(t->displayName());
    m_addr->setText(QStringLiteral("%1 · %2%3").arg(t->publicEndpointText(), kindTitle(t->kind),
                                                     t->group.isEmpty() ? QString() : QStringLiteral(" · ") + t->group));
    QString st = statusTitle(rt->status);
    if (rt->dependent && rt->status == Status::Down) st = QStringLiteral("За родителем");
    if (rt->flapping) st += QStringLiteral(" · дребезг");
    m_status->setText(QStringLiteral("  %1  ").arg(st));
    m_status->setStyleSheet(QStringLiteral("background:%1; color:%2; border-radius: 11px; padding: 4px 8px;")
                                .arg(Theme::statusWash(rt->status).name(QColor::HexArgb), Theme::statusText(rt->status).name()));
    m_pause->setText(t->enabled ? QStringLiteral("Пауза") : QStringLiteral("Продолжить"));

    const WindowStats w = rt->stats.window(mon->settings().statsWindow);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const double up = rt->stats.uptimePct(now);
    auto ms = [](double v) { return v >= 0 ? fmtMs(v) + QStringLiteral(" мс") : QStringLiteral("—"); };
    m_values[0]->setText(ms(w.last));
    m_values[1]->setText(ms(w.avg));
    m_values[2]->setText(w.min >= 0 ? QStringLiteral("%1 / %2").arg(fmtMs(w.min), fmtMs(w.max)) : QStringLiteral("—"));
    m_values[3]->setText(ms(w.jitter));
    m_values[4]->setText(w.sent ? fmtPct(w.lossPct) + QStringLiteral(" %") : QStringLiteral("—"));
    if (w.recv) {
        const double m = TargetStats::mos(w.avg, w.jitter, w.lossPct);
        m_values[5]->setText(QStringLiteral("%1 · %2").arg(fmtNum(m, 2), TargetStats::mosTitle(m)));
    } else {
        m_values[5]->setText(QStringLiteral("—"));
    }
    m_values[6]->setText(up >= 0 ? fmtNum(up, 2) + QStringLiteral(" %") : QStringLiteral("—"));
    m_values[7]->setText(QStringLiteral("%1 / %2").arg(rt->stats.totalSent()).arg(rt->stats.totalRecv()));
    m_values[8]->setText(QStringLiteral("%1, с %2").arg(fmtDuration(now - rt->since),
                                                         QDateTime::fromMSecsSinceEpoch(rt->since).toString(QStringLiteral("dd.MM HH:mm:ss"))));
    m_values[9]->setText(rt->stats.lastOkTime() ? QStringLiteral("%1 назад").arg(fmtDuration(now - rt->stats.lastOkTime())) : QStringLiteral("не было"));
    QStringList det;
    if (!rt->lastDetail.isEmpty()) det << rt->lastDetail;
    if (rt->lastTtl > 0) det << QStringLiteral("TTL %1").arg(rt->lastTtl);
    if (!rt->resolved.isEmpty() && rt->resolved != t->address) det << rt->resolved;
    if (rt->certExpiry) det << QStringLiteral("сертификат до %1").arg(QDateTime::fromMSecsSinceEpoch(rt->certExpiry).toString(QStringLiteral("dd.MM.yyyy")));
    if (!rt->reason.isEmpty()) det << rt->reason;
    m_values[10]->setText(det.isEmpty() ? QStringLiteral("—") : det.join(QStringLiteral(" · ")));
    m_values[11]->setText(rt->lastError.isEmpty() ? QStringLiteral("—") : rt->lastError);
    m_values[11]->setStyleSheet(rt->lastError.isEmpty() ? QString() : QStringLiteral("color:%1;").arg(pal.badText.name()));
}

void DetailDialog::refreshEvents()
{
    m_events->clear();
    const QVector<StatusEvent> evs = m_engine->log()->forTarget(m_id, 100);
    for (int i = evs.size() - 1; i >= 0; --i) {
        const StatusEvent &e = evs[i];
        auto *item = new QListWidgetItem(QStringLiteral("%1   %2 → %3   %4")
                                             .arg(QDateTime::fromMSecsSinceEpoch(e.t).toString(QStringLiteral("dd.MM HH:mm:ss")),
                                                  statusTitle(e.from), statusTitle(e.to), e.text()));
        item->setForeground(Theme::statusText(e.to));
        m_events->addItem(item);
    }
    if (evs.isEmpty())
        m_events->addItem(QStringLiteral("Смен статуса пока не было"));
}

} // namespace zr
