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

    _username = new QLineEdit(this);
    _username->setClearButtonEnabled(true);
    form->addRow(tr("&Username:"), _username);

    _password = new QLineEdit(this);
    _password->setEchoMode(QLineEdit::Password);
    _password->setClearButtonEnabled(true);
    auto *showPass = new QCheckBox(tr("Show"), this);
    connect(showPass, &QCheckBox::toggled, this, [this](bool on) {
        _password->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
    });
    auto *passRow = new QHBoxLayout();
    passRow->addWidget(_password, 1);
    passRow->addWidget(showPass);
    auto *passLabel = new QLabel(tr("&Password:"), this);
    passLabel->setBuddy(_password);
    form->addRow(passLabel, passRow);

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

QWidget *SettingsDialog::buildHandlerGroup()
{
    auto *g = new QGroupBox(tr("Protocol Handler"), this);
    auto *layout = new QVBoxLayout(g);

    _handlerStatus = new QLabel(QStringLiteral("..."), this);
    _handlerStatus->setTextFormat(Qt::PlainText);
    _handlerStatus->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    _handlerStatus->setWordWrap(true);
    layout->addWidget(_handlerStatus);

    auto *btnRow = new QHBoxLayout();
    auto *assign = new QPushButton(tr("Register"), this);
    connect(assign, &QPushButton::clicked, this, &SettingsDialog::assignHandler);
    auto *remove = new QPushButton(tr("Unregister"), this);
    connect(remove, &QPushButton::clicked, this, &SettingsDialog::removeHandler);
    btnRow->addWidget(assign);
    btnRow->addWidget(remove);
    btnRow->addStretch();
    layout->addLayout(btnRow);

    return g;
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
    _username->setText(_cfg.username);
    _password->setText(_cfg.getPassword());

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

    _cfg.clientType = _clientType->currentData().toString();
    _cfg.host = host;
    _cfg.port = _port->value();
    _cfg.useHttps = _https->isChecked();
    _cfg.username = _username->text().trimmed();
    _cfg.setPassword(_password->text());

    _cfg.quickMode = _quickMode->isChecked();
    _cfg.forceStartDefault = _forceStart->isChecked();
    _cfg.deleteOnCancel = _deleteOnCancel->isChecked();
    _cfg.autoCloseOnSuccess = _autoClose->isChecked();
    _cfg.defaultCategory = _defaultCategory->text().trimmed();
    _cfg.defaultTags = _defaultTags->text().trimmed();
    _cfg.contentLayout = _contentLayout->currentData().toString();
    _cfg.metadataTimeoutSec = _metadataTimeout->value();
    _cfg.requestTimeoutSec = _requestTimeout->value();
    _cfg.forceStartDelayMs = _forceDelayMs->value();
    _cfg.autoCloseMs = _autoCloseMs->value();
    _cfg.language = _languageBox->currentData().toString();

    try {
        _cfg.save();
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
    probe.username = _username->text().trimmed();
    probe.password = _password->text();
    probe.requestTimeoutSec = _requestTimeout->value();

    _testCancel = std::make_shared<std::atomic<bool>>(false);
    auto cancelToken = _testCancel;

    QPointer<SettingsDialog> self(this);
    _testThread = QThread::create([self, probe, cancelToken]() {
        QString message;
        bool ok = false;
        try {
            QbtClient client(probe, [](const QString &) {}, [cancelToken]() {
                return cancelToken ? cancelToken->load() : false;
            });
            client.login();
            client.fetchServerInfo();
            ok = true;
            message = tr("OK (%1, %2)").arg(client.appVersion, client.apiVersion);
        } catch (const QbtException &ex) {
            message = ex.message;
        } catch (const std::exception &ex) {
            message = QString::fromUtf8(ex.what());
        }

        if (!self)
            return;

        QMetaObject::invokeMethod(self, [self, ok, message]() {
            if (!self)
                return;
            setStatusLabelColor(self->_testResult, ok, self->palette());
            self->_testResult->setText(message);
            self->_testButton->setEnabled(true);
        }, Qt::QueuedConnection);
    });
    connect(_testThread, &QThread::finished, _testThread, &QObject::deleteLater);
    connect(_testThread, &QThread::finished, this, [this] { _testThread = nullptr; _testCancel.reset(); });
    _testThread->start();
}

void SettingsDialog::refreshHandlerStatus()
{
    HandlerInfo info;
    try {
        info = MagnetHandler::query();
    } catch (const std::exception &ex) {
        _handlerStatus->setText(tr("Query error: %1").arg(QString::fromUtf8(ex.what())));
        _handlerStatus->setPalette(QPalette());
        return;
    }

    QString state = info.isOurs
                        ? tr("qt-magnet is default handler.")
                        : tr("Current handler: %1.").arg(info.description);
    _handlerStatus->setText(state);

    if (info.isOurs) {
        setStatusLabelColor(_handlerStatus, true, palette());
    } else {
        _handlerStatus->setPalette(QPalette());
    }
}

void SettingsDialog::assignHandler()
{
    try {
        MagnetHandler::registerHandler();
        MagnetHandler::removeUserChoice();
        refreshHandlerStatus();
    } catch (const std::exception &ex) {
        Log::write(QStringLiteral("Assign handler error: %1").arg(QString::fromUtf8(ex.what())));
        QMessageBox::critical(this, QString::fromUtf8(kAppTitle),
                              tr("Registration failed:\n\n%1").arg(QString::fromUtf8(ex.what())));
    }
}

void SettingsDialog::removeHandler()
{
    try {
        bool removed = MagnetHandler::unregisterHandler();
        if (!removed)
            QMessageBox::information(this, QString::fromUtf8(kAppTitle),
                                     tr("No registry entries found."));
        refreshHandlerStatus();
    } catch (const std::exception &ex) {
        Log::write(QStringLiteral("Remove handler error: %1").arg(QString::fromUtf8(ex.what())));
        QMessageBox::critical(this, QString::fromUtf8(kAppTitle),
                              tr("Unregistration failed:\n\n%1").arg(QString::fromUtf8(ex.what())));
    }
}
