#include "Settings.h"

#include <QtGlobal>

namespace zr {

QJsonObject Settings::toJson() const
{
    QJsonObject o;
    o[QStringLiteral("interval_ms")] = intervalMs;
    o[QStringLiteral("timeout_ms")] = timeoutMs;
    o[QStringLiteral("packet_size")] = packetSize;
    o[QStringLiteral("ttl")] = ttl;
    o[QStringLiteral("down_after")] = downAfter;
    o[QStringLiteral("up_after")] = upAfter;
    o[QStringLiteral("latency_warn_ms")] = latencyWarnMs;
    o[QStringLiteral("latency_dynamic")] = latencyDynamic;
    o[QStringLiteral("latency_dynamic_delta_ms")] = latencyDynamicDeltaMs;
    o[QStringLiteral("loss_warn_pct")] = lossWarnPct;
    o[QStringLiteral("stats_window")] = statsWindow;
    o[QStringLiteral("flap_changes")] = flapChanges;
    o[QStringLiteral("flap_window_sec")] = flapWindowSec;

    o[QStringLiteral("theme")] = theme;
    o[QStringLiteral("density")] = density;
    o[QStringLiteral("sort")] = sortMode;
    o[QStringLiteral("columns")] = columns;
    o[QStringLiteral("always_on_top")] = alwaysOnTop;
    o[QStringLiteral("minimize_to_tray")] = minimizeToTray;
    o[QStringLiteral("close_to_tray")] = closeToTray;
    o[QStringLiteral("start_minimized")] = startMinimized;
    o[QStringLiteral("only_problems")] = onlyProblems;
    o[QStringLiteral("show_sparkline")] = showSparkline;
    o[QStringLiteral("show_mos")] = showMos;

    o[QStringLiteral("alert_tray")] = alertTray;
    o[QStringLiteral("alert_toast")] = alertToast;
    o[QStringLiteral("alert_corner")] = alertCorner;
    o[QStringLiteral("alert_sound")] = alertSound;
    o[QStringLiteral("alert_on_degraded")] = alertOnDegraded;
    o[QStringLiteral("alert_on_recover")] = alertOnRecover;
    o[QStringLiteral("alert_quiet_flapping")] = alertQuietFlapping;
    o[QStringLiteral("alert_min_down_sec")] = alertMinDownSec;
    o[QStringLiteral("alert_command")] = alertCommand;
    o[QStringLiteral("alert_command_shell")] = alertCommandShell;
    o[QStringLiteral("webhook_url")] = webhookUrl;

    o[QStringLiteral("icmp_mode")] = icmpMode;
    o[QStringLiteral("helper_path")] = helperPath;
    o[QStringLiteral("ping_max_parallel")] = pingMaxParallel;

    o[QStringLiteral("api_enabled")] = apiEnabled;
    o[QStringLiteral("api_listen")] = apiListen;
    o[QStringLiteral("api_token")] = apiToken;
    o[QStringLiteral("api_tls_cert")] = apiTlsCert;
    o[QStringLiteral("api_tls_key")] = apiTlsKey;

    o[QStringLiteral("events_keep_months")] = eventsKeepMonths;
    return o;
}

Settings Settings::fromJson(const QJsonObject &o)
{
    Settings s;
    auto i = [&o](const char *k, int def, int lo, int hi) {
        return qBound(lo, o.value(QLatin1String(k)).toInt(def), hi);
    };
    auto b = [&o](const char *k, bool def) { return o.value(QLatin1String(k)).toBool(def); };
    auto str = [&o](const char *k, const QString &def) { return o.value(QLatin1String(k)).toString(def); };

    s.intervalMs = i("interval_ms", s.intervalMs, 100, 86400000);
    s.timeoutMs = i("timeout_ms", s.timeoutMs, 50, 60000);
    s.packetSize = i("packet_size", s.packetSize, 0, 65000);
    s.ttl = i("ttl", s.ttl, 1, 255);
    s.downAfter = i("down_after", s.downAfter, 1, 100);
    s.upAfter = i("up_after", s.upAfter, 1, 100);
    s.latencyWarnMs = i("latency_warn_ms", s.latencyWarnMs, 0, 60000);
    s.latencyDynamic = b("latency_dynamic", s.latencyDynamic);
    s.latencyDynamicDeltaMs = i("latency_dynamic_delta_ms", s.latencyDynamicDeltaMs, 1, 60000);
    s.lossWarnPct = qBound(0.0, o.value(QStringLiteral("loss_warn_pct")).toDouble(s.lossWarnPct), 100.0);
    s.statsWindow = i("stats_window", s.statsWindow, 5, 600);
    s.flapChanges = i("flap_changes", s.flapChanges, 2, 100);
    s.flapWindowSec = i("flap_window_sec", s.flapWindowSec, 30, 86400);

    s.theme = str("theme", s.theme) == QLatin1String("light") ? QStringLiteral("light") : QStringLiteral("dark");
    s.density = str("density", s.density);
    if (s.density != QLatin1String("compact") && s.density != QLatin1String("list"))
        s.density = QStringLiteral("comfortable");
    s.sortMode = str("sort", s.sortMode);
    s.columns = i("columns", s.columns, 0, 32);
    s.alwaysOnTop = b("always_on_top", s.alwaysOnTop);
    s.minimizeToTray = b("minimize_to_tray", s.minimizeToTray);
    s.closeToTray = b("close_to_tray", s.closeToTray);
    s.startMinimized = b("start_minimized", s.startMinimized);
    s.onlyProblems = b("only_problems", s.onlyProblems);
    s.showSparkline = b("show_sparkline", s.showSparkline);
    s.showMos = b("show_mos", s.showMos);

    s.alertTray = b("alert_tray", s.alertTray);
    s.alertToast = b("alert_toast", s.alertToast);
    s.alertCorner = b("alert_corner", s.alertCorner);
    s.alertSound = b("alert_sound", s.alertSound);
    s.alertOnDegraded = b("alert_on_degraded", s.alertOnDegraded);
    s.alertOnRecover = b("alert_on_recover", s.alertOnRecover);
    s.alertQuietFlapping = b("alert_quiet_flapping", s.alertQuietFlapping);
    s.alertMinDownSec = i("alert_min_down_sec", s.alertMinDownSec, 0, 86400);
    s.alertCommand = str("alert_command", s.alertCommand);
    s.alertCommandShell = b("alert_command_shell", s.alertCommandShell);
    s.webhookUrl = str("webhook_url", s.webhookUrl);

    s.icmpMode = str("icmp_mode", s.icmpMode);
    s.helperPath = str("helper_path", s.helperPath);
    s.pingMaxParallel = i("ping_max_parallel", s.pingMaxParallel, 1, 1024);

    s.apiEnabled = b("api_enabled", s.apiEnabled);
    s.apiListen = str("api_listen", s.apiListen);
    s.apiToken = str("api_token", s.apiToken);
    s.apiTlsCert = str("api_tls_cert", s.apiTlsCert);
    s.apiTlsKey = str("api_tls_key", s.apiTlsKey);

    s.eventsKeepMonths = i("events_keep_months", s.eventsKeepMonths, 1, 120);
    return s;
}

} // namespace zr
