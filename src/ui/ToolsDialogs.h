// Инструменты: трассировка (и непрерывная, как MTR), сканирование подсети, DNS.
#pragma once

#include <QDialog>
#include <QHash>
#include <QTimer>
#include <QVector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace zr {

class Engine;
struct ProbeResult;

class TracerouteDialog : public QDialog {
    Q_OBJECT
public:
    TracerouteDialog(Engine *engine, const QString &host, QWidget *parent = nullptr);
    ~TracerouteDialog() override;
    void start();

private:
    struct Hop {
        QString addr;
        QString name;
        int sent = 0;
        int recv = 0;
        double last = -1, best = -1, worst = -1, sum = 0;
        bool final = false;
    };
    void round();
    void onResult(int ttl, const ProbeResult &r);
    void render();
    void stop();

    Engine *m_engine;
    QLineEdit *m_host;
    QSpinBox *m_maxHops;
    QCheckBox *m_continuous;
    QPushButton *m_go;
    QTableWidget *m_table;
    QLabel *m_status;
    QString m_ip;
    QVector<Hop> m_hops;
    QVector<quint64> m_tokens;
    QTimer m_probeTimer;
    QTimer m_roundTimer;
    quint64 m_generation = 0;
    int m_nextTtl = 1;
    int m_finalTtl = 0;
    int m_pending = 0;
    int m_rounds = 0;
    bool m_running = false;
};

class ScanDialog : public QDialog {
    Q_OBJECT
public:
    ScanDialog(Engine *engine, const QString &cidr, QWidget *parent = nullptr);
    ~ScanDialog() override;

    static QVector<quint32> expandRange(const QString &text, QString *error);

private:
    void start();
    void stop();
    void onPing(quint32 ip, const ProbeResult &r);
    void probePorts(int row, quint32 ip);
    void addSelected();

    Engine *m_engine;
    QLineEdit *m_range;
    QLineEdit *m_ports;
    QPushButton *m_go;
    QProgressBar *m_progress;
    QTableWidget *m_table;
    QComboBox *m_group;
    QLabel *m_status;
    QVector<quint64> m_tokens;
    QHash<quint32, int> m_rows;
    QVector<quint32> m_ips;
    QTimer m_probeTimer;
    quint64 m_generation = 0;
    int m_nextIp = 0;
    int m_total = 0;
    int m_done = 0;
    bool m_running = false;
};

class DnsDialog : public QDialog {
    Q_OBJECT
public:
    explicit DnsDialog(QWidget *parent = nullptr);

private:
    void lookup();
    QLineEdit *m_query;
    QPlainTextEdit *m_out;
};

} // namespace zr
