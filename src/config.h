#pragma once
#include <QString>

class Config {
public:
    QString host              = QStringLiteral("127.0.0.1");
    int     port              = 8080;
    bool    useHttps          = false;
    QString username;
    QString passwordEnc;
    QString password;
    QString authMode          = QStringLiteral("password");
    QString apiKeyEnc;
    QString apiKey;
    QString rutrackerPasskeyEnc;
    QString rutrackerPasskey;
    bool    quickMode         = false;
    int     forceStartDelayMs = 5000;
    int     metadataTimeoutSec = 120;
    int     requestTimeoutSec = 20;
    bool    forceStartDefault = true;
    bool    deleteOnCancel    = true;
    bool    autoCloseOnSuccess = false;
    int     autoCloseMs       = 2500;
    QString language;
    QString clientType        = QStringLiteral("auto");
    QString defaultCategory;
    QString defaultTags;
    QString contentLayout;
    QString treeHeaderState;

    // Monthly usage statistics ("yyyy-MM" of the accumulated counters).
    QString statsMonth;
    int     statsAdded  = 0;
    qint64  statsBytes  = 0;

    void    bumpStats(const QString &monthKey, int count, qint64 bytes);
    static void recordAdded(qint64 bytes);

    QString baseUrl() const;
    bool    isComplete() const;

    QString getPassword() const;
    void    setPassword(const QString &plain);

    QString getApiKey() const;
    void    setApiKey(const QString &plain);

    QString getRutrackerPasskey() const;
    void    setRutrackerPasskey(const QString &plain);

    static Config load();
    void save();

    static QString dirPath();
    static QString filePath();
};