#include "Dialogs.h"

#include "Icons.h"
#include "Theme.h"
#include "api/ApiController.h"
#include "core/EventLog.h"
#include "core/Monitor.h"
#include "probes/IcmpEngine.h"
#include "service/Engine.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace zr {

static QLabel *hint(const QString &text, QWidget *parent = nullptr)
{
    auto *l = new QLabel(text, parent);
    l->setProperty("role", "faint");
    l->setWordWrap(true);
    return l;
}

static QSpinBox *spin(int lo, int hi, int val, const QString &suffix = QString(), const QString &special = QString())
{
    auto *s = new QSpinBox;
    s->setRange(lo, hi);
    s->setValue(val);
    if (!suffix.isEmpty()) s->setSuffix(suffix);
    if (!special.isEmpty()) s->setSpecialValueText(special);
    s->setMinimumWidth(110);
    return s;
}

static QPushButton *accentButton(QPushButton *b)
{
    b->setProperty("accent", true);
    return b;
}

// ===========================================================================
// Цель
// ===========================================================================

TargetDialog::TargetDialog(Engine *engine, const TargetConfig &t, bool isNew, QWidget *parent)
    : QDialog(parent)
    , m_engine(engine)
    , m_t(t)
    , m_new(isNew)
{
    setWindowTitle(isNew ? QStringLiteral("Новая цель") : QStringLiteral("Цель «%1»").arg(t.displayName()));
    setMinimumWidth(560);
    auto *root = new QVBoxLayout(this);
    root->setSpacing(10);

    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(8);

    m_preset = new QComboBox;
    m_preset->addItem(QStringLiteral("—"));
    for (const TargetPreset &p : targetPresets())
        m_preset->addItem(p.title, p.key);
    form->addRow(QStringLiteral("Шаблон"), m_preset);

    m_name = new QLineEdit(t.name);
    m_name->setPlaceholderText(QStringLiteral("например, Приёмная · 101004001"));
    form->addRow(QStringLiteral("Название"), m_name);

    m_address = new QLineEdit(t.address);
    m_address->setPlaceholderText(QStringLiteral("10.0.0.1, имя хоста или https://адрес/путь"));
    form->addRow(QStringLiteral("Адрес"), m_address);

    auto *kindRow = new QHBoxLayout;
    m_kind = new QComboBox;
    m_kind->addItem(QStringLiteral("ICMP — пинг"), int(ProbeKind::Icmp));
    m_kind->addItem(QStringLiteral("TCP — порт"), int(ProbeKind::Tcp));
    m_kind->addItem(QStringLiteral("HTTP(S) — веб-проверка"), int(ProbeKind::Http));
    m_kind->addItem(QStringLiteral("SIP OPTIONS"), int(ProbeKind::Sip));
    m_kind->addItem(QStringLiteral("DNS — имя"), int(ProbeKind::Dns));
    m_kind->setCurrentIndex(qMax(0, m_kind->findData(int(t.kind))));
    m_port = spin(0, 65535, t.port, QString(), QStringLiteral("нет"));
    kindRow->addWidget(m_kind, 1);
    kindRow->addWidget(new QLabel(QStringLiteral("порт")));
    kindRow->addWidget(m_port);
    form->addRow(QStringLiteral("Проверка"), kindRow);

    m_group = new QComboBox;
    m_group->setEditable(true);
    for (const GroupInfo &g : engine->monitor()->groups())
        if (!g.name.isEmpty()) m_group->addItem(g.name);
    m_group->setCurrentText(t.group);
    m_group->lineEdit()->setPlaceholderText(QStringLiteral("Без группы"));
    form->addRow(QStringLiteral("Группа"), m_group);

    m_parent = new QComboBox;
    m_parent->addItem(QStringLiteral("— ни от чего"), QString());
    for (const TargetConfig &o : engine->monitor()->targets())
        if (o.id != t.id) m_parent->addItem(QStringLiteral("%1  (%2)").arg(o.displayName(), o.publicEndpointText()), o.id);
    m_parent->setCurrentIndex(qMax(0, m_parent->findData(t.parentId)));
    m_parent->setToolTip(QStringLiteral("Если родитель лежит, эта цель не шумит: «за родителем»"));
    form->addRow(QStringLiteral("Зависит от"), m_parent);
    root->addLayout(form);

    const Settings &s = engine->settings();
    auto *grid = new QHBoxLayout;
    m_icmpBox = new QGroupBox(QStringLiteral("Замер"));
    auto *f1 = new QFormLayout(m_icmpBox);
    m_interval = new QDoubleSpinBox;
    m_interval->setRange(0, 86400);
    m_interval->setDecimals(1);
    m_interval->setSingleStep(0.5);
    m_interval->setSpecialValueText(QStringLiteral("общий (%1 с)").arg(fmtNum(s.intervalMs / 1000.0, 1)));
    m_interval->setValue(t.intervalMs / 1000.0);
    m_interval->setSuffix(QStringLiteral(" с"));
    f1->addRow(QStringLiteral("Интервал"), m_interval);
    m_timeout = spin(0, 60000, t.timeoutMs, QStringLiteral(" мс"), QStringLiteral("общий (%1 мс)").arg(s.timeoutMs));
    f1->addRow(QStringLiteral("Таймаут"), m_timeout);
    m_size = spin(0, 65000, t.packetSize, QStringLiteral(" байт"), QStringLiteral("общий (%1)").arg(s.packetSize));
    f1->addRow(QStringLiteral("Размер"), m_size);
    m_ttl = spin(0, 255, t.ttl, QString(), QStringLiteral("общий (%1)").arg(s.ttl));
    f1->addRow(QStringLiteral("TTL"), m_ttl);
    m_df = new QCheckBox(QStringLiteral("Не фрагментировать (DF)"));
    m_df->setChecked(t.dontFragment);
    f1->addRow(QString(), m_df);
    grid->addWidget(m_icmpBox);

    auto *thr = new QGroupBox(QStringLiteral("Пороги"));
    auto *f2 = new QFormLayout(thr);
    m_latency = spin(0, 60000, t.latencyWarnMs, QStringLiteral(" мс"), QStringLiteral("общая (%1 мс)").arg(s.latencyWarnMs));
    f2->addRow(QStringLiteral("«Плохо» от"), m_latency);
    m_loss = new QDoubleSpinBox;
    m_loss->setRange(-1, 100);
    m_loss->setDecimals(1);
    m_loss->setSpecialValueText(QStringLiteral("общие (%1%)").arg(fmtNum(s.lossWarnPct, 0)));
    m_loss->setValue(t.lossWarnPct);
    m_loss->setSuffix(QStringLiteral(" %"));
    f2->addRow(QStringLiteral("Потери от"), m_loss);
    m_downAfter = spin(0, 100, t.downAfter, QString(), QStringLiteral("общее (%1)").arg(s.downAfter));
    f2->addRow(QStringLiteral("Неудач до «нет связи»"), m_downAfter);
    m_upAfter = spin(0, 100, t.upAfter, QString(), QStringLiteral("общее (%1)").arg(s.upAfter));
    f2->addRow(QStringLiteral("Ответов до «в норме»"), m_upAfter);
    grid->addWidget(thr);
    root->addLayout(grid);

    m_httpBox = new QGroupBox(QStringLiteral("HTTP(S)"));
    auto *f3 = new QFormLayout(m_httpBox);
    m_httpCode = spin(0, 599, t.httpExpect, QString(), QStringLiteral("любой 2xx/3xx"));
    f3->addRow(QStringLiteral("Ожидаемый код"), m_httpCode);
    m_httpMatch = new QLineEdit(t.httpMatch);
    m_httpMatch->setPlaceholderText(QStringLiteral("текст, который должен быть в ответе"));
    f3->addRow(QStringLiteral("Текст в ответе"), m_httpMatch);
    m_pin = new QLineEdit(t.tlsPin);
    m_pin->setPlaceholderText(QStringLiteral("SHA-256 сертификата — для самоподписанных, как у Росы"));
    m_pin->setFont(Theme::mono(12));
    f3->addRow(QStringLiteral("Отпечаток"), m_pin);
    m_insecure = new QCheckBox(QStringLiteral("Не проверять сертификат"));
    m_insecure->setChecked(t.tlsInsecure);
    f3->addRow(QString(), m_insecure);
    root->addWidget(m_httpBox);

    m_notes = new QPlainTextEdit(t.notes);
    m_notes->setPlaceholderText(QStringLiteral("Заметки: где стоит, кому звонить, что уже пробовали"));
    m_notes->setFixedHeight(64);
    root->addWidget(m_notes);

    m_error = new QLabel;
    m_error->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::p().badText.name()));
    m_error->hide();
    root->addWidget(m_error);

    auto *buttons = new QDialogButtonBox;
    auto *ok = accentButton(buttons->addButton(isNew ? QStringLiteral("Добавить") : QStringLiteral("Сохранить"), QDialogButtonBox::AcceptRole));
    ok->setDefault(true);
    buttons->addButton(QStringLiteral("Отмена"), QDialogButtonBox::RejectRole);
    connect(buttons, &QDialogButtonBox::accepted, this, &TargetDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &TargetDialog::reject);
    root->addWidget(buttons);

    connect(m_preset, QOverload<int>::of(&QComboBox::activated), this, &TargetDialog::applyPreset);
    connect(m_kind, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &TargetDialog::syncKind);
    syncKind();
}

