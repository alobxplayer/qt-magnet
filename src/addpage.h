#pragma once
#include <QWidget>
#include <QTreeWidget>
#include <QComboBox>
#include <QLineEdit>
#include <QCheckBox>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QMenu>
#include <QButtonGroup>
#include <functional>
#include "torrentpayload.h"
#include "torrenttypes.h"

class Config;
class Worker;
struct TorrentFile;
struct TorrentInfo;

// One torrent inside the Add window: header/progress, file table, options
// strip and its own status line. The hosting dialog (AddDialog) owns the
// tab bar and the shared Add / Add all / Cancel buttons.
class AddPage : public QWidget {
    Q_OBJECT
public:
    enum Phase { Preparing, Ready, Applying, Done };

    AddPage(const Config &cfg, const TorrentPayload &payload, bool quick, QWidget *parent = nullptr);
    ~AddPage() override;

    Phase phase() const { return _phase; }
    bool isTerminal() const { return _phase == Done; }

    // Push an externally learned rutracker passkey (another tab saved it).
    void setOwnPk(const QString &pk);

    // The Add action; false when the page cannot be committed right now.
    bool commit();
    // User-initiated cancel of this torrent (delete-on-cancel applies).
    void requestCancel();

    bool canCommit() const;
    // True while the metadata loads (and after a blind add): the hosting
    // dialog shows a compact window for this page.
    bool isCompact() const { return _compact; }
    int result() const { return _exitCode; }

    QString actionLabel() const;   // "&Add" / "&Update" / "Add &anyway"
    QString titleText() const;     // "qt-magnet [v1] - Name"
    QString displayName() const;   // resolved name, for tab labels
    const TorrentPayload &payload() const { return _payload; }

    // Footer state, rendered by the hosting dialog's single status row.
    QString statusText() const { return _statusText; }
    bool statusIsError() const { return _statusError; }
    QString summaryText() const { return _summaryText; }
    bool retryAvailable() const { return _retryVisible; }
    void retry();                 // re-run metadata fetch after a timeout

signals:
    void stateChanged();     // phase, committability or labels changed
    void closeRequested();   // the page finished its own teardown
    void passkeyLearned(const QString &passkey); // saved as the user's own

protected:
    void resizeEvent(QResizeEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *e) override;

private slots:
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
    void onRetry();
    void onLearnPasskey();
    void onFileViewModeChanged(const QString &mode);

private:
    void buildUi();
    void updateNameLabel();
    void updateMetaLine();
    void saveHeaderState();
    void startPrepare();
    void populateInteractive(const QVector<TorrentFile> &files);
    void buildFileTable(const QVector<TorrentFile> &files);
    void buildChips();
    void applyRowVisibility();
    void updateFileColumnTexts();
    void setTableMessage(const QString &text);
    void clearTableMessage();
    void updateTableMessageGeometry();
    void setPriority(int priority);
    void checkAll(bool on);
    void invertVisible();
    void setControlsEnabled(bool on);
    void setOptionsEnabled(bool on);
    bool summaryHasSelection() const;
    qint64 selectedBytes() const;
    void showError(const QString &msg);
    void cleanupAndClose();

    static void setFileCheck(QTreeWidgetItem *item, bool on);
    static void setFilePriority(QTreeWidgetItem *item, int priority);
    static void updateFileText(QTreeWidgetItem *item);
    static void forEachFileInTree(QTreeWidget *tree, const std::function<void(QTreeWidgetItem *)> &fn);

    const Config &_cfg;
    TorrentPayload _payload;
    bool _quick;
    Phase _phase = Preparing;
    int _exitCode = 1;
    bool _blockTreeSignals = false;
    QVector<TorrentFile> _files;
    QString _scopePath;   // current chip scope: empty = all files
    bool _blindAdd = false; // metadata timed out: commit adds the whole torrent
    QString _ownPk;       // the user's saved rutracker passkey

    Worker *_worker = nullptr;

    // Capabilities of the detected server; optimistic until Prepare finishes.
    BackendCaps _serverCaps = BackendCaps{true, true, true};

    // Compact mode: file table and options are hidden while metadata loads.
    bool _compact = false;
    QWidget *_optionsStrip = nullptr;
    QString _fullName;   // un-elided torrent name for the header label

    QLabel *_nameLabel = nullptr;
    QLabel *_versionBadge = nullptr;
    QLabel *_metaLabel = nullptr;
    QPushButton *_learnPkButton = nullptr;
    QProgressBar *_progress = nullptr;
    QWidget *_listBar = nullptr;
    QScrollArea *_chipScroll = nullptr;
    QLayout *_chipLayout = nullptr;
    QButtonGroup *_chipGroup = nullptr;
    QPushButton *_allButton = nullptr;
    QPushButton *_noneButton = nullptr;
    QLineEdit *_filterEdit = nullptr;
    QTreeWidget *_tree = nullptr;
    QLabel *_tableMessage = nullptr;
    QComboBox *_categoryBox = nullptr;
    QLineEdit *_tagsEdit = nullptr;
    QLineEdit *_pathEdit = nullptr;
    QCheckBox *_forceBox = nullptr;
    QString _statusText;
    QString _summaryText;
    bool _statusError = false;
    bool _retryVisible = false;
    QMenu *_treeMenu = nullptr;
    QAction *_actHigh = nullptr;
    QAction *_actMaximum = nullptr;
    QTimer *_pollTimer = nullptr;
};
