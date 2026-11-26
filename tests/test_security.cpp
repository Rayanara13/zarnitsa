// Защитные проверки: только синтетические пакеты/часы и локальные printf-скрипты.
// Ни один тест не отправляет Echo и не выдаёт права процессу окна.
#include "core/Monitor.h"
#include "icmp/icmp_packet.h"
#include "icmp/monitoring_budget.h"
#include "probes/IcmpEngine.h"
#include "probes/ProbeDispatcher.h"
#include "service/Alerter.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <arpa/inet.h>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <limits>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

using namespace zr;

namespace {

using Budget = zicmp::MonitoringBudget;
using Admission = Budget::Result;

QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool writeFile(const QString &path, const QByteArray &contents, bool executable = false)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(contents) != contents.size())
        return false;
    f.close();
    return !executable || f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

QString commandQuote(QString value)
{
    value.replace(QStringLiteral("\""), QStringLiteral("\"\"\""));
    return QLatin1Char('"') + value + QLatin1Char('"');
}

QString shellQuote(QString value)
{
    value.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + value + QLatin1Char('\'');
}

QByteArray nulArguments(const QStringList &args)
{
    QByteArray out;
    for (const QString &arg : args) {
        out += arg.toUtf8();
        out += '\0';
    }
    return out;
}

StatusEvent makeEvent()
{
    StatusEvent e;
    e.t = 1700000000000LL;
    e.name = QStringLiteral("узел");
    e.address = QStringLiteral("192.0.2.7");
    e.group = QStringLiteral("группа с пробелами");
    e.from = Status::Up;
    e.to = Status::Down;
    e.rttMs = 12.5;
    e.lossPct = 25.0;
    e.reason = QStringLiteral("причина {name} с \"кавычками\"");
    return e;
}

QStringList placeholders()
{
    return {QStringLiteral("{name}"), QStringLiteral("{address}"), QStringLiteral("{group}"),
            QStringLiteral("{status}"), QStringLiteral("{status_title}"), QStringLiteral("{prev}"),
            QStringLiteral("{rtt}"), QStringLiteral("{loss}"), QStringLiteral("{reason}"),
            QStringLiteral("{text}"), QStringLiteral("{time}")};
}

QStringList expandedArguments(const StatusEvent &e)
{
    return {e.name, e.address, e.group, statusKey(e.to), statusTitle(e.to), statusKey(e.from),
            e.rttMs >= 0 ? QString::number(e.rttMs, 'f', 1) : QString(),
            QString::number(e.lossPct, 'f', 1), e.reason, e.text(),
            QDateTime::fromMSecsSinceEpoch(e.t).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))};
}

uint32_t netAddress(const char *address)
{
    return htonl(QHostAddress(QString::fromLatin1(address)).toIPv4Address());
}

void checksumAt(QByteArray &bytes, int offset)
{
    bytes[offset] = bytes[offset + 1] = '\0';
    const uint16_t sum = zicmp::checksum(bytes.constData(), size_t(bytes.size()));
    bytes[offset] = char(sum >> 8);
    bytes[offset + 1] = char(sum & 0xff);
}

QByteArray ipv4(const char *source, const char *destination, int protocol, const QByteArray &body, int ttl = 64)
{
    QByteArray header(20, '\0');
    header[0] = char(0x45);
    const int total = header.size() + body.size();
    header[2] = char(total >> 8);
    header[3] = char(total & 0xff);
    header[8] = char(ttl);
    header[9] = char(protocol);
    const uint32_t src = netAddress(source), dst = netAddress(destination);
    std::memcpy(header.data() + 12, &src, sizeof src);
    std::memcpy(header.data() + 16, &dst, sizeof dst);
    checksumAt(header, 10);
    return header + body;
}

QByteArray echo(bool reply)
{
    QByteArray bytes(24, '\0');
    zicmp::buildEcho(reinterpret_cast<uint8_t *>(bytes.data()), size_t(bytes.size()), 0x1234, 0x5678, 16);
    if (reply) {
        bytes[0] = 0;
        checksumAt(bytes, 2);
    }
    return bytes;
}

