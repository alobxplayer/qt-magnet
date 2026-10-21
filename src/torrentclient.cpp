#include "torrentclient.h"
#include "config.h"
#include "logger.h"
#include "magnetlink.h"
#include "passkey.h"
#include "torrentfile.h"
#include "torrentbackend.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

namespace {
// Poll and settle intervals for the wait loops below.
constexpr int kResolvePollMs = 400;
constexpr int kMetadataPollMs = 600;
constexpr int kFirstVerifyDelayMs = 500;
constexpr int kRetryVerifyDelayMs = 900;
}

ClientException::ClientException(const QString &msg, int st, const QString &b)
    : message(msg), status(st), body(b), _utf8(msg.toUtf8())
{
}

TorrentClient::TorrentClient(const Config &cfg, std::function<void(const QString &)> log,
                     std::function<bool()> isCancelled)
{
    _shared = std::make_unique<ClientTransport>(cfg, std::move(log), std::move(isCancelled));

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
    setBackend(detectedType);
}

TorrentClient::~TorrentClient() = default;

void TorrentClient::setBackend(ClientType type)
{
    if (_backend && _backendType == type)
        return;
    _backend.reset();
    switch (type) {
    case ClientType::Transmission:
        _backend = std::make_unique<TransmissionBackend>(*_shared);
        break;
    case ClientType::Aria2:
        _backend = std::make_unique<Aria2Backend>(*_shared);
        break;
    case ClientType::QBittorrent:
    case ClientType::Auto:
        _backend = std::make_unique<QBittorrentBackend>(*_shared);
        break;
    }
    _backendType = type;
}

void TorrentClient::syncServerInfo()
{
    appVersion = _backend->appVersion;
    apiVersion = _backend->apiVersion;
}

// ------------------------------------------------------------- session ----

void TorrentClient::login()
{
    if (clientType == ClientType::Auto) {
        try {
            _shared->trace(QStringLiteral("Probing for qBittorrent at %1").arg(_shared->base));
            setBackend(ClientType::QBittorrent);
            _backend->login();
            detectedType = ClientType::QBittorrent;
            _shared->trace(QStringLiteral("Detected server: qBittorrent"));
            syncServerInfo();
            return;
        } catch (const ClientException &qbtEx) {
            _shared->trace(QStringLiteral("qBittorrent probe failed: %1. Probing for Transmission...").arg(qbtEx.message));
            try {
                setBackend(ClientType::Transmission);
                _backend->login();
                detectedType = ClientType::Transmission;
                _shared->trace(QStringLiteral("Detected server: Transmission"));
                syncServerInfo();
                return;
            } catch (const ClientException &trEx) {
                _shared->trace(QStringLiteral("Transmission probe failed: %1. Probing for Aria2...").arg(trEx.message));
                try {
                    setBackend(ClientType::Aria2);
                    _backend->login();
                    detectedType = ClientType::Aria2;
                    _shared->trace(QStringLiteral("Detected server: Aria2"));
                    syncServerInfo();
                    return;
                } catch (const ClientException &ariaEx) {
                    throw ClientException(QCoreApplication::translate("TorrentClient", "Could not connect to qBittorrent (%1), Transmission (%2), or Aria2 (%3)")
                                           .arg(qbtEx.message, trEx.message, ariaEx.message),
                                       qbtEx.status != 0 ? qbtEx.status : (trEx.status != 0 ? trEx.status : ariaEx.status));
                }
            }
        }
    }

    setBackend(clientType);
    _backend->login();
    detectedType = clientType;
    syncServerInfo();
}

void TorrentClient::fetchServerInfo()
{
    _backend->fetchServerInfo();
    syncServerInfo();
}

BackendCaps TorrentClient::backendCaps() const
{
    return _backend->caps;
}

QJsonObject TorrentClient::preferences()
{
    QJsonObject res = _backend->preferences();
    syncServerInfo();
    return res;
}

QString TorrentClient::defaultSavePath()
{
    return _backend->defaultSavePath();
}

QVector<QPair<QString, QString>> TorrentClient::categories()
{
    return _backend->categories();
}

QStringList TorrentClient::tags()
{
    return _backend->tags();
}

QList<QNetworkCookie> TorrentClient::cookies() const
{
    return _shared->cookies();
}

void TorrentClient::setCookies(const QList<QNetworkCookie> &cookies)
{
    _shared->setCookies(cookies);
}

QString TorrentClient::transmissionSessionId() const
{
    return _shared->transmissionSessionId;
}

void TorrentClient::setTransmissionSessionId(const QString &id)
{
    _shared->transmissionSessionId = id;
}

