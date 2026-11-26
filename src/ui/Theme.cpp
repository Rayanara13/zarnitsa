#include "Theme.h"

#include <QApplication>
#include <QFontDatabase>
#include <QLocale>
#include <QPalette>
#include <QStyleFactory>

static void initZarnitsaResources() { Q_INIT_RESOURCE(zarnitsa); }

namespace zr {

namespace {

Palette g_p;
QString g_name = QStringLiteral("dark");
QString g_ui = QStringLiteral("Golos Text");
QString g_mono = QStringLiteral("JetBrains Mono NL");

QColor rgba(int r, int g, int b, double a) { QColor c(r, g, b); c.setAlphaF(a); return c; }

Palette darkPalette()
{
    Palette p;
    p.dark = true;
    p.canvas = QColor(0x0f, 0x11, 0x17);
    p.surface = QColor(0x16, 0x19, 0x21);
    p.tile = QColor(0x1b, 0x1f, 0x29);
    p.raised = QColor(0x23, 0x28, 0x35);
    p.line = QColor(0x29, 0x2f, 0x3d);
    p.lineStrong = QColor(0x3a, 0x42, 0x55);
    p.ink = QColor(0xe9, 0xec, 0xf3);
    p.muted = QColor(0xa3, 0xac, 0xbd);
    p.faint = QColor(0x70, 0x7a, 0x8e);
    p.accent = QColor(0xa5, 0x94, 0xff);
    p.accentStrong = QColor(0xc4, 0xb8, 0xff);
    p.accentWash = rgba(0xa5, 0x94, 0xff, 0.16);
    p.onAccent = QColor(0x16, 0x11, 0x2b);
    p.ok = QColor(0x22, 0xc5, 0x5e);
    p.okText = QColor(0x86, 0xef, 0xac);
    p.okWash = rgba(0x22, 0xc5, 0x5e, 0.13);
    p.warn = QColor(0xf5, 0x9e, 0x0b);
    p.warnText = QColor(0xfc, 0xd3, 0x4d);
    p.warnWash = rgba(0xf5, 0x9e, 0x0b, 0.14);
    p.bad = QColor(0xef, 0x44, 0x44);
    p.badText = QColor(0xfc, 0xa5, 0xa5);
    p.badWash = rgba(0xef, 0x44, 0x44, 0.16);
    p.unknown = QColor(0x64, 0x74, 0x8b);
    p.unknownText = QColor(0xa3, 0xb1, 0xc6);
    p.unknownWash = rgba(0x64, 0x74, 0x8b, 0.16);
    p.paused = QColor(0x4b, 0x55, 0x69);
    p.shadow = rgba(0, 0, 0, 0.45);
    return p;
}

Palette lightPalette()
{
    Palette p;
    p.dark = false;
    p.canvas = QColor(0xf4, 0xf3, 0xef);
    p.surface = QColor(0xfd, 0xfc, 0xf9);
    p.tile = QColor(0xff, 0xff, 0xff);
    p.raised = QColor(0xf0, 0xef, 0xea);
    p.line = QColor(0xe1, 0xdf, 0xd7);
    p.lineStrong = QColor(0xc9, 0xc6, 0xbb);
    p.ink = QColor(0x1c, 0x20, 0x30);
    p.muted = QColor(0x59, 0x60, 0x71);
    p.faint = QColor(0x8a, 0x90, 0x9f);
    p.accent = QColor(0x5b, 0x46, 0xe0);
    p.accentStrong = QColor(0x46, 0x32, 0xc4);
    p.accentWash = QColor(0xec, 0xea, 0xfd);
    p.onAccent = QColor(0xff, 0xff, 0xff);
    p.ok = QColor(0x22, 0xc5, 0x5e);
    p.okText = QColor(0x15, 0x80, 0x3d);
    p.okWash = QColor(0xe7, 0xf6, 0xec);
    p.warn = QColor(0xf5, 0x9e, 0x0b);
    p.warnText = QColor(0xa1, 0x5c, 0x07);
    p.warnWash = QColor(0xfd, 0xf2, 0xdc);
    p.bad = QColor(0xef, 0x44, 0x44);
    p.badText = QColor(0xb9, 0x1c, 0x1c);
    p.badWash = QColor(0xfd, 0xeb, 0xeb);
    p.unknown = QColor(0x64, 0x74, 0x8b);
    p.unknownText = QColor(0x47, 0x55, 0x69);
    p.unknownWash = QColor(0xee, 0xf1, 0xf5);
    p.paused = QColor(0xa2, 0xa8, 0xb4);
    p.shadow = rgba(0x1c, 0x20, 0x30, 0.10);
    return p;
}

QString hex(const QColor &c)
{
    if (c.alpha() == 255)
        return c.name(QColor::HexRgb);
    return QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alphaF(), 0, 'f', 3);
}

} // namespace

