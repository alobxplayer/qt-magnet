#pragma once
#include <QString>
#include <QCoreApplication>

struct HandlerInfo {
    QString progId;
    QString command;
    bool isOurs = false;
    bool userChoiceOverride = false;
    QString userChoiceProgId;
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

    void registerAll();
    bool unregisterAll();

    inline void registerHandler() { registerAll(); }
    inline bool unregisterHandler() { return unregisterAll(); }
    inline HandlerInfo query() { return queryMagnet(); }
    inline bool removeUserChoice() { return removeMagnetUserChoice(); }

    void openSystemSettings();
}
