#include "magnethandler.h"
#include "logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTextStream>

static QString desktopDir()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
    if (dir.isEmpty())
        dir = QDir::homePath() + QStringLiteral("/.local/share/applications");
    return dir;
}

static QString desktopPath()
{
    return desktopDir() + QStringLiteral("/qt-magnet.desktop");
}

static void ensureDesktopFile()
{
    const QString exe = QCoreApplication::applicationFilePath();
    QDir().mkpath(desktopDir());

    QFile f(desktopPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        Log::write(QStringLiteral("Failed to create .desktop: %1").arg(f.errorString()));
        return;
    }
    QTextStream out(&f);
    out << "[Desktop Entry]\n"
        << "Type=Application\n"
        << "Name=qt-magnet\n"
        << "Comment=Add magnet links and torrent files to torrent clients via WebUI/RPC\n"
        << "Exec=\"" << exe << "\" -- %U\n"
        << "Terminal=false\n"
        << "MimeType=x-scheme-handler/magnet;application/x-bittorrent;\n"
        << "NoDisplay=true\n";
    f.close();
}

static QString queryXdgMime(const QString &mime)
{
    QProcess proc;
    proc.start(QStringLiteral("xdg-mime"),
               {QStringLiteral("query"), QStringLiteral("default"), mime});
    proc.waitForFinished(3000);
    return QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
}

static void removeMimeAssociation(const QString &mime)
{
    QString configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    if (configDir.isEmpty())
        configDir = QDir::homePath() + QStringLiteral("/.config");
    QString mimeFile = configDir + QStringLiteral("/mimeapps.list");
    QFile f(mimeFile);
    if (!f.open(QIODevice::ReadOnly))
        return;
    QString content = QString::fromUtf8(f.readAll());
    f.close();

    QStringList lines = content.split(QLatin1Char('\n'));
    QStringList newLines;
    const QString targetPrefix = mime + QLatin1Char('=');
    bool modified = false;
    for (const QString &line : lines) {
        QString trimmed = line.trimmed();
        if (trimmed.startsWith(targetPrefix)) {
            QString val = trimmed.mid(targetPrefix.length());
            QStringList apps = val.split(QLatin1Char(';'), Qt::SkipEmptyParts);
            apps.removeAll(QStringLiteral("qt-magnet.desktop"));
            if (apps.isEmpty()) {
                modified = true;
                continue;
            }
            newLines.append(targetPrefix + apps.join(QLatin1Char(';')) + QLatin1Char(';'));
            modified = true;
        } else {
            newLines.append(line);
        }
    }
    if (modified) {
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(newLines.join(QLatin1Char('\n')).toUtf8());
            f.close();
        }
    }
}

namespace MagnetHandler {

void registerMagnetHandler()
{
    ensureDesktopFile();
    QProcess::execute(QStringLiteral("xdg-mime"),
                      {QStringLiteral("default"), QStringLiteral("qt-magnet.desktop"),
                       QStringLiteral("x-scheme-handler/magnet")});
    QProcess::execute(QStringLiteral("update-desktop-database"), {desktopDir()});
    Log::write(QStringLiteral("Registered magnet: handler"));
}

bool unregisterMagnetHandler()
{
    bool isOurs = queryMagnet().isOurs;
    if (isOurs) {
        removeMimeAssociation(QStringLiteral("x-scheme-handler/magnet"));
        QProcess::execute(QStringLiteral("update-desktop-database"), {desktopDir()});
        Log::write(QStringLiteral("Unregistered magnet: handler"));
    }
    return isOurs;
}

HandlerInfo queryMagnet()
{
    HandlerInfo info;
    const QString current = queryXdgMime(QStringLiteral("x-scheme-handler/magnet"));
    info.isOurs = current.compare(QLatin1String("qt-magnet.desktop"), Qt::CaseInsensitive) == 0;
    info.description = info.isOurs
                           ? QCoreApplication::translate("MagnetHandler", "qt-magnet (this app)")
                           : (current.isEmpty() ? QCoreApplication::translate("MagnetHandler", "Not assigned") : current);
    return info;
}

bool removeMagnetUserChoice()
{
    return false;
}

void registerTorrentHandler()
{
    ensureDesktopFile();
    QProcess::execute(QStringLiteral("xdg-mime"),
                      {QStringLiteral("default"), QStringLiteral("qt-magnet.desktop"),
                       QStringLiteral("application/x-bittorrent")});
    QProcess::execute(QStringLiteral("update-desktop-database"), {desktopDir()});
    Log::write(QStringLiteral("Registered .torrent handler"));
}

bool unregisterTorrentHandler()
{
    bool isOurs = queryTorrent().isOurs;
    if (isOurs) {
        removeMimeAssociation(QStringLiteral("application/x-bittorrent"));
        QProcess::execute(QStringLiteral("update-desktop-database"), {desktopDir()});
        Log::write(QStringLiteral("Unregistered .torrent handler"));
    }
    return isOurs;
}

HandlerInfo queryTorrent()
{
    HandlerInfo info;
    const QString current = queryXdgMime(QStringLiteral("application/x-bittorrent"));
    info.isOurs = current.compare(QLatin1String("qt-magnet.desktop"), Qt::CaseInsensitive) == 0;
    info.description = info.isOurs
                           ? QCoreApplication::translate("MagnetHandler", "qt-magnet (this app)")
                           : (current.isEmpty() ? QCoreApplication::translate("MagnetHandler", "Not assigned") : current);
    return info;
}

bool removeTorrentUserChoice()
{
    return false;
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
    if (QFile::exists(desktopPath())) {
        QFile::remove(desktopPath());
        QProcess::execute(QStringLiteral("update-desktop-database"), {desktopDir()});
    }
    return r1 || r2;
}

void openSystemSettings()
{
}

} // namespace MagnetHandler
