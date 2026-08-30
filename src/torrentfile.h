#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QByteArray>
#include "qbtclient.h"

class TorrentFileData {
public:
    QString              filePath;
    QByteArray           rawData;
    QString              hash;
    QString              hashV2;
    QString              displayName;
    QString              comment;
    QStringList          trackers;
    QVector<TorrentFile> files;
    qint64               totalSize = 0;

    QString prettyName() const;
    static bool looksLikeTorrent(const QString &str);
    static TorrentFileData parse(const QByteArray &bytes, const QString &sourcePath = QString());
    static TorrentFileData loadFromFile(const QString &filePath);
};
