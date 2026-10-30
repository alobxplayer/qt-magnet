#include "settingsdialog.h"
#include "confignotifier.h"
#include "config.h"
#include "darktheme.h"
#include "format.h"
#include "logger.h"
#include "magnethandler.h"
#include "torrentclient.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDate>
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
#include <QTabWidget>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>

namespace {
constexpr int kThreadStopWaitMs = 1000;

void setStatusLabelColor(QLabel *label, bool isSuccess, const QPalette &pal)
{
    bool isDark = DarkTheme::isDark();
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
        _testThread->wait(kThreadStopWaitMs);
    }
    if (_rewriteCancel)
        *_rewriteCancel = true;
    if (_rewriteThread && _rewriteThread->isRunning()) {
        _rewriteThread->wait(kThreadStopWaitMs);
    }
}

void SettingsDialog::changeEvent(QEvent *e)
{
    QDialog::changeEvent(e);
    if (e->type() == QEvent::PaletteChange || e->type() == QEvent::ThemeChange) {
        // Theme re-apply (global) re-polishes state-colored labels automatically;
        // handler status text just needs a refresh.
        refreshHandlerStatus();
    }
}

void SettingsDialog::buildUi()
{
    setWindowTitle(tr("Settings"));
    resize(575, 596);
    setMinimumSize(480, 420);

    auto *mainLayout = new QVBoxLayout(this);

    auto *tabs = new QTabWidget(this);
    tabs->addTab(wrapInScroll(buildConnectionGroup()), tr("Connection"));
    tabs->addTab(wrapInScroll(buildGeneralTab()), tr("General"));
    tabs->addTab(wrapInScroll(buildAdvancedTab()), tr("Advanced"));
    mainLayout->addWidget(tabs, 1);

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    buttonBox->setContentsMargins(12, 10, 12, 10);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &SettingsDialog::onSave);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);
    mainLayout->addWidget(buttonBox);
}

QWidget *SettingsDialog::wrapSection(const QString &title, QWidget *content)
{
    auto *g = new QGroupBox(title, this);
    auto *lay = new QVBoxLayout(g);
    lay->setSpacing(6);
    lay->addWidget(content);
    return g;
}

// Tab pages must scroll: the General tab alone is taller than the dialog.
QWidget *SettingsDialog::wrapInScroll(QWidget *content)
{
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);
    return scroll;
}

QWidget *SettingsDialog::buildConnectionGroup()
{
    auto *page = new QWidget(this);
    auto *form = new QFormLayout(page);
    form->setContentsMargins(12, 12, 12, 12);
    form->setVerticalSpacing(6);
    form->setLabelAlignment(Qt::AlignVCenter | Qt::AlignLeft);

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

    return page;
}

