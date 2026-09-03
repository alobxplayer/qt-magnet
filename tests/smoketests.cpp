#include <iostream>
#include <cstdlib>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QThread>
#include <QEventLoop>

#include "config.h"
#include "format.h"
#include "logger.h"
#include "magnetlink.h"
#include "torrentfile.h"
#include "torrentpayload.h"
#include "secretstore.h"
#include "qbtclient.h"
#include "worker.h"
#include "mocktorrentserver.h"

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const QString &label)
{
    if (cond) {
        ++g_pass;
    } else {
        ++g_fail;
        std::cerr << "FAIL: " << label.toStdString() << std::endl;
    }
}

static void eq(const QString &got, const QString &want, const QString &label)
{
    check(got == want, label + QStringLiteral(" (got '%1', want '%2')").arg(got, want));
}

static bool isHex(const QString &s)
{
    if (s.isEmpty())
        return false;
    for (QChar ch : s) {
        ushort c = ch.unicode();
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

struct ConfigSandboxGuard {
    QString cfgPath;
    QByteArray originalBytes;
    bool existed = false;
    ConfigSandboxGuard() {
        cfgPath = Config::filePath();
        if (QFile::exists(cfgPath)) {
            QFile f(cfgPath);
            if (f.open(QIODevice::ReadOnly)) {
                originalBytes = f.readAll();
                existed = true;
                f.close();
            }
        }
    }
    ~ConfigSandboxGuard() {
        restore();
    }
    void restore() {
        if (existed) {
            QFile f(cfgPath);
            if (f.open(QIODevice::WriteOnly)) {
                f.write(originalBytes);
                f.close();
            }
        } else {
            QFile::remove(cfgPath);
        }
    }
};

static void runUnitTests()
{
    auto m1 = MagnetLink::parse(QStringLiteral(
        "magnet:?xt=urn:btih:c12fe1c06bba254a9dc9f519b335aa7c1367a88a&dn=Ubuntu+ISO&tr=udp%3A%2F%2Ftr1&tr=udp%3A%2F%2Ftr2"));
    eq(m1.hash, QStringLiteral("c12fe1c06bba254a9dc9f519b335aa7c1367a88a"), QStringLiteral("v1 hex hash"));
    eq(m1.displayName, QStringLiteral("Ubuntu ISO"), QStringLiteral("dn decoded"));
    check(m1.trackers.size() == 2, QStringLiteral("two trackers"));

    auto mPlus = MagnetLink::parse(QStringLiteral(
        "magnet:?xt=urn:btih:c12fe1c06bba254a9dc9f519b335aa7c1367a88a&dn=C%2B%2B+Primer"));
    eq(mPlus.displayName, QStringLiteral("C++ Primer"), QStringLiteral("dn + and %2B decoded correctly"));

    auto m2 = MagnetLink::parse(QStringLiteral("magnet:?xt=urn:btih:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"));
    eq(m2.hash, QStringLiteral("0000000000000000000000000000000000000000"), QStringLiteral("base32 -> hex (zeros)"));

    auto m3 = MagnetLink::parse(QStringLiteral("magnet:?xt=urn:btih:MFRGGZDFMZTWQ2LKNNWG23TPMFRGGZDF"));
    check(m3.hash.length() == 40 && isHex(m3.hash), QStringLiteral("base32 32chars -> 40 hex"));

    QString v2full = QStringLiteral("0000000000000000000000000000000000000000000000000000000000000001");
    auto m4 = MagnetLink::parse(QStringLiteral("magnet:?xt=urn:btmh:1220") + v2full);
    eq(m4.hashV2, v2full, QStringLiteral("v2 full hash"));
    eq(m4.hash, v2full, QStringLiteral("v2 hash without truncation"));

    auto m5 = MagnetLink::parse(
        QStringLiteral("magnet:?xt=urn:btih:c12fe1c06bba254a9dc9f519b335aa7c1367a88a&xt=urn:btmh:1220") + v2full);
    eq(m5.hash, QStringLiteral("c12fe1c06bba254a9dc9f519b335aa7c1367a88a"), QStringLiteral("v1 priority"));
    eq(m5.hashV2, v2full, QStringLiteral("v2 parsed"));

    auto m6 = MagnetLink::parse(QStringLiteral("\"magnet:?xt=urn:btih:c12fe1c06bba254a9dc9f519b335aa7c1367a88a\""));
    eq(m6.hash, QStringLiteral("c12fe1c06bba254a9dc9f519b335aa7c1367a88a"), QStringLiteral("quotes stripped"));

    auto mPercent = MagnetLink::parse(QStringLiteral("magnet%3A%3Fxt%3Durn%3Abtih%3Ac12fe1c06bba254a9dc9f519b335aa7c1367a88a%26dn%3DUbuntu%2BISO"));
    eq(mPercent.hash, QStringLiteral("c12fe1c06bba254a9dc9f519b335aa7c1367a88a"), QStringLiteral("percent-encoded magnet parsed"));
    eq(mPercent.displayName, QStringLiteral("Ubuntu ISO"), QStringLiteral("percent-encoded dn parsed"));

    auto m7 = MagnetLink::parse(QStringLiteral("magnet:?xt=urn:btih:C12FE1C06BBA254A9DC9F519B335AA7C1367A88A"));
    eq(m7.hash, QStringLiteral("c12fe1c06bba254a9dc9f519b335aa7c1367a88a"), QStringLiteral("hex lowercase"));
    check(isHex(QStringLiteral("C12FE1C06BBA254A9DC9F519B335AA7C1367A88A")), QStringLiteral("isHex uppercase check"));

    bool threw = false;
    try {
        MagnetLink::parse(QStringLiteral("magnet:?dn=only-name"));
    } catch (...) {
        threw = true;
    }
    check(threw, QStringLiteral("no xt -> exception"));

    check(MagnetLink::looksLikeMagnet(QStringLiteral("  magnet:?xt=...")), QStringLiteral("looksLikeMagnet with spaces"));
    check(MagnetLink::looksLikeMagnet(QStringLiteral("magnet%3A%3Fxt%3D...")), QStringLiteral("looksLikeMagnet percent-encoded"));
    check(MagnetLink::looksLikeMagnet(QStringLiteral("\"magnet:?xt=...\"")), QStringLiteral("looksLikeMagnet quoted"));
    check(!MagnetLink::looksLikeMagnet(QStringLiteral("http://x")), QStringLiteral("looksLikeMagnet: http -> false"));
    check(!MagnetLink::looksLikeMagnet(QStringLiteral("")), QStringLiteral("looksLikeMagnet: empty -> false"));

    check(m1.prettyName() == QStringLiteral("Ubuntu ISO"), QStringLiteral("prettyName from dn"));
    auto mNoName = MagnetLink::parse(QStringLiteral("magnet:?xt=urn:btih:c12fe1c06bba254a9dc9f519b335aa7c1367a88a"));
    check(mNoName.prettyName().startsWith(QStringLiteral("magnet ")), QStringLiteral("prettyName from hash"));

    eq(MagnetLink::base32ToHex(QStringLiteral("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA")),
       QStringLiteral("0000000000000000000000000000000000000000"), QStringLiteral("base32ToHex zeros"));
    eq(MagnetLink::base32ToHex(QStringLiteral("mfrggzdfmztwq2lknnwg23tpmfrggzdf")),
       MagnetLink::base32ToHex(QStringLiteral("MFRGGZDFMZTWQ2LKNNWG23TPMFRGGZDF")), QStringLiteral("base32ToHex case-insensitive"));
    check(MagnetLink::base32ToHex(QStringLiteral("SHORT")).isEmpty(), QStringLiteral("base32ToHex invalid length -> empty"));
    check(MagnetLink::base32ToHex(QStringLiteral("11111111111111111111111111111111")).isEmpty(), QStringLiteral("base32ToHex '1' invalid -> empty"));

    eq(Format::size(-1), QStringLiteral("?"), QStringLiteral("Format::size -1 -> ?"));
    check(!Format::size(0).isEmpty(), QStringLiteral("Format::size 0"));
    check(!Format::size(1024).isEmpty(), QStringLiteral("Format::size 1 KiB"));
    check(!Format::size(1048576).isEmpty(), QStringLiteral("Format::size 1 MiB"));
    check(Format::size(5368709120LL).contains(QStringLiteral("GiB")), QStringLiteral("Format::size 5 GiB (>2GB 32-bit boundary)"));
    check(Format::size(53687091200LL).contains(QStringLiteral("GiB")), QStringLiteral("Format::size 50 GiB"));
    check(Format::priorityName(0) == QStringLiteral("Do not download"), QStringLiteral("Format::priorityName 0"));
    check(Format::priorityName(1) == QStringLiteral("Normal"), QStringLiteral("Format::priorityName 1"));
    check(Format::priorityName(6) == QStringLiteral("High"), QStringLiteral("Format::priorityName 6"));
    check(Format::priorityName(7) == QStringLiteral("Maximum"), QStringLiteral("Format::priorityName 7"));

    Config cfg;
    cfg.host = QStringLiteral("10.0.0.5");
    cfg.port = 8080;
    cfg.username = QStringLiteral("admin");
    cfg.language = QStringLiteral("en_US");
    cfg.setPassword(QStringLiteral("s3cr3t-password"));
    check(!cfg.passwordEnc.isEmpty(), QStringLiteral("SecretStore: passwordEnc populated"));
    eq(cfg.getPassword(), QStringLiteral("s3cr3t-password"), QStringLiteral("SecretStore: decrypted matched"));
    eq(cfg.language, QStringLiteral("en_US"), QStringLiteral("Config: language preserved"));
    check(cfg.isComplete(), QStringLiteral("isComplete=true on full config"));
    eq(cfg.baseUrl(), QStringLiteral("http://10.0.0.5:8080"), QStringLiteral("baseUrl http"));
    cfg.useHttps = true;
    eq(cfg.baseUrl(), QStringLiteral("https://10.0.0.5:8080"), QStringLiteral("baseUrl https"));

    Config empty;
    empty.host.clear();
    empty.username.clear();
    check(!empty.isComplete(), QStringLiteral("isComplete=false on empty config"));

    // --- Bencode & Torrent File Tests ---
    QByteArray singleTorrentBytes = QByteArrayLiteral(
        "d8:announce18:http://tr1.org/ann13:announce-listll18:http://tr1.org/annel18:http://tr2.org/annee7:comment11:Sample File4:infod6:lengthi1048576e4:name8:test.iso12:piece lengthi16384e6:pieces0:ee");
    auto tf1 = TorrentFileData::parse(singleTorrentBytes, QStringLiteral("/tmp/test.torrent"));
    check(!tf1.hash.isEmpty() && tf1.hash.length() == 40 && isHex(tf1.hash), QStringLiteral("Torrent: single-file SHA1 infohash"));
    eq(tf1.displayName, QStringLiteral("test.iso"), QStringLiteral("Torrent: single-file displayName"));
    eq(tf1.comment, QStringLiteral("Sample File"), QStringLiteral("Torrent: single-file comment"));
    check(tf1.totalSize == 1048576, QStringLiteral("Torrent: single-file totalSize"));
    check(tf1.files.size() == 1, QStringLiteral("Torrent: single-file files count == 1"));
    if (tf1.files.size() == 1) {
        eq(tf1.files[0].name, QStringLiteral("test.iso"), QStringLiteral("Torrent: single-file file name"));
        check(tf1.files[0].size == 1048576, QStringLiteral("Torrent: single-file file size"));
    }
    check(tf1.trackers.size() == 2, QStringLiteral("Torrent: single-file trackers list"));

    QByteArray multiTorrentBytes = QByteArrayLiteral(
        "d8:announce18:http://tr1.org/ann4:infod5:filesld6:lengthi1024e4:pathl8:docs.txteed6:lengthi2048e4:pathl3:sub9:image.pngeee4:name10:my_project12:piece lengthi16384e6:pieces0:ee");
    auto tf2 = TorrentFileData::parse(multiTorrentBytes);
    check(!tf2.hash.isEmpty() && tf2.hash.length() == 40 && isHex(tf2.hash), QStringLiteral("Torrent: multi-file SHA1 infohash"));
    eq(tf2.displayName, QStringLiteral("my_project"), QStringLiteral("Torrent: multi-file displayName"));
    check(tf2.files.size() == 2, QStringLiteral("Torrent: multi-file files count == 2"));
    if (tf2.files.size() == 2) {
        eq(tf2.files[0].name, QStringLiteral("my_project/docs.txt"), QStringLiteral("Torrent: multi-file file 0 path"));
        check(tf2.files[0].size == 1024, QStringLiteral("Torrent: multi-file file 0 size"));
        eq(tf2.files[1].name, QStringLiteral("my_project/sub/image.png"), QStringLiteral("Torrent: multi-file file 1 path"));
        check(tf2.files[1].size == 2048, QStringLiteral("Torrent: multi-file file 1 size"));
    }
    check(tf2.totalSize == 3072, QStringLiteral("Torrent: multi-file totalSize sum"));

    check(TorrentFileData::looksLikeTorrent(QStringLiteral("test.torrent")), QStringLiteral("looksLikeTorrent .torrent"));
    check(TorrentFileData::looksLikeTorrent(QStringLiteral("\"C:/My Torrents/sample.TORRENT\"")), QStringLiteral("looksLikeTorrent quoted uppercase"));
    check(TorrentFileData::looksLikeTorrent(QStringLiteral("file:///home/user/test.torrent")), QStringLiteral("looksLikeTorrent file://"));
    check(!TorrentFileData::looksLikeTorrent(QStringLiteral("magnet:?xt=urn:btih:...")), QStringLiteral("looksLikeTorrent magnet -> false"));

    // --- Pure BitTorrent v2 (BEP 52) Torrent ---
    QByteArray v2TorrentBytes = QByteArrayLiteral(
        "d8:announce18:http://tr1.org/ann"
        "4:infod"
          "9:file treed"
            "4:docs"
              "d8:read.txtd0:d6:lengthi1024e11:pieces root32:01234567890123456789012345678901eee"
            "8:test.isod0:d6:lengthi2048e11:pieces root32:abcdefabcdefabcdefabcdefabcdefabeee"
          "e"
          "12:meta versioni2e"
          "4:name10:v2_torrent"
          "12:piece lengthi16384e"
        "ee"
    );
    auto tfV2 = TorrentFileData::parse(v2TorrentBytes, QStringLiteral("/tmp/v2.torrent"));
    check(tfV2.version == TorrentVersion::V2, QStringLiteral("TorrentFileData: pure v2 detected"));
    eq(tfV2.versionString(), QStringLiteral("v2"), QStringLiteral("TorrentFileData: versionString v2"));
    check(tfV2.files.size() == 2, QStringLiteral("TorrentFileData: 2 files extracted from file tree"));
    if (tfV2.files.size() == 2) {
        eq(tfV2.files[0].name, QStringLiteral("docs/read.txt"), QStringLiteral("v2 file 0 path"));
        check(tfV2.files[0].size == 1024, QStringLiteral("v2 file 0 size"));
        eq(tfV2.files[1].name, QStringLiteral("test.iso"), QStringLiteral("v2 file 1 path"));
        check(tfV2.files[1].size == 2048, QStringLiteral("v2 file 1 size"));
    }
    check(tfV2.totalSize == 3072, QStringLiteral("TorrentFileData: v2 totalSize matches"));
    check(!tfV2.hashV2.isEmpty() && tfV2.hashV2.length() == 64 && isHex(tfV2.hashV2), QStringLiteral("TorrentFileData: 64-char SHA256 hashV2"));
    eq(tfV2.hash, tfV2.hashV2, QStringLiteral("TorrentFileData: v2 primary hash equals hashV2"));

    // --- Hybrid v1+v2 with BEP 47/52 padding files ---
    QByteArray hybridBytes = QByteArrayLiteral(
        "d8:announce18:http://tr1.org/ann"
        "4:infod"
          "9:file treed"
            "4:docs"
              "d8:read.txtd0:d6:lengthi1024e11:pieces root32:01234567890123456789012345678901eee"
            "4:.padd9:pad_16384d0:d4:attr1:p6:lengthi15360eeee"
          "e"
          "12:meta versioni2e"
          "4:name14:hybrid_torrent"
          "12:piece lengthi16384e"
          "6:pieces20:11111111111111111111"
        "ee"
    );
    auto tfHybrid = TorrentFileData::parse(hybridBytes);
    check(tfHybrid.version == TorrentVersion::Hybrid, QStringLiteral("TorrentFileData: hybrid detected"));
    eq(tfHybrid.versionString(), QStringLiteral("Hybrid (v1+v2)"), QStringLiteral("TorrentFileData: versionString hybrid"));
    check(tfHybrid.files.size() == 1, QStringLiteral("TorrentFileData: padding file filtered out"));
    check(tfHybrid.totalSize == 1024, QStringLiteral("TorrentFileData: totalSize excludes padding"));
    check(!tfHybrid.hash.isEmpty() && tfHybrid.hash.length() == 40, QStringLiteral("TorrentFileData: hybrid v1 hash 40 chars"));
    check(!tfHybrid.hashV2.isEmpty() && tfHybrid.hashV2.length() == 64, QStringLiteral("TorrentFileData: hybrid v2 hash 64 chars"));
    check(!tfHybrid.toMagnetUri().isEmpty(), QStringLiteral("TorrentFileData: toMagnetUri generates valid magnet"));

    // --- BEP 53 select-only parsing in MagnetLink ---
    auto soSet = MagnetLink::parseSelectOnly(QStringLiteral("0, 2-4, 7, invalid, 10-8"));
    check(soSet.size() == 5, QStringLiteral("BEP 53 parseSelectOnly size == 5"));
    check(soSet.contains(0) && soSet.contains(2) && soSet.contains(3) && soSet.contains(4) && soSet.contains(7),
          QStringLiteral("BEP 53 contains valid parsed indices"));

    auto mSo = MagnetLink::parse(QStringLiteral("magnet:?xt=urn:btih:c12fe1c06bba254a9dc9f519b335aa7c1367a88a&so=1,3-5"));
    check(mSo.selectedIndices.size() == 4, QStringLiteral("MagnetLink so parsed into selectedIndices"));
    check(mSo.selectedIndices.contains(1) && mSo.selectedIndices.contains(3) && mSo.selectedIndices.contains(4) && mSo.selectedIndices.contains(5),
          QStringLiteral("mSo selectedIndices verified"));
    eq(mSo.versionString(), QStringLiteral("v1"), QStringLiteral("mSo versionString v1"));


    auto pMag = TorrentPayload::fromMagnet(QStringLiteral("magnet:?xt=urn:btih:c12fe1c06bba254a9dc9f519b335aa7c1367a88a&dn=Payload+Test"));
    check(pMag.isMagnet() && !pMag.isFile(), QStringLiteral("TorrentPayload fromMagnet kind"));
    eq(pMag.hash(), QStringLiteral("c12fe1c06bba254a9dc9f519b335aa7c1367a88a"), QStringLiteral("TorrentPayload magnet hash"));
    eq(pMag.displayName(), QStringLiteral("Payload Test"), QStringLiteral("TorrentPayload magnet displayName"));

    bool corrupt1 = false;
    try { TorrentFileData::parse(QByteArray()); } catch (...) { corrupt1 = true; }
    check(corrupt1, QStringLiteral("Torrent: empty bytes throws"));

    bool corrupt2 = false;
    try { TorrentFileData::parse(QByteArrayLiteral("i42e")); } catch (...) { corrupt2 = true; }
    check(corrupt2, QStringLiteral("Torrent: root integer throws"));

    bool corrupt3 = false;
    try { TorrentFileData::parse(QByteArrayLiteral("d4:spam4:eggse")); } catch (...) { corrupt3 = true; }
    check(corrupt3, QStringLiteral("Torrent: missing info dict throws"));

    bool corruptOverflow = false;
    try { TorrentFileData::parse(QByteArrayLiteral("d4:info2147483640:abce")); } catch (...) { corruptOverflow = true; }
    check(corruptOverflow, QStringLiteral("Torrent: integer overflow string length throws safely"));

    check(!TorrentFileData::looksLikeTorrent(QStringLiteral("magnet:?xt=urn:btih:c12fe1c06bba254a9dc9f519b335aa7c1367a88a&dn=file.torrent")),
          QStringLiteral("Torrent: magnet with .torrent dn is not a torrent file"));
}

static void runQBittorrentTests(MockTorrentServer &server)
{
    server.reset();

    Config cfg;
    cfg.host = server.host();
    cfg.port = server.port();
    cfg.username = QStringLiteral("admin");
    cfg.setPassword(QStringLiteral("adminpass"));
    cfg.clientType = QStringLiteral("qbittorrent");
    cfg.requestTimeoutSec = 5;

    QbtClient client(cfg, [](const QString &){});

    server.setAuthSuccess(false);
    bool authFailed = false;
    try {
        client.login();
    } catch (const QbtException &) {
        authFailed = true;
    }
    check(authFailed, QStringLiteral("qBt: login with wrong password fails"));

    server.setAuthSuccess(true);
    bool authOk = false;
    try {
        client.login();
        authOk = true;
    } catch (...) {}
    check(authOk, QStringLiteral("qBt: login succeeds"));
    check(client.detectedType == QbtClient::ClientType::QBittorrent, QStringLiteral("qBt: detectedType is QBittorrent"));

    client.fetchServerInfo();
    eq(client.appVersion, QStringLiteral("v4.6.5"), QStringLiteral("qBt: appVersion parsed"));
    eq(client.apiVersion, QStringLiteral("2.9.3"), QStringLiteral("qBt: apiVersion parsed"));
    eq(client.defaultSavePath(), QStringLiteral("C:/Downloads"), QStringLiteral("qBt: defaultSavePath"));

    QJsonObject pref = client.preferences();
    check(!pref.isEmpty(), QStringLiteral("qBt: preferences not empty"));
    check(client.serverAddsStopped, QStringLiteral("qBt: serverAddsStopped populated"));

    auto cats = client.categories();
    check(cats.size() >= 2, QStringLiteral("qBt: categories list parsed"));
    auto tags = client.tags();
    check(tags.size() >= 3, QStringLiteral("qBt: tags list parsed"));

    QString magnet = QStringLiteral("magnet:?xt=urn:btih:c12fe1c06bba254a9dc9f519b335aa7c1367a88a&dn=Ubuntu+24.04+Desktop&tr=udp%3A%2F%2Ftracker.test.org%3A80");
    client.addMagnet(magnet, true, false,
                     QStringLiteral("C:/Downloads/Linux"), QStringLiteral("Linux"),
                     QStringLiteral("iso, distro"), QStringLiteral("Original"));

    QSet<QString> hashes = client.allHashes();
    QString hash = QStringLiteral("c12fe1c06bba254a9dc9f519b335aa7c1367a88a");
    check(hashes.contains(hash), QStringLiteral("qBt: allHashes contains added torrent"));

    auto info = client.infoOne(hash);
    check(info.has_value(), QStringLiteral("qBt: infoOne returns torrent info"));
    if (info) {
        eq(info->name, QStringLiteral("Ubuntu 24.04 Desktop"), QStringLiteral("qBt: torrent name"));
        eq(info->category, QStringLiteral("Linux"), QStringLiteral("qBt: torrent category"));
        eq(info->tags, QStringLiteral("iso, distro"), QStringLiteral("qBt: torrent tags"));
        check(info->isStopped(), QStringLiteral("qBt: torrent is stopped/paused"));
    }

    auto files = client.files(hash);
    check(files.size() == 2, QStringLiteral("qBt: files list has 2 files"));
    client.filePrio(hash, {0}, 6);
    client.filePrio(hash, {1}, 0);

    files = client.files(hash);
    if (files.size() == 2) {
        check(files[0].priority == 6, QStringLiteral("qBt: file 0 priority changed to 6"));
        check(files[1].priority == 0, QStringLiteral("qBt: file 1 priority changed to 0"));
    }

    client.setCategory(hash, QStringLiteral("Movies"));
    client.addTags(hash, QStringLiteral("verified"));
    client.addTrackers(hash, {QStringLiteral("udp://tracker2.test.org:6969")});
    client.rename(hash, QStringLiteral("Ubuntu Linux 24.04"));
    client.setLocation(hash, QStringLiteral("C:/Torrents/Linux"));
    client.setAutoManagement(hash, true);
    client.setDownloadLimit(hash, 5242880);
    client.setUploadLimit(hash, 1048576);
    client.toggleSequentialDownload(hash);
    client.toggleFirstLastPiecePrio(hash);

    info = client.infoOne(hash);
    if (info) {
        eq(info->category, QStringLiteral("Movies"), QStringLiteral("qBt: setCategory verified"));
        eq(info->name, QStringLiteral("Ubuntu Linux 24.04"), QStringLiteral("qBt: rename verified"));
        eq(info->savePath, QStringLiteral("C:/Torrents/Linux"), QStringLiteral("qBt: setLocation verified"));
        check(info->autoTmm, QStringLiteral("qBt: setAutoManagement verified"));
        check(info->sequentialDownload, QStringLiteral("qBt: toggleSequentialDownload verified"));
        check(info->firstLastPiecePrio, QStringLiteral("qBt: toggleFirstLastPiecePrio verified"));
    }

    client.startTorrent(hash);
    info = client.infoOne(hash);
    check(info && !info->isStopped(), QStringLiteral("qBt: startTorrent verified"));

    client.stopTorrent(hash);
    info = client.infoOne(hash);
    check(info && info->isStopped(), QStringLiteral("qBt: stopTorrent verified"));

    bool forceOk = client.forceStartVerified(hash, 2, nullptr);
    check(forceOk, QStringLiteral("qBt: forceStartVerified succeeds"));
    info = client.infoOne(hash);
    check(info && (info->forceStart || info->isForced()), QStringLiteral("qBt: forceStart active in info"));

    client.deleteTorrent(hash, true);
    check(!client.allHashes().contains(hash), QStringLiteral("qBt: deleteTorrent removed torrent"));

    client.addMagnet(magnet, false, false, QString(), QString(), QString(), QString());
    check(client.allHashes().contains(hash), QStringLiteral("qBt: added torrent for re-login test"));
    server.setSessionValid(false);
    bool reloginOk = false;
    try {
        auto reInfo = client.infoOne(hash);
        reloginOk = reInfo.has_value();
    } catch (...) {}
    check(reloginOk, QStringLiteral("qBt: 403 triggers transparent re-login and request retry"));

    QByteArray sampleTorrent = QByteArrayLiteral(
        "d8:announce18:http://tr1.org/ann4:infod6:lengthi5000000e4:name15:file_sample.iso12:piece lengthi16384e6:pieces0:ee");
    auto tfParsed = TorrentFileData::parse(sampleTorrent);
    client.addTorrentFile(sampleTorrent, QStringLiteral("file_sample.iso.torrent"), true, false,
                          QStringLiteral("C:/Downloads/ISOs"), QStringLiteral("Linux"),
                          QStringLiteral("torrentfile, test"), QStringLiteral("Original"));
    check(client.allHashes().contains(tfParsed.hash), QStringLiteral("qBt: addTorrentFile adds torrent to server"));
    auto fileInfo = client.infoOne(tfParsed.hash);
    check(fileInfo.has_value(), QStringLiteral("qBt: addTorrentFile info retrieved"));
    if (fileInfo) {
        eq(fileInfo->name, QStringLiteral("file_sample.iso"), QStringLiteral("qBt: addTorrentFile name verified"));
        check(fileInfo->isStopped(), QStringLiteral("qBt: addTorrentFile is stopped"));
    }
}

static void runTransmissionTests(MockTorrentServer &server)
{
    server.reset();

    Config cfg;
    cfg.host = server.host();
    cfg.port = server.port();
    cfg.username = QStringLiteral("transmission");
    cfg.setPassword(QStringLiteral("tr_pass"));
    cfg.clientType = QStringLiteral("transmission");
    cfg.requestTimeoutSec = 5;

    QbtClient client(cfg, [](const QString &){});

    bool loginOk = false;
    try {
        client.login();
        loginOk = true;
    } catch (...) {}
    check(loginOk, QStringLiteral("Transmission: login with 409 CSRF handshake succeeds"));
    check(client.detectedType == QbtClient::ClientType::Transmission, QStringLiteral("Transmission: detectedType is Transmission"));
    check(client.appVersion.contains(QStringLiteral("Transmission")), QStringLiteral("Transmission: appVersion parsed"));
    check(client.apiVersion.contains(QStringLiteral("RPC")), QStringLiteral("Transmission: apiVersion parsed"));
    eq(client.defaultSavePath(), QStringLiteral("C:/Downloads"), QStringLiteral("Transmission: defaultSavePath"));

    QString magnet = QStringLiteral("magnet:?xt=urn:btih:c12fe1c06bba254a9dc9f519b335aa7c1367a88a&dn=Debian+NetInst");
    client.addMagnet(magnet, true, false, QStringLiteral("/downloads/debian"), QStringLiteral("Linux"), QStringLiteral("debian, netinst"), QString());

    QString hash = QStringLiteral("c12fe1c06bba254a9dc9f519b335aa7c1367a88a");
    QSet<QString> hashes = client.allHashes();
    check(hashes.contains(hash), QStringLiteral("Transmission: allHashes contains added torrent"));

    auto info = client.infoOne(hash);
    check(info.has_value(), QStringLiteral("Transmission: infoOne retrieved"));
    if (info) {
        eq(info->name, QStringLiteral("Debian NetInst"), QStringLiteral("Transmission: name parsed"));
        check(info->isStopped(), QStringLiteral("Transmission: paused state"));
    }

    auto files = client.files(hash);
    check(!files.isEmpty(), QStringLiteral("Transmission: files retrieved"));
    client.filePrio(hash, {0}, 6);
    files = client.files(hash);
    if (!files.isEmpty()) {
        check(files[0].priority == 6, QStringLiteral("Transmission: file priority high applied"));
    }
    client.filePrio(hash, {0}, 0);
    files = client.files(hash);
    if (!files.isEmpty()) {
        check(files[0].priority == 0, QStringLiteral("Transmission: file priority unwanted applied"));
    }

    client.setLocation(hash, QStringLiteral("/new/download/path"));
    client.addTags(hash, QStringLiteral("tagA, tagB"));
    client.rename(hash, QStringLiteral("Debian 12 NetInst"));
    client.setDownloadLimit(hash, 2048000);
    client.setUploadLimit(hash, 1024000);
    client.toggleSequentialDownload(hash);

    info = client.infoOne(hash);
    if (info) {
        eq(info->savePath, QStringLiteral("/new/download/path"), QStringLiteral("Transmission: location updated"));
        eq(info->name, QStringLiteral("Debian 12 NetInst"), QStringLiteral("Transmission: rename updated"));
    }

    client.startTorrent(hash);
    info = client.infoOne(hash);
    check(info && !info->isStopped(), QStringLiteral("Transmission: startTorrent updated state"));

    client.stopTorrent(hash);
    info = client.infoOne(hash);
    check(info && info->isStopped(), QStringLiteral("Transmission: stopTorrent updated state"));

    bool forceOk = client.forceStartVerified(hash, 2, nullptr);
    check(forceOk, QStringLiteral("Transmission: forceStartVerified succeeds"));

    client.deleteTorrent(hash, true);
    check(!client.allHashes().contains(hash), QStringLiteral("Transmission: deleteTorrent removed torrent"));

    QByteArray sampleTrTorrent = QByteArrayLiteral(
        "d8:announce18:http://tr1.org/ann4:infod6:lengthi7000000e4:name16:tr_file_test.iso12:piece lengthi16384e6:pieces0:ee");
    auto tfTrParsed = TorrentFileData::parse(sampleTrTorrent);
    client.addTorrentFile(sampleTrTorrent, QStringLiteral("tr_file_test.iso.torrent"), true, false,
                          QStringLiteral("/downloads/tr"), QStringLiteral("ISOs"),
                          QStringLiteral("tr_tag"), QString());
    check(client.allHashes().contains(tfTrParsed.hash), QStringLiteral("Transmission: addTorrentFile adds torrent to server"));
    auto trFileInfo = client.infoOne(tfTrParsed.hash);
    check(trFileInfo.has_value(), QStringLiteral("Transmission: addTorrentFile info retrieved"));
    if (trFileInfo) {
        eq(trFileInfo->name, QStringLiteral("tr_file_test.iso"), QStringLiteral("Transmission: addTorrentFile name verified"));
        check(trFileInfo->isStopped(), QStringLiteral("Transmission: addTorrentFile is stopped"));
    }
}

static void runAria2Tests(MockTorrentServer &server)
{
    server.reset();

    Config cfg;
    cfg.host = server.host();
    cfg.port = server.port();
    cfg.setPassword(QStringLiteral("mock-aria2-token"));
    cfg.clientType = QStringLiteral("aria2");
    cfg.requestTimeoutSec = 5;

    QbtClient client(cfg, [](const QString &){});

    bool loginOk = false;
    try {
        client.login();
        loginOk = true;
    } catch (...) {}
    check(loginOk, QStringLiteral("Aria2: login with token succeeds"));
    check(client.detectedType == QbtClient::ClientType::Aria2, QStringLiteral("Aria2: detectedType is Aria2"));
    check(client.appVersion.contains(QStringLiteral("Aria2")), QStringLiteral("Aria2: appVersion parsed"));
    eq(client.defaultSavePath(), QStringLiteral("C:/Downloads"), QStringLiteral("Aria2: defaultSavePath"));

    QString magnet = QStringLiteral("magnet:?xt=urn:btih:c12fe1c06bba254a9dc9f519b335aa7c1367a88a&dn=Arch+Linux+ISO");
    client.addMagnet(magnet, true, false, QStringLiteral("C:/Downloads/Arch"), QString(), QString(), QString());

    QString hash = QStringLiteral("c12fe1c06bba254a9dc9f519b335aa7c1367a88a");
    QSet<QString> hashes = client.allHashes();
    check(hashes.contains(hash), QStringLiteral("Aria2: allHashes contains added torrent"));

    auto info = client.infoOne(hash);
    check(info.has_value(), QStringLiteral("Aria2: infoOne retrieved"));
    if (info) {
        eq(info->name, QStringLiteral("Arch Linux ISO"), QStringLiteral("Aria2: name parsed"));
        check(info->isStopped(), QStringLiteral("Aria2: paused state"));
    }

    auto files = client.files(hash);
    check(!files.isEmpty(), QStringLiteral("Aria2: files retrieved"));
    client.filePrio(hash, {0}, 1);
    files = client.files(hash);
    if (!files.isEmpty()) {
        check(files[0].priority == 1, QStringLiteral("Aria2: file priority selected"));
    }

    client.setLocation(hash, QStringLiteral("C:/Torrents/Arch"));
    client.setDownloadLimit(hash, 4000000);
    client.setUploadLimit(hash, 2000000);

    client.startTorrent(hash);
    info = client.infoOne(hash);
    check(info && !info->isStopped(), QStringLiteral("Aria2: startTorrent unpauses"));

    client.stopTorrent(hash);
    info = client.infoOne(hash);
    check(info && info->isStopped(), QStringLiteral("Aria2: stopTorrent pauses"));

    bool forceOk = client.forceStartVerified(hash, 2, nullptr);
    check(forceOk, QStringLiteral("Aria2: forceStartVerified succeeds"));

    client.deleteTorrent(hash, true);
    check(!client.allHashes().contains(hash), QStringLiteral("Aria2: deleteTorrent removed torrent"));

    QByteArray sampleAria2Torrent = QByteArrayLiteral(
        "d8:announce18:http://tr1.org/ann4:infod6:lengthi9000000e4:name19:aria2_file_test.iso12:piece lengthi16384e6:pieces0:ee");
    auto tfAria2Parsed = TorrentFileData::parse(sampleAria2Torrent);
    client.addTorrentFile(sampleAria2Torrent, QStringLiteral("aria2_file_test.iso.torrent"), true, false,
                          QStringLiteral("C:/Downloads/Aria2"), QString(),
                          QString(), QString());
    check(client.allHashes().contains(tfAria2Parsed.hash), QStringLiteral("Aria2: addTorrentFile adds torrent to server"));
    auto aria2FileInfo = client.infoOne(tfAria2Parsed.hash);
    check(aria2FileInfo.has_value(), QStringLiteral("Aria2: addTorrentFile info retrieved"));
    if (aria2FileInfo) {
        eq(aria2FileInfo->name, QStringLiteral("aria2_file_test.iso"), QStringLiteral("Aria2: addTorrentFile name verified"));
        check(aria2FileInfo->isStopped(), QStringLiteral("Aria2: addTorrentFile is stopped"));
    }
}

static void runAutoDetectTests(MockTorrentServer &sQbt, MockTorrentServer &sTr, MockTorrentServer &sAria2)
{
    {
        Config cfg;
        cfg.host = sQbt.host();
        cfg.port = sQbt.port();
        cfg.username = QStringLiteral("admin");
        cfg.setPassword(QStringLiteral("pass"));
        cfg.clientType = QStringLiteral("auto");
        cfg.requestTimeoutSec = 3;

        QbtClient client(cfg, [](const QString &){});
        client.login();
        check(client.detectedType == QbtClient::ClientType::QBittorrent, QStringLiteral("Auto-detect: detected qBittorrent"));
    }

    {
        Config cfg;
        cfg.host = sTr.host();
        cfg.port = sTr.port();
        cfg.username = QStringLiteral("tr_user");
        cfg.setPassword(QStringLiteral("tr_pass"));
        cfg.clientType = QStringLiteral("auto");
        cfg.requestTimeoutSec = 3;

        QbtClient client(cfg, [](const QString &){});
        client.login();
        check(client.detectedType == QbtClient::ClientType::Transmission, QStringLiteral("Auto-detect: fallback to Transmission"));
    }

    {
        Config cfg;
        cfg.host = sAria2.host();
        cfg.port = sAria2.port();
        cfg.setPassword(QStringLiteral("mock-aria2-token"));
        cfg.clientType = QStringLiteral("auto");
        cfg.requestTimeoutSec = 3;

        QbtClient client(cfg, [](const QString &){});
        client.login();
        check(client.detectedType == QbtClient::ClientType::Aria2, QStringLiteral("Auto-detect: fallback to Aria2"));
    }
}

static void runWorkerTests(MockTorrentServer &server)
{
    server.reset();

    Config cfg;
    cfg.host = server.host();
    cfg.port = server.port();
    cfg.username = QStringLiteral("admin");
    cfg.setPassword(QStringLiteral("adminpass"));
    cfg.clientType = QStringLiteral("qbittorrent");
    cfg.requestTimeoutSec = 5;
    cfg.metadataTimeoutSec = 5;
    cfg.forceStartDelayMs = 0;

    auto link = TorrentPayload::fromMagnet(QStringLiteral("magnet:?xt=urn:btih:c12fe1c06bba254a9dc9f519b335aa7c1367a88a&dn=Worker+Test"));

    {
        Worker worker(cfg, link, false);
        worker.setTask(Worker::Prepare);

        bool finished = false;
        bool success = false;
        QObject::connect(&worker, &Worker::prepareFinished, [&](bool ok, const QString &) {
            finished = true;
            success = ok;
        });

        worker.start();
        while (worker.isRunning()) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QThread::msleep(10);
        }
        worker.wait();

        check(finished && success, QStringLiteral("Worker: Prepare finished with success"));
        check(worker.hash == QStringLiteral("c12fe1c06bba254a9dc9f519b335aa7c1367a88a"), QStringLiteral("Worker: hash resolved"));
        check(worker.files.size() == 2, QStringLiteral("Worker: metadata files populated"));
    }

    {
        Worker worker(cfg, link, false);
        worker.setTask(Worker::Apply);
        worker.applyParams.hash = QStringLiteral("c12fe1c06bba254a9dc9f519b335aa7c1367a88a");
        worker.applyParams.category = QStringLiteral("Linux");
        worker.applyParams.tags = QStringLiteral("worker-tag");
        worker.applyParams.forceStart = true;
        worker.applyParams.anyChanged = true;
        worker.applyParams.prioritiesByPrio[6] = {0};

        bool finished = false;
        bool success = false;
        QObject::connect(&worker, &Worker::applyFinished, [&](bool ok, const QString &) {
            finished = true;
            success = ok;
        });

        worker.start();
        while (worker.isRunning()) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QThread::msleep(10);
        }
        worker.wait();

        check(finished && success, QStringLiteral("Worker: Apply finished with success"));
    }

    {
        Worker worker(cfg, link, true);
        worker.hash = QStringLiteral("c12fe1c06bba254a9dc9f519b335aa7c1367a88a");
        worker.setTask(Worker::QuickFinish);

        bool finished = false;
        bool success = false;
        QObject::connect(&worker, &Worker::quickFinished, [&](bool ok, const QString &) {
            finished = true;
            success = ok;
        });

        worker.start();
        while (worker.isRunning()) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QThread::msleep(10);
        }
        worker.wait();

        check(finished && success, QStringLiteral("Worker: QuickFinish force-started torrent"));
    }

    {
        Worker worker(cfg, link, false);
        worker.hash = QStringLiteral("c12fe1c06bba254a9dc9f519b335aa7c1367a88a");
        worker.setTask(Worker::Cleanup);

        bool finished = false;
        QObject::connect(&worker, &Worker::cleanupFinished, [&]() {
            finished = true;
        });

        worker.start();
        while (worker.isRunning()) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QThread::msleep(10);
        }
        worker.wait();

        check(finished, QStringLiteral("Worker: Cleanup deleted torrent"));
        check(!server.torrents().contains(worker.hash), QStringLiteral("Worker: torrent removed from mock server"));
    }

    // Worker test with TorrentPayload from file
    {
        QByteArray sampleBytes = QByteArrayLiteral(
            "d8:announce18:http://tr1.org/ann4:infod6:lengthi8000000e4:name17:worker_sample.iso12:piece lengthi16384e6:pieces0:ee");
        TorrentPayload filePayload;
        filePayload.kind = TorrentPayload::Kind::TorrentFile;
        filePayload.torrentData = TorrentFileData::parse(sampleBytes, QStringLiteral("worker_sample.torrent"));

        Worker worker(cfg, filePayload, false);
        worker.setTask(Worker::Prepare);

        bool finished = false;
        bool success = false;
        QObject::connect(&worker, &Worker::prepareFinished, [&](bool ok, const QString &) {
            finished = true;
            success = ok;
        });

        worker.start();
        while (worker.isRunning()) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QThread::msleep(10);
        }
        worker.wait();

        check(finished && success, QStringLiteral("Worker: Prepare with torrent file succeeded"));
        check(!worker.hash.isEmpty(), QStringLiteral("Worker: hash populated from torrent file"));
        check(!worker.files.isEmpty(), QStringLiteral("Worker: files populated immediately for torrent file"));
    }
}

