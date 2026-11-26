#include "IcmpEngine.h"

#include "PingOutput.h"
#include "icmp/icmp_packet.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHostAddress>
#include <QProcessEnvironment>
#include <QRandomGenerator>
#include <QStandardPaths>

#include <arpa/inet.h>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <linux/errqueue.h>

namespace zr {

namespace {

constexpr int kIcmpFilterOpt = 1;   // ICMP_FILTER из <linux/icmp.h>
struct IcmpFilter { quint32 data; };
constexpr unsigned kEchoReply = 0, kDestUnreach = 3, kTimeExceeded = 11;

QString unreachText(int code)
{
    switch (code) {
    case 0: return QStringLiteral("Сеть недоступна");
    case 1: return QStringLiteral("Узел недоступен");
    case 2: return QStringLiteral("Протокол недоступен");
    case 3: return QStringLiteral("Порт недоступен");
    case 4: return QStringLiteral("Нужна фрагментация, стоит DF");
    case 9: case 10: case 13: return QStringLiteral("Запрещено фильтром");
    default: return QStringLiteral("Узел недоступен (код %1)").arg(code);
    }
}

void errnoOutcome(int e, ProbeOutcome &o, QString &text)
{
    switch (e) {
    case EMSGSIZE: o = ProbeOutcome::Error; text = QStringLiteral("Пакет больше MTU, стоит DF"); break;
    case ENETUNREACH: o = ProbeOutcome::Unreachable; text = QStringLiteral("Нет маршрута к сети"); break;
    case EHOSTUNREACH: o = ProbeOutcome::Unreachable; text = QStringLiteral("Нет маршрута к узлу"); break;
    case EPERM: case EACCES: o = ProbeOutcome::Error; text = QStringLiteral("Отправка запрещена фильтром"); break;
    case ENOBUFS: case EAGAIN: o = ProbeOutcome::Error; text = QStringLiteral("Переполнен буфер отправки"); break;
    default: o = ProbeOutcome::Error; text = QString::fromLocal8Bit(std::strerror(e)); break;
    }
}

void helperErrOutcome(const QByteArray &name, ProbeOutcome &o, QString &text)
{
    if (name == "EAGAIN") {
        o = ProbeOutcome::Error;
        text = QStringLiteral("Помощник ICMP: лимит частоты/объёма отправки (EAGAIN)");
        return;
    }
    if (name == "ENOBUFS") {
        o = ProbeOutcome::Error;
        text = QStringLiteral("Помощник ICMP: лимит запросов/адресов или буфер исчерпан (ENOBUFS)");
        return;
    }
    static const QHash<QByteArray, int> map = {
        {"EMSGSIZE", EMSGSIZE}, {"ENETUNREACH", ENETUNREACH}, {"EHOSTUNREACH", EHOSTUNREACH},
        {"EPERM", EPERM}, {"EACCES", EACCES}, {"ENOBUFS", ENOBUFS}, {"EAGAIN", EAGAIN},
        {"EINVAL", EINVAL},
    };
    const int e = map.value(name, EIO);
    if (e == EINVAL) { o = ProbeOutcome::Error; text = QStringLiteral("Неверные параметры запроса"); return; }
    errnoOutcome(e, o, text);
}

} // namespace

// ===========================================================================
// 1. Сокет в процессе
// ===========================================================================

IcmpSocketBackend::IcmpSocketBackend(QObject *parent)
    : ProbeBackend(parent)
{
    m_clock.start();
    m_timer.setInterval(20);
    connect(&m_timer, &QTimer::timeout, this, [this]() {
        if (m_type == Type::Dgram) drainErrQueue();
        expire();
    });
}

IcmpSocketBackend::~IcmpSocketBackend()
{
    close();
}

void IcmpSocketBackend::close()
{
    m_timer.stop();
    delete m_notifier;
    m_notifier = nullptr;
    if (m_fd >= 0)
        ::close(m_fd);
    m_fd = -1;
    m_type = Type::None;
    const QList<quint16> seqs = m_pending.keys();
    for (quint16 s : seqs)
        finish(s, ProbeOutcome::Error, -1, -1, QString(), QStringLiteral("ICMP-сокет закрыт"));
}

bool IcmpSocketBackend::open(bool allowRaw, QString *error)
{
    close();
    int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_ICMP);
    const int dgramErr = errno;
    Type type = Type::Dgram;
    if (fd < 0 && allowRaw) {
        fd = ::socket(AF_INET, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_ICMP);
        type = Type::Raw;
    }
    if (fd < 0) {
        if (error)
            *error = QStringLiteral("ICMP-сокет: %1").arg(QString::fromLocal8Bit(std::strerror(dgramErr)));
        return false;
    }
    int on = 1;
    if (type == Type::Dgram) {
        ::setsockopt(fd, SOL_IP, IP_RECVERR, &on, sizeof on);
        ::setsockopt(fd, SOL_IP, IP_RECVTTL, &on, sizeof on);
    } else {
        IcmpFilter filt{};
        filt.data = ~((1U << kEchoReply) | (1U << kDestUnreach) | (1U << kTimeExceeded));
        ::setsockopt(fd, SOL_RAW, kIcmpFilterOpt, &filt, sizeof filt);
    }
    int rcvbuf = 1 << 20;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);

    m_fd = fd;
    m_type = type;
    m_ident = quint16(QRandomGenerator::global()->bounded(1, 0xFFFF));
    m_nextSeq = quint16(QRandomGenerator::global()->bounded(1, 0xFFFF));
    m_curTtl = -1;
    m_curPmtu = -1;
    m_notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &IcmpSocketBackend::onReadable);
    m_timer.start();
    return true;
}

