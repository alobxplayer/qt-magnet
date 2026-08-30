#include "torrentpayload.h"

QString TorrentPayload::hash() const
{
    return isFile() ? torrentData.hash : magnet.hash;
}

QString TorrentPayload::displayName() const
{
    return isFile() ? torrentData.displayName : magnet.displayName;
}

QString TorrentPayload::prettyName() const
{
    return isFile() ? torrentData.prettyName() : magnet.prettyName();
}

QStringList TorrentPayload::trackers() const
{
    return isFile() ? torrentData.trackers : magnet.trackers;
}

TorrentPayload TorrentPayload::fromMagnet(const QString &uri)
{
    TorrentPayload p;
    p.kind = Kind::Magnet;
    p.magnet = MagnetLink::parse(uri);
    return p;
}

TorrentPayload TorrentPayload::fromFile(const QString &filePath)
{
    TorrentPayload p;
    p.kind = Kind::TorrentFile;
    p.torrentData = TorrentFileData::loadFromFile(filePath);
    return p;
}
