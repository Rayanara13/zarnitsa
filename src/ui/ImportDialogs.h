// Импорт: предпросмотр списка из файла, источники Инея и Росы (по сети, с синхронизацией).
#pragma once

#include "core/Config.h"
#include "integrations/Importers.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace zr {

class Engine;
class RosaClient;

class ImportPreviewDialog : public QDialog {
    Q_OBJECT
public:
    ImportPreviewDialog(Engine *engine, const ImportResult &r, const QString &title, QWidget *parent = nullptr);
    QVector<TargetConfig> selected() const;

private:
    Engine *m_engine;
    ImportResult m_r;
    QTableWidget *m_table;
    QComboBox *m_group;
    QLabel *m_count;
};

class SourceDialog : public QDialog {
    Q_OBJECT
public:
    SourceDialog(Engine *engine, const SourceConfig &src, QWidget *parent = nullptr);
    SourceConfig result() const { return m_src; }
    QString passwordOnce() const;

private:
    void probeCert();
    void accept() override;
    void collect();

    Engine *m_engine;
    SourceConfig m_src;
    bool m_rosa;
    QLineEdit *m_title, *m_url, *m_user, *m_pass, *m_pin;
    QCheckBox *m_remember, *m_shared, *m_sip, *m_h323, *m_server;
    QComboBox *m_group, *m_mode;
    QSpinBox *m_sync;
    QLabel *m_status;
    QPushButton *m_certBtn = nullptr;
    RosaClient *m_rosaClient = nullptr;
};

class SourcesDialog : public QDialog {
    Q_OBJECT
public:
    SourcesDialog(Engine *engine, QWidget *parent = nullptr);

private:
    void reload();
    QString currentId() const;
    void edit(const SourceConfig &src);

    Engine *m_engine;
    QTableWidget *m_table;
    QLabel *m_status;
};

// Открыть диалог источника, сохранить и сразу синхронизировать
void runSourceWizard(Engine *engine, const SourceConfig &src, QWidget *parent);

} // namespace zr
