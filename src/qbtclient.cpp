#include "qbtclient.h"
#include "config.h"
#include "logger.h"

#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QSslError>
#include <QEventLoop>
#include <QTimer>
#include <QUrl>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonValue>
#include <QHttpMultiPart>
#include <QHttpPart>
#include <QThread>
#include <QElapsedTimer>
#include <QDateTime>
#include <QHash>

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
} // namespace

QbtException::QbtException(const QString &msg, int st, const QString &b)
    : message(msg), status(st), body(b), _utf8(msg.toUtf8())
{
}

TorrentInfo TorrentInfo::from(const QJsonObject &d)
{
    TorrentInfo t;
    t.hash               = d.value(QStringLiteral("hash")).toString();
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

QbtClient::QbtClient(const Config &cfg, std::function<void(const QString &)> log,
                     std::function<bool()> isCancelled)
    : _cfg(cfg), _log(std::move(log)), _isCancelled(std::move(isCancelled)), _base(cfg.baseUrl())
{
    if (!_log)
        _log = [](const QString &) {};
    _nam = new QNetworkAccessManager();
    _nam->setCookieJar(new QNetworkCookieJar(_nam));

    if (cfg.clientType.compare(QLatin1String("transmission"), Qt::CaseInsensitive) == 0) {
        clientType = ClientType::Transmission;
        detectedType = ClientType::Transmission;
    } else if (cfg.clientType.compare(QLatin1String("aria2"), Qt::CaseInsensitive) == 0) {
        clientType = ClientType::Aria2;
        detectedType = ClientType::Aria2;
    } else if (cfg.clientType.compare(QLatin1String("qbittorrent"), Qt::CaseInsensitive) == 0) {
        clientType = ClientType::QBittorrent;
        detectedType = ClientType::QBittorrent;
    } else {
        clientType = ClientType::Auto;
    }
}

QbtClient::~QbtClient()
{
    delete _nam;
}

void QbtClient::trace(const QString &s)
{
    _log(s);
    Log::write(s);
}

QString QbtClient::esc(const QString &s)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(s));
}

void QbtClient::prepare(QNetworkRequest &req) const
{
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("QtMagnet/1.0"));
    req.setRawHeader("Referer", _base.toUtf8());
    req.setRawHeader("Origin", _base.toUtf8());
}

void QbtClient::setBasicAuth(QNetworkRequest &req) const
{
    const QString pass = _cfg.getPassword();
    if (!_cfg.username.isEmpty() || !pass.isEmpty()) {
        const QString creds = _cfg.username + QLatin1Char(':') + pass;
        req.setRawHeader("Authorization", "Basic " + creds.toUtf8().toBase64());
    }
}

QbtClient::RawResponse QbtClient::execRaw(const std::function<QNetworkReply *()> &make)
{
    QNetworkReply *reply = make();

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

    QTimer timer;
    timer.setSingleShot(true);
    bool timedOut = false;
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        timedOut = true;
        reply->abort();
    });
    timer.start(qMax(5, _cfg.requestTimeoutSec) * 1000);

    QTimer cancelTimer;
    if (_isCancelled) {
        cancelTimer.setInterval(100);
        QObject::connect(&cancelTimer, &QTimer::timeout, &loop, [&] {
            if (_isCancelled())
                reply->abort();
        });
        cancelTimer.start();
    }

    if (!reply->isFinished())
        loop.exec();
    timer.stop();
    cancelTimer.stop();

    RawResponse resp;
    resp.body = reply->readAll();
    resp.networkError = static_cast<int>(reply->error());
    resp.statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    resp.errorString = reply->errorString();
    resp.timedOut = timedOut;
    resp.rawHeaders = reply->rawHeaderPairs();
    delete reply;

    if (_isCancelled && _isCancelled())
        throw QbtException(QCoreApplication::translate("QbtClient", "Operation cancelled."), 0, QString());

    return resp;
}

QString QbtClient::exec(const std::function<QNetworkReply *()> &make, const QString &path, bool allowRelogin)
{
    RawResponse r = execRaw(make);

    if (r.networkError == QNetworkReply::NoError)
        return QString::fromUtf8(r.body);

    if (r.timedOut || r.statusCode == 0)
        throw QbtException(QCoreApplication::translate("QbtClient", "No connection to %1: %2").arg(_base, r.errorString), 0, QString());

    const QString text = QString::fromUtf8(r.body);
    if (r.statusCode == 403 && allowRelogin) {
        trace(QStringLiteral("403 on %1: session expired, logging in again").arg(path));
        login();
        return exec(make, path, false);
    }

    throw QbtException(QStringLiteral("HTTP %1 on %2%3")
                           .arg(QString::number(r.statusCode), path,
                                text.length() > 0 ? QStringLiteral(": ") + text.trimmed() : QString()),
                       r.statusCode, text);
}

QString QbtClient::get(const QString &path, bool allowRelogin)
{
    return exec([this, path] {
        QNetworkRequest req{ QUrl(_base + path) };
        prepare(req);
        return _nam->get(req);
    }, path, allowRelogin);
}

QString QbtClient::postForm(const QString &path, const QString &formBody, bool allowRelogin)
{
    const QByteArray data = formBody.toUtf8();
    return exec([this, path, data] {
        QNetworkRequest req{ QUrl(_base + path) };
        prepare(req);
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded; charset=UTF-8"));
        return _nam->post(req, data);
    }, path, allowRelogin);
}

QString QbtClient::postMultipart(const QString &path, const QVector<QPair<QString, QString>> &fields, bool allowRelogin)
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
        QNetworkRequest req{ QUrl(_base + path) };
        prepare(req);
        QNetworkReply *reply = _nam->post(req, multi);
        multi->setParent(reply);
        return reply;
    }, path, allowRelogin);
}

QString QbtClient::postMultipartWithFile(const QString &path, const QVector<QPair<QString, QString>> &fields,
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

        QNetworkRequest req{ QUrl(_base + path) };
        prepare(req);
        QNetworkReply *reply = _nam->post(req, multi);
        multi->setParent(reply);
        return reply;
    }, path, allowRelogin);
}

