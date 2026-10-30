#include "torrentbackend.h"
#include "config.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace {

namespace TransmissionStatus {
constexpr int Paused = 0;
constexpr int CheckWait = 1;
constexpr int Checking = 2;
constexpr int DownloadWait = 3;
constexpr int Downloading = 4;
constexpr int SeedWait = 5;
constexpr int Seeding = 6;
} // namespace TransmissionStatus

// Transmission has no categories: both the category and the tags map onto
// its flat "labels" list.
QJsonArray transmissionLabels(const QString &category, const QString &tags)
{
    QStringList all;
    if (!category.isEmpty())
        all.append(category);
    all += splitTagList(tags);
    QJsonArray arr;
    for (const QString &l : all)
        arr.append(l);
    return arr;
}

} // namespace

// ---------------------------------------------------------------- login ----

void TransmissionBackend::login()
{
    const QJsonObject &res = session();
    appVersion = QStringLiteral("Transmission ") + res.value(QStringLiteral("version")).toString();
    apiVersion = QStringLiteral("RPC ") + QString::number(res.value(QStringLiteral("rpc-version")).toInt());
    _sh.trace(QStringLiteral("Logged in to %1 (%2)").arg(appVersion, apiVersion));
}

void TransmissionBackend::fetchServerInfo()
{
    login();
}

// ------------------------------------------------------------------ rpc ----

QJsonObject TransmissionBackend::execTransmissionRpc(const QString &method, const QJsonObject &arguments)
{
    QJsonObject reqObj;
    reqObj[QStringLiteral("method")] = method;
    if (!arguments.isEmpty())
        reqObj[QStringLiteral("arguments")] = arguments;
    reqObj[QStringLiteral("tag")] = ++_tag;

    const QByteArray body = QJsonDocument(reqObj).toJson(QJsonDocument::Compact);
    ClientTransport::RawResponse r;

    for (int attempt = 0; attempt < 2; ++attempt) {
        r = _sh.execRaw([this, &body] {
            QNetworkRequest req{ QUrl(_sh.base + QStringLiteral("/transmission/rpc")) };
            _sh.prepare(req);
            req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
            if (!_sh.transmissionSessionId.isEmpty())
                req.setRawHeader("X-Transmission-Session-Id", _sh.transmissionSessionId.toUtf8());
            _sh.setBasicAuth(req);
            return _sh.nam->post(req, body);
        });

        QByteArray newSessionId = r.header("X-Transmission-Session-Id");
        if (r.statusCode == 409 && attempt == 0 && !newSessionId.isEmpty()) {
            _sh.transmissionSessionId = QString::fromUtf8(newSessionId);
            _sh.trace(QStringLiteral("Received Transmission session ID, retrying request..."));
            continue;
        }
        break;
    }

    _sh.throwIfNoConnection(r);

    if (r.statusCode == 401)
        throw ClientException(QCoreApplication::translate("TorrentClient", "Invalid username or password."), 401, QString::fromUtf8(r.body));

    if (r.statusCode != 200)
        throw ClientException(QStringLiteral("HTTP %1 on /transmission/rpc: %2").arg(QString::number(r.statusCode), QString::fromUtf8(r.body).trimmed()), r.statusCode, QString::fromUtf8(r.body));

    QJsonDocument doc = _sh.parseJson(r.body, QStringLiteral("Transmission"), true);

    QJsonObject res = doc.object();
    QString result = res.value(QStringLiteral("result")).toString();
    if (result != QLatin1String("success"))
        throw ClientException(QCoreApplication::translate("TorrentClient", "Transmission error: %1").arg(result), r.statusCode, QString::fromUtf8(r.body));

    return res.value(QStringLiteral("arguments")).toObject();
}

// ------------------------------------------------------------- server api ----

const QJsonObject &TransmissionBackend::session()
{
    if (!_sessionCache)
        _sessionCache = execTransmissionRpc(QStringLiteral("session-get"), QJsonObject());
    return *_sessionCache;
}

QJsonObject TransmissionBackend::preferences()
{
    return session();
}

QString TransmissionBackend::defaultSavePath()
{
    return session().value(QStringLiteral("download-dir")).toString();
}

QVector<QPair<QString, QString>> TransmissionBackend::categories()
{
    return {};
}

