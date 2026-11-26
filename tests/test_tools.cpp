// Widgets-проверки инструментов: исполнители синтетические, Echo и TCP-соединений нет.
#include "probes/ProbeDispatcher.h"
#include "probes/Resolver.h"
#include "service/Engine.h"
#include "ui/Theme.h"
#include "ui/ToolsDialogs.h"

#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QHash>
#include <QLineEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using namespace zr;

namespace {

class FakeProbeBackend : public ProbeBackend {
public:
    struct Started {
        ProbeRequest request;
        qint64 atUs = 0;
    };

    explicit FakeProbeBackend(QObject *parent) : ProbeBackend(parent) { clock.start(); }

    void start(const ProbeRequest &request) override
    {
        starts.append({request, clock.nsecsElapsed() / 1000});
        active.insert(request.token, request);
        if (!hold)
            answer(request, outcome);
    }

    void cancel(quint64 token) override
    {
        active.remove(token);
        cancelled.append(token);
    }

    void answer(const ProbeRequest &request, ProbeOutcome result)
    {
        // Разрешаем и запоздалый ответ уже отменённого запроса: диспетчер обязан его отбросить.
        active.remove(request.token);
        ProbeResult reply = makeResult(request, result);
        reply.rttMs = 1.0;
        reply.ttl = 64;
        finishLater(reply);
    }

    QElapsedTimer clock;
    QVector<Started> starts;
    QHash<quint64, ProbeRequest> active;
    QVector<quint64> cancelled;
    ProbeOutcome outcome = ProbeOutcome::Timeout;
    bool hold = false;
};

QPushButton *runButton(QWidget &dialog)
{
    for (QPushButton *button : dialog.findChildren<QPushButton *>())
        if (button->property("accent").toBool())
            return button;
    return nullptr;
}

} // namespace

class TestTools : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void tracePacesThreeRounds();
    void traceFinalHopSkipsUnsent();
    void traceContinuousKeepsRunning();
    void traceStopRestartCancelsRoundAndUnsent();
    void traceOldDnsCannotReplaceNewRun();
    void scanPacesEightAddresses();
    void scanStopRestartDiscardsOldResults();
    void scanKeepsPortsUntilClose();
    void scanRestartCancelsOldPorts();
};

void TestTools::initTestCase()
{
    Theme::init();
    Theme::apply(qApp, QStringLiteral("dark"));
    qApp->setQuitOnLastWindowClosed(false);
}

void TestTools::tracePacesThreeRounds()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Engine engine(dir.filePath(QStringLiteral("tools.json")), false);
    auto *icmp = new FakeProbeBackend(&engine);
    engine.dispatcher()->setBackend(ProbeKind::Icmp, icmp);
    auto dialog = std::make_unique<TracerouteDialog>(&engine, QStringLiteral("198.51.100.10"));
    auto *hops = dialog->findChild<QSpinBox *>();
    auto *table = dialog->findChild<QTableWidget *>();
    auto *go = runButton(*dialog);
    QVERIFY(hops && table && go);
    hops->setValue(3);
    dialog->start();
    QCOMPARE(icmp->starts.size(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(go->text(), QStringLiteral("Старт"), 6000);
    QCOMPARE(icmp->starts.size(), 9);
    QCOMPARE(engine.dispatcher()->inFlight(), 0);
    for (int i = 0; i < icmp->starts.size(); ++i) {
        QCOMPARE(icmp->starts.at(i).request.ttl, i % 3 + 1);
        if (i > 0)
            QVERIFY2(icmp->starts.at(i).atUs - icmp->starts.at(i - 1).atUs >= 140000,
                     "TTL ушли пачкой вместо интервала 150 мс");
    }
    QCOMPARE(table->rowCount(), 3);
    for (int row = 0; row < 3; ++row) {
        QVERIFY(table->item(row, 4));
        QCOMPARE(table->item(row, 4)->text(), QStringLiteral("3"));
    }
    QTest::qWait(250);
    QCOMPARE(icmp->starts.size(), 9);
}

void TestTools::traceFinalHopSkipsUnsent()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Engine engine(dir.filePath(QStringLiteral("tools.json")), false);
    auto *icmp = new FakeProbeBackend(&engine);
    icmp->outcome = ProbeOutcome::Ok;
    engine.dispatcher()->setBackend(ProbeKind::Icmp, icmp);
    auto dialog = std::make_unique<TracerouteDialog>(&engine, QStringLiteral("198.51.100.11"));
    auto *hops = dialog->findChild<QSpinBox *>();
    auto *table = dialog->findChild<QTableWidget *>();
    auto *go = runButton(*dialog);
    QVERIFY(hops && table && go);
    hops->setValue(6);
    dialog->start();
    QTRY_COMPARE_WITH_TIMEOUT(go->text(), QStringLiteral("Старт"), 4000);
    QCOMPARE(icmp->starts.size(), 3);
    for (const auto &started : icmp->starts)
        QCOMPARE(started.request.ttl, 1);
    QCOMPARE(table->rowCount(), 1);
    QVERIFY(table->item(0, 4));
    QCOMPARE(table->item(0, 4)->text(), QStringLiteral("3"));
    QCOMPARE(engine.dispatcher()->inFlight(), 0);
}

