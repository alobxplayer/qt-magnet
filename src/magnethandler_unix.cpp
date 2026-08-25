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

static QString queryXdgMime()
{
    QProcess proc;
    proc.start(QStringLiteral("xdg-mime"),
               {QStringLiteral("query"), QStringLiteral("default"), QStringLiteral("x-scheme-handler/magnet")});
    proc.waitForFinished(3000);
    return QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
}

namespace MagnetHandler {

void registerHandler()
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
        << "Comment=Add magnet links to torrent clients via WebUI/RPC\n"
        << "Exec=\"" << exe << "\" -- %u\n"
        << "Terminal=false\n"
        << "MimeType=x-scheme-handler/magnet;\n"
        << "NoDisplay=true\n";
    f.close();

    QProcess::execute(QStringLiteral("xdg-mime"),
                      {QStringLiteral("default"), QStringLiteral("qt-magnet.desktop"),
                       QStringLiteral("x-scheme-handler/magnet")});
    QProcess::execute(QStringLiteral("update-desktop-database"), {desktopDir()});
    Log::write(QStringLiteral("Registered magnet: to %1").arg(exe));
}

bool unregisterHandler()
{
    bool removed = false;
    if (QFile::exists(desktopPath())) {
        QFile::remove(desktopPath());
        removed = true;
        QProcess::execute(QStringLiteral("update-desktop-database"), {desktopDir()});
    }
    return removed;
}

HandlerInfo query()
{
    HandlerInfo info;
    const QString current = queryXdgMime();
    info.isOurs = current.compare(QLatin1String("qt-magnet.desktop"), Qt::CaseInsensitive) == 0;
    info.description = info.isOurs
                           ? QCoreApplication::translate("MagnetHandler", "qt-magnet (this app)")
                           : (current.isEmpty() ? QCoreApplication::translate("MagnetHandler", "Not assigned") : current);
    return info;
}

bool removeUserChoice()
{
    return false;
}

void openSystemSettings()
{
}

} // namespace MagnetHandler
