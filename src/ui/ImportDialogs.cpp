#include "ImportDialogs.h"

#include "Theme.h"
#include "core/Monitor.h"
#include "integrations/Inei.h"
#include "integrations/Rosa.h"
#include "service/Engine.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

namespace zr {

static QPushButton *accentBtn(QPushButton *b)
{
    b->setProperty("accent", true);
    return b;
}

// ===========================================================================
// Предпросмотр файла
// ===========================================================================

ImportPreviewDialog::ImportPreviewDialog(Engine *engine, const ImportResult &r, const QString &title, QWidget *parent)
    : QDialog(parent)
    , m_engine(engine)
    , m_r(r)
{
    setWindowTitle(title);
    resize(820, 560);
    auto *root = new QVBoxLayout(this);
    m_table = new QTableWidget(r.targets.size(), 5);
    m_table->setHorizontalHeaderLabels({QString(), QStringLiteral("Название"), QStringLiteral("Адрес"),
                                        QStringLiteral("Проверка"), QStringLiteral("Группа в файле")});
    m_table->verticalHeader()->hide();
    m_table->setShowGrid(false);
    m_table->setAlternatingRowColors(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->setColumnWidth(0, 32);
    m_table->setColumnWidth(2, 220);
    m_table->setColumnWidth(3, 90);
    m_table->setColumnWidth(4, 170);
    for (int i = 0; i < r.targets.size(); ++i) {
        const TargetConfig &t = r.targets[i];
        auto *chk = new QTableWidgetItem;
        chk->setCheckState(Qt::Checked);
        m_table->setItem(i, 0, chk);
        m_table->setItem(i, 1, new QTableWidgetItem(t.displayName()));
        auto *addr = new QTableWidgetItem(t.publicEndpointText());
        addr->setFont(Theme::mono(12));
        m_table->setItem(i, 2, addr);
        m_table->setItem(i, 3, new QTableWidgetItem(kindTitle(t.kind)));
        m_table->setItem(i, 4, new QTableWidgetItem(t.group.isEmpty() ? QStringLiteral("—") : t.group));
        for (int c = 1; c < 5; ++c) m_table->item(i, c)->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    }
    root->addWidget(m_table, 1);
    if (!r.warnings.isEmpty()) {
        auto *w = new QLabel(QStringLiteral("Пропущено %1: %2").arg(r.warnings.size()).arg(r.warnings.mid(0, 6).join(QStringLiteral("; "))
                                                                                        + (r.warnings.size() > 6 ? QStringLiteral("…") : QString())));
        w->setWordWrap(true);
        w->setStyleSheet(QStringLiteral("color:%1;").arg(Theme::p().warnText.name()));
        root->addWidget(w);
    }
    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(QStringLiteral("Куда:")));
    m_group = new QComboBox;
    m_group->setEditable(true);
    m_group->addItem(QStringLiteral("— группы из файла"), QStringLiteral("\x01"));
    for (const GroupInfo &g : engine->monitor()->groups())
        if (!g.name.isEmpty()) m_group->addItem(g.name, g.name);
    m_group->setMinimumWidth(240);
    row->addWidget(m_group);
    auto *all = new QPushButton(QStringLiteral("Все"));
    auto *none = new QPushButton(QStringLiteral("Ничего"));
    row->addWidget(all);
    row->addWidget(none);
    row->addStretch();
    m_count = new QLabel;
    m_count->setProperty("role", "muted");
    row->addWidget(m_count);
    root->addLayout(row);
    auto *buttons = new QDialogButtonBox;
    auto *ok = accentBtn(buttons->addButton(QStringLiteral("Добавить"), QDialogButtonBox::AcceptRole));
    buttons->addButton(QStringLiteral("Отмена"), QDialogButtonBox::RejectRole);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
    auto update = [this, ok]() {
        int n = 0;
        for (int i = 0; i < m_table->rowCount(); ++i) if (m_table->item(i, 0)->checkState() == Qt::Checked) ++n;
        m_count->setText(QStringLiteral("выбрано %1 из %2").arg(n).arg(m_table->rowCount()));
        ok->setText(QStringLiteral("Добавить %1").arg(n));
        ok->setEnabled(n > 0);
    };
    connect(m_table, &QTableWidget::itemChanged, this, update);
    connect(all, &QPushButton::clicked, this, [this]() { for (int i = 0; i < m_table->rowCount(); ++i) m_table->item(i, 0)->setCheckState(Qt::Checked); });
    connect(none, &QPushButton::clicked, this, [this]() { for (int i = 0; i < m_table->rowCount(); ++i) m_table->item(i, 0)->setCheckState(Qt::Unchecked); });
    update();
}

QVector<TargetConfig> ImportPreviewDialog::selected() const
{
    QVector<TargetConfig> out;
    const bool keepFileGroups = m_group->currentIndex() == 0 && m_group->currentText() == m_group->itemText(0);
    const QString g = m_group->currentText().trimmed();
    for (int i = 0; i < m_table->rowCount(); ++i) {
        if (m_table->item(i, 0)->checkState() != Qt::Checked) continue;
        TargetConfig t = m_r.targets[i];
        if (!keepFileGroups) t.group = g;
        out.append(t);
    }
    return out;
}

// ===========================================================================
// Источник Инея / Росы
// ===========================================================================

SourceDialog::SourceDialog(Engine *engine, const SourceConfig &src, QWidget *parent)
    : QDialog(parent)
    , m_engine(engine)
    , m_src(src)
    , m_rosa(src.kind == QLatin1String("rosa"))
{
    setWindowTitle(m_rosa ? QStringLiteral("Абоненты из Росы") : QStringLiteral("Телефоны из Инея"));
    setMinimumWidth(560);
    auto *root = new QVBoxLayout(this);
    auto *intro = new QLabel(m_rosa
        ? QStringLiteral("Зарница войдёт в Росу как администратор, возьмёт адреса зарегистрированных SIP-абонентов и терминала H.323 "
                         "и добавит проверку самого сервера. В Росе ничего не меняется.")
        : QStringLiteral("Зарница войдёт в панель Инея, прочитает карточки телефонов (номер, имя, IP) и будет за ними следить. "
                         "В Инее ничего не меняется."));
    intro->setWordWrap(true);
    intro->setProperty("role", "muted");
    root->addWidget(intro);

    auto *f = new QFormLayout;
    f->setLabelAlignment(Qt::AlignRight);
    m_title = new QLineEdit(src.title.isEmpty() ? (m_rosa ? QStringLiteral("Роса") : QStringLiteral("Иней")) : src.title);
    f->addRow(QStringLiteral("Название"), m_title);
    m_url = new QLineEdit(src.url);
    m_url->setPlaceholderText(m_rosa ? QStringLiteral("https://192.0.2.20:18443") : QStringLiteral("http://10.0.0.5:8000"));
    f->addRow(QStringLiteral("Адрес"), m_url);
    m_user = new QLineEdit(src.username.isEmpty() ? QStringLiteral("admin") : src.username);
    f->addRow(QStringLiteral("Логин"), m_user);
    m_pass = new QLineEdit(src.password);
    m_pass->setEchoMode(QLineEdit::Password);
    f->addRow(QStringLiteral("Пароль"), m_pass);
    m_remember = new QCheckBox(QStringLiteral("Запомнить пароль (файл с правами 0600) — нужно для синхронизации по расписанию"));
    m_remember->setChecked(src.remember);
    f->addRow(QString(), m_remember);
    m_group = new QComboBox;
    m_group->setEditable(true);
    m_group->setCurrentText(src.group.isEmpty() ? m_title->text() : src.group);
    for (const GroupInfo &g : engine->monitor()->groups())
        if (!g.name.isEmpty()) m_group->addItem(g.name);
    m_group->setCurrentText(src.group.isEmpty() ? m_title->text() : src.group);
    f->addRow(QStringLiteral("Группа"), m_group);
    m_mode = new QComboBox;
    if (m_rosa) {
        m_mode->addItem(QStringLiteral("Пинговать абонентов (ICMP)"), QStringLiteral("icmp"));
        m_mode->addItem(QStringLiteral("Проверять службу: SIP OPTIONS и H.323 TCP 1720"), QStringLiteral("sip"));
    } else {
        m_mode->addItem(QStringLiteral("Пинговать телефоны (ICMP)"), QStringLiteral("icmp"));
        m_mode->addItem(QStringLiteral("Спрашивать SIP OPTIONS на 5060"), QStringLiteral("sip"));
    }
    m_mode->setCurrentIndex(qMax(0, m_mode->findData(src.mode)));
    f->addRow(QStringLiteral("Как проверять"), m_mode);
    m_shared = new QCheckBox(QStringLiteral("Брать и общие наборы других пользователей"));
    m_shared->setChecked(src.includeShared);
    m_sip = new QCheckBox(QStringLiteral("Зарегистрированные SIP-абоненты"));
    m_sip->setChecked(src.rosaSip);
    m_h323 = new QCheckBox(QStringLiteral("Терминал H.323"));
    m_h323->setChecked(src.rosaH323);
    m_server = new QCheckBox(QStringLiteral("Сам сервер Росы: /healthz и SIP"));
    m_server->setChecked(src.rosaServer);
    if (m_rosa) {
        f->addRow(QString(), m_sip);
        f->addRow(QString(), m_h323);
        f->addRow(QString(), m_server);
        auto *pinRow = new QHBoxLayout;
        m_pin = new QLineEdit(src.tlsPin);
        m_pin->setFont(Theme::mono(11.5));
        m_pin->setPlaceholderText(QStringLiteral("заполнится после проверки сертификата"));
        m_certBtn = new QPushButton(QStringLiteral("Проверить сертификат"));
        pinRow->addWidget(m_pin, 1);
        pinRow->addWidget(m_certBtn);
        f->addRow(QStringLiteral("Отпечаток TLS"), pinRow);
        connect(m_certBtn, &QPushButton::clicked, this, &SourceDialog::probeCert);
    } else {
        f->addRow(QString(), m_shared);
        m_pin = nullptr;
    }
    m_sync = new QSpinBox;
    m_sync->setRange(0, 10080);
    m_sync->setValue(src.syncMinutes);
    m_sync->setSuffix(QStringLiteral(" мин"));
    m_sync->setSpecialValueText(QStringLiteral("только по кнопке"));
    f->addRow(QStringLiteral("Обновлять каждые"), m_sync);
    root->addLayout(f);

    if (!m_rosa) {
        auto *warn = new QLabel(QStringLiteral("Иней считает неверные пароли: после 7 неудач вход с этого адреса закрывается на 15 минут. "
                                               "После ошибки входа автосинхронизация останавливается до ручной попытки или изменения настроек. "
                                               "При HTTP пароль и cookie идут без шифрования: подключайтесь только в доверенной сети."));
        warn->setWordWrap(true);
        warn->setStyleSheet(QStringLiteral("color:%1;").arg(Theme::p().warnText.name()));
        root->addWidget(warn);
    }
    m_status = new QLabel;
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    root->addWidget(m_status);
    auto *buttons = new QDialogButtonBox;
    accentBtn(buttons->addButton(QStringLiteral("Сохранить и загрузить"), QDialogButtonBox::AcceptRole))->setDefault(true);
    buttons->addButton(QStringLiteral("Отмена"), QDialogButtonBox::RejectRole);
    connect(buttons, &QDialogButtonBox::accepted, this, &SourceDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
}

QString SourceDialog::passwordOnce() const
{
    return m_pass->text();
}

void SourceDialog::collect()
{
    m_src.kind = m_rosa ? QStringLiteral("rosa") : QStringLiteral("inei");
    m_src.title = m_title->text().trimmed();
    m_src.url = m_url->text().trimmed();
    m_src.username = m_user->text().trimmed();
    m_src.remember = m_remember->isChecked();
    m_src.password = m_remember->isChecked() ? m_pass->text() : QString();
    m_src.group = m_group->currentText().trimmed();
    m_src.mode = m_mode->currentData().toString();
    m_src.includeShared = m_shared->isChecked();
    m_src.rosaSip = m_sip->isChecked();
    m_src.rosaH323 = m_h323->isChecked();
    m_src.rosaServer = m_server->isChecked();
    if (m_pin) m_src.tlsPin = m_pin->text().trimmed().remove(QLatin1Char(':')).toLower();
    m_src.syncMinutes = m_sync->value();
}

void SourceDialog::probeCert()
{
    const QUrl url = RosaClient::normalizeUrl(m_url->text());
    if (!url.isValid() || url.host().isEmpty()) {
        m_status->setText(QStringLiteral("Укажите адрес Росы"));
        return;
    }
    if (!m_rosaClient) {
        m_rosaClient = new RosaClient(this);
        connect(m_rosaClient, &RosaClient::certificate, this,
                [this](const QString &sha, const QString &subject, const QDateTime &expiry, bool trusted, const QString &error) {
                    m_certBtn->setEnabled(true);
                    if (!error.isEmpty()) {
                        m_status->setText(QStringLiteral("Не удалось получить сертификат: %1").arg(error));
                        return;
                    }
                    const QString text = QStringLiteral("Сертификат «%1», действует до %2.%3\n\nSHA-256:\n%4\n\nСверьте отпечаток с сервером "
                                                        "(openssl x509 -noout -fingerprint -sha256 -in cert.pem) и подтвердите.")
                                             .arg(subject.isEmpty() ? QStringLiteral("без имени") : subject,
                                                  expiry.toString(QStringLiteral("dd.MM.yyyy")),
                                                  trusted ? QStringLiteral(" Системе он известен.") : QStringLiteral(" Сертификат самоподписанный."),
                                                  sha);
                    if (QMessageBox::question(this, QStringLiteral("Доверять сертификату Росы?"), text) == QMessageBox::Yes) {
                        m_pin->setText(sha);
                        m_status->setText(QStringLiteral("Отпечаток закреплён: Зарница будет говорить только с этим сервером."));
                    }
                });
    }
    m_certBtn->setEnabled(false);
    m_status->setText(QStringLiteral("Подключаюсь…"));
    m_rosaClient->probeCertificate(url);
}

void SourceDialog::accept()
{
    collect();
    if (m_src.url.isEmpty() || m_src.username.isEmpty() || m_pass->text().isEmpty()) {
        m_status->setText(QStringLiteral("Нужны адрес, логин и пароль"));
        return;
    }
    if (m_rosa && m_src.tlsPin.isEmpty() && RosaClient::normalizeUrl(m_src.url).scheme() == QLatin1String("https")) {
        m_status->setText(QStringLiteral("Сначала проверьте и закрепите сертификат Росы"));
        probeCert();
        return;
    }
    QDialog::accept();
}

void runSourceWizard(Engine *engine, const SourceConfig &src, QWidget *parent)
{
    SourceDialog dlg(engine, src, parent);
    if (dlg.exec() != QDialog::Accepted)
        return;
    SourceConfig s = dlg.result();
    engine->upsertSource(s);
    if (s.id.isEmpty())
        s.id = engine->config().sources.last().id;
    const QString id = s.id;
    auto *conn = new QMetaObject::Connection;
    *conn = QObject::connect(engine, &Engine::sourceSynced, parent, [conn, id, parent, engine](const QString &sid, bool ok, const QString &report) {
        if (sid != id) return;
        QObject::disconnect(*conn);
        delete conn;
        const SourceConfig *src2 = engine->source(id);
        const QString title = src2 ? src2->title : QStringLiteral("Источник");
        if (ok) QMessageBox::information(parent, title, QStringLiteral("Готово: %1").arg(report));
        else QMessageBox::warning(parent, title, report);
    });
    engine->syncSource(id, dlg.passwordOnce());
}

// ===========================================================================
// Список источников
// ===========================================================================

SourcesDialog::SourcesDialog(Engine *engine, QWidget *parent)
    : QDialog(parent)
    , m_engine(engine)
{
    setWindowTitle(QStringLiteral("Источники — Иней и Роса"));
    resize(900, 420);
    auto *root = new QVBoxLayout(this);
    m_table = new QTableWidget(0, 6);
    m_table->setHorizontalHeaderLabels({QStringLiteral("Название"), QStringLiteral("Откуда"), QStringLiteral("Адрес"),
                                        QStringLiteral("Группа"), QStringLiteral("Обновлено"), QStringLiteral("Итог")});
    m_table->verticalHeader()->hide();
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setShowGrid(false);
    m_table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    m_table->setColumnWidth(0, 140);
    m_table->setColumnWidth(1, 70);
    m_table->setColumnWidth(2, 220);
    m_table->setColumnWidth(3, 130);
    m_table->setColumnWidth(4, 120);
    root->addWidget(m_table, 1);
    m_status = new QLabel;
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    root->addWidget(m_status);
    auto *row = new QHBoxLayout;
    auto *addInei = new QPushButton(QStringLiteral("+ Иней…"));
    auto *addRosa = new QPushButton(QStringLiteral("+ Роса…"));
    auto *editBtn = new QPushButton(QStringLiteral("Изменить…"));
    auto *sync = accentBtn(new QPushButton(QStringLiteral("Обновить сейчас")));
    auto *del = new QPushButton(QStringLiteral("Удалить"));
    auto *close = new QPushButton(QStringLiteral("Закрыть"));
    for (QPushButton *b : {addInei, addRosa, editBtn, sync, del}) row->addWidget(b);
    row->addStretch();
    row->addWidget(close);
    root->addLayout(row);

    connect(addInei, &QPushButton::clicked, this, [this]() { SourceConfig s; s.kind = QStringLiteral("inei"); edit(s); });
    connect(addRosa, &QPushButton::clicked, this, [this]() { SourceConfig s; s.kind = QStringLiteral("rosa"); edit(s); });
    connect(editBtn, &QPushButton::clicked, this, [this]() {
        if (const SourceConfig *s = m_engine->source(currentId())) this->edit(*s);
    });
    connect(sync, &QPushButton::clicked, this, [this]() {
        const SourceConfig *s = m_engine->source(currentId());
        if (!s) return;
        if (s->password.isEmpty()) { this->edit(*s); return; }
        m_status->setText(QStringLiteral("Обновляю «%1»…").arg(s->title));
        m_engine->syncSource(s->id);
    });
    connect(del, &QPushButton::clicked, this, [this]() {
        const SourceConfig *s = m_engine->source(currentId());
        if (!s) return;
        QMessageBox box(QMessageBox::Question, QStringLiteral("Удалить источник"),
                        QStringLiteral("Удалить «%1»? Цели из него можно оставить как обычные.").arg(s->title), QMessageBox::NoButton, this);
        auto *keep = box.addButton(QStringLiteral("Удалить, цели оставить"), QMessageBox::AcceptRole);
        auto *drop = box.addButton(QStringLiteral("Удалить вместе с целями"), QMessageBox::DestructiveRole);
        box.addButton(QStringLiteral("Отмена"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() == keep) m_engine->removeSource(s->id, false);
        else if (box.clickedButton() == drop) m_engine->removeSource(s->id, true);
        reload();
    });
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_engine, &Engine::sourceSynced, this, [this](const QString &, bool ok, const QString &report) {
        m_status->setText(ok ? QStringLiteral("Готово: %1").arg(report) : QStringLiteral("Ошибка: %1").arg(report));
        m_status->setStyleSheet(QStringLiteral("color:%1;").arg((ok ? Theme::p().okText : Theme::p().badText).name()));
        reload();
    });
    reload();
}

QString SourcesDialog::currentId() const
{
    const int r = m_table->currentRow();
    return r >= 0 && m_table->item(r, 0) ? m_table->item(r, 0)->data(Qt::UserRole).toString() : QString();
}

void SourcesDialog::edit(const SourceConfig &src)
{
    runSourceWizard(m_engine, src, this);
    reload();
}

void SourcesDialog::reload()
{
    const QString keep = currentId();
    const QVector<SourceConfig> &list = m_engine->config().sources;
    m_table->setRowCount(list.size());
    for (int i = 0; i < list.size(); ++i) {
        const SourceConfig &s = list[i];
        auto *name = new QTableWidgetItem(s.title);
        name->setData(Qt::UserRole, s.id);
        m_table->setItem(i, 0, name);
        m_table->setItem(i, 1, new QTableWidgetItem(s.kind == QLatin1String("rosa") ? QStringLiteral("Роса") : QStringLiteral("Иней")));
        m_table->setItem(i, 2, new QTableWidgetItem(s.url));
        m_table->setItem(i, 3, new QTableWidgetItem(s.group));
        m_table->setItem(i, 4, new QTableWidgetItem(s.lastSync ? QDateTime::fromMSecsSinceEpoch(s.lastSync).toString(QStringLiteral("dd.MM HH:mm")) : QStringLiteral("—")));
        m_table->setItem(i, 5, new QTableWidgetItem(s.lastResult));
        if (s.id == keep) m_table->selectRow(i);
    }
    if (m_table->currentRow() < 0 && !list.isEmpty()) m_table->selectRow(0);
}

} // namespace zr