bool QbtClient::hasSessionCookie()
{
    if (!_nam->cookieJar())
        return false;
    const QList<QNetworkCookie> cookies = _nam->cookieJar()->cookiesForUrl(QUrl(_base));
    for (const QNetworkCookie &c : cookies) {
        if (c.name().compare(QByteArrayLiteral("SID"), Qt::CaseInsensitive) == 0 && !c.value().isEmpty())
            return true;
    }
    return false;
}

bool QbtClient::verifyAuthenticated()
{
    try {
        const QString v = get(QStringLiteral("/api/v2/app/version"), false);
        return !v.trimmed().isEmpty();
    } catch (const std::exception &) {
        return false;
    }
}

void QbtClient::loginQBittorrent()
{
    const QString pass = _cfg.getPassword();
    const QString form = QStringLiteral("username=") + esc(_cfg.username) + QStringLiteral("&password=") + esc(pass);

    RawResponse r = execRaw([this, &form] {
        QNetworkRequest req{ QUrl(_base + QStringLiteral("/api/v2/auth/login")) };
        prepare(req);
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded; charset=UTF-8"));
        return _nam->post(req, form.toUtf8());
    });

    if (r.timedOut || (r.networkError != QNetworkReply::NoError && r.statusCode == 0))
        throw QbtException(QCoreApplication::translate("QbtClient", "No connection to %1: %2").arg(_base, r.errorString), 0, QString());

    const QString res = QString::fromUtf8(r.body).trimmed();
    if (res == QLatin1String("Fails."))
        throw QbtException(QCoreApplication::translate("QbtClient", "Invalid username or password."), 403, res);

    if (!hasSessionCookie() && !verifyAuthenticated())
        throw QbtException(QCoreApplication::translate("QbtClient", "Authentication failed (no session cookie)."), r.statusCode, res);

    trace(QStringLiteral("Logged in to qBittorrent successfully."));
}

QJsonObject QbtClient::execTransmissionRpc(const QString &method, const QJsonObject &arguments)
{
    QJsonObject reqObj;
    reqObj[QStringLiteral("method")] = method;
    if (!arguments.isEmpty())
        reqObj[QStringLiteral("arguments")] = arguments;
    reqObj[QStringLiteral("tag")] = ++_transmissionTag;

    const QByteArray body = QJsonDocument(reqObj).toJson(QJsonDocument::Compact);
    RawResponse r;

    for (int attempt = 0; attempt < 2; ++attempt) {
        r = execRaw([this, &body] {
            QNetworkRequest req{ QUrl(_base + QStringLiteral("/transmission/rpc")) };
            prepare(req);
            req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
            if (!_transmissionSessionId.isEmpty())
                req.setRawHeader("X-Transmission-Session-Id", _transmissionSessionId.toUtf8());
            setBasicAuth(req);
            return _nam->post(req, body);
        });

        QByteArray newSessionId = r.header("X-Transmission-Session-Id");
        if (r.statusCode == 409 && attempt == 0 && !newSessionId.isEmpty()) {
            _transmissionSessionId = QString::fromUtf8(newSessionId);
            trace(QStringLiteral("Received Transmission session ID, retrying request..."));
            continue;
        }
        break;
    }

    if (r.timedOut || (r.networkError != QNetworkReply::NoError && r.statusCode == 0))
        throw QbtException(QCoreApplication::translate("QbtClient", "No connection to %1: %2").arg(_base, r.errorString), 0, QString());

    if (r.statusCode == 401)
        throw QbtException(QCoreApplication::translate("QbtClient", "Invalid username or password."), 401, QString::fromUtf8(r.body));

    if (r.statusCode != 200)
        throw QbtException(QStringLiteral("HTTP %1 on /transmission/rpc: %2").arg(QString::number(r.statusCode), QString::fromUtf8(r.body).trimmed()), r.statusCode, QString::fromUtf8(r.body));

    QJsonParseError perr{};
    QJsonDocument doc = QJsonDocument::fromJson(r.body, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject())
        throw QbtException(QCoreApplication::translate("QbtClient", "Invalid JSON from Transmission: %1").arg(perr.errorString()), r.statusCode, QString::fromUtf8(r.body));

    QJsonObject res = doc.object();
    QString result = res.value(QStringLiteral("result")).toString();
    if (result != QLatin1String("success"))
        throw QbtException(QCoreApplication::translate("QbtClient", "Transmission error: %1").arg(result), r.statusCode, QString::fromUtf8(r.body));

    return res.value(QStringLiteral("arguments")).toObject();
}

void QbtClient::loginTransmission()
{
    QJsonObject args;
    QJsonArray fields;
    fields.append(QStringLiteral("version"));
    fields.append(QStringLiteral("rpc-version"));
    args[QStringLiteral("fields")] = fields;

    QJsonObject res = execTransmissionRpc(QStringLiteral("session-get"), args);
    appVersion = QStringLiteral("Transmission ") + res.value(QStringLiteral("version")).toString();
    apiVersion = QStringLiteral("RPC ") + QString::number(res.value(QStringLiteral("rpc-version")).toInt());
    trace(QStringLiteral("Logged in to %1 (%2)").arg(appVersion, apiVersion));
}