void IcmpSocketBackend::start(const ProbeRequest &req)
{
    if (m_fd < 0) {
        finishLater(makeResult(req, ProbeOutcome::Error, QStringLiteral("ICMP-сокет закрыт")));
        return;
    }
    QHostAddress addr;
    if (!addr.setAddress(req.host) || addr.protocol() != QAbstractSocket::IPv4Protocol) {
        finishLater(makeResult(req, ProbeOutcome::Error, QStringLiteral("Нужен IPv4-адрес")));
        return;
    }
    if (m_pending.size() >= 60000) {
        finishLater(makeResult(req, ProbeOutcome::Error, QStringLiteral("Слишком много запросов в пути")));
        return;
    }
    quint16 seq = m_nextSeq++;
    while (m_pending.contains(seq))
        seq = m_nextSeq++;

    static thread_local QByteArray pkt(65600, '\0');
    const size_t len = zicmp::buildEcho(reinterpret_cast<uint8_t *>(pkt.data()), size_t(pkt.size()), m_ident, seq,
                                        size_t(qBound(0, req.size, 65000)));
    const int ttl = qBound(1, req.ttl, 255);
    if (ttl != m_curTtl) {
        ::setsockopt(m_fd, IPPROTO_IP, IP_TTL, &ttl, sizeof ttl);
        m_curTtl = ttl;
    }
    const int pmtu = req.df ? IP_PMTUDISC_DO : IP_PMTUDISC_DONT;
    if (pmtu != m_curPmtu) {
        ::setsockopt(m_fd, IPPROTO_IP, IP_MTU_DISCOVER, &pmtu, sizeof pmtu);
        m_curPmtu = pmtu;
    }
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(addr.toIPv4Address());
    const qint64 t = nowUs();
    const ssize_t w = ::sendto(m_fd, pkt.constData(), len, 0, reinterpret_cast<sockaddr *>(&sa), sizeof sa);
    if (w < 0) {
        ProbeOutcome o;
        QString text;
        errnoOutcome(errno, o, text);
        finishLater(makeResult(req, o, text));
        return;
    }
    Pend p;
    p.req = req;
    p.sentUs = t;
    p.deadlineUs = t + qint64(qMax(10, req.timeoutMs)) * 1000;
    p.destination = sa.sin_addr.s_addr;
    m_pending.insert(seq, p);
    m_byToken.insert(req.token, seq);
}

void IcmpSocketBackend::cancel(quint64 token)
{
    const auto it = m_byToken.find(token);
    if (it == m_byToken.end())
        return;
    m_pending.remove(it.value());
    m_byToken.erase(it);
}

