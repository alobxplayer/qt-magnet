#include "format.h"
#include <QCoreApplication>

namespace Format {

QString size(qint64 bytes)
{
    if (bytes < 0)
        return QStringLiteral("?");

    static const char *units[] = {
        QT_TRANSLATE_NOOP("Format", "B"),
        QT_TRANSLATE_NOOP("Format", "KiB"),
        QT_TRANSLATE_NOOP("Format", "MiB"),
        QT_TRANSLATE_NOOP("Format", "GiB"),
        QT_TRANSLATE_NOOP("Format", "TiB"),
        QT_TRANSLATE_NOOP("Format", "PiB")
    };
    constexpr int kMaxUnitIndex = int(sizeof(units) / sizeof(units[0])) - 1;
    double v = double(bytes);
    int u = 0;
    while (v >= 1024.0 && u < kMaxUnitIndex) {
        v /= 1024.0;
        ++u;
    }

    QString num;
    if (u == 0) {
        num = QString::number(bytes);
    } else {
        const int dec = v < 10.0 ? 2 : (v < 100.0 ? 1 : 0);
        num = QString::number(v, 'f', dec);
    }
    return num + QLatin1Char(' ') + QCoreApplication::translate("Format", units[u]);
}

QString priorityName(int priority)
{
    switch (priority) {
    case 0:  return QCoreApplication::translate("Format", "Do not download");
    case 6:  return QCoreApplication::translate("Format", "High");
    case 7:  return QCoreApplication::translate("Format", "Maximum");
    default: return QCoreApplication::translate("Format", "Normal");
    }
}

} // namespace Format
