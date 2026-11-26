// Импорт и связка: списки TXT/CSV, vmPing.xml, ответы Инея и Росы, SIP OPTIONS, слияние синхронизации.
#include "core/Monitor.h"
#include "integrations/Importers.h"
#include "integrations/Inei.h"
#include "integrations/Rosa.h"
#include "integrations/SourceSync.h"
#include "probes/NetProbers.h"
#include "probes/ProbeDispatcher.h"

#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

using namespace zr;

class TestIntegrations : public QObject {
    Q_OBJECT
private slots:
    void textList();
    void csv();
    void vmping();
    void ineiPhones();
    void ineiShared();
    void rosa();
    void sipMessages();
    void exportRoundtrip();
    void mergeSync();
};

void TestIntegrations::textList()
{
    const ImportResult r = parseTextList(QStringLiteral(
        "# Ядро\n10.0.0.1 # шлюз\nsrv:443\n\n# Офис 101\nsip:10.1.1.30\nне адрес\nhttps://10.0.0.9/healthz\n"),
        QStringLiteral("Импорт"));
    QCOMPARE(r.targets.size(), 4);
    QCOMPARE(r.warnings.size(), 1);
    QCOMPARE(r.targets[0].group, QStringLiteral("Ядро"));
    QCOMPARE(r.targets[0].name, QStringLiteral("шлюз"));
    QCOMPARE(r.targets[2].group, QStringLiteral("Офис 101"));
    QCOMPARE(int(r.targets[3].kind), int(ProbeKind::Http));
}

void TestIntegrations::csv()
{
    ImportResult r = parseCsv(QStringLiteral("\xFEFFИмя;Адрес;Группа;Тип;Порт\n\"Приёмная; 1\";10.1.1.21;Офис;icmp;\nАТС;10.1.1.2;Офис;sip;5070\nСайт;https://x.local/;;http;\n"),
                              QStringLiteral("по умолчанию"));
    QCOMPARE(r.targets.size(), 3);
    QCOMPARE(r.targets[0].name, QStringLiteral("Приёмная; 1"));
    QCOMPARE(int(r.targets[1].kind), int(ProbeKind::Sip));
    QCOMPARE(r.targets[1].port, 5070);
    QCOMPARE(r.targets[2].group, QStringLiteral("по умолчанию"));
    // без заголовка: адрес, имя, группа
    r = parseCsv(QStringLiteral("10.0.0.1,Шлюз,Ядро\n10.0.0.2,Коммутатор,Ядро\n"), QString());
    QCOMPARE(r.targets.size(), 2);
    QCOMPARE(r.targets[1].name, QStringLiteral("Коммутатор"));
    QCOMPARE(r.targets[1].group, QStringLiteral("Ядро"));
}

void TestIntegrations::vmping()
{
    const QByteArray xml =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<vmping>\n"
        "  <configuration/>\n"
        "  <favorites>\n"
        "    <favorite title=\"Web Servers\" columncount=\"2\" group=\"Production\">\n"
        "      <host>web01.example.com</host>\n"
        "      <host>192.168.1.20 # база</host>\n"
        "      <host>192.168.1.21:8080</host>\n"
        "      <host># подпись</host>\n"
        "      <host>D/example.com</host>\n"
        "    </favorite>\n"
        "    <favorite title=\"Дом\" columncount=\"1\">\n"
        "      <host>192.168.1.1</host>\n"
        "    </favorite>\n"
        "  </favorites>\n"
        "  <aliases>\n"
        "    <alias host=\"192.168.1.1\">Home Router</alias>\n"
        "    <alias host=\"web01.example.com\">Веб 1</alias>\n"
        "  </aliases>\n"
        "</vmping>\n";
    const ImportResult r = parseVmPingXml(xml);
    QVERIFY(r.ok());
    QCOMPARE(r.targets.size(), 5);
    QCOMPARE(r.targets[0].group, QStringLiteral("Production / Web Servers"));
    QCOMPARE(r.targets[0].name, QStringLiteral("Веб 1"));
    QCOMPARE(r.targets[1].name, QStringLiteral("база"));
    QCOMPARE(int(r.targets[2].kind), int(ProbeKind::Tcp));
    QCOMPARE(int(r.targets[3].kind), int(ProbeKind::Dns));
    QCOMPARE(r.targets[4].name, QStringLiteral("Home Router"));
    QCOMPARE(r.targets[4].group, QStringLiteral("Дом"));
    QVERIFY(!parseVmPingXml("<html/>").ok());
}