bool TorrentClient::hasSessionCookie()
{
    return _shared->hasSessionCookie();
}

// ------------------------------------------------------- torrent listing ----

QVector<TorrentInfo> TorrentClient::info(const QString &hashes)
{
    return _backend->info(hashes);
}

std::optional<TorrentInfo> TorrentClient::infoOne(const QString &hash)
{
    const auto list = info(hash);
    if (!list.isEmpty())
        return list.front();
    return std::nullopt;
}

QSet<QString> TorrentClient::allHashes()
{
    QSet<QString> set;
    for (const TorrentInfo &t : info(QString())) {
        if (!t.hash.isEmpty())
            set.insert(t.hash.toLower());
        if (!t.infoHashV1.isEmpty())
            set.insert(t.infoHashV1.toLower());
        if (!t.infoHashV2.isEmpty())
            set.insert(t.infoHashV2.toLower());
    }
    return set;
}

// ------------------------------------------------------------- forwarding ----

void TorrentClient::addMagnet(const QString &magnet, bool addStopped, bool stopAfterMetadata,
                          const QString &savepath, const QString &category,
                          const QString &tags, const QString &contentLayout)
{
    _backend->addMagnet(magnet, addStopped, stopAfterMetadata, savepath, category, tags, contentLayout);
}

void TorrentClient::addTorrentFile(const QByteArray &torrentData, const QString &fileName,
                               bool addStopped, bool stopAfterMetadata,
                               const QString &savepath, const QString &category,
                               const QString &tags, const QString &contentLayout)
{
    _backend->addTorrentFile(torrentData, fileName, addStopped, stopAfterMetadata, savepath, category, tags, contentLayout);
}

QVector<TorrentFile> TorrentClient::files(const QString &hash)
{
    return _backend->files(hash);
}

void TorrentClient::filePrio(const QString &hash, const QList<int> &ids, int priority)
{
    _backend->filePrio(hash, ids, priority);
}

void TorrentClient::setForceStart(const QString &hash, bool value)
{
    _backend->setForceStart(hash, value);
}

void TorrentClient::startTorrent(const QString &hash)
{
    _backend->startTorrent(hash);
}

void TorrentClient::stopTorrent(const QString &hash)
{
    _backend->stopTorrent(hash);
}

void TorrentClient::deleteTorrent(const QString &hash, bool deleteFiles)
{
    _backend->deleteTorrent(hash, deleteFiles);
}

void TorrentClient::setLocation(const QString &hash, const QString &location)
{
    _backend->setLocation(hash, location);
}

void TorrentClient::setCategory(const QString &hash, const QString &category)
{
    _backend->setCategory(hash, category);
}

void TorrentClient::addTags(const QString &hash, const QString &tags)
{
    _backend->addTags(hash, tags);
}

void TorrentClient::removeTags(const QString &hash, const QString &tags)
{
    _backend->removeTags(hash, tags);
}

void TorrentClient::setTags(const QString &hash, const QString &newTags, const QString &oldTags)
{
    _backend->setTags(hash, newTags, oldTags);
}

void TorrentClient::addTrackers(const QString &hash, const QStringList &trackers)
{
    _backend->addTrackers(hash, trackers);
}

void TorrentClient::removeTrackers(const QString &hash, const QStringList &trackers)
{
    _backend->removeTrackers(hash, trackers);
}

QStringList TorrentClient::torrentTrackers(const QString &hash)
{
    return _backend->torrentTrackers(hash);
}

void TorrentClient::replaceTracker(const QString &hash, const QString &origUrl, const QString &newUrl)
{
    _backend->replaceTracker(hash, origUrl, newUrl);
}

void TorrentClient::rename(const QString &hash, const QString &name)
{
    _backend->rename(hash, name);
}

void TorrentClient::setAutoManagement(const QString &hash, bool enable)
{
    _backend->setAutoManagement(hash, enable);
}

void TorrentClient::setDownloadLimit(const QString &hash, qint64 bytesPerSec)
{
    _backend->setDownloadLimit(hash, bytesPerSec);
}

void TorrentClient::setUploadLimit(const QString &hash, qint64 bytesPerSec)
{
    _backend->setUploadLimit(hash, bytesPerSec);
}

void TorrentClient::toggleSequentialDownload(const QString &hash)
{
    _backend->toggleSequentialDownload(hash);
}

void TorrentClient::toggleFirstLastPiecePrio(const QString &hash)
{
    _backend->toggleFirstLastPiecePrio(hash);
}

// ------------------------------------------------------ cross-backend ops ----