void IcmpSocketBackend::finish(quint16 seq, ProbeOutcome o, double rttMs, int ttl, const QString &from, const QString &err)
{
    auto it = m_pending.find(seq);
    if (it == m_pending.end())
        return;
    ProbeResult r = makeResult(it->req, o, err);
    r.rttMs = rttMs;
    r.ttl = ttl;
    r.from = from;
    m_byToken.remove(it->req.token);
    m_pending.erase(it);
    emit finished(r);
}

void IcmpSocketBackend::onReadable()
{
    static thread_local QByteArray buf(65600, '\0');
    for (int i = 0; i < 4096 && m_fd >= 0; ++i) {
        sockaddr_in from{};
        iovec iov{buf.data(), size_t(buf.size())};
        char ctrl[256];
        msghdr msg{};
        msg.msg_name = &from;
        msg.msg_namelen = sizeof from;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = ctrl;
        msg.msg_controllen = sizeof ctrl;
        const ssize_t n = ::recvmsg(m_fd, &msg, MSG_DONTWAIT);
        if (n < 0)
            break;
        const qint64 t = nowUs();
        if (msg.msg_namelen < sizeof from || from.sin_family != AF_INET)
            continue;
        zicmp::Parsed p;
        const auto *data = reinterpret_cast<const uint8_t *>(buf.constData());
        if (m_type == Type::Raw) {
            if (!zicmp::parseRaw(data, size_t(n), p) || p.id != m_ident || p.from != from.sin_addr.s_addr)
                continue;
        } else {
            if (!zicmp::parseDgram(data, size_t(n), p) || p.kind != zicmp::Kind::Reply)
                continue;
            p.from = from.sin_addr.s_addr;
            for (cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
                if (c->cmsg_level == SOL_IP && c->cmsg_type == IP_TTL) {
                    int v = 0;
                    std::memcpy(&v, CMSG_DATA(c), sizeof v);
                    p.ttl = v;
                }
            }
        }
        const auto it = m_pending.constFind(p.seq);
        if (it == m_pending.constEnd() || !zicmp::matchesDestination(p, it->destination))
            continue;
        const double rtt = double(t - it->sentUs) / 1000.0;
        const QString fromText = QHostAddress(ntohl(p.from)).toString();
        switch (p.kind) {
        case zicmp::Kind::Reply:
            finish(p.seq, ProbeOutcome::Ok, rtt, p.ttl, fromText);
            break;
        case zicmp::Kind::TimeExceeded:
            finish(p.seq, ProbeOutcome::TtlExceeded, rtt, -1, fromText, QStringLiteral("Истёк TTL"));
            break;
        case zicmp::Kind::Unreachable:
            finish(p.seq, ProbeOutcome::Unreachable, rtt, -1, fromText, unreachText(p.code));
            break;
        default:
            break;
        }
    }
    if (m_type == Type::Dgram)
        drainErrQueue();
}

void IcmpSocketBackend::drainErrQueue()
{
    static thread_local QByteArray buf(65600, '\0');
    for (int i = 0; i < 4096 && m_fd >= 0; ++i) {
        sockaddr_in to{};
        iovec iov{buf.data(), size_t(buf.size())};
        char ctrl[512];
        msghdr msg{};
        msg.msg_name = &to;
        msg.msg_namelen = sizeof to;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = ctrl;
        msg.msg_controllen = sizeof ctrl;
        const ssize_t n = ::recvmsg(m_fd, &msg, MSG_ERRQUEUE | MSG_DONTWAIT);
        if (n < 0)
            break;
        const qint64 t = nowUs();
        uint16_t id = 0, seq = 0;
        if (!zicmp::parseOriginalEcho(reinterpret_cast<const uint8_t *>(buf.constData()), size_t(n), id, seq))
            continue;
        const auto it = m_pending.constFind(seq);
        // В MSG_ERRQUEUE ядро отдаёт исходное назначение, а не адрес маршрутизатора.
        if (it == m_pending.constEnd() || msg.msg_namelen < sizeof to || to.sin_family != AF_INET
            || to.sin_addr.s_addr != it->destination)
            continue;
        const double rtt = double(t - it->sentUs) / 1000.0;
        for (cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level != SOL_IP || c->cmsg_type != IP_RECVERR
                || c->cmsg_len < CMSG_LEN(sizeof(sock_extended_err)))
                continue;
            sock_extended_err ee{};
            std::memcpy(&ee, CMSG_DATA(c), sizeof ee);
            QString fromText;
            if (ee.ee_origin == SO_EE_ORIGIN_ICMP) {
                if (ee.ee_type != kTimeExceeded && ee.ee_type != kDestUnreach)
                    continue;
                if (c->cmsg_len >= CMSG_LEN(sizeof(sock_extended_err) + sizeof(sockaddr_in))) {
                    sockaddr_in off{};
                    std::memcpy(&off, reinterpret_cast<const char *>(CMSG_DATA(c)) + sizeof(sock_extended_err), sizeof off);
                    if (off.sin_family == AF_INET)
                        fromText = QHostAddress(ntohl(off.sin_addr.s_addr)).toString();
                }
                if (ee.ee_type == kTimeExceeded)
                    finish(seq, ProbeOutcome::TtlExceeded, rtt, -1, fromText, QStringLiteral("Истёк TTL"));
                else
                    finish(seq, ProbeOutcome::Unreachable, rtt, -1, fromText, unreachText(ee.ee_code));
            } else {
                ProbeOutcome o;
                QString text;
                errnoOutcome(int(ee.ee_errno), o, text);
                finish(seq, o, rtt, -1, QString(), text);
            }
            break;
        }
    }
}

