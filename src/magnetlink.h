#pragma once
#include <QString>
#include <QStringList>
#include <QSet>
#include "torrentfile.h"

class MagnetLink {
public:
    QString        raw;
    TorrentVersion version = TorrentVersion::V1;
    QString        hash;
    QString        hashV2;
    QString        displayName;
    QStringList    trackers;
    QSet<int>      selectedIndices;

    QString versionString() const;
    QString prettyName() const;
    static bool looksLikeMagnet(const QString &s);
    static MagnetLink parse(const QString &uri);
    static QString base32ToHex(const QString &b32);
    static QSet<int> parseSelectOnly(const QString &soStr);
};

