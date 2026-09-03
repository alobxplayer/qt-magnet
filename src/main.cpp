#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QLabel>
#include <QLibraryInfo>
#include <QLocale>
#include <QMessageBox>
#include <QTranslator>
#include <QUrl>
#include <QVBoxLayout>

#include "config.h"
#include "logger.h"
#include "magnetlink.h"
#include "torrentfile.h"
#include "torrentpayload.h"
#include "magnethandler.h"
#include "adddialog.h"
#include "settingsdialog.h"

static const char *kAppTitle = "qt-magnet";

static void messageHandler(QtMsgType, const QMessageLogContext &, const QString &msg)
{
    Log::write(QStringLiteral("Qt: ") + msg);
}

struct Options {
    QString targetInput;
    QString badArgument;
    bool doRegisterMagnet = false;
    bool doUnregisterMagnet = false;
    bool doRegisterTorrent = false;
    bool doUnregisterTorrent = false;
    bool settingsOnly = false;
    bool showHelp = false;
    int  quickOverride = -1;

    static Options parse(const QStringList &args) {
        Options o;
        QStringList rest;

        for (int i = 1; i < args.size(); ++i) {
            QString a = args[i].trimmed();
            if (a.isEmpty())
                continue;

            if (a == QLatin1String("--")) {
                for (int j = i + 1; j < args.size(); ++j) {
                    QString val = args[j].trimmed();
                    if (!val.isEmpty())
                        rest.append(val);
                }
                break;
            }

            if (!MagnetLink::looksLikeMagnet(a) && !TorrentFileData::looksLikeTorrent(a) && !QFile::exists(a) &&
                (a[0] == QLatin1Char('/') || a[0] == QLatin1Char('-'))) {
                QString key = a;
                while (!key.isEmpty() && (key[0] == QLatin1Char('/') || key[0] == QLatin1Char('-')))
                    key = key.mid(1);
                key = key.toLower();

                if (key == QLatin1String("register-magnet") || key == QLatin1String("reg-magnet"))
                    o.doRegisterMagnet = true;
                else if (key == QLatin1String("unregister-magnet") || key == QLatin1String("unreg-magnet"))
                    o.doUnregisterMagnet = true;
                else if (key == QLatin1String("register-torrent") || key == QLatin1String("reg-torrent"))
                    o.doRegisterTorrent = true;
                else if (key == QLatin1String("unregister-torrent") || key == QLatin1String("unreg-torrent"))
                    o.doUnregisterTorrent = true;
                else if (key == QLatin1String("register") || key == QLatin1String("reg")) {
                    o.doRegisterMagnet = true;
                    o.doRegisterTorrent = true;
                } else if (key == QLatin1String("unregister") || key == QLatin1String("unreg")) {
                    o.doUnregisterMagnet = true;
                    o.doUnregisterTorrent = true;
                } else if (key == QLatin1String("settings") || key == QLatin1String("config"))
                    o.settingsOnly = true;
                else if (key == QLatin1String("quick"))
                    o.quickOverride = 1;
                else if (key == QLatin1String("dialog") || key == QLatin1String("ui"))
                    o.quickOverride = 0;
                else if (key == QLatin1String("help") || key == QLatin1String("h") || key == QLatin1String("?"))
                    o.showHelp = true;
                else
                    Log::write(QStringLiteral("Unknown switch: %1").arg(a));
                continue;
            }
            rest.append(a);
        }

        if (!rest.isEmpty()) {
            QString joined = rest.join(QLatin1Char(' ')).trimmed();
            if (MagnetLink::looksLikeMagnet(joined) || TorrentFileData::looksLikeTorrent(joined) || QFile::exists(joined))
                o.targetInput = joined;
            else if (rest.size() == 1 && (MagnetLink::looksLikeMagnet(rest.first()) || TorrentFileData::looksLikeTorrent(rest.first()) || QFile::exists(rest.first())))
                o.targetInput = rest.first();
            else
                o.badArgument = joined;
        }
        return o;
    }
};