void SettingsDialog::updateAuthModeVisibility()
{
    QString ctype = _clientType->currentData().toString();
    bool isQbt = (ctype == QLatin1String("qbittorrent"));
    bool isAria2 = (ctype == QLatin1String("aria2"));

    _authLabel->setVisible(isQbt);
    _authMode->setVisible(isQbt);

    // The rewrite is a server-side operation aria2 cannot do.
    if (isAria2) {
        _rewriteTrackersButton->setEnabled(false);
        _rewriteTrackersButton->setToolTip(tr("Not supported by this server."));
    } else {
        _rewriteTrackersButton->setEnabled(true);
        _rewriteTrackersButton->setToolTip(tr("Replaces the passkey in the rutracker announce URLs of every "
                                              "torrent already on the server with the passkey above."));
    }

    bool isApiKey = isQbt && (_authMode->currentData().toString() == QLatin1String("apikey"));

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

QWidget *SettingsDialog::buildSystemIntegrationGroup()
{
    auto *container = new QWidget(this);
    auto *mainLayout = new QVBoxLayout(container);
    mainLayout->setContentsMargins(0, 2, 0, 0);
    mainLayout->setSpacing(10);

    auto makeRow = [this](const QString &title, QLabel **statusOut,
                          const char *assignSlot, const char *removeSlot) -> QWidget * {
        auto *row = new QWidget(this);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(10);

        auto *textCol = new QVBoxLayout();
        textCol->setSpacing(2);
        auto *titleLabel = new QLabel(title, row);
        textCol->addWidget(titleLabel);
        QLabel *statusLabel = new QLabel(QStringLiteral("..."), row);
        statusLabel->setTextFormat(Qt::PlainText);
        statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        statusLabel->setWordWrap(true);
        textCol->addWidget(statusLabel);
        *statusOut = statusLabel;
        rowLayout->addLayout(textCol, 1);

        auto *assign = new QPushButton(tr("Set as default"), row);
        connect(assign, SIGNAL(clicked()), this, assignSlot);
        rowLayout->addWidget(assign, 0, Qt::AlignTop);

        auto *remove = new QPushButton(tr("Remove"), row);
        connect(remove, SIGNAL(clicked()), this, removeSlot);
        rowLayout->addWidget(remove, 0, Qt::AlignTop);

        return row;
    };

    mainLayout->addWidget(makeRow(tr("Magnet links (magnet:)"), &_magnetHandlerStatus,
                                  SLOT(assignMagnetHandler()), SLOT(removeMagnetHandler())));
    mainLayout->addSpacing(8);
    mainLayout->addWidget(makeRow(tr("Torrent files (.torrent)"), &_torrentHandlerStatus,
                                  SLOT(assignTorrentHandler()), SLOT(removeTorrentHandler())));
    mainLayout->addStretch();
    return container;
}

QWidget *SettingsDialog::buildGeneralTab()
{
    auto *page = new QWidget(this);
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(12, 12, 12, 12);
    lay->setSpacing(12);
    lay->addWidget(wrapSection(tr("System integration"), buildSystemIntegrationGroup()));
    lay->addWidget(wrapSection(tr("General"), buildGeneralGroup()));
    lay->addStretch();
    return page;
}

QWidget *SettingsDialog::buildGeneralGroup()
{
    auto *g = new QWidget(this);
    auto *form = new QFormLayout(g);
    form->setContentsMargins(0, 2, 0, 0);
    form->setVerticalSpacing(6);

    _quickMode = new QCheckBox(tr("Quick mode (skip dialog)"), this);
    form->addRow(_quickMode);

    _forceStart = new QCheckBox(tr("Force start by default"), this);
    form->addRow(_forceStart);

    _deleteOnCancel = new QCheckBox(tr("Delete torrent on cancel"), this);
    _deleteOnCancel->setToolTip(tr("Deletes only torrents added by qt-magnet in this session"));
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

    _fileViewMode = new QComboBox(this);
    _fileViewMode->addItem(tr("Flat list"), QStringLiteral("flat"));
    _fileViewMode->addItem(tr("Tree view"), QStringLiteral("tree"));
    connect(_fileViewMode, &QComboBox::currentIndexChanged, this, [this](int) {
        if (_fileViewMode) {
            QString mode = _fileViewMode->currentData().toString();
            _cfg.fileViewMode = mode;
            emit ConfigNotifier::instance()->fileViewModeChanged(mode);
        }
    });
    form->addRow(tr("File list &view:"), _fileViewMode);

    auto *statsLabel = new QLabel(this);
    statsLabel->setTextFormat(Qt::PlainText);
    const QString currentMonth = QDate::currentDate().toString(QStringLiteral("yyyy-MM"));
    const bool isCurrentMonth = (_cfg.statsMonth == currentMonth);
    const int added = isCurrentMonth ? _cfg.statsAdded : 0;
    const qint64 bytes = isCurrentMonth ? _cfg.statsBytes : 0;
    if (added > 0) {
        statsLabel->setText(tr("Added this month: %n torrent(s), %1", "", added)
                                .arg(Format::size(bytes)));
    } else {
        statsLabel->setText(tr("Added this month: none"));
    }
    form->addRow(QString(), statsLabel);

    _languageBox = new QComboBox(this);
    _languageBox->addItem(tr("System default"), QStringLiteral("system"));
    _languageBox->addItem(QStringLiteral("English (en_US)"), QStringLiteral("en_US"));
    _languageBox->addItem(QStringLiteral("Русский (ru_RU)"), QStringLiteral("ru_RU"));
    auto *langNote = new QLabel(tr("Takes effect after restart"), this);
    auto *langRow = new QHBoxLayout();
    langRow->setContentsMargins(0, 0, 0, 0);
    langRow->setSpacing(8);
    langRow->addWidget(_languageBox, 1);
    langRow->addWidget(langNote);
    auto *langWidget = new QWidget(this);
    langWidget->setLayout(langRow);
    form->addRow(tr("&Language:"), langWidget);

    return g;
}

QWidget *SettingsDialog::buildAdvancedTab()
{
    auto *page = new QWidget(this);
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(12, 12, 12, 12);
    lay->setSpacing(12);
    lay->addWidget(wrapSection(tr("Tracker passkey"), buildPasskeyGroup()));
    lay->addWidget(wrapSection(tr("Timeouts and delays"), buildAdvancedGroup()));
    lay->addStretch();
    return page;
}

QWidget *SettingsDialog::buildPasskeyGroup()
{
    auto *g = new QWidget(this);
    auto *form = new QFormLayout(g);
    form->setContentsMargins(0, 2, 0, 0);
    form->setVerticalSpacing(6);

    // Upload on rutracker is credited to the passkey embedded in announce
    // URLs; a foreign or missing one leaves the user's stats uncounted.
    _rutrackerPasskey = new QLineEdit(this);
    _rutrackerPasskey->setPlaceholderText(tr("e.g. 4f80d2d6cc6200e0f589cde305af5742"));
    _rutrackerPasskey->setToolTip(tr("Your rutracker.org passkey — the pk=… value from the tracker's "
                                     "announce URL (seen in any .torrent downloaded while logged in).\n"
                                     "When set, rutracker announce URLs are rewritten with it when "
                                     "adding, so your uploaded traffic is counted on your account."));
    form->addRow(tr("RuTracker &passkey:"), _rutrackerPasskey);

    _rewriteTrackersButton = new QPushButton(tr("Rewrite trackers on the server"), this);
    _rewriteTrackersButton->setToolTip(tr("Replaces the passkey in the rutracker announce URLs of every "
                                          "torrent already on the server with the passkey above."));
    connect(_rewriteTrackersButton, &QPushButton::clicked, this, &SettingsDialog::rewriteServerTrackers);
    _rewriteTrackersResult = new QLabel(this);
    _rewriteTrackersResult->setTextFormat(Qt::PlainText);
    _rewriteTrackersResult->setWordWrap(true);
    auto *rewriteRow = new QWidget(this);
    auto *rewriteLay = new QHBoxLayout(rewriteRow);
    rewriteLay->setContentsMargins(0, 0, 0, 0);
    rewriteLay->setSpacing(8);
    rewriteLay->addWidget(_rewriteTrackersButton, 0, Qt::AlignTop);
    rewriteLay->addWidget(_rewriteTrackersResult, 1, Qt::AlignTop);
    form->addRow(QString(), rewriteRow);

    return g;
}

QWidget *SettingsDialog::buildAdvancedGroup()
{
    auto *g = new QWidget(this);
    auto *form = new QFormLayout(g);
    form->setContentsMargins(0, 2, 0, 0);
    form->setVerticalSpacing(6);

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
    QString storedPassword;
    QString storedApiKey;
    QString storedRutrackerPk;
    try {
        storedPassword = _cfg.getPassword();
        storedApiKey = _cfg.getApiKey();
        storedRutrackerPk = _cfg.getRutrackerPasskey();
    } catch (const std::exception &ex) {
        Log::write(QStringLiteral("Failed to load stored credentials: %1").arg(QString::fromUtf8(ex.what())));
        QMessageBox::warning(this, QStringLiteral("qt-magnet"),
                              tr("Failed to load stored credentials:\n\n%1").arg(QString::fromUtf8(ex.what())));
    }
    _password->setText(storedPassword);
    _apiKey->setText(storedApiKey);
    _rutrackerPasskey->setText(storedRutrackerPk);
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

    _initialFileViewMode = _cfg.fileViewMode;
    int fvmIdx = _fileViewMode->findData(_cfg.fileViewMode);
    if (fvmIdx >= 0)
        _fileViewMode->setCurrentIndex(fvmIdx);

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
        QMessageBox::warning(this, QStringLiteral("qt-magnet"), tr("Specify server host."));
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

    newCfg.quickMode = _quickMode->isChecked();
    newCfg.forceStartDefault = _forceStart->isChecked();
    newCfg.deleteOnCancel = _deleteOnCancel->isChecked();
    newCfg.autoCloseOnSuccess = _autoClose->isChecked();
    newCfg.defaultCategory = _defaultCategory->text().trimmed();
    newCfg.defaultTags = _defaultTags->text().trimmed();
    newCfg.contentLayout = _contentLayout->currentData().toString();
    newCfg.fileViewMode = _fileViewMode->currentData().toString();
    newCfg.metadataTimeoutSec = _metadataTimeout->value();
    newCfg.requestTimeoutSec = _requestTimeout->value();
    newCfg.forceStartDelayMs = _forceDelayMs->value();
    newCfg.autoCloseMs = _autoCloseMs->value();
    newCfg.language = _languageBox->currentData().toString();

    try {
        newCfg.setPassword(_password->text());
        newCfg.setApiKey(_apiKey->text().trimmed());
        newCfg.setRutrackerPasskey(_rutrackerPasskey->text().trimmed());
        newCfg.save();
        _cfg = newCfg;
        _initialFileViewMode = _cfg.fileViewMode;
        Log::write(QStringLiteral("Settings saved."));
        accept();
    } catch (const std::exception &ex) {
        Log::write(QStringLiteral("Failed to save settings: %1").arg(QString::fromUtf8(ex.what())));
        QMessageBox::critical(this, QStringLiteral("qt-magnet"),
                              tr("Failed to save settings:\n\n%1").arg(QString::fromUtf8(ex.what())));
    }
}

Config SettingsDialog::probeConfigFromUi() const
{
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
    return probe;
}

void SettingsDialog::testConnection()
{
    if (_testThread && _testThread->isRunning())
        return;

    _testButton->setEnabled(false);
    _testResult->setPalette(QPalette());
    _testResult->setText(tr("Testing..."));

    Config probe = probeConfigFromUi();

    _testCancel = std::make_shared<std::atomic<bool>>(false);
    auto cancelToken = _testCancel;

    QPointer<SettingsDialog> self(this);
    _testThread = QThread::create([self, probe, cancelToken]() {
        QString message;
        bool ok = false;
        try {
            TorrentClient client(probe, [](const QString &) {}, [cancelToken]() {
                return cancelToken ? cancelToken->load() : false;
            });
            client.login();
            client.fetchServerInfo();
            ok = true;
            message = tr("OK (%1, %2)").arg(client.appVersion, client.apiVersion);
        } catch (const ClientException &ex) {
            message = ex.message;
        } catch (const std::exception &ex) {
            message = QString::fromUtf8(ex.what());
        }

        if (!self)
            return;

        QMetaObject::invokeMethod(self, [self, ok, message]() {
            if (!self)
                return;
            // ✓/✕ ride the font; the color follows the palette.
            setStatusLabelColor(self->_testResult, ok, self->palette());
            self->_testResult->setText((ok ? QStringLiteral("✓ ") : QStringLiteral("✕ ")) + message);
            self->_testButton->setEnabled(true);
        }, Qt::QueuedConnection);
    });
    connect(_testThread, &QThread::finished, _testThread, &QObject::deleteLater);
    connect(_testThread, &QThread::finished, this, [this] { _testThread = nullptr; _testCancel.reset(); });
    _testThread->start();
}

void SettingsDialog::rewriteServerTrackers()
{
    if (_rewriteThread && _rewriteThread->isRunning())
        return;

    const QString passkey = _rutrackerPasskey->text().trimmed();
    if (passkey.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("qt-magnet"), tr("Enter the RuTracker passkey first."));
        _rutrackerPasskey->setFocus();
        return;
    }

    _rewriteTrackersButton->setEnabled(false);
    _rewriteTrackersResult->setPalette(palette());
    _rewriteTrackersResult->setText(tr("Rewriting..."));

    Config probe = probeConfigFromUi();

    _rewriteCancel = std::make_shared<std::atomic<bool>>(false);
    auto cancelToken = _rewriteCancel;

    QPointer<SettingsDialog> self(this);
    _rewriteThread = QThread::create([self, probe, passkey, cancelToken]() {
        QString message;
        bool ok = false;
        try {
            TorrentClient client(probe, [](const QString &) {}, [cancelToken]() {
                return cancelToken ? cancelToken->load() : false;
            });
            client.login();
            client.fetchServerInfo();
            const TorrentClient::TrackerRewriteSummary s =
                client.rewriteRutrackerPasskeys(passkey, nullptr, [cancelToken]() {
                    return cancelToken ? cancelToken->load() : false;
                });
            ok = (s.failed == 0);
            message = tr("%1 torrent(s) scanned, %2 tracker URL(s) rewritten, %3 failed.")
                          .arg(s.torrents).arg(s.rewritten).arg(s.failed);
        } catch (const ClientException &ex) {
            message = ex.message;
        } catch (const std::exception &ex) {
            message = QString::fromUtf8(ex.what());
        }

        if (!self)
            return;

        QMetaObject::invokeMethod(self, [self, ok, message]() {
            if (!self)
                return;
            setStatusLabelColor(self->_rewriteTrackersResult, ok, self->palette());
            self->_rewriteTrackersResult->setText((ok ? QStringLiteral("✓ ") : QStringLiteral("✕ ")) + message);
            self->_rewriteTrackersButton->setEnabled(true);
        }, Qt::QueuedConnection);
    });
    connect(_rewriteThread, &QThread::finished, _rewriteThread, &QObject::deleteLater);
    connect(_rewriteThread, &QThread::finished, this, [this] { _rewriteThread = nullptr; _rewriteCancel.reset(); });
    _rewriteThread->start();
}

