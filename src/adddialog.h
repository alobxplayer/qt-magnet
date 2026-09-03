#pragma once
#include <QDialog>
#include <QTreeWidget>
#include <QComboBox>
#include <QLineEdit>
#include <QCheckBox>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QMenu>
#include <functional>
#include "torrentpayload.h"

class Config;
class Worker;
struct TorrentFile;
struct TorrentInfo;

enum TreeDataRole {
    FileIndexRole        = Qt::UserRole,
    FileSizeRole         = Qt::UserRole + 1,
    PriorityRole         = Qt::UserRole + 2,
    OriginalPriorityRole = Qt::UserRole + 3,
    PrioBeforeUncheckRole= Qt::UserRole + 4,
    IsFileRole           = Qt::UserRole + 5,
    BaseNameRole         = Qt::UserRole + 6,
    ProgressRole         = Qt::UserRole + 7
};

class AddDialog : public QDialog {
    Q_OBJECT
public:
    AddDialog(const Config &cfg, const TorrentPayload &payload, bool quick, QWidget *parent = nullptr);
    ~AddDialog() override;

    int exitCode() const { return _exitCode; }

protected:
    void closeEvent(QCloseEvent *e) override;
    void reject() override;

private slots:
    void onOk();
    void onCancel();
    void onStatus(const QString &text);
    void onPrepareFinished(bool success, const QString &error);
    void onApplyFinished(bool success, const QString &error);
    void onQuickFinished(bool success, const QString &error);
    void onCleanupFinished();
    void onTreeContextMenu(const QPoint &pos);
    void onHeaderContextMenu(const QPoint &pos);
    void onTreeItemChanged(QTreeWidgetItem *item, int column);
    void updateSummary();
    void onPollTick();
    void onPollFinished(bool success);

private:
    enum Phase { Preparing, Ready, Applying, Done };

    void buildUi();
    void updateTitle();
    void saveHeaderState();
    void startPrepare();
    void populateInteractive(const QVector<TorrentFile> &files);
    void buildTree(const QVector<TorrentFile> &files);
    void setPriority(int priority);
    void checkAll(bool on);
    void setControlsEnabled(bool on);
    void showError(const QString &msg);
    void scheduleClose();
    void cleanupAndClose();

    static void setSubtreeCheck(QTreeWidgetItem *item, bool on);
    static void setSubtreePriority(QTreeWidgetItem *item, int priority);
    static void refreshAncestors(QTreeWidgetItem *item);
    static Qt::CheckState computeFolderState(QTreeWidgetItem *folder);
    static void updateFileText(QTreeWidgetItem *item);
    static void forEachFile(QTreeWidgetItem *item, const std::function<void(QTreeWidgetItem *)> &fn);
    static void forEachFileInTree(QTreeWidget *tree, const std::function<void(QTreeWidgetItem *)> &fn);
    struct FolderStats {
        qint64 size = 0;
        int fileCount = 0;
        double doneBytes = 0.0;
    };
    static FolderStats computeFolder(QTreeWidgetItem *item);

    const Config &_cfg;
    TorrentPayload _payload;
    bool _quick;
    Phase _phase = Preparing;
    int _exitCode = 1;
    bool _blockTreeSignals = false;

    Worker *_worker = nullptr;

    QLabel *_nameLabel = nullptr;
    QLabel *_versionBadge = nullptr;
    QLineEdit *_filterEdit = nullptr;
    QTreeWidget *_tree = nullptr;
    QComboBox *_categoryBox = nullptr;
    QLineEdit *_tagsEdit = nullptr;
    QLineEdit *_pathEdit = nullptr;
    QCheckBox *_forceBox = nullptr;
    QLabel *_summaryLabel = nullptr;
    QLabel *_statusLabel = nullptr;
    QProgressBar *_progress = nullptr;
    QPushButton *_okButton = nullptr;
    QPushButton *_cancelButton = nullptr;
    QMenu *_treeMenu = nullptr;
    QAction *_actHigh = nullptr;
    QAction *_actMaximum = nullptr;
    QTimer *_autoCloseTimer = nullptr;
    QTimer *_pollTimer = nullptr;
};