void TestIntegrations::ineiPhones()
{
    // ответ GET /phones (server.py: get_sorted_phones)
    const QByteArray resp = R"({"phones":[
      {"number":"101004001","name":"Приёмная","ip":"10.1.1.21","state":"В_покое","ping":"3 ms","position":0,"flags":{}},
      {"number":"101004002","name":"","ip":"10.1.1.22","state":"OFFLINE","ping":"Недоступен","position":1,"flags":{}},
      {"number":"101004003","name":"Склад","ip":"мусор","state":"OFFLINE","position":2,"flags":{}}]})";
    ImportResult r = parseIneiPhones(QJsonDocument::fromJson(resp), QStringLiteral("Иней"), QStringLiteral("icmp"),
                                     QStringLiteral("s1"), QString());
    QVERIFY(r.ok());
    QCOMPARE(r.targets.size(), 2);
    QCOMPARE(r.warnings.size(), 1);   // IP «мусор» Иней не проверяет — пропускаем мы
    QCOMPARE(r.targets[0].name, QStringLiteral("Приёмная · 101004001"));
    QCOMPARE(r.targets[1].name, QStringLiteral("101004002"));
    QCOMPARE(r.targets[0].sourceKey, QStringLiteral("101004001"));
    QCOMPARE(r.targets[0].sourceId, QStringLiteral("s1"));
    // файл phones_<user>.json — массив; режим SIP
    const QByteArray file = R"([{"number":"205","name":"Начальник","ip":"10.0.0.12","position":1,"columns":2}])";
    r = parseIneiPhones(QJsonDocument::fromJson(file), QStringLiteral("Иней"), QStringLiteral("sip"), QString(), QString());
    QCOMPARE(r.targets.size(), 1);
    QCOMPARE(int(r.targets[0].kind), int(ProbeKind::Sip));
    QCOMPARE(r.targets[0].port, 5060);
    // importFile узнаёт файл Инея по содержимому
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("phones_Оператор.json"));
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(file);
    f.close();
    r = importFile(path, QString());
    QCOMPARE(r.targets.size(), 1);
    QCOMPARE(r.targets[0].group, QStringLiteral("Иней"));
}

void TestIntegrations::ineiShared()
{
    const QByteArray resp = R"({"ok":true,"sets":[
      {"username":"operator","count":2,"phones":[{"number":"101","name":"Приёмная","ip":"10.0.0.11"},{"number":"102","name":"Касса","ip":"10.0.0.13"}]},
      {"username":"Оператор2","count":1,"phones":[{"number":"101","name":"Приёмная","ip":"10.0.0.11"}]}]})";
    const ImportResult r = parseIneiPhones(QJsonDocument::fromJson(resp), QStringLiteral("Общие"), QStringLiteral("icmp"),
                                           QStringLiteral("s2"), QStringLiteral("shared/"));
    QCOMPARE(r.targets.size(), 3);
    QCOMPARE(r.targets[0].sourceKey, QStringLiteral("shared/operator/101"));
    QCOMPARE(r.targets[2].sourceKey, QStringLiteral("shared/Оператор2/101"));
}