void TargetDialog::applyPreset(int index)
{
    if (index <= 0) return;
    const TargetPreset p = targetPresets().value(index - 1);
    QString host = m_address->text().trimmed();
    if (host.contains(QStringLiteral("://"))) host = QUrl(host).host();
    const TargetConfig t = zr::applyPreset(p, host.isEmpty() ? QStringLiteral("10.0.0.1") : host, m_name->text());
    m_kind->setCurrentIndex(qMax(0, m_kind->findData(int(t.kind))));
    m_address->setText(t.address);
    m_port->setValue(t.port);
    if (host.isEmpty()) m_address->selectAll();
    syncKind();
}

void TargetDialog::syncKind()
{
    const ProbeKind k = ProbeKind(m_kind->currentData().toInt());
    m_port->setEnabled(k == ProbeKind::Tcp || k == ProbeKind::Sip);
    if (k == ProbeKind::Sip && m_port->value() == 0) m_port->setValue(5060);
    m_httpBox->setVisible(k == ProbeKind::Http);
    m_size->setEnabled(k == ProbeKind::Icmp);
    m_ttl->setEnabled(k == ProbeKind::Icmp);
    m_df->setEnabled(k == ProbeKind::Icmp);
    adjustSize();
}

void TargetDialog::accept()
{
    TargetConfig t = m_t;
    if (t.id.isEmpty()) t.id = newTargetId();
    t.name = m_name->text().trimmed();
    t.address = m_address->text().trimmed();
    t.kind = ProbeKind(m_kind->currentData().toInt());
    t.port = m_port->isEnabled() ? m_port->value() : 0;
    t.group = m_group->currentText().trimmed();
    t.parentId = m_parent->currentData().toString();
    t.intervalMs = qRound(m_interval->value() * 1000.0);
    t.timeoutMs = m_timeout->value();
    t.packetSize = m_size->value();
    t.ttl = m_ttl->value();
    t.dontFragment = m_df->isChecked();
    t.latencyWarnMs = m_latency->value();
    t.lossWarnPct = m_loss->value() < 0 ? -1.0 : m_loss->value();
    t.downAfter = m_downAfter->value();
    t.upAfter = m_upAfter->value();
    t.httpExpect = m_httpCode->value();
    t.httpMatch = m_httpMatch->text();
    t.tlsInsecure = m_insecure->isChecked();
    t.tlsPin = m_pin->text().trimmed().remove(QLatin1Char(':')).toLower();
    t.notes = m_notes->toPlainText().trimmed();

    QString err;
    if (t.address.isEmpty()) err = QStringLiteral("Укажите адрес");
    else if (t.kind == ProbeKind::Http) {
        const QUrl u(t.address);
        if (!u.isValid() || u.host().isEmpty() || (u.scheme() != QLatin1String("http") && u.scheme() != QLatin1String("https")))
            err = QStringLiteral("Для HTTP нужен адрес вида http://хост/путь или https://…");
    } else {
        if (!isValidHostName(t.address)) err = QStringLiteral("Адрес не похож на IP или имя хоста");
        else if (t.kind == ProbeKind::Tcp && t.port == 0) err = QStringLiteral("Для TCP укажите порт");
    }
    if (!err.isEmpty()) {
        m_error->setText(err);
        m_error->show();
        return;
    }
    m_t = t;
    QDialog::accept();
}