QJsonValue QbtClient::execAria2Rpc(const QString &method, const QJsonArray &params)
{
    QJsonObject reqObj;
    reqObj[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    reqObj[QStringLiteral("id")] = QStringLiteral("qt-magnet-") + QString::number(++_aria2Tag);
    reqObj[QStringLiteral("method")] = method;

    QJsonArray fullParams;
    const QString pass = _cfg.getPassword();
    if (!pass.isEmpty()) {
        fullParams.append(QStringLiteral("token:") + pass);
    }
    for (const QJsonValue &v : params)
        fullParams.append(v);

    reqObj[QStringLiteral("params")] = fullParams;

    const QByteArray body = QJsonDocument(reqObj).toJson(QJsonDocument::Compact);

    RawResponse r = execRaw([this, &body] {
        QNetworkRequest req{ QUrl(_base + QStringLiteral("/jsonrpc")) };
        prepare(req);
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        setBasicAuth(req);
        return _nam->post(req, body);
    });

    if (r.timedOut || (r.networkError != QNetworkReply::NoError && r.statusCode == 0))
        throw QbtException(QCoreApplication::translate("QbtClient", "No connection to %1: %2").arg(_base, r.errorString), 0, QString());

    if (r.statusCode == 401)
        throw QbtException(QCoreApplication::translate("QbtClient", "Invalid username, password, or RPC secret."), 401, QString::fromUtf8(r.body));

    if (r.statusCode != 200)
        throw QbtException(QStringLiteral("HTTP %1 on /jsonrpc: %2").arg(QString::number(r.statusCode), QString::fromUtf8(r.body).trimmed()), r.statusCode, QString::fromUtf8(r.body));

    QJsonParseError perr{};
    QJsonDocument doc = QJsonDocument::fromJson(r.body, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject())
        throw QbtException(QCoreApplication::translate("QbtClient", "Invalid JSON from Aria2: %1").arg(perr.errorString()), r.statusCode, QString::fromUtf8(r.body));

    QJsonObject res = doc.object();
    if (res.contains(QStringLiteral("error"))) {
        QJsonObject err = res.value(QStringLiteral("error")).toObject();
        QString errMsg = err.value(QStringLiteral("message")).toString();
        int code = err.value(QStringLiteral("code")).toInt();
        throw QbtException(QCoreApplication::translate("QbtClient", "Aria2 error %1: %2").arg(QString::number(code), errMsg), r.statusCode, QString::fromUtf8(r.body));
    }

    return res.value(QStringLiteral("result"));
}

void QbtClient::loginAria2()
{
    QJsonValue res = execAria2Rpc(QStringLiteral("aria2.getVersion"));
    QJsonObject vObj = res.toObject();
    appVersion = QStringLiteral("Aria2 ") + vObj.value(QStringLiteral("version")).toString();
    apiVersion = QStringLiteral("JSON-RPC 2.0");
    trace(QStringLiteral("Logged in to %1 (%2)").arg(appVersion, apiVersion));
}

QString QbtClient::findGidByHash(const QString &hash)
{
    const QString hLower = hash.toLower();
    if (_hashToGid.contains(hLower)) {
        QString gid = _hashToGid.value(hLower);
        try {
            QJsonArray params;
            params.append(gid);
            QJsonObject st = execAria2Rpc(QStringLiteral("aria2.tellStatus"), params).toObject();
            QJsonArray followed = st.value(QStringLiteral("followedBy")).toArray();
            if (!followed.isEmpty()) {
                QString childGid = followed.last().toString();
                if (!childGid.isEmpty()) {
                    _hashToGid[hLower] = childGid;
                    return childGid;
                }
            }
            return gid;
        } catch (const std::exception &) {}
    }

    auto checkList = [&](const QString &method, const QJsonArray &extraArgs = QJsonArray()) -> QString {
        try {
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
                        QJsonArray followed = o.value(QStringLiteral("followedBy")).toArray();
                        if (!followed.isEmpty()) {
                            QString childGid = followed.last().toString();
                            if (!childGid.isEmpty()) {
                                _hashToGid[hLower] = childGid;
                                return childGid;
                            }
                        }
                        return curGid;
                    }
                }
            }
        } catch (const std::exception &) {}
        return QString();
    };

    QString found = checkList(QStringLiteral("aria2.tellActive"));
    if (!found.isEmpty()) return found;

    QJsonArray pageArgs;
    pageArgs.append(0);
    pageArgs.append(1000);
    found = checkList(QStringLiteral("aria2.tellWaiting"), pageArgs);
    if (!found.isEmpty()) return found;

    found = checkList(QStringLiteral("aria2.tellStopped"), pageArgs);
    if (!found.isEmpty()) return found;

    return QString();
}

void QbtClient::login()
{
    if (clientType == ClientType::Aria2) {
        detectedType = ClientType::Aria2;
        loginAria2();
        return;
    }
    if (clientType == ClientType::Transmission) {
        detectedType = ClientType::Transmission;
        loginTransmission();
        return;
    }
    if (clientType == ClientType::QBittorrent) {
        detectedType = ClientType::QBittorrent;
        loginQBittorrent();
        return;
    }

    try {
        trace(QStringLiteral("Probing for qBittorrent at %1").arg(_base));
        loginQBittorrent();
        detectedType = ClientType::QBittorrent;
        trace(QStringLiteral("Detected server: qBittorrent"));
        return;
    } catch (const QbtException &qbtEx) {
        trace(QStringLiteral("qBittorrent probe failed: %1. Probing for Transmission...").arg(qbtEx.message));
        try {
            loginTransmission();
            detectedType = ClientType::Transmission;
            trace(QStringLiteral("Detected server: Transmission"));
            return;
        } catch (const QbtException &trEx) {
            trace(QStringLiteral("Transmission probe failed: %1. Probing for Aria2...").arg(trEx.message));
            try {
                loginAria2();
                detectedType = ClientType::Aria2;
                trace(QStringLiteral("Detected server: Aria2"));
                return;
            } catch (const QbtException &ariaEx) {
                throw QbtException(QCoreApplication::translate("QbtClient", "Could not connect to qBittorrent (%1), Transmission (%2), or Aria2 (%3)")
                                       .arg(qbtEx.message, trEx.message, ariaEx.message),
                                   qbtEx.status != 0 ? qbtEx.status : (trEx.status != 0 ? trEx.status : ariaEx.status));
            }
        }
    }
}

void QbtClient::fetchServerInfo()
{
    if (detectedType == ClientType::Aria2) {
        loginAria2();
    } else if (detectedType == ClientType::Transmission) {
        loginTransmission();
    } else {
        try {
            appVersion = get(QStringLiteral("/api/v2/app/version")).trimmed();
        } catch (const std::exception &ex) {
            trace(QStringLiteral("Could not fetch app version: %1").arg(QString::fromUtf8(ex.what())));
        }
        try {
            apiVersion = get(QStringLiteral("/api/v2/app/webapiVersion")).trimmed();
        } catch (const std::exception &ex) {
            trace(QStringLiteral("Could not fetch web API version: %1").arg(QString::fromUtf8(ex.what())));
        }
        trace(QStringLiteral("qBittorrent %1, Web API %2").arg(appVersion, apiVersion));
    }
}

