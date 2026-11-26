// Источники, сохранность секретов и границы ответов: локальные серверы без внешней сети.
#include "core/Config.h"
#include "integrations/SourceReplyGuard.h"
#include "service/Engine.h"

#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
#include <functional>

using namespace zr;

class SourceServer : public QTcpServer {
public:
    SourceServer()
    {
        connect(this, &QTcpServer::newConnection, this, [this]() {
            while (hasPendingConnections()) {
                QTcpSocket *s = nextPendingConnection();
                connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
                connect(s, &QTcpSocket::readyRead, s, [this, s]() {
                    if (s->property("handled").toBool()) { s->readAll(); return; }
                    const QByteArray request = s->property("request").toByteArray() + s->readAll();
                    s->setProperty("request", request);
                    if (!request.contains("\r\n\r\n")) return;
                    s->setProperty("handled", true);
                    const QByteArray path = request.split(' ').value(1);
                    if (custom) { custom(s, path); return; }
                    if (path == "/login") {
                        ++logins;
                        if (holdLogin) { held = s; return; }
                        respond(s, badAuth ? 401 : 200, badAuth ? QByteArrayLiteral("{\"error\":\"bad_password\"}") : QByteArrayLiteral("{\"ok\":true}"));
                    } else if (path == "/phones") {
                        respond(s, 200, phones);
                    } else {
                        respond(s, 200, QByteArrayLiteral("{\"ok\":true}"));
                    }
                });
            }
        });
    }

    QString url() const { return QStringLiteral("http://127.0.0.1:%1").arg(serverPort()); }
    static void respond(QTcpSocket *s, int code, const QByteArray &body)
    {
        s->write("HTTP/1.1 " + QByteArray::number(code) + " Test\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                 + QByteArray::number(body.size()) + "\r\n\r\n" + body);
        s->disconnectFromHost();
    }

    bool holdLogin = false;
    bool badAuth = false;
    int logins = 0;
    QPointer<QTcpSocket> held;
    QByteArray phones = QByteArrayLiteral("{\"phones\":[{\"number\":\"101\",\"name\":\"New\",\"ip\":\"10.0.0.22\"}]}");
    std::function<void(QTcpSocket *, const QByteArray &)> custom;
};

class TestEngine : public QObject {
    Q_OBJECT
private slots:
    void demoUsesFictionalTargets();
    void removeSourceAliasedId();
    void changedSourceDiscardsPendingSync();
    void authFailureBlocksAutomaticSync();
    void malformedSecretsArePreserved();
    void unknownConfigVersionIsPreserved();
    void sourceBodyLimit();
    void sourceAbsoluteDeadline();
};

void TestEngine::demoUsesFictionalTargets()
{
    Engine engine(QString(), true);
    QString err;
    QVERIFY(engine.load(&err));
    QCOMPARE(engine.monitor()->targets().size(), 19);
    const QRegularExpression phoneNumber(QStringLiteral("\\d{7,}"));
    for (const TargetConfig &target : engine.monitor()->targets()) {
        const QHostAddress address(target.hostPart());
        QVERIFY(address.isInSubnet(QHostAddress(QStringLiteral("192.0.2.0")), 24)
                || address.isInSubnet(QHostAddress(QStringLiteral("198.51.100.0")), 24)
                || address.isInSubnet(QHostAddress(QStringLiteral("203.0.113.0")), 24));
        QVERIFY(!phoneNumber.match(target.name).hasMatch());
    }
}

void TestEngine::removeSourceAliasedId()
{
    QTemporaryDir dir;
    Engine engine(dir.filePath(QStringLiteral("z.json")), false);
    QString err;
    QVERIFY(engine.load(&err));
    for (const QString &id : {QStringLiteral("first"), QStringLiteral("last")}) {
        SourceConfig source;
        source.id = id;
        source.kind = QStringLiteral("inei");
        engine.upsertSource(source);
        TargetConfig t;
        t.id = id + QStringLiteral("-target");
        t.address = id == QLatin1String("first") ? QStringLiteral("10.0.0.1") : QStringLiteral("10.0.0.2");
        t.sourceId = id;
        engine.monitor()->addTarget(t);
    }
    engine.removeSource(engine.source(QStringLiteral("first"))->id, true);
    QVERIFY(!engine.source(QStringLiteral("first")));
    QCOMPARE(engine.monitor()->targets().size(), 1);
    QCOMPARE(engine.monitor()->targets().first().sourceId, QStringLiteral("last"));
    engine.removeSource(engine.source(QStringLiteral("last"))->id, false);
    QCOMPARE(engine.monitor()->targets().size(), 1);
    QVERIFY(engine.monitor()->targets().first().sourceId.isEmpty());
    QVERIFY(engine.config().sources.isEmpty());
}

void TestEngine::changedSourceDiscardsPendingSync()
{
    QTemporaryDir dir;
    SourceServer oldServer, newServer;
    oldServer.holdLogin = true;
    QVERIFY(oldServer.listen(QHostAddress::LocalHost, 0));
    QVERIFY(newServer.listen(QHostAddress::LocalHost, 0));
    Engine engine(dir.filePath(QStringLiteral("z.json")), false);
    QString err;
    QVERIFY(engine.load(&err));
    SourceConfig source;
    source.id = QStringLiteral("s");
    source.kind = QStringLiteral("inei");
    source.url = oldServer.url();
    source.password = QStringLiteral("test-password");
    engine.upsertSource(source);
    QSignalSpy done(&engine, &Engine::sourceSynced);
    engine.syncSource(source.id);
    QTRY_COMPARE(oldServer.logins, 1);
    source.url = newServer.url();
    engine.upsertSource(source);
    engine.syncSource(source.id);
    QTRY_COMPARE(done.count(), 1);
    QVERIFY(done.first().at(1).toBool());
    QCOMPARE(newServer.logins, 1);
    QCOMPARE(engine.monitor()->targets().size(), 1);
    QCOMPARE(engine.monitor()->targets().first().address, QStringLiteral("10.0.0.22"));
    if (oldServer.held && oldServer.held->state() == QAbstractSocket::ConnectedState)
        SourceServer::respond(oldServer.held, 200, QByteArrayLiteral("{\"ok\":true}"));
    QTest::qWait(60);
    QCOMPARE(done.count(), 1);
    QCOMPARE(engine.source(source.id)->url, newServer.url());
}

void TestEngine::authFailureBlocksAutomaticSync()
{
    QTemporaryDir dir;
    SourceServer server;
    server.badAuth = true;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    Engine engine(dir.filePath(QStringLiteral("z.json")), false);
    QString err;
    QVERIFY(engine.load(&err));
    SourceConfig source;
    source.id = QStringLiteral("auth");
    source.kind = QStringLiteral("inei");
    source.url = server.url();
    source.password = QStringLiteral("wrong-test-password");
    source.remember = true;
    source.syncMinutes = 1;
    engine.upsertSource(source);
    QSignalSpy done(&engine, &Engine::sourceSynced);
    engine.syncSource(source.id);
    QTRY_COMPARE(done.count(), 1);
    QVERIFY(!done.first().at(1).toBool());
    QVERIFY(engine.source(source.id)->syncBlocked);
    QVERIFY(!engine.source(source.id)->autoSyncDue(QDateTime::currentMSecsSinceEpoch() + 86400000));
    engine.save();
    AppConfig saved;
    QVERIFY(engine.store().load(saved, &err));
    QVERIFY(saved.sources.first().syncBlocked);
    QVERIFY(!saved.sources.first().autoSyncDue(QDateTime::currentMSecsSinceEpoch() + 86400000));
    QCOMPARE(server.logins, 1);
    // Явная ручная попытка разрешена, но ошибка опять блокирует расписание.
    engine.syncSource(source.id);
    QTRY_COMPARE(done.count(), 2);
    QCOMPARE(server.logins, 2);
    QVERIFY(engine.source(source.id)->syncBlocked);
    source.password = QStringLiteral("corrected");
    engine.upsertSource(source);
    QVERIFY(!engine.source(source.id)->syncBlocked);
}

void TestEngine::malformedSecretsArePreserved()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("z.json"));
    ConfigStore store(path);
    AppConfig cfg;
    QString err;
    QVERIFY(store.save(cfg, &err));
    QFile secrets(dir.filePath(QStringLiteral("secrets.json")));
    QVERIFY(secrets.open(QIODevice::WriteOnly));
    const QByteArray original("{ broken secret bytes");
    secrets.write(original);
    secrets.close();
    QFile config(path);
    QVERIFY(config.open(QIODevice::ReadOnly));
    const QByteArray originalConfig = config.readAll();
    config.close();
    AppConfig back;
    QVERIFY(!store.load(back, &err));
    QVERIFY(err.contains(QStringLiteral("паролей")));
    QVERIFY(!store.save(cfg, &err));
    QVERIFY(secrets.open(QIODevice::ReadOnly));
    QCOMPARE(secrets.readAll(), original);
    QVERIFY(config.open(QIODevice::ReadOnly));
    QCOMPARE(config.readAll(), originalConfig);
}

