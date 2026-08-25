#include "mocktorrentserver.h"
#include "magnetlink.h"

#include <QUrl>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <iostream>

MockTorrentServer::MockTorrentServer(MockClientType type, QObject *parent)
    : QObject(parent), _type(type)
{
    _worker = new MockServerWorker(type);
    _worker->moveToThread(&_thread);
    _thread.start();
}

MockTorrentServer::~MockTorrentServer()
{
    stop();
    delete _worker;
    _worker = nullptr;
}

bool MockTorrentServer::start(quint16 port)
{
    if (!_worker) return false;
    bool ok = false;
    QMetaObject::invokeMethod(_worker, "startListening", Qt::BlockingQueuedConnection,
                              Q_RETURN_ARG(bool, ok), Q_ARG(quint16, port));
    return ok;
}

void MockTorrentServer::stop()
{
    if (_thread.isRunning() && _worker) {
        QMetaObject::invokeMethod(_worker, "stopListening", Qt::BlockingQueuedConnection);
        _thread.quit();
        _thread.wait(5000);
    }
}

quint16 MockTorrentServer::port() const
{
    return _worker ? _worker->port() : 0;
}

void MockTorrentServer::reset()
{
    if (_worker) _worker->reset();
}

void MockTorrentServer::addPreloadedTorrent(const MockTorrentData &t)
{
    if (_worker) _worker->addPreloadedTorrent(t);
}

int MockTorrentServer::requestCount() const
{
    return _worker ? _worker->requestCount() : 0;
}

QString MockTorrentServer::lastPath() const
{
    return _worker ? _worker->lastPath() : QString();
}

QMap<QString, MockTorrentData> MockTorrentServer::torrents() const
{
    return _worker ? _worker->torrents() : QMap<QString, MockTorrentData>();
}

void MockTorrentServer::setRequireAuth(bool req)
{
    if (_worker) _worker->setRequireAuth(req);
}

void MockTorrentServer::setAuthSuccess(bool ok)
{
    if (_worker) _worker->setAuthSuccess(ok);
}

void MockTorrentServer::setSessionValid(bool v)
{
    if (_worker) _worker->setSessionValid(v);
}

MockServerWorker::MockServerWorker(MockClientType type)
    : _type(type)
{
    reset();
}

MockServerWorker::~MockServerWorker()
{
    stopListening();
}

void MockServerWorker::reset()
{
    QMutexLocker locker(&_mutex);
    _torrents.clear();
    _categories.clear();
    _tags.clear();
    _requestCount = 0;
    _lastPath.clear();
    _idCounter = 1000;
    _requireAuth = true;
    _authSuccess = true;
    _sessionValid = true;

    _categories.insert(QStringLiteral("Linux"), QStringLiteral("C:/Downloads/Linux"));
    _categories.insert(QStringLiteral("Movies"), QStringLiteral("C:/Downloads/Movies"));
    _tags.append(QStringLiteral("iso"));
    _tags.append(QStringLiteral("distro"));
    _tags.append(QStringLiteral("verified"));
}

void MockServerWorker::addPreloadedTorrent(const MockTorrentData &t)
{
    QMutexLocker locker(&_mutex);
    _torrents.insert(t.hash.toLower(), t);
}

int MockServerWorker::requestCount() const
{
    QMutexLocker locker(&_mutex);
    return _requestCount;
}

QString MockServerWorker::lastPath() const
{
    QMutexLocker locker(&_mutex);
    return _lastPath;
}

QMap<QString, MockTorrentData> MockServerWorker::torrents() const
{
    QMutexLocker locker(&_mutex);
    return _torrents;
}

bool MockServerWorker::startListening(quint16 port)
{
    stopListening();
    _server = new QTcpServer(this);
    connect(_server, &QTcpServer::newConnection, this, &MockServerWorker::onNewConnection);
    if (!_server->listen(QHostAddress::LocalHost, port)) {
        std::cerr << "MockServerWorker listen failed: " << _server->errorString().toStdString() << std::endl;
        delete _server;
        _server = nullptr;
        _port = 0;
        return false;
    }
    _port = _server->serverPort();
    return true;
}

void MockServerWorker::stopListening()
{
    if (_server) {
        _server->close();
        delete _server;
        _server = nullptr;
    }
    _socketBuffers.clear();
    _port = 0;
}

void MockServerWorker::onNewConnection()
{
    while (_server && _server->hasPendingConnections()) {
        QTcpSocket *socket = _server->nextPendingConnection();
        socket->setParent(this);
        _socketBuffers.insert(socket, QByteArray());
        connect(socket, &QTcpSocket::readyRead, this, &MockServerWorker::onReadyRead);
        connect(socket, &QTcpSocket::disconnected, this, &MockServerWorker::onSocketDisconnected);
    }
}

void MockServerWorker::onSocketDisconnected()
{
    auto *socket = qobject_cast<QTcpSocket *>(sender());
    if (socket) {
        _socketBuffers.remove(socket);
        socket->deleteLater();
    }
}

void MockServerWorker::onReadyRead()
{
    auto *socket = qobject_cast<QTcpSocket *>(sender());
    if (!socket)
        return;

    QByteArray &buf = _socketBuffers[socket];
    buf.append(socket->readAll());

    int headerEnd = buf.indexOf("\r\n\r\n");
    if (headerEnd == -1)
        return;

    QByteArray headerBytes = buf.left(headerEnd);
    int contentLength = 0;
    QByteArrayList lines = headerBytes.split('\n');
    for (const QByteArray &line : lines) {
        QByteArray trimmed = line.trimmed();
        if (trimmed.toLower().startsWith("content-length:")) {
            contentLength = trimmed.mid(15).trimmed().toInt();
            break;
        }
    }

    int totalRequired = headerEnd + 4 + contentLength;
    if (buf.size() < totalRequired)
        return;

    QByteArray requestData = buf.left(totalRequired);
    buf.remove(0, totalRequired);

    HttpRequest req = parseHttpRequest(requestData);
    handleRequest(socket, req);
}

