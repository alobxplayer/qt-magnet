#pragma once

#include <QString>
#include <QStringList>
#include "magnetlink.h"
#include "torrentfile.h"

class TorrentPayload {
public:
    enum class Kind {
        Magnet,
        TorrentFile
    };

    Kind            kind = Kind::Magnet;
    MagnetLink      magnet;
    TorrentFileData torrentData;

    bool isFile() const { return kind == Kind::TorrentFile; }
    bool isMagnet() const { return kind == Kind::Magnet; }

    QString hash() const;
    QString hashV2() const;
    TorrentVersion version() const;
    QString versionString() const;
    QSet<int> selectedIndices() const;
    QString toMagnetUri() const;
    QString displayName() const;
    QString prettyName() const;
    QStringList trackers() const;

    static TorrentPayload fromMagnet(const QString &uri);
    static TorrentPayload fromFile(const QString &filePath);
};

