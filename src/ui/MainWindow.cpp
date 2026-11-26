#include "MainWindow.h"

#include "DetailDialog.h"
#include "Dialogs.h"
#include "HeaderBar.h"
#include "HistoryDialog.h"
#include "Icons.h"
#include "ImportDialogs.h"
#include "Theme.h"
#include "TileView.h"
#include "Toasts.h"
#include "ToolsDialogs.h"
#include "api/ApiController.h"
#include "core/EventLog.h"
#include "integrations/Importers.h"
#include "probes/IcmpEngine.h"
#include "probes/ProbeDispatcher.h"
#include "service/Alerter.h"
#include "service/Engine.h"
#include "service/Sound.h"

#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QShortcut>
#include <QStatusBar>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindowStateChangeEvent>

namespace zr {

// Поле «добавить»: вставка нескольких строк добавляет сразу список
class AddLineEdit : public QLineEdit {
public:
    std::function<void(const QString &)> onMultiPaste;
    using QLineEdit::QLineEdit;

protected:
    void keyPressEvent(QKeyEvent *e) override
    {
        if (e->matches(QKeySequence::Paste)) {
            const QString text = QApplication::clipboard()->text();
            if (text.contains(QLatin1Char('\n')) && onMultiPaste) {
                onMultiPaste(text);
                return;
            }
        }
        QLineEdit::keyPressEvent(e);
    }
};

MainWindow::MainWindow(Engine *engine, QWidget *parent)
    : QMainWindow(parent)
    , m_engine(engine)
{
    setWindowIcon(Icons::appIcon());
    setMinimumSize(760, 480);
    resize(1320, 820);

    auto *central = new QWidget;
    auto *v = new QVBoxLayout(central);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    m_header = new HeaderBar(engine);
    m_header->setFixedHeight(m_header->sizeHint().height());
    v->addWidget(m_header);

    // строка добавления
    m_addBar = new QWidget;
    m_addBar->setAutoFillBackground(true);
    auto *ab = new QHBoxLayout(m_addBar);
    ab->setContentsMargins(16, 10, 16, 10);
    ab->setSpacing(8);
    auto *input = new AddLineEdit;
    m_input = input;
    m_input->setPlaceholderText(QStringLiteral("Адрес, хост:порт, https://…, sip:хост — Enter добавит; можно вставить сразу список"));
    m_input->addAction(Icons::glyph(Glyph::Plus, Theme::p().faint, 16), QLineEdit::LeadingPosition);
    input->onMultiPaste = [this](const QString &text) { addFromText(text); };
    m_groupCombo = new QComboBox;
    m_groupCombo->setEditable(true);
    m_groupCombo->setMinimumWidth(190);
    m_groupCombo->lineEdit()->setPlaceholderText(QStringLiteral("Без группы"));
    m_groupCombo->setToolTip(QStringLiteral("В какую группу добавлять"));
    auto *add = new QPushButton(QStringLiteral("Добавить"));
    add->setProperty("accent", true);
    auto *more = new QToolButton;
    more->setText(QStringLiteral("Шаблон"));
    more->setToolButtonStyle(Qt::ToolButtonTextOnly);
    more->setPopupMode(QToolButton::InstantPopup);
    auto *presetMenu = new QMenu(more);
    for (const TargetPreset &p : targetPresets()) {
        const QString key = p.key;
        presetMenu->addAction(p.title, this, [this, key]() { newTarget(key); });
    }
    more->setMenu(presetMenu);
    auto *imp = new QToolButton;
    imp->setText(QStringLiteral("Импорт"));
    imp->setToolButtonStyle(Qt::ToolButtonTextOnly);
    imp->setPopupMode(QToolButton::InstantPopup);
    auto *impMenu = new QMenu(imp);
    impMenu->addAction(QStringLiteral("Из файла: TXT, CSV, vmPing.xml, JSON…"), this, &MainWindow::importFile);
    impMenu->addAction(QStringLiteral("Телефоны из Инея…"), this, [this]() { SourceConfig s; s.kind = QStringLiteral("inei"); runSourceWizard(m_engine, s, this); });
    impMenu->addAction(QStringLiteral("Абоненты из Росы…"), this, [this]() { SourceConfig s; s.kind = QStringLiteral("rosa"); runSourceWizard(m_engine, s, this); });
    impMenu->addSeparator();
    impMenu->addAction(QStringLiteral("Источники…"), this, [this]() { SourcesDialog d(m_engine, this); d.exec(); });
    imp->setMenu(impMenu);
    ab->addWidget(m_input, 1);
    ab->addWidget(m_groupCombo);
    ab->addWidget(add);
    ab->addWidget(more);
    ab->addWidget(imp);
    v->addWidget(m_addBar);

    m_tiles = new TileView(engine);
    v->addWidget(m_tiles, 1);
    setCentralWidget(central);

    m_toasts = new ToastHost(m_tiles);
    m_corner = new CornerPopup;

    // строка состояния
    m_sbTargets = new QLabel;
    m_sbRate = new QLabel;
    m_sbIcmp = new QLabel;
    m_sbApi = new QLabel;
    m_sbMsg = new QLabel;
    statusBar()->addWidget(m_sbTargets);
    statusBar()->addWidget(m_sbRate);
    statusBar()->addWidget(m_sbIcmp);
    statusBar()->addWidget(m_sbApi);
    statusBar()->addWidget(m_sbMsg, 1);
    statusBar()->setSizeGripEnabled(false);

    connect(m_input, &QLineEdit::returnPressed, this, [this]() { addFromText(m_input->text()); });
    connect(add, &QPushButton::clicked, this, [this]() {
        if (m_input->text().trimmed().isEmpty()) newTarget();
        else addFromText(m_input->text());
    });
    connect(m_header->search(), &QLineEdit::textChanged, m_tiles, &TileView::setFilterText);
    connect(m_header, &HeaderBar::statusFilterToggled, this, [this](Status st) {
        QSet<Status> f = m_tiles->statusFilter();
        if (f.contains(st)) f.remove(st);
        else f = {st};
        m_tiles->setStatusFilter(f);
        m_header->setStatusFilter(f);
    });
    connect(m_header, &HeaderBar::icmpClicked, this, [this]() { openSettings(QStringLiteral("ICMP")); });
    connect(m_header->pauseButton(), &QToolButton::toggled, this, [this](bool paused) {
        if (paused) m_engine->monitor()->stop();
        else m_engine->monitor()->start();
        m_header->refreshIcons();
        updateStatusBar();
    });
    connect(m_header->soundButton(), &QToolButton::toggled, this, [this](bool on) {
        Settings s = m_engine->settings();
        s.alertSound = on;
        m_engine->applySettings(s);
        m_header->refreshIcons();
    });
    connect(m_header->densityButton(), &QToolButton::clicked, this, &MainWindow::cycleDensity);
    connect(m_header->themeButton(), &QToolButton::clicked, this, [this]() {
        applyTheme(Theme::isDark() ? QStringLiteral("light") : QStringLiteral("dark"));
    });
    connect(m_header->wallButton(), &QToolButton::clicked, this, [this]() { setWall(!m_wall); });

    connect(m_tiles, &TileView::openDetails, this, &MainWindow::openDetails);
    connect(m_tiles, &TileView::contextMenuRequested, this, &MainWindow::showTileMenu);
    connect(m_tiles, &TileView::groupMenuRequested, this, &MainWindow::showGroupMenu);
    connect(m_tiles, &TileView::deleteRequested, this, &MainWindow::deleteTargets);
    connect(m_tiles, &TileView::togglePauseRequested, this, &MainWindow::togglePause);
    connect(m_toasts, &ToastHost::clicked, this, &MainWindow::openDetails);
    connect(m_corner, &CornerPopup::clicked, this, [this](const QString &id) {
        bringToFront();
        openDetails(id);
    });

    Monitor *mon = engine->monitor();
    connect(mon, &Monitor::summaryChanged, this, [this]() {
        updateTitle();
        updateTray();
    });
    connect(mon, &Monitor::targetsChanged, this, [this]() {
        const QString keep = m_groupCombo->currentText();
        m_groupCombo->blockSignals(true);
        m_groupCombo->clear();
        for (const GroupInfo &g : m_engine->monitor()->groups())
            if (!g.name.isEmpty()) m_groupCombo->addItem(g.name);
        m_groupCombo->setCurrentText(keep);
        m_groupCombo->blockSignals(false);
        updateStatusBar();
    });
    connect(engine->alerter(), &Alerter::alert, this, &MainWindow::onAlert);
    connect(engine->alerter(), &Alerter::actionFailed, this, [this](const QString &what) {
        m_sbMsg->setText(what);
        QTimer::singleShot(8000, m_sbMsg, [this]() { m_sbMsg->clear(); });
    });
    connect(engine->sound(), &Sound::fallbackBeep, this, []() { QApplication::beep(); });
    connect(engine->icmp(), &IcmpEngine::modeChanged, this, &MainWindow::updateStatusBar);
    connect(engine, &Engine::apiStateChanged, this, &MainWindow::updateStatusBar);
    connect(engine, &Engine::sourceSynced, this, [this](const QString &, bool ok, const QString &report) {
        m_sbMsg->setText(ok ? QStringLiteral("Синхронизация: %1").arg(report.section(QLatin1Char('\n'), 0, 0))
                            : QStringLiteral("Синхронизация не удалась: %1").arg(report));
        QTimer::singleShot(10000, m_sbMsg, [this]() { m_sbMsg->clear(); });
    });
    auto *tick = new QTimer(this);
    connect(tick, &QTimer::timeout, this, &MainWindow::updateStatusBar);
    tick->start(1000);

    // клавиши
    auto sc = [this](const QKeySequence &k, std::function<void()> f) {
        auto *s = new QShortcut(k, this);
        connect(s, &QShortcut::activated, this, f);
    };
    sc(QKeySequence(QStringLiteral("Ctrl+N")), [this]() { newTarget(); });
    sc(QKeySequence(QStringLiteral("Ctrl+L")), [this]() { m_input->setFocus(); m_input->selectAll(); });
    sc(QKeySequence::Find, [this]() { m_header->search()->setFocus(); m_header->search()->selectAll(); });
    sc(QKeySequence(Qt::Key_F12), [this]() { openHistory(); });
    sc(QKeySequence(Qt::Key_F11), [this]() { setWall(!m_wall); });
    sc(QKeySequence(QStringLiteral("Ctrl+T")), [this]() {
        const QStringList sel = m_tiles->selection();
        const TargetConfig *t = sel.isEmpty() ? nullptr : m_engine->monitor()->target(sel.first());
        openTraceroute(t ? t->hostPart() : QString());
    });
    sc(QKeySequence(QStringLiteral("Ctrl+,")), [this]() { openSettings(); });
    sc(QKeySequence(QStringLiteral("Ctrl+I")), [this]() { importFile(); });
    sc(QKeySequence::Quit, [this]() { m_quitting = true; close(); });

    buildMenu();
    buildTray();
    m_header->soundButton()->blockSignals(true);
    m_header->soundButton()->setChecked(engine->settings().alertSound);
    m_header->soundButton()->blockSignals(false);
    m_header->refreshIcons();

    const Settings &s = engine->settings();
    m_tiles->setDensity(TileView::densityFromKey(s.density));
    m_tiles->setSort(TileView::sortFromKey(s.sortMode));
    m_tiles->setColumns(s.columns);
    m_tiles->setOnlyProblems(s.onlyProblems);
    m_tiles->setShowSparkline(s.showSparkline);
    m_tiles->setShowMos(s.showMos);
    if (s.alwaysOnTop) setWindowFlag(Qt::WindowStaysOnTopHint, true);
    emit mon->targetsChanged();
    updateTitle();
    updateStatusBar();
}

MainWindow::~MainWindow()
{
    delete m_corner;
}

void MainWindow::restoreState()
{
    const QByteArray geo = QByteArray::fromBase64(m_engine->config().ui.value(QStringLiteral("geometry")).toString().toLatin1());
    if (!geo.isEmpty()) restoreGeometry(geo);
}

void MainWindow::persistView()
{
    Settings s = m_engine->settings();
    s.density = TileView::densityKey(m_tiles->density());
    s.sortMode = TileView::sortKey(m_tiles->sort());
    s.theme = Theme::name();
    m_engine->applySettings(s);
}

void MainWindow::applyTheme(const QString &name)
{
    Theme::apply(qApp, name);
    m_header->refreshIcons();
    m_tiles->refreshTheme();
    m_addBar->setPalette(QPalette(Theme::p().canvas));
    m_addBar->setStyleSheet(QStringLiteral("background:%1;").arg(Theme::p().canvas.name()));
    m_input->setStyleSheet(QString());
    buildMenu();
    updateTray();
    persistView();
    update();
}

QString MainWindow::currentGroup() const
{
    return m_groupCombo->currentText().trimmed();
}

void MainWindow::addFromText(const QString &text, bool showErrors)
{
    QString t = text;
    t.replace(QLatin1Char('\r'), QString());
    const QStringList lines = t.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (lines.isEmpty()) return;
    if (lines.size() > 1) {
        const ImportResult r = parseTextList(t, currentGroup());
        if (r.targets.isEmpty()) {
            if (showErrors) QMessageBox::warning(this, QStringLiteral("Добавить"), r.warnings.join(QLatin1Char('\n')));
            return;
        }
        if (r.targets.size() > 3) {
            ImportPreviewDialog dlg(m_engine, r, QStringLiteral("Добавить список"), this);
            if (dlg.exec() != QDialog::Accepted) return;
            m_engine->monitor()->addTargets(dlg.selected());
        } else {
            m_engine->monitor()->addTargets(r.targets);
        }
        m_input->clear();
        return;
    }
    ParsedInput p = parseTargetInput(lines.first());
    if (!p.ok) {
        m_sbMsg->setText(p.error);
        m_input->setStyleSheet(QStringLiteral("border: 1px solid %1;").arg(Theme::p().bad.name()));
        QTimer::singleShot(2500, m_input, [this]() { m_input->setStyleSheet(QString()); m_sbMsg->clear(); });
        return;
    }
    p.target.group = currentGroup();
    const QString id = m_engine->monitor()->addTarget(p.target);
    m_input->clear();
    m_tiles->setSelection({id});
    QTimer::singleShot(0, this, [this, id]() { m_tiles->ensureVisible(id); });
}

void MainWindow::handleRemote(const QStringList &hosts)
{
    bringToFront();
    if (!hosts.isEmpty())
        addFromText(hosts.join(QLatin1Char('\n')), false);
}

void MainWindow::bringToFront()
{
    if (isHidden()) show();
    if (isMinimized()) showNormal();
    raise();
    activateWindow();
}

void MainWindow::updateTitle()
{
    const Summary s = m_engine->monitor()->summary();
    QString t = QString::fromUtf8(kAppTitle);
    if (s.down) t = QStringLiteral("%1 нет связи — %2").arg(s.down).arg(t);
    else if (s.degraded) t = QStringLiteral("%1 плохо — %2").arg(s.degraded).arg(t);
    if (m_engine->isDemo()) t += QStringLiteral(" (демо)");
    setWindowTitle(t);
}

void MainWindow::updateStatusBar()
{
    const Summary s = m_engine->monitor()->summary();
    m_sbTargets->setText(QStringLiteral("Целей: %1").arg(s.total));
    const bool paused = !m_engine->monitor()->isRunning();
    m_sbRate->setText(paused ? QStringLiteral("проверки на паузе")
                             : QStringLiteral("%1 замеров/с").arg(fmtNum(m_engine->dispatcher()->probesPerSecond(), 0)));
    IcmpEngine *icmp = m_engine->icmp();
    m_sbIcmp->setText(m_engine->isDemo() ? QStringLiteral("демо: сеть не опрашивается") : QStringLiteral("ICMP: %1").arg(icmp->modeTitle()));
    m_sbIcmp->setToolTip(icmp->modeHint());
    ApiController *api = m_engine->api();
    m_sbApi->setText(api->isRunning() ? QStringLiteral("API: %1").arg(api->address())
                                      : (m_engine->apiError().isEmpty() ? QString() : QStringLiteral("API: ошибка")));
    m_sbApi->setToolTip(m_engine->apiError());
}

void MainWindow::buildTray()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;
    m_tray = new QSystemTrayIcon(this);
    m_trayMenu = new QMenu(this);
    m_trayMenu->addAction(QStringLiteral("Показать окно"), this, &MainWindow::bringToFront);
    m_trayMenu->addAction(QStringLiteral("Журнал"), this, [this]() { bringToFront(); openHistory(); });
    m_trayMenu->addSeparator();
    m_trayMenu->addAction(QStringLiteral("Выход"), this, [this]() { m_quitting = true; close(); });
    m_tray->setContextMenu(m_trayMenu);
    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
        if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::DoubleClick) {
            if (isVisible() && !isMinimized() && isActiveWindow()) hide();
            else bringToFront();
        }
    });
    connect(m_tray, &QSystemTrayIcon::messageClicked, this, &MainWindow::bringToFront);
    updateTray();
    m_tray->show();
}