// ===========================================================================
// Настройки
// ===========================================================================

SettingsDialog::SettingsDialog(Engine *engine, QWidget *parent)
    : QDialog(parent)
    , m_engine(engine)
    , m_s(engine->settings())
{
    setWindowTitle(QStringLiteral("Настройки"));
    setMinimumSize(640, 520);
    auto *root = new QVBoxLayout(this);
    m_tabs = new QTabWidget;
    root->addWidget(m_tabs, 1);

    // --- проверки
    {
        auto *w = new QWidget;
        auto *f = new QFormLayout(w);
        f->setLabelAlignment(Qt::AlignRight);
        m_interval = new QDoubleSpinBox;
        m_interval->setRange(0.1, 86400);
        m_interval->setDecimals(1);
        m_interval->setValue(m_s.intervalMs / 1000.0);
        m_interval->setSuffix(QStringLiteral(" с"));
        f->addRow(QStringLiteral("Интервал замеров"), m_interval);
        m_timeout = spin(50, 60000, m_s.timeoutMs, QStringLiteral(" мс"));
        f->addRow(QStringLiteral("Таймаут"), m_timeout);
        m_size = spin(0, 65000, m_s.packetSize, QStringLiteral(" байт"));
        f->addRow(QStringLiteral("Размер ICMP"), m_size);
        m_ttl = spin(1, 255, m_s.ttl);
        f->addRow(QStringLiteral("TTL"), m_ttl);
        m_downAfter = spin(1, 100, m_s.downAfter);
        f->addRow(QStringLiteral("Неудач до «нет связи»"), m_downAfter);
        m_upAfter = spin(1, 100, m_s.upAfter);
        f->addRow(QStringLiteral("Ответов до «в норме»"), m_upAfter);
        m_latency = spin(0, 60000, m_s.latencyWarnMs, QStringLiteral(" мс"), QStringLiteral("не следить"));
        f->addRow(QStringLiteral("«Плохо» при задержке от"), m_latency);
        auto *dyn = new QHBoxLayout;
        m_latDynamic = new QCheckBox(QStringLiteral("выше лучшего замера на"));
        m_latDynamic->setChecked(m_s.latencyDynamic);
        m_latDelta = spin(1, 60000, m_s.latencyDynamicDeltaMs, QStringLiteral(" мс"));
        dyn->addWidget(m_latDynamic);
        dyn->addWidget(m_latDelta);
        dyn->addStretch();
        f->addRow(QStringLiteral("…или"), dyn);
        m_loss = new QDoubleSpinBox;
        m_loss->setRange(0, 100);
        m_loss->setDecimals(1);
        m_loss->setValue(m_s.lossWarnPct);
        m_loss->setSuffix(QStringLiteral(" %"));
        m_loss->setSpecialValueText(QStringLiteral("не следить"));
        f->addRow(QStringLiteral("«Плохо» при потерях от"), m_loss);
        m_window = spin(5, 600, m_s.statsWindow, QStringLiteral(" замеров"));
        f->addRow(QStringLiteral("Окно потерь и джиттера"), m_window);
        auto *flap = new QHBoxLayout;
        m_flapN = spin(2, 100, m_s.flapChanges, QStringLiteral(" смен"));
        m_flapMin = spin(1, 1440, m_s.flapWindowSec / 60, QStringLiteral(" мин"));
        flap->addWidget(m_flapN);
        flap->addWidget(new QLabel(QStringLiteral("за")));
        flap->addWidget(m_flapMin);
        flap->addStretch();
        f->addRow(QStringLiteral("Дребезг"), flap);
        f->addRow(hint(QStringLiteral("Эти значения берут все цели, у которых в карточке стоит «общий». "
                                      "По умолчанию как в Инее: «нет связи» после трёх неудач подряд, возврат после двух ответов.")));
        m_tabs->addTab(w, QStringLiteral("Проверки"));
    }
    // --- оповещения
    {
        auto *w = new QWidget;
        auto *v = new QVBoxLayout(w);
        m_aTray = new QCheckBox(QStringLiteral("Уведомление в трее"));
        m_aToast = new QCheckBox(QStringLiteral("Сообщения внутри окна"));
        m_aCorner = new QCheckBox(QStringLiteral("Окошко в углу экрана поверх всех окон"));
        m_aSound = new QCheckBox(QStringLiteral("Звук"));
        m_aDegraded = new QCheckBox(QStringLiteral("Сообщать и о «Плохо» (задержка, потери)"));
        m_aRecover = new QCheckBox(QStringLiteral("Сообщать о восстановлении"));
        m_aFlap = new QCheckBox(QStringLiteral("Молчать о целях с дребезгом"));
        m_aTray->setChecked(m_s.alertTray);
        m_aToast->setChecked(m_s.alertToast);
        m_aCorner->setChecked(m_s.alertCorner);
        m_aSound->setChecked(m_s.alertSound);
        m_aDegraded->setChecked(m_s.alertOnDegraded);
        m_aRecover->setChecked(m_s.alertOnRecover);
        m_aFlap->setChecked(m_s.alertQuietFlapping);
        for (QCheckBox *c : {m_aTray, m_aToast, m_aCorner, m_aSound, m_aDegraded, m_aRecover, m_aFlap}) v->addWidget(c);
        auto *f = new QFormLayout;
        m_minDown = spin(0, 86400, m_s.alertMinDownSec, QStringLiteral(" с"), QStringLiteral("сразу"));
        f->addRow(QStringLiteral("Сообщать, если лежит дольше"), m_minDown);
        m_cmd = new QLineEdit(m_s.alertCommand);
        m_cmd->setPlaceholderText(QStringLiteral("например: /usr/local/bin/notify.sh {status} {name} {address}"));
        f->addRow(QStringLiteral("Выполнить команду"), m_cmd);
        m_cmdShell = new QCheckBox(QStringLiteral("через /bin/sh -c"));
        m_cmdShell->setChecked(m_s.alertCommandShell);
        m_cmdShell->setToolTip(QStringLiteral("Только доверенный скрипт. Данные целей передаются через ZARNITSA_*, а не подставляются в текст shell."));
        f->addRow(QString(), m_cmdShell);
        m_webhook = new QLineEdit(m_s.webhookUrl);
        m_webhook->setPlaceholderText(QStringLiteral("https://сервер/hook — POST JSON при каждом оповещении"));
        f->addRow(QStringLiteral("Вебхук"), m_webhook);
        v->addLayout(f);
        v->addWidget(hint(QStringLiteral("Без shell: {name} {address} {group} {status} {status_title} {prev} {rtt} {loss} {reason} {text} {time} "
                                         "подставляются в отдельные аргументы команды. Те же данные есть в переменных ZARNITSA_*. "
                                         "В режиме /bin/sh подстановок нет: используйте, например, logger -- \"$ZARNITSA_TEXT\". "
                                         "Почту можно отправлять своим доверенным скриптом через установленный mail/sendmail.")));
        v->addStretch();
        m_tabs->addTab(w, QStringLiteral("Оповещения"));
    }
    // --- ICMP
    {
        auto *w = new QWidget;
        auto *v = new QVBoxLayout(w);
        auto *f = new QFormLayout;
        m_icmpMode = new QComboBox;
        m_icmpMode->addItem(QStringLiteral("Сам выберу лучший путь"), QStringLiteral("auto"));
        m_icmpMode->addItem(QStringLiteral("ICMP-сокет в программе"), QStringLiteral("socket"));
        m_icmpMode->addItem(QStringLiteral("Помощник zarnitsa-icmp"), QStringLiteral("helper"));
        m_icmpMode->addItem(QStringLiteral("Системный ping"), QStringLiteral("ping"));
        m_icmpMode->setCurrentIndex(qMax(0, m_icmpMode->findData(m_s.icmpMode)));
        f->addRow(QStringLiteral("Путь"), m_icmpMode);
        m_helper = new QLineEdit(m_s.helperPath);
        m_helper->setPlaceholderText(QStringLiteral("рядом с программой или /opt/zarnitsa/bin/zarnitsa-icmp"));
        f->addRow(QStringLiteral("Помощник"), m_helper);
        m_pingPar = spin(1, 1024, m_s.pingMaxParallel, QStringLiteral(" процессов"));
        f->addRow(QStringLiteral("Системный ping: параллельно"), m_pingPar);
        v->addLayout(f);
        m_icmpInfo = new QLabel;
        m_icmpInfo->setWordWrap(true);
        m_icmpInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
        v->addWidget(m_icmpInfo);
        v->addWidget(hint(QStringLiteral(
            "На Astra обычному пользователю ICMP-сокеты закрыты (net.ipv4.ping_group_range = 1 0), а права cap_net_raw "
            "есть только у /usr/bin/ping. Поэтому без установки Зарница пингует через системный ping — это работает, "
            "но на сотнях адресов грузит процессор. Быстрый путь: sudo ./install.sh выдаст помощнику zarnitsa-icmp право "
            "cap_net_raw. Самому окну права не даются: помощник открывает сокет и сразу их сбрасывает.")));
        v->addStretch();
        connect(m_engine->icmp(), &IcmpEngine::modeChanged, this, &SettingsDialog::updateIcmpInfo);
        updateIcmpInfo();
        m_tabs->addTab(w, QStringLiteral("ICMP"));
    }
    // --- API
    {
        auto *w = new QWidget;
        auto *v = new QVBoxLayout(w);
        m_apiOn = new QCheckBox(QStringLiteral("Включить API для Инея, Росы и своих скриптов"));
        m_apiOn->setChecked(m_s.apiEnabled);
        v->addWidget(m_apiOn);
        auto *f = new QFormLayout;
        m_apiListen = new QLineEdit(m_s.apiListen);
        f->addRow(QStringLiteral("Адрес"), m_apiListen);
        auto *tok = new QHBoxLayout;
        m_apiToken = new QLineEdit(m_s.apiToken);
        m_apiToken->setReadOnly(true);
        m_apiToken->setFont(Theme::mono(12));
        m_apiToken->setPlaceholderText(QStringLiteral("появится при включении"));
        auto *copy = new QPushButton(QStringLiteral("Скопировать"));
        auto *regen = new QPushButton(QStringLiteral("Новый"));
        tok->addWidget(m_apiToken, 1);
        tok->addWidget(copy);
        tok->addWidget(regen);
        f->addRow(QStringLiteral("Токен"), tok);
        auto pick = [this](QLineEdit *e, const QString &title) {
            const QString p = QFileDialog::getOpenFileName(this, title, e->text(), QStringLiteral("PEM (*.pem *.crt *.key);;Все файлы (*)"));
            if (!p.isEmpty()) e->setText(p);
        };
        auto *certRow = new QHBoxLayout;
        m_apiCert = new QLineEdit(m_s.apiTlsCert);
        auto *cb = new QPushButton(QStringLiteral("…"));
        certRow->addWidget(m_apiCert, 1);
        certRow->addWidget(cb);
        f->addRow(QStringLiteral("Сертификат TLS"), certRow);
        auto *keyRow = new QHBoxLayout;
        m_apiKey = new QLineEdit(m_s.apiTlsKey);
        auto *kb = new QPushButton(QStringLiteral("…"));
        keyRow->addWidget(m_apiKey, 1);
        keyRow->addWidget(kb);
        f->addRow(QStringLiteral("Ключ TLS"), keyRow);
        v->addLayout(f);
        m_apiInfo = new QLabel;
        m_apiInfo->setWordWrap(true);
        m_apiInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_apiInfo->setFont(Theme::mono(11.5));
        v->addWidget(m_apiInfo);
        v->addWidget(hint(QStringLiteral("Как у Росы: вне 127.0.0.1 — только по TLS (сертификат выпустит make-cert.sh). "
                                         "Маршруты: /healthz, /api/v1/targets, /api/v1/summary, /api/v1/history, /api/v1/events (SSE), "
                                         "/api/v1/availability в формате «Виктории» Инея, /metrics для Prometheus.")));
        v->addStretch();
        connect(copy, &QPushButton::clicked, this, [this]() { QApplication::clipboard()->setText(m_apiToken->text()); });
        connect(regen, &QPushButton::clicked, this, [this]() { m_apiToken->setText(ApiController::generateToken()); });
        connect(cb, &QPushButton::clicked, this, [this, pick]() { pick(m_apiCert, QStringLiteral("Сертификат")); });
        connect(kb, &QPushButton::clicked, this, [this, pick]() { pick(m_apiKey, QStringLiteral("Ключ")); });
        connect(m_apiOn, &QCheckBox::toggled, this, [this](bool on) {
            if (on && m_apiToken->text().size() < 16) m_apiToken->setText(ApiController::generateToken());
            updateApiInfo();
        });
        connect(m_engine, &Engine::apiStateChanged, this, &SettingsDialog::updateApiInfo);
        updateApiInfo();
        m_tabs->addTab(w, QStringLiteral("API"));
    }
    // --- вид
    {
        auto *w = new QWidget;
        auto *f = new QFormLayout(w);
        m_theme = new QComboBox;
        m_theme->addItem(QStringLiteral("Тёмная"), QStringLiteral("dark"));
        m_theme->addItem(QStringLiteral("Светлая"), QStringLiteral("light"));
        m_theme->setCurrentIndex(qMax(0, m_theme->findData(m_s.theme)));
        f->addRow(QStringLiteral("Тема"), m_theme);
        m_density = new QComboBox;
        m_density->addItem(QStringLiteral("Плитки"), QStringLiteral("comfortable"));
        m_density->addItem(QStringLiteral("Компактно"), QStringLiteral("compact"));
        m_density->addItem(QStringLiteral("Таблица"), QStringLiteral("list"));
        m_density->setCurrentIndex(qMax(0, m_density->findData(m_s.density)));
        f->addRow(QStringLiteral("Вид"), m_density);
        m_columns = spin(0, 32, m_s.columns, QString(), QStringLiteral("по ширине окна"));
        f->addRow(QStringLiteral("Столбцов"), m_columns);
        m_spark = new QCheckBox(QStringLiteral("График задержки на плитках"));
        m_spark->setChecked(m_s.showSparkline);
        m_mos = new QCheckBox(QStringLiteral("Оценка качества голоса (MOS)"));
        m_mos->setChecked(m_s.showMos);
        m_onTop = new QCheckBox(QStringLiteral("Поверх всех окон"));
        m_onTop->setChecked(m_s.alwaysOnTop);
        m_minTray = new QCheckBox(QStringLiteral("Сворачивать в трей"));
        m_minTray->setChecked(m_s.minimizeToTray);
        m_closeTray = new QCheckBox(QStringLiteral("Закрытие окна прячет его в трей"));
        m_closeTray->setChecked(m_s.closeToTray);
        m_startMin = new QCheckBox(QStringLiteral("Запускать свёрнутой"));
        m_startMin->setChecked(m_s.startMinimized);
        for (QCheckBox *c : {m_spark, m_mos, m_onTop, m_minTray, m_closeTray, m_startMin}) f->addRow(QString(), c);
        m_keepMonths = spin(1, 120, m_s.eventsKeepMonths, QStringLiteral(" мес."));
        f->addRow(QStringLiteral("Хранить журнал"), m_keepMonths);
        auto *paths = new QLabel(QStringLiteral("Конфиг: %1\nДанные: %2").arg(m_engine->store().path(), m_engine->store().dataDir()));
        paths->setProperty("role", "faint");
        paths->setTextInteractionFlags(Qt::TextSelectableByMouse);
        f->addRow(paths);
        m_tabs->addTab(w, QStringLiteral("Вид"));
    }

    auto *buttons = new QDialogButtonBox;
    accentButton(buttons->addButton(QStringLiteral("Сохранить"), QDialogButtonBox::AcceptRole))->setDefault(true);
    buttons->addButton(QStringLiteral("Отмена"), QDialogButtonBox::RejectRole);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
}

