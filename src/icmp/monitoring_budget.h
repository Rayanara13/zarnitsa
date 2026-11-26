// Ограничения помощника ICMP без Qt: часы передаёт вызывающий код, очереди нет.
#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace zicmp {

class MonitoringBudget {
public:
    static constexpr uint64_t kGlobalPps = 1200;
    static constexpr uint64_t kGlobalBurst = 128;
    static constexpr uint64_t kGlobalBytesPerSecond = 256 * 1024;
    static constexpr uint64_t kGlobalByteBurst = 128 * 1024;
    static constexpr uint64_t kDestinationPps = 10;
    static constexpr uint64_t kDestinationBurst = 3;   // три пробы одного TTL трассировки
    static constexpr uint64_t kDestinationBytesPerSecond = 64 * 1024;
    static constexpr uint64_t kDestinationByteBurst = 64 * 1024;
    static constexpr size_t kMaxPending = 4096;
    static constexpr size_t kMaxDestinations = 4096;
    static constexpr size_t kMaxPayload = 65000;
    static constexpr size_t kIpv4IcmpOverhead = 20 + 8;
    static constexpr int64_t kDestinationIdleUs = 60000000;

    enum class Result { Allowed, RateLimited, PendingLimit, DestinationLimit, InvalidSize };

    explicit MonitoringBudget(int64_t nowUs = 0)
        : m_packets(kGlobalBurst, nowUs)
        , m_bytes(kGlobalByteBurst, nowUs)
        , m_lastCleanupUs(nowUs)
    {
    }

    // destination — IPv4 в сетевом порядке; считаем и IPv4-, и ICMP-заголовки.
    // Отказ не ставит запрос в очередь и не расходует остальные бюджеты.
    Result take(uint32_t destination, size_t payloadBytes, size_t pendingCount, int64_t nowUs)
    {
        if (payloadBytes > kMaxPayload)
            return Result::InvalidSize;
        if (pendingCount >= kMaxPending)
            return Result::PendingLimit;
        const uint64_t bytes = uint64_t(payloadBytes + kIpv4IcmpOverhead);
        m_packets.refill(nowUs, kGlobalPps, kGlobalBurst);
        m_bytes.refill(nowUs, kGlobalBytesPerSecond, kGlobalByteBurst);
        if (!m_packets.has(1) || !m_bytes.has(bytes))
            return Result::RateLimited;

        cleanup(nowUs);
        auto it = m_destinations.find(destination);
        if (it == m_destinations.end()) {
            if (m_destinations.size() >= kMaxDestinations)
                return Result::DestinationLimit;
            it = m_destinations.emplace(destination, Destination(nowUs)).first;
        }
        Destination &dst = it->second;
        dst.packets.refill(nowUs, kDestinationPps, kDestinationBurst);
        dst.bytes.refill(nowUs, kDestinationBytesPerSecond, kDestinationByteBurst);
        if (!dst.packets.has(1) || !dst.bytes.has(bytes))
            return Result::RateLimited;

        m_packets.spend(1);
        m_bytes.spend(bytes);
        dst.packets.spend(1);
        dst.bytes.spend(bytes);
        if (nowUs > dst.lastUsedUs)
            dst.lastUsedUs = nowUs;
        return Result::Allowed;
    }

    size_t trackedDestinations() const { return m_destinations.size(); }

private:
    // Доли токенов храним целыми: 100 мс при 10 pps дают ровно один токен.
    static constexpr uint64_t kScale = 1000000;

    struct Bucket {
        uint64_t credit;
        int64_t updatedUs;

        Bucket(uint64_t capacity, int64_t nowUs)
            : credit(capacity * kScale), updatedUs(nowUs) {}

        void refill(int64_t nowUs, uint64_t perSecond, uint64_t capacity)
        {
            if (nowUs <= updatedUs)
                return;   // откат часов не создаёт токенов
            const uint64_t elapsed = uint64_t(nowUs) - uint64_t(updatedUs);
            const uint64_t missing = capacity * kScale - credit;
            // Ограничиваем произведение до умножения, в том числе после долгого простоя.
            credit += elapsed >= (missing + perSecond - 1) / perSecond
                ? missing : elapsed * perSecond;
            updatedUs = nowUs;
        }

        bool has(uint64_t amount) const { return credit >= amount * kScale; }
        void spend(uint64_t amount) { credit -= amount * kScale; }
    };

    struct Destination {
        Bucket packets;
        Bucket bytes;
        int64_t lastUsedUs;

        explicit Destination(int64_t nowUs)
            : packets(kDestinationBurst, nowUs)
            , bytes(kDestinationByteBurst, nowUs)
            , lastUsedUs(nowUs)
        {
        }
    };

    void cleanup(int64_t nowUs)
    {
        if (nowUs <= m_lastCleanupUs || uint64_t(nowUs) - uint64_t(m_lastCleanupUs) < kScale)
            return;
        m_lastCleanupUs = nowUs;
        for (auto it = m_destinations.begin(); it != m_destinations.end();) {
            const int64_t last = it->second.lastUsedUs;
            // Активные записи не вытесняем: 60 с — максимальный таймаут запроса.
            if (nowUs >= last && uint64_t(nowUs) - uint64_t(last) >= uint64_t(kDestinationIdleUs))
                it = m_destinations.erase(it);
            else
                ++it;
        }
    }

    Bucket m_packets;
    Bucket m_bytes;
    int64_t m_lastCleanupUs;
    std::unordered_map<uint32_t, Destination> m_destinations;
};

static_assert(MonitoringBudget::kMaxPending < 65536, "Нужны свободные номера ICMP sequence");

} // namespace zicmp
