#pragma once
#include <QString>
#include <QCoreApplication>

struct HandlerInfo {
    bool isOurs = false;
    QString description;

    HandlerInfo()
        : description(QCoreApplication::translate("MagnetHandler", "Not assigned"))
    {
    }
};

namespace MagnetHandler {
    void registerMagnetHandler();
    bool unregisterMagnetHandler();
    HandlerInfo queryMagnet();
    bool removeMagnetUserChoice();

    void registerTorrentHandler();
    bool unregisterTorrentHandler();
    HandlerInfo queryTorrent();
    bool removeTorrentUserChoice();
}
