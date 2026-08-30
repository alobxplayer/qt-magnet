#include "magnethandler.h"
#include "logger.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QUrl>

static const QString kProgIdMagnet       = QStringLiteral("QtMagnet.Magnet");
static const QString kMagnetClass        = QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\magnet");
static const QString kOurClassMagnet     = QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\QtMagnet.Magnet");
static const QString kUserChoiceMagnet   = QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\magnet\\UserChoice");

static const QString kProgIdTorrent      = QStringLiteral("QtMagnet.Torrent");
static const QString kTorrentExtClass    = QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\.torrent");
static const QString kOurClassTorrent    = QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\QtMagnet.Torrent");
static const QString kUserChoiceTorrent  = QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.torrent\\UserChoice");

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

static QString exePath()
{
    return QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
}

static QString openCommand(const QString &exe)
{
    return QLatin1Char('"') + exe + QStringLiteral("\" -- \"%1\"");
}

static QString exeFromCommand(const QString &command)
{
    if (command.isEmpty())
        return QString();
    QString s = command.trimmed();
    if (s.isEmpty())
        return QString();
    if (s.at(0) == QLatin1Char('"')) {
        int end = s.indexOf(QLatin1Char('"'), 1);
        return end > 1 ? s.mid(1, end - 1) : s.mid(1);
    }
    int sp = s.indexOf(QLatin1Char(' '));
    return sp > 0 ? s.left(sp) : s;
}

static bool isOurCommand(const QString &command)
{
    const QString exe = exeFromCommand(command);
    if (exe.isEmpty())
        return false;
    return QDir::toNativeSeparators(QFileInfo(exe).absoluteFilePath())
               .compare(QDir::toNativeSeparators(QFileInfo(exePath()).absoluteFilePath()),
                        Qt::CaseInsensitive) == 0;
}

static QString readCommand(const QString &regPath)
{
    QSettings reg(regPath + QStringLiteral("\\shell\\open\\command"), QSettings::NativeFormat);
    QString val = reg.value(QStringLiteral(".")).toString();
    if (val.isEmpty())
        val = reg.value(QStringLiteral("Default")).toString();
    return val;
}

static void writeHandler(const QString &regPath, const QString &title, const QString &exe, bool isUrlProtocol)
{
    QSettings k(regPath, QSettings::NativeFormat);
    k.setValue(QStringLiteral("."), title);
    if (isUrlProtocol)
        k.setValue(QStringLiteral("URL Protocol"), QString());
    k.setValue(QStringLiteral("FriendlyTypeName"), title);

    QSettings icon(regPath + QStringLiteral("\\DefaultIcon"), QSettings::NativeFormat);
    icon.setValue(QStringLiteral("."), QLatin1Char('"') + exe + QStringLiteral("\",0"));

    QSettings open(regPath + QStringLiteral("\\shell\\open"), QSettings::NativeFormat);
    open.setValue(QStringLiteral("FriendlyAppName"), QStringLiteral("qt-magnet"));

    QSettings cmd(regPath + QStringLiteral("\\shell\\open\\command"), QSettings::NativeFormat);
    cmd.setValue(QStringLiteral("."), openCommand(exe));
}

