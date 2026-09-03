#include "torrentpayload.h"

QString TorrentPayload::hash() const
{
    return isFile() ? torrentData.hash : magnet.hash;
}

QString TorrentPayload::hashV2() const
{
    return isFile() ? torrentData.hashV2 : magnet.hashV2;
}

TorrentVersion TorrentPayload::version() const
{
    return isFile() ? torrentData.version : magnet.version;
}

QString TorrentPayload::versionString() const
{
    return isFile() ? torrentData.versionString() : magnet.versionString();
}

QSet<int> TorrentPayload::selectedIndices() const
{
    return isMagnet() ? magnet.selectedIndices : QSet<int>();
}

QString TorrentPayload::toMagnetUri() const
{
    return isFile() ? torrentData.toMagnetUri() : magnet.raw;
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