HttpRequest MockServerWorker::parseHttpRequest(const QByteArray &data)
{
    HttpRequest req;
    int headerEnd = data.indexOf("\r\n\r\n");
    if (headerEnd == -1)
        return req;

    QByteArray headerPart = data.left(headerEnd);
    req.body = data.mid(headerEnd + 4);

    QByteArrayList lines = headerPart.split('\n');
    if (lines.isEmpty())
        return req;

    QByteArray firstLine = lines[0].trimmed();
    QByteArrayList parts = firstLine.split(' ');
    if (parts.size() >= 2) {
        req.method = QString::fromLatin1(parts[0]).toUpper();
        QString fullPath = QString::fromUtf8(parts[1]);
        int qIdx = fullPath.indexOf(QLatin1Char('?'));
        if (qIdx != -1) {
            req.path = fullPath.left(qIdx);
            req.queryString = fullPath.mid(qIdx + 1);
            req.queryParams = parseQueryParams(req.queryString);
        } else {
            req.path = fullPath;
        }
    }

    for (int i = 1; i < lines.size(); ++i) {
        QByteArray line = lines[i].trimmed();
        int colon = line.indexOf(':');
        if (colon != -1) {
            QString key = QString::fromLatin1(line.left(colon)).trimmed().toLower();
            QString val = QString::fromUtf8(line.mid(colon + 1)).trimmed();
            req.headers.insert(key, val);
        }
    }

    return req;
}

QMap<QString, QString> MockServerWorker::parseQueryParams(const QString &query)
{
    QMap<QString, QString> map;
    QUrlQuery q(query);
    const auto items = q.queryItems(QUrl::FullyDecoded);
    for (const auto &item : items) {
        map.insert(item.first, item.second);
    }
    return map;
}

QMap<QString, QString> MockServerWorker::parseFormUrlEncoded(const QByteArray &data)
{
    QMap<QString, QString> map;
    QUrlQuery q(QString::fromUtf8(data));
    const auto items = q.queryItems(QUrl::FullyDecoded);
    for (const auto &item : items) {
        map.insert(item.first, item.second);
    }
    return map;
}

void MockServerWorker::sendResponse(QTcpSocket *socket, int statusCode, const QString &statusText,
                                     const QByteArray &body, const QString &contentType,
                                     const QMap<QString, QString> &extraHeaders)
{
    QByteArray res;
    res.append(QStringLiteral("HTTP/1.1 %1 %2\r\n").arg(QString::number(statusCode), statusText).toUtf8());
    res.append(QStringLiteral("Content-Type: %1\r\n").arg(contentType).toUtf8());
    res.append(QStringLiteral("Content-Length: %1\r\n").arg(body.size()).toUtf8());
    res.append("Connection: close\r\n");

    for (auto it = extraHeaders.constBegin(); it != extraHeaders.constEnd(); ++it) {
        res.append(QStringLiteral("%1: %2\r\n").arg(it.key(), it.value()).toUtf8());
    }
    res.append("\r\n");
    res.append(body);

    socket->write(res);
    socket->flush();
    socket->disconnectFromHost();
}

void MockServerWorker::handleRequest(QTcpSocket *socket, const HttpRequest &req)
{
    {
        QMutexLocker locker(&_mutex);
        ++_requestCount;
        _lastPath = req.path;
    }

    switch (_type) {
    case MockClientType::QBittorrent:
        handleQBittorrent(socket, req);
        break;
    case MockClientType::Transmission:
        handleTransmission(socket, req);
        break;
    case MockClientType::Aria2:
        handleAria2(socket, req);
        break;
    }
}