void MainWindow::updateTray()
{
    if (!m_tray) return;
    const Summary s = m_engine->monitor()->summary();
    const Status worst = s.worst();
    if (worst != m_trayStatus) {
        m_trayStatus = worst;
        QColor dot;
        if (worst == Status::Down) dot = Theme::p().bad;
        else if (worst == Status::Degraded) dot = Theme::p().warn;
        else if (worst == Status::Up) dot = Theme::p().ok;
        m_tray->setIcon(Icons::appIcon(dot));
    }
    m_tray->setToolTip(QStringLiteral("Зарница: %1 в норме, %2 плохо, %3 нет связи").arg(s.up).arg(s.degraded).arg(s.down));
}

void MainWindow::onAlert(const StatusEvent &e)
{
    const Settings &s = m_engine->settings();
    const bool focused = isActiveWindow() && !isMinimized() && isVisible();
    if (s.alertToast) m_toasts->push(e);
    if (s.alertCorner && !focused) m_corner->push(e);
    if (s.alertTray && m_tray && !focused)
        m_tray->showMessage(e.name, QStringLiteral("%1 · %2").arg(e.text(), e.address),
                            e.to == Status::Up ? QSystemTrayIcon::Information : QSystemTrayIcon::Warning, 6000);
    if (s.alertSound) m_engine->sound()->play(e.to == Status::Down || e.to == Status::Degraded);
}

