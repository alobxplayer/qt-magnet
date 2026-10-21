#pragma once

#include <QString>
#include <QVector>
#include <QJsonObject>
#include <QMetaType>

struct TorrentFile {
    int     index = 0;
    QString name;
    qint64  size = 0;
    int     priority = 1;
    double  progress = 0;
};

struct TorrentInfo {
    QString hash, name, state, savePath, category, tags;
    QString infoHashV1, infoHashV2;
    bool    forceStart = false, sequentialDownload = false, firstLastPiecePrio = false, autoTmm = false;
    qint64  size = 0, totalSize = 0;
    double  progress = 0;

    bool isStopped() const {
        return state == QLatin1String("pausedDL") || state == QLatin1String("pausedUP")
               || state == QLatin1String("stoppedDL") || state == QLatin1String("stoppedUP");
    }
    bool isForced() const {
        return state == QLatin1String("forcedDL") || state == QLatin1String("forcedUP")
               || state == QLatin1String("forcedMetaDL");
    }
    static TorrentInfo from(const QJsonObject &d);
};

Q_DECLARE_METATYPE(TorrentFile)
Q_DECLARE_METATYPE(TorrentInfo)
Q_DECLARE_METATYPE(QVector<TorrentFile>)

// Server-optional operations: the UI disables and the worker skips the
// unsupported ones instead of sending them and failing.
struct BackendCaps {
    bool setCategory = false;
    bool setTags = false;
    bool addTrackers = false;
};