QByteArray icmpError(int type, const char *destination, int protocol = 1)
{
    // Цитата содержит полный исходный IPv4-заголовок и первые восемь байт Echo.
    const QByteArray original = ipv4("192.0.2.10", destination, protocol, echo(false), 1);
    QByteArray error(8, '\0');
    error[0] = char(type);
    error[1] = char(type == 3 ? 1 : 0);
    error += original.left(28);
    checksumAt(error, 2);
    return ipv4("192.0.2.1", "192.0.2.10", 1, error, 254);
}

bool parseRaw(const QByteArray &bytes, zicmp::Parsed &out)
{
    return zicmp::parseRaw(reinterpret_cast<const uint8_t *>(bytes.constData()), size_t(bytes.size()), out);
}

// Ограничения устанавливаются только в дочернем процессе перед exec помощника.
// seccomp лишь запрещает системные вызовы; no_new_privs не позволяет получить filecap.
class RestrictedHelperProcess : public QProcess {
public:
    enum Mode { NoNewPrivileges, DenyCapset, DenyCapget, DenyPrctl, MismatchedUid, MismatchedGid };
    explicit RestrictedHelperProcess(Mode mode) : m_mode(mode) {}

protected:
    void setupChildProcess() override
    {
        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
            _exit(125);
        if (m_mode == MismatchedUid) {
            if (getuid() != 0 || geteuid() != 0 || setresuid(0, 65534, 65534) != 0)
                _exit(125);
            return;
        }
        if (m_mode == MismatchedGid) {
            if (getuid() != 0 || geteuid() != 0 || setresgid(getgid(), 65534, 65534) != 0)
                _exit(125);
            return;
        }
        if (m_mode == NoNewPrivileges)
            return;
        const unsigned blocked = m_mode == DenyCapset ? SYS_capset
            : m_mode == DenyCapget ? SYS_capget : SYS_prctl;
        sock_filter filter[] = {
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, blocked, 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | unsigned(EPERM)),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        };
        sock_fprog program{static_cast<unsigned short>(sizeof filter / sizeof filter[0]), filter};
        if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0)
            _exit(125);
    }

private:
    Mode m_mode;
};

} // namespace

class TestSecurity : public QObject {
    Q_OBJECT
private slots:
    void alertArguments_data();
    void alertArguments();
    void alertProgramNotExpanded();
    void alertShellLiteralAndEnvironment();
    void budgetMonitoringThousandNodes();
    void budgetGlobalPackets();
    void budgetGlobalBytes();
    void budgetDestinationPacketsAndTraceBurst();
    void budgetDestinationBytesAndAtomicAdmission();
    void budgetPendingAndInvalidSize();
    void budgetBoundedDestinationTable();
    void budgetClockRollbackAndLongIdle();
    void helperPrivilegeFailure_data();
    void helperPrivilegeFailure();
    void helperRejectsMismatchedIds_data();
    void helperRejectsMismatchedIds();
    void helperDropsCapabilities();
    void helperFailedStartStop();
    void helperStopCancelsRestart();
    void helperThrottleResult();
    void replyMustMatchDestination();
    void errorMustQuoteDestinationAndProtocol_data();
    void errorMustQuoteDestinationAndProtocol();
};

void TestSecurity::alertArguments_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<double>("rtt");
    QTest::newRow("quotes-flags") << QStringLiteral("узел\" --flag injected \"хвост") << 12.5;
    QTest::newRow("spaces-flags") << QStringLiteral("узел --flag injected") << -1.0;
    QTest::newRow("literal-placeholders") << QStringLiteral("узел {address} {reason} {text}") << 0.0;
}