static void runQBittorrentApiKeyTests(MockTorrentServer &server)
{
    server.reset();

    Config cfg;
    cfg.host = server.host();
    cfg.port = server.port();
    cfg.authMode = QStringLiteral("apikey");
    cfg.apiKey = QStringLiteral("mock_api_key_secret");
    cfg.clientType = QStringLiteral("qbittorrent");
    cfg.requestTimeoutSec = 5;

    QbtClient client(cfg, [](const QString &){});

    bool authOk = false;
    try {
        client.login();
        authOk = true;
    } catch (...) {}
    check(authOk, QStringLiteral("qBt API Key: login with valid API key succeeds"));

    client.fetchServerInfo();
    check(!client.appVersion.isEmpty(), QStringLiteral("qBt API Key: fetchServerInfo succeeds with API key"));

    cfg.apiKey = QStringLiteral("wrong_api_key");
    QbtClient badClient(cfg, [](const QString &){});
    bool badAuth = false;
    try {
        badClient.login();
    } catch (const QbtException &) {
        badAuth = true;
    }
    check(badAuth, QStringLiteral("qBt API Key: login with wrong API key fails"));
}

static void runDualHashResolutionTests(MockTorrentServer &server)
{
    server.reset();

    QString v1Hash = QStringLiteral("c12fe1c06bba254a9dc9f519b335aa7c1367a88a");
    QString v2Hash = QStringLiteral("0000000000000000000000000000000000000000000000000000000000000001");

    MockTorrentData td;
    td.hash = v2Hash;
    td.infoHashV1 = v1Hash;
    td.infoHashV2 = v2Hash;
    td.name = QStringLiteral("Hybrid Dual Hash Test");
    td.state = QStringLiteral("downloading");
    server.addPreloadedTorrent(td);

    Config cfg;
    cfg.host = server.host();
    cfg.port = server.port();
    cfg.username = QStringLiteral("admin");
    cfg.setPassword(QStringLiteral("adminpass"));
    cfg.clientType = QStringLiteral("qbittorrent");
    cfg.requestTimeoutSec = 5;

    QbtClient client(cfg, [](const QString &){});
    client.login();

    QString resolved = client.resolveHash(v1Hash, {}, 3000, nullptr, v2Hash);
    eq(resolved, v2Hash, QStringLiteral("Dual-hash: resolveHash finds torrent by dual-hash"));

    QSet<QString> allH = client.allHashes();
    check(allH.contains(v1Hash), QStringLiteral("Dual-hash: allHashes contains v1 hash"));
    check(allH.contains(v2Hash), QStringLiteral("Dual-hash: allHashes contains v2 hash"));
}