void TestTools::traceContinuousKeepsRunning()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Engine engine(dir.filePath(QStringLiteral("tools.json")), false);
    auto *icmp = new FakeProbeBackend(&engine);
    engine.dispatcher()->setBackend(ProbeKind::Icmp, icmp);
    auto dialog = std::make_unique<TracerouteDialog>(&engine, QStringLiteral("198.51.100.12"));
    auto *hops = dialog->findChild<QSpinBox *>();
    auto *continuous = dialog->findChild<QCheckBox *>();
    auto *table = dialog->findChild<QTableWidget *>();
    auto *go = runButton(*dialog);
    QVERIFY(hops && continuous && table && go);
    hops->setValue(1);
    continuous->setChecked(true);
    dialog->start();
    QTRY_VERIFY_WITH_TIMEOUT(table->item(0, 4) && table->item(0, 4)->text() == QStringLiteral("4"), 7000);
    QCOMPARE(icmp->starts.size(), 4);
    QCOMPARE(go->text(), QStringLiteral("Стоп"));
    go->click();
    QCOMPARE(go->text(), QStringLiteral("Старт"));
    QCOMPARE(engine.dispatcher()->inFlight(), 0);
}

void TestTools::traceStopRestartCancelsRoundAndUnsent()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Engine engine(dir.filePath(QStringLiteral("tools.json")), false);
    auto *icmp = new FakeProbeBackend(&engine);
    engine.dispatcher()->setBackend(ProbeKind::Icmp, icmp);
    auto dialog = std::make_unique<TracerouteDialog>(&engine, QStringLiteral("198.51.100.20"));
    auto *host = dialog->findChild<QLineEdit *>();
    auto *hops = dialog->findChild<QSpinBox *>();
    auto *continuous = dialog->findChild<QCheckBox *>();
    auto *table = dialog->findChild<QTableWidget *>();
    auto *go = runButton(*dialog);
    QVERIFY(host && hops && continuous && table && go);
    hops->setValue(1);
    continuous->setChecked(true);
    dialog->start();
    QTRY_VERIFY_WITH_TIMEOUT(table->item(0, 4) && table->item(0, 4)->text() == QStringLiteral("1"), 2000);
    QCOMPARE(icmp->starts.size(), 1);
    go->click();   // у старого запуска уже отложен следующий continuous-round
    QCOMPARE(engine.dispatcher()->inFlight(), 0);

    icmp->hold = true;
    continuous->setChecked(false);
    hops->setValue(4);
    host->setText(QStringLiteral("198.51.100.21"));
    dialog->start();
    QTest::qWait(1300);   // пересекаем срок старого round, ответы нового пока удерживаются
    QCOMPARE(icmp->starts.size(), 5);
    for (int i = 1; i < 5; ++i) {
        QCOMPARE(icmp->starts.at(i).request.host, QStringLiteral("198.51.100.21"));
        QCOMPARE(icmp->starts.at(i).request.ttl, i);
    }
    QCOMPARE(engine.dispatcher()->inFlight(), 4);
    go->click();
    QCOMPARE(engine.dispatcher()->inFlight(), 0);
    QVERIFY(icmp->active.isEmpty());

    // Ещё один старт останавливаем до второго TTL; оставшиеся не должны отправиться.
    const int before = icmp->starts.size();
    dialog->start();
    QTRY_COMPARE_WITH_TIMEOUT(icmp->starts.size(), before + 1, 1500);
    const ProbeRequest stale = icmp->starts.last().request;
    go->click();
    QCOMPARE(engine.dispatcher()->inFlight(), 0);
    QVERIFY(icmp->cancelled.contains(stale.token));
    icmp->answer(stale, ProbeOutcome::Ok);
    QTest::qWait(500);
    QCOMPARE(icmp->starts.size(), before + 1);
    QCOMPARE(engine.dispatcher()->inFlight(), 0);
}

