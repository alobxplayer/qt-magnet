#include "worker.h"
#include "config.h"
#include "logger.h"
#include "passkey.h"
#include "torrentpayload.h"

#include <QCoreApplication>
#include <QThread>

namespace {

// Poll interval of the progress monitoring loop, and how long to wait for
// the started state to settle before verifying it.
constexpr int kPollIntervalMs = 1000;
constexpr int kStartSettleMs = 500;
constexpr int kForceStartAttempts = 5;
// resolveHash gets at least this much time even with a tiny request timeout.
constexpr int kMinResolveTimeoutMs = 8000;

// Strip characters that are invalid in Windows filenames from the uploaded .torrent name.
QString sanitizeFileName(const QString &name)
{
    QString s;
    s.reserve(name.size());
    for (const QChar ch : name) {
        const ushort u = ch.unicode();
        const bool invalid = (u <= 0x1F) || u == 0x7F
            || u == '<' || u == '>' || u == ':' || u == '"' || u == '/'
            || u == '\\' || u == '|' || u == '?' || u == '*';
        s.append(invalid ? QLatin1Char('_') : ch);
    }
    return s;
}

} // namespace

Worker::Worker(const Config &cfg, const TorrentPayload &payload, bool quick, QObject *parent)
    : QThread(parent), _cfg(cfg), _payload(payload), _quick(quick)
{
}

Worker::~Worker()
{
    requestCancel();
    if (isRunning()) {
        wait();
    }
}

void Worker::setTask(Task task)
{
    _task = task;
}

void Worker::requestCancel()
{
    _cancelRequested.store(true, std::memory_order_relaxed);
}

void Worker::resetCancel()
{
    _cancelRequested.store(false, std::memory_order_relaxed);
}

bool Worker::wasCancelled() const
{
    return _cancelRequested.load(std::memory_order_relaxed);
}

void Worker::sleepCancellable(int ms)
{
    constexpr int step = 100;
    for (int e = 0; e < ms; e += step) {
        if (wasCancelled())
            return;
        QThread::msleep(qMin(step, ms - e));
    }
}

void Worker::run()
{
    auto statusCb = [this](const QString &s) {
        if (_task != Poll)
            emit status(s);
    };
    _client = new TorrentClient(_cfg, statusCb, [this]() { return wasCancelled(); });
    if (_detectedType != TorrentClient::ClientType::Auto) {
        _client->clientType = _detectedType;
        _client->detectedType = _detectedType;
    }
    if (!_sessionCookies.isEmpty())
        _client->setCookies(_sessionCookies);
    if (!_transmissionSessionId.isEmpty())
        _client->setTransmissionSessionId(_transmissionSessionId);

    switch (_task) {
    case Prepare:     doPrepare();     break;
    case Apply:       doApply();       break;
    case QuickFinish: doQuickFinish(); break;
    case Cleanup:     doCleanup();     break;
    case Poll:
        // The whole polling lifetime runs on ONE client, so the session
        // and the connection pool are reused across ticks instead of being
        // rebuilt every second. Cancellation breaks the loop promptly
        // (execRaw aborts an in-flight request within ~100 ms).
        while (!wasCancelled()) {
            doPoll();
            sleepCancellable(kPollIntervalMs);
        }
        break;
    }

    _sessionCookies = _client->cookies();
    _transmissionSessionId = _client->transmissionSessionId();

    delete _client;
    _client = nullptr;
}