TorrentClient::TrackerRewriteSummary TorrentClient::rewriteRutrackerPasskeys(
    const QString &passkey, const std::function<void(const QString &)> &progress,
    const std::function<bool()> &cancelled)
{
    if (passkey.isEmpty())
        throw ClientException(QStringLiteral("No passkey configured."));
    if (detectedType == ClientType::Aria2)
        throw ClientException(QStringLiteral("Tracker rewrite is not supported for aria2."));

    TrackerRewriteSummary s;
    const QList<QString> hashes = allHashes().values();
    for (const QString &h : hashes) {
        if (cancelled && cancelled())
            break;
        QStringList trs;
        try {
            trs = torrentTrackers(h);
            s.torrents++;
        } catch (const std::exception &ex) {
            ++s.failed;
            _shared->trace(QStringLiteral("trackers of %1: %2").arg(h, QString::fromUtf8(ex.what())));
            continue;
        }
        const QSet<QString> existing(trs.cbegin(), trs.cend());
        for (const QString &t : trs) {
            const QString rewritten = PassKey::rewriteUrl(t, passkey);
            if (rewritten == t)
                continue;
            if (existing.contains(rewritten)) {
                // The pk'd announce already exists (e.g. injected at add time
                // earlier): drop the bare original instead of failing the
                // edit with a duplicate-URL conflict.
                if (progress)
                    progress(QStringLiteral("%1: %2 (duplicate removed)").arg(h.left(12), t));
                try {
                    removeTrackers(h, {t});
                    ++s.rewritten;
                } catch (const std::exception &ex) {
                    ++s.failed;
                    _shared->trace(QStringLiteral("removeTrackers on %1 (%2): %3")
                                       .arg(h, t, QString::fromUtf8(ex.what())));
                }
                continue;
            }
            if (progress)
                progress(QStringLiteral("%1: %2").arg(h.left(12), t));
            try {
                replaceTracker(h, t, rewritten);
                ++s.rewritten;
            } catch (const std::exception &ex) {
                ++s.failed;
                _shared->trace(QStringLiteral("editTracker on %1 (%2): %3")
                                   .arg(h, t, QString::fromUtf8(ex.what())));
            }
        }
    }
    return s;
}

QString TorrentClient::resolveHash(const QString &expectedHash, const QSet<QString> &before,
                               int timeoutMs, const std::function<bool()> &cancelled,
                               const QString &expectedHashV2)
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
        }
        if (!expectedHashV2.isEmpty()) {
            const auto t2 = infoOne(expectedHashV2);
            if (t2 && !t2->hash.isEmpty())
                return t2->hash;
        }

        for (const TorrentInfo &t : info(QString())) {
            const QString hLower = t.hash.toLower();
            const QString v1Lower = t.infoHashV1.toLower();
            const QString v2Lower = t.infoHashV2.toLower();

            if (!expectedHash.isEmpty()) {
                const QString expLower = expectedHash.toLower();
                if (hLower == expLower || v1Lower == expLower || v2Lower == expLower)
                    return t.hash;
            }
            if (!expectedHashV2.isEmpty()) {
                const QString exp2Lower = expectedHashV2.toLower();
                if (hLower == exp2Lower || v1Lower == exp2Lower || v2Lower == exp2Lower)
                    return t.hash;
            }

            if (!before.contains(hLower) &&
                (v1Lower.isEmpty() || !before.contains(v1Lower)) &&
                (v2Lower.isEmpty() || !before.contains(v2Lower))) {
                _shared->trace(QStringLiteral("Identified hash by list diff: %1").arg(t.hash));
                return t.hash;
            }
        }

        QThread::msleep(kResolvePollMs);
    }
    return QString();
}

QVector<TorrentFile> TorrentClient::waitForMetadata(const QString &hash, int timeoutSec,
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
            _shared->trace(QStringLiteral("Metadata received in %1 s, files: %2")
                               .arg(int(sw.elapsed() / 1000)).arg(list.size()));
            return list;
        }

        if (nudgeStart && !nudged) {
            try {
                const auto t = infoOne(hash);
                if (t && t->isStopped()) {
                    _shared->trace(QStringLiteral("Torrent stopped without metadata; starting to fetch metadata"));
                    startTorrent(hash);
                    nudged = true;
                }
            } catch (const ClientException &ex) {
                _shared->trace(QStringLiteral("Could not nudge start: %1").arg(ex.message));
            }
        }

        const int secs = int(sw.elapsed() / 1000);
        if (secs != lastReported && onTick) {
            lastReported = secs;
            onTick(secs);
        }
        QThread::msleep(kMetadataPollMs);
    }
    _shared->trace(QStringLiteral("Metadata not received within %1 s (tried starting: %2)")
                       .arg(timeoutSec).arg(nudged ? QStringLiteral("true") : QStringLiteral("false")));
    return {};
}

