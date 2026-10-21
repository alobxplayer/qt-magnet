#include "torrentbackend.h"
#include "config.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QHttpMultiPart>
#include <QHttpPart>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace {

QString formHashes(const QString &hash)
{
    return QStringLiteral("hashes=") + ClientTransport::esc(hash);
}

} // namespace

TorrentInfo TorrentInfo::from(const QJsonObject &d)
{
    TorrentInfo t;
    t.hash               = d.value(QStringLiteral("hash")).toString();
    t.infoHashV1         = d.value(QStringLiteral("infohash_v1")).toString();
    t.infoHashV2         = d.value(QStringLiteral("infohash_v2")).toString();
    t.name               = d.value(QStringLiteral("name")).toString();
    t.state              = d.value(QStringLiteral("state")).toString();
    t.savePath           = d.value(QStringLiteral("save_path")).toString();
    t.category           = d.value(QStringLiteral("category")).toString();
    t.tags               = d.value(QStringLiteral("tags")).toString();
    t.forceStart         = d.value(QStringLiteral("force_start")).toBool(false);
    t.sequentialDownload = d.value(QStringLiteral("seq_dl")).toBool(false);
    t.firstLastPiecePrio = d.value(QStringLiteral("f_l_piece_prio")).toBool(false);
    t.autoTmm            = d.value(QStringLiteral("auto_tmm")).toBool(false);
    t.size               = qint64(d.value(QStringLiteral("size")).toDouble(0));
    t.totalSize          = qint64(d.value(QStringLiteral("total_size")).toDouble(0));
    t.progress           = d.value(QStringLiteral("progress")).toDouble(0);
    return t;
}

// ---------------------------------------------------------------- login ----

void QBittorrentBackend::login()
{
    if (_sh.cfg.authMode == QLatin1String("apikey") && !_sh.cfg.getApiKey().isEmpty()) {
        if (verifyAuthenticated()) {
            _sh.trace(QStringLiteral("Authenticated to qBittorrent via API key successfully."));
            return;
        }
        throw ClientException(QCoreApplication::translate("TorrentClient", "API key authentication failed."), 401, QString());
    }

    if (_sh.hasSessionCookie()) {
        return;
    }

    const QString pass = _sh.cfg.getPassword();
    const QString form = QStringLiteral("username=") + ClientTransport::esc(_sh.cfg.username)
                       + QStringLiteral("&password=") + ClientTransport::esc(pass);

    ClientTransport::RawResponse r = _sh.execRaw([this, &form] {
        QNetworkRequest req{ QUrl(_sh.base + QStringLiteral("/api/v2/auth/login")) };
        _sh.prepare(req);
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded; charset=UTF-8"));
        return _sh.nam->post(req, form.toUtf8());
    });

    _sh.throwIfNoConnection(r);

    const QString res = QString::fromUtf8(r.body).trimmed();
    if (res == QLatin1String("Fails."))
        throw ClientException(QCoreApplication::translate("TorrentClient", "Invalid username or password."), 403, res);

    if (!_sh.hasSessionCookie() && !verifyAuthenticated())
        throw ClientException(QCoreApplication::translate("TorrentClient", "Authentication failed (no session cookie)."), r.statusCode, res);

    _sh.trace(QStringLiteral("Logged in to qBittorrent successfully."));
}

void QBittorrentBackend::fetchServerInfo()
{
    try {
        appVersion = get(QStringLiteral("/api/v2/app/version")).trimmed();
    } catch (const std::exception &ex) {
        _sh.trace(QStringLiteral("Could not fetch app version: %1").arg(QString::fromUtf8(ex.what())));
    }
    try {
        apiVersion = get(QStringLiteral("/api/v2/app/webapiVersion")).trimmed();
    } catch (const std::exception &ex) {
        _sh.trace(QStringLiteral("Could not fetch web API version: %1").arg(QString::fromUtf8(ex.what())));
    }
    _sh.trace(QStringLiteral("qBittorrent %1, Web API %2").arg(appVersion, apiVersion));
}

// --------------------------------------------------------------- session ----

bool QBittorrentBackend::verifyAuthenticated()
{
    try {
        const QString v = get(QStringLiteral("/api/v2/app/version"), false);
        return !v.trimmed().isEmpty();
    } catch (const std::exception &) {
        return false;
    }
}

