#include "settingsdialog.h"
#include "config.h"
#include "logger.h"
#include "magnethandler.h"
#include "qbtclient.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>

static const char *kAppTitle = "qt-magnet";

namespace {
void setStatusLabelColor(QLabel *label, bool isSuccess, const QPalette &pal)
{
    bool isDark = pal.color(QPalette::WindowText).lightness() > 128;
    QPalette p = pal;
    p.setColor(QPalette::WindowText, isSuccess ? (isDark ? QColor(100, 220, 100) : Qt::darkGreen)
                                               : (isDark ? QColor(255, 110, 110) : Qt::darkRed));
    label->setPalette(p);
}
} // namespace

SettingsDialog::SettingsDialog(Config &cfg, QWidget *parent)
    : QDialog(parent), _cfg(cfg)
{
    buildUi();
    loadValues();
    refreshHandlerStatus();
}

SettingsDialog::~SettingsDialog()
{
    if (_testCancel)
        *_testCancel = true;
    if (_testThread && _testThread->isRunning()) {
        _testThread->wait(1000);
    }
}

void SettingsDialog::changeEvent(QEvent *e)
{
    QDialog::changeEvent(e);
    if (e->type() == QEvent::PaletteChange || e->type() == QEvent::ThemeChange) {
        refreshHandlerStatus();
        if (_testButton && _testButton->isEnabled() && _testResult && !_testResult->text().isEmpty()) {
            bool ok = _testResult->text().startsWith(tr("OK"));
            setStatusLabelColor(_testResult, ok, palette());
        }
    }
}

void SettingsDialog::buildUi()
{
    setWindowTitle(tr("Settings"));
    resize(540, 680);
    setMinimumSize(450, 400);

    auto *mainLayout = new QVBoxLayout(this);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *scrollContent = new QWidget();
    auto *stackLayout = new QVBoxLayout(scrollContent);
    stackLayout->addWidget(buildConnectionGroup());
    stackLayout->addWidget(buildHandlerGroup());
    stackLayout->addWidget(buildBehaviorGroup());
    stackLayout->addStretch();
    scroll->setWidget(scrollContent);
    mainLayout->addWidget(scroll, 1);

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    buttonBox->setContentsMargins(12, 10, 12, 10);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &SettingsDialog::onSave);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttonBox);
}

