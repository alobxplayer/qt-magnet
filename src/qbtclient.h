#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QPair>
#include <QSet>
#include <QJsonObject>
#include <QJsonArray>
#include <QMetaType>
#include <functional>
#include <optional>
#include <exception>

class Config;
class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;

class QbtException : public std::exception {
public:
    QString message;
    int     status;
    QString body;
    QbtException(const QString &msg, int st = 0, const QString &b = QString());
    const char *what() const noexcept override { return _utf8.constData(); }
private:
    QByteArray _utf8;
};

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
    bool isQueued() const { return state == QLatin1String("queuedDL") || state == QLatin1String("queuedUP"); }
    bool isForced() const {
        return state == QLatin1String("forcedDL") || state == QLatin1String("forcedUP")
               || state == QLatin1String("forcedMetaDL");
    }
    static TorrentInfo from(const QJsonObject &d);
};

class QbtClient {
public:
    enum class ClientType {
        Auto,
        QBittorrent,
        Transmission,
        Aria2
    };

    QString appVersion = QStringLiteral("?");
    QString apiVersion = QStringLiteral("?");
    ClientType clientType = ClientType::Auto;
    ClientType detectedType = ClientType::QBittorrent;
    bool    serverAddsStopped = false;

    QbtClient(const Config &cfg, std::function<void(const QString &)> log,
              std::function<bool()> isCancelled = nullptr);
    ~QbtClient();

    void login();
    void fetchServerInfo();
    QJsonObject preferences();
    QString defaultSavePath();
    QVector<QPair<QString, QString>> categories();
    QStringList tags();

    QVector<TorrentInfo> info(const QString &hashes);
    std::optional<TorrentInfo> infoOne(const QString &hash);
    QSet<QString> allHashes();
    void addMagnet(const QString &magnet, bool addStopped, bool stopAfterMetadata,
                   const QString &savepath, const QString &category,
                   const QString &tags, const QString &contentLayout);
    void addTorrentFile(const QByteArray &torrentData, const QString &fileName,
                        bool addStopped, bool stopAfterMetadata,
                        const QString &savepath, const QString &category,
                        const QString &tags, const QString &contentLayout);
    QVector<TorrentFile> files(const QString &hash);
    void filePrio(const QString &hash, const QList<int> &ids, int priority);
    void setForceStart(const QString &hash, bool value);
    void startTorrent(const QString &hash);
    void stopTorrent(const QString &hash);
    void deleteTorrent(const QString &hash, bool deleteFiles);
    void setLocation(const QString &hash, const QString &location);
    void setCategory(const QString &hash, const QString &category);
    void addTags(const QString &hash, const QString &tags);
    void removeTags(const QString &hash, const QString &tags);
    void setTags(const QString &hash, const QString &newTags, const QString &oldTags = QString());
    void addTrackers(const QString &hash, const QStringList &trackers);
    void rename(const QString &hash, const QString &name);
    void setAutoManagement(const QString &hash, bool enable);
    void setDownloadLimit(const QString &hash, qint64 bytesPerSec);
    void setUploadLimit(const QString &hash, qint64 bytesPerSec);
    void toggleSequentialDownload(const QString &hash);
    void toggleFirstLastPiecePrio(const QString &hash);

    QString resolveHash(const QString &expectedHash, const QSet<QString> &before,
                        int timeoutMs, const std::function<bool()> &cancelled,
                        const QString &expectedHashV2 = QString());
    QVector<TorrentFile> waitForMetadata(const QString &hash, int timeoutSec,
                                         const std::function<void(int)> &onTick,
                                         const std::function<bool()> &cancelled, bool nudgeStart);
    bool forceStartVerified(const QString &hash, int attempts,
                            const std::function<void(const QString &)> &progress);

    static QString stateString(const QString &state);

private:
    const Config &_cfg;
    std::function<void(const QString &)> _log;
    std::function<bool()> _isCancelled;
    QNetworkAccessManager *_nam = nullptr;
    QString _base;
    std::optional<bool> _useStartStop;

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

    void trace(const QString &s);
    void prepare(QNetworkRequest &req) const;
    void setBasicAuth(QNetworkRequest &req) const;
    RawResponse execRaw(const std::function<QNetworkReply *()> &make);
    QString exec(const std::function<QNetworkReply *()> &make, const QString &path, bool allowRelogin);
    QString get(const QString &path, bool allowRelogin = true);
    QString postForm(const QString &path, const QString &formBody, bool allowRelogin = true);
    QString postMultipart(const QString &path, const QVector<QPair<QString, QString>> &fields, bool allowRelogin = true);
    QString postMultipartWithFile(const QString &path, const QVector<QPair<QString, QString>> &fields,
                                  const QString &fileFieldName, const QString &fileName,
                                  const QByteArray &fileData, const QString &fileContentType,
                                  bool allowRelogin = true);
    bool hasSessionCookie();
    bool verifyAuthenticated();
    void callStartStop(const QString &hash, bool start);
    static QString esc(const QString &s);

    QString _transmissionSessionId;
    qint64  _transmissionTag = 0;
    void loginQBittorrent();
    void loginTransmission();
    QJsonObject execTransmissionRpc(const QString &method, const QJsonObject &arguments = QJsonObject());

    qint64  _aria2Tag = 0;
    QMap<QString, QString> _hashToGid;
    void loginAria2();
    QJsonValue execAria2Rpc(const QString &method, const QJsonArray &params = QJsonArray());
    QString findGidByHash(const QString &hash);
};

Q_DECLARE_METATYPE(TorrentFile)
Q_DECLARE_METATYPE(TorrentInfo)
Q_DECLARE_METATYPE(QVector<TorrentFile>)