QString QBittorrentBackend::exec(const std::function<QNetworkReply *()> &make, const QString &path, bool allowRelogin)
{
    ClientTransport::RawResponse r = _sh.execRaw(make);

    if (r.networkError == QNetworkReply::NoError)
        return QString::fromUtf8(r.body);

    _sh.throwIfNoConnection(r);

    const QString text = QString::fromUtf8(r.body);
    if (r.statusCode == 403 && allowRelogin) {
        _sh.trace(QStringLiteral("403 on %1: session expired, logging in again").arg(path));
        _sh.resetSession();
        login();
        return exec(make, path, false);
    }

    throw ClientException(QStringLiteral("HTTP %1 on %2%3")
                           .arg(QString::number(r.statusCode), path,
                                text.length() > 0 ? QStringLiteral(": ") + text.trimmed() : QString()),
                       r.statusCode, text);
}

QString QBittorrentBackend::get(const QString &path, bool allowRelogin)
{
    return exec([this, path] {
        QNetworkRequest req{ QUrl(_sh.base + path) };
        _sh.prepare(req);
        return _sh.nam->get(req);
    }, path, allowRelogin);
}

QString QBittorrentBackend::postForm(const QString &path, const QString &formBody, bool allowRelogin)
{
    const QByteArray data = formBody.toUtf8();
    return exec([this, path, data] {
        QNetworkRequest req{ QUrl(_sh.base + path) };
        _sh.prepare(req);
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded; charset=UTF-8"));
        return _sh.nam->post(req, data);
    }, path, allowRelogin);
}

QString QBittorrentBackend::postMultipart(const QString &path, const QVector<QPair<QString, QString>> &fields, bool allowRelogin)
{
    return exec([this, path, fields] {
        auto *multi = new QHttpMultiPart(QHttpMultiPart::FormDataType);
        for (const auto &pair : fields) {
            QHttpPart part;
            part.setHeader(QNetworkRequest::ContentDispositionHeader,
                           QStringLiteral("form-data; name=\"%1\"").arg(pair.first));
            part.setBody(pair.second.toUtf8());
            multi->append(part);
        }
        QNetworkRequest req{ QUrl(_sh.base + path) };
        _sh.prepare(req);
        QNetworkReply *reply = _sh.nam->post(req, multi);
        multi->setParent(reply);
        return reply;
    }, path, allowRelogin);
}

QString QBittorrentBackend::postMultipartWithFile(const QString &path, const QVector<QPair<QString, QString>> &fields,
                                                  const QString &fileFieldName, const QString &fileName,
                                                  const QByteArray &fileData, const QString &fileContentType,
                                                  bool allowRelogin)
{
    return exec([this, path, fields, fileFieldName, fileName, fileData, fileContentType] {
        auto *multi = new QHttpMultiPart(QHttpMultiPart::FormDataType);
        for (const auto &pair : fields) {
            QHttpPart part;
            part.setHeader(QNetworkRequest::ContentDispositionHeader,
                           QStringLiteral("form-data; name=\"%1\"").arg(pair.first));
            part.setBody(pair.second.toUtf8());
            multi->append(part);
        }
        QHttpPart filePart;
        filePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                           QStringLiteral("form-data; name=\"%1\"; filename=\"%2\"").arg(fileFieldName, fileName));
        filePart.setHeader(QNetworkRequest::ContentTypeHeader, fileContentType);
        filePart.setBody(fileData);
        multi->append(filePart);

        QNetworkRequest req{ QUrl(_sh.base + path) };
        _sh.prepare(req);
        QNetworkReply *reply = _sh.nam->post(req, multi);
        multi->setParent(reply);
        return reply;
    }, path, allowRelogin);
}

// ------------------------------------------------------------- server api ----

QJsonObject QBittorrentBackend::preferences()
{
    const QString json = get(QStringLiteral("/api/v2/app/preferences"));
    return _sh.parseJson(json.toUtf8(), QStringLiteral("server on app/preferences")).object();
}

QString QBittorrentBackend::defaultSavePath()
{
    try {
        return get(QStringLiteral("/api/v2/app/defaultSavePath")).trimmed();
    } catch (const std::exception &) {
        return QString();
    }
}