QWidget *SettingsDialog::buildConnectionGroup()
{
    auto *g = new QGroupBox(tr("Connection"), this);
    auto *form = new QFormLayout(g);

    _clientType = new QComboBox(this);
    _clientType->addItem(tr("Auto-detect"), QStringLiteral("auto"));
    _clientType->addItem(QStringLiteral("qBittorrent"), QStringLiteral("qbittorrent"));
    _clientType->addItem(QStringLiteral("Transmission"), QStringLiteral("transmission"));
    _clientType->addItem(QStringLiteral("Aria2"), QStringLiteral("aria2"));
    auto *typeLabel = new QLabel(tr("Server &type:"), this);
    typeLabel->setBuddy(_clientType);
    form->addRow(typeLabel, _clientType);

    _host = new QLineEdit(this);
    _host->setPlaceholderText(QStringLiteral("127.0.0.1"));
    _host->setClearButtonEnabled(true);
    _https = new QCheckBox(QStringLiteral("HTTPS"), this);

    connect(_host, &QLineEdit::editingFinished, this, [this] {
        QString txt = _host->text().trimmed();
        if (txt.startsWith(QLatin1String("http://"), Qt::CaseInsensitive) ||
            txt.startsWith(QLatin1String("https://"), Qt::CaseInsensitive)) {
            QUrl url(txt);
            if (url.isValid() && !url.host().isEmpty()) {
                _host->setText(url.host());
                if (url.port() > 0)
                    _port->setValue(url.port());
                _https->setChecked(url.scheme().toLower() == QLatin1String("https"));
            }
        }
    });

    auto *hostRow = new QHBoxLayout();
    hostRow->addWidget(_host, 1);
    hostRow->addWidget(_https);
    auto *hostLabel = new QLabel(tr("&Host / IP:"), this);
    hostLabel->setBuddy(_host);
    form->addRow(hostLabel, hostRow);

    _port = new QSpinBox(this);
    _port->setRange(1, 65535);
    form->addRow(tr("&Port:"), _port);

    _authMode = new QComboBox(this);
    _authMode->addItem(tr("Username / Password"), QStringLiteral("password"));
    _authMode->addItem(tr("API Key (qBittorrent v6+)"), QStringLiteral("apikey"));
    _authLabel = new QLabel(tr("&Authentication:"), this);
    _authLabel->setBuddy(_authMode);
    form->addRow(_authLabel, _authMode);

    _username = new QLineEdit(this);
    _username->setClearButtonEnabled(true);
    _userLabel = new QLabel(tr("&Username:"), this);
    _userLabel->setBuddy(_username);
    form->addRow(_userLabel, _username);

    _password = new QLineEdit(this);
    _password->setEchoMode(QLineEdit::Password);
    _password->setClearButtonEnabled(true);
    auto *showPass = new QCheckBox(tr("Show"), this);
    connect(showPass, &QCheckBox::toggled, this, [this](bool on) {
        _password->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
    });
    auto *passRow = new QHBoxLayout();
    passRow->setContentsMargins(0, 0, 0, 0);
    passRow->addWidget(_password, 1);
    passRow->addWidget(showPass);
    _passWidget = new QWidget(this);
    _passWidget->setLayout(passRow);
    _passLabel = new QLabel(tr("&Password:"), this);
    _passLabel->setBuddy(_password);
    form->addRow(_passLabel, _passWidget);

    _apiKey = new QLineEdit(this);
    _apiKey->setEchoMode(QLineEdit::Password);
    _apiKey->setClearButtonEnabled(true);
    _apiKey->setPlaceholderText(tr("Enter API key"));
    auto *showApiKey = new QCheckBox(tr("Show"), this);
    connect(showApiKey, &QCheckBox::toggled, this, [this](bool on) {
        _apiKey->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
    });
    auto *apiRow = new QHBoxLayout();
    apiRow->setContentsMargins(0, 0, 0, 0);
    apiRow->addWidget(_apiKey, 1);
    apiRow->addWidget(showApiKey);
    _apiKeyWidget = new QWidget(this);
    _apiKeyWidget->setLayout(apiRow);
    _apiKeyLabel = new QLabel(tr("API &Key:"), this);
    _apiKeyLabel->setBuddy(_apiKey);
    form->addRow(_apiKeyLabel, _apiKeyWidget);

    connect(_authMode, &QComboBox::currentIndexChanged, this, &SettingsDialog::updateAuthModeVisibility);
    connect(_clientType, &QComboBox::currentIndexChanged, this, &SettingsDialog::updateAuthModeVisibility);

    _testButton = new QPushButton(tr("Test connection"), this);
    connect(_testButton, &QPushButton::clicked, this, &SettingsDialog::testConnection);
    _testResult = new QLabel(this);
    _testResult->setTextFormat(Qt::PlainText);
    _testResult->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    _testResult->setWordWrap(true);
    auto *testRow = new QHBoxLayout();
    testRow->addWidget(_testButton);
    testRow->addWidget(_testResult, 1);
    form->addRow(QString(), testRow);

    return g;
}

void SettingsDialog::updateAuthModeVisibility()
{
    QString ctype = _clientType->currentData().toString();
    bool isQbt = (ctype == QLatin1String("qbittorrent"));

    if (_authLabel)
        _authLabel->setVisible(isQbt);
    if (_authMode)
        _authMode->setVisible(isQbt);

    bool isApiKey = isQbt && (_authMode->currentData().toString() == QLatin1String("apikey"));
    bool isAria2 = (ctype == QLatin1String("aria2"));

    _userLabel->setVisible(!isApiKey && !isAria2);
    _username->setVisible(!isApiKey && !isAria2);
    _passLabel->setVisible(!isApiKey);
    _passWidget->setVisible(!isApiKey);

    if (isAria2) {
        _passLabel->setText(tr("RPC &Secret Token:"));
    } else {
        _passLabel->setText(tr("&Password:"));
    }

    _apiKeyLabel->setVisible(isApiKey);
    _apiKeyWidget->setVisible(isApiKey);
}

