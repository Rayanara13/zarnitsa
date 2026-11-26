// Статистика цели: кольцо сырых замеров, окно потерь/джиттера, MOS, поминутные агрегаты за сутки.
#pragma once

#include <QVector>
#include <QtGlobal>

namespace zr {

struct Sample {
    qint64 t = 0;      // мс от эпохи
    float rtt = -1.f;  // мс, -1 — ответа нет
    bool ok = false;
};

struct MinuteBucket {
    qint64 minute = -1;    // номер минуты от эпохи
    quint16 sent = 0;
    quint16 recv = 0;
    float min = 0.f;
    float max = 0.f;
    double sum = 0.0;
    double avg() const { return recv ? sum / recv : -1.0; }
};

struct WindowStats {
    int sent = 0;
    int recv = 0;
    double lossPct = 0.0;
    double last = -1.0;
    double min = -1.0;
    double avg = -1.0;
    double max = -1.0;
    double jitter = -1.0;   // среднее |Δ| между соседними ответами
};

class TargetStats {
public:
    explicit TargetStats(int rawCapacity = 600);

    void add(qint64 tMs, bool ok, double rttMs);
    void clear();

    bool hasData() const { return m_count > 0; }
    int count() const { return m_count; }
    WindowStats window(int n) const;
    QVector<Sample> lastSamples(int n) const;
    double avgOfLastOk(int n) const;     // среднее по n последним ответам
    double uptimePct(qint64 nowMs, int minutes = 1440) const;   // -1, если нет данных
    QVector<MinuteBucket> minuteSeries(qint64 nowMs, int minutes) const;

    qint64 totalSent() const { return m_totalSent; }
    qint64 totalRecv() const { return m_totalRecv; }
    double bestEver() const { return m_bestEver; }
    qint64 lastOkTime() const { return m_lastOk; }

    // Упрощённая E-модель ITU-T G.107: эффективная задержка, затем R-фактор и MOS.
    static double mos(double avgRttMs, double jitterMs, double lossPct);
    static QString mosTitle(double mos);

private:
    QVector<Sample> m_raw;
    int m_head = 0;
    int m_count = 0;
    QVector<MinuteBucket> m_minutes;
    qint64 m_totalSent = 0;
    qint64 m_totalRecv = 0;
    double m_bestEver = -1.0;
    qint64 m_lastOk = 0;
};

} // namespace zr