void TestIntegrations::rosa()
{
    // GET /api/v1/admin/sip/status (internal/telephony/server.go)
    const QByteArray sip = R"({"enabled":true,"address":"192.0.2.251:15060","state":{
      "registrations":[{"number":"1001","user_id":"x","username":"phone1001","peer":"192.0.2.30:5060",
        "registered_at":"2026-11-26T09:41:07Z","expires_at":"2026-11-26T09:46:07Z"},
        {"number":"1002","user_id":"y","username":"1002","peer":"[::1]:5060"}],
      "calls":[],"recent_calls":[]}})";
    QString sipAddr;
    ImportResult r = parseRosaSipStatus(QJsonDocument::fromJson(sip).object(), QStringLiteral("Роса"), QStringLiteral("sip"),
                                        QStringLiteral("r1"), &sipAddr);
    QCOMPARE(sipAddr, QStringLiteral("192.0.2.251:15060"));
    QCOMPARE(r.targets.size(), 1);
    QCOMPARE(r.warnings.size(), 1);
    QCOMPARE(r.targets[0].address, QStringLiteral("192.0.2.30"));
    QCOMPARE(int(r.targets[0].kind), int(ProbeKind::Sip));
    QCOMPARE(r.targets[0].port, 5060);
    QCOMPARE(r.targets[0].name, QStringLiteral("SIP 1001 · phone1001"));
    QCOMPARE(r.targets[0].sourceKey, QStringLiteral("sip/1001"));
    QCOMPARE(parseRosaSipStatus(QJsonDocument::fromJson(R"({"enabled":false,"address":""})").object(), QString(),
                                QStringLiteral("icmp"), QString(), nullptr).targets.size(), 0);

    const QByteArray h323 = R"({"enabled":true,"adapter_connected":true,"listener":"192.0.2.251:1720",
      "gatekeeper":{"state":"registered","address":"127.0.0.1:1719","alias":"7001"},"room_id":"conf1","extension":"7001","peer_ip":"192.0.2.40"})";
    r = parseRosaH323Status(QJsonDocument::fromJson(h323).object(), QStringLiteral("Роса"), QStringLiteral("sip"), QStringLiteral("r1"));
    QCOMPARE(r.targets.size(), 1);
    QCOMPARE(r.targets[0].address, QStringLiteral("192.0.2.40"));
    QCOMPARE(int(r.targets[0].kind), int(ProbeKind::Tcp));
    QCOMPARE(r.targets[0].port, 1720);

    const QVector<TargetConfig> server = rosaServerTargets(QUrl(QStringLiteral("https://192.0.2.251:18443")), QStringLiteral("ab"),
                                                           QStringLiteral("0.0.0.0:15060"), QStringLiteral("Роса"), QStringLiteral("r1"));
    QCOMPARE(server.size(), 2);
    QCOMPARE(server[0].address, QStringLiteral("https://192.0.2.251:18443/healthz"));
    QCOMPARE(server[0].tlsPin, QStringLiteral("ab"));
    QCOMPARE(server[1].address, QStringLiteral("192.0.2.251"));   // 0.0.0.0 заменён адресом сервера
    QCOMPARE(server[1].port, 15060);
    QCOMPARE(RosaClient::normalizeUrl(QStringLiteral("192.0.2.251:18443/x")).toString(), QStringLiteral("https://192.0.2.251:18443"));
    QCOMPARE(IneiClient::normalizeUrl(QStringLiteral("10.0.0.5")).toString(), QStringLiteral("http://10.0.0.5:8000"));
}

void TestIntegrations::sipMessages()
{
    const QByteArray m = SipProber::buildOptions(QStringLiteral("10.1.1.30"), 5060, QStringLiteral("10.1.1.5"), 40000,
                                                 "abc@10.1.1.5", "b1", "t1");
    QVERIFY(m.startsWith("OPTIONS sip:10.1.1.30:5060 SIP/2.0\r\n"));
    QVERIFY(m.contains("Via: SIP/2.0/UDP 10.1.1.5:40000;branch=z9hG4bKb1;rport\r\n"));
    QVERIFY(m.contains("Call-ID: abc@10.1.1.5\r\n"));
    QVERIFY(m.endsWith("Content-Length: 0\r\n\r\n"));
    QByteArray callId;
    QString reason;
    QCOMPARE(SipProber::parseResponse("SIP/2.0 200 OK\r\nVia: x\r\ncall-id: abc@10.1.1.5\r\nCSeq: 1 OPTIONS\r\n\r\n", &callId, &reason), 200);
    QCOMPARE(callId, QByteArray("abc@10.1.1.5"));
    QCOMPARE(reason, QStringLiteral("OK"));
    QCOMPARE(SipProber::parseResponse("SIP/2.0 404 Not Found\r\ni: zz\r\n\r\n", &callId, &reason), 404);
    QCOMPARE(callId, QByteArray("zz"));
    QCOMPARE(SipProber::parseResponse("INVITE sip:x SIP/2.0\r\n", &callId, &reason), 0);
}

