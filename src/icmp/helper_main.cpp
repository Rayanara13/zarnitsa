// zarnitsa-icmp — помощник с правом cap_net_raw: открывает сырой ICMP-сокет, сразу
// сбрасывает все права и дальше только шлёт эхо-запросы по командам окна.
//
// Протокол — строки ASCII через stdin/stdout:
//   запрос:  E <метка> <IPv4> <ttl> <размер> <таймаут_мс> <df 0|1>
//   ответы:  R <метка> ok <rtt_мкс> <ttl> <IPv4>
//            R <метка> ttlx <rtt_мкс> <IPv4>
//            R <метка> unreach <код> <rtt_мкс> <IPv4>
//            R <метка> timeout
//            R <метка> err <ИМЯ_ОШИБКИ>
//   при старте: READY 1 raw   или   ERROR <текст>
#include "icmp_packet.h"
#include "monitoring_budget.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <linux/capability.h>
#include <map>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <unordered_map>

namespace {

// Из <linux/icmp.h>; свои определения, чтобы не ловить конфликты заголовков ядра и glibc.
constexpr int kIcmpFilterOpt = 1;   // ICMP_FILTER
struct IcmpFilter { uint32_t data; };
constexpr unsigned kEchoReply = 0, kDestUnreach = 3, kTimeExceeded = 11;

int64_t monoUs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

const char *errName(int e)
{
    switch (e) {
    case EMSGSIZE: return "EMSGSIZE";
    case ENETUNREACH: return "ENETUNREACH";
    case EHOSTUNREACH: return "EHOSTUNREACH";
    case EPERM: return "EPERM";
    case EACCES: return "EACCES";
    case ENOBUFS: return "ENOBUFS";
    case EINVAL: return "EINVAL";
    case EAGAIN: return "EAGAIN";
    default: return "EIO";
    }
}

struct Pending {
    unsigned long long tag = 0;
    int64_t sentUs = 0;
    uint32_t destination = 0;   // сетевой порядок байт, для корреляции ответов и ошибок
    std::multimap<int64_t, uint16_t>::iterator deadlineIt;
};

std::string g_out;

void emitLine(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void emitLine(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0)
        g_out.append(buf, size_t(n < int(sizeof buf) ? n : int(sizeof buf) - 1));
    g_out.push_back('\n');
}

bool flushOut()
{
    size_t off = 0;
    while (off < g_out.size()) {
        const ssize_t w = write(1, g_out.data() + off, g_out.size() - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += size_t(w);
    }
    g_out.clear();
    return true;
}

const char *dropCapabilities()
{
    __user_cap_header_struct hdr{};
    hdr.version = _LINUX_CAPABILITY_VERSION_3;
    hdr.pid = 0;
    __user_cap_data_struct data[2]{};
    if (syscall(SYS_capset, &hdr, data) != 0)
        return "capset";
    if (syscall(SYS_capget, &hdr, data) != 0)
        return "capget";
    for (const auto &part : data) {
        if (part.effective || part.permitted || part.inheritable) {
            errno = EPERM;
            return "capabilities-not-cleared";
        }
    }
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        return "no_new_privs";
    return nullptr;
}

std::string addrText(uint32_t netAddr)
{
    char buf[INET_ADDRSTRLEN] = "0.0.0.0";
    in_addr a{};
    a.s_addr = netAddr;
    inet_ntop(AF_INET, &a, buf, sizeof buf);
    return buf;
}

} // namespace

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    // Поддерживается только filecap: setuid/setgid-вызов отвергаем до открытия сокета.
    if (geteuid() != getuid() || getegid() != getgid()) {
        std::printf("ERROR setuid/setgid invocation is not supported; use cap_net_raw filecap\n");
        std::fflush(stdout);
        return 3;
    }
    if (argc > 1 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("zarnitsa-icmp 1\n");
        return 0;
    }

    const int s = socket(AF_INET, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, IPPROTO_ICMP);
    const int sockErr = errno;
    // Единственный сырой сокет уже открыт; любой отказ сброса прав закрывает помощник.
    if (const char *step = dropCapabilities()) {
        const int dropErr = errno;
        if (s >= 0) close(s);
        std::printf("ERROR %s %s\n", step, std::strerror(dropErr));
        std::fflush(stdout);
        return 3;
    }
    if (s < 0) {
        std::printf("ERROR socket %s\n", std::strerror(sockErr));
        std::fflush(stdout);
        return 2;
    }