static QString shorten(const QString &s, int max)
{
    if (s.length() <= max)
        return s;
    return s.left(max) + QStringLiteral("...");
}

static bool loadTranslation(QApplication &app, QTranslator &appTranslator, QTranslator &qtTranslator, const QString &langChoice)
{
    QString lang = langChoice;
    if (lang.isEmpty() || lang == QLatin1String("system")) {
        lang = (QLocale::system().language() == QLocale::Russian) ? QStringLiteral("ru_RU") : QStringLiteral("en_US");
    }

    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList searchDirs = {
        appDir + QStringLiteral("/translations"),
        appDir,
        QStringLiteral(":/translations"),
        QLibraryInfo::path(QLibraryInfo::TranslationsPath)
    };

    QString qmName;
    if (lang == QLatin1String("en_US") || lang == QLatin1String("en")) {
        qmName = QStringLiteral("qt-magnet_en_US");
    } else if (lang == QLatin1String("ru_RU") || lang == QLatin1String("ru")) {
        qmName = QStringLiteral("qt-magnet_ru_RU");
    } else {
        qmName = QStringLiteral("qt-magnet_") + lang;
    }

    for (const QString &dir : searchDirs) {
        if (appTranslator.load(qmName, dir)) {
            app.installTranslator(&appTranslator);
            break;
        }
    }

    QString shortLang = lang.contains(QLatin1Char('_')) ? lang.split(QLatin1Char('_')).first() : lang;
    for (const QString &dir : searchDirs) {
        if (qtTranslator.load(QStringLiteral("qt_") + lang, dir) ||
            qtTranslator.load(QStringLiteral("qt_") + shortLang, dir) ||
            qtTranslator.load(QStringLiteral("qtbase_") + lang, dir) ||
            qtTranslator.load(QStringLiteral("qtbase_") + shortLang, dir)) {
            app.installTranslator(&qtTranslator);
            break;
        }
    }
    return true;
}

static QString askFirstRunLanguage()
{
    QString detectedLang = (QLocale::system().language() == QLocale::Russian) ? QStringLiteral("ru_RU") : QStringLiteral("en_US");

    QDialog dlg;
    dlg.setWindowTitle(QStringLiteral("Language / Язык"));

    auto *layout = new QVBoxLayout(&dlg);
    layout->setSizeConstraint(QLayout::SetFixedSize);
    auto *label = new QLabel(QStringLiteral("Select language / Выберите язык:"), &dlg);
    layout->addWidget(label);

    auto *combo = new QComboBox(&dlg);
    combo->addItem(QStringLiteral("Русский (ru_RU)"), QStringLiteral("ru_RU"));
    combo->addItem(QStringLiteral("English (en_US)"), QStringLiteral("en_US"));
    label->setBuddy(combo);

    int defaultIndex = combo->findData(detectedLang);
    if (defaultIndex >= 0)
        combo->setCurrentIndex(defaultIndex);

    layout->addWidget(combo);

    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok, &dlg);
    QObject::connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    layout->addWidget(box);

    dlg.exec();
    return combo->currentData().toString();
}

static void showHelp()
{
    QString text;
    text += QCoreApplication::translate("main", "qt-magnet - Add magnet links and torrent files to torrent clients via WebUI/RPC.\n\n");
    text += QCoreApplication::translate("main", "Usage:\n");
    text += QCoreApplication::translate("main", "  qt-magnet \"magnet:?xt=urn:btih:...\"     Add magnet link\n");
    text += QCoreApplication::translate("main", "  qt-magnet \"/path/to/file.torrent\"       Add torrent file\n");
    text += QCoreApplication::translate("main", "  qt-magnet                              Open settings\n\n");
    text += QCoreApplication::translate("main", "Options:\n");
    text += QCoreApplication::translate("main", "  /register            Register both magnet: and .torrent handlers\n");
    text += QCoreApplication::translate("main", "  /unregister          Unregister both handlers\n");
    text += QCoreApplication::translate("main", "  /register-magnet     Register default magnet: handler\n");
    text += QCoreApplication::translate("main", "  /unregister-magnet   Unregister magnet: handler\n");
    text += QCoreApplication::translate("main", "  /register-torrent    Register default .torrent handler\n");
    text += QCoreApplication::translate("main", "  /unregister-torrent  Unregister .torrent handler\n");
    text += QCoreApplication::translate("main", "  /settings            Open settings dialog\n");
    text += QCoreApplication::translate("main", "  /quick               Quick mode (skip dialog)\n");
    text += QCoreApplication::translate("main", "  /dialog              Show dialog\n");
    text += QCoreApplication::translate("main", "  /help                Show this help\n\n");
    text += QCoreApplication::translate("main", "Config: ") + Config::filePath() + QLatin1Char('\n');
    text += QCoreApplication::translate("main", "Log:    ") + Log::filePath();
    QMessageBox::information(nullptr, QString::fromUtf8(kAppTitle), text);
}