void SettingsDialog::showTab(const QString &key)
{
    for (int i = 0; i < m_tabs->count(); ++i)
        if (m_tabs->tabText(i).compare(key, Qt::CaseInsensitive) == 0) m_tabs->setCurrentIndex(i);
}

void SettingsDialog::updateIcmpInfo()
{
    IcmpEngine *e = m_engine->icmp();
    const Palette &p = Theme::p();
    const QColor c = e->isFast() ? p.okText : (e->mode() == IcmpEngine::Mode::None ? p.badText : p.warnText);
    m_icmpInfo->setText(QStringLiteral("<b style='color:%1'>Сейчас: %2</b><br>%3").arg(c.name(), e->modeTitle(), e->modeHint().toHtmlEscaped()));
}

void SettingsDialog::updateApiInfo()
{
    ApiController *api = m_engine->api();
    QString text;
    if (api->isRunning()) {
        text = QStringLiteral("Работает: %1\ncurl -H 'Authorization: Bearer %2' %1/api/v1/summary")
                   .arg(api->address(), m_apiToken->text().isEmpty() ? QStringLiteral("<токен>") : m_apiToken->text());
    } else if (!m_engine->apiError().isEmpty()) {
        text = QStringLiteral("Ошибка: %1").arg(m_engine->apiError());
    } else {
        text = QStringLiteral("Выключено");
    }
    m_apiInfo->setText(text);
}