QWidget *SettingsDialog::buildHandlerGroup()
{
    auto *container = new QWidget(this);
    auto *mainLayout = new QVBoxLayout(container);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    auto *gMagnet = new QGroupBox(tr("Magnet Protocol Handler (magnet:)"), this);
    auto *lMagnet = new QVBoxLayout(gMagnet);
    _magnetHandlerStatus = new QLabel(QStringLiteral("..."), this);
    _magnetHandlerStatus->setTextFormat(Qt::PlainText);
    _magnetHandlerStatus->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    _magnetHandlerStatus->setWordWrap(true);
    lMagnet->addWidget(_magnetHandlerStatus);

    auto *btnRowMagnet = new QHBoxLayout();
    auto *assignMagnet = new QPushButton(tr("Register Magnet"), this);
    connect(assignMagnet, &QPushButton::clicked, this, &SettingsDialog::assignMagnetHandler);
    auto *removeMagnet = new QPushButton(tr("Unregister Magnet"), this);
    connect(removeMagnet, &QPushButton::clicked, this, &SettingsDialog::removeMagnetHandler);
    btnRowMagnet->addWidget(assignMagnet);
    btnRowMagnet->addWidget(removeMagnet);
    btnRowMagnet->addStretch();
    lMagnet->addLayout(btnRowMagnet);
    mainLayout->addWidget(gMagnet);

    auto *gTorrent = new QGroupBox(tr("Torrent Files Handler (.torrent)"), this);
    auto *lTorrent = new QVBoxLayout(gTorrent);
    _torrentHandlerStatus = new QLabel(QStringLiteral("..."), this);
    _torrentHandlerStatus->setTextFormat(Qt::PlainText);
    _torrentHandlerStatus->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    _torrentHandlerStatus->setWordWrap(true);
    lTorrent->addWidget(_torrentHandlerStatus);

    auto *btnRowTorrent = new QHBoxLayout();
    auto *assignTorrent = new QPushButton(tr("Register .torrent"), this);
    connect(assignTorrent, &QPushButton::clicked, this, &SettingsDialog::assignTorrentHandler);
    auto *removeTorrent = new QPushButton(tr("Unregister .torrent"), this);
    connect(removeTorrent, &QPushButton::clicked, this, &SettingsDialog::removeTorrentHandler);
    btnRowTorrent->addWidget(assignTorrent);
    btnRowTorrent->addWidget(removeTorrent);
    btnRowTorrent->addStretch();
    lTorrent->addLayout(btnRowTorrent);
    mainLayout->addWidget(gTorrent);

    mainLayout->addStretch();
    return container;
}