void Theme::init()
{
    initZarnitsaResources();
    const char *files[] = {":/fonts/GolosText-Regular.ttf", ":/fonts/GolosText-Medium.ttf",
                           ":/fonts/GolosText-SemiBold.ttf", ":/fonts/GolosText-Bold.ttf",
                           ":/fonts/JetBrainsMonoNL-Regular.ttf", ":/fonts/JetBrainsMonoNL-Medium.ttf",
                           ":/fonts/JetBrainsMonoNL-Bold.ttf"};
    for (const char *f : files) {
        const int id = QFontDatabase::addApplicationFont(QString::fromLatin1(f));
        if (id < 0) continue;
        const QStringList fam = QFontDatabase::applicationFontFamilies(id);
        if (fam.isEmpty()) continue;
        if (QByteArray(f).contains("Golos")) g_ui = fam.first();
        else g_mono = fam.first();
    }
    g_p = darkPalette();
}

const Palette &Theme::p() { return g_p; }
bool Theme::isDark() { return g_p.dark; }
QString Theme::name() { return g_name; }
QString Theme::uiFamily() { return g_ui; }
QString Theme::monoFamily() { return g_mono; }

QFont Theme::ui(qreal px, int weight)
{
    QFont f(g_ui);
    f.setPixelSize(qMax(1, qRound(px)));
    f.setWeight(weight);
    f.setStyleStrategy(QFont::PreferAntialias);
    return f;
}

QFont Theme::mono(qreal px, int weight)
{
    QFont f(g_mono);
    f.setPixelSize(qMax(1, qRound(px)));
    f.setWeight(weight);
    f.setStyleHint(QFont::Monospace);
    f.setStyleStrategy(QFont::PreferAntialias);
    return f;
}

QColor Theme::statusColor(Status s)
{
    switch (s) {
    case Status::Up: return g_p.ok;
    case Status::Degraded: return g_p.warn;
    case Status::Down: return g_p.bad;
    case Status::Unknown: return g_p.unknown;
    case Status::Paused: return g_p.paused;
    }
    return g_p.unknown;
}

QColor Theme::statusText(Status s)
{
    switch (s) {
    case Status::Up: return g_p.okText;
    case Status::Degraded: return g_p.warnText;
    case Status::Down: return g_p.badText;
    case Status::Unknown: return g_p.unknownText;
    case Status::Paused: return g_p.faint;
    }
    return g_p.unknownText;
}

QColor Theme::statusWash(Status s)
{
    switch (s) {
    case Status::Up: return g_p.okWash;
    case Status::Degraded: return g_p.warnWash;
    case Status::Down: return g_p.badWash;
    case Status::Unknown: return g_p.unknownWash;
    case Status::Paused: return g_p.unknownWash;
    }
    return g_p.unknownWash;
}