void IcmpSocketBackend::expire()
{
    if (m_pending.isEmpty())
        return;
    const qint64 now = nowUs();
    QVector<quint16> due;
    for (auto it = m_pending.constBegin(); it != m_pending.constEnd(); ++it)
        if (it->deadlineUs <= now)
            due.append(it.key());
    for (quint16 s : due)
        finish(s, ProbeOutcome::Timeout, -1, -1, QString());
}

// ===========================================================================
// 2. Помощник
// ===========================================================================

IcmpHelperBackend::IcmpHelperBackend(QObject *parent)
    : ProbeBackend(parent)
{
    m_restartWindow.start();
    m_restartTimer.setSingleShot(true);
    connect(&m_restartTimer, &QTimer::timeout, this, [this]() {
        if (m_state == State::Starting && !m_proc)
            startHelper(m_path);
    });
}

IcmpHelperBackend::~IcmpHelperBackend()
{
    stopHelper();
}

QString IcmpHelperBackend::findHelper(const QString &configured)
{
    QStringList candidates;
    if (!configured.trimmed().isEmpty())
        candidates << configured.trimmed();
    const QString appDir = QCoreApplication::applicationDirPath();
    candidates << appDir + QStringLiteral("/zarnitsa-icmp")
               << appDir + QStringLiteral("/../libexec/zarnitsa/zarnitsa-icmp")
               << QStringLiteral("/opt/zarnitsa/bin/zarnitsa-icmp")
               << QStringLiteral("/usr/lib/zarnitsa/zarnitsa-icmp");
    for (const QString &c : candidates) {
        const QFileInfo fi(c);
        if (fi.exists() && fi.isFile() && fi.isExecutable())
            return fi.canonicalFilePath();
    }
    return QString();
}

void IcmpHelperBackend::startHelper(const QString &path)
{
    stopHelper();
    m_path = path;
    m_failure.clear();
    m_buf.clear();
    m_state = State::Starting;
    m_proc = new QProcess(this);
    m_proc->setProcessChannelMode(QProcess::SeparateChannels);
    m_proc->setStandardErrorFile(QProcess::nullDevice());
    connect(m_proc, &QProcess::readyReadStandardOutput, this, &IcmpHelperBackend::onOutput);
    connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &IcmpHelperBackend::onExit);
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            m_state = State::Failed;
            m_failure = QStringLiteral("помощник не запустился: %1").arg(m_proc ? m_proc->errorString() : QString());
            failAll(m_failure);
            emit failed(m_failure);
        }
    });
    QPointer<QProcess> guard(m_proc);
    QTimer::singleShot(3000, this, [this, guard]() {
        if (guard && guard == m_proc && m_state == State::Starting) {
            m_state = State::Failed;
            m_failure = QStringLiteral("помощник не ответил");
            m_proc->kill();
            failAll(m_failure);
            emit failed(m_failure);
        }
    });
    m_proc->start(path, QStringList(), QIODevice::ReadWrite);
}