static void runTagSyncTests(MockTorrentServer &server)
{
    server.reset();

    QString hash = QStringLiteral("c12fe1c06bba254a9dc9f519b335aa7c1367a88a");
    MockTorrentData td;
    td.hash = hash;
    td.name = QStringLiteral("Tag Sync Test");
    td.tags = QStringLiteral("tag1, tag2");
    td.state = QStringLiteral("downloading");
    server.addPreloadedTorrent(td);

    Config cfg;
    cfg.host = server.host();
    cfg.port = server.port();
    cfg.username = QStringLiteral("admin");
    cfg.setPassword(QStringLiteral("adminpass"));
    cfg.clientType = QStringLiteral("qbittorrent");
    cfg.requestTimeoutSec = 5;

    QbtClient client(cfg, [](const QString &){});
    client.login();

    client.setTags(hash, QStringLiteral("tag2, tag3"), QStringLiteral("tag1, tag2"));

    auto info = client.infoOne(hash);
    check(info.has_value(), QStringLiteral("Tag Sync: infoOne retrieved"));
    if (info) {
        check(info->tags.contains(QStringLiteral("tag2")), QStringLiteral("Tag Sync: tag2 preserved"));
        check(info->tags.contains(QStringLiteral("tag3")), QStringLiteral("Tag Sync: tag3 added"));
        check(!info->tags.contains(QStringLiteral("tag1")), QStringLiteral("Tag Sync: tag1 removed"));
    }
}