QJsonObject QbtClient::preferences()
{
    if (detectedType == ClientType::Aria2) {
        QJsonObject res;
        try {
            QJsonValue opt = execAria2Rpc(QStringLiteral("aria2.getGlobalOption"));
            res = opt.toObject();
            if (res.contains(QStringLiteral("pause")))
                serverAddsStopped = (res.value(QStringLiteral("pause")).toString() == QLatin1String("true"));
        } catch (const std::exception &ex) {
            trace(QStringLiteral("Aria2 getGlobalOption error: %1").arg(QString::fromUtf8(ex.what())));
        }
        return res;
    }
    if (detectedType == ClientType::Transmission) {
        QJsonObject res = execTransmissionRpc(QStringLiteral("session-get"), QJsonObject());
        if (res.contains(QStringLiteral("start-added-torrents")))
            serverAddsStopped = !res.value(QStringLiteral("start-added-torrents")).toBool(true);
        return res;
    }

    const QString json = get(QStringLiteral("/api/v2/app/preferences"));
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    const QJsonObject obj = doc.object();

    if (obj.contains(QStringLiteral("add_stopped_enabled")))
        serverAddsStopped = obj.value(QStringLiteral("add_stopped_enabled")).toBool();
    else if (obj.contains(QStringLiteral("start_paused_enabled")))
        serverAddsStopped = obj.value(QStringLiteral("start_paused_enabled")).toBool();

    return obj;
}

QString QbtClient::defaultSavePath()
{
    if (detectedType == ClientType::Aria2) {
        try {
            QJsonValue opt = execAria2Rpc(QStringLiteral("aria2.getGlobalOption"));
            return opt.toObject().value(QStringLiteral("dir")).toString();
        } catch (const std::exception &) {
            return QString();
        }
    }
    if (detectedType == ClientType::Transmission) {
        try {
            QJsonObject res = execTransmissionRpc(QStringLiteral("session-get"), QJsonObject());
            return res.value(QStringLiteral("download-dir")).toString();
        } catch (const std::exception &) {
            return QString();
        }
    }

    try {
        return get(QStringLiteral("/api/v2/app/defaultSavePath")).trimmed();
    } catch (const std::exception &) {
        return QString();
    }
}

QVector<QPair<QString, QString>> QbtClient::categories()
{
    if (detectedType == ClientType::Transmission || detectedType == ClientType::Aria2)
        return {};

    const QString json = get(QStringLiteral("/api/v2/torrents/categories"));
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    QVector<QPair<QString, QString>> list;
    const QJsonObject obj = doc.object();
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        const QJsonObject c = it.value().toObject();
        list.append({ it.key(), c.value(QStringLiteral("savePath")).toString() });
    }
    return list;
}

QStringList QbtClient::tags()
{
    if (detectedType == ClientType::Aria2)
        return {};

    if (detectedType == ClientType::Transmission) {
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

    const QString json = get(QStringLiteral("/api/v2/torrents/tags"));
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    QStringList list;
    const QJsonArray arr = doc.array();
    for (const QJsonValue &v : arr)
        list.append(v.toString());
    return list;
}

QVector<TorrentInfo> QbtClient::info(const QString &hashes)
{
    if (detectedType == ClientType::Aria2) {
        QVector<TorrentInfo> list;
        auto collect = [&](const QString &method, const QJsonArray &extra = QJsonArray()) {
            try {
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

                    if (hashes.isEmpty() || (!iHash.isEmpty() && hashes.contains(iHash, Qt::CaseInsensitive)))
                        list.append(t);
                }
            } catch (const std::exception &) {}
        };

        collect(QStringLiteral("aria2.tellActive"));
        QJsonArray p; p.append(0); p.append(1000);
        collect(QStringLiteral("aria2.tellWaiting"), p);
        collect(QStringLiteral("aria2.tellStopped"), p);
        return list;
    }

    if (detectedType == ClientType::Transmission) {
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
            list.append(t);
        }
        return list;
    }

    QString path = QStringLiteral("/api/v2/torrents/info");
    if (!hashes.isEmpty())
        path += QStringLiteral("?hashes=") + esc(hashes);

    const QString json = get(path);
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    QVector<TorrentInfo> list;
    const QJsonArray arr = doc.array();
    for (const QJsonValue &v : arr)
        list.append(TorrentInfo::from(v.toObject()));
    return list;
}

std::optional<TorrentInfo> QbtClient::infoOne(const QString &hash)
{
    const auto list = info(hash);
    if (!list.isEmpty())
        return list.front();
    return std::nullopt;
}

QSet<QString> QbtClient::allHashes()
{
    QSet<QString> set;
    for (const TorrentInfo &t : info(QString()))
        if (!t.hash.isEmpty())
            set.insert(t.hash.toLower());
    return set;
}

void QbtClient::addMagnet(const QString &magnet, bool addStopped, bool stopAfterMetadata,
                          const QString &savepath, const QString &category,
                          const QString &tags, const QString &contentLayout)
{
    if (detectedType == ClientType::Aria2) {
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
            try {
                QJsonArray stParams;
                stParams.append(gid);
                QJsonObject st = execAria2Rpc(QStringLiteral("aria2.tellStatus"), stParams).toObject();
                QString h = st.value(QStringLiteral("infoHash")).toString().toLower();
                if (!h.isEmpty())
                    _hashToGid[h] = gid;
            } catch (const std::exception &ex) {
                trace(QStringLiteral("Aria2 tellStatus on addMagnet: %1").arg(QString::fromUtf8(ex.what())));
            }
        }
        return;
    }

    if (detectedType == ClientType::Transmission) {
        QJsonObject args;
        args[QStringLiteral("filename")] = magnet;
        if (addStopped || stopAfterMetadata)
            args[QStringLiteral("paused")] = true;
        if (!savepath.isEmpty())
            args[QStringLiteral("download-dir")] = savepath;
        QStringList allLabels;
        if (!category.isEmpty())
            allLabels.append(category);
        if (!tags.isEmpty()) {
            const QStringList tagList = tags.split(QLatin1Char(','), Qt::SkipEmptyParts);
            for (const QString &t : tagList)
                allLabels.append(t.trimmed());
        }
        if (!allLabels.isEmpty()) {
            QJsonArray arr;
            for (const QString &l : allLabels)
                arr.append(l);
            args[QStringLiteral("labels")] = arr;
        }
        execTransmissionRpc(QStringLiteral("torrent-add"), args);
        return;
    }

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