Settings SettingsDialog::result() const
{
    Settings s = m_s;
    s.intervalMs = qRound(m_interval->value() * 1000.0);
    s.timeoutMs = m_timeout->value();
    s.packetSize = m_size->value();
    s.ttl = m_ttl->value();
    s.downAfter = m_downAfter->value();
    s.upAfter = m_upAfter->value();
    s.latencyWarnMs = m_latency->value();
    s.latencyDynamic = m_latDynamic->isChecked();
    s.latencyDynamicDeltaMs = m_latDelta->value();
    s.lossWarnPct = m_loss->value();
    s.statsWindow = m_window->value();
    s.flapChanges = m_flapN->value();
    s.flapWindowSec = m_flapMin->value() * 60;
    s.alertTray = m_aTray->isChecked();
    s.alertToast = m_aToast->isChecked();
    s.alertCorner = m_aCorner->isChecked();
    s.alertSound = m_aSound->isChecked();
    s.alertOnDegraded = m_aDegraded->isChecked();
    s.alertOnRecover = m_aRecover->isChecked();
    s.alertQuietFlapping = m_aFlap->isChecked();
    s.alertMinDownSec = m_minDown->value();
    s.alertCommand = m_cmd->text().trimmed();
    s.alertCommandShell = m_cmdShell->isChecked();
    s.webhookUrl = m_webhook->text().trimmed();
    s.icmpMode = m_icmpMode->currentData().toString();
    s.helperPath = m_helper->text().trimmed();
    s.pingMaxParallel = m_pingPar->value();
    s.apiEnabled = m_apiOn->isChecked();
    s.apiListen = m_apiListen->text().trimmed();
    s.apiToken = m_apiToken->text().trimmed();
    s.apiTlsCert = m_apiCert->text().trimmed();
    s.apiTlsKey = m_apiKey->text().trimmed();
    s.theme = m_theme->currentData().toString();
    s.density = m_density->currentData().toString();
    s.columns = m_columns->value();
    s.showSparkline = m_spark->isChecked();
    s.showMos = m_mos->isChecked();
    s.alwaysOnTop = m_onTop->isChecked();
    s.minimizeToTray = m_minTray->isChecked();
    s.closeToTray = m_closeTray->isChecked();
    s.startMinimized = m_startMin->isChecked();
    s.eventsKeepMonths = m_keepMonths->value();
    return s;
}

