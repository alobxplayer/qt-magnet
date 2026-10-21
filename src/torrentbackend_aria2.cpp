#include "torrentbackend.h"
#include "config.h"
#include "magnetlink.h"
#include "torrentfile.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace {

// Aria2 returns a metadata download gid first; the actual torrent is the last "followedBy" entry.
QString lastFollowedGid(const QJsonObject &status)
{
    const QJsonArray followed = status.value(QStringLiteral("followedBy")).toArray();
    if (followed.isEmpty())
        return QString();
    return followed.last().toString();
}

// How many waiting/stopped downloads a single tellWaiting/tellStopped call returns.
constexpr int kAria2ListLimit = 1000;

} // namespace

// ---------------------------------------------------------------- login ----

void Aria2Backend::login()
{
    QJsonValue res = execAria2Rpc(QStringLiteral("aria2.getVersion"));
    QJsonObject vObj = res.toObject();
    appVersion = QStringLiteral("Aria2 ") + vObj.value(QStringLiteral("version")).toString();
    apiVersion = QStringLiteral("JSON-RPC 2.0");
    _sh.trace(QStringLiteral("Logged in to %1 (%2)").arg(appVersion, apiVersion));
}

void Aria2Backend::fetchServerInfo()
{
    login();
}

// ------------------------------------------------------------------ rpc ----

