#pragma once
#include <QString>
#include <QStringList>

class MagnetLink {
public:
    QString     raw;
    QString     hash;
    QString     hashV2;
    QString     displayName;
    QStringList trackers;

    QString prettyName() const;
    static bool looksLikeMagnet(const QString &s);
    static MagnetLink parse(const QString &uri);
    static QString base32ToHex(const QString &b32);
};
