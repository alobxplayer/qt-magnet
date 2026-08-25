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
#include "magnethandler.h"
#include "adddialog.h"
#include "settingsdialog.h"

static const char *kAppTitle = "qt-magnet";

static void messageHandler(QtMsgType, const QMessageLogContext &, const QString &msg)
{
    Log::write(QStringLiteral("Qt: ") + msg);
}

struct Options {
    QString magnet;
    QString badArgument;
    bool doRegister = false;
    bool doUnregister = false;
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

            if ((a[0] == QLatin1Char('/') || (a[0] == QLatin1Char('-') && !MagnetLink::looksLikeMagnet(a)))) {
                QString key = a;
                while (!key.isEmpty() && (key[0] == QLatin1Char('/') || key[0] == QLatin1Char('-')))
                    key = key.mid(1);
                key = key.toLower();

                if (key == QLatin1String("register") || key == QLatin1String("reg"))
                    o.doRegister = true;
                else if (key == QLatin1String("unregister") || key == QLatin1String("unreg"))
                    o.doUnregister = true;
                else if (key == QLatin1String("settings") || key == QLatin1String("config"))
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
            if (MagnetLink::looksLikeMagnet(joined))
                o.magnet = joined;
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
    text += QCoreApplication::translate("main", "qt-magnet - Add magnet links to torrent clients via WebUI/RPC.\n\n");
    text += QCoreApplication::translate("main", "Usage:\n");
    text += QCoreApplication::translate("main", "  qt-magnet \"magnet:?xt=urn:btih:...\"   Add magnet link\n");
    text += QCoreApplication::translate("main", "  qt-magnet                            Open settings\n\n");
    text += QCoreApplication::translate("main", "Options:\n");
    text += QCoreApplication::translate("main", "  /register     Register as default magnet: handler\n");
    text += QCoreApplication::translate("main", "  /unregister   Unregister magnet: handler\n");
    text += QCoreApplication::translate("main", "  /settings     Open settings dialog\n");
    text += QCoreApplication::translate("main", "  /quick        Quick mode (skip dialog)\n");
    text += QCoreApplication::translate("main", "  /dialog       Show dialog\n");
    text += QCoreApplication::translate("main", "  /help         Show this help\n\n");
    text += QCoreApplication::translate("main", "Config: ") + Config::filePath() + QLatin1Char('\n');
    text += QCoreApplication::translate("main", "Log:    ") + Log::filePath();
    QMessageBox::information(nullptr, QString::fromUtf8(kAppTitle), text);
}

static int runRegister()
{
    try {
        MagnetHandler::registerHandler();
        MagnetHandler::removeUserChoice();
        QMessageBox::information(nullptr, QString::fromUtf8(kAppTitle),
                                 QCoreApplication::translate("main", "Registered as default magnet: handler."));
        return 0;
    } catch (const std::exception &ex) {
        Log::write(QStringLiteral("Registration failed: %1").arg(QString::fromUtf8(ex.what())));
        QMessageBox::critical(nullptr, QString::fromUtf8(kAppTitle),
                              QCoreApplication::translate("main", "Registration failed:\n\n%1").arg(QString::fromUtf8(ex.what())));
        return 1;
    }
}

static int runUnregister()
{
    try {
        bool removed = MagnetHandler::unregisterHandler();
        QMessageBox::information(nullptr, QString::fromUtf8(kAppTitle),
                                 removed ? QCoreApplication::translate("main", "Registration removed.")
                                         : QCoreApplication::translate("main", "No registry entries found."));
        return 0;
    } catch (const std::exception &ex) {
        Log::write(QStringLiteral("Unregistration failed: %1").arg(QString::fromUtf8(ex.what())));
        QMessageBox::critical(nullptr, QString::fromUtf8(kAppTitle),
                              QCoreApplication::translate("main", "Unregistration failed:\n\n%1").arg(QString::fromUtf8(ex.what())));
        return 1;
    }
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("QtMagnet"));
    QApplication::setApplicationName(QStringLiteral("qt-magnet"));
    QApplication::setApplicationVersion(QStringLiteral("1.0.0"));

    qInstallMessageHandler(messageHandler);
    Log::write(QStringLiteral("Startup: %1").arg(app.arguments().join(QLatin1Char(' '))));

    Config cfg = Config::load();

    if (cfg.language.isEmpty()) {
        cfg.language = askFirstRunLanguage();
        try { cfg.save(); } catch (const std::exception &ex) { Log::write(QStringLiteral("Failed to save initial config: %1").arg(QString::fromUtf8(ex.what()))); }
    }

    QTranslator appTranslator;
    QTranslator qtTranslator;
    loadTranslation(app, appTranslator, qtTranslator, cfg.language);

    Options opt = Options::parse(app.arguments());

    if (opt.showHelp) { showHelp(); return 0; }
    if (opt.doRegister) return runRegister();
    if (opt.doUnregister) return runUnregister();

    if (opt.magnet.isEmpty() || opt.settingsOnly) {
        if (opt.magnet.isEmpty() && !opt.badArgument.isEmpty()) {
            QMessageBox::warning(nullptr, QString::fromUtf8(kAppTitle),
                                 QCoreApplication::translate("main", "Invalid magnet link:\n\n%1\n\nOpening settings.")
                                     .arg(shorten(opt.badArgument, 300)));
        }
        SettingsDialog dlg(cfg);
        dlg.exec();
        return 0;
    }

    MagnetLink link;
    try {
        link = MagnetLink::parse(opt.magnet);
    } catch (const std::exception &ex) {
        Log::write(QStringLiteral("Failed to parse link: %1").arg(QString::fromUtf8(ex.what())));
        QMessageBox::warning(nullptr, QString::fromUtf8(kAppTitle),
                             QCoreApplication::translate("main", "Failed to parse magnet link:\n\n%1").arg(QString::fromUtf8(ex.what())));
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

    AddDialog add(cfg, link, quick);
    add.show();
    app.exec();
    return add.exitCode();
}