QWidget *SettingsDialog::buildBehaviorGroup()
{
    auto *g = new QGroupBox(tr("Behavior"), this);
    auto *form = new QFormLayout(g);

    _quickMode = new QCheckBox(tr("Quick mode (skip dialog)"), this);
    form->addRow(_quickMode);

    _forceStart = new QCheckBox(tr("Force start by default"), this);
    form->addRow(_forceStart);

    _deleteOnCancel = new QCheckBox(tr("Delete torrent on cancel"), this);
    form->addRow(_deleteOnCancel);

    _autoClose = new QCheckBox(tr("Close window on success"), this);
    form->addRow(_autoClose);

    _defaultCategory = new QLineEdit(this);
    form->addRow(tr("Default &category:"), _defaultCategory);

    _defaultTags = new QLineEdit(this);
    form->addRow(tr("Default &tags:"), _defaultTags);

    _contentLayout = new QComboBox(this);
    _contentLayout->addItem(tr("Server default"), QString());
    _contentLayout->addItem(tr("Original"), QStringLiteral("Original"));
    _contentLayout->addItem(tr("Subfolder"), QStringLiteral("Subfolder"));
    _contentLayout->addItem(tr("NoSubfolder"), QStringLiteral("NoSubfolder"));
    form->addRow(tr("Content &layout:"), _contentLayout);

    _metadataTimeout = new QSpinBox(this);
    _metadataTimeout->setRange(5, 3600);
    _metadataTimeout->setSuffix(tr(" s"));
    form->addRow(tr("&Metadata timeout:"), _metadataTimeout);

    _requestTimeout = new QSpinBox(this);
    _requestTimeout->setRange(5, 300);
    _requestTimeout->setSuffix(tr(" s"));
    form->addRow(tr("&Request timeout:"), _requestTimeout);

    _forceDelayMs = new QSpinBox(this);
    _forceDelayMs->setRange(0, 60000);
    _forceDelayMs->setSuffix(tr(" ms"));
    form->addRow(tr("&Force start delay:"), _forceDelayMs);

    _autoCloseMs = new QSpinBox(this);
    _autoCloseMs->setRange(500, 60000);
    _autoCloseMs->setSuffix(tr(" ms"));
    form->addRow(tr("&Auto-close delay:"), _autoCloseMs);

    _languageBox = new QComboBox(this);
    _languageBox->addItem(tr("System default"), QStringLiteral("system"));
    _languageBox->addItem(QStringLiteral("English (en_US)"), QStringLiteral("en_US"));
    _languageBox->addItem(QStringLiteral("Русский (ru_RU)"), QStringLiteral("ru_RU"));
    form->addRow(tr("&Language:"), _languageBox);

    return g;
}

void SettingsDialog::loadValues()
{
    int typeIdx = _clientType->findData(_cfg.clientType);
    _clientType->setCurrentIndex(typeIdx >= 0 ? typeIdx : 0);

    _host->setText(_cfg.host);
    _port->setValue(qBound(1, _cfg.port, 65535));
    _https->setChecked(_cfg.useHttps);
    int authIdx = _authMode->findData(_cfg.authMode);
    _authMode->setCurrentIndex(authIdx >= 0 ? authIdx : 0);
    _username->setText(_cfg.username);
    _password->setText(_cfg.getPassword());
    _apiKey->setText(_cfg.getApiKey());
    updateAuthModeVisibility();

    _quickMode->setChecked(_cfg.quickMode);
    _forceStart->setChecked(_cfg.forceStartDefault);
    _deleteOnCancel->setChecked(_cfg.deleteOnCancel);
    _autoClose->setChecked(_cfg.autoCloseOnSuccess);
    _defaultCategory->setText(_cfg.defaultCategory);
    _defaultTags->setText(_cfg.defaultTags);
    _metadataTimeout->setValue(qBound(5, _cfg.metadataTimeoutSec, 3600));
    _requestTimeout->setValue(qBound(5, _cfg.requestTimeoutSec, 300));
    _forceDelayMs->setValue(qBound(0, _cfg.forceStartDelayMs, 60000));
    _autoCloseMs->setValue(qBound(500, _cfg.autoCloseMs, 60000));

    int langIdx = _languageBox->findData(_cfg.language);
    if (langIdx < 0) {
        if (_cfg.language == QLatin1String("en"))
            langIdx = _languageBox->findData(QStringLiteral("en_US"));
        else if (_cfg.language == QLatin1String("ru"))
            langIdx = _languageBox->findData(QStringLiteral("ru_RU"));
    }
    if (langIdx >= 0)
        _languageBox->setCurrentIndex(langIdx);
    else
        _languageBox->setCurrentIndex(0);

    for (int i = 0; i < _contentLayout->count(); ++i) {
        if (_contentLayout->itemData(i).toString() == _cfg.contentLayout) {
            _contentLayout->setCurrentIndex(i);
            break;
        }
    }
}

