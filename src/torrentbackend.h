#pragma once

#include "torrentclient.h"

#include <functional>

class Config;
class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;

// Transport and configuration shared by every backend: connection settings,
// logging, and the network manager with its cookie jar.
class ClientTransport {
public:
    ClientTransport(const Config &cfg, std::function<void(const QString &)> log,
              std::function<bool()> isCancelled);
    ~ClientTransport();

    void trace(const QString &s) const;
    bool isCancelled() const { return _isCancelled ? _isCancelled() : false; }

    struct RawResponse {
        int statusCode = 0;
        QByteArray body;
        int networkError = 0;
        QString errorString;
        bool timedOut = false;
        QList<QPair<QByteArray, QByteArray>> rawHeaders;

        QByteArray header(const QByteArray &name) const {
            for (const auto &h : rawHeaders) {
                if (h.first.compare(name, Qt::CaseInsensitive) == 0)
                    return h.second;
            }
            return QByteArray();
        }
    };
    RawResponse execRaw(const std::function<QNetworkReply *()> &make);

    // Parses a JSON body, throwing ClientException on a parse error.
    QJsonDocument parseJson(const QByteArray &body, const QString &source,
                            bool requireObject = false) const;
    // Throws the shared "No connection" error for a failed raw response.
    void throwIfNoConnection(const RawResponse &r) const;

    void prepare(QNetworkRequest &req) const;
    void setBasicAuth(QNetworkRequest &req) const;
    void resetSession(); // drops cookies; used after a 403

    bool hasSessionCookie() const;
    QList<QNetworkCookie> cookies() const;
    void setCookies(const QList<QNetworkCookie> &cookies);

    static QString esc(const QString &s);

    const Config &cfg;
    QString base;
    QNetworkAccessManager *nam = nullptr;
    // Transmission RPC session id, kept across backend switches.
    QString transmissionSessionId;

private:
    std::function<void(const QString &)> _log;
    std::function<bool()> _isCancelled;
};

// Splits a comma-separated tag/category list, trimmed, without empties.
QStringList splitTagList(const QString &tags);

// One implementation per supported server. All methods throw ClientException
// on connection, HTTP, and protocol errors. Operations a server does not
// support are not silent no-ops: the base implementations throw.
class TorrentBackend {
public:
    virtual ~TorrentBackend() = default;

    // Server info, filled in by login()/fetchServerInfo().
    QString appVersion = QStringLiteral("?");
    QString apiVersion = QStringLiteral("?");

    virtual QString backendName() const = 0;

    // Filled in by each backend constructor: what this server supports.
    BackendCaps caps;

    virtual void login() = 0;
    virtual void fetchServerInfo() = 0;
    virtual QJsonObject preferences() = 0;
    virtual QString defaultSavePath() = 0;
    virtual QVector<QPair<QString, QString>> categories() = 0;
    virtual QStringList tags() = 0;

    virtual QVector<TorrentInfo> info(const QString &hashes) = 0;
    virtual void addMagnet(const QString &magnet, bool addStopped, bool stopAfterMetadata,
                           const QString &savepath, const QString &category,
                           const QString &tags, const QString &contentLayout) = 0;
    virtual void addTorrentFile(const QByteArray &torrentData, const QString &fileName,
                                bool addStopped, bool stopAfterMetadata,
                                const QString &savepath, const QString &category,
                                const QString &tags, const QString &contentLayout) = 0;
    virtual QVector<TorrentFile> files(const QString &hash) = 0;
    virtual void filePrio(const QString &hash, const QList<int> &ids, int priority) = 0;
    virtual void setForceStart(const QString &hash, bool value) = 0;
    virtual void startTorrent(const QString &hash) = 0;
    virtual void stopTorrent(const QString &hash) = 0;
    virtual void deleteTorrent(const QString &hash, bool deleteFiles) = 0;
    virtual void setLocation(const QString &hash, const QString &location) = 0;
    virtual void setDownloadLimit(const QString &hash, qint64 bytesPerSec) = 0;
    virtual void setUploadLimit(const QString &hash, qint64 bytesPerSec) = 0;