void QbtClient::addTorrentFile(const QByteArray &torrentData, const QString &fileName,
                              bool addStopped, bool stopAfterMetadata,
                              const QString &savepath, const QString &category,
                              const QString &tags, const QString &contentLayout)
{
    if (detectedType == ClientType::Aria2) {
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
            try {
                QJsonArray stParams;
                stParams.append(gid);
                QJsonObject st = execAria2Rpc(QStringLiteral("aria2.tellStatus"), stParams).toObject();
                QString h = st.value(QStringLiteral("infoHash")).toString().toLower();
                if (!h.isEmpty())
                    _hashToGid[h] = gid;
            } catch (const std::exception &ex) {
                trace(QStringLiteral("Aria2 tellStatus on addTorrent: %1").arg(QString::fromUtf8(ex.what())));
            }
        }
        return;
    }

    if (detectedType == ClientType::Transmission) {
        QJsonObject args;
        args[QStringLiteral("metainfo")] = QString::fromLatin1(torrentData.toBase64());
        if (addStopped || stopAfterMetadata)
            args[QStringLiteral("paused")] = true;
        if (!savepath.isEmpty())
            args[QStringLiteral("download-dir")] = savepath;
        QStringList allLabels;
        if (!category.isEmpty())
            allLabels.append(category);
        if (!tags.isEmpty()) {
            const QStringList tagList = tags.split(QLatin1Char(','), Qt::SkipEmptyParts);
            for (const QString &t : tagList)
                allLabels.append(t.trimmed());
        }
        if (!allLabels.isEmpty()) {
            QJsonArray arr;
            for (const QString &l : allLabels)
                arr.append(l);
            args[QStringLiteral("labels")] = arr;
        }
        execTransmissionRpc(QStringLiteral("torrent-add"), args);
        return;
    }

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