void SettingsDialog::refreshHandlerStatus()
{
    try {
        HandlerInfo infoMagnet = MagnetHandler::queryMagnet();
        QString stateMagnet = infoMagnet.isOurs
                                  ? tr("● qt-magnet is the default for magnet links.")
                                  : tr("○ Currently handled by: %1.").arg(infoMagnet.description);
        _magnetHandlerStatus->setText(stateMagnet);
        if (infoMagnet.isOurs)
            setStatusLabelColor(_magnetHandlerStatus, true, palette());
        else
            _magnetHandlerStatus->setPalette(palette());
    } catch (const std::exception &ex) {
        _magnetHandlerStatus->setText(tr("Query error: %1").arg(QString::fromUtf8(ex.what())));
        setStatusLabelColor(_magnetHandlerStatus, false, palette());
    }

    try {
        HandlerInfo infoTorrent = MagnetHandler::queryTorrent();
        QString stateTorrent = infoTorrent.isOurs
                                   ? tr("● qt-magnet is the default for .torrent files.")
                                   : tr("○ Currently handled by: %1.").arg(infoTorrent.description);
        _torrentHandlerStatus->setText(stateTorrent);
        if (infoTorrent.isOurs)
            setStatusLabelColor(_torrentHandlerStatus, true, palette());
        else
            _torrentHandlerStatus->setPalette(palette());
    } catch (const std::exception &ex) {
        _torrentHandlerStatus->setText(tr("Query error: %1").arg(QString::fromUtf8(ex.what())));
        setStatusLabelColor(_torrentHandlerStatus, false, palette());
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
        QMessageBox::critical(this, QStringLiteral("qt-magnet"),
                              tr("Magnet registration failed:\n\n%1").arg(QString::fromUtf8(ex.what())));
    }
}

