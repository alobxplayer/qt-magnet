#pragma once
#include <QDialog>
#include <memory>
#include <atomic>

class QLineEdit;
class QSpinBox;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class Config;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(Config &cfg, QWidget *parent = nullptr);
    ~SettingsDialog() override;

protected:
    void changeEvent(QEvent *e) override;

private slots:
    void onSave();
    void testConnection();
    void assignMagnetHandler();
    void removeMagnetHandler();
    void assignTorrentHandler();
    void removeTorrentHandler();

private:
    void buildUi();
    void loadValues();
    void refreshHandlerStatus();
    QWidget *buildConnectionGroup();
    QWidget *buildHandlerGroup();
    QWidget *buildBehaviorGroup();

    Config &_cfg;

    QComboBox *_clientType = nullptr;
    QLineEdit *_host = nullptr;
    QSpinBox *_port = nullptr;
    QCheckBox *_https = nullptr;
    QLineEdit *_username = nullptr;
    QLineEdit *_password = nullptr;
    QPushButton *_testButton = nullptr;
    QLabel *_testResult = nullptr;
    QThread *_testThread = nullptr;
    std::shared_ptr<std::atomic<bool>> _testCancel;

    QLabel *_magnetHandlerStatus = nullptr;
    QLabel *_torrentHandlerStatus = nullptr;

    QCheckBox *_quickMode = nullptr;
    QCheckBox *_forceStart = nullptr;
    QCheckBox *_deleteOnCancel = nullptr;
    QCheckBox *_autoClose = nullptr;
    QLineEdit *_defaultCategory = nullptr;
    QLineEdit *_defaultTags = nullptr;
    QComboBox *_contentLayout = nullptr;
    QSpinBox *_metadataTimeout = nullptr;
    QSpinBox *_requestTimeout = nullptr;
    QSpinBox *_forceDelayMs = nullptr;
    QSpinBox *_autoCloseMs = nullptr;
    QComboBox *_languageBox = nullptr;
};