void MainWindow::buildMenu()
{
    delete m_mainMenu;
    m_mainMenu = new QMenu(this);
    const Palette &pal = Theme::p();
    QMenu *m = m_mainMenu;
    m->addAction(Icons::glyph(Glyph::Plus, pal.muted), QStringLiteral("Новая цель…\tCtrl+N"), this, [this]() { newTarget(); });
    QMenu *imp = m->addMenu(Icons::glyph(Glyph::Import, pal.muted), QStringLiteral("Импорт"));
    imp->addAction(QStringLiteral("Из файла: TXT, CSV, vmPing.xml, JSON…\tCtrl+I"), this, &MainWindow::importFile);
    imp->addAction(QStringLiteral("Телефоны из Инея…"), this, [this]() { SourceConfig s; s.kind = QStringLiteral("inei"); runSourceWizard(m_engine, s, this); });
    imp->addAction(QStringLiteral("Абоненты из Росы…"), this, [this]() { SourceConfig s; s.kind = QStringLiteral("rosa"); runSourceWizard(m_engine, s, this); });
    imp->addAction(QStringLiteral("Источники…"), this, [this]() { SourcesDialog d(m_engine, this); d.exec(); });
    QMenu *exp = m->addMenu(Icons::glyph(Glyph::Export, pal.muted), QStringLiteral("Экспорт"));
    exp->addAction(QStringLiteral("CSV — для таблиц…"), this, [this]() { exportTargets(QStringLiteral("csv")); });
    exp->addAction(QStringLiteral("Список TXT — как в vmPing…"), this, [this]() { exportTargets(QStringLiteral("txt")); });
    exp->addAction(QStringLiteral("JSON Зарницы — со всеми настройками целей…"), this, [this]() { exportTargets(QStringLiteral("json")); });
    QMenu *sets = m->addMenu(QStringLiteral("Наборы"));
    connect(sets, &QMenu::aboutToShow, this, [this, sets]() {
        sets->clear();
        sets->addAction(QStringLiteral("Сохранить всё как набор…"), this, &MainWindow::saveSet);
        if (!m_engine->config().sets.isEmpty()) sets->addSeparator();
        for (const TargetSet &ts : m_engine->config().sets) {
            QMenu *one = sets->addMenu(QStringLiteral("%1  (%2)").arg(ts.name).arg(ts.targets.size()));
            const QString name = ts.name;
            one->addAction(QStringLiteral("Открыть вместо текущих"), this, [this, name]() {
                if (QMessageBox::question(this, QStringLiteral("Набор"), QStringLiteral("Заменить текущие цели набором «%1»?").arg(name)) == QMessageBox::Yes)
                    m_engine->openSet(name, true);
            });
            one->addAction(QStringLiteral("Добавить к текущим"), this, [this, name]() { m_engine->openSet(name, false); });
            one->addSeparator();
            one->addAction(QStringLiteral("Удалить набор"), this, [this, name]() { m_engine->removeSet(name); });
        }
    });
    m->addAction(QStringLiteral("Новая группа…"), this, [this]() {
        bool ok = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("Новая группа"), QStringLiteral("Название"), QLineEdit::Normal, QString(), &ok).trimmed();
        if (ok && !name.isEmpty()) {
            m_engine->monitor()->addGroup(name);
            m_groupCombo->setCurrentText(name);
        }
    });
    m->addSeparator();
    m->addAction(Icons::glyph(Glyph::Clock, pal.muted), QStringLiteral("Журнал\tF12"), this, &MainWindow::openHistory);
    m->addAction(Icons::glyph(Glyph::Route, pal.muted), QStringLiteral("Трассировка…\tCtrl+T"), this, [this]() {
        const QStringList sel = m_tiles->selection();
        const TargetConfig *t = sel.isEmpty() ? nullptr : m_engine->monitor()->target(sel.first());
        openTraceroute(t ? t->hostPart() : QString());
    });
    m->addAction(Icons::glyph(Glyph::Radar, pal.muted), QStringLiteral("Сканирование подсети…"), this, &MainWindow::openScan);
    m->addAction(Icons::glyph(Glyph::Globe, pal.muted), QStringLiteral("DNS…"), this, [this]() { (new DnsDialog(this))->show(); });
    m->addSeparator();
    QMenu *view = m->addMenu(QStringLiteral("Вид"));
    auto addCheck = [](QMenu *menu, const QString &title, bool on, std::function<void(bool)> f) {
        QAction *a = menu->addAction(title);
        a->setCheckable(true);
        a->setChecked(on);
        QObject::connect(a, &QAction::toggled, menu, f);
        return a;
    };
    const TileView::Density d = m_tiles->density();
    auto *dens = new QActionGroup(view);
    for (auto pr : {qMakePair(TileView::Density::Comfortable, QStringLiteral("Плитки")),
                    qMakePair(TileView::Density::Compact, QStringLiteral("Компактно")),
                    qMakePair(TileView::Density::List, QStringLiteral("Таблица"))}) {
        QAction *a = addCheck(view, pr.second, d == pr.first, [this, pr](bool on) {
            if (on) { m_tiles->setDensity(pr.first); persistView(); m_header->refreshIcons(); }
        });
        dens->addAction(a);
    }
    view->addSeparator();
    auto *sorts = new QActionGroup(view);
    for (auto pr : {qMakePair(TileView::Sort::Manual, QStringLiteral("Порядок: как расставлено")),
                    qMakePair(TileView::Sort::Problems, QStringLiteral("Порядок: проблемные сверху")),
                    qMakePair(TileView::Sort::Name, QStringLiteral("Порядок: по имени")),
                    qMakePair(TileView::Sort::Rtt, QStringLiteral("Порядок: по задержке"))}) {
        QAction *a = addCheck(view, pr.second, m_tiles->sort() == pr.first, [this, pr](bool on) {
            if (on) { m_tiles->setSort(pr.first); persistView(); }
        });
        sorts->addAction(a);
    }
    view->addSeparator();
    addCheck(view, QStringLiteral("Только проблемные"), m_engine->settings().onlyProblems, [this](bool on) {
        m_tiles->setOnlyProblems(on);
        Settings s = m_engine->settings();
        s.onlyProblems = on;
        m_engine->applySettings(s);
    });
    addCheck(view, QStringLiteral("Поверх всех окон"), m_engine->settings().alwaysOnTop, [this](bool on) {
        setWindowFlag(Qt::WindowStaysOnTopHint, on);
        show();
        Settings s = m_engine->settings();
        s.alwaysOnTop = on;
        m_engine->applySettings(s);
    });
    view->addAction(QStringLiteral("Экран дежурного\tF11"), this, [this]() { setWall(!m_wall); });
    view->addAction(Theme::isDark() ? QStringLiteral("Светлая тема") : QStringLiteral("Тёмная тема"), this, [this]() {
        applyTheme(Theme::isDark() ? QStringLiteral("light") : QStringLiteral("dark"));
    });
    m->addAction(QStringLiteral("Настройки…\tCtrl+,"), this, [this]() { openSettings(); });
    m->addAction(QStringLiteral("О программе"), this, [this]() { AboutDialog d(m_engine, this); d.exec(); });
    m->addSeparator();
    m->addAction(QStringLiteral("Выход\tCtrl+Q"), this, [this]() { m_quitting = true; close(); });
    m_header->menuButton()->setMenu(m_mainMenu);
}