QVector<QPair<QString, QString>> QBittorrentBackend::categories()
{
    const QString json = get(QStringLiteral("/api/v2/torrents/categories"));
    const QJsonObject obj = _sh.parseJson(json.toUtf8(), QStringLiteral("server on torrents/categories")).object();
    QVector<QPair<QString, QString>> list;
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        const QJsonObject c = it.value().toObject();
        list.append({ it.key(), c.value(QStringLiteral("savePath")).toString() });
    }
    return list;
}

QStringList QBittorrentBackend::tags()
{
    const QJsonArray arr = _sh.parseJson(get(QStringLiteral("/api/v2/torrents/tags")).toUtf8(),
                                         QStringLiteral("server on torrents/tags")).array();
    QStringList list;
    for (const QJsonValue &v : arr)
        list.append(v.toString());
    return list;
}

QVector<TorrentInfo> QBittorrentBackend::info(const QString &hashes)
{
    QString path = QStringLiteral("/api/v2/torrents/info");
    if (!hashes.isEmpty())
        path += QStringLiteral("?hashes=") + ClientTransport::esc(hashes);

    const QString json = get(path);
    const QJsonArray arr = _sh.parseJson(json.toUtf8(), QStringLiteral("server on torrents/info")).array();
    QVector<TorrentInfo> list;
    for (const QJsonValue &v : arr)
        list.append(TorrentInfo::from(v.toObject()));
    return list;
}

// ------------------------------------------------------------------ add ----

void QBittorrentBackend::addMagnet(const QString &magnet, bool addStopped, bool stopAfterMetadata,
                                    const QString &savepath, const QString &category,
                                    const QString &tags, const QString &contentLayout)
{
    QVector<QPair<QString, QString>> fields;
    fields.append({ QStringLiteral("urls"), magnet });
    if (!savepath.isEmpty())
        fields.append({ QStringLiteral("savepath"), savepath });
    if (!category.isEmpty())
        fields.append({ QStringLiteral("category"), category });
    if (!tags.isEmpty())
        fields.append({ QStringLiteral("tags"), tags });
    if (!contentLayout.isEmpty())
        fields.append({ QStringLiteral("contentLayout"), contentLayout });

    if (stopAfterMetadata)
        fields.append({ QStringLiteral("stopCondition"), QStringLiteral("MetadataReceived") });

    if (addStopped) {
        fields.append({ QStringLiteral("stopped"), QStringLiteral("true") });
        fields.append({ QStringLiteral("paused"), QStringLiteral("true") });
    }

    postMultipart(QStringLiteral("/api/v2/torrents/add"), fields);
}

void QBittorrentBackend::addTorrentFile(const QByteArray &torrentData, const QString &fileName,
                                        bool addStopped, bool stopAfterMetadata,
                                        const QString &savepath, const QString &category,
                                        const QString &tags, const QString &contentLayout)
{
    QVector<QPair<QString, QString>> fields;
    if (!savepath.isEmpty())
        fields.append({ QStringLiteral("savepath"), savepath });
    if (!category.isEmpty())
        fields.append({ QStringLiteral("category"), category });
    if (!tags.isEmpty())
        fields.append({ QStringLiteral("tags"), tags });
    if (!contentLayout.isEmpty())
        fields.append({ QStringLiteral("contentLayout"), contentLayout });

    if (stopAfterMetadata)
        fields.append({ QStringLiteral("stopCondition"), QStringLiteral("MetadataReceived") });

    if (addStopped) {
        fields.append({ QStringLiteral("stopped"), QStringLiteral("true") });
        fields.append({ QStringLiteral("paused"), QStringLiteral("true") });
    }

    QString uploadFileName = fileName.isEmpty() ? QStringLiteral("torrent.torrent") : fileName;
    postMultipartWithFile(QStringLiteral("/api/v2/torrents/add"), fields,
                          QStringLiteral("torrents"), uploadFileName,
                          torrentData, QStringLiteral("application/x-bittorrent"));
}

QVector<TorrentFile> QBittorrentBackend::files(const QString &hash)
{
    const QString json = get(QStringLiteral("/api/v2/torrents/files?hash=") + ClientTransport::esc(hash));
    const QJsonArray arr = _sh.parseJson(json.toUtf8(), QStringLiteral("server on torrents/files")).array();
    QVector<TorrentFile> list;
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        TorrentFile f;
        f.index    = o.value(QStringLiteral("index")).toInt(list.size());
        f.name     = o.value(QStringLiteral("name")).toString();
        f.size     = qint64(o.value(QStringLiteral("size")).toDouble(0));
        f.priority = o.value(QStringLiteral("priority")).toInt(1);
        f.progress = o.value(QStringLiteral("progress")).toDouble(0);
        list.append(f);
    }
    return list;
}

