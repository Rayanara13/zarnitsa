// Диалоги: цель (добавить/изменить), настройки, «О программе».
#pragma once

#include "core/Settings.h"
#include "core/Target.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QGroupBox;
class QTabWidget;

namespace zr {

class Engine;

class TargetDialog : public QDialog {
    Q_OBJECT
public:
    TargetDialog(Engine *engine, const TargetConfig &t, bool isNew, QWidget *parent = nullptr);
    TargetConfig result() const { return m_t; }

private:
    void applyPreset(int index);
    void syncKind();
    void accept() override;

    Engine *m_engine;
    TargetConfig m_t;
    bool m_new;
    QComboBox *m_preset;
    QLineEdit *m_name;
    QLineEdit *m_address;
    QComboBox *m_kind;
    QSpinBox *m_port;
    QComboBox *m_group;
    QComboBox *m_parent;
    QDoubleSpinBox *m_interval;
    QSpinBox *m_timeout;
    QSpinBox *m_size;
    QSpinBox *m_ttl;
    QCheckBox *m_df;
    QSpinBox *m_latency;
    QDoubleSpinBox *m_loss;
    QSpinBox *m_downAfter;
    QSpinBox *m_upAfter;
    QGroupBox *m_httpBox;
    QGroupBox *m_icmpBox;
    QSpinBox *m_httpCode;
    QLineEdit *m_httpMatch;
    QCheckBox *m_insecure;
    QLineEdit *m_pin;
    QPlainTextEdit *m_notes;
    QLabel *m_error;
};

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    SettingsDialog(Engine *engine, QWidget *parent = nullptr);
    Settings result() const;
    void showTab(const QString &key);

private:
    void updateApiInfo();
    void updateIcmpInfo();

    Engine *m_engine;
    Settings m_s;
    QTabWidget *m_tabs;
    // проверки
    QDoubleSpinBox *m_interval;
    QSpinBox *m_timeout, *m_size, *m_ttl, *m_downAfter, *m_upAfter, *m_latency, *m_latDelta, *m_window, *m_flapN, *m_flapMin;
    QCheckBox *m_latDynamic;
    QDoubleSpinBox *m_loss;
    // оповещения
    QCheckBox *m_aTray, *m_aToast, *m_aCorner, *m_aSound, *m_aDegraded, *m_aRecover, *m_aFlap, *m_cmdShell;
    QSpinBox *m_minDown;
    QLineEdit *m_cmd, *m_webhook;
    // ICMP
    QComboBox *m_icmpMode;
    QLineEdit *m_helper;
    QSpinBox *m_pingPar;
    QLabel *m_icmpInfo;
    // API
    QCheckBox *m_apiOn;
    QLineEdit *m_apiListen, *m_apiToken, *m_apiCert, *m_apiKey;
    QLabel *m_apiInfo;
    // вид
    QComboBox *m_theme, *m_density;
    QSpinBox *m_columns;
    QCheckBox *m_spark, *m_mos, *m_onTop, *m_minTray, *m_closeTray, *m_startMin;
    QSpinBox *m_keepMonths;
};

class AboutDialog : public QDialog {
    Q_OBJECT
public:
    AboutDialog(Engine *engine, QWidget *parent = nullptr);
};

} // namespace zr
