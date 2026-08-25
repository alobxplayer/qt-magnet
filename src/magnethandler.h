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
    void registerHandler();
    bool unregisterHandler();
    HandlerInfo query();
    bool removeUserChoice();
    void openSystemSettings();
}