void Worker::doPrepare()
{
    try {
        emit status(tr("Connecting..."));
        _client->login();
        _detectedType = _client->detectedType;
        caps = _client->backendCaps();
        _client->fetchServerInfo();
        try { categories = _client->categories(); } catch (const std::exception &ex) { Log::write(QStringLiteral("fetch categories: %1").arg(QString::fromUtf8(ex.what()))); }

        if (wasCancelled()) { emit prepareFinished(false, QString()); return; }

        const QString targetHash = _payload.hash();
        QSet<QString> before = _client->allHashes();
        existed = !targetHash.isEmpty() && before.contains(targetHash.toLower());

        // Injected into magnet announce URLs before the add itself.
        QString ownPk;
        try {
            ownPk = _cfg.getRutrackerPasskey();
        } catch (const std::exception &ex) {
            Log::write(QStringLiteral("Failed to load rutracker passkey: %1").arg(QString::fromUtf8(ex.what())));
        }

        if (_payload.isFile()) {
            emit status(tr("Adding torrent file..."));
            try {
                QString fn = _payload.displayName().isEmpty()
                    ? QStringLiteral("torrent.torrent")
                    : (sanitizeFileName(_payload.displayName()) + QStringLiteral(".torrent"));
                _client->addTorrentFile(_payload.torrentData.rawData,
                                       fn,
                                       !_quick && !existed,
                                       false,
                                       QString(),
                                       _cfg.defaultCategory,
                                       _cfg.defaultTags,
                                       _cfg.contentLayout);
            } catch (const std::exception &ex) {
                Log::write(QStringLiteral("addTorrentFile response: %1").arg(QString::fromUtf8(ex.what())));
                if (!existed)
                    throw;
            }
        } else {
            emit status(tr("Adding magnet link..."));
            QString magnetUri = _payload.magnet.raw;
            if (!ownPk.isEmpty()) {
                const QString injected = PassKey::injectIntoMagnet(magnetUri, ownPk);
                if (injected != magnetUri) {
                    magnetUri = injected;
                    Log::write(QStringLiteral("RuTracker passkey injected into the magnet announce URLs (%1)")
                                   .arg(targetHash));
                }
            }
            try {
                _client->addMagnet(magnetUri,
                                   false,
                                   !_quick && !existed,
                                   QString(),
                                   _cfg.defaultCategory,
                                   _cfg.defaultTags,
                                   _cfg.contentLayout);
            } catch (const std::exception &ex) {
                Log::write(QStringLiteral("addMagnet response: %1").arg(QString::fromUtf8(ex.what())));
                if (!existed)
                    throw;
            }
        }

        QStringList trList = _payload.trackers();
        if (!ownPk.isEmpty())
            trList += PassKey::applyRutrackerPasskey(trList, ownPk);
        if (existed && caps.addTrackers && !trList.isEmpty() && !targetHash.isEmpty()) {
            emit status(tr("Updating trackers..."));
            try {
                _client->addTrackers(targetHash, trList);
                Log::write(QStringLiteral("Merged %1 trackers into existing torrent %2")
                               .arg(trList.size()).arg(targetHash));
            } catch (const std::exception &ex) {
                Log::write(QStringLiteral("addTrackers: %1").arg(QString::fromUtf8(ex.what())));
            }
        }

        emit status(existed ? tr("Torrent already exists. Loading...") : tr("Waiting for torrent..."));
        QString resolved = _client->resolveHash(
            targetHash, before,
            qMax(kMinResolveTimeoutMs, _cfg.requestTimeoutSec * 1000),
            [this]() { return wasCancelled(); },
            _payload.hashV2());

        if (resolved.isEmpty()) {
            if (wasCancelled()) { emit prepareFinished(false, QString()); return; }
            if (!targetHash.isEmpty())
                resolved = targetHash;
            else
                throw ClientException(tr("Torrent not found."), 0, QString());
        }

        hash = resolved;
        weAdded = !existed;
        Log::write(QStringLiteral("Torrent %1 %2").arg(hash, existed ? QStringLiteral("already existed") : QStringLiteral("added by us")));

        torrentInfo = _client->infoOne(hash);
        if (torrentInfo) {
            initialCategory = torrentInfo->category;
            initialSavePath = torrentInfo->savePath;
            initialTags = torrentInfo->tags;
        }

        if (_quick) {
            doQuickFinish();
            return;
        }

        if (_payload.isFile()) {
            try {
                files = _client->files(hash);
            } catch (const std::exception &ex) {
                // Fall back to the file list parsed from the local .torrent.
                Log::write(QStringLiteral("files: %1").arg(QString::fromUtf8(ex.what())));
            }
            if (files.isEmpty())
                files = _payload.torrentData.files;

            if (!existed && !files.isEmpty()) {
                try { _client->stopTorrent(hash); }
                catch (const std::exception &ex) {
                    Log::write(QStringLiteral("Pause after add: %1").arg(QString::fromUtf8(ex.what())));
                }
            }
        } else {
            emit status(existed ? tr("Updating torrent metadata...") : tr("Fetching metadata..."));
            files = _client->waitForMetadata(
                hash, _cfg.metadataTimeoutSec,
                [this](int sec) { emit status(tr("Fetching metadata (%1 s)...").arg(sec)); },
                [this]() { return wasCancelled(); },
                !existed);

            if (wasCancelled()) { emit prepareFinished(false, QString()); return; }

            if (!existed && !files.isEmpty()) {
                try { _client->stopTorrent(hash); }
                catch (const std::exception &ex) {
                    Log::write(QStringLiteral("Pause after metadata: %1").arg(QString::fromUtf8(ex.what())));
                }
            }
        }

        emit prepareFinished(true, QString());
    } catch (const std::exception &ex) {
        Log::write(QStringLiteral("Prepare error: %1").arg(QString::fromUtf8(ex.what())));
        emit prepareFinished(false, QString::fromUtf8(ex.what()));
    }
}

