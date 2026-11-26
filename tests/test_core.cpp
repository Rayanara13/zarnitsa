// Ядро: пакеты ICMP, статистика и MOS, разбор ввода, автомат статусов, конфиг.
#include "core/Config.h"
#include "core/Monitor.h"
#include "core/Stats.h"
#include "core/Target.h"
#include "icmp/icmp_packet.h"
#include "probes/PingOutput.h"
#include "probes/ProbeDispatcher.h"

#include <QFile>
#include <QHostAddress>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <arpa/inet.h>
#include <cstring>

using namespace zr;

class TestCore : public QObject {
    Q_OBJECT
private slots:
    void icmpChecksumAndEcho();
    void icmpParseRawReplyAndTimeExceeded();
    void icmpParseDgramAndOriginal();
    void statsWindow();
    void statsMos();
    void statsMinutesAndUptime();
    void parseInput_data();
    void parseInput();
    void pingOutput();
    void monitorUpDown();
    void monitorDegradedByLoss();
    void monitorDegradedByLatency();
    void monitorFlapping();
    void monitorParentDependency();
    void monitorPauseAndMove();
    void monitorGroups();
    void configRoundtrip();
    void publicUrlHidesSecrets();
};

void TestCore::icmpChecksumAndEcho()
{
    // RFC 1071: пример 00 01 f2 03 f4 f5 f6 f7 → сумма 0xddf2, контрольная 0x220d
    const uint8_t sample[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
    QCOMPARE(zicmp::checksum(sample, sizeof sample), uint16_t(0x220d));

    uint8_t buf[128];
    const size_t n = zicmp::buildEcho(buf, sizeof buf, 0x1234, 0xabcd, 56);
    QCOMPARE(n, size_t(64));
    QCOMPARE(int(buf[0]), 8);
    QCOMPARE(zicmp::checksum(buf, n), uint16_t(0));   // корректный пакет даёт ноль
    QCOMPARE(int(buf[4]), 0x12);
    QCOMPARE(int(buf[7]), 0xcd);
    QCOMPARE(zicmp::buildEcho(buf, 10, 1, 1, 56), size_t(0));
}

static QByteArray ipHeader(int ttl, const char *src, int payload)
{
    QByteArray h(20, '\0');
    h[0] = char(0x45);
    h[2] = char((20 + payload) >> 8);
    h[3] = char((20 + payload) & 0xff);
    h[8] = char(ttl);
    h[9] = 1;
    QHostAddress a(QString::fromLatin1(src));
    const quint32 v = a.toIPv4Address();
    h[12] = char(v >> 24); h[13] = char(v >> 16); h[14] = char(v >> 8); h[15] = char(v);
    return h;
}

void TestCore::icmpParseRawReplyAndTimeExceeded()
{
    // эхо-ответ: IP + ICMP type 0
    uint8_t echo[64];
    zicmp::buildEcho(echo, sizeof echo, 0x0102, 0x0304, 56);
    QByteArray reply = ipHeader(57, "10.0.0.7", 64) + QByteArray(reinterpret_cast<char *>(echo), 64);
    reply[20] = 0;   // type 0
    zicmp::Parsed p;
    QVERIFY(zicmp::parseRaw(reinterpret_cast<const uint8_t *>(reply.constData()), size_t(reply.size()), p));
    QCOMPARE(int(p.kind), int(zicmp::Kind::Reply));
    QCOMPARE(p.id, uint16_t(0x0102));
    QCOMPARE(p.seq, uint16_t(0x0304));
    QCOMPARE(p.ttl, 57);
    QCOMPARE(QHostAddress(ntohl(p.from)).toString(), QStringLiteral("10.0.0.7"));

    // истёк TTL: внешний IP от маршрутизатора, внутри — наш исходный запрос
    QByteArray inner = ipHeader(1, "192.0.2.252", 64) + QByteArray(reinterpret_cast<char *>(echo), 8);
    QByteArray icmp(8, '\0');
    icmp[0] = 11;
    QByteArray tx = ipHeader(254, "192.0.2.1", 8 + inner.size()) + icmp + inner;
    QVERIFY(zicmp::parseRaw(reinterpret_cast<const uint8_t *>(tx.constData()), size_t(tx.size()), p));
    QCOMPARE(int(p.kind), int(zicmp::Kind::TimeExceeded));
    QCOMPARE(p.seq, uint16_t(0x0304));
    QCOMPARE(QHostAddress(ntohl(p.from)).toString(), QStringLiteral("192.0.2.1"));

    // мусор не принимаем
    QVERIFY(!zicmp::parseRaw(reinterpret_cast<const uint8_t *>(tx.constData()), 10, p));
}

void TestCore::icmpParseDgramAndOriginal()
{
    uint8_t echo[24];
    zicmp::buildEcho(echo, sizeof echo, 7, 99, 16);
    uint16_t id = 0, seq = 0;
    QVERIFY(zicmp::parseOriginalEcho(echo, sizeof echo, id, seq));
    QCOMPARE(seq, uint16_t(99));
    echo[0] = 0;
    zicmp::Parsed p;
    QVERIFY(zicmp::parseDgram(echo, sizeof echo, p));
    QCOMPARE(int(p.kind), int(zicmp::Kind::Reply));
    QCOMPARE(p.seq, uint16_t(99));
    QCOMPARE(p.ttl, -1);
}

void TestCore::statsWindow()
{
    TargetStats s(100);
    const qint64 t0 = 1700000000000;
    const double rtts[] = {10, 12, -1, 14, 10, -1, 11, 13, 12, 10};
    for (int i = 0; i < 10; ++i) s.add(t0 + i * 1000, rtts[i] >= 0, rtts[i]);
    const WindowStats w = s.window(10);
    QCOMPARE(w.sent, 10);
    QCOMPARE(w.recv, 8);
    QCOMPARE(w.lossPct, 20.0);
    QCOMPARE(w.min, 10.0);
    QCOMPARE(w.max, 14.0);
    QCOMPARE(w.avg, 11.5);
    QCOMPARE(w.last, 10.0);
    // джиттер — среднее |Δ| между соседними ответами: 2,2,4,1,2,1,2 → 14/7
    QCOMPARE(w.jitter, 2.0);
    QCOMPARE(s.avgOfLastOk(2), 11.0);
    QCOMPARE(s.bestEver(), 10.0);
    // кольцо не растёт сверх ёмкости
    for (int i = 0; i < 500; ++i) s.add(t0 + 20000 + i, true, 1);
    QCOMPARE(s.count(), 100);
}

void TestCore::statsMos()
{
    const double good = TargetStats::mos(20, 2, 0);
    QVERIFY2(good > 4.3 && good <= 4.5, qPrintable(QString::number(good)));
    const double lossy = TargetStats::mos(20, 2, 10);
    QVERIFY(lossy < 3.7);
    QVERIFY(TargetStats::mos(400, 50, 0) < TargetStats::mos(100, 5, 0));
    QCOMPARE(TargetStats::mos(-1, 0, 0), 1.0);
    QCOMPARE(TargetStats::mos(20, 2, 100), 1.0);
    QCOMPARE(TargetStats::mosTitle(4.4), QStringLiteral("отлично"));
}

void TestCore::statsMinutesAndUptime()
{
    TargetStats s;
    const qint64 base = 1700000000000 / 60000 * 60000;
    for (int i = 0; i < 120; ++i) s.add(base + i * 1000, i % 4 != 0, 5);   // 25% потерь, две минуты
    QCOMPARE(s.uptimePct(base + 119000), 75.0);
    const QVector<MinuteBucket> m = s.minuteSeries(base + 119000, 3);
    QCOMPARE(m.size(), 3);
    QCOMPARE(int(m[1].sent), 60);
    QCOMPARE(int(m[1].recv), 45);
    QCOMPARE(m[0].sent, quint16(0));   // минута до начала — пустая
    QCOMPARE(s.uptimePct(base + 119000 + 3 * 86400000LL), -1.0);
}

void TestCore::parseInput_data()
{
    QTest::addColumn<QString>("line");
    QTest::addColumn<bool>("ok");
    QTest::addColumn<int>("kind");
    QTest::addColumn<QString>("address");
    QTest::addColumn<int>("port");
    QTest::addColumn<QString>("name");
    QTest::newRow("ip") << "10.0.0.1" << true << int(ProbeKind::Icmp) << "10.0.0.1" << 0 << "";
    QTest::newRow("label") << "10.0.0.1 # core switch" << true << int(ProbeKind::Icmp) << "10.0.0.1" << 0 << "core switch";
    QTest::newRow("tcp") << "srv.local:443" << true << int(ProbeKind::Tcp) << "srv.local" << 443 << "";
    QTest::newRow("sip default") << "sip:10.1.1.30" << true << int(ProbeKind::Sip) << "10.1.1.30" << 5060 << "";
    QTest::newRow("sip port") << "sip:192.0.2.251:15060" << true << int(ProbeKind::Sip) << "192.0.2.251" << 15060 << "";
    QTest::newRow("https") << "https://192.0.2.251:18443/healthz" << true << int(ProbeKind::Http)
                           << "https://192.0.2.251:18443/healthz" << 0 << "";
    QTest::newRow("dns") << "dns:ya.ru" << true << int(ProbeKind::Dns) << "ya.ru" << 0 << "";
    QTest::newRow("v6") << "fe80::1" << true << int(ProbeKind::Icmp) << "fe80::1" << 0 << "";
    QTest::newRow("v6 port") << "[fe80::1]:22" << true << int(ProbeKind::Tcp) << "fe80::1" << 22 << "";
    QTest::newRow("tcp no port") << "tcp:host" << false << 0 << "" << 0 << "";
    QTest::newRow("bad port") << "host:99999" << false << 0 << "" << 0 << "";
    QTest::newRow("garbage") << "не адрес" << false << 0 << "" << 0 << "";
    QTest::newRow("empty") << "   " << false << 0 << "" << 0 << "";
}

void TestCore::parseInput()
{
    QFETCH(QString, line);
    QFETCH(bool, ok);
    const ParsedInput p = parseTargetInput(line);
    QCOMPARE(p.ok, ok);
    if (!ok) return;
    QFETCH(int, kind);
    QFETCH(QString, address);
    QFETCH(int, port);
    QFETCH(QString, name);
    QCOMPARE(int(p.target.kind), kind);
    QCOMPARE(p.target.address, address);
    QCOMPARE(p.target.port, port);
    QCOMPARE(p.target.name, name);
}

void TestCore::pingOutput()
{
    PingParse p = parsePingOutput(QStringLiteral(
        "PING 192.0.2.1 (192.0.2.1) 56(84) bytes of data.\n64 bytes from 192.0.2.1: icmp_seq=1 ttl=64 time=0.415 ms\n\n"
        "--- 192.0.2.1 ping statistics ---\n1 packets transmitted, 1 received, 0% packet loss, time 0ms\n"));
    QCOMPARE(int(p.kind), int(PingParse::Reply));
    QCOMPARE(p.rttMs, 0.415);
    QCOMPARE(p.ttl, 64);
    QCOMPARE(p.from, QStringLiteral("192.0.2.1"));

    p = parsePingOutput(QStringLiteral("PING 8.8.8.8 (8.8.8.8) 56(84) bytes of data.\nFrom 192.0.2.1 icmp_seq=1 Time to live exceeded\n"));
    QCOMPARE(int(p.kind), int(PingParse::TtlExceeded));
    QCOMPARE(p.from, QStringLiteral("192.0.2.1"));

    p = parsePingOutput(QStringLiteral("From 192.0.2.252 icmp_seq=1 Destination Host Unreachable\n"));
    QCOMPARE(int(p.kind), int(PingParse::Unreachable));

    p = parsePingOutput(QStringLiteral("ping: socket: Operation not permitted\n"));
    QCOMPARE(int(p.kind), int(PingParse::Error));
    QCOMPARE(p.error, QStringLiteral("socket: Operation not permitted"));

    p = parsePingOutput(QStringLiteral("PING 10.255.255.1 (10.255.255.1) 56(84) bytes of data.\n\n1 packets transmitted, 0 received, 100% packet loss\n"));
    QCOMPARE(int(p.kind), int(PingParse::NoReply));

    // busybox и имя хоста в скобках
    p = parsePingOutput(QStringLiteral("64 bytes from host.local (10.0.0.9): seq=0 ttl=63 time=1,25 ms\n"));
    QCOMPARE(int(p.kind), int(PingParse::Reply));
    QCOMPARE(p.from, QStringLiteral("10.0.0.9"));
    QCOMPARE(p.rttMs, 1.25);
}

static QString addPlain(Monitor &m, const QString &addr, const QString &group = QString())
{
    TargetConfig t = parseTargetInput(addr).target;
    t.group = group;
    return m.addTarget(t);
}

void TestCore::monitorUpDown()
{
    ProbeDispatcher d;
    Monitor m(&d);
    Settings s;
    m.setSettings(s);
    const QString id = addPlain(m, QStringLiteral("10.0.0.1"));
    QSignalSpy spy(&m, &Monitor::statusChanged);
    QCOMPARE(m.runtime(id)->status, Status::Unknown);
    m.injectResult(id, true, 1.0);
    QCOMPARE(m.runtime(id)->status, Status::Up);
    m.injectResult(id, false, -1, QStringLiteral("Таймаут"));
    m.injectResult(id, false, -1, QStringLiteral("Таймаут"));
    QCOMPARE(m.runtime(id)->status, Status::Up);   // две неудачи — ещё не «нет связи»
    m.injectResult(id, false, -1, QStringLiteral("Таймаут"));
    QCOMPARE(m.runtime(id)->status, Status::Down);
    QCOMPARE(m.runtime(id)->reason, QStringLiteral("Таймаут"));
    m.injectResult(id, true, 1.0);
    QCOMPARE(m.runtime(id)->status, Status::Down);   // один ответ — ещё лежит
    m.injectResult(id, true, 1.0);
    QCOMPARE(m.runtime(id)->status, Status::Up);
    QCOMPARE(spy.count(), 3);
    const StatusEvent ev = spy.last().first().value<StatusEvent>();
    QCOMPARE(ev.from, Status::Down);
    QCOMPARE(ev.to, Status::Up);
    QVERIFY(ev.downForMs >= 0);
    QCOMPARE(m.summary().up, 1);
}

void TestCore::monitorDegradedByLoss()
{
    ProbeDispatcher d;
    Monitor m(&d);
    Settings s;
    s.statsWindow = 20;
    s.lossWarnPct = 10;
    s.latencyWarnMs = 0;
    m.setSettings(s);
    const QString id = addPlain(m, QStringLiteral("10.0.0.2"));
    for (int i = 0; i < 10; ++i) m.injectResult(id, true, 2);
    QCOMPARE(m.runtime(id)->status, Status::Up);
    // по одной потере на каждые 4 замера: 25% — «плохо», но до «нет связи» не доходит
    for (int i = 0; i < 12; ++i) m.injectResult(id, i % 4 != 0, 2, QStringLiteral("Таймаут"));
    QCOMPARE(m.runtime(id)->status, Status::Degraded);
    QVERIFY(m.runtime(id)->reason.startsWith(QStringLiteral("потери")));
    for (int i = 0; i < 25; ++i) m.injectResult(id, true, 2);
    QCOMPARE(m.runtime(id)->status, Status::Up);
}

void TestCore::monitorDegradedByLatency()
{
    ProbeDispatcher d;
    Monitor m(&d);
    Settings s;
    s.latencyWarnMs = 100;
    m.setSettings(s);
    const QString id = addPlain(m, QStringLiteral("10.0.0.3"));
    for (int i = 0; i < 6; ++i) m.injectResult(id, true, 20);
    QCOMPARE(m.runtime(id)->status, Status::Up);
    for (int i = 0; i < 8; ++i) m.injectResult(id, true, 250);
    QCOMPARE(m.runtime(id)->status, Status::Degraded);
    QVERIFY(m.runtime(id)->reason.startsWith(QStringLiteral("задержка")));

    // динамический порог: выше лучшего на 60 мс
    s.latencyDynamic = true;
    s.latencyDynamicDeltaMs = 60;
    m.setSettings(s);
    const QString id2 = addPlain(m, QStringLiteral("10.0.0.4"));
    for (int i = 0; i < 6; ++i) m.injectResult(id2, true, 5);
    for (int i = 0; i < 8; ++i) m.injectResult(id2, true, 90);
    QCOMPARE(m.runtime(id2)->status, Status::Degraded);
}

void TestCore::monitorFlapping()
{
    ProbeDispatcher d;
    Monitor m(&d);
    Settings s;
    s.flapChanges = 4;
    s.downAfter = 1;
    s.upAfter = 1;
    m.setSettings(s);
    const QString id = addPlain(m, QStringLiteral("10.0.0.5"));
    m.injectResult(id, true, 1);
    for (int i = 0; i < 3; ++i) {
        m.injectResult(id, false, -1);
        m.injectResult(id, true, 1);
    }
    QVERIFY(m.runtime(id)->flapping);
}

void TestCore::monitorParentDependency()
{
    ProbeDispatcher d;
    Monitor m(&d);
    m.setSettings(Settings());
    const QString sw = addPlain(m, QStringLiteral("10.0.0.10"));
    TargetConfig phone = parseTargetInput(QStringLiteral("10.0.0.11")).target;
    phone.parentId = sw;
    const QString ph = m.addTarget(phone);
    QSignalSpy spy(&m, &Monitor::statusChanged);
    for (int i = 0; i < 3; ++i) m.injectResult(sw, false, -1);
    for (int i = 0; i < 3; ++i) m.injectResult(ph, false, -1);
    QCOMPARE(m.runtime(ph)->status, Status::Down);
    QVERIFY(m.runtime(ph)->dependent);
    const StatusEvent ev = spy.last().first().value<StatusEvent>();
    QCOMPARE(ev.id, ph);
    QVERIFY(ev.dependent);
    // удаление родителя снимает ссылку
    m.removeTargets({sw});
    QVERIFY(m.target(ph)->parentId.isEmpty());
}

void TestCore::monitorPauseAndMove()
{
    ProbeDispatcher d;
    Monitor m(&d);
    m.setSettings(Settings());
    const QString a = addPlain(m, QStringLiteral("10.0.1.1"), QStringLiteral("A"));
    const QString b = addPlain(m, QStringLiteral("10.0.1.2"), QStringLiteral("A"));
    const QString c = addPlain(m, QStringLiteral("10.0.1.3"), QStringLiteral("B"));
    m.setEnabled({a}, false);
    QCOMPARE(m.runtime(a)->status, Status::Paused);
    m.injectResult(a, true, 1);
    QCOMPARE(m.runtime(a)->status, Status::Paused);   // на паузе замеры не влияют
    m.setEnabled({a}, true);
    QCOMPARE(m.runtime(a)->status, Status::Unknown);
    m.moveTargets({c}, QStringLiteral("A"), b);
    QCOMPARE(m.target(c)->group, QStringLiteral("A"));
    QStringList order;
    for (const TargetConfig &t : m.targets()) order << t.id;
    QCOMPARE(order, QStringList({a, c, b}));
    QCOMPARE(m.target(b)->order, 2);
}

void TestCore::monitorGroups()
{
    ProbeDispatcher d;
    Monitor m(&d);
    addPlain(m, QStringLiteral("10.0.2.1"), QStringLiteral("Офис 101"));
    addPlain(m, QStringLiteral("10.0.2.2"), QStringLiteral("Офис 114"));
    m.renameGroup(QStringLiteral("Офис 101"), QStringLiteral("Офис 207"));
    QCOMPARE(m.groups().first().name, QStringLiteral("Офис 207"));
    m.moveGroup(QStringLiteral("Офис 114"), 0);
    QCOMPARE(m.targets().first().group, QStringLiteral("Офис 114"));
    m.removeGroup(QStringLiteral("Офис 114"), true);
    QCOMPARE(m.targets().size(), 1);
    m.setGroupCollapsed(QStringLiteral("Офис 207"), true);
    bool collapsed = false;
    for (const GroupInfo &g : m.groups()) if (g.name == QStringLiteral("Офис 207")) collapsed = g.collapsed;
    QVERIFY(collapsed);
}

void TestCore::configRoundtrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("z.json"));
    ConfigStore store(path);
    AppConfig cfg;
    cfg.settings.intervalMs = 2500;
    cfg.settings.theme = QStringLiteral("light");
    cfg.settings.apiToken = QStringLiteral("секрет-не-секрет-1234567890");
    TargetConfig t = parseTargetInput(QStringLiteral("https://10.0.0.1/healthz # Роса")).target;
    t.tlsPin = QStringLiteral("ab12");
    t.group = QStringLiteral("ВКС");
    cfg.targets << t;
    GroupInfo g;
    g.name = QStringLiteral("ВКС");
    g.collapsed = true;
    cfg.groups << g;
    SourceConfig src;
    src.id = QStringLiteral("s1");
    src.kind = QStringLiteral("inei");
    src.url = QStringLiteral("http://10.0.0.5:8000");
    src.username = QStringLiteral("admin");
    src.password = QStringLiteral("пароль");
    src.remember = true;
    cfg.sources << src;
    QString err;
    QVERIFY2(store.save(cfg, &err), qPrintable(err));
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QVERIFY(!f.readAll().contains("пароль"));   // пароль — только в secrets.json
    QCOMPARE(QFile::permissions(path) & (QFileDevice::ReadOther | QFileDevice::ReadGroup), QFileDevice::Permissions());
    QCOMPARE(QFile::permissions(dir.filePath(QStringLiteral("secrets.json"))) & QFileDevice::ReadOther, QFileDevice::Permissions());
    AppConfig back;
    QVERIFY(store.load(back, &err));
    QCOMPARE(back.settings.intervalMs, 2500);
    QCOMPARE(back.settings.theme, QStringLiteral("light"));
    QCOMPARE(back.targets.size(), 1);
    QCOMPARE(back.targets.first().tlsPin, QStringLiteral("ab12"));
    QCOMPARE(back.targets.first().name, QStringLiteral("Роса"));
    QVERIFY(back.groups.first().collapsed);
    QCOMPARE(back.sources.first().password, QStringLiteral("пароль"));

    QFile bad(path);
    QVERIFY(bad.open(QIODevice::WriteOnly));
    bad.write("{ не json");
    bad.close();
    QVERIFY(!store.load(back, &err));
    QVERIFY(err.contains(QStringLiteral("повреждён")));
}