    IcmpFilter filt{};
    filt.data = ~((1U << kEchoReply) | (1U << kDestUnreach) | (1U << kTimeExceeded));
    setsockopt(s, SOL_RAW, kIcmpFilterOpt, &filt, sizeof filt);
    int rcvbuf = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);

    const uint16_t ident = uint16_t((getpid() ^ (monoUs() >> 3)) & 0xFFFF);
    std::unordered_map<uint16_t, Pending> pending;
    std::multimap<int64_t, uint16_t> deadlines;
    uint16_t nextSeq = uint16_t(monoUs() & 0xFFFF);
    int curTtl = -1;
    int curPmtu = -1;
    zicmp::MonitoringBudget budget(monoUs());

    emitLine("READY 1 raw");
    if (!flushOut())
        return 1;

    std::string inbuf;
    static uint8_t pkt[65536 + 64];
    static uint8_t rbuf[65536 + 64];
    bool stdinOpen = true;

    while (stdinOpen || !pending.empty()) {
        int timeoutMs = 1000;
        if (!deadlines.empty()) {
            const int64_t left = deadlines.begin()->first - monoUs();
            timeoutMs = left <= 0 ? 0 : int(left / 1000 + 1);
            if (timeoutMs > 1000) timeoutMs = 1000;
        }
        pollfd fds[2] = {{0, short(stdinOpen ? POLLIN : 0), 0}, {s, POLLIN, 0}};
        const int pr = poll(fds, 2, timeoutMs);
        if (pr < 0 && errno != EINTR)
            break;

        if (stdinOpen && (fds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            char tmp[8192];
            const ssize_t n = read(0, tmp, sizeof tmp);
            if (n <= 0) {
                if (n == 0 || (errno != EINTR && errno != EAGAIN)) stdinOpen = false;
            } else {
                inbuf.append(tmp, size_t(n));
                if (inbuf.size() > (1 << 20)) inbuf.clear();   // защита от мусора
            }
            size_t pos;
            while ((pos = inbuf.find('\n')) != std::string::npos) {
                std::string line = inbuf.substr(0, pos);
                inbuf.erase(0, pos + 1);
                if (line == "Q") { stdinOpen = false; pending.clear(); deadlines.clear(); break; }
                unsigned long long tag = 0;
                char ip[64] = {0};
                int ttl = 64, size = 56, timeout = 1000, df = 0;
                if (std::sscanf(line.c_str(), "E %llu %63s %d %d %d %d", &tag, ip, &ttl, &size, &timeout, &df) != 6) {
                    continue;
                }
                in_addr dst{};
                if (inet_pton(AF_INET, ip, &dst) != 1 || ttl < 1 || ttl > 255 || size < 0 || size > 65000
                    || timeout < 10 || timeout > 60000) {
                    emitLine("R %llu err EINVAL", tag);
                    continue;
                }
                const auto admitted = budget.take(dst.s_addr, size_t(size), pending.size(), monoUs());
                if (admitted != zicmp::MonitoringBudget::Result::Allowed) {
                    // Без повторов и очередей: следующий плановый замер решит, пробовать ли снова.
                    const char *name = admitted == zicmp::MonitoringBudget::Result::RateLimited ? "EAGAIN"
                        : admitted == zicmp::MonitoringBudget::Result::InvalidSize ? "EINVAL" : "ENOBUFS";
                    emitLine("R %llu err %s", tag, name);
                    continue;
                }
                uint16_t seq = nextSeq++;
                while (pending.count(seq)) seq = nextSeq++;
                const size_t len = zicmp::buildEcho(pkt, sizeof pkt, ident, seq, size_t(size));
                if (ttl != curTtl) {
                    setsockopt(s, IPPROTO_IP, IP_TTL, &ttl, sizeof ttl);
                    curTtl = ttl;
                }
                const int pmtu = df ? IP_PMTUDISC_DO : IP_PMTUDISC_DONT;
                if (pmtu != curPmtu) {
                    setsockopt(s, IPPROTO_IP, IP_MTU_DISCOVER, &pmtu, sizeof pmtu);
                    curPmtu = pmtu;
                }
                sockaddr_in sa{};
                sa.sin_family = AF_INET;
                sa.sin_addr = dst;
                const int64_t now = monoUs();
                const ssize_t w = sendto(s, pkt, len, 0, reinterpret_cast<sockaddr *>(&sa), sizeof sa);
                if (w < 0) {
                    emitLine("R %llu err %s", tag, errName(errno));
                    continue;
                }
                Pending p;
                p.tag = tag;
                p.sentUs = now;
                p.destination = dst.s_addr;
                p.deadlineIt = deadlines.emplace(now + int64_t(timeout) * 1000, seq);
                pending.emplace(seq, p);
            }
        }

        if (fds[1].revents & POLLIN) {
            for (int burst = 0; burst < 4096; ++burst) {
                sockaddr_in peer{};
                socklen_t peerLen = sizeof peer;
                const ssize_t n = recvfrom(s, rbuf, sizeof rbuf, MSG_DONTWAIT,
                                           reinterpret_cast<sockaddr *>(&peer), &peerLen);
                if (n < 0) break;
                const int64_t now = monoUs();
                zicmp::Parsed p;
                if (peerLen < sizeof peer || peer.sin_family != AF_INET
                    || !zicmp::parseRaw(rbuf, size_t(n), p) || p.id != ident
                    || p.from != peer.sin_addr.s_addr)
                    continue;
                auto it = pending.find(p.seq);
                if (it == pending.end() || !zicmp::matchesDestination(p, it->second.destination))
                    continue;
                const long long rtt = (long long)(now - it->second.sentUs);
                const std::string from = addrText(p.from);
                switch (p.kind) {
                case zicmp::Kind::Reply:
                    emitLine("R %llu ok %lld %d %s", it->second.tag, rtt, p.ttl, from.c_str());
                    break;
                case zicmp::Kind::TimeExceeded:
                    emitLine("R %llu ttlx %lld %s", it->second.tag, rtt, from.c_str());
                    break;
                case zicmp::Kind::Unreachable:
                    emitLine("R %llu unreach %d %lld %s", it->second.tag, int(p.code), rtt, from.c_str());
                    break;
                default:
                    continue;
                }
                deadlines.erase(it->second.deadlineIt);
                pending.erase(it);
            }
        }

        const int64_t now = monoUs();
        while (!deadlines.empty() && deadlines.begin()->first <= now) {
            const uint16_t seq = deadlines.begin()->second;
            deadlines.erase(deadlines.begin());
            auto it = pending.find(seq);
            if (it != pending.end()) {
                emitLine("R %llu timeout", it->second.tag);
                pending.erase(it);
            }
        }
        if (!g_out.empty() && !flushOut())
            break;
    }
    close(s);
    return 0;
}