void TestEngine::unknownConfigVersionIsPreserved()
{
    QTemporaryDir dir;
    ConfigStore store(dir.filePath(QStringLiteral("future.json")));
    QJsonObject object = ConfigStore::toJson(AppConfig());
    object[QStringLiteral("version")] = 99;
    const QByteArray original = QJsonDocument(object).toJson();
    QString err;
    QVERIFY(writeFileAtomic(store.path(), original, true, &err));
    AppConfig cfg;
    QVERIFY(!store.load(cfg, &err));
    QVERIFY(err.contains(QStringLiteral("версия")));
    QFile file(store.path());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), original);
}

void TestEngine::sourceBodyLimit()
{
    SourceServer server;
    server.custom = [](QTcpSocket *s, const QByteArray &) {
        s->write("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n" + QByteArray(40, 'x'));
    };
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    QNetworkAccessManager nam;
    QNetworkReply *reply = nam.get(QNetworkRequest(QUrl(server.url())));
    guardSourceReply(reply, 1000, 32);
    QSignalSpy done(reply, &QNetworkReply::finished);
    QTRY_COMPARE(done.count(), 1);
    QCOMPARE(sourceHttpStatus(reply), 0);
    QVERIFY(sourceReplyError(reply).contains(QStringLiteral("размер")));
    reply->deleteLater();
}

void TestEngine::sourceAbsoluteDeadline()
{
    SourceServer server;
    server.custom = [](QTcpSocket *s, const QByteArray &) {
        s->write("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\na");
        auto *trickle = new QTimer(s);
        QObject::connect(trickle, &QTimer::timeout, s, [s]() {
            if (s->state() == QAbstractSocket::ConnectedState) s->write("a");
        });
        trickle->start(20);
    };
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    QNetworkAccessManager nam;
    QNetworkReply *reply = nam.get(QNetworkRequest(QUrl(server.url())));
    guardSourceReply(reply, 120, 256);
    QSignalSpy done(reply, &QNetworkReply::finished);
    QTRY_COMPARE(done.count(), 1);
    QCOMPARE(sourceHttpStatus(reply), 0);
    QVERIFY(sourceReplyError(reply).contains(QStringLiteral("срок")));
    reply->deleteLater();
}

QTEST_GUILESS_MAIN(TestEngine)
#include "test_engine.moc"
