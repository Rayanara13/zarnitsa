// Утилита для живой проверки импорта против настоящих Инея и Росы.
//   zr-import-probe inei <url> <логин> <пароль> [--shared]
//   zr-import-probe rosa <url> <логин> <пароль> [--pin auto|<sha256>] [--mode icmp|sip]
// Печатает JSON с найденными целями; код возврата 0 — импорт удался.
#include "integrations/Inei.h"
#include "integrations/Rosa.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <cstdio>

using namespace zr;

static int print(const ImportResult &r, const QJsonObject &extra)
{
    QJsonObject o = extra;
    o[QStringLiteral("ok")] = r.ok();
    if (!r.ok()) o[QStringLiteral("error")] = r.error;
    o[QStringLiteral("warnings")] = QJsonArray::fromStringList(r.warnings);
    QJsonArray ts;
    for (const TargetConfig &t : r.targets) {
        QJsonObject j;
        j[QStringLiteral("name")] = t.name;
        j[QStringLiteral("address")] = t.address;
        j[QStringLiteral("kind")] = kindToKey(t.kind);
        j[QStringLiteral("port")] = t.port;
        j[QStringLiteral("group")] = t.group;
        j[QStringLiteral("source_key")] = t.sourceKey;
        if (!t.tlsPin.isEmpty()) j[QStringLiteral("tls_pin")] = t.tlsPin;
        ts.append(j);
    }
    o[QStringLiteral("targets")] = ts;
    std::printf("%s\n", QJsonDocument(o).toJson(QJsonDocument::Indented).constData());
    return r.ok() ? 0 : 1;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList a = app.arguments();
    if (a.size() < 5) {
        std::fprintf(stderr, "использование: zr-import-probe inei|rosa <url> <логин> <пароль> [--shared] [--pin auto|sha] [--mode icmp|sip]\n");
        return 2;
    }
    const QString kind = a[1], url = a[2], user = a[3], pass = a[4];
    const bool shared = a.contains(QStringLiteral("--shared"));
    QString pin, mode = QStringLiteral("icmp");
    const int pi = a.indexOf(QStringLiteral("--pin"));
    if (pi > 0 && pi + 1 < a.size()) pin = a[pi + 1];
    const int mi = a.indexOf(QStringLiteral("--mode"));
    if (mi > 0 && mi + 1 < a.size()) mode = a[mi + 1];
    int code = 3;
    QTimer::singleShot(30000, &app, [&]() { std::fprintf(stderr, "таймаут\n"); app.exit(4); });

    if (kind == QLatin1String("inei")) {
        IneiClient c;
        QObject::connect(&c, &IneiClient::finished, &app, [&](const ImportResult &r) {
            code = print(r, QJsonObject());
            app.quit();
        });
        c.fetch(IneiClient::normalizeUrl(url), user, pass, shared, QStringLiteral("Иней"), mode, QStringLiteral("s-inei"));
        app.exec();
        return code;
    }

    RosaClient c;
    QJsonObject extra;
    auto doFetch = [&](const QString &usePin) {
        RosaOptions o;
        o.mode = mode;
        c.fetch(RosaClient::normalizeUrl(url), user, pass, usePin, o, QStringLiteral("Роса"), QStringLiteral("s-rosa"));
    };
    QObject::connect(&c, &RosaClient::finished, &app, [&](const ImportResult &r) {
        code = print(r, extra);
        app.quit();
    });
    QObject::connect(&c, &RosaClient::certificate, &app,
                     [&](const QString &sha, const QString &subject, const QDateTime &expiry, bool trusted, const QString &error) {
                         QJsonObject cert;
                         cert[QStringLiteral("sha256")] = sha;
                         cert[QStringLiteral("subject")] = subject;
                         cert[QStringLiteral("expires")] = expiry.toString(Qt::ISODate);
                         cert[QStringLiteral("trusted")] = trusted;
                         if (!error.isEmpty()) cert[QStringLiteral("error")] = error;
                         extra[QStringLiteral("cert")] = cert;
                         if (sha.isEmpty()) {
                             ImportResult r;
                             r.error = error;
                             code = print(r, extra);
                             app.quit();
                             return;
                         }
                         doFetch(sha);
                     });
    if (pin == QLatin1String("auto")) c.probeCertificate(RosaClient::normalizeUrl(url));
    else doFetch(pin);
    app.exec();
    return code;
}