QStringList TransmissionBackend::tags()
{
    try {
        QJsonObject args;
        QJsonArray fields;
        fields.append(QStringLiteral("labels"));
        args[QStringLiteral("fields")] = fields;
        QJsonObject res = execTransmissionRpc(QStringLiteral("torrent-get"), args);
        QJsonArray torrents = res.value(QStringLiteral("torrents")).toArray();
        QSet<QString> tagSet;
        for (const auto &v : torrents) {
            QJsonArray labels = v.toObject().value(QStringLiteral("labels")).toArray();
            for (const auto &l : labels)
                tagSet.insert(l.toString());
        }
        QStringList list = tagSet.values();
        list.sort(Qt::CaseInsensitive);
        return list;
    } catch (const std::exception &) {
        return {};
    }
}

QVector<TorrentInfo> TransmissionBackend::info(const QString &hashes)
{
    QJsonObject args;
    QJsonArray fields;
    fields.append(QStringLiteral("id"));
    fields.append(QStringLiteral("hashString"));
    fields.append(QStringLiteral("name"));
    fields.append(QStringLiteral("status"));
    fields.append(QStringLiteral("downloadDir"));
    fields.append(QStringLiteral("labels"));
    fields.append(QStringLiteral("totalSize"));
    fields.append(QStringLiteral("percentDone"));
    fields.append(QStringLiteral("sequentialDownload"));
    args[QStringLiteral("fields")] = fields;

    if (!hashes.isEmpty()) {
        QJsonArray idsArr;
        const QStringList list = hashes.split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString &h : list)
            idsArr.append(h.trimmed());
        args[QStringLiteral("ids")] = idsArr;
    }

    QJsonObject res = execTransmissionRpc(QStringLiteral("torrent-get"), args);
    QJsonArray torrents = res.value(QStringLiteral("torrents")).toArray();
    QVector<TorrentInfo> list;
    for (const QJsonValue &v : torrents) {
        const QJsonObject d = v.toObject();
        TorrentInfo t;
        t.hash = d.value(QStringLiteral("hashString")).toString().toLower();
        t.name = d.value(QStringLiteral("name")).toString();
        t.savePath = d.value(QStringLiteral("downloadDir")).toString();
        int status = d.value(QStringLiteral("status")).toInt();
        if (status == TransmissionStatus::Paused) t.state = QStringLiteral("pausedDL");
        else if (status == TransmissionStatus::CheckWait || status == TransmissionStatus::DownloadWait) t.state = QStringLiteral("queuedDL");
        else if (status == TransmissionStatus::Checking) t.state = QStringLiteral("checkingDL");
        else if (status == TransmissionStatus::Downloading) t.state = QStringLiteral("downloading");
        else if (status == TransmissionStatus::SeedWait) t.state = QStringLiteral("queuedUP");
        else if (status == TransmissionStatus::Seeding) t.state = QStringLiteral("uploading");
        else t.state = QStringLiteral("unknown");

        QJsonArray labels = d.value(QStringLiteral("labels")).toArray();
        QStringList tagList;
        for (const auto &l : labels)
            tagList.append(l.toString());
        t.tags = tagList.join(QStringLiteral(", "));
        t.totalSize = qint64(d.value(QStringLiteral("totalSize")).toDouble(0));
        t.progress = d.value(QStringLiteral("percentDone")).toDouble();
        t.sequentialDownload = d.value(QStringLiteral("sequentialDownload")).toBool(false);
        list.append(t);
    }
    return list;
}

// ------------------------------------------------------------------ add ----

void TransmissionBackend::addMagnet(const QString &magnet, bool addStopped, bool stopAfterMetadata,
                                    const QString &savepath, const QString &category,
                                    const QString &tags, const QString &contentLayout)
{
    Q_UNUSED(contentLayout);
    QJsonObject args;
    args[QStringLiteral("filename")] = magnet;
    if (addStopped || stopAfterMetadata)
        args[QStringLiteral("paused")] = true;
    if (!savepath.isEmpty())
        args[QStringLiteral("download-dir")] = savepath;
    const QJsonArray labels = transmissionLabels(category, tags);
    if (!labels.isEmpty())
        args[QStringLiteral("labels")] = labels;
    execTransmissionRpc(QStringLiteral("torrent-add"), args);
}