void TestSecurity::alertArguments()
{
    QFETCH(QString, name);
    QFETCH(double, rtt);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString script = dir.filePath(QStringLiteral("capture.sh"));
    const QString output = dir.filePath(QStringLiteral("argv"));
    // Никаких eval: printf получает недоверенные значения только через "$@".
    QVERIFY(writeFile(script, "#!/bin/sh\nout=$1\nshift\nprintf '%s\\000' \"$@\" > \"$out\"\n"));
    Settings s;
    s.alertCommand = QStringLiteral("/bin/sh ") + commandQuote(script) + QLatin1Char(' ') + commandQuote(output)
        + QStringLiteral(" before");
    for (const QString &token : placeholders())
        s.alertCommand += QLatin1Char(' ') + commandQuote(token);
    s.alertCommand += QStringLiteral(" after");
    StatusEvent e = makeEvent();
    e.name = name;
    e.rttMs = rtt;
    ProbeDispatcher dispatcher;
    Monitor monitor(&dispatcher);
    Alerter alerter(&monitor);
    alerter.setSettings(s);
    QSignalSpy failed(&alerter, &Alerter::actionFailed);
    QStringList args{QStringLiteral("before")};
    args += expandedArguments(e);
    args += QStringLiteral("after");
    const QByteArray expected = nulArguments(args);
    alerter.runCommand(e);
    QCOMPARE(failed.count(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(readFile(output), expected, 3000);
}

void TestSecurity::alertProgramNotExpanded()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString script = dir.filePath(QStringLiteral("capture.sh"));
    const QString output = dir.filePath(QStringLiteral("must-not-exist"));
    QVERIFY(writeFile(script, "#!/bin/sh\nprintf '%s' fixed > \"$1\"\n", true));
    Settings s;
    s.alertCommand = commandQuote(dir.filePath(QStringLiteral("{name}")))
        + QLatin1Char(' ') + commandQuote(output);
    StatusEvent e = makeEvent();
    e.name = QStringLiteral("capture.sh");
    ProbeDispatcher dispatcher;
    Monitor monitor(&dispatcher);
    Alerter alerter(&monitor);
    alerter.setSettings(s);
    QSignalSpy failed(&alerter, &Alerter::actionFailed);
    alerter.runCommand(e);
    QCOMPARE(failed.count(), 1);
    QVERIFY(!QFileInfo::exists(output));
}

void TestSecurity::alertShellLiteralAndEnvironment()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString output = dir.filePath(QStringLiteral("shell-argv"));
    const QString marker = dir.filePath(QStringLiteral("must-not-exist"));
    StatusEvent e = makeEvent();
    // Даже при регрессии полезная нагрузка ограничена встроенным printf и локальным файлом.
    e.name = QStringLiteral("$(printf injected > %1)").arg(shellQuote(marker));
    e.reason = QStringLiteral("причина");
    Settings s;
    s.alertCommandShell = true;
    s.alertCommand = QStringLiteral("printf '%s\\000'");
    QStringList expected = placeholders();
    for (const QString &token : placeholders())
        s.alertCommand += QStringLiteral(" \"") + token + QLatin1Char('"');
    const QStringList variables{QStringLiteral("NAME"), QStringLiteral("ADDRESS"), QStringLiteral("GROUP"),
        QStringLiteral("STATUS"), QStringLiteral("PREV"), QStringLiteral("TEXT"), QStringLiteral("RTT"),
        QStringLiteral("LOSS"), QStringLiteral("REASON"), QStringLiteral("TIME")};
    for (const QString &variable : variables)
        s.alertCommand += QStringLiteral(" \"$ZARNITSA_") + variable + QLatin1Char('"');
    s.alertCommand += QStringLiteral(" > ") + shellQuote(output);
    expected += QStringList{e.name, e.address, e.group, statusKey(e.to), statusKey(e.from), e.text(),
        QString::number(e.rttMs, 'f', 1), QString::number(e.lossPct, 'f', 1), e.reason,
        QDateTime::fromMSecsSinceEpoch(e.t).toString(Qt::ISODate)};
    ProbeDispatcher dispatcher;
    Monitor monitor(&dispatcher);
    Alerter alerter(&monitor);
    alerter.setSettings(s);
    QSignalSpy failed(&alerter, &Alerter::actionFailed);
    alerter.runCommand(e);
    QCOMPARE(failed.count(), 0);
    const QByteArray bytes = nulArguments(expected);
    QTRY_COMPARE_WITH_TIMEOUT(readFile(output), bytes, 3000);
    QVERIFY(!QFileInfo::exists(marker));
}

void TestSecurity::budgetMonitoringThousandNodes()
{
    Budget budget;
    // 1000 адресов в секунду пачками по 25 на тике 25 мс; настоящих отправок нет.
    for (int second = 0; second < 3; ++second) {
        for (int tick = 0; tick < 40; ++tick) {
            const int64_t now = int64_t(second) * 1000000 + tick * 25000;
            for (int node = 0; node < 25; ++node)
                QCOMPARE(budget.take(uint32_t(1 + tick * 25 + node), 56, 1000, now), Admission::Allowed);
        }
    }
    QCOMPARE(budget.trackedDestinations(), size_t(1000));
}