static void runAria2V2RejectionAndHybridFallbackTests(MockTorrentServer &server)
{
    server.reset();

    Config cfg;
    cfg.host = server.host();
    cfg.port = server.port();
    cfg.setPassword(QStringLiteral("mock-aria2-token"));
    cfg.clientType = QStringLiteral("aria2");
    cfg.requestTimeoutSec = 5;

    QbtClient client(cfg, [](const QString &){});
    client.login();

    QString pureV2Magnet = QStringLiteral("magnet:?xt=urn:btmh:12200000000000000000000000000000000000000000000000000000000000000001&dn=PureV2");
    bool v2MagnetRejected = false;
    try {
        client.addMagnet(pureV2Magnet, false, false, QString(), QString(), QString(), QString());
    } catch (const QbtException &ex) {
        v2MagnetRejected = ex.message.contains(QStringLiteral("BitTorrent v2"), Qt::CaseInsensitive);
    }
    check(v2MagnetRejected, QStringLiteral("Aria2: Pure v2 magnet rejected with informative message"));

    QByteArray pureV2Torrent = QByteArrayLiteral(
        "d8:announce18:http://tr1.org/ann4:infod9:file tree9:file1.txtd0:d6:lengthi1048576e11:pieces root32:00000000000000000000000000000001eee12:meta versioni2e4:name13:purev2torrent12:piece lengthi16384ee");
    bool v2TorrentRejected = false;
    try {
        client.addTorrentFile(pureV2Torrent, QStringLiteral("purev2.torrent"), false, false, QString(), QString(), QString(), QString());
    } catch (const QbtException &ex) {
        v2TorrentRejected = ex.message.contains(QStringLiteral("BitTorrent v2"), Qt::CaseInsensitive);
    }
    check(v2TorrentRejected, QStringLiteral("Aria2: Pure v2 .torrent file rejected with informative message"));

    QByteArray hybridTorrent = QByteArrayLiteral(
        "d8:announce18:http://tr1.org/ann4:infod6:lengthi1048576e4:name10:hybrid.iso12:piece lengthi16384e6:pieces20:123456789012345678909:file tree10:hybrid.isod0:d6:lengthi1048576e11:pieces root32:00000000000000000000000000000001eee12:meta versioni2eee");
    bool hybridFallbackOk = false;
    try {
        client.addTorrentFile(hybridTorrent, QStringLiteral("hybrid.torrent"), false, false, QString(), QString(), QString(), QString());
        hybridFallbackOk = true;
    } catch (...) {}
    check(hybridFallbackOk, QStringLiteral("Aria2: Hybrid .torrent file converted to v1 magnet fallback and added"));

    QString v1Magnet = QStringLiteral("magnet:?xt=urn:btih:d12fe1c06bba254a9dc9f519b335aa7c1367a88a&dn=ImmediateGidTest");
    client.addMagnet(v1Magnet, false, false, QString(), QString(), QString(), QString());
    QSet<QString> hashes = client.allHashes();
    check(hashes.contains(QStringLiteral("d12fe1c06bba254a9dc9f519b335aa7c1367a88a")),
          QStringLiteral("Aria2: Magnet infohash mapped to GID immediately on addMagnet"));
}