QVector<TorrentFile> QbtClient::files(const QString &hash)
{
    if (detectedType == ClientType::Aria2) {
        try {
            QString gid = findGidByHash(hash);
            if (gid.isEmpty())
                return {};

            QJsonArray params;
            params.append(gid);
            QJsonObject st = execAria2Rpc(QStringLiteral("aria2.tellStatus"), params).toObject();

            QJsonArray followed = st.value(QStringLiteral("followedBy")).toArray();
            if (!followed.isEmpty()) {
                QString childGid = followed.last().toString();
                if (!childGid.isEmpty()) {
                    gid = childGid;
                    _hashToGid[hash.toLower()] = gid;
                    QJsonArray childParams;
                    childParams.append(gid);
                    st = execAria2Rpc(QStringLiteral("aria2.tellStatus"), childParams).toObject();
                }
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
        } catch (const std::exception &) {
            return {};
        }
    }

    if (detectedType == ClientType::Transmission) {
        try {
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
        } catch (const std::exception &ex) {
            trace(QStringLiteral("Transmission files error: %1").arg(QString::fromUtf8(ex.what())));
            return {};
        }
    }

    try {
        const QString json = get(QStringLiteral("/api/v2/torrents/files?hash=") + esc(hash));
        const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
        QVector<TorrentFile> list;
        const QJsonArray arr = doc.array();
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
    } catch (const std::exception &ex) {
        trace(QStringLiteral("qBittorrent files error: %1").arg(QString::fromUtf8(ex.what())));
        return {};
    }
}

void QbtClient::filePrio(const QString &hash, const QList<int> &ids, int priority)
{
    if (ids.isEmpty())
        return;

    if (detectedType == ClientType::Aria2) {
        QString gid = findGidByHash(hash);
        if (gid.isEmpty())
            return;
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
        QJsonArray params;
        params.append(gid);
        params.append(opt);
        try {
            execAria2Rpc(QStringLiteral("aria2.changeOption"), params);
        } catch (const std::exception &ex) {
            trace(QStringLiteral("Aria2 changeOption on filePrio: %1").arg(QString::fromUtf8(ex.what())));
        }
        return;
    }

    if (detectedType == ClientType::Transmission) {
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
        return;
    }

    QStringList sids;
    for (int id : ids)
        sids.append(QString::number(id));
    const QString form = QStringLiteral("hash=") + esc(hash)
                         + QStringLiteral("&id=") + esc(sids.join(QLatin1Char('|')))
                         + QStringLiteral("&priority=") + QString::number(priority);
    postForm(QStringLiteral("/api/v2/torrents/filePrio"), form);
}

void QbtClient::setForceStart(const QString &hash, bool value)
{
    if (detectedType == ClientType::Aria2) {
        if (value)
            startTorrent(hash);
        return;
    }

    if (detectedType == ClientType::Transmission) {
        QJsonObject args;
        args[QStringLiteral("ids")] = QJsonArray{hash};
        execTransmissionRpc(value ? QStringLiteral("torrent-start-now") : QStringLiteral("torrent-start"), args);
        return;
    }

    const QString form = QStringLiteral("hashes=") + esc(hash)
                         + QStringLiteral("&value=") + (value ? QStringLiteral("true") : QStringLiteral("false"));
    postForm(QStringLiteral("/api/v2/torrents/setForceStart"), form);
}

void QbtClient::callStartStop(const QString &hash, bool start)
{
    const QString hashes = QStringLiteral("hashes=") + esc(hash);

    if (!_useStartStop.has_value()) {
        try {
            postForm(start ? QStringLiteral("/api/v2/torrents/start") : QStringLiteral("/api/v2/torrents/stop"), hashes);
            _useStartStop = true;
            return;
        } catch (const QbtException &ex) {
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

void QbtClient::startTorrent(const QString &hash)
{
    if (detectedType == ClientType::Aria2) {
        QString gid = findGidByHash(hash);
        if (!gid.isEmpty()) {
            QJsonArray params;
            params.append(gid);
            try {
                execAria2Rpc(QStringLiteral("aria2.unpause"), params);
            } catch (const std::exception &ex) {
                trace(QStringLiteral("Aria2 unpause error: %1").arg(QString::fromUtf8(ex.what())));
            }
        }
        return;
    }

    if (detectedType == ClientType::Transmission) {
        QJsonObject args;
        args[QStringLiteral("ids")] = QJsonArray{hash};
        execTransmissionRpc(QStringLiteral("torrent-start"), args);
        return;
    }
    callStartStop(hash, true);
}

void QbtClient::stopTorrent(const QString &hash)
{
    if (detectedType == ClientType::Aria2) {
        QString gid = findGidByHash(hash);
        if (!gid.isEmpty()) {
            QJsonArray params;
            params.append(gid);
            try {
                execAria2Rpc(QStringLiteral("aria2.pause"), params);
            } catch (const std::exception &ex) {
                trace(QStringLiteral("Aria2 pause error: %1").arg(QString::fromUtf8(ex.what())));
            }
        }
        return;
    }

    if (detectedType == ClientType::Transmission) {
        QJsonObject args;
        args[QStringLiteral("ids")] = QJsonArray{hash};
        execTransmissionRpc(QStringLiteral("torrent-stop"), args);
        return;
    }
    callStartStop(hash, false);
}

void QbtClient::deleteTorrent(const QString &hash, bool deleteFiles)
{
    if (detectedType == ClientType::Aria2) {
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
                    trace(QStringLiteral("Aria2 remove error: %1").arg(QString::fromUtf8(ex.what())));
                }
            }
        }
        return;
    }

    if (detectedType == ClientType::Transmission) {
        QJsonObject args;
        args[QStringLiteral("ids")] = QJsonArray{hash};
        args[QStringLiteral("delete-local-data")] = deleteFiles;
        execTransmissionRpc(QStringLiteral("torrent-remove"), args);
        return;
    }

    const QString form = QStringLiteral("hashes=") + esc(hash)
                         + QStringLiteral("&deleteFiles=") + (deleteFiles ? QStringLiteral("true") : QStringLiteral("false"));
    postForm(QStringLiteral("/api/v2/torrents/delete"), form);
}

void QbtClient::setLocation(const QString &hash, const QString &location)
{
    if (detectedType == ClientType::Aria2) {
        QString gid = findGidByHash(hash);
        if (!gid.isEmpty()) {
            QJsonObject opt;
            opt[QStringLiteral("dir")] = location;
            QJsonArray params;
            params.append(gid);
            params.append(opt);
            try {
                execAria2Rpc(QStringLiteral("aria2.changeOption"), params);
            } catch (const std::exception &ex) {
                trace(QStringLiteral("Aria2 changeOption on setLocation: %1").arg(QString::fromUtf8(ex.what())));
            }
        }
        return;
    }

    if (detectedType == ClientType::Transmission) {
        QJsonObject args;
        args[QStringLiteral("ids")] = QJsonArray{hash};
        args[QStringLiteral("location")] = location;
        args[QStringLiteral("move")] = true;
        execTransmissionRpc(QStringLiteral("torrent-set-location"), args);
        return;
    }

    const QString form = QStringLiteral("hashes=") + esc(hash) + QStringLiteral("&location=") + esc(location);
    postForm(QStringLiteral("/api/v2/torrents/setLocation"), form);
}

void QbtClient::setCategory(const QString &hash, const QString &category)
{
    if (detectedType == ClientType::Transmission) {
        if (!category.isEmpty())
            addTags(hash, category);
        return;
    }
    if (detectedType == ClientType::Aria2)
        return;

    const QString form = QStringLiteral("hashes=") + esc(hash) + QStringLiteral("&category=") + esc(category);
    postForm(QStringLiteral("/api/v2/torrents/setCategory"), form);
}

void QbtClient::addTags(const QString &hash, const QString &tags)
{
    if (detectedType == ClientType::Transmission) {
        const QStringList tagList = tags.split(QLatin1Char(','), Qt::SkipEmptyParts);
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
            trace(QStringLiteral("Transmission get labels error: %1").arg(QString::fromUtf8(ex.what())));
        }

        for (const QString &t : tagList)
            allLabels.insert(t.trimmed());

        QJsonObject args;
        args[QStringLiteral("ids")] = QJsonArray{hash};
        QJsonArray labelsArr;
        for (const QString &l : allLabels)
            labelsArr.append(l);
        args[QStringLiteral("labels")] = labelsArr;
        execTransmissionRpc(QStringLiteral("torrent-set"), args);
        return;
    }
    if (detectedType == ClientType::Aria2)
        return;

    const QString form = QStringLiteral("hashes=") + esc(hash) + QStringLiteral("&tags=") + esc(tags);
    postForm(QStringLiteral("/api/v2/torrents/addTags"), form);
}

void QbtClient::addTrackers(const QString &hash, const QStringList &trackers)
{
    if (trackers.isEmpty())
        return;

    if (detectedType == ClientType::Transmission) {
        QJsonObject args;
        args[QStringLiteral("ids")] = QJsonArray{hash};
        args[QStringLiteral("trackerList")] = trackers.join(QLatin1Char('\n'));
        QJsonArray trAdd;
        for (const QString &tr : trackers)
            trAdd.append(tr);
        args[QStringLiteral("trackerAdd")] = trAdd;
        execTransmissionRpc(QStringLiteral("torrent-set"), args);
        return;
    }
    if (detectedType == ClientType::Aria2)
        return;

    const QString form = QStringLiteral("hash=") + esc(hash)
                         + QStringLiteral("&urls=") + esc(trackers.join(QLatin1Char('\n')));
    postForm(QStringLiteral("/api/v2/torrents/addTrackers"), form);
}