void Theme::apply(QApplication *app, const QString &name)
{
    g_name = name == QLatin1String("light") ? QStringLiteral("light") : QStringLiteral("dark");
    g_p = g_name == QLatin1String("light") ? lightPalette() : darkPalette();
    if (!app)
        return;
    app->setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QPalette pal;
    pal.setColor(QPalette::Window, g_p.canvas);
    pal.setColor(QPalette::WindowText, g_p.ink);
    pal.setColor(QPalette::Base, g_p.surface);
    pal.setColor(QPalette::AlternateBase, g_p.raised);
    pal.setColor(QPalette::Text, g_p.ink);
    pal.setColor(QPalette::Button, g_p.surface);
    pal.setColor(QPalette::ButtonText, g_p.ink);
    pal.setColor(QPalette::Highlight, g_p.accent);
    pal.setColor(QPalette::HighlightedText, g_p.onAccent);
    pal.setColor(QPalette::ToolTipBase, g_p.raised);
    pal.setColor(QPalette::ToolTipText, g_p.ink);
    pal.setColor(QPalette::PlaceholderText, g_p.faint);
    pal.setColor(QPalette::Link, g_p.accent);
    pal.setColor(QPalette::Light, g_p.raised);
    pal.setColor(QPalette::Midlight, g_p.line);
    pal.setColor(QPalette::Mid, g_p.lineStrong);
    pal.setColor(QPalette::Dark, g_p.lineStrong);
    pal.setColor(QPalette::Shadow, g_p.shadow);
    pal.setColor(QPalette::Disabled, QPalette::Text, g_p.faint);
    pal.setColor(QPalette::Disabled, QPalette::WindowText, g_p.faint);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, g_p.faint);
    app->setPalette(pal);
    app->setFont(ui(13));
    app->setStyleSheet(styleSheet());
}

QString Theme::styleSheet()
{
    const Palette &p = g_p;
    QString s = QStringLiteral(R"(
QWidget { color: @ink; }
QMainWindow, QDialog { background: @canvas; }
QToolTip { background: @raised; color: @ink; border: 1px solid @lineStrong; padding: 6px 8px; border-radius: 6px; }
QLabel[role="muted"] { color: @muted; }
QLabel[role="faint"] { color: @faint; }
QLabel[role="title"] { font-size: 18px; font-weight: 600; }
QLabel[role="section"] { font-size: 12px; font-weight: 600; color: @muted; text-transform: uppercase; }
QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox, QPlainTextEdit, QTextEdit, QDateTimeEdit {
  background: @surface; border: 1px solid @line; border-radius: 7px; padding: 5px 9px;
  selection-background-color: @accent; selection-color: @onAccent; }
QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus, QPlainTextEdit:focus, QTextEdit:focus { border: 1px solid @accent; }
QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled { color: @faint; background: @raised; }
QComboBox::drop-down { border: none; width: 22px; }
QComboBox QAbstractItemView { background: @surface; border: 1px solid @line; selection-background-color: @accentWash; selection-color: @ink; outline: none; padding: 4px; }
QSpinBox::up-button, QSpinBox::down-button, QDoubleSpinBox::up-button, QDoubleSpinBox::down-button { width: 16px; border: none; background: transparent; }
QPushButton { background: @surface; border: 1px solid @lineStrong; border-radius: 7px; padding: 6px 14px; min-height: 18px; }
QPushButton:hover { background: @raised; }
QPushButton:pressed { background: @line; }
QPushButton:disabled { color: @faint; border-color: @line; }
QPushButton[accent="true"] { background: @accent; color: @onAccent; border: 1px solid @accent; font-weight: 600; }
QPushButton[accent="true"]:hover { background: @accentStrong; border-color: @accentStrong; }
QPushButton[danger="true"] { color: @badText; border-color: @bad; }
QPushButton[flat="true"] { border: none; background: transparent; }
QToolButton { background: transparent; border: 1px solid transparent; border-radius: 7px; padding: 5px; }
QToolButton:hover { background: @raised; border-color: @line; }
QToolButton:checked { background: @accentWash; border-color: @accent; }
QToolButton::menu-indicator { image: none; width: 0; }
QMenu { background: @surface; border: 1px solid @lineStrong; border-radius: 8px; padding: 6px; }
QMenu::item { padding: 6px 26px 6px 12px; border-radius: 5px; }
QMenu::item:selected { background: @accentWash; color: @ink; }
QMenu::item:disabled { color: @faint; }
QMenu::separator { height: 1px; background: @line; margin: 5px 8px; }
QMenu::icon { padding-left: 8px; }
QCheckBox, QRadioButton { spacing: 8px; }
QGroupBox { border: 1px solid @line; border-radius: 9px; margin-top: 14px; padding: 12px 10px 10px 10px; }
QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 4px; color: @muted; font-weight: 600; }
QTabWidget::pane { border: 1px solid @line; border-radius: 9px; top: -1px; background: @surface; }
QTabBar::tab { background: transparent; padding: 7px 14px; margin-right: 2px; color: @muted; border-bottom: 2px solid transparent; }
QTabBar::tab:selected { color: @ink; border-bottom: 2px solid @accent; }
QTabBar::tab:hover { color: @ink; }
QHeaderView::section { background: @surface; color: @muted; border: none; border-bottom: 1px solid @line; padding: 6px 8px; font-weight: 600; }
QTableView, QTreeView, QListView { background: @surface; alternate-background-color: @raised; border: 1px solid @line; border-radius: 8px;
  gridline-color: @line; selection-background-color: @accentWash; selection-color: @ink; outline: none; }
QTableView::item, QTreeView::item, QListView::item { padding: 3px 6px; }
QScrollBar:vertical { background: transparent; width: 11px; margin: 2px; }
QScrollBar::handle:vertical { background: @lineStrong; border-radius: 4px; min-height: 36px; }
QScrollBar::handle:vertical:hover { background: @faint; }
QScrollBar:horizontal { background: transparent; height: 11px; margin: 2px; }
QScrollBar::handle:horizontal { background: @lineStrong; border-radius: 4px; min-width: 36px; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QStatusBar { background: @surface; border-top: 1px solid @line; color: @muted; }
QStatusBar QLabel { color: @muted; padding: 0 6px; }
QProgressBar { background: @raised; border: none; border-radius: 4px; height: 8px; text-align: center; color: transparent; }
QProgressBar::chunk { background: @accent; border-radius: 4px; }
QSplitter::handle { background: @line; }
)");
    s.replace(QStringLiteral("@ui"), g_ui);
    s.replace(QStringLiteral("@canvas"), hex(p.canvas));
    s.replace(QStringLiteral("@surface"), hex(p.surface));
    s.replace(QStringLiteral("@raised"), hex(p.raised));
    s.replace(QStringLiteral("@lineStrong"), hex(p.lineStrong));
    s.replace(QStringLiteral("@line"), hex(p.line));
    s.replace(QStringLiteral("@ink"), hex(p.ink));
    s.replace(QStringLiteral("@muted"), hex(p.muted));
    s.replace(QStringLiteral("@faint"), hex(p.faint));
    s.replace(QStringLiteral("@accentStrong"), hex(p.accentStrong));
    s.replace(QStringLiteral("@accentWash"), hex(p.accentWash));
    s.replace(QStringLiteral("@accent"), hex(p.accent));
    s.replace(QStringLiteral("@onAccent"), hex(p.onAccent));
    s.replace(QStringLiteral("@badText"), hex(p.badText));
    s.replace(QStringLiteral("@bad"), hex(p.bad));
    return s;
}

