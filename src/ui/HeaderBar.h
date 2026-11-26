// Шапка окна: логотип, счётчики-фильтры по статусам, поиск, путь ICMP и быстрые кнопки.
#pragma once

#include "core/Types.h"

#include <QSet>
#include <QWidget>

class QAction;
class QLineEdit;
class QToolButton;
class QLabel;

namespace zr {

class Engine;

class HeaderBar : public QWidget {
    Q_OBJECT
public:
    explicit HeaderBar(Engine *engine, QWidget *parent = nullptr);

    QLineEdit *search() const { return m_search; }
    QToolButton *pauseButton() const { return m_pause; }
    QToolButton *soundButton() const { return m_sound; }
    QToolButton *densityButton() const { return m_density; }
    QToolButton *themeButton() const { return m_theme; }
    QToolButton *wallButton() const { return m_wall; }
    QToolButton *menuButton() const { return m_menu; }
    void setStatusFilter(const QSet<Status> &f) { m_filter = f; update(); }
    void setWall(bool on);
    void refreshIcons();
    void updateIcmp();
    QSize sizeHint() const override;

signals:
    void statusFilterToggled(zr::Status st);
    void icmpClicked();

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;

private:
    void layoutChildren();
    struct Counter { Status st; QRect rect; };
    Engine *m_engine;
    QLineEdit *m_search;
    QToolButton *m_icmp;
    QToolButton *m_pause;
    QToolButton *m_sound;
    QToolButton *m_density;
    QToolButton *m_theme;
    QToolButton *m_wall;
    QToolButton *m_menu;
    QAction *m_searchIcon = nullptr;
    QVector<Counter> m_counters;
    QSet<Status> m_filter;
    bool m_wallMode = false;
    int m_countersRight = 0;
};

} // namespace zr