void MockServerWorker::handleQBittorrent(QTcpSocket *socket, const HttpRequest &req)
{
    QMutexLocker locker(&_mutex);

    if (req.path == QLatin1String("/api/v2/auth/login")) {
        auto form = parseFormUrlEncoded(req.body);
        if (_authSuccess && !form.value(QStringLiteral("password")).isEmpty()) {
            _sessionValid = true;
            QMap<QString, QString> headers;
            headers.insert(QStringLiteral("Set-Cookie"), QStringLiteral("SID=mock_sid_qbt_12345; HttpOnly; Path=/"));
            sendResponse(socket, 200, QStringLiteral("OK"), QByteArrayLiteral("Ok."), QStringLiteral("text/plain"), headers);
        } else {
            sendResponse(socket, 200, QStringLiteral("OK"), QByteArrayLiteral("Fails."));
        }
        return;
    }

    if (_requireAuth) {
        QString cookie = req.headers.value(QStringLiteral("cookie"));
        if (!_sessionValid || !cookie.contains(QLatin1String("SID=mock_sid_qbt_12345"))) {
            sendResponse(socket, 403, QStringLiteral("Forbidden"), QByteArrayLiteral("Forbidden"));
            return;
        }
    }

    if (req.path == QLatin1String("/api/v2/app/version")) {
        sendResponse(socket, 200, QStringLiteral("OK"), QByteArrayLiteral("v4.6.5"));
        return;
    }
    if (req.path == QLatin1String("/api/v2/app/webapiVersion")) {
        sendResponse(socket, 200, QStringLiteral("OK"), QByteArrayLiteral("2.9.3"));
        return;
    }
    if (req.path == QLatin1String("/api/v2/app/defaultSavePath")) {
        sendResponse(socket, 200, QStringLiteral("OK"), _defaultSavePath.toUtf8());
        return;
    }
    if (req.path == QLatin1String("/api/v2/app/preferences")) {
        QJsonObject pref;
        pref[QStringLiteral("add_stopped_enabled")] = true;
        pref[QStringLiteral("save_path")] = _defaultSavePath;
        sendResponse(socket, 200, QStringLiteral("OK"), QJsonDocument(pref).toJson(QJsonDocument::Compact), QStringLiteral("application/json"));
        return;
    }
    if (req.path == QLatin1String("/api/v2/torrents/categories")) {
        QJsonObject root;
        for (auto it = _categories.constBegin(); it != _categories.constEnd(); ++it) {
            QJsonObject cat;
            cat[QStringLiteral("name")] = it.key();
            cat[QStringLiteral("savePath")] = it.value();
            root[it.key()] = cat;
        }
        sendResponse(socket, 200, QStringLiteral("OK"), QJsonDocument(root).toJson(QJsonDocument::Compact), QStringLiteral("application/json"));
        return;
    }
    if (req.path == QLatin1String("/api/v2/torrents/tags")) {
        QJsonArray arr;
        for (const QString &t : _tags)
            arr.append(t);
        sendResponse(socket, 200, QStringLiteral("OK"), QJsonDocument(arr).toJson(QJsonDocument::Compact), QStringLiteral("application/json"));
        return;
    }
    if (req.path == QLatin1String("/api/v2/torrents/info")) {
        QString filterHashes = req.queryParams.value(QStringLiteral("hashes")).toLower();
        QStringList hashList = filterHashes.split(QLatin1Char('|'), Qt::SkipEmptyParts);

        QJsonArray arr;
        for (const auto &t : _torrents) {
            if (!hashList.isEmpty() && !hashList.contains(t.hash.toLower()))
                continue;
            QJsonObject obj;
            obj[QStringLiteral("hash")] = t.hash;
            obj[QStringLiteral("name")] = t.name;
            obj[QStringLiteral("state")] = t.state;
            obj[QStringLiteral("save_path")] = t.savePath;
            obj[QStringLiteral("category")] = t.category;
            obj[QStringLiteral("tags")] = t.tags;
            obj[QStringLiteral("force_start")] = t.forceStart;
            obj[QStringLiteral("seq_dl")] = t.seqDl;
            obj[QStringLiteral("f_l_piece_prio")] = t.flPiecePrio;
            obj[QStringLiteral("auto_tmm")] = t.autoTmm;
            obj[QStringLiteral("size")] = qint64(t.size);
            obj[QStringLiteral("total_size")] = qint64(t.totalSize);
            obj[QStringLiteral("progress")] = t.progress;
            arr.append(obj);
        }
        sendResponse(socket, 200, QStringLiteral("OK"), QJsonDocument(arr).toJson(QJsonDocument::Compact), QStringLiteral("application/json"));
        return;
    }
    if (req.path == QLatin1String("/api/v2/torrents/add")) {
        QString bodyStr = QString::fromUtf8(req.body);
        MockTorrentData t;
        t.state = QStringLiteral("downloading");
        t.savePath = _defaultSavePath;

        QRegularExpression rxUrls(QStringLiteral("name=\"urls\"\\r?\\n\\r?\\n([^\\r\\n]+)"));
        auto match = rxUrls.match(bodyStr);
        if (match.hasMatch()) {
            QString magnet = match.captured(1).trimmed();
            try {
                MagnetLink parsed = MagnetLink::parse(magnet);
                t.hash = parsed.hash.toLower();
                t.name = parsed.prettyName();
                t.trackers = parsed.trackers;
            } catch (...) {
                t.hash = QStringLiteral("c12fe1c06bba254a9dc9f519b335aa7c1367a88a");
                t.name = QStringLiteral("Mock Torrent");
            }
        }
        if (bodyStr.contains(QStringLiteral("name=\"stopped\"")) || bodyStr.contains(QStringLiteral("name=\"stopCondition\""))) {
            t.state = QStringLiteral("pausedDL");
        }
        QRegularExpression rxCategory(QStringLiteral("name=\"category\"\\r?\\n\\r?\\n([^\\r\\n]+)"));
        match = rxCategory.match(bodyStr);
        if (match.hasMatch()) t.category = match.captured(1).trimmed();

        QRegularExpression rxTags(QStringLiteral("name=\"tags\"\\r?\\n\\r?\\n([^\\r\\n]+)"));
        match = rxTags.match(bodyStr);
        if (match.hasMatch()) t.tags = match.captured(1).trimmed();

        QRegularExpression rxSavepath(QStringLiteral("name=\"savepath\"\\r?\\n\\r?\\n([^\\r\\n]+)"));
        match = rxSavepath.match(bodyStr);
        if (match.hasMatch()) t.savePath = match.captured(1).trimmed();

        MockTorrentFile f1{0, t.name + QStringLiteral("/video.mkv"), 1048576000, 1, 0.0};
        MockTorrentFile f2{1, t.name + QStringLiteral("/readme.txt"), 1024, 1, 0.0};
        t.files = {f1, f2};
        t.size = f1.size + f2.size;
        t.totalSize = t.size;

        _torrents.insert(t.hash.toLower(), t);
        sendResponse(socket, 200, QStringLiteral("OK"), QByteArrayLiteral("Ok."));
        return;
    }
    if (req.path == QLatin1String("/api/v2/torrents/files")) {
        QString hash = req.queryParams.value(QStringLiteral("hash")).toLower();
        QJsonArray arr;
        if (_torrents.contains(hash)) {
            for (const auto &f : _torrents[hash].files) {
                QJsonObject fo;
                fo[QStringLiteral("index")] = f.index;
                fo[QStringLiteral("name")] = f.name;
                fo[QStringLiteral("size")] = qint64(f.size);
                fo[QStringLiteral("priority")] = f.priority;
                fo[QStringLiteral("progress")] = f.progress;
                arr.append(fo);
            }
        }
        sendResponse(socket, 200, QStringLiteral("OK"), QJsonDocument(arr).toJson(QJsonDocument::Compact), QStringLiteral("application/json"));
        return;
    }
    if (req.path == QLatin1String("/api/v2/torrents/filePrio")) {
        auto form = parseFormUrlEncoded(req.body);
        QString hash = form.value(QStringLiteral("hash")).toLower();
        int priority = form.value(QStringLiteral("priority")).toInt();
        QStringList ids = form.value(QStringLiteral("id")).split(QLatin1Char('|'), Qt::SkipEmptyParts);
        if (_torrents.contains(hash)) {
            for (const QString &idStr : ids) {
                int id = idStr.toInt();
                for (auto &f : _torrents[hash].files) {
                    if (f.index == id) f.priority = priority;
                }
            }
        }
        sendResponse(socket, 200, QStringLiteral("OK"), QByteArrayLiteral("Ok."));
        return;
    }
    if (req.path == QLatin1String("/api/v2/torrents/start") || req.path == QLatin1String("/api/v2/torrents/resume")) {
        auto form = parseFormUrlEncoded(req.body);
        QString hash = form.value(QStringLiteral("hashes")).toLower();
        if (_torrents.contains(hash)) _torrents[hash].state = QStringLiteral("downloading");
        sendResponse(socket, 200, QStringLiteral("OK"), QByteArrayLiteral("Ok."));
        return;
    }
    if (req.path == QLatin1String("/api/v2/torrents/stop") || req.path == QLatin1String("/api/v2/torrents/pause")) {
        auto form = parseFormUrlEncoded(req.body);
        QString hash = form.value(QStringLiteral("hashes")).toLower();
        if (_torrents.contains(hash)) _torrents[hash].state = QStringLiteral("pausedDL");
        sendResponse(socket, 200, QStringLiteral("OK"), QByteArrayLiteral("Ok."));
        return;
    }
    if (req.path == QLatin1String("/api/v2/torrents/setForceStart")) {
        auto form = parseFormUrlEncoded(req.body);
        QString hash = form.value(QStringLiteral("hashes")).toLower();
        bool val = (form.value(QStringLiteral("value")) == QLatin1String("true"));
        if (_torrents.contains(hash)) {
            _torrents[hash].forceStart = val;
            _torrents[hash].state = val ? QStringLiteral("forcedDL") : QStringLiteral("downloading");
        }
        sendResponse(socket, 200, QStringLiteral("OK"), QByteArrayLiteral("Ok."));
        return;
    }
    if (req.path == QLatin1String("/api/v2/torrents/setLocation")) {
        auto form = parseFormUrlEncoded(req.body);
        QString hash = form.value(QStringLiteral("hashes")).toLower();
        if (_torrents.contains(hash)) _torrents[hash].savePath = form.value(QStringLiteral("location"));
        sendResponse(socket, 200, QStringLiteral("OK"), QByteArrayLiteral("Ok."));
        return;
    }
    if (req.path == QLatin1String("/api/v2/torrents/setCategory")) {
        auto form = parseFormUrlEncoded(req.body);
        QString hash = form.value(QStringLiteral("hashes")).toLower();
        if (_torrents.contains(hash)) _torrents[hash].category = form.value(QStringLiteral("category"));
        sendResponse(socket, 200, QStringLiteral("OK"), QByteArrayLiteral("Ok."));
        return;
    }
    if (req.path == QLatin1String("/api/v2/torrents/addTags")) {
        auto form = parseFormUrlEncoded(req.body);
        QString hash = form.value(QStringLiteral("hashes")).toLower();
        if (_torrents.contains(hash)) _torrents[hash].tags = form.value(QStringLiteral("tags"));
        sendResponse(socket, 200, QStringLiteral("OK"), QByteArrayLiteral("Ok."));
        return;
    }
    if (req.path == QLatin1String("/api/v2/torrents/addTrackers")) {
        auto form = parseFormUrlEncoded(req.body);
        QString hash = form.value(QStringLiteral("hash")).toLower();
        if (_torrents.contains(hash)) {
            QStringList trs = form.value(QStringLiteral("urls")).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            _torrents[hash].trackers.append(trs);
        }
        sendResponse(socket, 200, QStringLiteral("OK"), QByteArrayLiteral("Ok."));
        return;
    }
    if (req.path == QLatin1String("/api/v2/torrents/rename")) {
        auto form = parseFormUrlEncoded(req.body);
        QString hash = form.value(QStringLiteral("hash")).toLower();
        if (_torrents.contains(hash)) _torrents[hash].name = form.value(QStringLiteral("name"));
        sendResponse(socket, 200, QStringLiteral("OK"), QByteArrayLiteral("Ok."));
        return;
    }
    if (req.path == QLatin1String("/api/v2/torrents/setAutoManagement")) {
        auto form = parseFormUrlEncoded(req.body);
        QString hash = form.value(QStringLiteral("hashes")).toLower();
        if (_torrents.contains(hash)) _torrents[hash].autoTmm = (form.value(QStringLiteral("enable")) == QLatin1String("true"));
        sendResponse(socket, 200, QStringLiteral("OK"), QByteArrayLiteral("Ok."));
        return;
    }
    if (req.path == QLatin1String("/api/v2/torrents/setDownloadLimit")) {
        auto form = parseFormUrlEncoded(req.body);
        QString hash = form.value(QStringLiteral("hashes")).toLower();
        if (_torrents.contains(hash)) _torrents[hash].downloadLimit = form.value(QStringLiteral("limit")).toLongLong();
        sendResponse(socket, 200, QStringLiteral("OK"), QByteArrayLiteral("Ok."));
        return;
    }
    if (req.path == QLatin1String("/api/v2/torrents/setUploadLimit")) {
        auto form = parseFormUrlEncoded(req.body);
        QString hash = form.value(QStringLiteral("hashes")).toLower();
        if (_torrents.contains(hash)) _torrents[hash].uploadLimit = form.value(QStringLiteral("limit")).toLongLong();
        sendResponse(socket, 200, QStringLiteral("OK"), QByteArrayLiteral("Ok."));
        return;
    }
    if (req.path == QLatin1String("/api/v2/torrents/toggleSequentialDownload")) {
        auto form = parseFormUrlEncoded(req.body);
        QString hash = form.value(QStringLiteral("hashes")).toLower();
        if (_torrents.contains(hash)) _torrents[hash].seqDl = !_torrents[hash].seqDl;
        sendResponse(socket, 200, QStringLiteral("OK"), QByteArrayLiteral("Ok."));
        return;
    }
    if (req.path == QLatin1String("/api/v2/torrents/toggleFirstLastPiecePrio")) {
        auto form = parseFormUrlEncoded(req.body);
        QString hash = form.value(QStringLiteral("hashes")).toLower();
        if (_torrents.contains(hash)) _torrents[hash].flPiecePrio = !_torrents[hash].flPiecePrio;
        sendResponse(socket, 200, QStringLiteral("OK"), QByteArrayLiteral("Ok."));
        return;
    }
    if (req.path == QLatin1String("/api/v2/torrents/delete")) {
        auto form = parseFormUrlEncoded(req.body);
        QString hash = form.value(QStringLiteral("hashes")).toLower();
        _torrents.remove(hash);
        sendResponse(socket, 200, QStringLiteral("OK"), QByteArrayLiteral("Ok."));
        return;
    }

    sendResponse(socket, 404, QStringLiteral("Not Found"), QByteArrayLiteral("404 Not Found"));
}