QString fmtNum(double v, int prec)
{
    static const QLocale ru(QLocale::Russian, QLocale::Russia);
    return ru.toString(v, 'f', prec);
}

QString fmtMs(double ms)
{
    if (ms < 0) return QStringLiteral("—");
    if (ms < 1.0) return fmtNum(ms, 2);
    if (ms < 100.0) return fmtNum(ms, 1);
    return fmtNum(ms, 0);
}

QString fmtPct(double pct)
{
    if (pct <= 0.0) return QStringLiteral("0");
    if (pct < 10.0) return fmtNum(pct, 1);
    return fmtNum(pct, 0);
}

QString fmtDuration(qint64 ms)
{
    const qint64 s = qMax<qint64>(0, ms / 1000);
    if (s < 60) return QStringLiteral("%1 с").arg(s);
    if (s < 3600) return QStringLiteral("%1 мин").arg(s / 60);
    if (s < 86400) {
        const qint64 m = (s % 3600) / 60;
        return m ? QStringLiteral("%1 ч %2 мин").arg(s / 3600).arg(m) : QStringLiteral("%1 ч").arg(s / 3600);
    }
    const qint64 h = (s % 86400) / 3600;
    return h ? QStringLiteral("%1 д %2 ч").arg(s / 86400).arg(h) : QStringLiteral("%1 д").arg(s / 86400);
}

} // namespace zr
