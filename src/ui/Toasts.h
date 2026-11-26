// Оповещения на экране без трея: стопка сообщений в окне и окошко в углу экрана поверх всех окон.
// fly-wm трей может и не показать — эти два пути работают в любом оконном менеджере.
#pragma once

#include "core/Monitor.h"

#include <QWidget>

class QTimer;

namespace zr {

class ToastHost : public QWidget {
    Q_OBJECT
public:
    explicit ToastHost(QWidget *parent);
    void push(const StatusEvent &e);
    void reposition();

signals:
    void clicked(const QString &id);

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;

private:
    struct Toast { StatusEvent e; qint64 born; };
    QVector<Toast> m_toasts;
    QTimer *m_timer;
};

class CornerPopup : public QWidget {
    Q_OBJECT
public:
    CornerPopup();
    void push(const StatusEvent &e);

signals:
    void clicked(const QString &id);

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void enterEvent(QEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    void place();
    QVector<StatusEvent> m_events;
    QTimer *m_hide;
};

} // namespace zr
