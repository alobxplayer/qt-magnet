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
    bool    quickMode         = false;
    int     forceStartDelayMs = 5000;
    int     metadataTimeoutSec = 120;
    int     requestTimeoutSec = 20;
    bool    forceStartDefault = true;
    bool    deleteOnCancel    = true;
    bool    autoCloseOnSuccess = true;
    int     autoCloseMs       = 2500;
    QString language;
    QString clientType        = QStringLiteral("auto");
    QString defaultCategory;
    QString defaultTags;
    QString contentLayout;
    QString treeHeaderState;

    QString baseUrl() const;
    bool    isComplete() const;

    QString getPassword() const;
    void    setPassword(const QString &plain);

    static Config load();
    void save() const;

    static QString dirPath();
    static QString filePath();
};