void TransmissionBackend::addTorrentFile(const QByteArray &torrentData, const QString &fileName,
                                         bool addStopped, bool stopAfterMetadata,
                                         const QString &savepath, const QString &category,
                                         const QString &tags, const QString &contentLayout)
{
    Q_UNUSED(fileName);
    Q_UNUSED(contentLayout);
    QJsonObject args;
    args[QStringLiteral("metainfo")] = QString::fromLatin1(torrentData.toBase64());
    if (addStopped || stopAfterMetadata)
        args[QStringLiteral("paused")] = true;
    if (!savepath.isEmpty())
        args[QStringLiteral("download-dir")] = savepath;
    const QJsonArray labels = transmissionLabels(category, tags);
    if (!labels.isEmpty())
        args[QStringLiteral("labels")] = labels;
    execTransmissionRpc(QStringLiteral("torrent-add"), args);
}

QVector<TorrentFile> TransmissionBackend::files(const QString &hash)
{
    QJsonObject args;
    QJsonArray idsArr;
    idsArr.append(hash);
    args[QStringLiteral("ids")] = idsArr;
    QJsonArray fields;
    fields.append(QStringLiteral("files"));
    fields.append(QStringLiteral("fileStats"));
    args[QStringLiteral("fields")] = fields;

    QJsonObject res = execTransmissionRpc(QStringLiteral("torrent-get"), args);
    QJsonArray torrents = res.value(QStringLiteral("torrents")).toArray();
    if (torrents.isEmpty())
        return {};

    QJsonObject tObj = torrents.first().toObject();
    QJsonArray filesArr = tObj.value(QStringLiteral("files")).toArray();
    QJsonArray fileStatsArr = tObj.value(QStringLiteral("fileStats")).toArray();

    QVector<TorrentFile> list;
    for (int i = 0; i < filesArr.size(); ++i) {
        QJsonObject f = filesArr[i].toObject();
        QJsonObject s = fileStatsArr.size() > i ? fileStatsArr[i].toObject() : QJsonObject();

        TorrentFile tf;
        tf.index = i;
        tf.name = f.value(QStringLiteral("name")).toString();
        tf.size = qint64(f.value(QStringLiteral("length")).toDouble(0));
        bool wanted = s.value(QStringLiteral("wanted")).toBool(true);
        int trPrio = s.value(QStringLiteral("priority")).toInt(0);
        if (!wanted)
            tf.priority = 0;
        else if (trPrio > 0)
            tf.priority = 6;
        else
            tf.priority = 1;

        qint64 done = qint64(f.value(QStringLiteral("bytesCompleted")).toDouble(0));
        tf.progress = tf.size > 0 ? double(done) / double(tf.size) : 0.0;
        list.append(tf);
    }
    return list;
}

void TransmissionBackend::filePrio(const QString &hash, const QList<int> &ids, int priority)
{
    if (ids.isEmpty())
        return;

    QJsonObject args;
    QJsonArray idsArr;
    idsArr.append(hash);
    args[QStringLiteral("ids")] = idsArr;

    QJsonArray idList;
    for (int id : ids)
        idList.append(id);

    if (priority == 0) {
        args[QStringLiteral("files-unwanted")] = idList;
    } else if (priority >= 6) {
        args[QStringLiteral("files-wanted")] = idList;
        args[QStringLiteral("priority-high")] = idList;
    } else {
        args[QStringLiteral("files-wanted")] = idList;
        args[QStringLiteral("priority-normal")] = idList;
    }
    execTransmissionRpc(QStringLiteral("torrent-set"), args);
}

// ------------------------------------------------------- torrent control ----

void TransmissionBackend::setForceStart(const QString &hash, bool value)
{
    // Transmission has no force-start flag; unsetting it must be a no-op,
    // otherwise a paused torrent would be started as a side effect.
    if (!value)
        return;
    QJsonObject args;
    args[QStringLiteral("ids")] = QJsonArray{hash};
    execTransmissionRpc(QStringLiteral("torrent-start-now"), args);
}

void TransmissionBackend::startTorrent(const QString &hash)
{
    QJsonObject args;
    args[QStringLiteral("ids")] = QJsonArray{hash};
    execTransmissionRpc(QStringLiteral("torrent-start"), args);
}

void TransmissionBackend::stopTorrent(const QString &hash)
{
    QJsonObject args;
    args[QStringLiteral("ids")] = QJsonArray{hash};
    execTransmissionRpc(QStringLiteral("torrent-stop"), args);
}

void TransmissionBackend::deleteTorrent(const QString &hash, bool deleteFiles)
{
    QJsonObject args;
    args[QStringLiteral("ids")] = QJsonArray{hash};
    args[QStringLiteral("delete-local-data")] = deleteFiles;
    execTransmissionRpc(QStringLiteral("torrent-remove"), args);
}

