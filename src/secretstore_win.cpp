#include "secretstore.h"

#include <QByteArray>

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>

namespace {
QByteArray entropy()
{
    return QByteArrayLiteral("QtMagnet/v1");
}
}

namespace SecretStore {

QString store(const QString & /*account*/, const QString &plain)
{
    if (plain.isEmpty())
        return QString();

    QByteArray data = plain.toUtf8();
    QByteArray ent = entropy();

    DATA_BLOB in{ static_cast<DWORD>(data.size()), reinterpret_cast<BYTE *>(data.data()) };
    DATA_BLOB salt{ static_cast<DWORD>(ent.size()), reinterpret_cast<BYTE *>(ent.data()) };
    DATA_BLOB out{ 0, nullptr };

    if (!CryptProtectData(&in, nullptr, &salt, nullptr, nullptr, 0, &out))
        return QString();

    QByteArray blob(reinterpret_cast<const char *>(out.pbData), int(out.cbData));
    LocalFree(out.pbData);
    return QString::fromLatin1(blob.toBase64());
}

QString load(const QString & /*account*/, const QString &token)
{
    if (token.isEmpty())
        return QString();

    QByteArray blob = QByteArray::fromBase64(token.toLatin1());
    QByteArray ent = entropy();

    DATA_BLOB in{ static_cast<DWORD>(blob.size()), reinterpret_cast<BYTE *>(blob.data()) };
    DATA_BLOB salt{ static_cast<DWORD>(ent.size()), reinterpret_cast<BYTE *>(ent.data()) };
    DATA_BLOB out{ 0, nullptr };

    if (!CryptUnprotectData(&in, nullptr, &salt, nullptr, nullptr, 0, &out))
        return QString();

    QString plain = QString::fromUtf8(reinterpret_cast<const char *>(out.pbData), int(out.cbData));
    LocalFree(out.pbData);
    return plain;
}

void clear(const QString & /*account*/, const QString & /*token*/)
{
}

bool isSecure()
{
    return true;
}

} // namespace SecretStore