void SettingsDialog::removeMagnetHandler()
{
    try {
        bool removed = MagnetHandler::unregisterMagnetHandler();
        if (!removed)
            QMessageBox::information(this, QStringLiteral("qt-magnet"),
                                     tr("No magnet registry entries found."));
        refreshHandlerStatus();
    } catch (const std::exception &ex) {
        Log::write(QStringLiteral("Remove magnet handler error: %1").arg(QString::fromUtf8(ex.what())));
        QMessageBox::critical(this, QStringLiteral("qt-magnet"),
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
        QMessageBox::critical(this, QStringLiteral("qt-magnet"),
                              tr("Torrent registration failed:\n\n%1").arg(QString::fromUtf8(ex.what())));
    }
}

void SettingsDialog::removeTorrentHandler()
{
    try {
        bool removed = MagnetHandler::unregisterTorrentHandler();
        if (!removed)
            QMessageBox::information(this, QStringLiteral("qt-magnet"),
                                     tr("No .torrent registry entries found."));
        refreshHandlerStatus();
    } catch (const std::exception &ex) {
        Log::write(QStringLiteral("Remove torrent handler error: %1").arg(QString::fromUtf8(ex.what())));
        QMessageBox::critical(this, QStringLiteral("qt-magnet"),
                              tr("Torrent unregistration failed:\n\n%1").arg(QString::fromUtf8(ex.what())));
    }
}

void SettingsDialog::reject()
{
    if (_cfg.fileViewMode != _initialFileViewMode) {
        _cfg.fileViewMode = _initialFileViewMode;
        emit ConfigNotifier::instance()->fileViewModeChanged(_initialFileViewMode);
    }
    QDialog::reject();
}