void TransmissionBackend::setLocation(const QString &hash, const QString &location)
{
    QJsonObject args;
    args[QStringLiteral("ids")] = QJsonArray{hash};
    args[QStringLiteral("location")] = location;
    args[QStringLiteral("move")] = true;
    execTransmissionRpc(QStringLiteral("torrent-set-location"), args);
}

void TransmissionBackend::setCategory(const QString &hash, const QString &category)
{
    if (!category.isEmpty())
        addTags(hash, category);
}

void TransmissionBackend::addTags(const QString &hash, const QString &tags)
{
    const QStringList tagList = splitTagList(tags);
    if (tagList.isEmpty())
        return;

    QSet<QString> allLabels;
    try {
        QJsonObject getArgs;
        getArgs[QStringLiteral("ids")] = QJsonArray{hash};
        getArgs[QStringLiteral("fields")] = QJsonArray{QStringLiteral("labels")};
        QJsonObject res = execTransmissionRpc(QStringLiteral("torrent-get"), getArgs);
        QJsonArray torrents = res.value(QStringLiteral("torrents")).toArray();
        if (!torrents.isEmpty()) {
            QJsonArray cur = torrents[0].toObject().value(QStringLiteral("labels")).toArray();
            for (const QJsonValue &v : cur)
                allLabels.insert(v.toString());
        }
    } catch (const std::exception &ex) {
        _sh.trace(QStringLiteral("Transmission get labels error: %1").arg(QString::fromUtf8(ex.what())));
    }

    for (const QString &t : tagList)
        allLabels.insert(t);

    QJsonObject args;
    args[QStringLiteral("ids")] = QJsonArray{hash};
    QJsonArray labelsArr;
    for (const QString &l : allLabels)
        labelsArr.append(l);
    args[QStringLiteral("labels")] = labelsArr;
    execTransmissionRpc(QStringLiteral("torrent-set"), args);
}

void TransmissionBackend::setTags(const QString &hash, const QString &newTags, const QString &oldTags)
{
    Q_UNUSED(oldTags);
    QJsonObject args;
    args[QStringLiteral("ids")] = QJsonArray{hash};
    QJsonArray labelsArr;
    for (const QString &t : splitTagList(newTags))
        labelsArr.append(t);
    args[QStringLiteral("labels")] = labelsArr;
    execTransmissionRpc(QStringLiteral("torrent-set"), args);
}

// ------------------------------------------------------------- trackers ----

void TransmissionBackend::addTrackers(const QString &hash, const QStringList &trackers)
{
    if (trackers.isEmpty())
        return;

    // trackerAdd appends to the existing announces; "trackerList" would
    // REPLACE the whole list, so it must not be sent here.
    QJsonObject args;
    args[QStringLiteral("ids")] = QJsonArray{hash};
    QJsonArray trAdd;
    for (const QString &tr : trackers)
        trAdd.append(tr);
    args[QStringLiteral("trackerAdd")] = trAdd;
    execTransmissionRpc(QStringLiteral("torrent-set"), args);
}

void TransmissionBackend::removeTrackers(const QString &hash, const QStringList &trackers)
{
    if (trackers.isEmpty())
        return;

    QJsonObject getArgs;
    getArgs[QStringLiteral("ids")] = QJsonArray{ hash };
    getArgs[QStringLiteral("fields")] = QJsonArray{ QStringLiteral("trackerStats") };
    QJsonObject res = execTransmissionRpc(QStringLiteral("torrent-get"), getArgs);
    const QJsonArray torrents = res.value(QStringLiteral("torrents")).toArray();
    if (torrents.isEmpty())
        return;

    const QJsonArray stats = torrents[0].toObject().value(QStringLiteral("trackerStats")).toArray();
    QJsonArray trRemove;
    for (const QJsonValue &v : stats) {
        const QJsonObject tobj = v.toObject();
        const QString url = tobj.value(QStringLiteral("announce")).toString();
        if (trackers.contains(url))
            trRemove.append(tobj.value(QStringLiteral("id")).toInt());
    }

    if (!trRemove.isEmpty()) {
        QJsonObject setArgs;
        setArgs[QStringLiteral("ids")] = QJsonArray{ hash };
        setArgs[QStringLiteral("trackerRemove")] = trRemove;
        execTransmissionRpc(QStringLiteral("torrent-set"), setArgs);
    }
}

