#include "passkey.h"

#include <QUrl>
#include <QUrlQuery>

namespace PassKey {

namespace {

// Query parameter names known to carry a per-user passkey. "pk" is
// rutracker's spelling; the rest are the common private-tracker parameter
// names seen across TBDev/Gazelle-style trackers.
const QStringList &paramNames()
{
    static const QStringList names = {
        QStringLiteral("pk"),
        QStringLiteral("passkey"),
        QStringLiteral("authkey"),
        QStringLiteral("torrent_pass"),
        QStringLiteral("key"),
        QStringLiteral("secure"),
        QStringLiteral("uk"),
    };
    return names;
}

} // namespace

bool isRutrackerHost(const QString &host)
{
    const QString h = host.toLower();
    return h == QLatin1String("t-ru.org") || h.endsWith(QLatin1String(".t-ru.org"));
}

static bool isPasskeyParam(const QString &name)
{
    for (const QString &n : paramNames())
        if (n.compare(name, Qt::CaseInsensitive) == 0)
            return true;
    return false;
}

QVector<Found> extract(const QStringList &trackers)
{
    QVector<Found> result;
    for (const QString &t : trackers) {
        const QUrl url(t);
        if (!url.isValid() || url.host().isEmpty())
            continue;
        const QUrlQuery q(url);
        const QList<QPair<QString, QString>> items = q.queryItems(QUrl::FullyDecoded);
        for (const auto &kv : items) {
            if (!isPasskeyParam(kv.first) || kv.second.trimmed().isEmpty())
                continue;
            Found f;
            f.host = url.host();
            f.param = kv.first.toLower();
            f.value = kv.second.trimmed();
            f.isRutracker = isRutrackerHost(f.host);
            result.append(f);
        }
    }
    return result;
}

QStringList applyRutrackerPasskey(const QStringList &trackers, const QString &passkey,
                                  int *changedCount)
{
    QStringList result;
    int changed = 0;
    for (const QString &t : trackers) {
        const QString rewritten = rewriteUrl(t, passkey);
        if (rewritten != t) {
            result.append(rewritten);
            ++changed;
        }
    }
    if (changedCount)
        *changedCount = changed;
    return result;
}

QString rewriteUrl(const QString &trackerUrl, const QString &passkey)
{
    if (passkey.isEmpty())
        return trackerUrl;
    QUrl url(trackerUrl);
    if (!url.isValid() || !isRutrackerHost(url.host()))
        return trackerUrl;

    QUrlQuery q(url);
    QString pkParam;
    QString pkValue;
    const QList<QPair<QString, QString>> items = q.queryItems(QUrl::FullyDecoded);
    for (const auto &kv : items) {
        if (kv.first.compare(QLatin1String("pk"), Qt::CaseInsensitive) == 0) {
            pkParam = kv.first;
            pkValue = kv.second;
            break;
        }
    }
    if (!pkParam.isEmpty() && pkValue == passkey)
        return trackerUrl;
    if (!pkParam.isEmpty())
        q.removeAllQueryItems(pkParam);
    q.addQueryItem(QStringLiteral("pk"), passkey);
    url.setQuery(q);
    return url.toString();
}

QString injectIntoMagnet(const QString &magnetUri, const QString &passkey)
{
    if (passkey.isEmpty())
        return magnetUri;
    const int qmark = magnetUri.indexOf(QLatin1Char('?'));
    if (qmark < 0 || !magnetUri.left(qmark).startsWith(QLatin1String("magnet"), Qt::CaseInsensitive))
        return magnetUri;

    const QStringList params = magnetUri.mid(qmark + 1).split(QLatin1Char('&'), Qt::SkipEmptyParts);
    QStringList out;
    bool changed = false;
    for (const QString &param : params) {
        const int eq = param.indexOf(QLatin1Char('='));
        if (eq > 0) {
            const QString key = param.left(eq).toLower();
            if (key == QLatin1String("tr") || key.startsWith(QLatin1String("tr."))) {
                const QString url = QUrl::fromPercentEncoding(param.mid(eq + 1).toLatin1());
                const QString rewritten = rewriteUrl(url, passkey);
                if (rewritten != url) {
                    out << QStringLiteral("tr=")
                               + QString::fromLatin1(QUrl::toPercentEncoding(rewritten));
                    changed = true;
                    continue;
                }
            }
        }
        out << param;
    }
    return changed ? magnetUri.left(qmark + 1) + out.join(QLatin1Char('&')) : magnetUri;
}

} // namespace PassKey
