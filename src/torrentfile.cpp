#include "torrentfile.h"
#include "magnetlink.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <stdexcept>
#include <QCoreApplication>

namespace {

class BencodeParser {
public:
    explicit BencodeParser(const QByteArray &data) : m_data(data), m_pos(0) {}

    struct Value {
        enum Type { Integer, String, List, Dict } type = Integer;
        qint64 intVal = 0;
        QByteArray strVal;
        QVector<Value> listVal;
        QVector<QPair<QByteArray, Value>> dictVal;

        int rawStart = 0;
        int rawEnd = 0;

        const Value *dictGet(const QByteArray &key) const {
            for (const auto &pair : dictVal) {
                if (pair.first == key)
                    return &pair.second;
            }
            return nullptr;
        }

        QString asUtf8() const {
            return QString::fromUtf8(strVal);
        }
    };

    Value parseRoot() {
        if (m_data.isEmpty())
            throw std::runtime_error(QCoreApplication::translate("TorrentFile", "Empty torrent data.").toStdString());
        Value root = parseValue(0);
        if (root.type != Value::Dict)
            throw std::runtime_error(QCoreApplication::translate("TorrentFile", "Invalid torrent file (root is not a dictionary).").toStdString());
        return root;
    }

private:
    const QByteArray &m_data;
    int m_pos;

    char peek() const {
        if (m_pos >= m_data.size())
            throw std::runtime_error(QCoreApplication::translate("TorrentFile", "Unexpected end of torrent data.").toStdString());
        return m_data.at(m_pos);
    }

    char get() {
        if (m_pos >= m_data.size())
            throw std::runtime_error(QCoreApplication::translate("TorrentFile", "Unexpected end of torrent data.").toStdString());
        return m_data.at(m_pos++);
    }

    Value parseValue(int depth) {
        if (depth > 64)
            throw std::runtime_error(QCoreApplication::translate("TorrentFile", "Torrent data nesting too deep.").toStdString());

        Value v;
        v.rawStart = m_pos;
        char ch = peek();

        if (ch == 'i') {
            v.type = Value::Integer;
            get();
            int end = m_data.indexOf('e', m_pos);
            if (end < 0)
                throw std::runtime_error(QCoreApplication::translate("TorrentFile", "Unterminated integer in torrent data.").toStdString());
            QByteArray numStr = m_data.mid(m_pos, end - m_pos);
            m_pos = end + 1;
            bool ok = false;
            v.intVal = numStr.toLongLong(&ok);
            if (!ok)
                throw std::runtime_error(QCoreApplication::translate("TorrentFile", "Invalid integer format in torrent data.").toStdString());
            v.rawEnd = m_pos;
            return v;
        }

        if (ch >= '0' && ch <= '9') {
            v.type = Value::String;
            int colon = m_data.indexOf(':', m_pos);
            if (colon < 0)
                throw std::runtime_error(QCoreApplication::translate("TorrentFile", "Invalid string format in torrent data.").toStdString());
            QByteArray lenStr = m_data.mid(m_pos, colon - m_pos);
            bool ok = false;
            int len = lenStr.toInt(&ok);
            if (!ok || len < 0 || len > m_data.size() - (colon + 1))
                throw std::runtime_error(QCoreApplication::translate("TorrentFile", "Invalid string length in torrent data.").toStdString());
            m_pos = colon + 1;
            v.strVal = m_data.mid(m_pos, len);
            m_pos += len;
            v.rawEnd = m_pos;
            return v;
        }

        if (ch == 'l') {
            v.type = Value::List;
            get();
            while (peek() != 'e') {
                v.listVal.append(parseValue(depth + 1));
            }
            get();
            v.rawEnd = m_pos;
            return v;
        }

        if (ch == 'd') {
            v.type = Value::Dict;
            get();
            while (peek() != 'e') {
                Value keyVal = parseValue(depth + 1);
                if (keyVal.type != Value::String)
                    throw std::runtime_error(QCoreApplication::translate("TorrentFile", "Invalid dictionary key in torrent data.").toStdString());
                Value val = parseValue(depth + 1);
                v.dictVal.append({ keyVal.strVal, val });
            }
            get();
            v.rawEnd = m_pos;
            return v;
        }

        throw std::runtime_error(QCoreApplication::translate("TorrentFile", "Corrupted torrent data token.").toStdString());
    }
};

} // namespace

QString TorrentFileData::prettyName() const
{
    if (!displayName.isEmpty())
        return displayName;
    if (!hash.isEmpty())
        return QStringLiteral("torrent ") + hash.left(16) + QStringLiteral("...");
    if (!filePath.isEmpty())
        return QFileInfo(filePath).fileName();
    return QStringLiteral("torrent file");
}

bool TorrentFileData::looksLikeTorrent(const QString &str)
{
    if (MagnetLink::looksLikeMagnet(str))
        return false;

    QString trimmed = str.trimmed();
    if (trimmed.length() > 1 && trimmed.front() == QLatin1Char('"') && trimmed.back() == QLatin1Char('"'))
        trimmed = trimmed.mid(1, trimmed.length() - 2).trimmed();

    if (trimmed.startsWith(QLatin1String("file://"), Qt::CaseInsensitive))
        trimmed = QUrl(trimmed).toLocalFile();

    if (trimmed.endsWith(QLatin1String(".torrent"), Qt::CaseInsensitive))
        return true;

    if (QFileInfo::exists(trimmed) && QFileInfo(trimmed).isFile()) {
        QFile f(trimmed);
        if (f.open(QIODevice::ReadOnly)) {
            char header = 0;
            if (f.read(&header, 1) == 1 && header == 'd')
                return true;
        }
    }
    return false;
}