void Worker::doQuickFinish()
{
    try {
        if (_cfg.forceStartDelayMs > 0) {
            emit status(tr("Pausing..."));
            sleepCancellable(_cfg.forceStartDelayMs);
        }
        if (wasCancelled()) { emit quickFinished(false, QString()); return; }

        emit status(tr("Force starting..."));
        resultOk = _client->forceStartVerified(hash, kForceStartAttempts,
            [this](const QString &s) { emit status(s); });

        emit quickFinished(resultOk, resultOk ? QString() : tr("Failed to force start."));
    } catch (const std::exception &ex) {
        Log::write(QStringLiteral("Quick mode error: %1").arg(QString::fromUtf8(ex.what())));
        emit quickFinished(false, QString::fromUtf8(ex.what()));
    }
}

void Worker::doApply()
{
    try {
        _client->login();
        const auto &p = applyParams;

        if (!p.trackers.isEmpty() && caps.addTrackers) {
            try {
                _client->addTrackers(p.hash, p.trackers);
            } catch (const std::exception &ex) {
                Log::write(QStringLiteral("addTrackers in apply: %1").arg(QString::fromUtf8(ex.what())));
            }
        }

        if (p.anyChanged) {
            emit status(tr("Applying priorities..."));
            for (auto it = p.prioritiesByPrio.constBegin(); it != p.prioritiesByPrio.constEnd(); ++it) {
                if (!it.value().isEmpty())
                    _client->filePrio(p.hash, it.value(), it.key());
            }
        }

        if (caps.setCategory && p.category != initialCategory) {
            emit status(tr("Setting category..."));
            try { _client->setCategory(p.hash, p.category); }
            catch (const ClientException &ex) { Log::write(QStringLiteral("setCategory: %1").arg(ex.message)); }
        }

        if (caps.setTags && p.tags != initialTags) {
            emit status(tr("Setting tags..."));
            try { _client->setTags(p.hash, p.tags, initialTags); }
            catch (const ClientException &ex) { Log::write(QStringLiteral("setTags: %1").arg(ex.message)); }
        }

        if (!p.savePath.isEmpty()
            && p.savePath.compare(initialSavePath, Qt::CaseInsensitive) != 0) {
            emit status(tr("Setting save path..."));
            try { _client->setLocation(p.hash, p.savePath); }
            catch (const ClientException &ex) { Log::write(QStringLiteral("setLocation: %1").arg(ex.message)); }
        }

        if (p.forceStart) {
            emit status(tr("Force starting..."));
            resultOk = _client->forceStartVerified(p.hash, kForceStartAttempts,
                [this](const QString &s) { emit status(s); });
        } else {
            emit status(tr("Starting..."));
            try { _client->setForceStart(p.hash, false); } catch (const std::exception &ex) { Log::write(QStringLiteral("reset forceStart: %1").arg(QString::fromUtf8(ex.what()))); }
            _client->startTorrent(p.hash);
            sleepCancellable(kStartSettleMs);
            auto t = _client->infoOne(p.hash);
            resultOk = t && !t->isStopped();
        }

        emit applyFinished(true, resultOk ? QString() : tr("Status unverified."));
    } catch (const std::exception &ex) {
        Log::write(QStringLiteral("Apply error: %1").arg(QString::fromUtf8(ex.what())));
        emit applyFinished(false, QString::fromUtf8(ex.what()));
    }
}

void Worker::doCleanup()
{
    try {
        _client->login();
        _client->deleteTorrent(hash, true);
    } catch (const std::exception &ex) {
        Log::write(QStringLiteral("Cleanup error: %1").arg(QString::fromUtf8(ex.what())));
    }
    emit cleanupFinished();
}

void Worker::doPoll()
{
    try {
        QString targetHash = hash;
        if (targetHash.isEmpty())
            targetHash = _payload.hash();
        if (targetHash.isEmpty()) {
            emit pollFinished(false);
            return;
        }

        // Re-login only when a session-based backend lost its session; aria2
        // is stateless and needs no login per tick.
        const bool sessionLost =
            (_detectedType == TorrentClient::ClientType::QBittorrent && !_client->hasSessionCookie())
            || (_detectedType == TorrentClient::ClientType::Transmission
                && _client->transmissionSessionId().isEmpty());
        if (sessionLost)
            _client->login();
        pollInfo = _client->infoOne(targetHash);
        pollFiles = _client->files(targetHash);
        if (wasCancelled()) {
            emit pollFinished(false);
            return;
        }
        emit pollFinished(true);
    } catch (const std::exception &ex) {
        Log::write(QStringLiteral("poll error: %1").arg(QString::fromUtf8(ex.what())));
        emit pollFinished(false);
    }
}
