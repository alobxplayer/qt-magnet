#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QByteArray>
#include "torrenttypes.h"

enum class TorrentVersion {
    V1,
    V2,
    Hybrid
};

inline QString torrentVersionString(TorrentVersion version)
{
    switch (version) {
    case TorrentVersion::V1:     return QStringLiteral("v1");
    case TorrentVersion::V2:     return QStringLiteral("v2");
    case TorrentVersion::Hybrid: return QStringLiteral("Hybrid (v1+v2)");
    }
    return QStringLiteral("v1");
}

class TorrentFileData {
public:
    QString              filePath;
    QByteArray           rawData;
    TorrentVersion       version = TorrentVersion::V1;
    QString              hash;
    QString              hashV2;
    QString              displayName;
    QString              comment;
    QStringList          trackers;
    QVector<TorrentFile> files;
    qint64               totalSize = 0;
    qint64               pieceLength = 0;
    int                  metaVersion = 1;

    QString versionString() const;
    bool isHybrid() const { return version == TorrentVersion::Hybrid; }
    bool isV2() const { return version == TorrentVersion::V2; }
    bool isV1() const { return version == TorrentVersion::V1; }

    QString toMagnetUri() const;
    QString prettyName() const;
    static bool looksLikeTorrent(const QString &str);
    static TorrentFileData parse(const QByteArray &bytes, const QString &sourcePath = QString());
    static TorrentFileData loadFromFile(const QString &filePath);
};