void SettingsDialog::onSave()
{
    QString host = _host->text().trimmed();
    if (host.isEmpty()) {
        QMessageBox::warning(this, QString::fromUtf8(kAppTitle), tr("Specify server host."));
        _host->setFocus();
        return;
    }

    Config newCfg = _cfg;
    newCfg.clientType = _clientType->currentData().toString();
    newCfg.host = host;
    newCfg.port = _port->value();
    newCfg.useHttps = _https->isChecked();
    newCfg.authMode = (newCfg.clientType == QLatin1String("qbittorrent"))
                          ? _authMode->currentData().toString()
                          : QStringLiteral("password");
    newCfg.username = _username->text().trimmed();
    newCfg.setPassword(_password->text());
    newCfg.setApiKey(_apiKey->text().trimmed());

    newCfg.quickMode = _quickMode->isChecked();
    newCfg.forceStartDefault = _forceStart->isChecked();
    newCfg.deleteOnCancel = _deleteOnCancel->isChecked();
    newCfg.autoCloseOnSuccess = _autoClose->isChecked();
    newCfg.defaultCategory = _defaultCategory->text().trimmed();
    newCfg.defaultTags = _defaultTags->text().trimmed();
    newCfg.contentLayout = _contentLayout->currentData().toString();
    newCfg.metadataTimeoutSec = _metadataTimeout->value();
    newCfg.requestTimeoutSec = _requestTimeout->value();
    newCfg.forceStartDelayMs = _forceDelayMs->value();
    newCfg.autoCloseMs = _autoCloseMs->value();
    newCfg.language = _languageBox->currentData().toString();

    try {
        newCfg.save();
        _cfg = newCfg;
        Log::write(QStringLiteral("Settings saved."));
        accept();
    } catch (const std::exception &ex) {
        Log::write(QStringLiteral("Failed to save settings: %1").arg(QString::fromUtf8(ex.what())));
        QMessageBox::critical(this, QString::fromUtf8(kAppTitle),
                              tr("Failed to save settings:\n\n%1").arg(QString::fromUtf8(ex.what())));
    }
}

void SettingsDialog::testConnection()
{
    if (_testThread && _testThread->isRunning())
        return;

    _testButton->setEnabled(false);
    _testResult->setPalette(QPalette());
    _testResult->setText(tr("Testing..."));

    Config probe;
    probe.clientType = _clientType->currentData().toString();
    probe.host = _host->text().trimmed();
    probe.port = _port->value();
    probe.useHttps = _https->isChecked();
    probe.authMode = (probe.clientType == QLatin1String("qbittorrent"))
                         ? _authMode->currentData().toString()
                         : QStringLiteral("password");
    probe.username = _username->text().trimmed();
    probe.password = _password->text();
    probe.apiKey = _apiKey->text().trimmed();
    probe.requestTimeoutSec = _requestTimeout->value();

    _testCancel = std::make_shared<std::atomic<bool>>(false);
    auto cancelToken = _testCancel;

    QPointer<SettingsDialog> self(this);
    _testThread = QThread::create([self, probe, cancelToken]() {
        QString message;
        bool ok = false;
        QbtClient::ClientType detected = QbtClient::ClientType::Auto;
        try {
            QbtClient client(probe, [](const QString &) {}, [cancelToken]() {
                return cancelToken ? cancelToken->load() : false;
            });
            client.login();
            client.fetchServerInfo();
            detected = client.detectedType;
            ok = true;
            message = tr("OK (%1, %2)").arg(client.appVersion, client.apiVersion);
        } catch (const QbtException &ex) {
            message = ex.message;
        } catch (const std::exception &ex) {
            message = QString::fromUtf8(ex.what());
        }

        if (!self)
            return;

        QMetaObject::invokeMethod(self, [self, ok, message, detected]() {
            if (!self)
                return;
            setStatusLabelColor(self->_testResult, ok, self->palette());
            self->_testResult->setText(message);
            self->_testButton->setEnabled(true);
            if (ok && detected == QbtClient::ClientType::QBittorrent) {
                self->updateAuthModeVisibility();
            }
        }, Qt::QueuedConnection);
    });
    connect(_testThread, &QThread::finished, _testThread, &QObject::deleteLater);
    connect(_testThread, &QThread::finished, this, [this] { _testThread = nullptr; _testCancel.reset(); });
    _testThread->start();
}

