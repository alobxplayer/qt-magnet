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
    void rewriteServerTrackers();
    void assignMagnetHandler();
    void removeMagnetHandler();
    void assignTorrentHandler();
    void removeTorrentHandler();

private:
    void buildUi();
    void loadValues();
    void refreshHandlerStatus();
    void updateAuthModeVisibility();
    Config probeConfigFromUi() const;
    QWidget *wrapSection(const QString &title, QWidget *content);
    QWidget *wrapInScroll(QWidget *content);
    QWidget *buildConnectionGroup();
    QWidget *buildGeneralTab();
    QWidget *buildSystemIntegrationGroup();
    QWidget *buildGeneralGroup();
    QWidget *buildAdvancedTab();
    QWidget *buildPasskeyGroup();
    QWidget *buildAdvancedGroup();

    Config &_cfg;

    QComboBox *_clientType = nullptr;
    QLabel *_authLabel = nullptr;
    QComboBox *_authMode = nullptr;
    QLineEdit *_host = nullptr;
    QSpinBox *_port = nullptr;
    QCheckBox *_https = nullptr;
    QLabel *_userLabel = nullptr;
    QLineEdit *_username = nullptr;
    QLabel *_passLabel = nullptr;
    QWidget *_passWidget = nullptr;
    QLineEdit *_password = nullptr;
    QLabel *_apiKeyLabel = nullptr;
    QWidget *_apiKeyWidget = nullptr;
    QLineEdit *_apiKey = nullptr;
    QPushButton *_testButton = nullptr;
    QLabel *_testResult = nullptr;
    QThread *_testThread = nullptr;
    std::shared_ptr<std::atomic<bool>> _testCancel;
    QPushButton *_rewriteTrackersButton = nullptr;
    QLabel *_rewriteTrackersResult = nullptr;
    QThread *_rewriteThread = nullptr;
    std::shared_ptr<std::atomic<bool>> _rewriteCancel;

    QLabel *_magnetHandlerStatus = nullptr;
    QLabel *_torrentHandlerStatus = nullptr;

    QCheckBox *_quickMode = nullptr;
    QCheckBox *_forceStart = nullptr;
    QCheckBox *_deleteOnCancel = nullptr;
    QCheckBox *_autoClose = nullptr;
    QLineEdit *_defaultCategory = nullptr;
    QLineEdit *_defaultTags = nullptr;
    QLineEdit *_rutrackerPasskey = nullptr;
    QComboBox *_contentLayout = nullptr;
    QSpinBox *_metadataTimeout = nullptr;
    QSpinBox *_requestTimeout = nullptr;
    QSpinBox *_forceDelayMs = nullptr;
    QSpinBox *_autoCloseMs = nullptr;
    QComboBox *_languageBox = nullptr;
};