void MainWindow::cycleDensity()
{
    const TileView::Density next = m_tiles->density() == TileView::Density::Comfortable ? TileView::Density::Compact
        : m_tiles->density() == TileView::Density::Compact ? TileView::Density::List : TileView::Density::Comfortable;
    m_tiles->setDensity(next);
    persistView();
    m_header->refreshIcons();
    buildMenu();
}

void MainWindow::setWall(bool on)
{
    if (on == m_wall) return;
    m_wall = on;
    m_addBar->setVisible(!on);
    statusBar()->setVisible(!on);
    m_header->setWall(on);
    m_tiles->setWall(on);
    if (on) {
        m_geometryBeforeWall = saveGeometry();
        showFullScreen();
    } else {
        showNormal();
        if (!m_geometryBeforeWall.isEmpty()) restoreGeometry(m_geometryBeforeWall);
    }
}

void MainWindow::openDetails(const QString &id)
{
    if (!m_engine->monitor()->target(id)) return;
    QPointer<DetailDialog> d = m_details.value(id);
    if (!d) {
        d = new DetailDialog(m_engine, id, this);
        connect(d, &DetailDialog::editRequested, this, &MainWindow::editTarget);
        connect(d, &DetailDialog::traceRequested, this, [this](const QString &tid) {
            if (const TargetConfig *t = m_engine->monitor()->target(tid)) openTraceroute(t->hostPart());
        });
        m_details.insert(id, d);
    }
    d->show();
    d->raise();
    d->activateWindow();
}