// ===========================================================================
// О программе
// ===========================================================================

class LogoWidget : public QWidget {
public:
    explicit LogoWidget(int size, QWidget *parent = nullptr) : QWidget(parent), m_size(size) { setFixedSize(size, size); }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        Icons::paintLogo(p, QRectF(0, 0, m_size, m_size));
    }
private:
    int m_size;
};

AboutDialog::AboutDialog(Engine *engine, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("О программе"));
    setMinimumWidth(520);
    auto *root = new QVBoxLayout(this);
    auto *top = new QHBoxLayout;
    top->addWidget(new LogoWidget(84));
    auto *col = new QVBoxLayout;
    auto *title = new QLabel(QStringLiteral("Зарница %1").arg(appVersion()));
    title->setFont(Theme::ui(24, QFont::DemiBold));
    col->addWidget(title);
    col->addWidget(hint(QStringLiteral("Пинговалка для Astra Linux. Живёт сама по себе и дружит с Инеем и Росой.")));
    auto *by = new QLabel(QStringLiteral("Зарница by Стёпчик"));
    by->setProperty("role", "muted");
    col->addWidget(by);
    col->addStretch();
    top->addLayout(col, 1);
    root->addLayout(top);

    IcmpEngine *icmp = engine->icmp();
    const QString info = QStringLiteral(
        "<table cellspacing=4>"
        "<tr><td style='color:%1'>Qt</td><td>%2 (собрана с %3)</td></tr>"
        "<tr><td style='color:%1'>ICMP</td><td>%4</td></tr>"
        "<tr><td style='color:%1'>Конфиг</td><td>%5</td></tr>"
        "<tr><td style='color:%1'>Журнал</td><td>%6</td></tr>"
        "</table>")
        .arg(Theme::p().faint.name(), QString::fromLatin1(qVersion()), QStringLiteral(QT_VERSION_STR),
             icmp->modeTitle().toHtmlEscaped(), engine->store().path().toHtmlEscaped(), engine->log()->dir().toHtmlEscaped());
    auto *infoL = new QLabel(info);
    infoL->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(infoL);
    root->addWidget(hint(QStringLiteral(
        "Правообладатель — Иванцов Степан Сергеевич. Бесплатно для использования внутри локальных сетей; "
        "изменения кода согласуются с автором.\nШрифты Golos Text и JetBrains Mono — SIL Open Font License 1.1. "
        "Qt — LGPL v3, используется системная библиотека Astra.")));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("Закрыть"));
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
}

} // namespace zr