void IcmpHelperBackend::stopHelper()
{
    // Явная остановка отменяет и ещё не состоявшийся перезапуск после падения.
    m_restartTimer.stop();
    if (m_proc) {
        disconnect(m_proc, nullptr, this, nullptr);
        if (m_proc->state() != QProcess::NotRunning) {
            m_proc->write("Q\n");
            m_proc->closeWriteChannel();
            if (!m_proc->waitForFinished(300))
                m_proc->kill();
            m_proc->waitForFinished(300);
        }
        m_proc->deleteLater();
        m_proc = nullptr;
    }
    m_state = State::Stopped;
    failAll(QStringLiteral("помощник остановлен"));
}

void IcmpHelperBackend::failAll(const QString &why)
{
    QVector<ProbeRequest> reqs;
    for (auto it = m_pending.constBegin(); it != m_pending.constEnd(); ++it)
        reqs.append(it.value());
    while (!m_waiting.isEmpty())
        reqs.append(m_waiting.dequeue());
    m_pending.clear();
    for (const ProbeRequest &r : reqs)
        finishLater(makeResult(r, ProbeOutcome::Error, why));
}

void IcmpHelperBackend::start(const ProbeRequest &req)
{
    QHostAddress addr;
    if (!addr.setAddress(req.host) || addr.protocol() != QAbstractSocket::IPv4Protocol) {
        finishLater(makeResult(req, ProbeOutcome::Error, QStringLiteral("Нужен IPv4-адрес")));
        return;
    }
    switch (m_state) {
    case State::Ready: send(req); break;
    case State::Starting: m_waiting.enqueue(req); break;
    default:
        finishLater(makeResult(req, ProbeOutcome::Error,
                               m_failure.isEmpty() ? QStringLiteral("помощник не запущен") : m_failure));
        break;
    }
}

void IcmpHelperBackend::send(const ProbeRequest &req)
{
    m_pending.insert(req.token, req);
    const QByteArray line = QByteArrayLiteral("E ") + QByteArray::number(req.token) + ' ' + req.host.toLatin1() + ' '
        + QByteArray::number(qBound(1, req.ttl, 255)) + ' ' + QByteArray::number(qBound(0, req.size, 65000)) + ' '
        + QByteArray::number(qBound(10, req.timeoutMs, 60000)) + ' ' + (req.df ? "1" : "0") + '\n';
    m_proc->write(line);
}

void IcmpHelperBackend::cancel(quint64 token)
{
    m_pending.remove(token);
    for (int i = 0; i < m_waiting.size(); ++i) {
        if (m_waiting.at(i).token == token) {
            m_waiting.removeAt(i);
            break;
        }
    }
}

void IcmpHelperBackend::onOutput()
{
    if (!m_proc)
        return;
    m_buf += m_proc->readAllStandardOutput();
    int pos;
    while ((pos = m_buf.indexOf('\n')) >= 0) {
        const QByteArray line = m_buf.left(pos).trimmed();
        m_buf.remove(0, pos + 1);
        if (!line.isEmpty())
            handleLine(line);
    }
    if (m_buf.size() > (1 << 20))
        m_buf.clear();
}

void IcmpHelperBackend::handleLine(const QByteArray &line)
{
    if (line.startsWith("READY")) {
        m_state = State::Ready;
        while (!m_waiting.isEmpty())
            send(m_waiting.dequeue());
        emit ready();
        return;
    }
    if (line.startsWith("ERROR")) {
        m_state = State::Failed;
        m_failure = QStringLiteral("помощник: %1").arg(QString::fromLocal8Bit(line.mid(6)));
        failAll(m_failure);
        emit failed(m_failure);
        return;
    }
    if (!line.startsWith("R "))
        return;
    const QList<QByteArray> f = line.split(' ');
    if (f.size() < 3)
        return;
    bool okTok = false;
    const quint64 token = f.at(1).toULongLong(&okTok);
    if (!okTok)
        return;
    auto it = m_pending.find(token);
    if (it == m_pending.end())
        return;
    const ProbeRequest req = it.value();
    m_pending.erase(it);
    const QByteArray kind = f.at(2);
    ProbeResult r;
    if (kind == "ok" && f.size() >= 6) {
        r = makeResult(req, ProbeOutcome::Ok);
        r.rttMs = f.at(3).toDouble() / 1000.0;
        r.ttl = f.at(4).toInt();
        r.from = QString::fromLatin1(f.at(5));
    } else if (kind == "ttlx" && f.size() >= 5) {
        r = makeResult(req, ProbeOutcome::TtlExceeded, QStringLiteral("Истёк TTL"));
        r.rttMs = f.at(3).toDouble() / 1000.0;
        r.from = QString::fromLatin1(f.at(4));
    } else if (kind == "unreach" && f.size() >= 6) {
        r = makeResult(req, ProbeOutcome::Unreachable, unreachText(f.at(3).toInt()));
        r.rttMs = f.at(4).toDouble() / 1000.0;
        r.from = QString::fromLatin1(f.at(5));
    } else if (kind == "timeout") {
        r = makeResult(req, ProbeOutcome::Timeout);
    } else if (kind == "err" && f.size() >= 4) {
        ProbeOutcome o;
        QString text;
        helperErrOutcome(f.at(3), o, text);
        r = makeResult(req, o, text);
    } else {
        r = makeResult(req, ProbeOutcome::Error, QStringLiteral("непонятный ответ помощника"));
    }
    emit finished(r);
}

