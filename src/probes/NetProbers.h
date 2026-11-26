// Проверки TCP, HTTP(S), SIP OPTIONS и DNS, плюс демо-исполнитель для показа и скриншотов.
#pragma once

#include "Probe.h"

#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>

class QTcpSocket;
class QUdpSocket;
class QNetworkReply;

namespace zr {

class TcpProber : public ProbeBackend {
    Q_OBJECT
public:
    explicit TcpProber(QObject *parent = nullptr);
    void start(const ProbeRequest &req) override;
    void cancel(quint64 token) override;

private:
    struct Run {
        ProbeRequest req;
        QTcpSocket *sock = nullptr;
        QTimer *timer = nullptr;
        QElapsedTimer clock;
    };
    void done(quint64 token, ProbeOutcome o, const QString &err = QString());
    QHash<quint64, Run> m_runs;
};

class HttpProber : public ProbeBackend {
    Q_OBJECT
public:
    explicit HttpProber(QObject *parent = nullptr);
    void start(const ProbeRequest &req) override;
    void cancel(quint64 token) override;

    static QString certSha256(const QByteArray &der);

private:
    struct Run {
        ProbeRequest req;
        QPointer<QNetworkReply> reply;
        QTimer *timer = nullptr;
        QElapsedTimer clock;
        double headersMs = -1.0;
        QByteArray body;
        bool timedOut = false;
        bool pinFailed = false;
        QString pinSeen;
    };
    void onFinished(quint64 token);
    QNetworkAccessManager *namFor(const ProbeRequest &req);
    // Отдельный менеджер на каждую политику TLS: QNAM переиспользует соединения по хосту и порту
    // и не проверяет сертификат заново — иначе цель без отпечатка «проедет» по чужому соединению.
    // Отпечаток имеет приоритет над insecure и в ключе менеджера, и при проверке сертификата.
    QHash<QString, QNetworkAccessManager *> m_nams;
    QHash<quint64, Run> m_runs;
};

class SipProber : public ProbeBackend {
    Q_OBJECT
public:
    explicit SipProber(QObject *parent = nullptr);
    void start(const ProbeRequest &req) override;
    void cancel(quint64 token) override;

    static QByteArray buildOptions(const QString &host, int port, const QString &localIp, int localPort,
                                   const QByteArray &callId, const QByteArray &branch, const QByteArray &tag);
    // Возвращает код ответа (0 — не SIP-ответ), Call-ID и текст причины.
    static int parseResponse(const QByteArray &datagram, QByteArray *callId, QString *reason);

private:
    struct Pend {
        ProbeRequest req;
        QHostAddress peer;
        quint16 port = 0;
        QElapsedTimer clock;
        qint64 deadline = 0;
    };
    bool ensureSocket(QString *err);
    QString localIpFor(const QString &target, int port);
    void onReadyRead();
    void expire();

    QUdpSocket *m_sock = nullptr;
    QHash<QByteArray, Pend> m_pending;      // по Call-ID
    QHash<quint64, QByteArray> m_byToken;
    QHash<QString, QPair<QString, qint64>> m_localIps;
    QTimer m_timer;
    QElapsedTimer m_clock;
};

class DnsProber : public ProbeBackend {
    Q_OBJECT
public:
    explicit DnsProber(QObject *parent = nullptr);
    void start(const ProbeRequest &req) override;
    void cancel(quint64 token) override;

private:
    struct Run {
        ProbeRequest req;
        int lookupId = -1;
        QTimer *timer = nullptr;
        QElapsedTimer clock;
    };
    QHash<quint64, Run> m_runs;
};

// Синтетические ответы для --demo: стабильные картинки для скриншотов и показа без сети.
class DemoProber : public ProbeBackend {
    Q_OBJECT
public:
    explicit DemoProber(QObject *parent = nullptr);
    void start(const ProbeRequest &req) override;

    struct Sim {
        bool ok = true;
        double rtt = 1.0;
        int code = 0;
        QString detail;
        qint64 certExpiry = 0;
    };
    // tSec — секунды «жизни» сценария; noise — случайное 0..1
    static Sim simulate(const QString &scenario, double tSec, double noise, double noise2);

private:
    QElapsedTimer m_clock;
};

} // namespace zr
