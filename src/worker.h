#pragma once
#include <QThread>
#include <QString>
#include <QVector>
#include <QMap>
#include <QList>
#include <atomic>
#include "config.h"
#include "qbtclient.h"
#include "torrentpayload.h"

class Worker : public QThread {
    Q_OBJECT
public:
    enum Task { Prepare, Apply, QuickFinish, Cleanup, Poll };

    Worker(const Config &cfg, const TorrentPayload &payload, bool quick, QObject *parent = nullptr);
    ~Worker() override;

    void setTask(Task task);
    void requestCancel();
    void resetCancel();
    bool wasCancelled() const;

    struct ApplyParams {
        QString hash;
        QMap<int, QList<int>> prioritiesByPrio;
        bool anyChanged = false;
        QString category;
        QString tags;
        QString savePath;
        bool forceStart = true;
        QString initialCategory;
        QString initialSavePath;
        QString initialTags;
        QStringList trackers;
    };
    ApplyParams applyParams;

    QString hash;
    bool weAdded = false;
    bool existed = false;
    QString initialCategory;
    QString initialSavePath;
    QString initialTags;
    QVector<TorrentFile> files;
    QVector<QPair<QString, QString>> categories;
    std::optional<TorrentInfo> torrentInfo;

    std::optional<TorrentInfo> pollInfo;
    QVector<TorrentFile> pollFiles;

    bool resultOk = false;

    QbtClient *client() { return _client; }

signals:
    void status(const QString &text);
    void prepareFinished(bool success, const QString &error);
    void applyFinished(bool success, const QString &error);
    void quickFinished(bool success, const QString &error);
    void cleanupFinished();
    void pollFinished(bool success);

protected:
    void run() override;

private:
    void doPrepare();
    void doApply();
    void doQuickFinish();
    void doCleanup();
    void doPoll();
    void sleepCancellable(int ms);

    Config _cfg;
    TorrentPayload _payload;
    bool _quick;
    Task _task = Prepare;
    QbtClient *_client = nullptr;
    std::atomic<bool> _cancelRequested{false};
    QbtClient::ClientType _detectedType = QbtClient::ClientType::Auto;
    QList<QNetworkCookie> _sessionCookies;
    QString _transmissionSessionId;
};
