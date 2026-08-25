#ifdef HAVE_LIBSECRET
#pragma push_macro("signals")
#pragma push_macro("slots")
#pragma push_macro("emit")
#undef signals
#undef slots
#undef emit

#include <libsecret/secret.h>

#pragma pop_macro("emit")
#pragma pop_macro("slots")
#pragma pop_macro("signals")
#endif

#include "secretstore.h"
#include "logger.h"

#include <QByteArray>

namespace {

const QString kMarker = QStringLiteral("secret-service:v1");
const QString kB64Prefix = QStringLiteral("b64:");

QString fallbackStore(const QString &plain)
{
    static bool warned = false;
    if (!warned) {
        Log::write(QStringLiteral("SecretStore: system keyring unavailable, falling back to base64 encoding."));
        warned = true;
    }
    return kB64Prefix + QString::fromLatin1(plain.toUtf8().toBase64());
}

QString fallbackLoad(const QString &token)
{
    if (!token.startsWith(kB64Prefix))
        return QString();
    return QString::fromUtf8(QByteArray::fromBase64(token.mid(kB64Prefix.length()).toLatin1()));
}

#ifdef HAVE_LIBSECRET
const SecretSchema *schema()
{
    static const SecretSchema s = {
        "org.qt-magnet.Password", SECRET_SCHEMA_NONE,
        {
            { "application", SECRET_SCHEMA_ATTRIBUTE_STRING },
            { "account",     SECRET_SCHEMA_ATTRIBUTE_STRING },
            { nullptr,       SECRET_SCHEMA_ATTRIBUTE_STRING },
        },
        0, 0, 0, 0, 0, 0, 0, 0
    };
    return &s;
}
#endif

} // namespace

namespace SecretStore {

QString store(const QString &account, const QString &plain)
{
    if (plain.isEmpty())
        return QString();

#ifdef HAVE_LIBSECRET
    GError *err = nullptr;
    gboolean ok = secret_password_store_sync(
        schema(), SECRET_COLLECTION_DEFAULT, "qt-magnet WebUI password",
        plain.toUtf8().constData(), nullptr, &err,
        "application", "qt-magnet",
        "account", account.toUtf8().constData(),
        nullptr);
    if (err) {
        Log::write(QStringLiteral("SecretStore: error writing to keyring: %1").arg(QString::fromUtf8(err->message)));
        g_error_free(err);
    }
    if (ok)
        return kMarker;
    return fallbackStore(plain);
#else
    return fallbackStore(plain);
#endif
}

QString load(const QString &account, const QString &token)
{
    if (token.isEmpty())
        return QString();

    if (token == kMarker) {
#ifdef HAVE_LIBSECRET
        GError *err = nullptr;
        gchar *pw = secret_password_lookup_sync(
            schema(), nullptr, &err,
            "application", "qt-magnet",
            "account", account.toUtf8().constData(),
            nullptr);
        if (err) {
            Log::write(QStringLiteral("SecretStore: error reading from keyring: %1").arg(QString::fromUtf8(err->message)));
            g_error_free(err);
        }
        if (pw) {
            QString r = QString::fromUtf8(pw);
            secret_password_free(pw);
            return r;
        }
        return QString();
#else
        return QString();
#endif
    }

    return fallbackLoad(token);
}

void clear(const QString &account, const QString &token)
{
#ifdef HAVE_LIBSECRET
    if (token == kMarker) {
        GError *err = nullptr;
        secret_password_clear_sync(
            schema(), nullptr, &err,
            "application", "qt-magnet",
            "account", account.toUtf8().constData(),
            nullptr);
        if (err)
            g_error_free(err);
    }
#else
    Q_UNUSED(account)
    Q_UNUSED(token)
#endif
}

bool isSecure()
{
#ifdef HAVE_LIBSECRET
    return true;
#else
    return false;
#endif
}

} // namespace SecretStore