static int runRegisterMagnet()
{
    try {
        MagnetHandler::registerMagnetHandler();
        MagnetHandler::removeMagnetUserChoice();
        QMessageBox::information(nullptr, QString::fromUtf8(kAppTitle),
                                 QCoreApplication::translate("main", "Registered as default magnet: handler."));
        return 0;
    } catch (const std::exception &ex) {
        Log::write(QStringLiteral("Magnet registration failed: %1").arg(QString::fromUtf8(ex.what())));
        QMessageBox::critical(nullptr, QString::fromUtf8(kAppTitle),
                              QCoreApplication::translate("main", "Magnet registration failed:\n\n%1").arg(QString::fromUtf8(ex.what())));
        return 1;
    }
}

static int runUnregisterMagnet()
{
    try {
        bool removed = MagnetHandler::unregisterMagnetHandler();
        QMessageBox::information(nullptr, QString::fromUtf8(kAppTitle),
                                 removed ? QCoreApplication::translate("main", "Magnet registration removed.")
                                         : QCoreApplication::translate("main", "No magnet registry entries found."));
        return 0;
    } catch (const std::exception &ex) {
        Log::write(QStringLiteral("Magnet unregistration failed: %1").arg(QString::fromUtf8(ex.what())));
        QMessageBox::critical(nullptr, QString::fromUtf8(kAppTitle),
                              QCoreApplication::translate("main", "Magnet unregistration failed:\n\n%1").arg(QString::fromUtf8(ex.what())));
        return 1;
    }
}

static int runRegisterTorrent()
{
    try {
        MagnetHandler::registerTorrentHandler();
        MagnetHandler::removeTorrentUserChoice();
        QMessageBox::information(nullptr, QString::fromUtf8(kAppTitle),
                                 QCoreApplication::translate("main", "Registered as default .torrent file handler."));
        return 0;
    } catch (const std::exception &ex) {
        Log::write(QStringLiteral("Torrent registration failed: %1").arg(QString::fromUtf8(ex.what())));
        QMessageBox::critical(nullptr, QString::fromUtf8(kAppTitle),
                              QCoreApplication::translate("main", "Torrent registration failed:\n\n%1").arg(QString::fromUtf8(ex.what())));
        return 1;
    }
}