void MockServerWorker::handleTransmission(QTcpSocket *socket, const HttpRequest &req)
{
    QMutexLocker locker(&_mutex);

    if (req.path != QLatin1String("/transmission/rpc")) {
        sendResponse(socket, 404, QStringLiteral("Not Found"), QByteArrayLiteral("Not Found"));
        return;
    }

    QString sessHeader = req.headers.value(QStringLiteral("x-transmission-session-id"));
    if (sessHeader != _transmissionSessionId) {
        QMap<QString, QString> h;
        h.insert(QStringLiteral("X-Transmission-Session-Id"), _transmissionSessionId);
        sendResponse(socket, 409, QStringLiteral("Conflict"),
                     QByteArrayLiteral("<h1>409: Conflict</h1><p>Compulsory X-Transmission-Session-Id header missing or incorrect.</p>"),
                     QStringLiteral("text/html"), h);
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(req.body);
    QJsonObject reqObj = doc.object();
    QString method = reqObj.value(QStringLiteral("method")).toString();
    QJsonObject args = reqObj.value(QStringLiteral("arguments")).toObject();
    int tag = reqObj.value(QStringLiteral("tag")).toInt();

    QJsonObject resObj;
    resObj[QStringLiteral("result")] = QStringLiteral("success");
    if (tag > 0) resObj[QStringLiteral("tag")] = tag;
    QJsonObject resArgs;

    if (method == QLatin1String("session-get")) {
        resArgs[QStringLiteral("version")] = QStringLiteral("4.0.5");
        resArgs[QStringLiteral("rpc-version")] = 17;
        resArgs[QStringLiteral("download-dir")] = _defaultSavePath;
        resArgs[QStringLiteral("start-added-torrents")] = false;
        resObj[QStringLiteral("arguments")] = resArgs;
        sendResponse(socket, 200, QStringLiteral("OK"), QJsonDocument(resObj).toJson(QJsonDocument::Compact), QStringLiteral("application/json"));
        return;
    }

    if (method == QLatin1String("torrent-get")) {
        QJsonArray torArr;
        for (const auto &t : _torrents) {
            QJsonObject to;
            to[QStringLiteral("id")] = t.transmissionId;
            to[QStringLiteral("hashString")] = t.hash;
            to[QStringLiteral("name")] = t.name;
            int status = (t.state == QLatin1String("downloading") || t.state == QLatin1String("forcedDL")) ? 4 : 0;
            to[QStringLiteral("status")] = status;
            to[QStringLiteral("downloadDir")] = t.savePath;
            QJsonArray labelsArr;
            for (const QString &lbl : t.tags.split(QLatin1Char(','), Qt::SkipEmptyParts))
                labelsArr.append(lbl.trimmed());
            to[QStringLiteral("labels")] = labelsArr;
            to[QStringLiteral("totalSize")] = qint64(t.totalSize);
            to[QStringLiteral("percentDone")] = t.progress;

            QJsonArray filesArr, statsArr;
            for (const auto &f : t.files) {
                QJsonObject fo, so;
                fo[QStringLiteral("name")] = f.name;
                fo[QStringLiteral("length")] = qint64(f.size);
                fo[QStringLiteral("bytesCompleted")] = qint64(f.progress * f.size);
                filesArr.append(fo);

                so[QStringLiteral("wanted")] = (f.priority > 0);
                so[QStringLiteral("priority")] = f.priority >= 6 ? 1 : 0;
                statsArr.append(so);
            }
            to[QStringLiteral("files")] = filesArr;
            to[QStringLiteral("fileStats")] = statsArr;
            torArr.append(to);
        }
        resArgs[QStringLiteral("torrents")] = torArr;
        resObj[QStringLiteral("arguments")] = resArgs;
        sendResponse(socket, 200, QStringLiteral("OK"), QJsonDocument(resObj).toJson(QJsonDocument::Compact), QStringLiteral("application/json"));
        return;
    }

    if (method == QLatin1String("torrent-add")) {
        QString magnet = args.value(QStringLiteral("filename")).toString();
        MockTorrentData t;
        try {
            MagnetLink parsed = MagnetLink::parse(magnet);
            t.hash = parsed.hash.toLower();
            t.name = parsed.prettyName();
            t.trackers = parsed.trackers;
        } catch (...) {
            t.hash = QStringLiteral("c12fe1c06bba254a9dc9f519b335aa7c1367a88a");
            t.name = QStringLiteral("Transmission Mock");
        }
        t.transmissionId = ++_idCounter;
        t.state = args.value(QStringLiteral("paused")).toBool(false) ? QStringLiteral("pausedDL") : QStringLiteral("downloading");
        t.savePath = args.value(QStringLiteral("download-dir")).toString(_defaultSavePath);

        QJsonArray labels = args.value(QStringLiteral("labels")).toArray();
        QStringList lblList;
        for (const auto &l : labels) lblList.append(l.toString());
        t.tags = lblList.join(QStringLiteral(", "));

        MockTorrentFile f1{0, t.name + QStringLiteral("/content.iso"), 104857600, 1, 0.0};
        t.files = {f1};
        t.size = f1.size;
        t.totalSize = f1.size;

        _torrents.insert(t.hash.toLower(), t);

        QJsonObject addedObj;
        addedObj[QStringLiteral("id")] = t.transmissionId;
        addedObj[QStringLiteral("hashString")] = t.hash;
        addedObj[QStringLiteral("name")] = t.name;
        resArgs[QStringLiteral("torrent-added")] = addedObj;
        resObj[QStringLiteral("arguments")] = resArgs;
        sendResponse(socket, 200, QStringLiteral("OK"), QJsonDocument(resObj).toJson(QJsonDocument::Compact), QStringLiteral("application/json"));
        return;
    }

    if (method == QLatin1String("torrent-set")) {
        QJsonArray ids = args.value(QStringLiteral("ids")).toArray();
        for (const auto &idVal : ids) {
            QString hashOrId = idVal.toString().toLower();
            for (auto &t : _torrents) {
                if (t.hash == hashOrId || QString::number(t.transmissionId) == hashOrId) {
                    if (args.contains(QStringLiteral("files-unwanted"))) {
                        for (const auto &unw : args.value(QStringLiteral("files-unwanted")).toArray()) {
                            int idx = unw.toInt();
                            if (idx >= 0 && idx < t.files.size()) t.files[idx].priority = 0;
                        }
                    }
                    if (args.contains(QStringLiteral("files-wanted"))) {
                        for (const auto &w : args.value(QStringLiteral("files-wanted")).toArray()) {
                            int idx = w.toInt();
                            if (idx >= 0 && idx < t.files.size()) t.files[idx].priority = 1;
                        }
                    }
                    if (args.contains(QStringLiteral("priority-high"))) {
                        for (const auto &h : args.value(QStringLiteral("priority-high")).toArray()) {
                            int idx = h.toInt();
                            if (idx >= 0 && idx < t.files.size()) t.files[idx].priority = 6;
                        }
                    }
                    if (args.contains(QStringLiteral("labels"))) {
                        QStringList lbls;
                        for (const auto &l : args.value(QStringLiteral("labels")).toArray())
                            lbls.append(l.toString());
                        t.tags = lbls.join(QStringLiteral(", "));
                    }
                    if (args.contains(QStringLiteral("downloadLimit")))
                        t.downloadLimit = qint64(args.value(QStringLiteral("downloadLimit")).toDouble() * 1024);
                    if (args.contains(QStringLiteral("uploadLimit")))
                        t.uploadLimit = qint64(args.value(QStringLiteral("uploadLimit")).toDouble() * 1024);
                    if (args.contains(QStringLiteral("sequentialDownload")))
                        t.seqDl = args.value(QStringLiteral("sequentialDownload")).toBool();
                }
            }
        }
        resObj[QStringLiteral("arguments")] = resArgs;
        sendResponse(socket, 200, QStringLiteral("OK"), QJsonDocument(resObj).toJson(QJsonDocument::Compact), QStringLiteral("application/json"));
        return;
    }

    if (method == QLatin1String("torrent-start") || method == QLatin1String("torrent-start-now")) {
        QJsonArray ids = args.value(QStringLiteral("ids")).toArray();
        for (const auto &idVal : ids) {
            QString hashOrId = idVal.toString().toLower();
            for (auto &t : _torrents) {
                if (t.hash == hashOrId || QString::number(t.transmissionId) == hashOrId) {
                    t.state = QStringLiteral("downloading");
                }
            }
        }
        resObj[QStringLiteral("arguments")] = resArgs;
        sendResponse(socket, 200, QStringLiteral("OK"), QJsonDocument(resObj).toJson(QJsonDocument::Compact), QStringLiteral("application/json"));
        return;
    }

    if (method == QLatin1String("torrent-stop")) {
        QJsonArray ids = args.value(QStringLiteral("ids")).toArray();
        for (const auto &idVal : ids) {
            QString hashOrId = idVal.toString().toLower();
            for (auto &t : _torrents) {
                if (t.hash == hashOrId || QString::number(t.transmissionId) == hashOrId) {
                    t.state = QStringLiteral("pausedDL");
                }
            }
        }
        resObj[QStringLiteral("arguments")] = resArgs;
        sendResponse(socket, 200, QStringLiteral("OK"), QJsonDocument(resObj).toJson(QJsonDocument::Compact), QStringLiteral("application/json"));
        return;
    }

    if (method == QLatin1String("torrent-set-location")) {
        QJsonArray ids = args.value(QStringLiteral("ids")).toArray();
        QString loc = args.value(QStringLiteral("location")).toString();
        for (const auto &idVal : ids) {
            QString hashOrId = idVal.toString().toLower();
            for (auto &t : _torrents) {
                if (t.hash == hashOrId || QString::number(t.transmissionId) == hashOrId) {
                    t.savePath = loc;
                }
            }
        }
        resObj[QStringLiteral("arguments")] = resArgs;
        sendResponse(socket, 200, QStringLiteral("OK"), QJsonDocument(resObj).toJson(QJsonDocument::Compact), QStringLiteral("application/json"));
        return;
    }

    if (method == QLatin1String("torrent-rename-path")) {
        QString name = args.value(QStringLiteral("name")).toString();
        QJsonArray ids = args.value(QStringLiteral("ids")).toArray();
        for (const auto &idVal : ids) {
            QString hashOrId = idVal.toString().toLower();
            for (auto &t : _torrents) {
                if (t.hash == hashOrId || QString::number(t.transmissionId) == hashOrId) {
                    t.name = name;
                }
            }
        }
        resObj[QStringLiteral("arguments")] = resArgs;
        sendResponse(socket, 200, QStringLiteral("OK"), QJsonDocument(resObj).toJson(QJsonDocument::Compact), QStringLiteral("application/json"));
        return;
    }

    if (method == QLatin1String("torrent-remove")) {
        QJsonArray ids = args.value(QStringLiteral("ids")).toArray();
        for (const auto &idVal : ids) {
            QString hashOrId = idVal.toString().toLower();
            QString hashToRemove;
            for (auto &t : _torrents) {
                if (t.hash == hashOrId || QString::number(t.transmissionId) == hashOrId) {
                    hashToRemove = t.hash;
                    break;
                }
            }
            if (!hashToRemove.isEmpty()) _torrents.remove(hashToRemove);
        }
        resObj[QStringLiteral("arguments")] = resArgs;
        sendResponse(socket, 200, QStringLiteral("OK"), QJsonDocument(resObj).toJson(QJsonDocument::Compact), QStringLiteral("application/json"));
        return;
    }

    resObj[QStringLiteral("arguments")] = resArgs;
    sendResponse(socket, 200, QStringLiteral("OK"), QJsonDocument(resObj).toJson(QJsonDocument::Compact), QStringLiteral("application/json"));
}

void MockServerWorker::handleAria2(QTcpSocket *socket, const HttpRequest &req)
{
    QMutexLocker locker(&_mutex);

    if (req.path != QLatin1String("/jsonrpc")) {
        sendResponse(socket, 404, QStringLiteral("Not Found"), QByteArrayLiteral("Not Found"));
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(req.body);
    QJsonObject reqObj = doc.object();
    QString reqId = reqObj.value(QStringLiteral("id")).toString();
    QString method = reqObj.value(QStringLiteral("method")).toString();
    QJsonArray fullParams = reqObj.value(QStringLiteral("params")).toArray();

    QJsonArray params;
    int startIdx = 0;
    if (!fullParams.isEmpty() && fullParams.first().toString().startsWith(QLatin1String("token:"))) {
        startIdx = 1;
    }
    for (int i = startIdx; i < fullParams.size(); ++i) {
        params.append(fullParams[i]);
    }

    QJsonObject resObj;
    resObj[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    resObj[QStringLiteral("id")] = reqId;

    if (method == QLatin1String("aria2.getVersion")) {
        QJsonObject res;
        res[QStringLiteral("version")] = QStringLiteral("1.36.0");
        QJsonArray feat; feat.append(QStringLiteral("BitTorrent"));
        res[QStringLiteral("enabledFeatures")] = feat;
        resObj[QStringLiteral("result")] = res;
        sendResponse(socket, 200, QStringLiteral("OK"), QJsonDocument(resObj).toJson(QJsonDocument::Compact), QStringLiteral("application/json"));
        return;
    }

    if (method == QLatin1String("aria2.getGlobalOption")) {
        QJsonObject res;
        res[QStringLiteral("dir")] = _defaultSavePath;
        res[QStringLiteral("pause")] = QStringLiteral("false");
        resObj[QStringLiteral("result")] = res;
        sendResponse(socket, 200, QStringLiteral("OK"), QJsonDocument(resObj).toJson(QJsonDocument::Compact), QStringLiteral("application/json"));
        return;
    }

    if (method == QLatin1String("aria2.addUri")) {
        QJsonArray uris = !params.isEmpty() ? params.at(0).toArray() : QJsonArray();
        QString magnet = !uris.isEmpty() ? uris.at(0).toString() : QString();
        QJsonObject opt = params.size() > 1 ? params.at(1).toObject() : QJsonObject();

        MockTorrentData t;
        try {
            MagnetLink parsed = MagnetLink::parse(magnet);
            t.hash = parsed.hash.toLower();
            t.name = parsed.prettyName();
            t.trackers = parsed.trackers;
        } catch (...) {
            t.hash = QStringLiteral("c12fe1c06bba254a9dc9f519b335aa7c1367a88a");
            t.name = QStringLiteral("Aria2 Mock");
        }
        t.gid = QString::number(++_idCounter, 16);
        t.savePath = opt.value(QStringLiteral("dir")).toString(_defaultSavePath);
        t.state = (opt.value(QStringLiteral("pause")).toString() == QLatin1String("true")) ? QStringLiteral("pausedDL") : QStringLiteral("downloading");

        MockTorrentFile f1{0, t.savePath + QStringLiteral("/") + t.name + QStringLiteral(".bin"), 52428800, 1, 0.0};
        t.files = {f1};
        t.size = f1.size;
        t.totalSize = f1.size;

        _torrents.insert(t.hash.toLower(), t);
        resObj[QStringLiteral("result")] = t.gid;
        sendResponse(socket, 200, QStringLiteral("OK"), QJsonDocument(resObj).toJson(QJsonDocument::Compact), QStringLiteral("application/json"));
        return;
    }

    if (method == QLatin1String("aria2.tellStatus")) {
        QString gid = !params.isEmpty() ? params.at(0).toString() : QString();
        for (const auto &t : _torrents) {
            if (t.gid == gid || t.hash == gid.toLower()) {
                QJsonObject st;
                st[QStringLiteral("gid")] = t.gid;
                st[QStringLiteral("status")] = (t.state == QLatin1String("pausedDL") || t.state == QLatin1String("stoppedDL")) ? QStringLiteral("paused") : QStringLiteral("active");
                st[QStringLiteral("totalLength")] = QString::number(t.totalSize);
                st[QStringLiteral("completedLength")] = QString::number(qint64(t.progress * t.totalSize));
                st[QStringLiteral("dir")] = t.savePath;
                st[QStringLiteral("infoHash")] = t.hash;

                QJsonObject bt;
                bt[QStringLiteral("infoHash")] = t.hash;
                QJsonObject btInfo;
                btInfo[QStringLiteral("name")] = t.name;
                bt[QStringLiteral("info")] = btInfo;
                st[QStringLiteral("bittorrent")] = bt;

                QJsonArray filesArr;
                for (const auto &f : t.files) {
                    QJsonObject fo;
                    fo[QStringLiteral("index")] = QString::number(f.index + 1);
                    fo[QStringLiteral("path")] = f.name;
                    fo[QStringLiteral("length")] = QString::number(f.size);
                    fo[QStringLiteral("completedLength")] = QString::number(qint64(f.progress * f.size));
                    fo[QStringLiteral("selected")] = f.priority > 0 ? QStringLiteral("true") : QStringLiteral("false");
                    filesArr.append(fo);
                }
                st[QStringLiteral("files")] = filesArr;
                resObj[QStringLiteral("result")] = st;
                sendResponse(socket, 200, QStringLiteral("OK"), QJsonDocument(resObj).toJson(QJsonDocument::Compact), QStringLiteral("application/json"));
                return;
            }
        }
        resObj[QStringLiteral("result")] = QJsonObject();
        sendResponse(socket, 200, QStringLiteral("OK"), QJsonDocument(resObj).toJson(QJsonDocument::Compact), QStringLiteral("application/json"));
        return;
    }

    if (method == QLatin1String("aria2.tellActive") || method == QLatin1String("aria2.tellWaiting") || method == QLatin1String("aria2.tellStopped")) {
        QJsonArray arr;
        for (const auto &t : _torrents) {
            bool matches = false;
            bool isPaused = (t.state == QLatin1String("pausedDL") || t.state == QLatin1String("stoppedDL"));
            if (method == QLatin1String("aria2.tellActive") && !isPaused) matches = true;
            if (method == QLatin1String("aria2.tellWaiting") && isPaused) matches = true;
            if (method == QLatin1String("aria2.tellStopped") && isPaused) matches = true;

            if (matches) {
                QJsonObject st;
                st[QStringLiteral("gid")] = t.gid;
                st[QStringLiteral("status")] = isPaused ? QStringLiteral("paused") : QStringLiteral("active");
                st[QStringLiteral("totalLength")] = QString::number(t.totalSize);
                st[QStringLiteral("completedLength")] = QString::number(qint64(t.progress * t.totalSize));
                st[QStringLiteral("dir")] = t.savePath;
                st[QStringLiteral("infoHash")] = t.hash;

                QJsonObject bt;
                bt[QStringLiteral("infoHash")] = t.hash;
                QJsonObject btInfo;
                btInfo[QStringLiteral("name")] = t.name;
                bt[QStringLiteral("info")] = btInfo;
                st[QStringLiteral("bittorrent")] = bt;
                arr.append(st);
            }
        }
        resObj[QStringLiteral("result")] = arr;
        sendResponse(socket, 200, QStringLiteral("OK"), QJsonDocument(resObj).toJson(QJsonDocument::Compact), QStringLiteral("application/json"));
        return;
    }

    if (method == QLatin1String("aria2.changeOption")) {
        QString gid = !params.isEmpty() ? params.at(0).toString() : QString();
        QJsonObject opt = params.size() > 1 ? params.at(1).toObject() : QJsonObject();
        for (auto &t : _torrents) {
            if (t.gid == gid || t.hash == gid.toLower()) {
                if (opt.contains(QStringLiteral("dir"))) t.savePath = opt.value(QStringLiteral("dir")).toString();
                if (opt.contains(QStringLiteral("max-download-limit"))) t.downloadLimit = opt.value(QStringLiteral("max-download-limit")).toString().toLongLong();
                if (opt.contains(QStringLiteral("max-upload-limit"))) t.uploadLimit = opt.value(QStringLiteral("max-upload-limit")).toString().toLongLong();
                if (opt.contains(QStringLiteral("select-file"))) {
                    QStringList selList = opt.value(QStringLiteral("select-file")).toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
                    QSet<int> selSet;
                    for (const auto &s : selList) selSet.insert(s.toInt() - 1);
                    for (auto &f : t.files) {
                        f.priority = selSet.contains(f.index) ? 1 : 0;
                    }
                }
            }
        }
        resObj[QStringLiteral("result")] = QStringLiteral("OK");
        sendResponse(socket, 200, QStringLiteral("OK"), QJsonDocument(resObj).toJson(QJsonDocument::Compact), QStringLiteral("application/json"));
        return;
    }

    if (method == QLatin1String("aria2.pause") || method == QLatin1String("aria2.forcePause")) {
        QString gid = !params.isEmpty() ? params.at(0).toString() : QString();
        for (auto &t : _torrents) {
            if (t.gid == gid || t.hash == gid.toLower()) t.state = QStringLiteral("pausedDL");
        }
        resObj[QStringLiteral("result")] = gid;
        sendResponse(socket, 200, QStringLiteral("OK"), QJsonDocument(resObj).toJson(QJsonDocument::Compact), QStringLiteral("application/json"));
        return;
    }

    if (method == QLatin1String("aria2.unpause") || method == QLatin1String("aria2.forceUnpause")) {
        QString gid = !params.isEmpty() ? params.at(0).toString() : QString();
        for (auto &t : _torrents) {
            if (t.gid == gid || t.hash == gid.toLower()) t.state = QStringLiteral("downloading");
        }
        resObj[QStringLiteral("result")] = gid;
        sendResponse(socket, 200, QStringLiteral("OK"), QJsonDocument(resObj).toJson(QJsonDocument::Compact), QStringLiteral("application/json"));
        return;
    }

    if (method == QLatin1String("aria2.remove") || method == QLatin1String("aria2.forceRemove")) {
        QString gid = !params.isEmpty() ? params.at(0).toString() : QString();
        QString hashToRemove;
        for (const auto &t : _torrents) {
            if (t.gid == gid || t.hash == gid.toLower()) {
                hashToRemove = t.hash;
                break;
            }
        }
        if (!hashToRemove.isEmpty()) _torrents.remove(hashToRemove);
        resObj[QStringLiteral("result")] = gid;
        sendResponse(socket, 200, QStringLiteral("OK"), QJsonDocument(resObj).toJson(QJsonDocument::Compact), QStringLiteral("application/json"));
        return;
    }

    resObj[QStringLiteral("result")] = QStringLiteral("OK");
    sendResponse(socket, 200, QStringLiteral("OK"), QJsonDocument(resObj).toJson(QJsonDocument::Compact), QStringLiteral("application/json"));
}
