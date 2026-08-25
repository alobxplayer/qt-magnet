#pragma once

#include <QObject>
#include <QThread>
#include <QTcpServer>
#include <QTcpSocket>
#include <QMutex>
#include <QMap>
#include <QVector>
#include <QStringList>
#include <QJsonObject>
#include <QJsonArray>
#include <QHostAddress>

enum class MockClientType {
    QBittorrent,
    Transmission,
    Aria2
};

struct MockTorrentFile {
    int index = 0;
    QString name;
    qint64 size = 1048576;
    int priority = 1;
    double progress = 0.0;
};

struct MockTorrentData {
    QString hash;
    QString name;
    QString state;
    QString savePath;
    QString category;
    QString tags;
    bool forceStart = false;
    bool seqDl = false;
    bool flPiecePrio = false;
    bool autoTmm = false;
    qint64 size = 10485760;
    qint64 totalSize = 10485760;
    double progress = 0.0;
    QStringList trackers;
    qint64 downloadLimit = 0;
    qint64 uploadLimit = 0;
    QVector<MockTorrentFile> files;
    QString gid;
    int transmissionId = 1;
};

struct HttpRequest {
    QString method;
    QString path;
    QString queryString;
    QMap<QString, QString> queryParams;
    QMap<QString, QString> headers;
    QByteArray body;
};

class MockServerWorker;

class MockTorrentServer : public QObject {
    Q_OBJECT
public:
    explicit MockTorrentServer(MockClientType type, QObject *parent = nullptr);
    ~MockTorrentServer() override;

    bool start(quint16 port = 0);
    void stop();

    quint16 port() const;
    QString host() const { return QStringLiteral("127.0.0.1"); }
    MockClientType clientType() const { return _type; }

    void reset();
    void addPreloadedTorrent(const MockTorrentData &t);
    int requestCount() const;
    QString lastPath() const;
    QMap<QString, MockTorrentData> torrents() const;

    void setRequireAuth(bool req);
    void setAuthSuccess(bool ok);
    void setSessionValid(bool v);

private:
    MockClientType _type;
    QThread _thread;
    MockServerWorker *_worker = nullptr;
};

class MockServerWorker : public QObject {
    Q_OBJECT
public:
    explicit MockServerWorker(MockClientType type);
    ~MockServerWorker() override;

    void reset();
    void addPreloadedTorrent(const MockTorrentData &t);
    int requestCount() const;
    QString lastPath() const;
    QMap<QString, MockTorrentData> torrents() const;
    quint16 port() const { return _port; }

    void setRequireAuth(bool req) { QMutexLocker l(&_mutex); _requireAuth = req; }
    void setAuthSuccess(bool ok) { QMutexLocker l(&_mutex); _authSuccess = ok; }
    void setSessionValid(bool v) { QMutexLocker l(&_mutex); _sessionValid = v; }

public slots:
    bool startListening(quint16 port);
    void stopListening();

private slots:
    void onNewConnection();
    void onReadyRead();
    void onSocketDisconnected();

private:
    void handleRequest(QTcpSocket *socket, const HttpRequest &req);
    void handleQBittorrent(QTcpSocket *socket, const HttpRequest &req);
    void handleTransmission(QTcpSocket *socket, const HttpRequest &req);
    void handleAria2(QTcpSocket *socket, const HttpRequest &req);

    void sendResponse(QTcpSocket *socket, int statusCode, const QString &statusText,
                      const QByteArray &body, const QString &contentType = QStringLiteral("text/plain"),
                      const QMap<QString, QString> &extraHeaders = {});

    static HttpRequest parseHttpRequest(const QByteArray &data);
    static QMap<QString, QString> parseQueryParams(const QString &query);
    static QMap<QString, QString> parseFormUrlEncoded(const QByteArray &data);

    MockClientType _type;
    QTcpServer *_server = nullptr;
    quint16 _port = 0;
    mutable QMutex _mutex;

    QMap<QTcpSocket *, QByteArray> _socketBuffers;
    int _requestCount = 0;
    QString _lastPath;
    bool _requireAuth = true;
    bool _authSuccess = true;
    bool _sessionValid = true;

    QString _transmissionSessionId = QStringLiteral("mock-tr-session-42");
    QMap<QString, MockTorrentData> _torrents;
    QMap<QString, QString> _categories;
    QStringList _tags;
    QString _defaultSavePath = QStringLiteral("C:/Downloads");
    int _idCounter = 1;
};