void IcmpHelperBackend::onExit(int code, QProcess::ExitStatus st)
{
    Q_UNUSED(st);
    const bool wasReady = m_state == State::Ready;
    if (m_proc) {
        m_proc->deleteLater();
        m_proc = nullptr;
    }
    if (m_state == State::Starting) {
        m_state = State::Failed;
        if (m_failure.isEmpty())
            m_failure = QStringLiteral("помощник завершился с кодом %1").arg(code);
        failAll(m_failure);
        emit failed(m_failure);
        return;
    }
    if (!wasReady)
        return;
    failAll(QStringLiteral("помощник остановился"));
    if (m_restartWindow.elapsed() > 60000) {
        m_restartWindow.restart();
        m_restarts = 0;
    }
    if (++m_restarts <= 5) {
        m_state = State::Starting;
        m_restartTimer.start(1000);
    } else {
        m_state = State::Failed;
        m_failure = QStringLiteral("помощник падает раз за разом");
        emit failed(m_failure);
    }
}

// ===========================================================================
// 3. Системный ping
// ===========================================================================

SystemPingBackend::SystemPingBackend(QObject *parent)
    : ProbeBackend(parent)
    , m_ping(findPing())
{
}

SystemPingBackend::~SystemPingBackend()
{
    for (auto it = m_running.begin(); it != m_running.end(); ++it) {
        if (it->proc) {
            disconnect(it->proc, nullptr, this, nullptr);
            it->proc->kill();
            it->proc->waitForFinished(200);
        }
    }
}

QString SystemPingBackend::findPing()
{
    for (const QString &p : {QStringLiteral("/usr/bin/ping"), QStringLiteral("/bin/ping")}) {
        const QFileInfo fi(p);
        if (fi.exists() && fi.isExecutable())
            return p;
    }
    return QStandardPaths::findExecutable(QStringLiteral("ping"));
}

void SystemPingBackend::start(const ProbeRequest &req)
{
    if (m_ping.isEmpty()) {
        finishLater(makeResult(req, ProbeOutcome::Error, QStringLiteral("Нет программы ping")));
        return;
    }
    m_queue.enqueue(req);
    pump();
}

void SystemPingBackend::pump()
{
    while (m_running.size() < m_maxParallel && !m_queue.isEmpty())
        launch(m_queue.dequeue());
}

