#include "worker.h"
#include "config.h"
#include "logger.h"
#include "magnetlink.h"

#include <QCoreApplication>
#include <QThread>

Worker::Worker(const Config &cfg, const MagnetLink &link, bool quick, QObject *parent)
    : QThread(parent), _cfg(cfg), _link(link), _quick(quick)
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
    _client = new QbtClient(_cfg, [this](const QString &s) { emit status(s); }, [this]() { return wasCancelled(); });

    switch (_task) {
    case Prepare:     doPrepare();     break;
    case Apply:       doApply();       break;
    case QuickFinish: doQuickFinish(); break;
    case Cleanup:     doCleanup();     break;
    }

    delete _client;
    _client = nullptr;
}

void Worker::doPrepare()
{
    try {
        emit status(tr("Connecting..."));
        _client->login();
        _client->fetchServerInfo();
        try { _client->preferences(); } catch (const std::exception &ex) { Log::write(QStringLiteral("fetch preferences: %1").arg(QString::fromUtf8(ex.what()))); }
        try { categories = _client->categories(); } catch (const std::exception &ex) { Log::write(QStringLiteral("fetch categories: %1").arg(QString::fromUtf8(ex.what()))); }

        if (wasCancelled()) { emit prepareFinished(false, QString()); return; }

        emit status(tr("Adding magnet link..."));
        QSet<QString> before = _client->allHashes();
        existed = !_link.hash.isEmpty() && before.contains(_link.hash.toLower());

        try {
            _client->addMagnet(_link.raw,
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

        if (existed && !_link.trackers.isEmpty()) {
            emit status(tr("Updating trackers..."));
            try {
                _client->addTrackers(_link.hash, _link.trackers);
                Log::write(QStringLiteral("Merged %1 trackers into existing torrent %2")
                               .arg(_link.trackers.size()).arg(_link.hash));
            } catch (const std::exception &ex) {
                Log::write(QStringLiteral("addTrackers: %1").arg(QString::fromUtf8(ex.what())));
            }
        }

        emit status(existed ? tr("Torrent already exists. Loading...") : tr("Waiting for torrent..."));
        QString resolved = _client->resolveHash(
            _link.hash, before,
            qMax(8000, _cfg.requestTimeoutSec * 1000),
            [this]() { return wasCancelled(); });

        if (resolved.isEmpty()) {
            if (wasCancelled()) { emit prepareFinished(false, QString()); return; }
            if (!_link.hash.isEmpty())
                resolved = _link.hash;
            else
                throw QbtException(tr("Torrent not found."), 0, QString());
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
        resultOk = _client->forceStartVerified(hash, 5,
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

        if (!p.trackers.isEmpty()) {
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

        if (p.category != p.initialCategory) {
            emit status(tr("Setting category..."));
            try { _client->setCategory(p.hash, p.category); }
            catch (const QbtException &ex) { Log::write(QStringLiteral("setCategory: %1").arg(ex.message)); }
        }

        if (!p.tags.isEmpty()) {
            emit status(tr("Setting tags..."));
            try { _client->addTags(p.hash, p.tags); }
            catch (const QbtException &ex) { Log::write(QStringLiteral("addTags: %1").arg(ex.message)); }
        }

        if (!p.savePath.isEmpty()
            && p.savePath.compare(p.initialSavePath, Qt::CaseInsensitive) != 0) {
            emit status(tr("Setting save path..."));
            try { _client->setLocation(p.hash, p.savePath); }
            catch (const QbtException &ex) { Log::write(QStringLiteral("setLocation: %1").arg(ex.message)); }
        }

        if (p.forceStart) {
            emit status(tr("Force starting..."));
            resultOk = _client->forceStartVerified(p.hash, 5,
                [this](const QString &s) { emit status(s); });
        } else {
            emit status(tr("Starting..."));
            try { _client->setForceStart(p.hash, false); } catch (const std::exception &ex) { Log::write(QStringLiteral("reset forceStart: %1").arg(QString::fromUtf8(ex.what()))); }
            _client->startTorrent(p.hash);
            QThread::msleep(500);
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