void TestTools::traceOldDnsCannotReplaceNewRun()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Engine engine(dir.filePath(QStringLiteral("tools.json")), false);
    auto *icmp = new FakeProbeBackend(&engine);
    icmp->hold = true;
    engine.dispatcher()->setBackend(ProbeKind::Icmp, icmp);
    auto dialog = std::make_unique<TracerouteDialog>(&engine, QStringLiteral("localhost"));
    auto *host = dialog->findChild<QLineEdit *>();
    auto *hops = dialog->findChild<QSpinBox *>();
    QVERIFY(host && hops);
    hops->setValue(2);
    dialog->start();   // локальный DNS-ответ доставляется через очередь
    bool oldDnsCompleted = false;
    engine.dispatcher()->resolver()->resolve(QStringLiteral("localhost"),
        [&oldDnsCompleted](const QHostAddress &, const QString &) { oldDnsCompleted = true; });
    host->setText(QStringLiteral("198.51.100.30"));
    dialog->start();
    QTRY_VERIFY_WITH_TIMEOUT(oldDnsCompleted, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(icmp->starts.size(), 2, 2500);
    QTest::qWait(200);
    QCOMPARE(icmp->starts.size(), 2);
    for (int i = 0; i < 2; ++i) {
        QCOMPARE(icmp->starts.at(i).request.host, QStringLiteral("198.51.100.30"));
        QCOMPARE(icmp->starts.at(i).request.ttl, i + 1);
    }
    dialog.reset();
    QCOMPARE(engine.dispatcher()->inFlight(), 0);
    QVERIFY(icmp->active.isEmpty());
}