static int runUnregisterTorrent()
{
    try {
        bool removed = MagnetHandler::unregisterTorrentHandler();
        QMessageBox::information(nullptr, QString::fromUtf8(kAppTitle),
                                 removed ? QCoreApplication::translate("main", "Torrent file registration removed.")
                                         : QCoreApplication::translate("main", "No torrent registry entries found."));
        return 0;
    } catch (const std::exception &ex) {
        Log::write(QStringLiteral("Torrent unregistration failed: %1").arg(QString::fromUtf8(ex.what())));
        QMessageBox::critical(nullptr, QString::fromUtf8(kAppTitle),
                              QCoreApplication::translate("main", "Torrent unregistration failed:\n\n%1").arg(QString::fromUtf8(ex.what())));
        return 1;
    }
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("QtMagnet"));
    QApplication::setApplicationName(QStringLiteral("qt-magnet"));
    QApplication::setApplicationVersion(QStringLiteral("1.1.0"));

    qInstallMessageHandler(messageHandler);
    Log::write(QStringLiteral("Startup: %1").arg(app.arguments().join(QLatin1Char(' '))));

    Config cfg = Config::load();
    Options opt = Options::parse(app.arguments());

    bool isNonInteractive = opt.showHelp || opt.doRegisterMagnet || opt.doUnregisterMagnet ||
                            opt.doRegisterTorrent || opt.doUnregisterTorrent ||
                            (opt.quickOverride == 1 || (opt.quickOverride < 0 && cfg.quickMode && !opt.targetInput.isEmpty() && cfg.isComplete()));

    if (cfg.language.isEmpty()) {
        if (!isNonInteractive) {
            cfg.language = askFirstRunLanguage();
            try { cfg.save(); } catch (const std::exception &ex) { Log::write(QStringLiteral("Failed to save initial config: %1").arg(QString::fromUtf8(ex.what()))); }
        } else {
            cfg.language = (QLocale::system().language() == QLocale::Russian) ? QStringLiteral("ru_RU") : QStringLiteral("en_US");
            try { cfg.save(); } catch (const std::exception &ex) { Log::write(QStringLiteral("Failed to save initial config: %1").arg(QString::fromUtf8(ex.what()))); }
        }
    }

    QTranslator appTranslator;
    QTranslator qtTranslator;
    loadTranslation(app, appTranslator, qtTranslator, cfg.language);

    if (opt.showHelp) { showHelp(); return 0; }
    if (opt.doRegisterMagnet && opt.doRegisterTorrent) {
        int r1 = runRegisterMagnet();
        int r2 = runRegisterTorrent();
        return (r1 != 0) ? r1 : r2;
    }
    if (opt.doUnregisterMagnet && opt.doUnregisterTorrent) {
        int r1 = runUnregisterMagnet();
        int r2 = runUnregisterTorrent();
        return (r1 != 0) ? r1 : r2;
    }
    if (opt.doRegisterMagnet) return runRegisterMagnet();
    if (opt.doUnregisterMagnet) return runUnregisterMagnet();
    if (opt.doRegisterTorrent) return runRegisterTorrent();
    if (opt.doUnregisterTorrent) return runUnregisterTorrent();

    if (opt.targetInput.isEmpty() || opt.settingsOnly) {
        if (opt.targetInput.isEmpty() && !opt.badArgument.isEmpty()) {
            QMessageBox::warning(nullptr, QString::fromUtf8(kAppTitle),
                                 QCoreApplication::translate("main", "Invalid link or file:\n\n%1\n\nOpening settings.")
                                     .arg(shorten(opt.badArgument, 300)));
        }
        SettingsDialog dlg(cfg);
        dlg.exec();
        return 0;
    }

    TorrentPayload payload;
    try {
        if (MagnetLink::looksLikeMagnet(opt.targetInput))
            payload = TorrentPayload::fromMagnet(opt.targetInput);
        else
            payload = TorrentPayload::fromFile(opt.targetInput);
    } catch (const std::exception &ex) {
        Log::write(QStringLiteral("Failed to parse link/file: %1").arg(QString::fromUtf8(ex.what())));
        QMessageBox::warning(nullptr, QString::fromUtf8(kAppTitle),
                             QCoreApplication::translate("main", "Failed to parse magnet link or torrent file:\n\n%1").arg(QString::fromUtf8(ex.what())));
        return 2;
    }

    if (!cfg.isComplete()) {
        QMessageBox::information(nullptr, QString::fromUtf8(kAppTitle),
                                 QCoreApplication::translate("main", "Configure server connection details first."));
        SettingsDialog dlg(cfg);
        if (dlg.exec() != QDialog::Accepted)
            return 3;
        cfg = Config::load();
        if (!cfg.isComplete())
            return 3;
    }

    bool quick = opt.quickOverride >= 0 ? (opt.quickOverride == 1) : cfg.quickMode;

    AddDialog add(cfg, payload, quick);
    add.show();
    app.exec();
    return add.exitCode();
}