void QbtClient::rename(const QString &hash, const QString &name)
{
    if (detectedType == ClientType::Transmission) {
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
            trace(QStringLiteral("Transmission get name error: %1").arg(QString::fromUtf8(ex.what())));
        }
        if (oldName.isEmpty() || oldName == name)
            return;

        QJsonObject args;
        args[QStringLiteral("ids")] = QJsonArray{hash};
        args[QStringLiteral("path")] = oldName;
        args[QStringLiteral("name")] = name;
        execTransmissionRpc(QStringLiteral("torrent-rename-path"), args);
        return;
    }
    if (detectedType == ClientType::Aria2)
        return;

    const QString form = QStringLiteral("hash=") + esc(hash) + QStringLiteral("&name=") + esc(name);
    postForm(QStringLiteral("/api/v2/torrents/rename"), form);
}

void QbtClient::setAutoManagement(const QString &hash, bool enable)
{
    if (detectedType == ClientType::Transmission || detectedType == ClientType::Aria2)
        return;

    const QString form = QStringLiteral("hashes=") + esc(hash)
                         + QStringLiteral("&enable=") + (enable ? QStringLiteral("true") : QStringLiteral("false"));
    postForm(QStringLiteral("/api/v2/torrents/setAutoManagement"), form);
}

void QbtClient::setDownloadLimit(const QString &hash, qint64 bytesPerSec)
{
    if (detectedType == ClientType::Aria2) {
        QString gid = findGidByHash(hash);
        if (!gid.isEmpty()) {
            QJsonObject opt;
            opt[QStringLiteral("max-download-limit")] = QString::number(bytesPerSec);
            QJsonArray params;
            params.append(gid);
            params.append(opt);
            try {
                execAria2Rpc(QStringLiteral("aria2.changeOption"), params);
            } catch (const std::exception &ex) {
                trace(QStringLiteral("Aria2 changeOption on setDownloadLimit: %1").arg(QString::fromUtf8(ex.what())));
            }
        }
        return;
    }

    if (detectedType == ClientType::Transmission) {
        QJsonObject args;
        args[QStringLiteral("ids")] = QJsonArray{hash};
        args[QStringLiteral("downloadLimit")] = bytesPerSec > 0 ? (bytesPerSec + 1023) / 1024 : 0;
        args[QStringLiteral("downloadLimited")] = (bytesPerSec > 0);
        execTransmissionRpc(QStringLiteral("torrent-set"), args);
        return;
    }

    const QString form = QStringLiteral("hashes=") + esc(hash) + QStringLiteral("&limit=") + QString::number(bytesPerSec);
    postForm(QStringLiteral("/api/v2/torrents/setDownloadLimit"), form);
}

void QbtClient::setUploadLimit(const QString &hash, qint64 bytesPerSec)
{
    if (detectedType == ClientType::Aria2) {
        QString gid = findGidByHash(hash);
        if (!gid.isEmpty()) {
            QJsonObject opt;
            opt[QStringLiteral("max-upload-limit")] = QString::number(bytesPerSec);
            QJsonArray params;
            params.append(gid);
            params.append(opt);
            try {
                execAria2Rpc(QStringLiteral("aria2.changeOption"), params);
            } catch (const std::exception &ex) {
                trace(QStringLiteral("Aria2 changeOption on setUploadLimit: %1").arg(QString::fromUtf8(ex.what())));
            }
        }
        return;
    }

    if (detectedType == ClientType::Transmission) {
        QJsonObject args;
        args[QStringLiteral("ids")] = QJsonArray{hash};
        args[QStringLiteral("uploadLimit")] = bytesPerSec > 0 ? (bytesPerSec + 1023) / 1024 : 0;
        args[QStringLiteral("uploadLimited")] = (bytesPerSec > 0);
        execTransmissionRpc(QStringLiteral("torrent-set"), args);
        return;
    }

    const QString form = QStringLiteral("hashes=") + esc(hash) + QStringLiteral("&limit=") + QString::number(bytesPerSec);
    postForm(QStringLiteral("/api/v2/torrents/setUploadLimit"), form);
}

void QbtClient::toggleSequentialDownload(const QString &hash)
{
    if (detectedType == ClientType::Transmission) {
        QJsonObject args;
        args[QStringLiteral("ids")] = QJsonArray{hash};
        args[QStringLiteral("sequentialDownload")] = true;
        execTransmissionRpc(QStringLiteral("torrent-set"), args);
        return;
    }
    if (detectedType == ClientType::Aria2)
        return;

    postForm(QStringLiteral("/api/v2/torrents/toggleSequentialDownload"), QStringLiteral("hashes=") + esc(hash));
}

void QbtClient::toggleFirstLastPiecePrio(const QString &hash)
{
    if (detectedType == ClientType::Transmission || detectedType == ClientType::Aria2)
        return;

    postForm(QStringLiteral("/api/v2/torrents/toggleFirstLastPiecePrio"), QStringLiteral("hashes=") + esc(hash));
}

QString QbtClient::resolveHash(const QString &expectedHash, const QSet<QString> &before,
                                int timeoutMs, const std::function<bool()> &cancelled)
{
    QElapsedTimer sw;
    sw.start();
    while (sw.elapsed() < timeoutMs) {
        if (cancelled && cancelled())
            return QString();

        if (!expectedHash.isEmpty()) {
            const auto t = infoOne(expectedHash);
            if (t && !t->hash.isEmpty())
                return t->hash;
        } else {
            for (const TorrentInfo &t : info(QString())) {
                if (!before.contains(t.hash.toLower())) {
                    trace(QStringLiteral("Identified hash by list diff: %1").arg(t.hash));
                    return t.hash;
                }
            }
        }

        QThread::msleep(400);
    }
    return QString();
}

