// Сборка и разбор ICMPv4 без Qt — общий код для окна и помощника zarnitsa-icmp.
#pragma once

#include <cstddef>
#include <cstdint>

namespace zicmp {

enum class Kind { Reply, TimeExceeded, Unreachable, Other };

struct Parsed {
    Kind kind = Kind::Other;
    uint8_t type = 0;
    uint8_t code = 0;
    uint16_t id = 0;
    uint16_t seq = 0;
    uint32_t from = 0;   // адрес ответившего, сетевой порядок байт (0 — неизвестен)
    uint32_t quotedDestination = 0;   // назначение исходного IPv4-пакета внутри ICMP-ошибки
    int ttl = -1;        // TTL ответа, если виден IP-заголовок
};

uint16_t checksum(const void *data, size_t len);

// Эхо-запрос: заголовок 8 байт + полезная нагрузка с узором. Возвращает длину или 0.
size_t buildEcho(uint8_t *out, size_t cap, uint16_t id, uint16_t seq, size_t payloadLen);

// Сырой сокет: буфер начинается с IPv4-заголовка.
bool parseRaw(const uint8_t *buf, size_t len, Parsed &out);

// Датаграммный ICMP-сокет: ядро отдаёт ICMP без IP-заголовка.
bool parseDgram(const uint8_t *buf, size_t len, Parsed &out);

// Исходный эхо-запрос, вернувшийся в очереди ошибок (IP_RECVERR): достаём seq.
bool parseOriginalEcho(const uint8_t *buf, size_t len, uint16_t &id, uint16_t &seq);

// После проверки id/seq: ответ должен быть от назначения, ошибка — цитировать его.
// Внешний отправитель ошибки может быть промежуточным маршрутизатором.
bool matchesDestination(const Parsed &packet, uint32_t destination);

} // namespace zicmp
