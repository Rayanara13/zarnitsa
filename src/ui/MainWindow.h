// Главное окно Зарницы.
#pragma once

#include "core/Monitor.h"

#include <QHash>
#include <QMainWindow>
#include <QPointer>
#include <QSystemTrayIcon>

class QComboBox;
class QLabel;
class QLineEdit;
class QMenu;
class QPushButton;
class QToolButton;

namespace zr {

class Engine;
class HeaderBar;
class TileView;
class ToastHost;
class CornerPopup;
class DetailDialog;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(Engine *engine, QWidget *parent = nullptr);
    ~MainWindow() override;

    TileView *tiles() const { return m_tiles; }
    void addFromText(const QString &text, bool showErrors = true);
    void handleRemote(const QStringList &hosts);
    void setWall(bool on);
    bool isWall() const { return m_wall; }
    void openDetails(const QString &id);
    void restoreState();

protected:
    void closeEvent(QCloseEvent *e) override;
    void changeEvent(QEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;

private:
    void buildMenu();
    void buildTray();
    void applyTheme(const QString &name);
    void updateTitle();
    void updateStatusBar();
    void updateTray();
    void onAlert(const StatusEvent &e);
    void showTileMenu(const QStringList &ids, const QPoint &pos);
    void showGroupMenu(const QString &group, const QPoint &pos);
    void editTarget(const QString &id);
    void newTarget(const QString &preset = QString());
    void deleteTargets(const QStringList &ids);
    void togglePause(const QStringList &ids);
    void openTraceroute(const QString &host);
    void openScan();
    void openHistory();
    void openSettings(const QString &tab = QString());
    void importFile();
    void exportTargets(const QString &format);
    void saveSet();
    void persistView();
    void cycleDensity();
    void bringToFront();
    QString currentGroup() const;

    Engine *m_engine;
    HeaderBar *m_header;
    QWidget *m_addBar;
    QLineEdit *m_input;
    QComboBox *m_groupCombo;
    TileView *m_tiles;
    ToastHost *m_toasts;
    CornerPopup *m_corner;
    QSystemTrayIcon *m_tray = nullptr;
    QMenu *m_trayMenu = nullptr;
    QLabel *m_sbTargets, *m_sbRate, *m_sbIcmp, *m_sbApi, *m_sbMsg;
    QMenu *m_mainMenu = nullptr;
    QHash<QString, QPointer<DetailDialog>> m_details;
    bool m_wall = false;
    bool m_quitting = false;
    QByteArray m_geometryBeforeWall;
    Status m_trayStatus = Status::Paused;
};

} // namespace zr