QVector<TorrentFile> QbtClient::waitForMetadata(const QString &hash, int timeoutSec,
                                                const std::function<void(int)> &onTick,
                                                const std::function<bool()> &cancelled, bool nudgeStart)
{
    QElapsedTimer sw;
    sw.start();
    int lastReported = -1;
    bool nudged = false;
    while (sw.elapsed() / 1000.0 < timeoutSec) {
        if (cancelled && cancelled())
            return {};

        const QVector<TorrentFile> list = files(hash);
        if (!list.isEmpty()) {
            trace(QStringLiteral("Metadata received in %1 s, files: %2")
                      .arg(int(sw.elapsed() / 1000)).arg(list.size()));
            return list;
        }

        if (nudgeStart) {
            try {
                const auto t = infoOne(hash);
                if (t && t->isStopped()) {
                    trace(QStringLiteral("Torrent stopped without metadata; starting to fetch metadata"));
                    startTorrent(hash);
                    nudged = true;
                }
            } catch (const QbtException &ex) {
                trace(QStringLiteral("Could not nudge start: %1").arg(ex.message));
            }
        }

        const int secs = int(sw.elapsed() / 1000);
        if (secs != lastReported && onTick) {
            lastReported = secs;
            onTick(secs);
        }
        QThread::msleep(600);
    }
    trace(QStringLiteral("Metadata not received within %1 s (tried starting: %2)")
              .arg(timeoutSec).arg(nudged ? QStringLiteral("true") : QStringLiteral("false")));
    return {};
}

bool QbtClient::forceStartVerified(const QString &hash, int attempts,
                                   const std::function<void(const QString &)> &progress)
{
    for (int i = 1; i <= attempts; ++i) {
        try {
            setForceStart(hash, true);
        } catch (const QbtException &ex) {
            trace(QStringLiteral("setForceStart attempt %1: %2").arg(i).arg(ex.message));
        }

        const int sleepTotal = (i == 1 ? 500 : 900);
        for (int ms = 0; ms < sleepTotal; ms += 100) {
            if (_isCancelled && _isCancelled())
                return false;
            QThread::msleep(qMin(100, sleepTotal - ms));
        }

        const auto t = infoOne(hash);
        if (!t) {
            trace(QStringLiteral("Attempt %1: torrent not found in list").arg(i));
            continue;
        }

        trace(QStringLiteral("Attempt %1: state '%2', force_start=%3")
                  .arg(i).arg(t->state, t->forceStart ? QStringLiteral("true") : QStringLiteral("false")));
        if (progress)
            progress(QCoreApplication::translate("QbtClient", "Verification %1/%2: %3")
                         .arg(QString::number(i), QString::number(attempts), stateString(t->state)));

        if (detectedType == ClientType::Transmission || detectedType == ClientType::Aria2) {
            if (!t->isStopped())
                return true;
        } else {
            if (t->forceStart && !t->isStopped())
                return true;
            if (t->isForced())
                return true;
        }

        try {
            startTorrent(hash);
        } catch (const QbtException &ex) {
            trace(QStringLiteral("start attempt %1: %2").arg(i).arg(ex.message));
        }
    }

    const auto last = infoOne(hash);
    if (detectedType == ClientType::Transmission || detectedType == ClientType::Aria2)
        return last && !last->isStopped();
    return last && last->forceStart && !last->isStopped();
}

QString QbtClient::stateString(const QString &state)
{
    static const QHash<QString, const char *> m = {
        { QStringLiteral("error"), QT_TRANSLATE_NOOP("QbtClient", "Error") },
        { QStringLiteral("missingFiles"), QT_TRANSLATE_NOOP("QbtClient", "Missing files") },
        { QStringLiteral("uploading"), QT_TRANSLATE_NOOP("QbtClient", "Uploading") },
        { QStringLiteral("pausedUP"), QT_TRANSLATE_NOOP("QbtClient", "Paused (upload)") },
        { QStringLiteral("stoppedUP"), QT_TRANSLATE_NOOP("QbtClient", "Paused (upload)") },
        { QStringLiteral("queuedUP"), QT_TRANSLATE_NOOP("QbtClient", "Queued (upload)") },
        { QStringLiteral("stalledUP"), QT_TRANSLATE_NOOP("QbtClient", "Stalled (upload)") },
        { QStringLiteral("checkingUP"), QT_TRANSLATE_NOOP("QbtClient", "Checking (upload)") },
        { QStringLiteral("forcedUP"), QT_TRANSLATE_NOOP("QbtClient", "Forced upload") },
        { QStringLiteral("allocating"), QT_TRANSLATE_NOOP("QbtClient", "Allocating") },
        { QStringLiteral("downloading"), QT_TRANSLATE_NOOP("QbtClient", "Downloading") },
        { QStringLiteral("metaDL"), QT_TRANSLATE_NOOP("QbtClient", "Fetching metadata") },
        { QStringLiteral("forcedMetaDL"), QT_TRANSLATE_NOOP("QbtClient", "Fetching metadata (forced)") },
        { QStringLiteral("pausedDL"), QT_TRANSLATE_NOOP("QbtClient", "Paused") },
        { QStringLiteral("stoppedDL"), QT_TRANSLATE_NOOP("QbtClient", "Paused") },
        { QStringLiteral("queuedDL"), QT_TRANSLATE_NOOP("QbtClient", "Queued") },
        { QStringLiteral("stalledDL"), QT_TRANSLATE_NOOP("QbtClient", "Stalled") },
        { QStringLiteral("checkingDL"), QT_TRANSLATE_NOOP("QbtClient", "Checking") },
        { QStringLiteral("forcedDL"), QT_TRANSLATE_NOOP("QbtClient", "Forced download") },
        { QStringLiteral("checkingResumeData"), QT_TRANSLATE_NOOP("QbtClient", "Checking resume data") },
        { QStringLiteral("moving"), QT_TRANSLATE_NOOP("QbtClient", "Moving") },
    };
    const auto it = m.constFind(state);
    return it != m.constEnd() ? QCoreApplication::translate("QbtClient", it.value()) : state;
}