bool TorrentClient::forceStartVerified(const QString &hash, int attempts,
                                   const std::function<void(const QString &)> &progress)
{
    for (int i = 1; i <= attempts; ++i) {
        try {
            setForceStart(hash, true);
        } catch (const ClientException &ex) {
            _shared->trace(QStringLiteral("setForceStart attempt %1: %2").arg(i).arg(ex.message));
        }

        const int sleepTotal = (i == 1 ? kFirstVerifyDelayMs : kRetryVerifyDelayMs);
        for (int ms = 0; ms < sleepTotal; ms += 100) {
            if (_shared->isCancelled())
                return false;
            QThread::msleep(qMin(100, sleepTotal - ms));
        }

        const auto t = infoOne(hash);
        if (!t) {
            _shared->trace(QStringLiteral("Attempt %1: torrent not found in list").arg(i));
            continue;
        }

        _shared->trace(QStringLiteral("Attempt %1: state '%2', force_start=%3")
                           .arg(i).arg(t->state, t->forceStart ? QStringLiteral("true") : QStringLiteral("false")));
        if (progress)
            progress(QCoreApplication::translate("TorrentClient", "Verification %1/%2: %3")
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
        } catch (const ClientException &ex) {
            _shared->trace(QStringLiteral("start attempt %1: %2").arg(i).arg(ex.message));
        }
    }

    const auto last = infoOne(hash);
    if (detectedType == ClientType::Transmission || detectedType == ClientType::Aria2)
        return last && !last->isStopped();
    return last && last->forceStart && !last->isStopped();
}

QString TorrentClient::stateString(const QString &state)
{
    static const QHash<QString, const char *> m = {
        { QStringLiteral("error"), QT_TRANSLATE_NOOP("TorrentClient", "Error") },
        { QStringLiteral("missingFiles"), QT_TRANSLATE_NOOP("TorrentClient", "Missing files") },
        { QStringLiteral("uploading"), QT_TRANSLATE_NOOP("TorrentClient", "Uploading") },
        { QStringLiteral("pausedUP"), QT_TRANSLATE_NOOP("TorrentClient", "Paused (upload)") },
        { QStringLiteral("stoppedUP"), QT_TRANSLATE_NOOP("TorrentClient", "Paused (upload)") },
        { QStringLiteral("queuedUP"), QT_TRANSLATE_NOOP("TorrentClient", "Queued (upload)") },
        { QStringLiteral("stalledUP"), QT_TRANSLATE_NOOP("TorrentClient", "Stalled (upload)") },
        { QStringLiteral("checkingUP"), QT_TRANSLATE_NOOP("TorrentClient", "Checking (upload)") },
        { QStringLiteral("forcedUP"), QT_TRANSLATE_NOOP("TorrentClient", "Forced upload") },
        { QStringLiteral("allocating"), QT_TRANSLATE_NOOP("TorrentClient", "Allocating") },
        { QStringLiteral("downloading"), QT_TRANSLATE_NOOP("TorrentClient", "Downloading") },
        { QStringLiteral("metaDL"), QT_TRANSLATE_NOOP("TorrentClient", "Fetching metadata") },
        { QStringLiteral("forcedMetaDL"), QT_TRANSLATE_NOOP("TorrentClient", "Fetching metadata (forced)") },
        { QStringLiteral("pausedDL"), QT_TRANSLATE_NOOP("TorrentClient", "Paused") },
        { QStringLiteral("stoppedDL"), QT_TRANSLATE_NOOP("TorrentClient", "Paused") },
        { QStringLiteral("queuedDL"), QT_TRANSLATE_NOOP("TorrentClient", "Queued") },
        { QStringLiteral("stalledDL"), QT_TRANSLATE_NOOP("TorrentClient", "Stalled") },
        { QStringLiteral("checkingDL"), QT_TRANSLATE_NOOP("TorrentClient", "Checking") },
        { QStringLiteral("forcedDL"), QT_TRANSLATE_NOOP("TorrentClient", "Forced download") },
        { QStringLiteral("checkingResumeData"), QT_TRANSLATE_NOOP("TorrentClient", "Checking resume data") },
        { QStringLiteral("moving"), QT_TRANSLATE_NOOP("TorrentClient", "Moving") },
    };
    const auto it = m.constFind(state);
    return it != m.constEnd() ? QCoreApplication::translate("TorrentClient", it.value()) : state;
}