QJsonValue Aria2Backend::execAria2Rpc(const QString &method, const QJsonArray &params)
{
    QJsonObject reqObj;
    reqObj[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    reqObj[QStringLiteral("id")] = QStringLiteral("qt-magnet-") + QString::number(++_tag);
    reqObj[QStringLiteral("method")] = method;

    QJsonArray fullParams;
    const QString pass = _sh.cfg.getPassword();
    if (!pass.isEmpty()) {
        fullParams.append(QStringLiteral("token:") + pass);
    }
    for (const QJsonValue &v : params)
        fullParams.append(v);

    reqObj[QStringLiteral("params")] = fullParams;

    const QByteArray body = QJsonDocument(reqObj).toJson(QJsonDocument::Compact);

    ClientTransport::RawResponse r = _sh.execRaw([this, &body] {
        QNetworkRequest req{ QUrl(_sh.base + QStringLiteral("/jsonrpc")) };
        _sh.prepare(req);
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        _sh.setBasicAuth(req);
        return _sh.nam->post(req, body);
    });

    _sh.throwIfNoConnection(r);

    if (r.statusCode == 401)
        throw ClientException(QCoreApplication::translate("TorrentClient", "Invalid username, password, or RPC secret."), 401, QString::fromUtf8(r.body));

    if (r.statusCode != 200)
        throw ClientException(QStringLiteral("HTTP %1 on /jsonrpc: %2").arg(QString::number(r.statusCode), QString::fromUtf8(r.body).trimmed()), r.statusCode, QString::fromUtf8(r.body));

    QJsonDocument doc = _sh.parseJson(r.body, QStringLiteral("Aria2"), true);

    QJsonObject res = doc.object();
    if (res.contains(QStringLiteral("error"))) {
        QJsonObject err = res.value(QStringLiteral("error")).toObject();
        QString errMsg = err.value(QStringLiteral("message")).toString();
        int code = err.value(QStringLiteral("code")).toInt();
        throw ClientException(QCoreApplication::translate("TorrentClient", "Aria2 error %1: %2").arg(QString::number(code), errMsg), r.statusCode, QString::fromUtf8(r.body));
    }

    return res.value(QStringLiteral("result"));
}

QString Aria2Backend::findGidByHash(const QString &hash)
{
    const QString hLower = hash.toLower();
    if (_hashToGid.contains(hLower)) {
        QString gid = _hashToGid.value(hLower);
        try {
            QJsonArray params;
            params.append(gid);
            QJsonObject st = execAria2Rpc(QStringLiteral("aria2.tellStatus"), params).toObject();
            QString childGid = lastFollowedGid(st);
            if (!childGid.isEmpty()) {
                _hashToGid[hLower] = childGid;
                return childGid;
            }
            return gid;
        } catch (const std::exception &) {}
    }

    auto checkList = [&](const QString &method, const QJsonArray &extraArgs = QJsonArray()) -> QString {
        QJsonValue res = execAria2Rpc(method, extraArgs);
        QJsonArray arr = res.toArray();
        for (const QJsonValue &v : arr) {
            QJsonObject o = v.toObject();
            QString iHash = o.value(QStringLiteral("infoHash")).toString().toLower();
            if (iHash.isEmpty()) {
                QJsonObject bt = o.value(QStringLiteral("bittorrent")).toObject();
                iHash = bt.value(QStringLiteral("infoHash")).toString().toLower();
            }
            QString curGid = o.value(QStringLiteral("gid")).toString();
            if (!iHash.isEmpty() && !curGid.isEmpty()) {
                _hashToGid[iHash] = curGid;
                if (iHash == hLower) {
                    QString childGid = lastFollowedGid(o);
                    if (!childGid.isEmpty()) {
                        _hashToGid[hLower] = childGid;
                        return childGid;
                    }
                    return curGid;
                }
            }
        }
        return QString();
    };

    QString found = checkList(QStringLiteral("aria2.tellActive"));
    if (!found.isEmpty()) return found;

    QJsonArray pageArgs;
    pageArgs.append(0);
    pageArgs.append(kAria2ListLimit);
    found = checkList(QStringLiteral("aria2.tellWaiting"), pageArgs);
    if (!found.isEmpty()) return found;

    found = checkList(QStringLiteral("aria2.tellStopped"), pageArgs);
    if (!found.isEmpty()) return found;

    return QString();
}

void Aria2Backend::aria2TorrentRpc(const QString &hash, const QString &method,
                                   const QJsonObject &opt, const QString &what)
{
    const QString gid = findGidByHash(hash);
    if (gid.isEmpty())
        throw ClientException(QCoreApplication::translate("TorrentClient",
            "Torrent %1 was not found on the Aria2 server.").arg(hash.left(12)));
    QJsonArray params;
    params.append(gid);
    if (!opt.isEmpty())
        params.append(opt);
    try {
        execAria2Rpc(method, params);
    } catch (const ClientException &ex) {
        // Surface the failing operation instead of a bare RPC error.
        throw ClientException(QStringLiteral("Aria2 %1: %2").arg(what, ex.message),
                              ex.status, ex.body);
    }
}

void Aria2Backend::registerGidFromStatus(const QString &gid, const QString &what)
{
    try {
        QJsonArray stParams;
        stParams.append(gid);
        QJsonObject st = execAria2Rpc(QStringLiteral("aria2.tellStatus"), stParams).toObject();
        QString h = st.value(QStringLiteral("infoHash")).toString().toLower();
        if (!h.isEmpty())
            _hashToGid[h] = gid;
    } catch (const std::exception &ex) {
        _sh.trace(QStringLiteral("Aria2 tellStatus on %1: %2").arg(what, QString::fromUtf8(ex.what())));
    }
}

// ------------------------------------------------------------- server api ----

const QJsonObject &Aria2Backend::globalOptions()
{
    if (!_globalOptionsCache)
        _globalOptionsCache = execAria2Rpc(QStringLiteral("aria2.getGlobalOption")).toObject();
    return *_globalOptionsCache;
}

QJsonObject Aria2Backend::preferences()
{
    return globalOptions();
}

QString Aria2Backend::defaultSavePath()
{
    return globalOptions().value(QStringLiteral("dir")).toString();
}

QVector<QPair<QString, QString>> Aria2Backend::categories()
{
    return {};
}

QStringList Aria2Backend::tags()
{
    return {};
}

QVector<TorrentInfo> Aria2Backend::info(const QString &hashes)
{
    QVector<TorrentInfo> list;
    const QStringList wanted = hashes.split(QLatin1Char('|'), Qt::SkipEmptyParts);
    auto collect = [&](const QString &method, const QJsonArray &extra = QJsonArray()) {
        QJsonValue res = execAria2Rpc(method, extra);
        for (const QJsonValue &v : res.toArray()) {
            QJsonObject o = v.toObject();
            QString iHash = o.value(QStringLiteral("infoHash")).toString().toLower();
            QJsonObject bt = o.value(QStringLiteral("bittorrent")).toObject();
            if (iHash.isEmpty())
                iHash = bt.value(QStringLiteral("infoHash")).toString().toLower();
            QString gid = o.value(QStringLiteral("gid")).toString();
            if (!iHash.isEmpty())
                _hashToGid[iHash] = gid;

            TorrentInfo t;
            t.hash = iHash;
            QJsonObject btInfo = bt.value(QStringLiteral("info")).toObject();
            t.name = btInfo.value(QStringLiteral("name")).toString();
            if (t.name.isEmpty()) {
                QJsonArray fArr = o.value(QStringLiteral("files")).toArray();
                if (!fArr.isEmpty())
                    t.name = fArr.first().toObject().value(QStringLiteral("path")).toString();
            }
            if (t.name.isEmpty())
                t.name = gid;

            t.savePath = o.value(QStringLiteral("dir")).toString();
            QString st = o.value(QStringLiteral("status")).toString();
            if (st == QLatin1String("paused")) t.state = QStringLiteral("pausedDL");
            else if (st == QLatin1String("waiting")) t.state = QStringLiteral("queuedDL");
            else if (st == QLatin1String("active")) {
                qint64 total = o.value(QStringLiteral("totalLength")).toString().toLongLong();
                qint64 comp = o.value(QStringLiteral("completedLength")).toString().toLongLong();
                if (total > 0 && comp >= total)
                    t.state = QStringLiteral("uploading");
                else if (bt.isEmpty())
                    t.state = QStringLiteral("metaDL");
                else
                    t.state = QStringLiteral("downloading");
            } else if (st == QLatin1String("complete")) {
                t.state = QStringLiteral("pausedUP");
            } else if (st == QLatin1String("error")) {
                t.state = QStringLiteral("error");
            } else {
                t.state = QStringLiteral("stoppedDL");
            }

            t.totalSize = o.value(QStringLiteral("totalLength")).toString().toLongLong();
            qint64 compLen = o.value(QStringLiteral("completedLength")).toString().toLongLong();
            t.progress = t.totalSize > 0 ? double(compLen) / double(t.totalSize) : 0.0;

            if (wanted.isEmpty() || wanted.contains(iHash, Qt::CaseInsensitive))
                list.append(t);
        }
    };

    collect(QStringLiteral("aria2.tellActive"));
    QJsonArray p; p.append(0); p.append(kAria2ListLimit);
    collect(QStringLiteral("aria2.tellWaiting"), p);
    collect(QStringLiteral("aria2.tellStopped"), p);
    return list;
}

// ------------------------------------------------------------------ add ----

void Aria2Backend::addMagnet(const QString &magnet, bool addStopped, bool stopAfterMetadata,
                             const QString &savepath, const QString &category,
                             const QString &tags, const QString &contentLayout)
{
    Q_UNUSED(category);
    Q_UNUSED(tags);
    Q_UNUSED(contentLayout);
    if (MagnetLink::looksLikeMagnet(magnet)) {
        MagnetLink m = MagnetLink::parse(magnet);
        if (m.version == TorrentVersion::V2) {
            throw ClientException(QCoreApplication::translate("TorrentClient",
                "Aria2 does not support pure BitTorrent v2 (BEP 52). Please configure qBittorrent or Transmission in Settings."));
        }
    }

    QJsonArray params;
    QJsonArray uris;
    uris.append(magnet);
    params.append(uris);

    QJsonObject opts;
    if (addStopped || stopAfterMetadata)
        opts[QStringLiteral("pause")] = QStringLiteral("true");
    if (!savepath.isEmpty())
        opts[QStringLiteral("dir")] = savepath;

    params.append(opts);
    QJsonValue res = execAria2Rpc(QStringLiteral("aria2.addUri"), params);
    QString gid = res.toString();
    if (!gid.isEmpty()) {
        if (MagnetLink::looksLikeMagnet(magnet)) {
            MagnetLink m = MagnetLink::parse(magnet);
            if (!m.hash.isEmpty())
                _hashToGid[m.hash.toLower()] = gid;
            if (!m.hashV2.isEmpty())
                _hashToGid[m.hashV2.toLower()] = gid;
        }
        registerGidFromStatus(gid, QStringLiteral("addMagnet"));
    }
}

void Aria2Backend::addTorrentFile(const QByteArray &torrentData, const QString &fileName,
                                  bool addStopped, bool stopAfterMetadata,
                                  const QString &savepath, const QString &category,
                                  const QString &tags, const QString &contentLayout)
{
    TorrentFileData tf = TorrentFileData::parse(torrentData, fileName);
    if (tf.version == TorrentVersion::V2) {
        throw ClientException(QCoreApplication::translate("TorrentClient",
            "Aria2 does not support pure BitTorrent v2 (BEP 52). Please configure qBittorrent or Transmission in Settings."));
    }
    if (tf.version == TorrentVersion::Hybrid) {
        addMagnet(tf.toMagnetUri(), addStopped, stopAfterMetadata, savepath, category, tags, contentLayout);
        return;
    }

    QJsonArray params;
    params.append(QString::fromLatin1(torrentData.toBase64()));
    params.append(QJsonArray()); // uris

    QJsonObject opts;
    if (addStopped || stopAfterMetadata)
        opts[QStringLiteral("pause")] = QStringLiteral("true");
    if (!savepath.isEmpty())
        opts[QStringLiteral("dir")] = savepath;

    params.append(opts);
    QJsonValue res = execAria2Rpc(QStringLiteral("aria2.addTorrent"), params);
    QString gid = res.toString();
    if (!gid.isEmpty()) {
        if (!tf.hash.isEmpty())
            _hashToGid[tf.hash.toLower()] = gid;
        registerGidFromStatus(gid, QStringLiteral("addTorrent"));
    }
}

QVector<TorrentFile> Aria2Backend::files(const QString &hash)
{
    QString gid = findGidByHash(hash);
    if (gid.isEmpty())
        return {};

    QJsonArray params;
    params.append(gid);
    QJsonObject st = execAria2Rpc(QStringLiteral("aria2.tellStatus"), params).toObject();

    QString childGid = lastFollowedGid(st);
    if (!childGid.isEmpty()) {
        gid = childGid;
        _hashToGid[hash.toLower()] = gid;
        QJsonArray childParams;
        childParams.append(gid);
        st = execAria2Rpc(QStringLiteral("aria2.tellStatus"), childParams).toObject();
    }

    QJsonArray filesArr = st.value(QStringLiteral("files")).toArray();
    if (filesArr.isEmpty())
        return {};

    QString dir = st.value(QStringLiteral("dir")).toString();
    QVector<TorrentFile> list;
    for (int i = 0; i < filesArr.size(); ++i) {
        QJsonObject f = filesArr[i].toObject();
        TorrentFile tf;
        tf.index = f.value(QStringLiteral("index")).toString().toInt() - 1;
        if (tf.index < 0) tf.index = i;
        tf.name = f.value(QStringLiteral("path")).toString();
        if (!dir.isEmpty() && tf.name.startsWith(dir)) {
            QString rel = tf.name.mid(dir.length());
            if (rel.startsWith(QLatin1Char('/')) || rel.startsWith(QLatin1Char('\\')))
                rel = rel.mid(1);
            if (!rel.isEmpty())
                tf.name = rel;
        }
        if (tf.name.isEmpty())
            tf.name = QStringLiteral("File %1").arg(tf.index + 1);

        tf.size = f.value(QStringLiteral("length")).toString().toLongLong();
        qint64 done = f.value(QStringLiteral("completedLength")).toString().toLongLong();
        tf.progress = tf.size > 0 ? double(done) / double(tf.size) : 0.0;
        bool selected = f.value(QStringLiteral("selected")).toString() != QLatin1String("false");
        tf.priority = selected ? 1 : 0;
        list.append(tf);
    }
    if (list.size() == 1 && list.first().name.contains(QLatin1String("[METADATA]"), Qt::CaseInsensitive))
        return {};
    return list;
}

void Aria2Backend::filePrio(const QString &hash, const QList<int> &ids, int priority)
{
    if (ids.isEmpty())
        return;

    const QString gid = findGidByHash(hash);
    if (gid.isEmpty())
        throw ClientException(QCoreApplication::translate("TorrentClient",
            "Torrent %1 was not found on the Aria2 server.").arg(hash.left(12)));
    QVector<TorrentFile> curFiles = files(hash);
    if (curFiles.isEmpty())
        return;
    QSet<int> wantedIndices;
    for (const auto &f : curFiles) {
        if (f.priority > 0)
            wantedIndices.insert(f.index);
    }
    for (int id : ids) {
        if (priority == 0)
            wantedIndices.remove(id);
        else
            wantedIndices.insert(id);
    }
    QStringList sel;
    for (int idx : wantedIndices)
        sel.append(QString::number(idx + 1));
    QJsonObject opt;
    opt[QStringLiteral("select-file")] = sel.join(QLatin1Char(','));
    aria2TorrentRpc(hash, QStringLiteral("aria2.changeOption"), opt, QStringLiteral("filePrio"));
}

// ------------------------------------------------------- torrent control ----

void Aria2Backend::setForceStart(const QString &hash, bool value)
{
    if (value)
        startTorrent(hash);
}

void Aria2Backend::startTorrent(const QString &hash)
{
    aria2TorrentRpc(hash, QStringLiteral("aria2.unpause"), QJsonObject(), QStringLiteral("unpause"));
}

void Aria2Backend::stopTorrent(const QString &hash)
{
    aria2TorrentRpc(hash, QStringLiteral("aria2.pause"), QJsonObject(), QStringLiteral("pause"));
}

void Aria2Backend::deleteTorrent(const QString &hash, bool deleteFiles)
{
    // aria2 has no RPC to delete downloaded files; removing the torrent
    // works, the flag does not.
    if (deleteFiles)
        _sh.trace(QStringLiteral("Aria2 cannot delete downloaded files; removing the torrent only."));
    QString gid = findGidByHash(hash);
    if (!gid.isEmpty()) {
        QJsonArray params;
        params.append(gid);
        try {
            execAria2Rpc(QStringLiteral("aria2.forceRemove"), params);
        } catch (const std::exception &) {
            try {
                execAria2Rpc(QStringLiteral("aria2.remove"), params);
            } catch (const std::exception &ex) {
                _sh.trace(QStringLiteral("Aria2 remove error: %1").arg(QString::fromUtf8(ex.what())));
            }
        }
    }
}

void Aria2Backend::setLocation(const QString &hash, const QString &location)
{
    QJsonObject opt;
    opt[QStringLiteral("dir")] = location;
    aria2TorrentRpc(hash, QStringLiteral("aria2.changeOption"), opt, QStringLiteral("setLocation"));
}

void Aria2Backend::setDownloadLimit(const QString &hash, qint64 bytesPerSec)
{
    QJsonObject opt;
    opt[QStringLiteral("max-download-limit")] = QString::number(bytesPerSec);
    aria2TorrentRpc(hash, QStringLiteral("aria2.changeOption"), opt, QStringLiteral("setDownloadLimit"));
}

void Aria2Backend::setUploadLimit(const QString &hash, qint64 bytesPerSec)
{
    QJsonObject opt;
    opt[QStringLiteral("max-upload-limit")] = QString::number(bytesPerSec);
    aria2TorrentRpc(hash, QStringLiteral("aria2.changeOption"), opt, QStringLiteral("setUploadLimit"));
}
