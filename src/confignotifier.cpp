#include "confignotifier.h"

ConfigNotifier::ConfigNotifier(QObject *parent)
    : QObject(parent)
{
}

ConfigNotifier *ConfigNotifier::instance()
{
    static ConfigNotifier notifier;
    return &notifier;
}