void TestIntegrations::exportRoundtrip()
{
    const ImportResult src = parseTextList(QStringLiteral("# Г1\n10.0.0.1 # один\nsip:10.0.0.2:5070\n# Г2\nhttps://h/x\n"), QString());
    QTemporaryDir dir;
    // TXT
    const QString txt = exportTextList(src.targets);
    ImportResult back = parseTextList(txt, QString());
    QCOMPARE(back.targets.size(), 3);
    QCOMPARE(back.targets[1].port, 5070);
    QCOMPARE(back.targets[2].group, QStringLiteral("Г2"));
    // CSV
    back = parseCsv(exportCsv(src.targets), QString());
    QCOMPARE(back.targets.size(), 3);
    QCOMPARE(back.targets[0].name, QStringLiteral("один"));
    QCOMPARE(int(back.targets[1].kind), int(ProbeKind::Sip));
    // JSON Зарницы
    const QString jp = dir.filePath(QStringLiteral("z.json"));
    QFile f(jp);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(exportJson(src.targets, src.groups));
    f.close();
    back = importFile(jp, QString());
    QCOMPARE(back.targets.size(), 3);
    QVERIFY(back.targets[0].id != src.targets[0].id);   // при импорте — новые id
}

void TestIntegrations::mergeSync()
{
    ProbeDispatcher d;
    Monitor m(&d);
    const QByteArray v1 = R"({"phones":[{"number":"1","name":"А","ip":"10.0.0.1"},{"number":"2","name":"Б","ip":"10.0.0.2"}]})";
    SyncReport rep = mergeImport(&m, QStringLiteral("s1"), QStringLiteral("Иней"),
                                 parseIneiPhones(QJsonDocument::fromJson(v1), QStringLiteral("Иней"), QStringLiteral("icmp"), QStringLiteral("s1"), QString()));
    QCOMPARE(rep.added, 2);
    // человек поменял порог у первой цели — синхронизация его не трогает
    TargetConfig first = *m.findBySource(QStringLiteral("s1"), QStringLiteral("1"));
    first.latencyWarnMs = 333;
    m.updateTarget(first);
    const QByteArray v2 = R"({"phones":[{"number":"1","name":"А","ip":"10.0.0.11"},{"number":"3","name":"В","ip":"10.0.0.3"}]})";
    rep = mergeImport(&m, QStringLiteral("s1"), QStringLiteral("Иней"),
                      parseIneiPhones(QJsonDocument::fromJson(v2), QStringLiteral("Иней"), QStringLiteral("icmp"), QStringLiteral("s1"), QString()));
    QCOMPARE(rep.added, 1);
    QCOMPARE(rep.updated, 1);
    QCOMPARE(rep.missing, 1);
    const TargetConfig *one = m.findBySource(QStringLiteral("s1"), QStringLiteral("1"));
    QCOMPARE(one->address, QStringLiteral("10.0.0.11"));
    QCOMPARE(one->latencyWarnMs, 333);
    QVERIFY(m.findBySource(QStringLiteral("s1"), QStringLiteral("2"))->sourceMissing);
    QCOMPARE(m.targets().size(), 3);   // пропавшую не удалили
}

QTEST_GUILESS_MAIN(TestIntegrations)
#include "test_integrations.moc"