void TestSecurity::budgetGlobalPackets()
{
    Budget budget;
    for (uint32_t i = 1; i <= Budget::kGlobalBurst; ++i)
        QCOMPARE(budget.take(i, 56, 0, 0), Admission::Allowed);
    QCOMPARE(budget.take(1000, 56, 0, 0), Admission::RateLimited);
    QCOMPARE(budget.take(1000, 56, 0, 833), Admission::RateLimited);
    QCOMPARE(budget.trackedDestinations(), size_t(Budget::kGlobalBurst));
    QCOMPARE(budget.take(1000, 56, 0, 834), Admission::Allowed);
    QCOMPARE(budget.take(1001, 56, 0, 834), Admission::RateLimited);
}

void TestSecurity::budgetGlobalBytes()
{
    Budget budget;
    QCOMPARE(budget.take(1, Budget::kMaxPayload, 0, 0), Admission::Allowed);
    QCOMPARE(budget.take(2, Budget::kMaxPayload, 0, 0), Admission::Allowed);
    QCOMPARE(budget.take(3, Budget::kMaxPayload, 0, 0), Admission::RateLimited);
    QCOMPARE(budget.trackedDestinations(), size_t(2));
    QCOMPARE(budget.take(3, 0, 0, 0), Admission::Allowed);
    QCOMPARE(budget.take(4, Budget::kMaxPayload, 0, 1000000), Admission::Allowed);
    QCOMPARE(budget.take(5, Budget::kMaxPayload, 0, 1000000), Admission::Allowed);
    QCOMPARE(budget.take(6, Budget::kMaxPayload, 0, 1000000), Admission::RateLimited);
}

void TestSecurity::budgetDestinationPacketsAndTraceBurst()
{
    Budget budget;
    QCOMPARE(Budget::kDestinationBurst, uint64_t(3));
    for (int probe = 0; probe < 3; ++probe)
        QCOMPARE(budget.take(1, 56, 0, 0), Admission::Allowed);
    QCOMPARE(budget.take(1, 56, 0, 0), Admission::RateLimited);
    QCOMPARE(budget.take(1, 56, 0, 99999), Admission::RateLimited);
    for (int tick = 1; tick <= 20; ++tick) {
        const int64_t now = int64_t(tick) * 100000;
        QCOMPARE(budget.take(1, 56, 0, now), Admission::Allowed);
        QCOMPARE(budget.take(1, 56, 0, now), Admission::RateLimited);
    }
    QCOMPARE(budget.take(2, 56, 0, 2000000), Admission::Allowed);
}

void TestSecurity::budgetDestinationBytesAndAtomicAdmission()
{
    Budget budget;
    QCOMPARE(budget.take(1, Budget::kMaxPayload, 0, 0), Admission::Allowed);
    QCOMPARE(budget.take(1, Budget::kMaxPayload, 0, 0), Admission::RateLimited);
    // Отказ одного адреса не расходует общий байтовый бюджет второго.
    QCOMPARE(budget.take(2, Budget::kMaxPayload, 0, 0), Admission::Allowed);
    QCOMPARE(budget.take(1, Budget::kMaxPayload, 0, 1000000), Admission::Allowed);
    QCOMPARE(budget.take(1, 56, 0, 1000000), Admission::Allowed);
    QCOMPARE(budget.take(1, Budget::kMaxPayload, 0, 1000000), Admission::RateLimited);
    QCOMPARE(budget.take(1, 56, 0, 1000000), Admission::Allowed);
}

void TestSecurity::budgetPendingAndInvalidSize()
{
    Budget budget;
    QCOMPARE(budget.take(1, 56, Budget::kMaxPending, 0), Admission::PendingLimit);
    QCOMPARE(budget.take(1, Budget::kMaxPayload + 1, 0, 0), Admission::InvalidSize);
    QCOMPARE(budget.take(1, std::numeric_limits<size_t>::max(), 0, 0), Admission::InvalidSize);
    QCOMPARE(budget.trackedDestinations(), size_t(0));
    for (int probe = 0; probe < 3; ++probe)
        QCOMPARE(budget.take(1, 56, Budget::kMaxPending - 1, 0), Admission::Allowed);
    QCOMPARE(budget.take(1, 56, 0, 0), Admission::RateLimited);
}

