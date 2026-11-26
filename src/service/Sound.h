// Звук без QtMultimedia: тоны генерируем сами в WAV и играем через paplay/pw-play/aplay.
#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>

namespace zr {

class Sound : public QObject {
    Q_OBJECT
public:
    explicit Sound(QObject *parent = nullptr);
    void play(bool alarm);
    QString player() const { return m_player; }
    static QByteArray makeWav(const QVector<QPair<double, int>> &tones);   // (Гц, мс)

signals:
    void fallbackBeep();

private:
    QString ensureFile(bool alarm);
    QString m_player;
    QString m_dir;
    QElapsedTimer m_last;
};

} // namespace zr