void SystemPingBackend::launch(const ProbeRequest &req)
{
    Run run;
    run.req = req;
    run.proc = new QProcess(this);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    env.insert(QStringLiteral("LANG"), QStringLiteral("C"));
    run.proc->setProcessEnvironment(env);
    run.proc->setProcessChannelMode(QProcess::MergedChannels);

    const int waitSec = qMax(1, int(std::ceil(req.timeoutMs / 1000.0)));
    QStringList args{QStringLiteral("-n"), QStringLiteral("-c"), QStringLiteral("1"),
                     QStringLiteral("-W"), QString::number(waitSec),
                     QStringLiteral("-s"), QString::number(qBound(16, req.size, 65000)),
                     QStringLiteral("-t"), QString::number(qBound(1, req.ttl, 255))};
    if (req.df)
        args << QStringLiteral("-M") << QStringLiteral("do");
    if (req.host.contains(QLatin1Char(':')))
        args << QStringLiteral("-6");
    args << req.host;

    const quint64 token = req.token;
    connect(run.proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, token](int, QProcess::ExitStatus) { onDone(token); });
    connect(run.proc, &QProcess::errorOccurred, this, [this, token](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) onDone(token);
    });
    run.killer = new QTimer(run.proc);
    run.killer->setSingleShot(true);
    QProcess *proc = run.proc;
    connect(run.killer, &QTimer::timeout, proc, [proc]() { proc->kill(); });
    run.killer->start(req.timeoutMs + 2500);
    run.clock.start();
    m_running.insert(token, run);
    proc->start(m_ping, args, QIODevice::ReadOnly);
}

void SystemPingBackend::onDone(quint64 token)
{
    auto it = m_running.find(token);
    if (it == m_running.end())
        return;
    Run run = it.value();
    m_running.erase(it);
    const double elapsed = double(run.clock.nsecsElapsed()) / 1e6;
    const bool failedToStart = run.proc->error() == QProcess::FailedToStart;
    const QString out = failedToStart ? QString() : QString::fromLocal8Bit(run.proc->readAll());
    disconnect(run.proc, nullptr, this, nullptr);
    run.proc->deleteLater();

    ProbeResult r;
    if (failedToStart) {
        r = makeResult(run.req, ProbeOutcome::Error, QStringLiteral("ping не запустился"));
    } else {
        const PingParse pp = parsePingOutput(out);
        switch (pp.kind) {
        case PingParse::Reply:
            r = makeResult(run.req, ProbeOutcome::Ok);
            r.rttMs = pp.rttMs >= 0 ? pp.rttMs : elapsed;
            r.ttl = pp.ttl;
            r.from = pp.from;
            break;
        case PingParse::TtlExceeded:
            r = makeResult(run.req, ProbeOutcome::TtlExceeded, QStringLiteral("Истёк TTL"));
            r.rttMs = elapsed;   // ping не печатает время для time exceeded — меряем сами, с запасом на запуск
            r.from = pp.from;
            break;
        case PingParse::Unreachable:
            r = makeResult(run.req, ProbeOutcome::Unreachable, QStringLiteral("Узел недоступен"));
            r.from = pp.from;
            break;
        case PingParse::Error:
            r = makeResult(run.req, ProbeOutcome::Error, QStringLiteral("ping: %1").arg(pp.error));
            break;
        case PingParse::NoReply:
            r = makeResult(run.req, ProbeOutcome::Timeout);
            break;
        }
    }
    emit finished(r);
    pump();
}

void SystemPingBackend::cancel(quint64 token)
{
    for (int i = 0; i < m_queue.size(); ++i) {
        if (m_queue.at(i).token == token) {
            m_queue.removeAt(i);
            return;
        }
    }
    auto it = m_running.find(token);
    if (it == m_running.end())
        return;
    QProcess *p = it->proc;
    m_running.erase(it);
    disconnect(p, nullptr, this, nullptr);
    p->kill();
    p->deleteLater();
    pump();
}

// ===========================================================================
// Выбор пути
// ===========================================================================

IcmpEngine::IcmpEngine(QObject *parent)
    : ProbeBackend(parent)
    , m_socket(new IcmpSocketBackend(this))
    , m_helper(new IcmpHelperBackend(this))
    , m_ping(new SystemPingBackend(this))
{
    for (ProbeBackend *b : {static_cast<ProbeBackend *>(m_socket), static_cast<ProbeBackend *>(m_helper),
                            static_cast<ProbeBackend *>(m_ping)}) {
        connect(b, &ProbeBackend::finished, this, [this](const ProbeResult &r) {
            m_routed.remove(r.token);
            emit finished(r);
        });
    }
    connect(m_helper, &IcmpHelperBackend::ready, this, &IcmpEngine::modeChanged);
    connect(m_helper, &IcmpHelperBackend::failed, this, &IcmpEngine::fallbackFromHelper);
}

