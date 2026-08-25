#pragma once
#include <QString>

namespace SecretStore {
    QString store(const QString &account, const QString &plain);
    QString load(const QString &account, const QString &token);
    void    clear(const QString &account, const QString &token);
    bool    isSecure();
}
