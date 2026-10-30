#include "torrentbackend.h"
#include "config.h"
#include "logger.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QEventLoop>
#include <QTimer>
#include <QUrl>

namespace {
// How often an in-flight request is checked for cancellation, and the
// floor for the request timeout when the config value is too small.
constexpr int kCancelPollMs = 100;
constexpr int kMinRequestTimeoutSec = 5;
}

QStringList splitTagList(const QString &tags)
{
    QStringList out;
    for (const QString &t : tags.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString trimmed = t.trimmed();
        if (!trimmed.isEmpty())
            out.append(trimmed);
    }
    return out;
}

ClientTransport::ClientTransport(const Config &cfg, std::function<void(const QString &)> log,
                     std::function<bool()> isCancelled)
    : cfg(cfg), _log(std::move(log)), _isCancelled(std::move(isCancelled)), base(cfg.baseUrl())
{
    if (!_log)
        _log = [](const QString &) {};
    nam = new QNetworkAccessManager();
    nam->setCookieJar(new QNetworkCookieJar(nam));
}

ClientTransport::~ClientTransport()
{
    delete nam;
}

void ClientTransport::trace(const QString &s) const
{
    _log(s);
    Log::write(s);
}

QString ClientTransport::esc(const QString &s)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(s));
}

QJsonDocument ClientTransport::parseJson(const QByteArray &body, const QString &source,
                                   bool requireObject) const
{
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &perr);
    if (perr.error != QJsonParseError::NoError || (requireObject && !doc.isObject()))
        throw ClientException(QCoreApplication::translate("TorrentClient", "Invalid JSON from %1: %2")
                               .arg(source, perr.errorString()), 0, QString::fromUtf8(body));
    return doc;
}

void ClientTransport::throwIfNoConnection(const RawResponse &r) const
{
    if (r.timedOut || (r.networkError != QNetworkReply::NoError && r.statusCode == 0))
        throw ClientException(QCoreApplication::translate("TorrentClient", "No connection to %1: %2")
                               .arg(base, r.errorString), 0, QString());
}

void ClientTransport::prepare(QNetworkRequest &req) const
{
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("QtMagnet/1.0"));
    req.setRawHeader("Referer", base.toUtf8());
    req.setRawHeader("Origin", base.toUtf8());
    if (cfg.authMode == QLatin1String("apikey") && !cfg.getApiKey().isEmpty()) {
        const QByteArray keyBytes = cfg.getApiKey().toUtf8();
        req.setRawHeader("X-Api-Key", keyBytes);
        req.setRawHeader("Authorization", "Bearer " + keyBytes);
    }
}

void ClientTransport::setBasicAuth(QNetworkRequest &req) const
{
    const QString pass = cfg.getPassword();
    if (!cfg.username.isEmpty() || !pass.isEmpty()) {
        const QString creds = cfg.username + QLatin1Char(':') + pass;
        req.setRawHeader("Authorization", "Basic " + creds.toUtf8().toBase64());
    }
}

void ClientTransport::resetSession()
{
    if (nam && nam->cookieJar()) {
        nam->cookieJar()->deleteLater();
        nam->setCookieJar(new QNetworkCookieJar(nam));
    }
}

ClientTransport::RawResponse ClientTransport::execRaw(const std::function<QNetworkReply *()> &make)
{
    QNetworkReply *reply = make();

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

    QTimer timer;
    timer.setSingleShot(true);
    bool timedOut = false;
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        timedOut = true;
        reply->abort();
    });
    timer.start(qMax(kMinRequestTimeoutSec, cfg.requestTimeoutSec) * 1000);

    QTimer cancelTimer;
    if (_isCancelled) {
        cancelTimer.setInterval(kCancelPollMs);
        QObject::connect(&cancelTimer, &QTimer::timeout, &loop, [&] {
            if (_isCancelled())
                reply->abort();
        });
        cancelTimer.start();
    }

    if (!reply->isFinished())
        loop.exec();
    timer.stop();
    cancelTimer.stop();

    RawResponse resp;
    resp.body = reply->readAll();
    resp.networkError = static_cast<int>(reply->error());
    resp.statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    resp.errorString = reply->errorString();
    resp.timedOut = timedOut;
    resp.rawHeaders = reply->rawHeaderPairs();
    reply->deleteLater();

    if (isCancelled())
        throw ClientException(QCoreApplication::translate("TorrentClient", "Operation cancelled."), 0, QString());

    return resp;
}

