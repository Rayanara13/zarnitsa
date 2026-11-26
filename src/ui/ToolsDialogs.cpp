#include "ToolsDialogs.h"

#include "Theme.h"
#include "core/Monitor.h"
#include "probes/IcmpEngine.h"
#include "probes/ProbeDispatcher.h"
#include "probes/Resolver.h"
#include "service/Engine.h"

#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHostAddress>
#include <QHostInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>

namespace zr {

static QTableWidgetItem *cell(const QString &text, bool mono = false, Qt::Alignment al = Qt::AlignLeft | Qt::AlignVCenter)
{
    auto *i = new QTableWidgetItem(text);
    if (mono) i->setFont(Theme::mono(12));
    i->setTextAlignment(int(al));
    i->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    return i;
}

// ===========================================================================
// Трассировка
// ===========================================================================

TracerouteDialog::TracerouteDialog(Engine *engine, const QString &host, QWidget *parent)
    : QDialog(parent)
    , m_engine(engine)
{
    setWindowTitle(QStringLiteral("Трассировка — Зарница"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(900, 560);
    auto *root = new QVBoxLayout(this);
    auto *bar = new QHBoxLayout;
    m_host = new QLineEdit(host);
    m_host->setPlaceholderText(QStringLiteral("IP или имя"));
    m_maxHops = new QSpinBox;
    m_maxHops->setRange(1, 64);
    m_maxHops->setValue(30);
    m_maxHops->setPrefix(QStringLiteral("до "));
    m_maxHops->setSuffix(QStringLiteral(" прыжков"));
    m_continuous = new QCheckBox(QStringLiteral("Непрерывно, как MTR"));
    m_go = new QPushButton(QStringLiteral("Старт"));
    m_go->setProperty("accent", true);
    bar->addWidget(m_host, 1);
    bar->addWidget(m_maxHops);
    bar->addWidget(m_continuous);
    bar->addWidget(m_go);
    root->addLayout(bar);
    m_table = new QTableWidget(0, 9);
    m_table->setHorizontalHeaderLabels({QStringLiteral("№"), QStringLiteral("Узел"), QStringLiteral("Имя"), QStringLiteral("Потери"),
                                        QStringLiteral("Отпр."), QStringLiteral("Посл."), QStringLiteral("Сред."),
                                        QStringLiteral("Лучш."), QStringLiteral("Худш.")});
    m_table->verticalHeader()->hide();
    m_table->setShowGrid(false);
    m_table->setAlternatingRowColors(true);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->setColumnWidth(0, 40);
    m_table->setColumnWidth(1, 150);
    for (int c = 3; c < 9; ++c) m_table->setColumnWidth(c, 70);
    root->addWidget(m_table, 1);
    m_status = new QLabel;
    m_status->setProperty("role", "muted");
    root->addWidget(m_status);
    m_probeTimer.setSingleShot(true);
    m_probeTimer.setTimerType(Qt::PreciseTimer);
    m_probeTimer.setInterval(150);   // оставляем запас до 10 pps одного адреса для монитора
    connect(&m_probeTimer, &QTimer::timeout, this, [this]() {
        if (!m_running || m_nextTtl > qMin(m_hops.size(), m_finalTtl)) return;
        const int ttl = m_nextTtl++;
        const quint64 generation = m_generation;
        QPointer<TracerouteDialog> self(this);
        ProbeRequest req;
        req.kind = ProbeKind::Icmp;
        req.host = m_ip;
        req.ttl = ttl;
        req.size = 56;
        req.timeoutMs = 1500;
        ++m_pending;
        auto *dispatcher = m_engine->dispatcher();
        const quint64 token = dispatcher->start(req, [self, generation, ttl](const ProbeResult &r) {
            if (!self || self->m_generation != generation || !self->m_running) return;
            self->m_tokens.removeOne(r.token);
            self->onResult(ttl, r);
        });
        if (!self || self->m_generation != generation) {
            dispatcher->cancel(token);
            return;
        }
        self->m_tokens << token;
        if (self->m_running && self->m_nextTtl <= qMin(self->m_hops.size(), self->m_finalTtl))
            self->m_probeTimer.start();
    });
    m_roundTimer.setSingleShot(true);
    m_roundTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_roundTimer, &QTimer::timeout, this, &TracerouteDialog::round);
    connect(m_go, &QPushButton::clicked, this, [this]() { m_running ? stop() : start(); });
    connect(m_host, &QLineEdit::returnPressed, this, &TracerouteDialog::start);
    if (m_engine->icmp()->mode() == IcmpEngine::Mode::SystemPing)
        m_status->setText(QStringLiteral("Через системный ping: время прыжков приблизительное (включает запуск ping)."));
}

TracerouteDialog::~TracerouteDialog()
{
    stop();
}

void TracerouteDialog::stop()
{
    ++m_generation;
    m_running = false;
    m_probeTimer.stop();
    m_roundTimer.stop();
    for (quint64 t : qAsConst(m_tokens)) m_engine->dispatcher()->cancel(t);
    m_tokens.clear();
    m_pending = 0;
    m_go->setText(QStringLiteral("Старт"));
}

void TracerouteDialog::start()
{
    stop();
    const QString host = m_host->text().trimmed();
    if (host.isEmpty()) return;
    m_running = true;
    m_go->setText(QStringLiteral("Стоп"));
    m_status->setText(QStringLiteral("Определяю адрес…"));
    QPointer<TracerouteDialog> self(this);
    const quint64 generation = m_generation;
    m_engine->dispatcher()->resolver()->resolve(host, [self, generation](const QHostAddress &a, const QString &err) {
        if (!self || self->m_generation != generation || !self->m_running) return;
        if (!err.isEmpty() || a.isNull()) {
            self->m_status->setText(QStringLiteral("Имя не найдено"));
            self->stop();
            return;
        }
        self->m_ip = a.toString();
        self->m_hops = QVector<Hop>(self->m_maxHops->value());
        self->m_finalTtl = self->m_maxHops->value() + 1;
        self->m_rounds = 0;
        self->m_status->setText(QStringLiteral("Трассировка до %1…").arg(self->m_ip));
        self->round();
    });
}

void TracerouteDialog::round()
{
    if (!m_running) return;
    m_nextTtl = 1;
    m_pending = 0;
    m_probeTimer.start();
}

void TracerouteDialog::onResult(int ttl, const ProbeResult &r)
{
    if (!m_running || ttl < 1 || ttl > m_hops.size()) return;
    Hop &h = m_hops[ttl - 1];
    ++h.sent;
    const bool answered = r.outcome == ProbeOutcome::Ok || r.outcome == ProbeOutcome::TtlExceeded
        || (r.outcome == ProbeOutcome::Unreachable && !r.from.isEmpty());
    if (answered) {
        ++h.recv;
        if (!r.from.isEmpty() && h.addr != r.from) {
            h.addr = r.from;
            const QString addr = r.from;
            QPointer<TracerouteDialog> self(this);
            const quint64 generation = m_generation;
            QHostInfo::lookupHost(addr, this, [self, generation, ttl, addr](const QHostInfo &info) {
                if (!self || self->m_generation != generation || ttl > self->m_hops.size()
                    || self->m_hops[ttl - 1].addr != addr) return;
                if (info.error() == QHostInfo::NoError && info.hostName() != addr) self->m_hops[ttl - 1].name = info.hostName();
                self->render();
            });
        }
        if (r.rttMs >= 0) {
            h.last = r.rttMs;
            h.sum += r.rttMs;
            if (h.best < 0 || r.rttMs < h.best) h.best = r.rttMs;
            if (r.rttMs > h.worst) h.worst = r.rttMs;
        }
        if (r.outcome == ProbeOutcome::Ok || r.outcome == ProbeOutcome::Unreachable) {
            h.final = true;
            m_finalTtl = qMin(m_finalTtl, ttl);
        }
    } else {
        h.last = -1;
    }
    render();
    const int limit = qMin(m_hops.size(), m_finalTtl);
    if (m_nextTtl > limit)
        m_probeTimer.stop();   // после финального узла ещё не отправленные TTL не нужны
    if (--m_pending > 0 || m_nextTtl <= limit) return;
    ++m_rounds;
    if (m_continuous->isChecked() || m_rounds < 3) {
        m_roundTimer.start(m_continuous->isChecked() ? 1000 : 150);
    } else {
        m_running = false;
        m_go->setText(QStringLiteral("Старт"));
        m_status->setText(m_finalTtl <= m_hops.size()
                              ? QStringLiteral("Готово: %1 прыжков до %2").arg(m_finalTtl).arg(m_ip)
                              : QStringLiteral("До %1 не дошли за %2 прыжков").arg(m_ip).arg(m_hops.size()));
    }
}

void TracerouteDialog::render()
{
    const int rows = qMin(m_hops.size(), m_finalTtl);
    m_table->setRowCount(rows);
    const Palette &pal = Theme::p();
    for (int i = 0; i < rows; ++i) {
        const Hop &h = m_hops[i];
        const double loss = h.sent ? 100.0 * (h.sent - h.recv) / h.sent : 0.0;
        m_table->setItem(i, 0, cell(QString::number(i + 1), true, Qt::AlignRight | Qt::AlignVCenter));
        m_table->setItem(i, 1, cell(h.addr.isEmpty() ? QStringLiteral("*") : h.addr, true));
        m_table->setItem(i, 2, cell(h.name));
        auto *l = cell(h.sent ? fmtPct(loss) + QLatin1Char('%') : QString(), true, Qt::AlignRight | Qt::AlignVCenter);
        if (loss > 0) l->setForeground(loss >= 100 ? pal.badText : pal.warnText);
        m_table->setItem(i, 3, l);
        m_table->setItem(i, 4, cell(QString::number(h.sent), true, Qt::AlignRight | Qt::AlignVCenter));
        m_table->setItem(i, 5, cell(fmtMs(h.last), true, Qt::AlignRight | Qt::AlignVCenter));
        m_table->setItem(i, 6, cell(h.recv ? fmtMs(h.sum / h.recv) : QStringLiteral("—"), true, Qt::AlignRight | Qt::AlignVCenter));
        m_table->setItem(i, 7, cell(fmtMs(h.best), true, Qt::AlignRight | Qt::AlignVCenter));
        m_table->setItem(i, 8, cell(fmtMs(h.worst), true, Qt::AlignRight | Qt::AlignVCenter));
    }
}

// ===========================================================================
// Сканирование подсети
// ===========================================================================

QVector<quint32> ScanDialog::expandRange(const QString &textIn, QString *error)
{
    QVector<quint32> out;
    const QString text = textIn.trimmed();
    QHostAddress a;
    auto v4 = [](const QString &s, quint32 *ip) {
        QHostAddress h;
        if (!h.setAddress(s.trimmed()) || h.protocol() != QAbstractSocket::IPv4Protocol) return false;
        *ip = h.toIPv4Address();
        return true;
    };
    quint32 lo = 0, hi = 0;
    if (text.contains(QLatin1Char('/'))) {
        const QPair<QHostAddress, int> net = QHostAddress::parseSubnet(text);
        if (net.first.isNull() || net.first.protocol() != QAbstractSocket::IPv4Protocol) {
            if (error) *error = QStringLiteral("Неверная подсеть");
            return out;
        }
        const int bits = net.second;
        if (bits < 20) {
            if (error) *error = QStringLiteral("Слишком большая подсеть: не шире /20 (4096 адресов)");
            return out;
        }
        const quint32 mask = bits == 0 ? 0 : 0xFFFFFFFFu << (32 - bits);
        lo = net.first.toIPv4Address() & mask;
        hi = lo | ~mask;
        if (bits <= 30) { ++lo; --hi; }   // без адреса сети и широковещательного
    } else if (text.contains(QLatin1Char('-'))) {
        const QString left = text.section(QLatin1Char('-'), 0, 0), right = text.section(QLatin1Char('-'), 1);
        if (!v4(left, &lo)) { if (error) *error = QStringLiteral("Неверное начало диапазона"); return out; }
        if (!right.contains(QLatin1Char('.'))) {
            bool ok = false;
            const int last = right.trimmed().toInt(&ok);
            if (!ok || last < 0 || last > 255) { if (error) *error = QStringLiteral("Неверный конец диапазона"); return out; }
            hi = (lo & 0xFFFFFF00u) | quint32(last);
        } else if (!v4(right, &hi)) {
            if (error) *error = QStringLiteral("Неверный конец диапазона");
            return out;
        }
    } else if (v4(text, &lo)) {
        hi = lo;
    } else {
        if (error) *error = QStringLiteral("Нужна подсеть 10.1.1.0/24, диапазон 10.1.1.1-254 или адрес");
        return out;
    }
    if (hi < lo || hi - lo >= 4096) {
        if (error) *error = QStringLiteral("Диапазон пуст или шире 4096 адресов");
        return out;
    }
    for (quint32 ip = lo; ip <= hi; ++ip) {
        out.append(ip);
        if (ip == 0xFFFFFFFFu) break;
    }
    return out;
}

ScanDialog::ScanDialog(Engine *engine, const QString &cidr, QWidget *parent)
    : QDialog(parent)
    , m_engine(engine)
{
    setWindowTitle(QStringLiteral("Сканирование подсети — Зарница"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(860, 600);
    auto *root = new QVBoxLayout(this);
    auto *bar = new QHBoxLayout;
    m_range = new QLineEdit(cidr);
    m_range->setPlaceholderText(QStringLiteral("10.1.1.0/24 или 10.1.1.1-254"));
    m_ports = new QLineEdit(QStringLiteral("5060 1720 80 443 8000 22"));
    m_ports->setToolTip(QStringLiteral("Какие TCP-порты проверить у ответивших; пусто — не проверять"));
    m_go = new QPushButton(QStringLiteral("Сканировать"));
    m_go->setProperty("accent", true);
    bar->addWidget(new QLabel(QStringLiteral("Диапазон")));
    bar->addWidget(m_range, 2);
    bar->addWidget(new QLabel(QStringLiteral("Порты")));
    bar->addWidget(m_ports, 1);
    bar->addWidget(m_go);
    root->addLayout(bar);
    m_progress = new QProgressBar;
    m_progress->setTextVisible(false);
    root->addWidget(m_progress);
    m_table = new QTableWidget(0, 6);
    m_table->setHorizontalHeaderLabels({QString(), QStringLiteral("Адрес"), QStringLiteral("Имя"), QStringLiteral("мс"),
                                        QStringLiteral("TTL"), QStringLiteral("Открытые порты")});
    m_table->verticalHeader()->hide();
    m_table->setShowGrid(false);
    m_table->setAlternatingRowColors(true);
    m_table->setSortingEnabled(false);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->setColumnWidth(0, 32);
    m_table->setColumnWidth(1, 140);
    m_table->setColumnWidth(3, 70);
    m_table->setColumnWidth(4, 110);
    m_table->setColumnWidth(5, 190);
    root->addWidget(m_table, 1);
    auto *bottom = new QHBoxLayout;
    m_status = new QLabel;
    m_status->setProperty("role", "muted");
    bottom->addWidget(m_status, 1);
    bottom->addWidget(new QLabel(QStringLiteral("в группу")));
    m_group = new QComboBox;
    m_group->setEditable(true);
    for (const GroupInfo &g : engine->monitor()->groups())
        if (!g.name.isEmpty()) m_group->addItem(g.name);
    m_group->setCurrentText(QStringLiteral("Найдено в %1").arg(cidr.isEmpty() ? QStringLiteral("сети") : cidr));
    m_group->setMinimumWidth(200);
    bottom->addWidget(m_group);
    auto *add = new QPushButton(QStringLiteral("Добавить выбранные"));
    bottom->addWidget(add);
    root->addLayout(bottom);
    m_probeTimer.setSingleShot(true);
    m_probeTimer.setTimerType(Qt::PreciseTimer);
    m_probeTimer.setInterval(10);   // 100 адресов/с, без начальной пачки на всю подсеть
    connect(&m_probeTimer, &QTimer::timeout, this, [this]() {
        if (!m_running || m_nextIp >= m_ips.size()) return;
        const quint32 ip = m_ips.at(m_nextIp++);
        const quint64 generation = m_generation;
        QPointer<ScanDialog> self(this);
        ProbeRequest req;
        req.kind = ProbeKind::Icmp;
        req.host = QHostAddress(ip).toString();
        req.timeoutMs = 900;
        req.size = 56;
        req.ttl = 64;
        auto *dispatcher = m_engine->dispatcher();
        const quint64 token = dispatcher->start(req, [self, generation, ip](const ProbeResult &r) {
            if (!self || self->m_generation != generation || !self->m_running) return;
            self->m_tokens.removeOne(r.token);
            self->onPing(ip, r);
        });
        if (!self || self->m_generation != generation) {
            dispatcher->cancel(token);
            return;
        }
        self->m_tokens << token;
        if (self->m_running && self->m_nextIp < self->m_ips.size())
            self->m_probeTimer.start();
    });
    connect(m_go, &QPushButton::clicked, this, [this]() { m_running ? stop() : start(); });
    connect(m_range, &QLineEdit::returnPressed, this, &ScanDialog::start);
    connect(add, &QPushButton::clicked, this, &ScanDialog::addSelected);
}

ScanDialog::~ScanDialog()
{
    stop();
}

void ScanDialog::stop()
{
    ++m_generation;
    m_running = false;
    m_probeTimer.stop();
    m_ips.clear();
    m_nextIp = 0;
    for (quint64 t : qAsConst(m_tokens)) m_engine->dispatcher()->cancel(t);
    m_tokens.clear();
    m_go->setText(QStringLiteral("Сканировать"));
}

void ScanDialog::start()
{
    stop();
    QString err;
    m_ips = expandRange(m_range->text(), &err);
    if (m_ips.isEmpty()) {
        m_status->setText(err);
        return;
    }
    m_table->setRowCount(0);
    m_rows.clear();
    m_total = m_ips.size();
    m_done = 0;
    m_progress->setRange(0, m_total);
    m_progress->setValue(0);
    m_running = true;
    m_go->setText(QStringLiteral("Стоп"));
    m_status->setText(QStringLiteral("Опрашиваю %1 адресов…").arg(m_total));
    m_probeTimer.start();
}

void ScanDialog::onPing(quint32 ip, const ProbeResult &r)
{
    ++m_done;
    m_progress->setValue(m_done);
    if (r.ok) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        auto *chk = new QTableWidgetItem;
        chk->setCheckState(Qt::Checked);
        m_table->setItem(row, 0, chk);
        const QString addr = QHostAddress(ip).toString();
        m_table->setItem(row, 1, cell(addr, true));
        m_table->setItem(row, 2, cell(QString()));
        m_table->setItem(row, 3, cell(fmtMs(r.rttMs), true, Qt::AlignRight | Qt::AlignVCenter));
        QString ttl = r.ttl > 0 ? QString::number(r.ttl) : QStringLiteral("—");
        if (r.ttl > 0) ttl += r.ttl <= 64 ? QStringLiteral(" · Linux/устр.") : (r.ttl <= 128 ? QStringLiteral(" · Windows") : QStringLiteral(" · сеть"));
        m_table->setItem(row, 4, cell(ttl, true));
        m_table->setItem(row, 5, cell(QString()));
        m_rows.insert(ip, row);
        QPointer<ScanDialog> self(this);
        const quint64 generation = m_generation;
        QHostInfo::lookupHost(addr, this, [self, generation, ip, addr](const QHostInfo &info) {
            if (!self || self->m_generation != generation) return;
            const int rr = self->m_rows.value(ip, -1);
            if (rr >= 0 && rr < self->m_table->rowCount() && self->m_table->item(rr, 2)
                && info.error() == QHostInfo::NoError && info.hostName() != addr)
                self->m_table->item(rr, 2)->setText(info.hostName());
        });
        probePorts(row, ip);
    }
    if (m_done >= m_total) {
        m_running = false;
        m_probeTimer.stop();
        m_ips.clear();
        m_go->setText(QStringLiteral("Сканировать"));
        m_status->setText(QStringLiteral("Ответили %1 из %2").arg(m_table->rowCount()).arg(m_total));
        // TCP ещё может работать: его токены удаляет только ответ либо stop()/деструктор.
    } else {
        m_status->setText(QStringLiteral("Проверено %1 из %2, ответили %3").arg(m_done).arg(m_total).arg(m_table->rowCount()));
    }
}

void ScanDialog::probePorts(int row, quint32 ip)
{
    const QStringList ports = m_ports->text().split(QRegularExpression(QStringLiteral("[\\s,;]+")), Qt::SkipEmptyParts);
    if (ports.isEmpty()) return;
    auto open = std::make_shared<QStringList>();
    auto left = std::make_shared<int>(ports.size());
    QPointer<ScanDialog> self(this);
    const quint64 generation = m_generation;
    for (const QString &ps : ports) {
        const int port = ps.toInt();
        if (port <= 0 || port > 65535) { --*left; continue; }
        ProbeRequest req;
        req.kind = ProbeKind::Tcp;
        req.host = QHostAddress(ip).toString();
        req.port = quint16(port);
        req.timeoutMs = 700;
        m_tokens << m_engine->dispatcher()->start(req, [self, generation, row, port, open, left](const ProbeResult &r) {
            if (!self || self->m_generation != generation) return;
            self->m_tokens.removeOne(r.token);
            if (r.ok) open->append(QString::number(port));
            if (--*left == 0 || r.ok) {
                if (row < self->m_table->rowCount() && self->m_table->item(row, 5)) {
                    QStringList sorted = *open;
                    std::sort(sorted.begin(), sorted.end(), [](const QString &a, const QString &b) { return a.toInt() < b.toInt(); });
                    self->m_table->item(row, 5)->setText(sorted.join(QStringLiteral(" ")));
                }
            }
        });
    }
}

void ScanDialog::addSelected()
{
    QVector<TargetConfig> list;
    const QString group = m_group->currentText().trimmed();
    for (int r = 0; r < m_table->rowCount(); ++r) {
        if (m_table->item(r, 0)->checkState() != Qt::Checked) continue;
        TargetConfig t;
        t.id = newTargetId();
        t.address = m_table->item(r, 1)->text();
        t.name = m_table->item(r, 2)->text();
        t.group = group;
        t.kind = ProbeKind::Icmp;
        const QString ports = m_table->item(r, 5)->text();
        if (!ports.isEmpty()) t.notes = QStringLiteral("Открытые порты при сканировании: %1").arg(ports);
        list.append(t);
    }
    if (list.isEmpty()) return;
    m_engine->monitor()->addTargets(list);
    m_status->setText(QStringLiteral("Добавлено %1 в «%2»").arg(list.size()).arg(group));
}

// ===========================================================================
// DNS
// ===========================================================================

DnsDialog::DnsDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("DNS — Зарница"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(560, 360);
    auto *root = new QVBoxLayout(this);
    auto *bar = new QHBoxLayout;
    m_query = new QLineEdit;
    m_query->setPlaceholderText(QStringLiteral("Имя хоста или IP для обратного поиска"));
    auto *go = new QPushButton(QStringLiteral("Найти"));
    go->setProperty("accent", true);
    bar->addWidget(m_query, 1);
    bar->addWidget(go);
    root->addLayout(bar);
    m_out = new QPlainTextEdit;
    m_out->setReadOnly(true);
    m_out->setFont(Theme::mono(12.5));
    root->addWidget(m_out, 1);
    connect(go, &QPushButton::clicked, this, &DnsDialog::lookup);
    connect(m_query, &QLineEdit::returnPressed, this, &DnsDialog::lookup);
}

void DnsDialog::lookup()
{
    const QString q = m_query->text().trimmed();
    if (q.isEmpty()) return;
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    m_out->appendPlainText(QStringLiteral("> %1").arg(q));
    QPointer<DnsDialog> self(this);
    QHostInfo::lookupHost(q, this, [self, q, clock](const QHostInfo &info) {
        if (!self) return;
        const QString ms = fmtMs(double(clock->nsecsElapsed()) / 1e6);
        if (info.error() != QHostInfo::NoError) {
            self->m_out->appendPlainText(QStringLiteral("  не найдено (%1 мс): %2\n").arg(ms, info.errorString()));
            return;
        }
        QHostAddress a;
        if (a.setAddress(q)) {
            self->m_out->appendPlainText(QStringLiteral("  имя: %1  (%2 мс)\n").arg(info.hostName(), ms));
            return;
        }
        for (const QHostAddress &x : info.addresses())
            self->m_out->appendPlainText(QStringLiteral("  %1").arg(x.toString()));
        self->m_out->appendPlainText(QStringLiteral("  (%1 мс)\n").arg(ms));
    });
}

} // namespace zr
