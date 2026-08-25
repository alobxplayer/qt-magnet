#include "magnethandler.h"
#include "logger.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QUrl>

static const QString kProgId       = QStringLiteral("QtMagnet.Magnet");
static const QString kMagnetClass  = QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\magnet");
static const QString kOurClass     = QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\QtMagnet.Magnet");
static const QString kUserChoice   = QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\magnet\\UserChoice");

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

static void writeHandler(const QString &regPath, const QString &title, const QString &exe)
{
    QSettings k(regPath, QSettings::NativeFormat);
    k.setValue(QStringLiteral("."), title);
    k.setValue(QStringLiteral("URL Protocol"), QString());
    k.setValue(QStringLiteral("FriendlyTypeName"), QStringLiteral("Magnet Link"));

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
    if (progId.compare(QLatin1String("magnet"), Qt::CaseInsensitive) == 0)
        return name;
    return name + QStringLiteral(" (") + progId + QLatin1Char(')');
}

namespace MagnetHandler {

void registerHandler()
{
    const QString exe = exePath();
    writeHandler(kMagnetClass, QStringLiteral("URL:Magnet Link"), exe);
    writeHandler(kOurClass, QStringLiteral("Magnet Link (qt-magnet)"), exe);
    notifyShell();
    Log::write(QStringLiteral("Registered magnet: to %1").arg(exe));
}

bool unregisterHandler()
{
    bool removed = false;

    if (isOurCommand(readCommand(kMagnetClass))) {
        QSettings(kMagnetClass, QSettings::NativeFormat).clear();
        removed = true;
    }

    QSettings ourReg(kOurClass, QSettings::NativeFormat);
    if (!ourReg.allKeys().isEmpty()) {
        ourReg.clear();
        removed = true;
    }

    if (removed)
        notifyShell();
    return removed;
}

HandlerInfo query()
{
    HandlerInfo info;

    QString userChoice;
    {
        QSettings uc(kUserChoice, QSettings::NativeFormat);
        userChoice = uc.value(QStringLiteral("ProgId")).toString();
    }

    if (!userChoice.isEmpty()) {
        QString cmd;
        cmd = readCommand(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\") + userChoice);
        if (cmd.isEmpty())
            cmd = readCommand(QStringLiteral("HKEY_CLASSES_ROOT\\") + userChoice);

        bool ours = userChoice.compare(kProgId, Qt::CaseInsensitive) == 0 || isOurCommand(cmd);
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

bool removeUserChoice()
{
    QSettings uc(kUserChoice, QSettings::NativeFormat);
    uc.clear();
    uc.sync();
    notifyShell();
    bool ok = uc.status() == QSettings::NoError;
    if (ok)
        Log::write(QStringLiteral("Removed UserChoice for magnet:"));
    else
        Log::write(QStringLiteral("Failed to remove UserChoice"));
    return ok;
}

void openSystemSettings()
{
    QDesktopServices::openUrl(QUrl(QStringLiteral("ms-settings:defaultapps")));
}

} // namespace MagnetHandler