bool ClientTransport::hasSessionCookie() const
{
    if (!nam || !nam->cookieJar())
        return false;
    const QList<QNetworkCookie> cookies = nam->cookieJar()->cookiesForUrl(QUrl(base));
    for (const QNetworkCookie &c : cookies) {
        if (c.name().compare(QByteArrayLiteral("SID"), Qt::CaseInsensitive) == 0 && !c.value().isEmpty())
            return true;
    }
    return false;
}

QList<QNetworkCookie> ClientTransport::cookies() const
{
    if (!nam || !nam->cookieJar())
        return {};
    return nam->cookieJar()->cookiesForUrl(QUrl(base));
}

void ClientTransport::setCookies(const QList<QNetworkCookie> &cookies)
{
    if (!nam || !nam->cookieJar() || cookies.isEmpty())
        return;
    nam->cookieJar()->setCookiesFromUrl(cookies, QUrl(base));
}

// --------------------------------------------------- default backend ops ----

[[noreturn]] void TorrentBackend::throwUnsupported(const char *operation) const
{
    throw ClientException(QCoreApplication::translate("TorrentClient", "%1 is not supported by %2.")
                           .arg(QString::fromLatin1(operation), backendName()), 0, QString());
}

void TorrentBackend::setCategory(const QString &hash, const QString &category)
{
    Q_UNUSED(hash);
    Q_UNUSED(category);
    throwUnsupported("Setting the category");
}

void TorrentBackend::addTags(const QString &hash, const QString &tags)
{
    Q_UNUSED(hash);
    Q_UNUSED(tags);
    throwUnsupported("Adding tags");
}

void TorrentBackend::removeTags(const QString &hash, const QString &tags)
{
    Q_UNUSED(hash);
    Q_UNUSED(tags);
    throwUnsupported("Removing tags");
}

void TorrentBackend::setTags(const QString &hash, const QString &newTags, const QString &oldTags)
{
    Q_UNUSED(hash);
    Q_UNUSED(newTags);
    Q_UNUSED(oldTags);
    throwUnsupported("Setting tags");
}

void TorrentBackend::addTrackers(const QString &hash, const QStringList &trackers)
{
    Q_UNUSED(hash);
    Q_UNUSED(trackers);
    throwUnsupported("Adding trackers");
}

void TorrentBackend::removeTrackers(const QString &hash, const QStringList &trackers)
{
    Q_UNUSED(hash);
    Q_UNUSED(trackers);
    throwUnsupported("Removing trackers");
}

QStringList TorrentBackend::torrentTrackers(const QString &hash)
{
    Q_UNUSED(hash);
    throwUnsupported("Listing trackers");
}

void TorrentBackend::replaceTracker(const QString &hash, const QString &origUrl, const QString &newUrl)
{
    Q_UNUSED(hash);
    Q_UNUSED(origUrl);
    Q_UNUSED(newUrl);
    throwUnsupported("Tracker rewrite");
}

void TorrentBackend::rename(const QString &hash, const QString &name)
{
    Q_UNUSED(hash);
    Q_UNUSED(name);
    throwUnsupported("Renaming");
}

void TorrentBackend::setAutoManagement(const QString &hash, bool enable)
{
    Q_UNUSED(hash);
    Q_UNUSED(enable);
    throwUnsupported("Automatic torrent management");
}

void TorrentBackend::toggleSequentialDownload(const QString &hash)
{
    Q_UNUSED(hash);
    throwUnsupported("Sequential download");
}

void TorrentBackend::toggleFirstLastPiecePrio(const QString &hash)
{
    Q_UNUSED(hash);
    throwUnsupported("First/last piece priority");
}