void IcmpEngine::configure(const QString &modeIn, const QString &helperPath, int pingParallel)
{
    m_requested = modeIn.isEmpty() ? QStringLiteral("auto") : modeIn;
    m_ping->setMaxParallel(pingParallel);
    m_socket->close();
    m_helper->stopHelper();
    m_lastFailure.clear();

    auto trySocket = [this]() {
        QString err;
        if (m_socket->open(true, &err)) {
            setMode(m_socket->type() == IcmpSocketBackend::Type::Raw ? Mode::RawSocket : Mode::Socket);
            return true;
        }
        m_lastFailure = err;
        return false;
    };
    auto tryHelper = [this, helperPath]() {
        const QString p = IcmpHelperBackend::findHelper(helperPath);
        if (p.isEmpty()) {
            if (m_lastFailure.isEmpty())
                m_lastFailure = QStringLiteral("помощник zarnitsa-icmp не найден");
            return false;
        }
        setMode(Mode::Helper);
        m_helper->startHelper(p);
        return true;
    };
    auto tryPing = [this]() {
        if (!m_ping->available())
            return false;
        setMode(Mode::SystemPing);
        return true;
    };

    bool ok = false;
    if (m_requested == QLatin1String("socket"))
        ok = trySocket();
    else if (m_requested == QLatin1String("helper"))
        ok = tryHelper();
    else if (m_requested == QLatin1String("ping"))
        ok = tryPing();
    else
        ok = trySocket() || tryHelper() || tryPing();
    if (!ok)
        setMode(Mode::None);
}

void IcmpEngine::fallbackFromHelper(const QString &why)
{
    m_lastFailure = why;
    if (m_mode != Mode::Helper)
        return;
    if (m_requested == QLatin1String("auto") && m_ping->available())
        setMode(Mode::SystemPing);
    else
        emit modeChanged();
}

void IcmpEngine::setMode(Mode m)
{
    m_mode = m;
    emit modeChanged();
}

ProbeBackend *IcmpEngine::active() const
{
    switch (m_mode) {
    case Mode::Socket:
    case Mode::RawSocket: return m_socket;
    case Mode::Helper: return m_helper;
    case Mode::SystemPing: return m_ping;
    case Mode::None: break;
    }
    return nullptr;
}

void IcmpEngine::start(const ProbeRequest &req)
{
    ProbeBackend *b = nullptr;
    if (req.host.contains(QLatin1Char(':')))
        b = m_ping->available() ? m_ping : nullptr;   // IPv6 — только через системный ping
    else
        b = active();
    if (!b) {
        finishLater(makeResult(req, ProbeOutcome::Error, QStringLiteral("ICMP недоступен")));
        return;
    }
    m_routed.insert(req.token, b);
    b->start(req);
}

void IcmpEngine::cancel(quint64 token)
{
    if (ProbeBackend *b = m_routed.take(token))
        b->cancel(token);
}

QString IcmpEngine::modeTitle() const
{
    switch (m_mode) {
    case Mode::Socket: return QStringLiteral("ICMP-сокет");
    case Mode::RawSocket: return QStringLiteral("сырой сокет");
    case Mode::Helper:
        return m_helper->state() == IcmpHelperBackend::State::Ready ? QStringLiteral("помощник")
                                                                    : QStringLiteral("помощник, запуск");
    case Mode::SystemPing: return QStringLiteral("системный ping");
    case Mode::None: break;
    }
    return QStringLiteral("недоступен");
}

QString IcmpEngine::modeHint() const
{
    switch (m_mode) {
    case Mode::Socket:
        return QStringLiteral("Быстрый путь: ICMP-сокет без особых прав (ping_group_range).");
    case Mode::RawSocket:
        return QStringLiteral("Быстрый путь: сырой сокет, программа запущена с правами.");
    case Mode::Helper:
        return QStringLiteral("Быстрый путь: помощник zarnitsa-icmp с правом cap_net_raw (%1).").arg(m_helper->path());
    case Mode::SystemPing:
        return QStringLiteral("Медленный путь: каждый замер запускает системный ping. Для сотен адресов "
                              "поставьте помощника: sudo ./install.sh%1")
            .arg(m_lastFailure.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(m_lastFailure));
    case Mode::None:
        break;
    }
    return QStringLiteral("ICMP недоступен: нет прав на сокет и нет программы ping. TCP, HTTP и SIP работают.");
}

} // namespace zr