void TestCore::publicUrlHidesSecrets()
{
    TargetConfig target = parseTargetInput(QStringLiteral("https://alice:secret-password@example.test/healthz?token=secret-token&mode=secret-mode#secret-fragment")).target;
    const QString publicUrl = target.publicEndpointText();
    QVERIFY(!publicUrl.contains(QStringLiteral("alice")));
    QVERIFY(!publicUrl.contains(QStringLiteral("secret")));
    QVERIFY(publicUrl.contains(QStringLiteral("example.test/healthz")));
    QVERIFY(publicUrl.contains(QStringLiteral("token=")));
    QCOMPARE(target.displayName(), publicUrl);
    QVERIFY(target.address.contains(QStringLiteral("secret-password")));
    QVERIFY(target.address.contains(QStringLiteral("secret-token")));
    Monitor monitor(nullptr);
    monitor.addTarget(target);
    QSignalSpy events(&monitor, &Monitor::statusChanged);
    for (int i = 0; i < 3; ++i) monitor.injectResult(target.id, false, -1, QStringLiteral("Таймаут"));
    QCOMPARE(events.count(), 1);
    const StatusEvent event = qvariant_cast<StatusEvent>(events.first().first());
    const QByteArray logged = QJsonDocument(event.toJson()).toJson();
    QVERIFY(!logged.contains("alice"));
    QVERIFY(!logged.contains("secret"));
    QCOMPARE(event.address, publicUrl);
}

QTEST_GUILESS_MAIN(TestCore)
#include "test_core.moc"