    // Server-optional operations: the defaults throw, backends that support
    // them override.
    virtual void setCategory(const QString &hash, const QString &category);
    virtual void addTags(const QString &hash, const QString &tags);
    virtual void removeTags(const QString &hash, const QString &tags);
    virtual void setTags(const QString &hash, const QString &newTags, const QString &oldTags);
    virtual void addTrackers(const QString &hash, const QStringList &trackers);
    virtual void removeTrackers(const QString &hash, const QStringList &trackers);
    virtual QStringList torrentTrackers(const QString &hash);
    virtual void replaceTracker(const QString &hash, const QString &origUrl, const QString &newUrl);
    virtual void rename(const QString &hash, const QString &name);
    virtual void setAutoManagement(const QString &hash, bool enable);
    virtual void toggleSequentialDownload(const QString &hash);
    virtual void toggleFirstLastPiecePrio(const QString &hash);

protected:
    explicit TorrentBackend(ClientTransport &sh) : _sh(sh) {}

    ClientTransport &_sh;

    [[noreturn]] void throwUnsupported(const char *operation) const;
};

class QBittorrentBackend : public TorrentBackend {
public:
    explicit QBittorrentBackend(ClientTransport &sh) : TorrentBackend(sh)
    {
        caps = BackendCaps{true, true, true};
    }

    QString backendName() const override { return QStringLiteral("qBittorrent"); }

    void login() override;
    void fetchServerInfo() override;
    QJsonObject preferences() override;
    QString defaultSavePath() override;
    QVector<QPair<QString, QString>> categories() override;
    QStringList tags() override;

    QVector<TorrentInfo> info(const QString &hashes) override;
    void addMagnet(const QString &magnet, bool addStopped, bool stopAfterMetadata,
                   const QString &savepath, const QString &category,
                   const QString &tags, const QString &contentLayout) override;
    void addTorrentFile(const QByteArray &torrentData, const QString &fileName,
                        bool addStopped, bool stopAfterMetadata,
                        const QString &savepath, const QString &category,
                        const QString &tags, const QString &contentLayout) override;
    QVector<TorrentFile> files(const QString &hash) override;
    void filePrio(const QString &hash, const QList<int> &ids, int priority) override;
    void setForceStart(const QString &hash, bool value) override;
    void startTorrent(const QString &hash) override;
    void stopTorrent(const QString &hash) override;
    void deleteTorrent(const QString &hash, bool deleteFiles) override;
    void setLocation(const QString &hash, const QString &location) override;
    void setDownloadLimit(const QString &hash, qint64 bytesPerSec) override;
    void setUploadLimit(const QString &hash, qint64 bytesPerSec) override;

    void setCategory(const QString &hash, const QString &category) override;
    void addTags(const QString &hash, const QString &tags) override;
    void removeTags(const QString &hash, const QString &tags) override;
    void setTags(const QString &hash, const QString &newTags, const QString &oldTags) override;
    void addTrackers(const QString &hash, const QStringList &trackers) override;
    void removeTrackers(const QString &hash, const QStringList &trackers) override;
    QStringList torrentTrackers(const QString &hash) override;
    void replaceTracker(const QString &hash, const QString &origUrl, const QString &newUrl) override;
    void rename(const QString &hash, const QString &name) override;
    void setAutoManagement(const QString &hash, bool enable) override;
    void toggleSequentialDownload(const QString &hash) override;
    void toggleFirstLastPiecePrio(const QString &hash) override;

private:
    bool verifyAuthenticated();
    void callStartStop(const QString &hash, bool start);
    QString exec(const std::function<QNetworkReply *()> &make, const QString &path, bool allowRelogin);
    QString get(const QString &path, bool allowRelogin = true);
    QString postForm(const QString &path, const QString &formBody, bool allowRelogin = true);
    QString postMultipart(const QString &path, const QVector<QPair<QString, QString>> &fields,
                           bool allowRelogin = true);
    QString postMultipartWithFile(const QString &path, const QVector<QPair<QString, QString>> &fields,
                                  const QString &fileFieldName, const QString &fileName,
                                  const QByteArray &fileData, const QString &fileContentType,
                                  bool allowRelogin = true);

    std::optional<bool> _useStartStop;
};

class TransmissionBackend : public TorrentBackend {
public:
    explicit TransmissionBackend(ClientTransport &sh) : TorrentBackend(sh)
    {
        caps = BackendCaps{true, true, true};
    }

    QString backendName() const override { return QStringLiteral("Transmission"); }