void MainWindow::newTarget(const QString &preset)
{
    TargetConfig t;
    t.group = currentGroup();
    QString host = m_input->text().trimmed();
    if (!preset.isEmpty()) {
        for (const TargetPreset &p : targetPresets()) {
            if (p.key != preset) continue;
            t = applyPreset(p, host.isEmpty() ? QString() : host, QString());
            if (host.isEmpty() && p.kind == ProbeKind::Http) t.address = QString(p.pathOrScheme).replace(QStringLiteral("{host}"), QStringLiteral("10.0.0.1"));
            t.group = currentGroup();
        }
    } else if (!host.isEmpty()) {
        const ParsedInput p = parseTargetInput(host);
        if (p.ok) { t = p.target; t.group = currentGroup(); }
    }
    TargetDialog dlg(m_engine, t, true, this);
    if (dlg.exec() == QDialog::Accepted) {
        const QString id = m_engine->monitor()->addTarget(dlg.result());
        m_input->clear();
        m_tiles->setSelection({id});
    }
}

void MainWindow::editTarget(const QString &id)
{
    const TargetConfig *t = m_engine->monitor()->target(id);
    if (!t) return;
    TargetDialog dlg(m_engine, *t, false, this);
    if (dlg.exec() == QDialog::Accepted)
        m_engine->monitor()->updateTarget(dlg.result());
}

