// Главный вид: группы и плитки (обычные, компактные) или таблица — всё рисуется вручную,
// чтобы тысяча целей не тормозила и выглядела одинаково в Fly и где угодно.
#pragma once

#include "core/Types.h"

#include <QAbstractScrollArea>
#include <QHash>
#include <QSet>
#include <QTimer>
#include <QVector>

namespace zr {

class Engine;
class Monitor;
struct TargetConfig;
struct TargetRuntime;
struct StatusEvent;

class TileView : public QAbstractScrollArea {
    Q_OBJECT
public:
    enum class Density { Comfortable, Compact, List };
    enum class Sort { Manual, Problems, Name, Rtt };

    explicit TileView(Engine *engine, QWidget *parent = nullptr);

    void setDensity(Density d);
    Density density() const { return m_density; }
    void setSort(Sort s);
    Sort sort() const { return m_sort; }
    void setFilterText(const QString &text);
    void setStatusFilter(const QSet<Status> &f);
    QSet<Status> statusFilter() const { return m_statusFilter; }
    void setOnlyProblems(bool on);
    void setWall(bool on);
    bool isWall() const { return m_wall; }
    void setColumns(int c);
    void setShowSparkline(bool on) { m_sparkline = on; viewport()->update(); }
    void setShowMos(bool on) { m_showMos = on; viewport()->update(); }

    QStringList selection() const;
    void setSelection(const QStringList &ids);
    void selectAll();
    void clearSelection();
    QStringList visibleIds() const { return m_order; }
    void flash(const QString &id);
    void refreshTheme() { viewport()->update(); }
    void ensureVisible(const QString &id);

    static Density densityFromKey(const QString &k);
    static QString densityKey(Density d);
    static Sort sortFromKey(const QString &k);
    static QString sortKey(Sort s);

signals:
    void openDetails(const QString &id);
    void contextMenuRequested(const QStringList &ids, const QPoint &globalPos);
    void groupMenuRequested(const QString &group, const QPoint &globalPos);
    void deleteRequested(const QStringList &ids);
    void togglePauseRequested(const QStringList &ids);
    void selectionChanged();

protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void leaveEvent(QEvent *e) override;
    bool viewportEvent(QEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
    void dragLeaveEvent(QDragLeaveEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    struct Item {
        enum Type { Group, Tile, ListHead, Empty } type = Tile;
        QRect rect;           // в координатах содержимого
        QString id;
        QString group;
    };

    void scheduleRelayout();
    void relayout();
    void updateScrollBar();
    bool passesFilter(const TargetConfig &t, const TargetRuntime *rt) const;
    int itemAt(const QPoint &contentPos) const;
    QPoint toContent(const QPoint &viewportPos) const;
    QRect toViewport(const QRect &content) const;
    void onTargetUpdated(const QString &id);
    void onStatusChanged(const StatusEvent &ev);
    void flushDirty();
    void updateDrop(const QPoint &contentPos);
    QString tooltipFor(const QString &id) const;

    void paintGroup(QPainter &p, const Item &it);
    void paintEmptyGroup(QPainter &p, const Item &it);
    void paintTile(QPainter &p, const QRect &r, const QString &id);
    void paintCompact(QPainter &p, const QRect &r, const QString &id);
    void paintListHead(QPainter &p, const QRect &r);
    void paintRow(QPainter &p, const QRect &r, const QString &id);
    void paintEmptyState(QPainter &p);
    void paintBackground(QPainter &p, const QRectF &r, const QString &id, Status st, bool dependent, qreal radius);
    void paintSparkline(QPainter &p, const QRectF &area, const TargetRuntime &rt, Status st);
    QRect groupMenuRect(const QRect &header) const;
    QVector<int> listColumns(int width) const;

    Engine *m_engine;
    Monitor *m_mon;
    Density m_density = Density::Comfortable;
    Sort m_sort = Sort::Manual;
    QString m_filter;
    QSet<Status> m_statusFilter;
    bool m_onlyProblems = false;
    bool m_wall = false;
    bool m_sparkline = true;
    bool m_showMos = true;
    int m_columns = 0;
    qreal m_scale = 1.0;

    QVector<Item> m_items;
    QHash<QString, int> m_itemIndex;
    QStringList m_order;              // видимые цели по порядку
    int m_contentH = 0;
    QSet<QString> m_sel;
    QString m_anchor;
    QString m_hover;

    QTimer m_relayoutTimer;
    QTimer m_repaintTimer;
    QTimer m_animTimer;
    QTimer m_secondTimer;
    QSet<QString> m_dirty;
    QHash<QString, qint64> m_flash;

    QPoint m_pressPos;
    QString m_pressId;
    bool m_dragArmed = false;
    bool m_dropActive = false;
    QString m_dropGroup;
    QString m_dropBefore;
    QRect m_dropLine;
};

} // namespace zr
