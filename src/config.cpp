#include "config.h"
#include "logger.h"
#include "secretstore.h"

#include <QCoreApplication>
#include <QDate>
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

namespace {

// Credentials live in the OS secret store, referenced by an encrypted marker
// in config.json; `plain` is the pre-migration in-memory copy.
QString loadCredential(const QString &plain, const QString &enc, const QString &storeKey)
{
    if (!plain.isEmpty())
        return plain;
    if (enc.isEmpty())
        return QString();
    return SecretStore::load(storeKey, enc);
}

void storeCredential(QString *enc, const QString &plain, const QString &storeKey)
{
    if (plain.isEmpty()) {
        if (!enc->isEmpty())
            SecretStore::clear(storeKey, *enc);
        enc->clear();
        return;
    }
    *enc = SecretStore::store(storeKey, plain);
}

} // namespace

QString Config::getPassword() const
{
    return loadCredential(password, passwordEnc, username);
}

void Config::setPassword(const QString &plain)
{
    password.clear();
    storeCredential(&passwordEnc, plain, username);
}

QString Config::getApiKey() const
{
    return loadCredential(apiKey, apiKeyEnc, QStringLiteral("apikey"));
}

void Config::setApiKey(const QString &plain)
{
    apiKey.clear();
    storeCredential(&apiKeyEnc, plain, QStringLiteral("apikey"));
}

QString Config::getRutrackerPasskey() const
{
    return loadCredential(rutrackerPasskey, rutrackerPasskeyEnc, QStringLiteral("rutracker-passkey"));
}

void Config::setRutrackerPasskey(const QString &plain)
{
    rutrackerPasskey.clear();
    storeCredential(&rutrackerPasskeyEnc, plain, QStringLiteral("rutracker-passkey"));
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

void Config::bumpStats(const QString &monthKey, int count, qint64 bytes)
{
    if (statsMonth != monthKey) {
        statsMonth = monthKey;
        statsAdded = 0;
        statsBytes = 0;
    }
    statsAdded += count;
    statsBytes += bytes;
}

void Config::recordAdded(qint64 bytes)
{
    // Fresh load-modify-save: the live Config copy may be stale relative to
    // other writes (settings, header state).
    Config c = Config::load();
    c.bumpStats(QDate::currentDate().toString(QStringLiteral("yyyy-MM")), 1, bytes);
    try {
        c.save();
    } catch (const std::exception &ex) {
        // Statistics must never break the add flow; log and continue.
        Log::write(QStringLiteral("Failed to save stats: %1").arg(QString::fromUtf8(ex.what())));
    }
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
    c.authMode           = o.value(QStringLiteral("authMode")).toString(c.authMode);
    c.apiKeyEnc          = o.value(QStringLiteral("apiKeyEnc")).toString(c.apiKeyEnc);
    c.apiKey             = o.value(QStringLiteral("apiKey")).toString(c.apiKey);
    c.rutrackerPasskeyEnc = o.value(QStringLiteral("rutrackerPasskeyEnc")).toString(c.rutrackerPasskeyEnc);
    c.rutrackerPasskey    = o.value(QStringLiteral("rutrackerPasskey")).toString(c.rutrackerPasskey);
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
    c.statsMonth         = o.value(QStringLiteral("statsMonth")).toString();
    c.statsAdded         = qMax(0, o.value(QStringLiteral("statsAdded")).toInt());
    c.statsBytes         = qMax<qint64>(0, static_cast<qint64>(o.value(QStringLiteral("statsBytes")).toDouble()));
    return c;
}

void Config::save()
{
    // Migrate any plaintext credentials into the secret store; never persist
    // them in config.json.
    if (!password.isEmpty()) {
        passwordEnc = SecretStore::store(username, password);
        password.clear();
    }
    if (!apiKey.isEmpty()) {
        apiKeyEnc = SecretStore::store(QStringLiteral("apikey"), apiKey);
        apiKey.clear();
    }
    if (!rutrackerPasskey.isEmpty()) {
        rutrackerPasskeyEnc = SecretStore::store(QStringLiteral("rutracker-passkey"), rutrackerPasskey);
        rutrackerPasskey.clear();
    }

    QJsonObject o;
    o[QStringLiteral("host")]               = host;
    o[QStringLiteral("port")]               = port;
    o[QStringLiteral("useHttps")]           = useHttps;
    o[QStringLiteral("username")]           = username;
    o[QStringLiteral("passwordEnc")]        = passwordEnc;
    o[QStringLiteral("authMode")]           = authMode;
    o[QStringLiteral("apiKeyEnc")]          = apiKeyEnc;
    if (!rutrackerPasskeyEnc.isEmpty())
        o[QStringLiteral("rutrackerPasskeyEnc")] = rutrackerPasskeyEnc;
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
    if (statsAdded > 0) {
        o[QStringLiteral("statsMonth")] = statsMonth;
        o[QStringLiteral("statsAdded")] = statsAdded;
        o[QStringLiteral("statsBytes")] = static_cast<double>(statsBytes);
    }

    QDir().mkpath(dirPath());
    QSaveFile f(filePath());
    if (!f.open(QIODevice::WriteOnly))
        throw std::runtime_error(QCoreApplication::translate("Config", "Cannot write config.json: %1").arg(f.errorString()).toStdString());
    f.write(QJsonDocument(o).toJson());
    if (!f.commit())
        throw std::runtime_error(QCoreApplication::translate("Config", "Cannot commit config.json: %1").arg(f.errorString()).toStdString());
}