void QBittorrentBackend::filePrio(const QString &hash, const QList<int> &ids, int priority)
{
    if (ids.isEmpty())
        return;

    QStringList sids;
    for (int id : ids)
        sids.append(QString::number(id));
    const QString form = QStringLiteral("hash=") + ClientTransport::esc(hash)
                         + QStringLiteral("&id=") + ClientTransport::esc(sids.join(QLatin1Char('|')))
                         + QStringLiteral("&priority=") + QString::number(priority);
    postForm(QStringLiteral("/api/v2/torrents/filePrio"), form);
}

// ------------------------------------------------------- torrent control ----

void QBittorrentBackend::setForceStart(const QString &hash, bool value)
{
    const QString form = formHashes(hash)
                         + QStringLiteral("&value=") + (value ? QStringLiteral("true") : QStringLiteral("false"));
    postForm(QStringLiteral("/api/v2/torrents/setForceStart"), form);
}

void QBittorrentBackend::callStartStop(const QString &hash, bool start)
{
    const QString hashes = formHashes(hash);

    if (!_useStartStop.has_value()) {
        try {
            postForm(start ? QStringLiteral("/api/v2/torrents/start") : QStringLiteral("/api/v2/torrents/stop"), hashes);
            _useStartStop = true;
            return;
        } catch (const ClientException &ex) {
            if (ex.status == 404) {
                _useStartStop = false;
            } else {
                throw;
            }
        }
    }

    if (_useStartStop.value()) {
        postForm(start ? QStringLiteral("/api/v2/torrents/start") : QStringLiteral("/api/v2/torrents/stop"), hashes);
    } else {
        postForm(start ? QStringLiteral("/api/v2/torrents/resume") : QStringLiteral("/api/v2/torrents/pause"), hashes);
    }
}

void QBittorrentBackend::startTorrent(const QString &hash)
{
    callStartStop(hash, true);
}

void QBittorrentBackend::stopTorrent(const QString &hash)
{
    callStartStop(hash, false);
}

void QBittorrentBackend::deleteTorrent(const QString &hash, bool deleteFiles)
{
    const QString form = formHashes(hash)
                         + QStringLiteral("&deleteFiles=") + (deleteFiles ? QStringLiteral("true") : QStringLiteral("false"));
    postForm(QStringLiteral("/api/v2/torrents/delete"), form);
}

void QBittorrentBackend::setLocation(const QString &hash, const QString &location)
{
    const QString form = formHashes(hash) + QStringLiteral("&location=") + ClientTransport::esc(location);
    postForm(QStringLiteral("/api/v2/torrents/setLocation"), form);
}

void QBittorrentBackend::setCategory(const QString &hash, const QString &category)
{
    const QString form = formHashes(hash) + QStringLiteral("&category=") + ClientTransport::esc(category);
    postForm(QStringLiteral("/api/v2/torrents/setCategory"), form);
}

void QBittorrentBackend::addTags(const QString &hash, const QString &tags)
{
    const QString form = formHashes(hash) + QStringLiteral("&tags=") + ClientTransport::esc(tags);
    postForm(QStringLiteral("/api/v2/torrents/addTags"), form);
}

void QBittorrentBackend::removeTags(const QString &hash, const QString &tags)
{
    const QString form = formHashes(hash) + QStringLiteral("&tags=") + ClientTransport::esc(tags);
    postForm(QStringLiteral("/api/v2/torrents/removeTags"), form);
}

void QBittorrentBackend::setTags(const QString &hash, const QString &newTags, const QString &oldTags)
{
    const QStringList oldList = splitTagList(oldTags);
    const QStringList newList = splitTagList(newTags);
    const QSet<QString> oldSet(oldList.cbegin(), oldList.cend());
    const QSet<QString> newSet(newList.cbegin(), newList.cend());

    QStringList toRemove;
    for (const QString &t : oldSet) {
        if (!newSet.contains(t))
            toRemove.append(t);
    }

    QStringList toAdd;
    for (const QString &t : newSet) {
        if (!oldSet.contains(t))
            toAdd.append(t);
    }

    if (!toRemove.isEmpty())
        removeTags(hash, toRemove.join(QLatin1Char(',')));
    if (!toAdd.isEmpty())
        addTags(hash, toAdd.join(QLatin1Char(',')));
}