QStringList TransmissionBackend::torrentTrackers(const QString &hash)
{
    QJsonObject args;
    args[QStringLiteral("ids")] = QJsonArray{ hash };
    QJsonObject res = execTransmissionRpc(QStringLiteral("torrent-get"), args);
    const QJsonArray torrents = res.value(QStringLiteral("torrents")).toArray();
    QStringList out;
    if (!torrents.isEmpty()) {
        const QJsonArray stats = torrents[0].toObject()
                                     .value(QStringLiteral("trackerStats")).toArray();
        for (const QJsonValue &v : stats) {
            const QString url = v.toObject().value(QStringLiteral("announce")).toString();
            if (!url.isEmpty())
                out.append(url);
        }
    }
    return out;
}

void TransmissionBackend::replaceTracker(const QString &hash, const QString &origUrl, const QString &newUrl)
{
    // Transmission has no per-URL replace: swap it inside the full list.
    QStringList trs = torrentTrackers(hash);
    bool found = false;
    for (QString &t : trs) {
        if (t == origUrl) {
            t = newUrl;
            found = true;
        }
    }
    if (!found)
        return;
    QJsonObject args;
    args[QStringLiteral("ids")] = QJsonArray{ hash };
    args[QStringLiteral("trackerList")] = trs.join(QLatin1Char('\n'));
    execTransmissionRpc(QStringLiteral("torrent-set"), args);
}

// ------------------------------------------------------------- the rest ----

void TransmissionBackend::rename(const QString &hash, const QString &name)
{
    QString oldName;
    try {
        QJsonObject getArgs;
        getArgs[QStringLiteral("ids")] = QJsonArray{hash};
        getArgs[QStringLiteral("fields")] = QJsonArray{QStringLiteral("name")};
        QJsonObject res = execTransmissionRpc(QStringLiteral("torrent-get"), getArgs);
        QJsonArray torrents = res.value(QStringLiteral("torrents")).toArray();
        if (!torrents.isEmpty())
            oldName = torrents[0].toObject().value(QStringLiteral("name")).toString();
    } catch (const std::exception &ex) {
        _sh.trace(QStringLiteral("Transmission get name error: %1").arg(QString::fromUtf8(ex.what())));
    }
    if (oldName.isEmpty() || oldName == name)
        return;

    QJsonObject args;
    args[QStringLiteral("ids")] = QJsonArray{hash};
    args[QStringLiteral("path")] = oldName;
    args[QStringLiteral("name")] = name;
    execTransmissionRpc(QStringLiteral("torrent-rename-path"), args);
}

void TransmissionBackend::setDownloadLimit(const QString &hash, qint64 bytesPerSec)
{
    QJsonObject args;
    args[QStringLiteral("ids")] = QJsonArray{hash};
    args[QStringLiteral("downloadLimit")] = bytesPerSec > 0 ? (bytesPerSec + 1023) / 1024 : 0;
    args[QStringLiteral("downloadLimited")] = (bytesPerSec > 0);
    execTransmissionRpc(QStringLiteral("torrent-set"), args);
}

void TransmissionBackend::setUploadLimit(const QString &hash, qint64 bytesPerSec)
{
    QJsonObject args;
    args[QStringLiteral("ids")] = QJsonArray{hash};
    args[QStringLiteral("uploadLimit")] = bytesPerSec > 0 ? (bytesPerSec + 1023) / 1024 : 0;
    args[QStringLiteral("uploadLimited")] = (bytesPerSec > 0);
    execTransmissionRpc(QStringLiteral("torrent-set"), args);
}

void TransmissionBackend::toggleSequentialDownload(const QString &hash)
{
    // Transmission has no toggle RPC: read the current flag and set the
    // opposite. Servers older than 4.1 ignore both calls (feature absent).
    QJsonObject getArgs;
    getArgs[QStringLiteral("ids")] = QJsonArray{hash};
    getArgs[QStringLiteral("fields")] = QJsonArray{QStringLiteral("sequentialDownload")};
    const QJsonObject res = execTransmissionRpc(QStringLiteral("torrent-get"), getArgs);
    const QJsonArray torrents = res.value(QStringLiteral("torrents")).toArray();
    const bool current = !torrents.isEmpty()
        && torrents[0].toObject().value(QStringLiteral("sequentialDownload")).toBool(false);

    QJsonObject args;
    args[QStringLiteral("ids")] = QJsonArray{hash};
    args[QStringLiteral("sequentialDownload")] = !current;
    execTransmissionRpc(QStringLiteral("torrent-set"), args);
}