void MainWindow::deleteTargets(const QStringList &ids)
{
    if (ids.isEmpty()) return;
    const TargetConfig *first = m_engine->monitor()->target(ids.first());
    const QString what = ids.size() == 1 && first ? QStringLiteral("«%1»").arg(first->displayName())
                                                  : QStringLiteral("%1 целей").arg(ids.size());
    if (QMessageBox::question(this, QStringLiteral("Удалить"), QStringLiteral("Удалить %1?").arg(what)) != QMessageBox::Yes)
        return;
    m_engine->monitor()->removeTargets(ids);
}

void MainWindow::togglePause(const QStringList &ids)
{
    bool anyOn = false;
    for (const QString &id : ids)
        if (const TargetConfig *t = m_engine->monitor()->target(id)) anyOn |= t->enabled;
    m_engine->monitor()->setEnabled(ids, !anyOn);
}

void MainWindow::showTileMenu(const QStringList &ids, const QPoint &pos)
{
    if (ids.isEmpty()) return;
    Monitor *mon = m_engine->monitor();
    const TargetConfig *t = mon->target(ids.first());
    if (!t) return;
    const Palette &pal = Theme::p();
    QMenu menu(this);
    if (ids.size() == 1) {
        menu.addAction(QStringLiteral("Карточка"), this, [this, id = ids.first()]() { openDetails(id); });
        menu.addAction(Icons::glyph(Glyph::Edit, pal.muted), QStringLiteral("Изменить…"), this, [this, id = ids.first()]() { editTarget(id); });
    }
    bool anyOn = false;
    for (const QString &id : ids)
        if (const TargetConfig *x = mon->target(id)) anyOn |= x->enabled;
    menu.addAction(Icons::glyph(anyOn ? Glyph::Pause : Glyph::Play, pal.muted), anyOn ? QStringLiteral("Пауза") : QStringLiteral("Продолжить"),
                   this, [this, ids]() { togglePause(ids); });
    if (ids.size() == 1 && t->isNetworkHost()) {
        menu.addAction(Icons::glyph(Glyph::Route, pal.muted), QStringLiteral("Трассировка"), this, [this, host = t->hostPart()]() { openTraceroute(host); });
        menu.addAction(Icons::glyph(Glyph::Copy, pal.muted), QStringLiteral("Копировать адрес"), this, [t]() {
            QApplication::clipboard()->setText(t->kind == ProbeKind::Http ? t->address : t->hostPart());
        });
        if (t->kind == ProbeKind::Http)
            menu.addAction(Icons::glyph(Glyph::Globe, pal.muted), QStringLiteral("Открыть в браузере"), this, [url = t->address]() {
                QDesktopServices::openUrl(QUrl(url));
            });
    }
    QMenu *move = menu.addMenu(QStringLiteral("Переместить в группу"));
    for (const GroupInfo &g : mon->groups()) {
        const QString name = g.name;
        move->addAction(name.isEmpty() ? QStringLiteral("Без группы") : name, this, [mon, ids, name]() { mon->moveTargets(ids, name, QString()); });
    }
    move->addSeparator();
    move->addAction(QStringLiteral("Новая группа…"), this, [this, mon, ids]() {
        bool ok = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("Новая группа"), QStringLiteral("Название"), QLineEdit::Normal, QString(), &ok).trimmed();
        if (ok && !name.isEmpty()) mon->moveTargets(ids, name, QString());
    });
    if (ids.size() == 1) {
        QMenu *dep = menu.addMenu(Icons::glyph(Glyph::Link, pal.muted), QStringLiteral("Зависит от"));
        dep->addAction(QStringLiteral("— ни от чего"), this, [mon, id = ids.first()]() {
            if (const TargetConfig *x = mon->target(id)) { TargetConfig c = *x; c.parentId.clear(); mon->updateTarget(c); }
        });
        for (const TargetConfig &o : mon->targets()) {
            if (o.id == t->id || o.group != t->group) continue;
            const QString pid = o.id;
            QAction *a = dep->addAction(o.displayName(), this, [mon, id = ids.first(), pid]() {
                if (const TargetConfig *x = mon->target(id)) { TargetConfig c = *x; c.parentId = pid; mon->updateTarget(c); }
            });
            a->setCheckable(true);
            a->setChecked(t->parentId == pid);
        }
    }
    menu.addAction(QStringLiteral("Сбросить статистику"), this, [mon, ids]() { mon->resetStats(ids); });
    menu.addSeparator();
    menu.addAction(Icons::glyph(Glyph::Trash, pal.badText), QStringLiteral("Удалить…"), this, [this, ids]() { deleteTargets(ids); });
    menu.exec(pos);
}

