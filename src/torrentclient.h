#pragma once

#include "torrenttypes.h"

#include <QString>
#include <QStringList>
#include <QVector>
#include <QPair>
#include <QSet>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkCookie>
#include <functional>
#include <memory>
#include <optional>
#include <exception>

class Config;
class ClientTransport;
class TorrentBackend;

class ClientException : public std::exception {
public:
    QString message;
    int     status;
    QString body;
    ClientException(const QString &msg, int st = 0, const QString &b = QString());
    const char *what() const noexcept override { return _utf8.constData(); }
private:
    QByteArray _utf8;
};

// Client facade: picks the backend matching the configured server type
// (auto-detecting it on login) and forwards calls to it. Cross-backend
// logic — tracker passkey rewrite, hash resolution, metadata waits —
// lives here; everything protocol-specific lives in TorrentBackend subclasses.
class TorrentClient {
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

    TorrentClient(const Config &cfg, std::function<void(const QString &)> log,
              std::function<bool()> isCancelled = nullptr);
    ~TorrentClient();

    void login();
    void fetchServerInfo();
    QJsonObject preferences();
    QString defaultSavePath();
    QVector<QPair<QString, QString>> categories();
    QStringList tags();
    // Capabilities of the currently selected backend (valid after login).
    BackendCaps backendCaps() const;

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
    void removeTrackers(const QString &hash, const QStringList &trackers);
    QStringList torrentTrackers(const QString &hash);
    void replaceTracker(const QString &hash, const QString &origUrl, const QString &newUrl);

    struct TrackerRewriteSummary {
        int torrents = 0;   // torrents scanned
        int rewritten = 0;  // announce URLs rewritten with the passkey
        int failed = 0;     // URLs or torrents that could not be updated
    };
    // Rewrite the rutracker announce URLs of EVERY torrent on the server to
    // carry the given passkey. Throws for aria2 (unsupported) and when the
    // passkey is empty.
    TrackerRewriteSummary rewriteRutrackerPasskeys(const QString &passkey,
                                                   const std::function<void(const QString &)> &progress = nullptr,
                                                   const std::function<bool()> &cancelled = nullptr);
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

    QList<QNetworkCookie> cookies() const;
    void setCookies(const QList<QNetworkCookie> &cookies);
    QString transmissionSessionId() const;
    void setTransmissionSessionId(const QString &id);
    bool hasSessionCookie();

private:
    void setBackend(ClientType type);
    void syncServerInfo();

    std::unique_ptr<ClientTransport> _shared;
    std::unique_ptr<TorrentBackend> _backend;
    ClientType _backendType = ClientType::QBittorrent;
};

