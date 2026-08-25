#pragma once
#include <QString>

namespace Log {
    QString filePath();
    void write(const QString &message);
}