TorrentFileData TorrentFileData::parse(const QByteArray &bytes, const QString &sourcePath)
{
    BencodeParser parser(bytes);
    auto root = parser.parseRoot();

    const auto *infoVal = root.dictGet("info");
    if (!infoVal || infoVal->type != BencodeParser::Value::Dict)
        throw std::runtime_error(QCoreApplication::translate("TorrentFile", "Missing 'info' dictionary in torrent data.").toStdString());

    TorrentFileData meta;
    meta.filePath = sourcePath;
    meta.rawData = bytes;

    QByteArray infoRaw = bytes.mid(infoVal->rawStart, infoVal->rawEnd - infoVal->rawStart);
    meta.hash = QString::fromLatin1(QCryptographicHash::hash(infoRaw, QCryptographicHash::Sha1).toHex()).toLower();
    meta.hashV2 = QString::fromLatin1(QCryptographicHash::hash(infoRaw, QCryptographicHash::Sha256).toHex()).toLower();

    const auto *nameUtf8 = infoVal->dictGet("name.utf-8");
    const auto *nameNormal = infoVal->dictGet("name");
    if (nameUtf8 && nameUtf8->type == BencodeParser::Value::String)
        meta.displayName = nameUtf8->asUtf8();
    else if (nameNormal && nameNormal->type == BencodeParser::Value::String)
        meta.displayName = nameNormal->asUtf8();

    const auto *comment = root.dictGet("comment");
    if (comment && comment->type == BencodeParser::Value::String)
        meta.comment = comment->asUtf8();

    const auto *lengthVal = infoVal->dictGet("length");
    const auto *filesVal = infoVal->dictGet("files");

    if (lengthVal && lengthVal->type == BencodeParser::Value::Integer) {
        TorrentFile tf;
        tf.index = 0;
        tf.name = meta.displayName.isEmpty() ? QStringLiteral("file") : meta.displayName;
        tf.size = lengthVal->intVal;
        tf.priority = 1;
        tf.progress = 0.0;
        meta.files.append(tf);
        meta.totalSize = tf.size;
    } else if (filesVal && filesVal->type == BencodeParser::Value::List) {
        int fileIndex = 0;
        for (const auto &fEntry : filesVal->listVal) {
            if (fEntry.type != BencodeParser::Value::Dict)
                continue;

            const auto *fLen = fEntry.dictGet("length");
            qint64 size = (fLen && fLen->type == BencodeParser::Value::Integer) ? fLen->intVal : 0;

            const auto *pathListVal = fEntry.dictGet("path.utf-8");
            if (!pathListVal || pathListVal->type != BencodeParser::Value::List)
                pathListVal = fEntry.dictGet("path");

            QStringList pathParts;
            if (pathListVal && pathListVal->type == BencodeParser::Value::List) {
                for (const auto &p : pathListVal->listVal) {
                    if (p.type == BencodeParser::Value::String)
                        pathParts.append(p.asUtf8());
                }
            }

            QString relativePath = pathParts.join(QLatin1Char('/'));
            if (relativePath.isEmpty())
                relativePath = QStringLiteral("file_%1").arg(fileIndex);

            QString fullPath = meta.displayName.isEmpty()
                ? relativePath
                : (meta.displayName + QLatin1Char('/') + relativePath);

            TorrentFile tf;
            tf.index = fileIndex++;
            tf.name = fullPath;
            tf.size = size;
            tf.priority = 1;
            tf.progress = 0.0;

            meta.files.append(tf);
            meta.totalSize += size;
        }
    }

    const auto *announce = root.dictGet("announce");
    if (announce && announce->type == BencodeParser::Value::String) {
        QString tr = announce->asUtf8().trimmed();
        if (!tr.isEmpty())
            meta.trackers.append(tr);
    }

    const auto *announceList = root.dictGet("announce-list");
    if (announceList && announceList->type == BencodeParser::Value::List) {
        for (const auto &tier : announceList->listVal) {
            if (tier.type == BencodeParser::Value::List) {
                for (const auto &trVal : tier.listVal) {
                    if (trVal.type == BencodeParser::Value::String) {
                        QString tr = trVal.asUtf8().trimmed();
                        if (!tr.isEmpty() && !meta.trackers.contains(tr))
                            meta.trackers.append(tr);
                    }
                }
            } else if (tier.type == BencodeParser::Value::String) {
                QString tr = tier.asUtf8().trimmed();
                if (!tr.isEmpty() && !meta.trackers.contains(tr))
                    meta.trackers.append(tr);
            }
        }
    }

    return meta;
}

TorrentFileData TorrentFileData::loadFromFile(const QString &filePath)
{
    QString path = filePath.trimmed();
    if (path.length() > 1 && path.front() == QLatin1Char('"') && path.back() == QLatin1Char('"'))
        path = path.mid(1, path.length() - 2).trimmed();

    if (path.startsWith(QLatin1String("file://"), Qt::CaseInsensitive))
        path = QUrl(path).toLocalFile();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error(QCoreApplication::translate("TorrentFile", "Cannot open file: %1").arg(file.errorString()).toStdString());

    QByteArray data = file.readAll();
    file.close();
    return parse(data, path);
}
