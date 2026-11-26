#include "Sound.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QVector>
#include <QtEndian>
#include <cmath>

namespace zr {

Sound::Sound(QObject *parent)
    : QObject(parent)
{
    for (const char *p : {"paplay", "pw-play", "aplay"}) {
        const QString path = QStandardPaths::findExecutable(QString::fromLatin1(p));
        if (!path.isEmpty()) { m_player = path; break; }
    }
    m_dir = QDir::tempPath() + QStringLiteral("/zarnitsa-sound-%1").arg(QString::fromLocal8Bit(qgetenv("USER")));
}

QByteArray Sound::makeWav(const QVector<QPair<double, int>> &tones)
{
    const int rate = 44100;
    QVector<qint16> pcm;
    for (const auto &t : tones) {
        const int n = rate * t.second / 1000;
        const int fade = qMin(n / 4, rate * 12 / 1000);
        for (int i = 0; i < n; ++i) {
            double env = 1.0;
            if (i < fade) env = double(i) / fade;
            else if (i > n - fade) env = double(n - i) / fade;
            // основной тон и мягкая октава — звучит как колокольчик, а не как писк
            constexpr double kPi = 3.14159265358979323846;
            const double s = 0.55 * std::sin(2.0 * kPi * t.first * i / rate)
                + 0.15 * std::sin(4.0 * kPi * t.first * i / rate);
            pcm.append(qint16(s * env * 20000.0));
        }
        for (int i = 0; i < rate * 35 / 1000; ++i) pcm.append(0);
    }
    QByteArray wav;
    const quint32 dataLen = quint32(pcm.size() * 2);
    auto u32 = [&wav](quint32 v) { char b[4]; qToLittleEndian(v, b); wav.append(b, 4); };
    auto u16 = [&wav](quint16 v) { char b[2]; qToLittleEndian(v, b); wav.append(b, 2); };
    wav.append("RIFF", 4); u32(36 + dataLen); wav.append("WAVE", 4);
    wav.append("fmt ", 4); u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
    wav.append("data", 4); u32(dataLen);
    for (qint16 s : pcm) u16(quint16(s));
    return wav;
}

QString Sound::ensureFile(bool alarm)
{
    QDir().mkpath(m_dir);
    const QString path = m_dir + (alarm ? QStringLiteral("/down.wav") : QStringLiteral("/up.wav"));
    if (!QFile::exists(path)) {
        const QByteArray wav = alarm ? makeWav({{880.0, 140}, {622.0, 220}}) : makeWav({{660.0, 110}, {990.0, 170}});
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) f.write(wav);
    }
    return path;
}

void Sound::play(bool alarm)
{
    if (m_last.isValid() && m_last.elapsed() < 1500)
        return;   // сотня падений разом — один сигнал, а не канонада
    m_last.start();
    if (m_player.isEmpty()) {
        emit fallbackBeep();
        return;
    }
    QStringList args;
    if (m_player.endsWith(QLatin1String("aplay"))) args << QStringLiteral("-q");
    args << ensureFile(alarm);
    if (!QProcess::startDetached(m_player, args))
        emit fallbackBeep();
}

} // namespace zr