void TestTools::scanPacesEightAddresses()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Engine engine(dir.filePath(QStringLiteral("tools.json")), false);
    auto *icmp = new FakeProbeBackend(&engine);
    auto *tcp = new FakeProbeBackend(&engine);
    engine.dispatcher()->setBackend(ProbeKind::Icmp, icmp);
    engine.dispatcher()->setBackend(ProbeKind::Tcp, tcp);
    auto dialog = std::make_unique<ScanDialog>(&engine, QStringLiteral("198.51.100.1-8"));
    const auto edits = dialog->findChildren<QLineEdit *>();
    auto *go = runButton(*dialog);
    auto *progress = dialog->findChild<QProgressBar *>();
    QVERIFY(edits.size() >= 2 && go && progress);
    edits.at(1)->clear();
    go->click();
    QCOMPARE(icmp->starts.size(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(progress->value(), 8, 3000);
    QCOMPARE(icmp->starts.size(), 8);
    for (int i = 0; i < 8; ++i) {
        QCOMPARE(icmp->starts.at(i).request.host, QStringLiteral("198.51.100.%1").arg(i + 1));
        if (i > 0)
            QVERIFY2(icmp->starts.at(i).atUs - icmp->starts.at(i - 1).atUs >= 8000,
                     "Адреса ушли пачкой вместо интервала 10 мс");
    }
    QCOMPARE(go->text(), QStringLiteral("Сканировать"));
    QCOMPARE(engine.dispatcher()->inFlight(), 0);
    QVERIFY(tcp->starts.isEmpty());
    QTest::qWait(100);
    QCOMPARE(icmp->starts.size(), 8);
}

void TestTools::scanStopRestartDiscardsOldResults()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Engine engine(dir.filePath(QStringLiteral("tools.json")), false);
    auto *icmp = new FakeProbeBackend(&engine);
    icmp->hold = true;
    auto *tcp = new FakeProbeBackend(&engine);
    engine.dispatcher()->setBackend(ProbeKind::Icmp, icmp);
    engine.dispatcher()->setBackend(ProbeKind::Tcp, tcp);
    auto dialog = std::make_unique<ScanDialog>(&engine, QStringLiteral("198.51.100.1-8"));
    const auto edits = dialog->findChildren<QLineEdit *>();
    auto *go = runButton(*dialog);
    auto *progress = dialog->findChild<QProgressBar *>();
    auto *table = dialog->findChild<QTableWidget *>();
    QVERIFY(edits.size() >= 2 && go && progress && table);
    edits.at(1)->clear();
    go->click();
    QTRY_VERIFY_WITH_TIMEOUT(!icmp->starts.isEmpty(), 1000);
    const int oldCount = icmp->starts.size();
    QVERIFY(oldCount < 8);
    QVector<ProbeRequest> stale;
    for (const auto &started : icmp->starts)
        stale.append(started.request);
    go->click();
    QCOMPARE(engine.dispatcher()->inFlight(), 0);
    QVERIFY(icmp->active.isEmpty());
    for (const ProbeRequest &request : stale)
        QVERIFY(icmp->cancelled.contains(request.token));

    edits.at(0)->setText(QStringLiteral("203.0.113.1-6"));
    go->click();
    for (const ProbeRequest &request : stale)
        icmp->answer(request, ProbeOutcome::Ok);
    QTRY_COMPARE_WITH_TIMEOUT(icmp->starts.size(), oldCount + 6, 2500);
    for (int i = 0; i < 6; ++i)
        QCOMPARE(icmp->starts.at(oldCount + i).request.host, QStringLiteral("203.0.113.%1").arg(i + 1));
    QCOMPARE(progress->value(), 0);
    QCOMPARE(table->rowCount(), 0);
    QCOMPARE(engine.dispatcher()->inFlight(), 6);
    go->click();
    QCOMPARE(engine.dispatcher()->inFlight(), 0);
    QVERIFY(icmp->active.isEmpty());
    QTest::qWait(100);
    QCOMPARE(icmp->starts.size(), oldCount + 6);
    QVERIFY(tcp->starts.isEmpty());
}

void TestTools::scanKeepsPortsUntilClose()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Engine engine(dir.filePath(QStringLiteral("tools.json")), false);
    auto *icmp = new FakeProbeBackend(&engine);
    icmp->outcome = ProbeOutcome::Ok;
    auto *tcp = new FakeProbeBackend(&engine);
    tcp->hold = true;
    engine.dispatcher()->setBackend(ProbeKind::Icmp, icmp);
    engine.dispatcher()->setBackend(ProbeKind::Tcp, tcp);
    // Положительный ответ нужен для TCP-ветки; reverse lookup localhost остаётся локальным.
    auto dialog = std::make_unique<ScanDialog>(&engine, QStringLiteral("127.0.0.1"));
    const auto edits = dialog->findChildren<QLineEdit *>();
    auto *go = runButton(*dialog);
    auto *progress = dialog->findChild<QProgressBar *>();
    auto *table = dialog->findChild<QTableWidget *>();
    QVERIFY(edits.size() >= 2 && go && progress && table);
    edits.at(1)->setText(QStringLiteral("80 443"));
    go->click();
    QTRY_COMPARE_WITH_TIMEOUT(progress->value(), 1, 2000);
    QCOMPARE(go->text(), QStringLiteral("Сканировать"));
    QCOMPARE(tcp->starts.size(), 2);
    QCOMPARE(engine.dispatcher()->inFlight(), 2);
    QCOMPARE(table->rowCount(), 1);

    tcp->answer(tcp->starts.first().request, ProbeOutcome::Ok);
    QTRY_COMPARE_WITH_TIMEOUT(engine.dispatcher()->inFlight(), 1, 1000);
    QVERIFY(table->item(0, 5));
    QCOMPARE(table->item(0, 5)->text(), QStringLiteral("80"));
    const ProbeRequest unfinished = tcp->starts.last().request;
    QPointer<ScanDialog> closing(dialog.release());
    closing->close();
    QTRY_VERIFY_WITH_TIMEOUT(closing.isNull(), 1000);
    QCOMPARE(engine.dispatcher()->inFlight(), 0);
    QVERIFY(tcp->active.isEmpty());
    QVERIFY(tcp->cancelled.contains(unfinished.token));
    tcp->answer(unfinished, ProbeOutcome::Ok);
    QTest::qWait(50);   // ответ после удаления диалога безопасно отбрасывается
    QCOMPARE(engine.dispatcher()->inFlight(), 0);
}