void MainWindow::showGroupMenu(const QString &group, const QPoint &pos)
{
    Monitor *mon = m_engine->monitor();
    QStringList ids;
    bool anyOn = false;
    for (const TargetConfig &t : mon->targets()) {
        if (t.group != group) continue;
        ids << t.id;
        anyOn |= t.enabled;
    }
    QMenu menu(this);
    if (!group.isEmpty())
        menu.addAction(QStringLiteral("Переименовать…"), this, [this, mon, group]() {
            bool ok = false;
            const QString name = QInputDialog::getText(this, QStringLiteral("Группа"), QStringLiteral("Новое название"), QLineEdit::Normal, group, &ok).trimmed();
            if (ok && !name.isEmpty()) mon->renameGroup(group, name);
        });
    menu.addAction(QStringLiteral("Добавить цель сюда…"), this, [this, group]() {
        m_groupCombo->setCurrentText(group);
        newTarget();
    });
    if (!ids.isEmpty()) {
        menu.addAction(anyOn ? QStringLiteral("Пауза всей группы") : QStringLiteral("Продолжить всю группу"), this,
                       [mon, ids, anyOn]() { mon->setEnabled(ids, !anyOn); });
        menu.addAction(QStringLiteral("Выделить все цели группы"), this, [this, ids]() { m_tiles->setSelection(ids); });
        menu.addAction(QStringLiteral("Сохранить группу как набор…"), this, [this, mon, group]() {
            bool ok = false;
            const QString name = QInputDialog::getText(this, QStringLiteral("Набор"), QStringLiteral("Название набора"), QLineEdit::Normal, group, &ok).trimmed();
            if (!ok || name.isEmpty()) return;
            QVector<TargetConfig> list;
            for (const TargetConfig &t : mon->targets())
                if (t.group == group) list.append(t);
            m_engine->saveSet(name, list);
        });
    }
    const QVector<GroupInfo> gs = mon->groups();
    int idx = -1;
    for (int i = 0; i < gs.size(); ++i) if (gs[i].name == group) idx = i;
    if (idx > 0) menu.addAction(QStringLiteral("Выше"), this, [mon, group, idx]() { mon->moveGroup(group, idx - 1); });
    if (idx >= 0 && idx < gs.size() - 1) menu.addAction(QStringLiteral("Ниже"), this, [mon, group, idx]() { mon->moveGroup(group, idx + 1); });
    menu.addSeparator();
    menu.addAction(QStringLiteral("Удалить группу, цели оставить"), this, [mon, group]() { mon->removeGroup(group, false); });
    if (!ids.isEmpty())
        menu.addAction(QStringLiteral("Удалить группу вместе с целями…"), this, [this, mon, group, ids]() {
            if (QMessageBox::question(this, QStringLiteral("Удалить"), QStringLiteral("Удалить группу «%1» и %2 целей?").arg(group).arg(ids.size())) == QMessageBox::Yes)
                mon->removeGroup(group, true);
        });
    menu.exec(pos);
}