void TestSecurity::budgetBoundedDestinationTable()
{
    Budget budget;
    for (uint32_t i = 1; i <= Budget::kMaxDestinations; ++i)
        QCOMPARE(budget.take(i, 56, 0, int64_t(i) * 1000), Admission::Allowed);
    const int64_t now = int64_t(Budget::kMaxDestinations + 1) * 1000;
    QCOMPARE(budget.trackedDestinations(), Budget::kMaxDestinations);
    QCOMPARE(budget.take(100000, 56, 0, now), Admission::DestinationLimit);
    // Заполненная таблица не вытесняет живой адрес и не обнуляет его ограничения.
    for (int i = 0; i < 3; ++i)
        QCOMPARE(budget.take(1, 56, 0, now), Admission::Allowed);
    QCOMPARE(budget.take(1, 56, 0, now), Admission::RateLimited);
    QCOMPARE(budget.take(100001, 56, 0, now), Admission::DestinationLimit);
    QCOMPARE(budget.trackedDestinations(), Budget::kMaxDestinations);
    QCOMPARE(budget.take(100000, 56, 0, now + Budget::kDestinationIdleUs + 1000000), Admission::Allowed);
    QCOMPARE(budget.trackedDestinations(), size_t(1));
}

void TestSecurity::budgetClockRollbackAndLongIdle()
{
    Budget budget(1000000);
    for (int i = 0; i < 3; ++i)
        QCOMPARE(budget.take(1, 56, 0, 1000000), Admission::Allowed);
    QCOMPARE(budget.take(1, 56, 0, 0), Admission::RateLimited);
    QCOMPARE(budget.take(1, 56, 0, 1099999), Admission::RateLimited);
    QCOMPARE(budget.take(1, 56, 0, 1100000), Admission::Allowed);
    const int64_t muchLater = std::numeric_limits<int64_t>::max();
    for (int i = 0; i < 3; ++i)
        QCOMPARE(budget.take(1, 56, 0, muchLater), Admission::Allowed);
    QCOMPARE(budget.take(1, 56, 0, muchLater), Admission::RateLimited);
}

void TestSecurity::helperPrivilegeFailure_data()
{
    QTest::addColumn<int>("restriction");
    QTest::addColumn<QByteArray>("step");
    QTest::newRow("capset") << int(RestrictedHelperProcess::DenyCapset) << QByteArray("capset");
    QTest::newRow("capget") << int(RestrictedHelperProcess::DenyCapget) << QByteArray("capget");
    QTest::newRow("no-new-privs") << int(RestrictedHelperProcess::DenyPrctl) << QByteArray("no_new_privs");
}

void TestSecurity::helperPrivilegeFailure()
{
    QFETCH(int, restriction);
    QFETCH(QByteArray, step);
    const QString path = QStringLiteral(ZR_HELPER_PATH);
    QVERIFY2(QFileInfo::exists(path), "помощник должен собираться зависимостью zr_test");
    RestrictedHelperProcess p(static_cast<RestrictedHelperProcess::Mode>(restriction));
    p.start(path, QStringList());
    QVERIFY(p.waitForStarted(2000));
    p.write("Q\n");
    p.closeWriteChannel();
    QVERIFY(p.waitForFinished(3000));
    if (p.exitCode() == 125)
        QSKIP("seccomp/no_new_privs недоступен в окружении теста");
    const QByteArray out = p.readAllStandardOutput();
    QCOMPARE(p.exitStatus(), QProcess::NormalExit);
    QCOMPARE(p.exitCode(), 3);
    QVERIFY2(out.startsWith("ERROR " + step + ' '), out.constData());
    QVERIFY(!out.contains("READY"));
}

void TestSecurity::helperRejectsMismatchedIds_data()
{
    QTest::addColumn<int>("restriction");
    QTest::newRow("uid-euid") << int(RestrictedHelperProcess::MismatchedUid);
    QTest::newRow("gid-egid") << int(RestrictedHelperProcess::MismatchedGid);
}

