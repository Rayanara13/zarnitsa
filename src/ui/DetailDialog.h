// Карточка цели: живые цифры, график задержки за 10 минут или сутки, журнал цели и действия.
#pragma once

#include <QDialog>
#include <QWidget>

class QLabel;
class QListWidget;
class QPushButton;

namespace zr {

class Engine;
struct TargetRuntime;

class RttChart : public QWidget {
    Q_OBJECT
public:
    enum class Range { Minutes10, Hour, Day };
    RttChart(Engine *engine, const QString &id, QWidget *parent = nullptr);
    void setRange(Range r) { m_range = r; update(); }
    QSize sizeHint() const override { return QSize(640, 220); }

protected:
    void paintEvent(QPaintEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    Engine *m_engine;
    QString m_id;
    Range m_range = Range::Minutes10;
    int m_hoverX = -1;
};

class DetailDialog : public QDialog {
    Q_OBJECT
public:
    DetailDialog(Engine *engine, const QString &id, QWidget *parent = nullptr);
    QString targetId() const { return m_id; }

signals:
    void editRequested(const QString &id);
    void traceRequested(const QString &id);

private:
    void refresh();
    void refreshEvents();

    Engine *m_engine;
    QString m_id;
    QLabel *m_title;
    QLabel *m_status;
    QLabel *m_addr;
    QVector<QLabel *> m_values;
    RttChart *m_chart;
    QListWidget *m_events;
    QPushButton *m_pause;
};

} // namespace zr
