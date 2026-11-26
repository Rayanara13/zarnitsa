#include "icmp_packet.h"

#include <cstring>

namespace zicmp {

static constexpr uint8_t kEchoReply = 0;
static constexpr uint8_t kDestUnreach = 3;
static constexpr uint8_t kEchoRequest = 8;
static constexpr uint8_t kTimeExceeded = 11;

static inline uint16_t rd16(const uint8_t *p) { return uint16_t((p[0] << 8) | p[1]); }

uint16_t checksum(const void *data, size_t len)
{
    const uint8_t *p = static_cast<const uint8_t *>(data);
    uint32_t sum = 0;
    while (len > 1) {
        sum += uint32_t((p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }
    if (len)
        sum += uint32_t(p[0] << 8);
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return uint16_t(~sum & 0xFFFF);
}

size_t buildEcho(uint8_t *out, size_t cap, uint16_t id, uint16_t seq, size_t payloadLen)
{
    const size_t total = 8 + payloadLen;
    if (total > cap)
        return 0;
    out[0] = kEchoRequest;
    out[1] = 0;
    out[2] = out[3] = 0;
    out[4] = uint8_t(id >> 8);
    out[5] = uint8_t(id & 0xFF);
    out[6] = uint8_t(seq >> 8);
    out[7] = uint8_t(seq & 0xFF);
    // узор как у iputils: 0x10, 0x11, ... — удобно узнавать свои пакеты в tcpdump
    for (size_t i = 0; i < payloadLen; ++i)
        out[8 + i] = uint8_t(0x10 + (i & 0x3F));
    const uint16_t c = checksum(out, total);
    out[2] = uint8_t(c >> 8);
    out[3] = uint8_t(c & 0xFF);
    return total;
}

// Разбор ICMP-сообщения, начиная с заголовка ICMP; внутри ошибок лежит исходный IP-заголовок.
static bool parseIcmp(const uint8_t *icmp, size_t len, Parsed &out)
{
    if (len < 8)
        return false;
    out.type = icmp[0];
    out.code = icmp[1];
    if (out.type == kEchoReply) {
        out.kind = Kind::Reply;
        out.id = rd16(icmp + 4);
        out.seq = rd16(icmp + 6);
        return true;
    }
    if (out.type == kTimeExceeded || out.type == kDestUnreach) {
        out.kind = out.type == kTimeExceeded ? Kind::TimeExceeded : Kind::Unreachable;
        const uint8_t *inner = icmp + 8;
        const size_t innerLen = len - 8;
        if (innerLen < 20)
            return false;
        const size_t ihl = size_t(inner[0] & 0x0F) * 4;
        if ((inner[0] >> 4) != 4 || ihl < 20 || innerLen < ihl + 8 || inner[9] != 1)
            return false;   // совпавшие id/seq в UDP/TCP не относятся к нашему Echo
        const uint8_t *orig = inner + ihl;
        if (!parseOriginalEcho(orig, innerLen - ihl, out.id, out.seq))
            return false;
        std::memcpy(&out.quotedDestination, inner + 16, 4);
        return true;
    }
    out.kind = Kind::Other;
    return false;
}

bool parseRaw(const uint8_t *buf, size_t len, Parsed &out)
{
    out = Parsed{};
    if (len < 20)
        return false;
    if ((buf[0] >> 4) != 4 || buf[9] != 1)
        return false;
    const size_t ihl = size_t(buf[0] & 0x0F) * 4;
    if (ihl < 20 || len < ihl + 8)
        return false;
    out.ttl = buf[8];
    std::memcpy(&out.from, buf + 12, 4);
    return parseIcmp(buf + ihl, len - ihl, out);
}

bool parseDgram(const uint8_t *buf, size_t len, Parsed &out)
{
    out = Parsed{};
    return parseIcmp(buf, len, out);
}

bool parseOriginalEcho(const uint8_t *buf, size_t len, uint16_t &id, uint16_t &seq)
{
    if (len < 8 || buf[0] != kEchoRequest)
        return false;
    id = rd16(buf + 4);
    seq = rd16(buf + 6);
    return true;
}

bool matchesDestination(const Parsed &packet, uint32_t destination)
{
    switch (packet.kind) {
    case Kind::Reply:
        return packet.from == destination;
    case Kind::TimeExceeded:
    case Kind::Unreachable:
        return packet.quotedDestination == destination;
    default:
        return false;
    }
}

} // namespace zicmp