    void login() override;
    void fetchServerInfo() override;
    QJsonObject preferences() override;
    QString defaultSavePath() override;
    QVector<QPair<QString, QString>> categories() override;
    QStringList tags() override;

    QVector<TorrentInfo> info(const QString &hashes) override;
    void addMagnet(const QString &magnet, bool addStopped, bool stopAfterMetadata,
                   const QString &savepath, const QString &category,
                   const QString &tags, const QString &contentLayout) override;
    void addTorrentFile(const QByteArray &torrentData, const QString &fileName,
                        bool addStopped, bool stopAfterMetadata,
                        const QString &savepath, const QString &category,
                        const QString &tags, const QString &contentLayout) override;
    QVector<TorrentFile> files(const QString &hash) override;
    void filePrio(const QString &hash, const QList<int> &ids, int priority) override;
    void setForceStart(const QString &hash, bool value) override;
    void startTorrent(const QString &hash) override;
    void stopTorrent(const QString &hash) override;
    void deleteTorrent(const QString &hash, bool deleteFiles) override;
    void setLocation(const QString &hash, const QString &location) override;
    void setDownloadLimit(const QString &hash, qint64 bytesPerSec) override;
    void setUploadLimit(const QString &hash, qint64 bytesPerSec) override;

    void setCategory(const QString &hash, const QString &category) override;
    void addTags(const QString &hash, const QString &tags) override;
    void setTags(const QString &hash, const QString &newTags, const QString &oldTags) override;
    void addTrackers(const QString &hash, const QStringList &trackers) override;
    void removeTrackers(const QString &hash, const QStringList &trackers) override;
    QStringList torrentTrackers(const QString &hash) override;
    void replaceTracker(const QString &hash, const QString &origUrl, const QString &newUrl) override;
    void rename(const QString &hash, const QString &name) override;
    void toggleSequentialDownload(const QString &hash) override;

private:
    QJsonObject execTransmissionRpc(const QString &method, const QJsonObject &arguments = QJsonObject());
    // session-get result, fetched once and reused by login/preferences/defaultSavePath.
    const QJsonObject &session();

    std::optional<QJsonObject> _sessionCache;
    qint64 _tag = 0;
};

class Aria2Backend : public TorrentBackend {
public:
    explicit Aria2Backend(ClientTransport &sh) : TorrentBackend(sh) {}

    QString backendName() const override { return QStringLiteral("Aria2"); }

    void login() override;
    void fetchServerInfo() override;
    QJsonObject preferences() override;
    QString defaultSavePath() override;
    QVector<QPair<QString, QString>> categories() override;
    QStringList tags() override;

    QVector<TorrentInfo> info(const QString &hashes) override;
    void addMagnet(const QString &magnet, bool addStopped, bool stopAfterMetadata,
                   const QString &savepath, const QString &category,
                   const QString &tags, const QString &contentLayout) override;
    void addTorrentFile(const QByteArray &torrentData, const QString &fileName,
                        bool addStopped, bool stopAfterMetadata,
                        const QString &savepath, const QString &category,
                        const QString &tags, const QString &contentLayout) override;
    QVector<TorrentFile> files(const QString &hash) override;
    void filePrio(const QString &hash, const QList<int> &ids, int priority) override;
    void setForceStart(const QString &hash, bool value) override;
    void startTorrent(const QString &hash) override;
    void stopTorrent(const QString &hash) override;
    void deleteTorrent(const QString &hash, bool deleteFiles) override;
    void setLocation(const QString &hash, const QString &location) override;
    void setDownloadLimit(const QString &hash, qint64 bytesPerSec) override;
    void setUploadLimit(const QString &hash, qint64 bytesPerSec) override;

private:
    QJsonValue execAria2Rpc(const QString &method, const QJsonArray &params = QJsonArray());
    QString findGidByHash(const QString &hash);
    void aria2TorrentRpc(const QString &hash, const QString &method,
                         const QJsonObject &opt, const QString &what);
    void registerGidFromStatus(const QString &gid, const QString &what);
    // aria2.getGlobalOption result, fetched once and reused by preferences/defaultSavePath.
    const QJsonObject &globalOptions();

    std::optional<QJsonObject> _globalOptionsCache;
    qint64 _tag = 0;
    QMap<QString, QString> _hashToGid;
};