void SettingsDialog::refreshHandlerStatus()
{
    try {
        HandlerInfo infoMagnet = MagnetHandler::queryMagnet();
        QString stateMagnet = infoMagnet.isOurs
                                  ? tr("qt-magnet is default handler for magnet links.")
                                  : tr("Current handler: %1.").arg(infoMagnet.description);
        _magnetHandlerStatus->setText(stateMagnet);
        if (infoMagnet.isOurs) {
            setStatusLabelColor(_magnetHandlerStatus, true, palette());
        } else {
            _magnetHandlerStatus->setPalette(QPalette());
        }
    } catch (const std::exception &ex) {
        _magnetHandlerStatus->setText(tr("Query error: %1").arg(QString::fromUtf8(ex.what())));
        _magnetHandlerStatus->setPalette(QPalette());
    }

    try {
        HandlerInfo infoTorrent = MagnetHandler::queryTorrent();
        QString stateTorrent = infoTorrent.isOurs
                                   ? tr("qt-magnet is default handler for .torrent files.")
                                   : tr("Current handler: %1.").arg(infoTorrent.description);
        _torrentHandlerStatus->setText(stateTorrent);
        if (infoTorrent.isOurs) {
            setStatusLabelColor(_torrentHandlerStatus, true, palette());
        } else {
            _torrentHandlerStatus->setPalette(QPalette());
        }
    } catch (const std::exception &ex) {
        _torrentHandlerStatus->setText(tr("Query error: %1").arg(QString::fromUtf8(ex.what())));
        _torrentHandlerStatus->setPalette(QPalette());
    }
}

void SettingsDialog::assignMagnetHandler()
{
    try {
        MagnetHandler::registerMagnetHandler();
        MagnetHandler::removeMagnetUserChoice();
        refreshHandlerStatus();
    } catch (const std::exception &ex) {
        Log::write(QStringLiteral("Assign magnet handler error: %1").arg(QString::fromUtf8(ex.what())));
        QMessageBox::critical(this, QString::fromUtf8(kAppTitle),
                              tr("Magnet registration failed:\n\n%1").arg(QString::fromUtf8(ex.what())));
    }
}

void SettingsDialog::removeMagnetHandler()
{
    try {
        bool removed = MagnetHandler::unregisterMagnetHandler();
        if (!removed)
            QMessageBox::information(this, QString::fromUtf8(kAppTitle),
                                     tr("No magnet registry entries found."));
        refreshHandlerStatus();
    } catch (const std::exception &ex) {
        Log::write(QStringLiteral("Remove magnet handler error: %1").arg(QString::fromUtf8(ex.what())));
        QMessageBox::critical(this, QString::fromUtf8(kAppTitle),
                              tr("Magnet unregistration failed:\n\n%1").arg(QString::fromUtf8(ex.what())));
    }
}

void SettingsDialog::assignTorrentHandler()
{
    try {
        MagnetHandler::registerTorrentHandler();
        MagnetHandler::removeTorrentUserChoice();
        refreshHandlerStatus();
    } catch (const std::exception &ex) {
        Log::write(QStringLiteral("Assign torrent handler error: %1").arg(QString::fromUtf8(ex.what())));
        QMessageBox::critical(this, QString::fromUtf8(kAppTitle),
                              tr("Torrent registration failed:\n\n%1").arg(QString::fromUtf8(ex.what())));
    }
}

void SettingsDialog::removeTorrentHandler()
{
    try {
        bool removed = MagnetHandler::unregisterTorrentHandler();
        if (!removed)
            QMessageBox::information(this, QString::fromUtf8(kAppTitle),
                                     tr("No .torrent registry entries found."));
        refreshHandlerStatus();
    } catch (const std::exception &ex) {
        Log::write(QStringLiteral("Remove torrent handler error: %1").arg(QString::fromUtf8(ex.what())));
        QMessageBox::critical(this, QString::fromUtf8(kAppTitle),
                              tr("Torrent unregistration failed:\n\n%1").arg(QString::fromUtf8(ex.what())));
    }
}