void TestSecurity::helperRejectsMismatchedIds()
{
    QFETCH(int, restriction);
    if (getuid() != 0 || geteuid() != 0)
        QSKIP("различающиеся real/effective ID создаём только понижением прав потомка root в контейнере");
    RestrictedHelperProcess p(static_cast<RestrictedHelperProcess::Mode>(restriction));
    p.start(QStringLiteral(ZR_HELPER_PATH), QStringList());
    QVERIFY(p.waitForStarted(2000));
    p.write("Q\n");
    p.closeWriteChannel();
    QVERIFY(p.waitForFinished(3000));
    if (p.exitCode() == 125)
        QSKIP("окружение не разрешило понизить effective ID потомка");
    const QByteArray out = p.readAllStandardOutput();
    QCOMPARE(p.exitStatus(), QProcess::NormalExit);
    QCOMPARE(p.exitCode(), 3);
    QVERIFY2(out.startsWith("ERROR setuid/setgid "), out.constData());
    QVERIFY(!out.contains("READY"));
}

void TestSecurity::helperDropsCapabilities()
{
    RestrictedHelperProcess p(RestrictedHelperProcess::NoNewPrivileges);
    p.start(QStringLiteral(ZR_HELPER_PATH), QStringList());
    QVERIFY(p.waitForStarted(2000));
    QByteArray out;
    QTRY_VERIFY_WITH_TIMEOUT((out += p.readAllStandardOutput()).contains("READY") || p.state() == QProcess::NotRunning, 3000);
    out += p.readAllStandardOutput();
    if (p.exitCode() == 125 && p.state() == QProcess::NotRunning)
        QSKIP("no_new_privs недоступен в окружении теста");
    if (out.startsWith("ERROR socket "))
        QSKIP("сырой сокет недоступен без прав; проверка READY/CapEff выполняется в контейнере root");
    QVERIFY2(out.startsWith("READY 1 raw\n"), out.constData());
    const QByteArray status = readFile(QStringLiteral("/proc/%1/status").arg(p.processId()));
    p.write("Q\n");
    p.closeWriteChannel();
    QVERIFY(p.waitForFinished(3000));
    QCOMPARE(p.exitCode(), 0);
    QVERIFY(!status.isEmpty());
    for (const QByteArray &line : status.split('\n')) {
        if (line.startsWith("CapEff:") || line.startsWith("CapPrm:") || line.startsWith("CapInh:"))
            QCOMPARE(line.mid(line.indexOf(':') + 1).trimmed(), QByteArray("0000000000000000"));
    }
    QVERIFY(status.contains("NoNewPrivs:\t1"));
}

void TestSecurity::helperFailedStartStop()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    IcmpHelperBackend backend;
    QSignalSpy failed(&backend, &IcmpHelperBackend::failed);
    QSignalSpy ready(&backend, &IcmpHelperBackend::ready);
    backend.startHelper(dir.filePath(QStringLiteral("missing-helper")));
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 3000);
    backend.stopHelper();
    QCOMPARE(backend.state(), IcmpHelperBackend::State::Stopped);
    QTest::qWait(1300);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(failed.count(), 1);
    QCOMPARE(ready.count(), 0);
    QCOMPARE(backend.state(), IcmpHelperBackend::State::Stopped);
    QVERIFY(backend.findChildren<QProcess *>().isEmpty());
}

void TestSecurity::helperStopCancelsRestart()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString script = dir.filePath(QStringLiteral("helper.sh"));
    const QString launches = dir.filePath(QStringLiteral("launches"));
    const QByteArray contents = "#!/bin/sh\nprintf 'start\\n' >> " + shellQuote(launches).toUtf8()
        + "\nprintf 'READY 1 raw\\n'\n";
    QVERIFY(writeFile(script, contents, true));
    IcmpHelperBackend backend;
    QSignalSpy ready(&backend, &IcmpHelperBackend::ready);
    QSignalSpy failed(&backend, &IcmpHelperBackend::failed);
    backend.startHelper(script);
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() > 0 && backend.state() == IcmpHelperBackend::State::Starting, 3000);
    const QByteArray beforeStop = readFile(launches);
    QVERIFY(!beforeStop.isEmpty());
    backend.stopHelper();
    QCOMPARE(backend.state(), IcmpHelperBackend::State::Stopped);
    QTest::qWait(1300);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(readFile(launches), beforeStop);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(backend.state(), IcmpHelperBackend::State::Stopped);
    QVERIFY(backend.findChildren<QProcess *>().isEmpty());
}

