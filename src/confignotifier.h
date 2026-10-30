#pragma once
#include <QObject>

class ConfigNotifier : public QObject {
    Q_OBJECT
public:
    static ConfigNotifier *instance();

signals:
    void fileViewModeChanged(const QString &mode);

private:
    explicit ConfigNotifier(QObject *parent = nullptr);
};
