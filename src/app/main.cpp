// Точка входа: окно (QApplication) или служба без окна (--headless, QCoreApplication).
#include "core/EventLog.h"
#include "core/Monitor.h"
#include "integrations/Importers.h"
#include "probes/IcmpEngine.h"
#include "service/Alerter.h"
#include "service/Engine.h"
#include "api/ApiController.h"
#include "ui/DetailDialog.h"
#include "ui/Icons.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/TileView.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMessageBox>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTimer>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <sys/socket.h>
#include <unistd.h>

using namespace zr;

namespace {

QFile *g_log = nullptr;
int g_sigFd[2] = {-1, -1};

void messageHandler(QtMsgType type, const QMessageLogContext &, const QString &msg)
{
    const char *lvl = type == QtDebugMsg ? "debug" : type == QtInfoMsg ? "info" : type == QtWarningMsg ? "warn" : "error";
    const QByteArray line = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")).toUtf8()
        + ' ' + lvl + ' ' + msg.toUtf8();
    std::fprintf(stderr, "%s\n", line.constData());
    if (g_log) {
        g_log->write(line + '\n');
        g_log->flush();
    }
}

void openLog(const QString &dir)
{
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/zarnitsa.log");
    if (QFileInfo(path).size() > 2 * 1024 * 1024) {
        QFile::remove(path + QStringLiteral(".2"));
        QFile::rename(path + QStringLiteral(".1"), path + QStringLiteral(".2"));
        QFile::rename(path, path + QStringLiteral(".1"));
    }
    g_log = new QFile(path);
    if (!g_log->open(QIODevice::WriteOnly | QIODevice::Append)) {
        delete g_log;
        g_log = nullptr;
    }
}

void onSignal(int)
{
    const int savedErrno = errno;
    const char c = 1;
    if (g_sigFd[1] >= 0) (void)::write(g_sigFd[1], &c, 1);
    errno = savedErrno;
}

QString socketName()
{
    return QStringLiteral("zarnitsa-%1").arg(::getuid());
}

// Адреса из командной строки и --list добавляются, если таких ещё нет
QStringList addCliTargets(Engine &engine, const QStringList &hosts, const QString &listFile, const QString &group, QString *err)
{
    QVector<TargetConfig> add;
    ImportResult fromFile;
    if (!listFile.isEmpty()) {
        fromFile = importFile(listFile, group);
        if (!fromFile.ok() && err) *err = fromFile.error;
        add += fromFile.targets;
    }
    for (const QString &h : hosts) {
        ParsedInput p = parseTargetInput(h);
        if (!p.ok) {
            if (err) *err += QStringLiteral("%1: %2\n").arg(h, p.error);
            continue;
        }
        p.target.group = group;
        add.append(p.target);
    }
    QVector<TargetConfig> fresh;
    for (const TargetConfig &t : add) {
        bool exists = false;
        for (const TargetConfig &o : engine.monitor()->targets())
            if (o.address == t.address && o.kind == t.kind && o.port == t.port) exists = true;
        if (!exists) fresh.append(t);
    }
    return engine.monitor()->addTargets(fresh);
}

int renderIcons(const QString &dir)
{
    QDir().mkpath(dir);
    for (int s : {16, 22, 24, 32, 48, 64, 96, 128, 256, 512}) {
        const QPixmap pm = Icons::logo(s);
        const QString sub = dir + QStringLiteral("/%1x%1/apps").arg(s);
        QDir().mkpath(sub);
        if (!pm.save(sub + QStringLiteral("/zarnitsa.png"))) {
            std::fprintf(stderr, "не удалось записать %s\n", qPrintable(sub));
            return 1;
        }
    }
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    bool headless = false;
    for (int i = 1; i < argc; ++i)
        if (qstrcmp(argv[i], "--headless") == 0) headless = true;

    QCoreApplication::setApplicationName(QStringLiteral("zarnitsa"));
    QCoreApplication::setOrganizationName(QStringLiteral("zarnitsa"));
    QCoreApplication::setApplicationVersion(appVersion());
    if (!headless) QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QScopedPointer<QCoreApplication> app(headless ? new QCoreApplication(argc, argv) : new QApplication(argc, argv));
    qInstallMessageHandler(messageHandler);

    QCommandLineParser cli;
    cli.setApplicationDescription(QStringLiteral("Зарница — пинговалка для Astra Linux. Адреса можно перечислить через пробел: "
                                                 "10.0.0.1 host:80 https://сервер/ sip:10.0.0.5"));
    cli.addHelpOption();
    cli.addVersionOption();
    const QCommandLineOption oInterval({QStringLiteral("i"), QStringLiteral("interval")}, QStringLiteral("Интервал замеров, с"), QStringLiteral("сек"));
    const QCommandLineOption oTimeout({QStringLiteral("w"), QStringLiteral("timeout")}, QStringLiteral("Таймаут, мс"), QStringLiteral("мс"));
    const QCommandLineOption oList(QStringLiteral("list"), QStringLiteral("Добавить адреса из файла (TXT, CSV, vmPing.xml, JSON)"), QStringLiteral("файл"));
    const QCommandLineOption oGroup(QStringLiteral("group"), QStringLiteral("Группа для адресов из командной строки"), QStringLiteral("имя"));
    const QCommandLineOption oConfig(QStringLiteral("config"), QStringLiteral("Свой файл конфига (переносной режим)"), QStringLiteral("файл"));
    const QCommandLineOption oHeadless(QStringLiteral("headless"), QStringLiteral("Без окна: мониторинг и API, для серверов и systemd"));
    const QCommandLineOption oApi(QStringLiteral("api"), QStringLiteral("Адрес API, например 127.0.0.1:8790"), QStringLiteral("адрес:порт"));
    const QCommandLineOption oNoApi(QStringLiteral("no-api"), QStringLiteral("Не поднимать API"));
    const QCommandLineOption oPrintToken(QStringLiteral("print-api-token"), QStringLiteral("Явно вывести токен API в stdout (--headless); не использовать в журналируемой службе"));
    const QCommandLineOption oDemo(QStringLiteral("demo"), QStringLiteral("Демо-режим: синтетические данные, сеть не опрашивается"));
    const QCommandLineOption oShot(QStringLiteral("screenshot"), QStringLiteral("Снять окно в PNG и выйти"), QStringLiteral("файл"));
    const QCommandLineOption oShotDelay(QStringLiteral("screenshot-delay"), QStringLiteral("Через сколько мс снимать"), QStringLiteral("мс"), QStringLiteral("2500"));
    const QCommandLineOption oShotDetail(QStringLiteral("screenshot-detail"), QStringLiteral("Ещё снять карточку первой цели"), QStringLiteral("файл"));
    const QCommandLineOption oSize(QStringLiteral("size"), QStringLiteral("Размер окна, ШxВ"), QStringLiteral("ШxВ"));
    const QCommandLineOption oTheme(QStringLiteral("theme"), QStringLiteral("Тема: dark или light"), QStringLiteral("тема"));
    const QCommandLineOption oDensity(QStringLiteral("density"), QStringLiteral("Вид: comfortable, compact, list"), QStringLiteral("вид"));
    const QCommandLineOption oWall(QStringLiteral("wall"), QStringLiteral("Сразу экран дежурного"));
    const QCommandLineOption oMin(QStringLiteral("minimized"), QStringLiteral("Запуститься свёрнутой в трей"));
    const QCommandLineOption oNoSingle(QStringLiteral("no-single"), QStringLiteral("Не передавать адреса уже открытому окну"));
    const QCommandLineOption oIcons(QStringLiteral("render-icons"), QStringLiteral("Нарисовать значки для меню в каталог и выйти"), QStringLiteral("каталог"));
    for (const auto &o : {oInterval, oTimeout, oList, oGroup, oConfig, oHeadless, oApi, oNoApi, oPrintToken, oDemo, oShot, oShotDelay, oShotDetail,
                          oSize, oTheme, oDensity, oWall, oMin, oNoSingle, oIcons})
        cli.addOption(o);
    cli.addPositionalArgument(QStringLiteral("адреса"), QStringLiteral("Цели для наблюдения"), QStringLiteral("[адрес…]"));
    cli.process(*app);

    if (cli.isSet(oIcons)) {
        if (headless) return 2;
        Theme::init();
        return renderIcons(cli.value(oIcons));
    }

    const bool demo = cli.isSet(oDemo);
    const QString group = cli.isSet(oGroup) ? cli.value(oGroup) : QStringLiteral("Из командной строки");
    const QStringList hosts = cli.positionalArguments();

    // --- уже запущенное окно получает адреса и выходит вперёд
    const bool single = !headless && !demo && !cli.isSet(oShot) && !cli.isSet(oNoSingle) && !cli.isSet(oConfig);
    if (single) {
        QLocalSocket probe;
        probe.connectToServer(socketName());
        if (probe.waitForConnected(400)) {
            QJsonObject o;
            o[QStringLiteral("hosts")] = QJsonArray::fromStringList(hosts);
            probe.write(QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n');
            probe.waitForBytesWritten(1000);
            probe.disconnectFromServer();
            return 0;
        }
    }

    Engine engine(cli.value(oConfig), demo);
    if (!demo) openLog(engine.store().dataDir());
    QString err;
    if (!engine.load(&err)) {
        qCritical("%s", qPrintable(err));
        if (!headless)
            QMessageBox::critical(nullptr, QStringLiteral("Зарница"),
                                  QStringLiteral("%1\n\nФайл: %2").arg(err, engine.store().path()));
        return 1;
    }

    // Разовые интервалы применяем после настройки API: applySettings возвращает настройки конфига.
    const auto applyProbeOverrides = [&]() {
        Settings run = engine.monitor()->settings();
        if (cli.isSet(oInterval)) run.intervalMs = qMax(100, int(cli.value(oInterval).toDouble() * 1000.0));
        if (cli.isSet(oTimeout)) run.timeoutMs = qMax(50, cli.value(oTimeout).toInt());
        engine.monitor()->setSettings(run);
    };

    QString addErr;
    addCliTargets(engine, hosts, cli.value(oList), group, &addErr);
    if (!addErr.isEmpty()) qWarning("%s", qPrintable(addErr.trimmed()));

    if (headless) {
        if (!cli.isSet(oNoApi)) {
            Settings s = engine.settings();
            const bool fresh = s.apiToken.size() < 16;
            s.apiEnabled = true;
            if (cli.isSet(oApi)) s.apiListen = cli.value(oApi);
            if (fresh) s.apiToken = ApiController::generateToken();
            engine.applySettings(s);
            if (!engine.api()->isRunning()) engine.startApi(nullptr);
            if (engine.api()->isRunning()) {
                std::printf("API: %s\n", qPrintable(engine.api()->address()));
                if (cli.isSet(oPrintToken)) std::printf("Токен API: %s\n", qPrintable(s.apiToken));
                else if (fresh) std::printf("Создан токен API; он хранится в %s (права 0600)\n", qPrintable(engine.store().path()));
            } else {
                std::fprintf(stderr, "API не поднялся: %s\n", qPrintable(engine.apiError()));
            }
        }
        std::printf("Зарница %s без окна: целей %d, ICMP — %s\n", qPrintable(appVersion()),
                    engine.monitor()->targets().size(), qPrintable(engine.icmp()->modeTitle()));
        std::fflush(stdout);
        QObject::connect(engine.icmp(), &IcmpEngine::modeChanged, [&engine]() {
            std::printf("ICMP: %s\n", qPrintable(engine.icmp()->modeTitle()));
            std::fflush(stdout);
        });
        QObject::connect(engine.log(), &EventLog::appended, [](const StatusEvent &e) {
            std::printf("%s  %s  %s  %s\n", qPrintable(QDateTime::fromMSecsSinceEpoch(e.t).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))),
                        qPrintable(e.name), qPrintable(e.address), qPrintable(e.text()));
            std::fflush(stdout);
        });
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, g_sigFd) == 0) {
            auto *sn = new QSocketNotifier(g_sigFd[0], QSocketNotifier::Read, app.data());
            QObject::connect(sn, &QSocketNotifier::activated, [&engine, &app]() {
                char c;
                (void)::read(g_sigFd[0], &c, 1);
                engine.save();
                app->quit();
            });
            std::signal(SIGTERM, onSignal);
            std::signal(SIGINT, onSignal);
            std::signal(SIGHUP, onSignal);
        }
        applyProbeOverrides();
        engine.start();
        return app->exec();
    }

    // --- окно
    auto *gui = static_cast<QApplication *>(app.data());
    gui->setQuitOnLastWindowClosed(false);
    gui->setWindowIcon(Icons::appIcon());
    Theme::init();
    const QString theme = cli.isSet(oTheme) ? cli.value(oTheme) : engine.settings().theme;
    Theme::apply(gui, theme);
    if (cli.isSet(oDensity)) {
        Settings s = engine.monitor()->settings();
        s.density = cli.value(oDensity);
        engine.monitor()->setSettings(s);
        engine.config().settings.density = s.density;
    }

    MainWindow w(&engine);
    w.restoreState();
    if (cli.isSet(oDensity)) w.tiles()->setDensity(TileView::densityFromKey(cli.value(oDensity)));

    QLocalServer server;
    if (single) {
        QLocalServer::removeServer(socketName());
        server.setSocketOptions(QLocalServer::UserAccessOption);
        if (server.listen(socketName())) {
            QObject::connect(&server, &QLocalServer::newConnection, [&server, &w]() {
                QLocalSocket *c = server.nextPendingConnection();
                QObject::connect(c, &QLocalSocket::readyRead, c, [c, &w]() {
                    if (!c->canReadLine()) return;
                    const QJsonObject o = QJsonDocument::fromJson(c->readLine()).object();
                    QStringList hs;
                    for (const QJsonValue &v : o.value(QStringLiteral("hosts")).toArray()) hs << v.toString();
                    w.handleRemote(hs);
                    c->disconnectFromServer();
                });
                QObject::connect(c, &QLocalSocket::disconnected, c, &QObject::deleteLater);
            });
        }
    }

    if (!demo && !cli.isSet(oNoApi) && (engine.settings().apiEnabled || cli.isSet(oApi))) {
        if (cli.isSet(oApi)) {
            Settings s = engine.settings();
            s.apiEnabled = true;
            s.apiListen = cli.value(oApi);
            engine.applySettings(s);
        }
        QString apiErr;
        if (!engine.api()->isRunning() && !engine.startApi(&apiErr)) qWarning("API: %s", qPrintable(apiErr));
    }
    applyProbeOverrides();
    engine.start();

    if (cli.isSet(oShot)) {
        QSize size(1440, 900);
        const QStringList wh = cli.value(oSize).split(QLatin1Char('x'));
        if (wh.size() == 2) size = QSize(wh[0].toInt(), wh[1].toInt());
        w.resize(size);
        w.show();
        if (cli.isSet(oWall)) w.setWall(true);
        const int delay = cli.value(oShotDelay).toInt();
        const QString shot = cli.value(oShot);
        const QString detailShot = cli.value(oShotDetail);
        QTimer::singleShot(delay, gui, [&w, &engine, shot, detailShot, gui, size]() {
            if (w.isWall()) w.resize(size);
            const QPixmap pm = w.grab();
            const bool saved = pm.save(shot);
            if (!saved) std::fprintf(stderr, "не удалось сохранить %s\n", qPrintable(shot));
            if (!detailShot.isEmpty() && !engine.monitor()->targets().isEmpty()) {
                // карточка самой «интересной» цели: первой с проблемой, иначе первой
                QString id = engine.monitor()->targets().first().id;
                for (const TargetConfig &t : engine.monitor()->targets()) {
                    const TargetRuntime *rt = engine.monitor()->runtime(t.id);
                    if (rt && rt->status == Status::Degraded) { id = t.id; break; }
                }
                auto *d = new DetailDialog(&engine, id, &w);
                d->resize(860, 760);
                d->show();
                QTimer::singleShot(600, gui, [d, detailShot, gui, saved]() {
                    const bool detailSaved = d->grab().save(detailShot);
                    if (!detailSaved) std::fprintf(stderr, "не удалось сохранить %s\n", qPrintable(detailShot));
                    gui->exit(saved && detailSaved ? 0 : 1);
                });
                return;
            }
            gui->exit(saved ? 0 : 1);
        });
        return gui->exec();
    }

    const bool startHidden = (cli.isSet(oMin) || engine.settings().startMinimized) && QSystemTrayIcon::isSystemTrayAvailable();
    if (!startHidden) w.show();
    if (cli.isSet(oWall)) w.setWall(true);
    return gui->exec();
}