static void runConfigAndSecretStoreTests()
{
    ConfigSandboxGuard guard;

    Config cfg;
    cfg.host = QStringLiteral("127.0.0.1");
    cfg.port = 9090;
    cfg.username = QStringLiteral("testuser");
    cfg.setPassword(QStringLiteral("mysecretpassword"));
    cfg.authMode = QStringLiteral("apikey");
    cfg.setApiKey(QStringLiteral("mysecretapikey"));

    eq(cfg.getPassword(), QStringLiteral("mysecretpassword"), QStringLiteral("Config: getPassword returns cleartext"));
    eq(cfg.getApiKey(), QStringLiteral("mysecretapikey"), QStringLiteral("Config: getApiKey returns cleartext"));

    cfg.save();

    Config loaded = Config::load();
    eq(loaded.host, QStringLiteral("127.0.0.1"), QStringLiteral("Config: host reloaded"));
    check(loaded.port == 9090, QStringLiteral("Config: port reloaded"));
    eq(loaded.username, QStringLiteral("testuser"), QStringLiteral("Config: username reloaded"));
    eq(loaded.authMode, QStringLiteral("apikey"), QStringLiteral("Config: authMode reloaded"));
    eq(loaded.getPassword(), QStringLiteral("mysecretpassword"), QStringLiteral("Config: getPassword reloaded from store"));
    eq(loaded.getApiKey(), QStringLiteral("mysecretapikey"), QStringLiteral("Config: getApiKey reloaded from store"));
}

