#include "logger.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QStandardPaths>

namespace {
constexpr qint64 kMaxBytes = 512 * 1024;

QString logDir()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (dir.isEmpty())
        dir = QDir::tempPath() + QStringLiteral("/QtMagnet");
    return dir;
}
}

namespace Log {

QString filePath()
{
    return logDir() + QStringLiteral("/qt-magnet.log");
}

void write(const QString &message)
{
    static QMutex mutex;
    QMutexLocker locker(&mutex);

    QDir().mkpath(logDir());

    const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    const QString line = ts + QStringLiteral("  [") + QString::number(QCoreApplication::applicationPid())
                         + QStringLiteral("]  ") + message + QLatin1Char('\n');

    const QString path = filePath();
    QFile f(path);

    if (f.exists() && f.size() > kMaxBytes) {
        if (f.open(QIODevice::ReadOnly)) {
            QString all = QString::fromUtf8(f.readAll());
            f.close();
            int idx = all.indexOf(QLatin1Char('\n'), all.length() / 2);
            all = (idx >= 0) ? all.mid(idx + 1) : all.mid(all.length() / 2);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                f.write(all.toUtf8());
                f.close();
            }
        }
    }

    if (f.open(QIODevice::Append | QIODevice::WriteOnly)) {
        f.write(line.toUtf8());
        f.close();
    }
}

} // namespace Log
