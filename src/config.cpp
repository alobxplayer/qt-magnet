#include "config.h"
#include "logger.h"
#include "secretstore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <stdexcept>

QString Config::baseUrl() const
{
    const QString scheme = useHttps ? QStringLiteral("https") : QStringLiteral("http");
    return QStringLiteral("%1://%2:%3").arg(scheme, host, QString::number(port));
}

bool Config::isComplete() const
{
    return !host.isEmpty() && port > 0 && port < 65536;
}

QString Config::getPassword() const
{
    if (!password.isEmpty())
        return password;
    if (passwordEnc.isEmpty())
        return QString();
    return SecretStore::load(username, passwordEnc);
}

void Config::setPassword(const QString &plain)
{
    password.clear();
    if (plain.isEmpty()) {
        passwordEnc.clear();
        return;
    }
    passwordEnc = SecretStore::store(username, plain);
}

QString Config::dirPath()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (dir.isEmpty())
        dir = QDir::homePath() + QStringLiteral("/.qt-magnet");
    return dir;
}

QString Config::filePath()
{
    return dirPath() + QStringLiteral("/config.json");
}

Config Config::load()
{
    Config c;

    QFile f(filePath());
    if (!f.open(QIODevice::ReadOnly))
        return c;
    const QByteArray bytes = f.readAll();
    f.close();

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        Log::write(QStringLiteral("Config: JSON parse error: %1").arg(perr.errorString()));
        return c;
    }

    const QJsonObject o = doc.object();
    c.host               = o.value(QStringLiteral("host")).toString(c.host);
    c.port               = qBound(1, o.value(QStringLiteral("port")).toInt(c.port), 65535);
    c.useHttps           = o.value(QStringLiteral("useHttps")).toBool(c.useHttps);
    c.username           = o.value(QStringLiteral("username")).toString(c.username);
    c.passwordEnc        = o.value(QStringLiteral("passwordEnc")).toString(c.passwordEnc);
    c.password           = o.value(QStringLiteral("password")).toString(c.password);
    c.quickMode          = o.value(QStringLiteral("quickMode")).toBool(c.quickMode);
    c.forceStartDelayMs  = qBound(0, o.value(QStringLiteral("forceStartDelayMs")).toInt(c.forceStartDelayMs), 60000);
    c.metadataTimeoutSec = qBound(5, o.value(QStringLiteral("metadataTimeoutSec")).toInt(c.metadataTimeoutSec), 3600);
    c.requestTimeoutSec  = qBound(3, o.value(QStringLiteral("requestTimeoutSec")).toInt(c.requestTimeoutSec), 300);
    c.forceStartDefault  = o.value(QStringLiteral("forceStartDefault")).toBool(c.forceStartDefault);
    c.deleteOnCancel     = o.value(QStringLiteral("deleteOnCancel")).toBool(c.deleteOnCancel);
    c.autoCloseOnSuccess = o.value(QStringLiteral("autoCloseOnSuccess")).toBool(c.autoCloseOnSuccess);
    c.autoCloseMs        = qBound(500, o.value(QStringLiteral("autoCloseMs")).toInt(c.autoCloseMs), 60000);
    c.language           = o.value(QStringLiteral("language")).toString();
    c.clientType         = o.value(QStringLiteral("clientType")).toString(c.clientType);
    c.defaultCategory    = o.value(QStringLiteral("defaultCategory")).toString(c.defaultCategory);
    c.defaultTags        = o.value(QStringLiteral("defaultTags")).toString(c.defaultTags);
    c.contentLayout      = o.value(QStringLiteral("contentLayout")).toString(c.contentLayout);
    c.treeHeaderState    = o.value(QStringLiteral("treeHeaderState")).toString();
    return c;
}

void Config::save() const
{
    QJsonObject o;
    o[QStringLiteral("host")]               = host;
    o[QStringLiteral("port")]               = port;
    o[QStringLiteral("useHttps")]           = useHttps;
    o[QStringLiteral("username")]           = username;
    o[QStringLiteral("passwordEnc")]        = passwordEnc;
    if (!password.isEmpty())
        o[QStringLiteral("password")]       = password;
    o[QStringLiteral("quickMode")]          = quickMode;
    o[QStringLiteral("forceStartDelayMs")]  = forceStartDelayMs;
    o[QStringLiteral("metadataTimeoutSec")] = metadataTimeoutSec;
    o[QStringLiteral("requestTimeoutSec")]  = requestTimeoutSec;
    o[QStringLiteral("forceStartDefault")]  = forceStartDefault;
    o[QStringLiteral("deleteOnCancel")]     = deleteOnCancel;
    o[QStringLiteral("autoCloseOnSuccess")] = autoCloseOnSuccess;
    o[QStringLiteral("autoCloseMs")]        = autoCloseMs;
    o[QStringLiteral("language")]           = language;
    o[QStringLiteral("clientType")]         = clientType;
    o[QStringLiteral("defaultCategory")]    = defaultCategory;
    o[QStringLiteral("defaultTags")]        = defaultTags;
    o[QStringLiteral("contentLayout")]      = contentLayout;
    if (!treeHeaderState.isEmpty())
        o[QStringLiteral("treeHeaderState")] = treeHeaderState;

    QDir().mkpath(dirPath());
    QSaveFile f(filePath());
    if (!f.open(QIODevice::WriteOnly))
        throw std::runtime_error(QCoreApplication::translate("Config", "Cannot write config.json: %1").arg(f.errorString()).toStdString());
    f.write(QJsonDocument(o).toJson());
    if (!f.commit())
        throw std::runtime_error(QCoreApplication::translate("Config", "Cannot commit config.json: %1").arg(f.errorString()).toStdString());
}