// ------------------------------------------------------------- trackers ----

void QBittorrentBackend::addTrackers(const QString &hash, const QStringList &trackers)
{
    if (trackers.isEmpty())
        return;

    const QString form = QStringLiteral("hash=") + ClientTransport::esc(hash)
                         + QStringLiteral("&urls=") + ClientTransport::esc(trackers.join(QLatin1Char('\n')));
    postForm(QStringLiteral("/api/v2/torrents/addTrackers"), form);
}

void QBittorrentBackend::removeTrackers(const QString &hash, const QStringList &trackers)
{
    if (trackers.isEmpty())
        return;

    const QString form = QStringLiteral("hash=") + ClientTransport::esc(hash)
                        + QStringLiteral("&urls=") + ClientTransport::esc(trackers.join(QLatin1Char('|')));
    postForm(QStringLiteral("/api/v2/torrents/removeTrackers"), form);
}

QStringList QBittorrentBackend::torrentTrackers(const QString &hash)
{
    const QString body = get(QStringLiteral("/api/v2/torrents/trackers?hash=") + ClientTransport::esc(hash));
    const QJsonArray arr = _sh.parseJson(body.toUtf8(), QStringLiteral("server on torrents/trackers")).array();
    QStringList out;
    for (const QJsonValue &v : arr) {
        const QString url = v.toObject().value(QStringLiteral("url")).toString();
        // qBittorrent lists DHT/PEX/LSD pseudo-trackers here; skip them.
        if (!url.isEmpty() && !url.startsWith(QLatin1String("**")))
            out.append(url);
    }
    return out;
}

void QBittorrentBackend::replaceTracker(const QString &hash, const QString &origUrl, const QString &newUrl)
{
    // qBittorrent 5.2+ expects "url" for the original tracker; older
    // versions used "origUrl" — send both, extra params are ignored.
    const QString form = QStringLiteral("hash=") + ClientTransport::esc(hash)
                       + QStringLiteral("&url=") + ClientTransport::esc(origUrl)
                       + QStringLiteral("&origUrl=") + ClientTransport::esc(origUrl)
                       + QStringLiteral("&newUrl=") + ClientTransport::esc(newUrl);
    postForm(QStringLiteral("/api/v2/torrents/editTracker"), form);
}

// ------------------------------------------------------------- the rest ----

void QBittorrentBackend::rename(const QString &hash, const QString &name)
{
    const QString form = QStringLiteral("hash=") + ClientTransport::esc(hash) + QStringLiteral("&name=") + ClientTransport::esc(name);
    postForm(QStringLiteral("/api/v2/torrents/rename"), form);
}

void QBittorrentBackend::setAutoManagement(const QString &hash, bool enable)
{
    const QString form = formHashes(hash)
                         + QStringLiteral("&enable=") + (enable ? QStringLiteral("true") : QStringLiteral("false"));
    postForm(QStringLiteral("/api/v2/torrents/setAutoManagement"), form);
}

void QBittorrentBackend::setDownloadLimit(const QString &hash, qint64 bytesPerSec)
{
    const QString form = formHashes(hash) + QStringLiteral("&limit=") + QString::number(bytesPerSec);
    postForm(QStringLiteral("/api/v2/torrents/setDownloadLimit"), form);
}

void QBittorrentBackend::setUploadLimit(const QString &hash, qint64 bytesPerSec)
{
    const QString form = formHashes(hash) + QStringLiteral("&limit=") + QString::number(bytesPerSec);
    postForm(QStringLiteral("/api/v2/torrents/setUploadLimit"), form);
}

void QBittorrentBackend::toggleSequentialDownload(const QString &hash)
{
    postForm(QStringLiteral("/api/v2/torrents/toggleSequentialDownload"), formHashes(hash));
}

void QBittorrentBackend::toggleFirstLastPiecePrio(const QString &hash)
{
    postForm(QStringLiteral("/api/v2/torrents/toggleFirstLastPiecePrio"), formHashes(hash));
}
