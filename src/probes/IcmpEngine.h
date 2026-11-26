// ICMP: три пути с автоматическим выбором.
//   1) ICMP-сокет в процессе (датаграммный — если ping_group_range разрешает; сырой — если есть права);
//   2) помощник zarnitsa-icmp с cap_net_raw;
//   3) системный ping — без прав, медленнее, с ограничением параллельности.
#pragma once

#include "Probe.h"

#include <QElapsedTimer>
#include <QHash>
#include <QPointer>
#include <QProcess>
#include <QQueue>
#include <QSet>
#include <QSocketNotifier>
#include <QTimer>

namespace zr {

// --- 1. сокет в процессе -------------------------------------------------
class IcmpSocketBackend : public ProbeBackend {
    Q_OBJECT
public:
    enum class Type { None, Dgram, Raw };
    explicit IcmpSocketBackend(QObject *parent = nullptr);
    ~IcmpSocketBackend() override;

    bool open(bool allowRaw, QString *error);
    void close();
    Type type() const { return m_type; }
    void start(const ProbeRequest &req) override;
    void cancel(quint64 token) override;

private:
    struct Pend {
        ProbeRequest req;
        qint64 sentUs = 0;
        qint64 deadlineUs = 0;
        quint32 destination = 0;   // IPv4 в сетевом порядке байт
    };
    void onReadable();
    void drainErrQueue();
    void expire();
    void finish(quint16 seq, ProbeOutcome o, double rttMs, int ttl, const QString &from, const QString &err = QString());
    qint64 nowUs() const { return m_clock.nsecsElapsed() / 1000; }

    int m_fd = -1;
    Type m_type = Type::None;
    QSocketNotifier *m_notifier = nullptr;
    QTimer m_timer;
    QElapsedTimer m_clock;
    quint16 m_ident = 0;
    quint16 m_nextSeq = 1;
    int m_curTtl = -1;
    int m_curPmtu = -1;
    QHash<quint16, Pend> m_pending;
    QHash<quint64, quint16> m_byToken;
};

// --- 2. помощник ------------------------------------------------------------
class IcmpHelperBackend : public ProbeBackend {
    Q_OBJECT
public:
    enum class State { Stopped, Starting, Ready, Failed };
    explicit IcmpHelperBackend(QObject *parent = nullptr);
    ~IcmpHelperBackend() override;

    static QString findHelper(const QString &configured);
    void startHelper(const QString &path);
    void stopHelper();
    State state() const { return m_state; }
    QString failure() const { return m_failure; }
    QString path() const { return m_path; }
    void start(const ProbeRequest &req) override;
    void cancel(quint64 token) override;

signals:
    void ready();
    void failed(const QString &reason);

private:
    void onOutput();
    void onExit(int code, QProcess::ExitStatus st);
    void handleLine(const QByteArray &line);
    void send(const ProbeRequest &req);
    void failAll(const QString &why);

    QProcess *m_proc = nullptr;
    State m_state = State::Stopped;
    QString m_path;
    QString m_failure;
    QByteArray m_buf;
    QHash<quint64, ProbeRequest> m_pending;
    QQueue<ProbeRequest> m_waiting;
    QSet<quint64> m_cancelled;
    int m_restarts = 0;
    QElapsedTimer m_restartWindow;
    QTimer m_restartTimer;
};

// --- 3. системный ping --------------------------------------------------
class SystemPingBackend : public ProbeBackend {
    Q_OBJECT
public:
    explicit SystemPingBackend(QObject *parent = nullptr);
    ~SystemPingBackend() override;

    static QString findPing();
    bool available() const { return !m_ping.isEmpty(); }
    QString program() const { return m_ping; }
    void setMaxParallel(int n) { m_maxParallel = qMax(1, n); pump(); }
    int queued() const { return m_queue.size(); }
    void start(const ProbeRequest &req) override;
    void cancel(quint64 token) override;

private:
    struct Run {
        ProbeRequest req;
        QProcess *proc = nullptr;
        QElapsedTimer clock;
        QTimer *killer = nullptr;
    };
    void pump();
    void launch(const ProbeRequest &req);
    void onDone(quint64 token);

    QString m_ping;
    int m_maxParallel = 64;
    QQueue<ProbeRequest> m_queue;
    QHash<quint64, Run> m_running;
};

// --- выбор пути ---------------------------------------------------------
class IcmpEngine : public ProbeBackend {
    Q_OBJECT
public:
    enum class Mode { None, Socket, RawSocket, Helper, SystemPing };
    explicit IcmpEngine(QObject *parent = nullptr);

    // mode: auto | socket | helper | ping
    void configure(const QString &mode, const QString &helperPath, int pingParallel);
    Mode mode() const { return m_mode; }
    QString modeTitle() const;
    QString modeHint() const;
    bool isFast() const { return m_mode == Mode::Socket || m_mode == Mode::RawSocket || m_mode == Mode::Helper; }
    QString helperPath() const { return m_helper->path(); }
    QString lastFailure() const { return m_lastFailure; }

    void start(const ProbeRequest &req) override;
    void cancel(quint64 token) override;

signals:
    void modeChanged();

private:
    void setMode(Mode m);
    void fallbackFromHelper(const QString &why);
    ProbeBackend *active() const;

    IcmpSocketBackend *m_socket;
    IcmpHelperBackend *m_helper;
    SystemPingBackend *m_ping;
    Mode m_mode = Mode::None;
    QString m_requested = QStringLiteral("auto");
    QString m_lastFailure;
    QHash<quint64, ProbeBackend *> m_routed;
};

} // namespace zr
