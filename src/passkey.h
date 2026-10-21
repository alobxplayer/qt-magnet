#pragma once
#include <QString>
#include <QStringList>
#include <QVector>

// Passkey extraction from private-tracker announce URLs.
//
// Private trackers credit upload/download statistics to a per-user key
// embedded in the announce URL as a query parameter ("pk" on rutracker,
// "passkey"/"authkey"/"torrent_pass" and friends elsewhere). A torrent
// downloaded by someone else carries their key, and a bare magnet link
// carries none — in both cases the user's own upload goes uncounted.
namespace PassKey {

struct Found {
    QString host;               // tracker announce host, e.g. "bt.t-ru.org"
    QString param;              // query parameter carrying the passkey
    QString value;              // passkey value
    bool isRutracker = false;   // host belongs to the t-ru.org announcers
};

// Extract every passkey parameter found in tracker announce URLs.
QVector<Found> extract(const QStringList &trackers);

// True for rutracker announce hosts (bt.t-ru.org, bt2..bt4.t-ru.org,
// any *.t-ru.org); case-insensitive, suffix-spoof safe.
bool isRutrackerHost(const QString &host);

// Rewrite rutracker announce URLs to carry the user's passkey: a foreign
// "pk" value is replaced, a missing one is appended; other parameters are
// preserved. Returns only the changed URLs (changedCount receives their
// number). The caller keeps the originals: torrent clients merge trackers,
// so both announcements reach the swarm and the user's upload is counted
// on their account.
QStringList applyRutrackerPasskey(const QStringList &trackers, const QString &passkey,
                                  int *changedCount = nullptr);

// Rewrite a single tracker announce URL to carry the user's passkey: a
// foreign "pk" value is replaced, a missing one is appended, other
// parameters are preserved. Non-rutracker and already-own URLs are
// returned unchanged.
QString rewriteUrl(const QString &trackerUrl, const QString &passkey);

// Inject the passkey into every rutracker tr= parameter of a magnet URI
// (the magnet itself is modified, so the torrent enters the client with
// the user's passkey from the start). Returns the original string when
// nothing needed changing.
QString injectIntoMagnet(const QString &magnetUri, const QString &passkey);

} // namespace PassKey