void TestTools::scanRestartCancelsOldPorts()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Engine engine(dir.filePath(QStringLiteral("tools.json")), false);
    auto *icmp = new FakeProbeBackend(&engine);
    icmp->outcome = ProbeOutcome::Ok;
    auto *tcp = new FakeProbeBackend(&engine);
    tcp->hold = true;
    engine.dispatcher()->setBackend(ProbeKind::Icmp, icmp);
    engine.dispatcher()->setBackend(ProbeKind::Tcp, tcp);
    auto dialog = std::make_unique<ScanDialog>(&engine, QStringLiteral("127.0.0.1"));
    const auto edits = dialog->findChildren<QLineEdit *>();
    auto *go = runButton(*dialog);
    auto *progress = dialog->findChild<QProgressBar *>();
    auto *table = dialog->findChild<QTableWidget *>();
    QVERIFY(edits.size() >= 2 && go && progress && table);
    edits.at(1)->setText(QStringLiteral("80 443"));
    go->click();
    QTRY_COMPARE_WITH_TIMEOUT(go->text(), QStringLiteral("Сканировать"), 2000);
    QCOMPARE(tcp->starts.size(), 2);
    QCOMPARE(engine.dispatcher()->inFlight(), 2);
    QVector<ProbeRequest> stale;
    for (const auto &started : tcp->starts)
        stale.append(started.request);

    icmp->hold = true;
    edits.at(0)->setText(QStringLiteral("198.51.100.200"));
    edits.at(1)->clear();
    go->click();
    QCOMPARE(engine.dispatcher()->inFlight(), 0);   // старые TCP отменены до первого нового ICMP
    QVERIFY(tcp->active.isEmpty());
    for (const ProbeRequest &request : stale) {
        QVERIFY(tcp->cancelled.contains(request.token));
        tcp->answer(request, ProbeOutcome::Ok);
    }
    QTRY_COMPARE_WITH_TIMEOUT(icmp->starts.size(), 2, 1500);
    QCOMPARE(progress->value(), 0);
    QCOMPARE(table->rowCount(), 0);
    QCOMPARE(engine.dispatcher()->inFlight(), 1);
    const ProbeRequest pending = icmp->starts.last().request;
    QPointer<ScanDialog> deleted(dialog.get());
    dialog.reset();
    QVERIFY(deleted.isNull());
    QCOMPARE(engine.dispatcher()->inFlight(), 0);
    QVERIFY(icmp->cancelled.contains(pending.token));
    icmp->answer(pending, ProbeOutcome::Ok);
    for (const ProbeRequest &request : stale)
        tcp->answer(request, ProbeOutcome::Ok);
    QTest::qWait(50);
    QCOMPARE(engine.dispatcher()->inFlight(), 0);
}

QTEST_MAIN(TestTools)
#include "test_tools.moc"
