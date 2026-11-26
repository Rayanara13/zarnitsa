#include "HistoryDialog.h"

#include "Theme.h"
#include "core/EventLog.h"
#include "service/Engine.h"

#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>

namespace zr {

void EventModel::setEvents(const QVector<StatusEvent> &evs)
{
    beginResetModel();
    m_ev.clear();
    m_ev.reserve(evs.size());
    for (int i = evs.size() - 1; i >= 0; --i) m_ev.append(evs[i]);
    endResetModel();
}

void EventModel::prepend(const StatusEvent &e)
{
    beginInsertRows(QModelIndex(), 0, 0);
    m_ev.prepend(e);
    endInsertRows();
}

QVariant EventModel::data(const QModelIndex &idx, int role) const
{
    if (!idx.isValid() || idx.row() >= m_ev.size()) return QVariant();
    const StatusEvent &e = m_ev[idx.row()];
    if (role == Qt::DisplayRole) {
        switch (idx.column()) {
        case 0: return QDateTime::fromMSecsSinceEpoch(e.t).toString(QStringLiteral("dd.MM.yyyy HH:mm:ss"));
        case 1: return e.name;
        case 2: return e.address;
        case 3: return e.group;
        case 4: return QStringLiteral("%1 → %2").arg(statusTitle(e.from), statusTitle(e.to));
        case 5: return e.text();
        case 6: return e.rttMs >= 0 ? fmtMs(e.rttMs) : QStringLiteral("—");
        }
    }
    if (role == Qt::ForegroundRole && idx.column() == 4) return Theme::statusText(e.to);
    if (role == Qt::FontRole && (idx.column() == 0 || idx.column() == 2 || idx.column() == 6)) return Theme::mono(12);
    if (role == Qt::TextAlignmentRole && idx.column() == 6) return int(Qt::AlignRight | Qt::AlignVCenter);
    if (role == Qt::UserRole) return e.id;
    return QVariant();
}

QVariant EventModel::headerData(int section, Qt::Orientation o, int role) const
{
    if (o != Qt::Horizontal || role != Qt::DisplayRole) return QVariant();
    static const char *heads[] = {"Время", "Цель", "Адрес", "Группа", "Смена", "Что случилось", "мс"};
    return QString::fromUtf8(heads[section]);
}

bool EventFilter::filterAcceptsRow(int row, const QModelIndex &) const
{
    auto *m = static_cast<EventModel *>(sourceModel());
    const StatusEvent &e = m->at(row);
    if (m_status >= 0 && int(e.to) != m_status) return false;
    if (m_text.isEmpty()) return true;
    return e.name.contains(m_text, Qt::CaseInsensitive) || e.address.contains(m_text, Qt::CaseInsensitive)
        || e.group.contains(m_text, Qt::CaseInsensitive) || e.reason.contains(m_text, Qt::CaseInsensitive);
}

HistoryDialog::HistoryDialog(Engine *engine, QWidget *parent)
    : QDialog(parent)
    , m_engine(engine)
{
    setWindowTitle(QStringLiteral("Журнал — Зарница"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(1020, 620);
    auto *root = new QVBoxLayout(this);
    auto *bar = new QHBoxLayout;
    m_search = new QLineEdit;
    m_search->setPlaceholderText(QStringLiteral("Цель, адрес, группа или причина"));
    m_search->setClearButtonEnabled(true);
    m_status = new QComboBox;
    m_status->addItem(QStringLiteral("Все смены"), -1);
    m_status->addItem(QStringLiteral("Нет связи"), int(Status::Down));
    m_status->addItem(QStringLiteral("Плохо"), int(Status::Degraded));
    m_status->addItem(QStringLiteral("В норме"), int(Status::Up));
    m_status->addItem(QStringLiteral("Пауза"), int(Status::Paused));
    m_count = new QLabel;
    m_count->setProperty("role", "muted");
    auto *exp = new QPushButton(QStringLiteral("Выгрузить CSV…"));
    bar->addWidget(m_search, 1);
    bar->addWidget(m_status);
    bar->addWidget(m_count);
    bar->addWidget(exp);
    root->addLayout(bar);

    m_model = new EventModel(this);
    m_model->setEvents(engine->log()->recent());
    m_filter = new EventFilter(this);
    m_filter->setSourceModel(m_model);
    m_view = new QTableView;
    m_view->setModel(m_filter);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setAlternatingRowColors(true);
    m_view->setShowGrid(false);
    m_view->verticalHeader()->hide();
    m_view->verticalHeader()->setDefaultSectionSize(28);
    m_view->horizontalHeader()->setStretchLastSection(false);
    m_view->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    m_view->setColumnWidth(0, 150);
    m_view->setColumnWidth(1, 200);
    m_view->setColumnWidth(2, 150);
    m_view->setColumnWidth(3, 130);
    m_view->setColumnWidth(4, 170);
    m_view->setColumnWidth(6, 60);
    root->addWidget(m_view, 1);

    auto *note = new QLabel(QStringLiteral("Файлы журнала: %1 (по месяцам, JSON Lines)").arg(QDir::toNativeSeparators(engine->log()->dir())));
    note->setProperty("role", "faint");
    note->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(note);

    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &t) { m_filter->setText(t.trimmed()); updateCount(); });
    connect(m_status, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        m_filter->setStatus(m_status->currentData().toInt());
        updateCount();
    });
    connect(exp, &QPushButton::clicked, this, &HistoryDialog::exportCsv);
    connect(engine->log(), &EventLog::appended, this, [this](const StatusEvent &e) {
        m_model->prepend(e);
        updateCount();
    });
    connect(m_view, &QTableView::doubleClicked, this, [this](const QModelIndex &idx) {
        emit openTarget(m_filter->data(idx, Qt::UserRole).toString());
    });
    updateCount();
}

void HistoryDialog::updateCount()
{
    m_count->setText(QStringLiteral("%1 из %2").arg(m_filter->rowCount()).arg(m_model->rowCount()));
}

void HistoryDialog::exportCsv()
{
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Выгрузить журнал"),
                                                      QDir::homePath() + QStringLiteral("/zarnitsa-журнал-%1.csv").arg(QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd"))),
                                                      QStringLiteral("CSV (*.csv)"));
    if (path.isEmpty()) return;
    QVector<StatusEvent> evs;
    for (int r = m_filter->rowCount() - 1; r >= 0; --r)
        evs.append(m_model->at(m_filter->mapToSource(m_filter->index(r, 0)).row()));
    QString err;
    if (!EventLog::exportCsv(evs, path, QLatin1Char(';'), &err))
        QMessageBox::warning(this, QStringLiteral("Журнал"), QStringLiteral("Не удалось сохранить: %1").arg(err));
}

} // namespace zr