void MainWindow::openTraceroute(const QString &host)
{
    auto *d = new TracerouteDialog(m_engine, host, this);
    d->show();
    if (!host.isEmpty()) d->start();
}

void MainWindow::openScan()
{
    QString guess;
    const QStringList sel = m_tiles->selection();
    if (!sel.isEmpty())
        if (const TargetConfig *t = m_engine->monitor()->target(sel.first()))
            if (isValidIpv4(t->hostPart())) guess = t->hostPart().section(QLatin1Char('.'), 0, 2) + QStringLiteral(".0/24");
    (new ScanDialog(m_engine, guess, this))->show();
}

void MainWindow::openHistory()
{
    auto *d = new HistoryDialog(m_engine, this);
    connect(d, &HistoryDialog::openTarget, this, &MainWindow::openDetails);
    d->show();
}

void MainWindow::openSettings(const QString &tab)
{
    SettingsDialog dlg(m_engine, this);
    if (!tab.isEmpty()) dlg.showTab(tab);
    if (dlg.exec() != QDialog::Accepted) return;
    const Settings s = dlg.result();
    const QString oldTheme = m_engine->settings().theme;
    m_engine->applySettings(s);
    m_tiles->setDensity(TileView::densityFromKey(s.density));
    m_tiles->setColumns(s.columns);
    m_tiles->setShowSparkline(s.showSparkline);
    m_tiles->setShowMos(s.showMos);
    setWindowFlag(Qt::WindowStaysOnTopHint, s.alwaysOnTop);
    show();
    m_header->soundButton()->blockSignals(true);
    m_header->soundButton()->setChecked(s.alertSound);
    m_header->soundButton()->blockSignals(false);
    if (s.theme != oldTheme || s.theme != Theme::name()) applyTheme(s.theme);
    m_header->refreshIcons();
    buildMenu();
    updateStatusBar();
}

void MainWindow::importFile()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Импорт целей"), QDir::homePath(),
                                                      QStringLiteral("Списки (*.txt *.csv *.tsv *.xml *.json);;Все файлы (*)"));
    if (path.isEmpty()) return;
    const ImportResult r = zr::importFile(path, currentGroup());
    if (!r.ok()) {
        QMessageBox::warning(this, QStringLiteral("Импорт"), r.error);
        return;
    }
    if (r.targets.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Импорт"), QStringLiteral("В файле не нашлось адресов.\n%1").arg(r.warnings.mid(0, 10).join(QLatin1Char('\n'))));
        return;
    }
    ImportPreviewDialog dlg(m_engine, r, QStringLiteral("Импорт: %1").arg(QFileInfo(path).fileName()), this);
    if (dlg.exec() == QDialog::Accepted)
        m_engine->monitor()->addTargets(dlg.selected());
}

void MainWindow::exportTargets(const QString &format)
{
    const QString ext = format == QLatin1String("json") ? QStringLiteral("json") : format;
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Экспорт"),
                                                      QDir::homePath() + QStringLiteral("/zarnitsa-цели.%1").arg(ext),
                                                      QStringLiteral("*.%1").arg(ext));
    if (path.isEmpty()) return;
    const QVector<TargetConfig> &list = m_engine->monitor()->targets();
    QByteArray data;
    if (format == QLatin1String("csv")) data = QByteArray("\xEF\xBB\xBF") + exportCsv(list).toUtf8();
    else if (format == QLatin1String("txt")) data = exportTextList(list).toUtf8();
    else data = exportJson(list, m_engine->monitor()->groups());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit())
        QMessageBox::warning(this, QStringLiteral("Экспорт"), QStringLiteral("Не удалось сохранить файл"));
    else
        m_sbMsg->setText(QStringLiteral("Сохранено: %1").arg(QDir::toNativeSeparators(path)));
}

void MainWindow::saveSet()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Набор"), QStringLiteral("Название набора"), QLineEdit::Normal,
                                               QDate::currentDate().toString(QStringLiteral("Набор dd.MM.yyyy")), &ok).trimmed();
    if (ok && !name.isEmpty())
        m_engine->saveSet(name, m_engine->monitor()->targets());
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    if (!m_quitting && m_engine->settings().closeToTray && m_tray) {
        hide();
        e->ignore();
        return;
    }
    QJsonObject ui = m_engine->config().ui;
    if (!m_wall) ui[QStringLiteral("geometry")] = QString::fromLatin1(saveGeometry().toBase64());
    m_engine->config().ui = ui;
    m_engine->save();
    m_corner->hide();
    e->accept();
    qApp->quit();
}

void MainWindow::changeEvent(QEvent *e)
{
    QMainWindow::changeEvent(e);
    if (e->type() == QEvent::WindowStateChange && isMinimized() && m_tray && m_engine->settings().minimizeToTray)
        QTimer::singleShot(0, this, &QWidget::hide);
}

void MainWindow::resizeEvent(QResizeEvent *e)
{
    QMainWindow::resizeEvent(e);
    m_toasts->reposition();
}

void MainWindow::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape && m_wall) {
        setWall(false);
        return;
    }
    QMainWindow::keyPressEvent(e);
}

} // namespace zr
