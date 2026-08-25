#include "magnetlink.h"

#include <QCoreApplication>
#include <QUrl>
#include <stdexcept>

namespace {

bool isHex(const QString &s)
{
    if (s.isEmpty())
        return false;
    for (QChar ch : s) {
        const ushort c = ch.unicode();
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!ok)
            return false;
    }
    return true;
}

QString unescape(const QString &s)
{
    return QUrl::fromPercentEncoding(s.toUtf8());
}

void takeExactTopic(MagnetLink &m, const QString &val)
{
    static const QString btih = QStringLiteral("urn:btih:");
    static const QString btmh = QStringLiteral("urn:btmh:");

    if (val.startsWith(btih, Qt::CaseInsensitive)) {
        const QString raw = val.mid(btih.length()).trimmed();
        QString hex;
        if (raw.length() == 40 && isHex(raw))
            hex = raw.toLower();
        else if (raw.length() == 32)
            hex = MagnetLink::base32ToHex(raw);
        if (!hex.isEmpty())
            m.hash = hex;
    } else if (val.startsWith(btmh, Qt::CaseInsensitive)) {
        const QString raw = val.mid(btmh.length()).trimmed().toLower();
        if (raw.startsWith(QLatin1String("1220")) && raw.length() >= 68 && isHex(raw)) {
            m.hashV2 = raw.mid(4, 64);
            if (m.hash.isEmpty())
                m.hash = m.hashV2.left(40);
        }
    }
}

} // namespace

QString MagnetLink::prettyName() const
{
    if (!displayName.isEmpty())
        return displayName;
    if (!hash.isEmpty())
        return QStringLiteral("magnet ") + hash.left(16) + QStringLiteral("...");
    return QStringLiteral("magnet link");
}

bool MagnetLink::looksLikeMagnet(const QString &s)
{
    QString trimmed = s.trimmed();
    if (trimmed.length() > 1 && trimmed.front() == QLatin1Char('"') && trimmed.back() == QLatin1Char('"'))
        trimmed = trimmed.mid(1, trimmed.length() - 2).trimmed();
    return trimmed.startsWith(QLatin1String("magnet:"), Qt::CaseInsensitive)
        || trimmed.startsWith(QLatin1String("magnet%3a"), Qt::CaseInsensitive);
}

MagnetLink MagnetLink::parse(const QString &uri)
{
    MagnetLink m;
    m.raw = uri.trimmed();

    QString s = m.raw;
    if (s.length() > 1 && s.front() == QLatin1Char('"') && s.back() == QLatin1Char('"'))
        s = s.mid(1, s.length() - 2).trimmed();

    if (s.startsWith(QLatin1String("magnet%3a"), Qt::CaseInsensitive))
        s = unescape(s);

    const int q = s.indexOf(QLatin1Char('?'));
    const QString query = q >= 0 ? s.mid(q + 1) : QString();

    const QStringList pairs = query.split(QLatin1Char('&'));
    for (const QString &pair : pairs) {
        if (pair.isEmpty())
            continue;
        const int eq = pair.indexOf(QLatin1Char('='));
        if (eq <= 0)
            continue;
        const QString key = pair.left(eq).toLower();
        QString rawVal = pair.mid(eq + 1);
        if (key == QLatin1String("dn")) {
            rawVal.replace(QLatin1Char('+'), QLatin1Char(' '));
        }
        const QString val = unescape(rawVal);

        if (key == QLatin1String("xt") || key.startsWith(QLatin1String("xt."))) {
            takeExactTopic(m, val);
        } else if (key == QLatin1String("dn") && m.displayName.isEmpty()) {
            m.displayName = val.trimmed();
        } else if (key == QLatin1String("tr") && !val.isEmpty()) {
            m.trackers.append(val);
        }
    }

    if (m.hash.isEmpty())
        throw std::runtime_error(QCoreApplication::translate("MagnetLink", "No infohash found in magnet link.").toStdString());
    return m;
}

static inline int base32Val(QChar ch)
{
    const ushort u = ch.unicode();
    if (u >= 'A' && u <= 'Z') return u - 'A';
    if (u >= 'a' && u <= 'z') return u - 'a';
    if (u >= '2' && u <= '7') return u - '2' + 26;
    return -1;
}

QString MagnetLink::base32ToHex(const QString &b32)
{
    QByteArray bytes;
    int buffer = 0;
    int bitsLeft = 0;
    for (QChar ch : b32.trimmed()) {
        if (ch == QLatin1Char('='))
            break;
        const int idx = base32Val(ch);
        if (idx < 0)
            return QString();
        buffer = (buffer << 5) | idx;
        bitsLeft += 5;
        if (bitsLeft >= 8) {
            bitsLeft -= 8;
            bytes.append(char((buffer >> bitsLeft) & 0xFF));
        }
    }
    if (bytes.size() != 20)
        return QString();

    return QString::fromLatin1(bytes.toHex());
}
