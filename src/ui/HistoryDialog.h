// Журнал смен статусов (F12, как в vmPing): поиск, фильтр по статусу, выгрузка в CSV.
#pragma once

#include "core/Monitor.h"

#include <QAbstractTableModel>
#include <QDialog>
#include <QSortFilterProxyModel>

class QComboBox;
class QLineEdit;
class QLabel;
class QTableView;

namespace zr {

class Engine;

class EventModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit EventModel(QObject *parent = nullptr) : QAbstractTableModel(parent) {}
    void setEvents(const QVector<StatusEvent> &evs);
    void prepend(const StatusEvent &e);
    const StatusEvent &at(int row) const { return m_ev[row]; }
    int rowCount(const QModelIndex & = QModelIndex()) const override { return m_ev.size(); }
    int columnCount(const QModelIndex & = QModelIndex()) const override { return 7; }
    QVariant data(const QModelIndex &idx, int role) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;

private:
    QVector<StatusEvent> m_ev;   // новые сверху
};

class EventFilter : public QSortFilterProxyModel {
    Q_OBJECT
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    void setText(const QString &t) { m_text = t; invalidateFilter(); }
    void setStatus(int s) { m_status = s; invalidateFilter(); }

protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override;

private:
    QString m_text;
    int m_status = -1;
};

class HistoryDialog : public QDialog {
    Q_OBJECT
public:
    HistoryDialog(Engine *engine, QWidget *parent = nullptr);

signals:
    void openTarget(const QString &id);

private:
    void exportCsv();
    void updateCount();

    Engine *m_engine;
    EventModel *m_model;
    EventFilter *m_filter;
    QTableView *m_view;
    QLineEdit *m_search;
    QComboBox *m_status;
    QLabel *m_count;
};

} // namespace zr