void TestSecurity::helperThrottleResult()
{
    qRegisterMetaType<ProbeResult>();
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString script = dir.filePath(QStringLiteral("limited-helper.sh"));
    QVERIFY(writeFile(script, "#!/bin/sh\nprintf 'READY 1 raw\\n'\n"
        "while read -r op tag rest; do\n"
        "  [ \"$op\" = Q ] && break\n"
        "  printf 'R %s err EAGAIN\\n' \"$tag\"\n"
        "done\n", true));
    IcmpHelperBackend backend;
    QSignalSpy ready(&backend, &IcmpHelperBackend::ready);
    QSignalSpy finished(&backend, &ProbeBackend::finished);
    backend.startHelper(script);
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 3000);
    ProbeRequest request;
    request.token = 17;
    request.host = QStringLiteral("127.0.0.1");
    backend.start(request);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 3000);
    const ProbeResult result = finished.first().first().value<ProbeResult>();
    QCOMPARE(result.token, request.token);
    QCOMPARE(result.outcome, ProbeOutcome::Error);
    QVERIFY(result.error.contains(QStringLiteral("лимит")));
    QVERIFY(result.error.contains(QStringLiteral("EAGAIN")));
    backend.stopHelper();
    QTest::qWait(50);
    QCOMPARE(finished.count(), 1);   // немедленного автоматического повтора нет
}

void TestSecurity::replyMustMatchDestination()
{
    const uint32_t expected = netAddress("198.51.100.7");
    zicmp::Parsed packet;
    QVERIFY(parseRaw(ipv4("198.51.100.7", "192.0.2.10", 1, echo(true)), packet));
    QCOMPARE(packet.id, uint16_t(0x1234));
    QCOMPARE(packet.seq, uint16_t(0x5678));
    QVERIFY(zicmp::matchesDestination(packet, expected));
    QVERIFY(parseRaw(ipv4("198.51.100.8", "192.0.2.10", 1, echo(true)), packet));
    QCOMPARE(packet.id, uint16_t(0x1234));
    QCOMPARE(packet.seq, uint16_t(0x5678));
    QVERIFY(!zicmp::matchesDestination(packet, expected));
    QVERIFY(!parseRaw(ipv4("198.51.100.7", "192.0.2.10", 17, echo(true)), packet));
    const QByteArray dgram = echo(true);
    QVERIFY(zicmp::parseDgram(reinterpret_cast<const uint8_t *>(dgram.constData()), size_t(dgram.size()), packet));
    packet.from = expected;   // sender из recvmsg датаграммного сокета
    QVERIFY(zicmp::matchesDestination(packet, expected));
    packet.from = netAddress("198.51.100.8");
    QVERIFY(!zicmp::matchesDestination(packet, expected));
    packet.kind = zicmp::Kind::Other;
    QVERIFY(!zicmp::matchesDestination(packet, expected));
}

void TestSecurity::errorMustQuoteDestinationAndProtocol_data()
{
    QTest::addColumn<int>("type");
    QTest::newRow("ttl-exceeded-router") << 11;
    QTest::newRow("unreachable-router") << 3;
}

void TestSecurity::errorMustQuoteDestinationAndProtocol()
{
    QFETCH(int, type);
    const uint32_t expected = netAddress("198.51.100.7");
    zicmp::Parsed packet;
    const QByteArray good = icmpError(type, "198.51.100.7");
    QVERIFY(parseRaw(good, packet));
    QCOMPARE(packet.id, uint16_t(0x1234));
    QCOMPARE(packet.seq, uint16_t(0x5678));
    QCOMPARE(packet.from, netAddress("192.0.2.1"));
    QCOMPARE(packet.quotedDestination, expected);
    QVERIFY(packet.from != expected);
    QVERIFY(zicmp::matchesDestination(packet, expected));
    QVERIFY(parseRaw(icmpError(type, "198.51.100.8"), packet));
    QVERIFY(!zicmp::matchesDestination(packet, expected));
    QVERIFY(!parseRaw(icmpError(type, "198.51.100.7", 17), packet));
    QVERIFY(!parseRaw(icmpError(type, "198.51.100.7", 6), packet));
    QVERIFY(!parseRaw(good.left(good.size() - 1), packet));
    const QByteArray dgram = echo(true);
    QVERIFY(zicmp::parseDgram(reinterpret_cast<const uint8_t *>(dgram.constData()), size_t(dgram.size()), packet));
    QCOMPARE(packet.quotedDestination, uint32_t(0));
}

QTEST_GUILESS_MAIN(TestSecurity)
#include "test_security.moc"