static void runCliAppE2ETests(MockTorrentServer &server)
{
    QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates = {
        appDir + QStringLiteral("/qt-magnet.exe"),
        appDir + QStringLiteral("/qt-magnet"),
        appDir + QStringLiteral("/../../qt-magnet.exe"),
        appDir + QStringLiteral("/../../dist/win_64/qt-magnet.exe"),
        appDir + QStringLiteral("/../dist/win_64/qt-magnet.exe"),
        appDir + QStringLiteral("/dist/win_64/qt-magnet.exe")
    };

    QString exePath;
    for (const QString &c : candidates) {
        if (QFile::exists(c)) {
            exePath = QFileInfo(c).canonicalFilePath();
            break;
        }
    }

    if (exePath.isEmpty())
        return;

    ConfigSandboxGuard guard;

    Config testCfg;
    testCfg.host = server.host();
    testCfg.port = server.port();
    testCfg.username = QStringLiteral("admin");
    testCfg.setPassword(QStringLiteral("adminpass"));
    testCfg.clientType = QStringLiteral("qbittorrent");
    testCfg.quickMode = true;
    testCfg.forceStartDelayMs = 0;
    testCfg.metadataTimeoutSec = 5;
    testCfg.requestTimeoutSec = 5;
    testCfg.autoCloseMs = 500;
    testCfg.autoCloseOnSuccess = true;
    testCfg.language = QStringLiteral("en_US");
    testCfg.save();

    {
        int countBefore = server.requestCount();
        QString magnet = QStringLiteral("magnet:?xt=urn:btih:c12fe1c06bba254a9dc9f519b335aa7c1367a88a&dn=E2E+Quick+Test");
        QProcess proc;
        proc.setProcessChannelMode(QProcess::MergedChannels);
        proc.start(exePath, {QStringLiteral("/quick"), magnet});
        bool finished = proc.waitForFinished(15000);
        if (!finished)
            proc.kill();
        check(finished, QStringLiteral("E2E: qt-magnet /quick finished within 15s"));
        check(proc.exitCode() == 0, QStringLiteral("E2E: qt-magnet /quick exit code is 0"));
        check(server.requestCount() > countBefore, QStringLiteral("E2E: mock server received HTTP requests from qt-magnet.exe"));
        check(server.torrents().contains(QStringLiteral("c12fe1c06bba254a9dc9f519b335aa7c1367a88a")),
              QStringLiteral("E2E: torrent added to mock server by qt-magnet.exe"));
    }

    {
        int countBefore = server.requestCount();
        QString tempTorrentPath = QDir::tempPath() + QStringLiteral("/e2e_test.torrent");
        QFile tf(tempTorrentPath);
        if (tf.open(QIODevice::WriteOnly)) {
            tf.write(QByteArrayLiteral(
                "d8:announce18:http://tr1.org/ann4:infod6:lengthi1048576e4:name8:test.iso12:piece lengthi16384e6:pieces0:ee"));
            tf.close();
        }
        auto sampleParsed = TorrentFileData::parse(QByteArrayLiteral(
            "d8:announce18:http://tr1.org/ann4:infod6:lengthi1048576e4:name8:test.iso12:piece lengthi16384e6:pieces0:ee"));

        QProcess proc;
        proc.setProcessChannelMode(QProcess::MergedChannels);
        proc.start(exePath, {QStringLiteral("/quick"), tempTorrentPath});
        bool finished = proc.waitForFinished(15000);
        if (!finished)
            proc.kill();
        check(finished, QStringLiteral("E2E: qt-magnet /quick with .torrent file finished within 15s"));
        check(proc.exitCode() == 0, QStringLiteral("E2E: qt-magnet /quick with .torrent exit code is 0"));
        check(server.requestCount() > countBefore, QStringLiteral("E2E: mock server received HTTP requests for .torrent file"));
        check(server.torrents().contains(sampleParsed.hash),
              QStringLiteral("E2E: torrent from .torrent file added to mock server"));
        QFile::remove(tempTorrentPath);
    }
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("QtMagnet"));
    QCoreApplication::setApplicationName(QStringLiteral("qt-magnet"));

    runUnitTests();
    runConfigAndSecretStoreTests();

    MockTorrentServer serverQbt(MockClientType::QBittorrent);
    MockTorrentServer serverTr(MockClientType::Transmission);
    MockTorrentServer serverAria2(MockClientType::Aria2);

    bool qbtStarted = serverQbt.start(0);
    bool trStarted = serverTr.start(0);
    bool aria2Started = serverAria2.start(0);

    check(qbtStarted, QStringLiteral("Mock qBittorrent server started on 127.0.0.1:%1").arg(serverQbt.port()));
    check(trStarted, QStringLiteral("Mock Transmission server started on 127.0.0.1:%1").arg(serverTr.port()));
    check(aria2Started, QStringLiteral("Mock Aria2 server started on 127.0.0.1:%1").arg(serverAria2.port()));

    if (qbtStarted && trStarted && aria2Started) {
        runQBittorrentTests(serverQbt);
        runQBittorrentApiKeyTests(serverQbt);
        runDualHashResolutionTests(serverQbt);
        runTagSyncTests(serverQbt);
        runTransmissionTests(serverTr);
        runAria2Tests(serverAria2);
        runAria2V2RejectionAndHybridFallbackTests(serverAria2);
        runAutoDetectTests(serverQbt, serverTr, serverAria2);
        runWorkerTests(serverQbt);
        runCliAppE2ETests(serverQbt);
    }

    serverQbt.stop();
    serverTr.stop();
    serverAria2.stop();

    std::cout << "smoketests: " << g_pass << " passed, " << g_fail << " failed" << std::endl;
    return g_fail == 0 ? 0 : 1;
}