static void notifyShell()
{
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

static QString describe(const QString &progId, const QString &command)
{
    if (isOurCommand(command))
        return QCoreApplication::translate("MagnetHandler", "qt-magnet (this app)");
    const QString exe = exeFromCommand(command);
    if (exe.isEmpty())
        return progId;
    QString name = QFileInfo(exe).fileName();
    if (progId.compare(QLatin1String("magnet"), Qt::CaseInsensitive) == 0 ||
        progId.compare(QLatin1String(".torrent"), Qt::CaseInsensitive) == 0)
        return name;
    return name + QStringLiteral(" (") + progId + QLatin1Char(')');
}

namespace MagnetHandler {

void registerMagnetHandler()
{
    const QString exe = exePath();
    writeHandler(kMagnetClass, QStringLiteral("URL:Magnet Link"), exe, true);
    writeHandler(kOurClassMagnet, QStringLiteral("Magnet Link (qt-magnet)"), exe, true);
    notifyShell();
    Log::write(QStringLiteral("Registered magnet: to %1").arg(exe));
}

bool unregisterMagnetHandler()
{
    bool removed = false;

    if (isOurCommand(readCommand(kMagnetClass))) {
        QSettings(kMagnetClass, QSettings::NativeFormat).clear();
        removed = true;
    }

    QSettings ourReg(kOurClassMagnet, QSettings::NativeFormat);
    if (!ourReg.allKeys().isEmpty()) {
        ourReg.clear();
        removed = true;
    }

    if (removed)
        notifyShell();
    return removed;
}

HandlerInfo queryMagnet()
{
    HandlerInfo info;

    QString userChoice;
    {
        QSettings uc(kUserChoiceMagnet, QSettings::NativeFormat);
        userChoice = uc.value(QStringLiteral("ProgId")).toString();
    }

    if (!userChoice.isEmpty()) {
        QString cmd;
        cmd = readCommand(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\") + userChoice);
        if (cmd.isEmpty())
            cmd = readCommand(QStringLiteral("HKEY_CLASSES_ROOT\\") + userChoice);

        bool ours = userChoice.compare(kProgIdMagnet, Qt::CaseInsensitive) == 0 || isOurCommand(cmd);
        info.userChoiceProgId = userChoice;
        info.userChoiceOverride = !ours;
        info.isOurs = ours;
        info.progId = userChoice;
        info.command = cmd;
        info.description = describe(userChoice, cmd);
        return info;
    }

    QString effective = readCommand(kMagnetClass);
    if (effective.isEmpty())
        effective = readCommand(QStringLiteral("HKEY_CLASSES_ROOT\\magnet"));

    info.command = effective;
    info.isOurs = isOurCommand(effective);
    info.description = effective.isEmpty()
                           ? QCoreApplication::translate("MagnetHandler", "Not assigned")
                           : describe(QStringLiteral("magnet"), effective);
    return info;
}

bool removeMagnetUserChoice()
{
    QSettings uc(kUserChoiceMagnet, QSettings::NativeFormat);
    uc.clear();
    uc.sync();
    notifyShell();
    bool ok = uc.status() == QSettings::NoError;
    if (ok)
        Log::write(QStringLiteral("Removed UserChoice for magnet:"));
    return ok;
}

void registerTorrentHandler()
{
    const QString exe = exePath();
    {
        QSettings ext(kTorrentExtClass, QSettings::NativeFormat);
        ext.setValue(QStringLiteral("."), kProgIdTorrent);
        ext.setValue(QStringLiteral("Content Type"), QStringLiteral("application/x-bittorrent"));
    }
    writeHandler(kOurClassTorrent, QStringLiteral("Torrent File (qt-magnet)"), exe, false);
    notifyShell();
    Log::write(QStringLiteral("Registered .torrent to %1").arg(exe));
}

bool unregisterTorrentHandler()
{
    bool removed = false;

    QSettings ext(kTorrentExtClass, QSettings::NativeFormat);
    if (ext.value(QStringLiteral(".")).toString().compare(kProgIdTorrent, Qt::CaseInsensitive) == 0) {
        ext.clear();
        removed = true;
    }

    QSettings ourReg(kOurClassTorrent, QSettings::NativeFormat);
    if (!ourReg.allKeys().isEmpty()) {
        ourReg.clear();
        removed = true;
    }

    if (removed)
        notifyShell();
    return removed;
}

HandlerInfo queryTorrent()
{
    HandlerInfo info;

    QString userChoice;
    {
        QSettings uc(kUserChoiceTorrent, QSettings::NativeFormat);
        userChoice = uc.value(QStringLiteral("ProgId")).toString();
    }

    if (!userChoice.isEmpty()) {
        QString cmd;
        cmd = readCommand(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\") + userChoice);
        if (cmd.isEmpty())
            cmd = readCommand(QStringLiteral("HKEY_CLASSES_ROOT\\") + userChoice);

        bool ours = userChoice.compare(kProgIdTorrent, Qt::CaseInsensitive) == 0 || isOurCommand(cmd);
        info.userChoiceProgId = userChoice;
        info.userChoiceOverride = !ours;
        info.isOurs = ours;
        info.progId = userChoice;
        info.command = cmd;
        info.description = describe(userChoice, cmd);
        return info;
    }

    QSettings ext(kTorrentExtClass, QSettings::NativeFormat);
    QString progId = ext.value(QStringLiteral(".")).toString();
    if (progId.isEmpty()) {
        QSettings rootExt(QStringLiteral("HKEY_CLASSES_ROOT\\.torrent"), QSettings::NativeFormat);
        progId = rootExt.value(QStringLiteral(".")).toString();
    }

    QString cmd;
    if (!progId.isEmpty()) {
        cmd = readCommand(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\") + progId);
        if (cmd.isEmpty())
            cmd = readCommand(QStringLiteral("HKEY_CLASSES_ROOT\\") + progId);
    }

    info.command = cmd;
    info.isOurs = isOurCommand(cmd);
    info.description = progId.isEmpty()
                           ? QCoreApplication::translate("MagnetHandler", "Not assigned")
                           : describe(progId, cmd);
    return info;
}

bool removeTorrentUserChoice()
{
    QSettings uc(kUserChoiceTorrent, QSettings::NativeFormat);
    uc.clear();
    uc.sync();
    notifyShell();
    bool ok = uc.status() == QSettings::NoError;
    if (ok)
        Log::write(QStringLiteral("Removed UserChoice for .torrent"));
    return ok;
}

void registerAll()
{
    registerMagnetHandler();
    registerTorrentHandler();
}

bool unregisterAll()
{
    bool r1 = unregisterMagnetHandler();
    bool r2 = unregisterTorrentHandler();
    return r1 || r2;
}

void openSystemSettings()
{
    QDesktopServices::openUrl(QUrl(QStringLiteral("ms-settings:defaultapps")));
}

} // namespace MagnetHandler
