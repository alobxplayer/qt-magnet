#pragma once
#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>
#include "torrentpayload.h"

class Config;
class AddPage;
class QMimeData;

// The Add window: hosts one AddPage per torrent in tabs (the tab bar is
// hidden for a single torrent), owns the shared Add / Add all / Cancel
// buttons, and accepts dropped or pasted magnet links and .torrent files
// as new tabs.
class AddDialog : public QDialog {
    Q_OBJECT
public:
    // Normal flow: a compact window while the metadata loads.
    AddDialog(const Config &cfg, const TorrentPayload &payload, bool quick, QWidget *parent = nullptr);
    // Interactive window: adopts pages handed off from a loading window.
    AddDialog(const Config &cfg, QWidget *parent);
    ~AddDialog() override;

    int exitCode() const { return _successor ? _successor->exitCode() : _exitCode; }

protected:
    void closeEvent(QCloseEvent *e) override;
    void reject() override;
    void keyPressEvent(QKeyEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;

private slots:
    void onPageStateChanged();
    void onPageCloseRequested();
    void onTabCloseRequested(int index);
    void onCurrentTabChanged(int index);
    void onAdd();
    void onAddAll();
    void onCancel();

private:
    void init();
    void addTorrent(const TorrentPayload &payload);
    void attachPage(AddPage *page);
    void removeTab(int index);
    void updateFooter();
    void openInteractiveWindow();
    void updateTabLabels();
    void handleMimeData(const QMimeData *md);
    void addMagnetsFromText(const QString &text, int &added, int &dups, QStringList &problems);
    void reportDropStatus(int added, int dups, const QStringList &problems);
    bool isAlreadyOpen(const TorrentPayload &payload) const;
    void pasteFromClipboard();
    void scheduleShellClose();
    void collectPageResults();
    void recomputeExitCode();
    bool allPagesTerminal() const;
    bool allPagesSucceeded() const;
    AddPage *currentPage() const;
    QList<AddPage *> pages() const;

    const Config &_cfg;
    bool _quick;
    // The interactive window never hands off; the loading window passes its
    // pages to a successor window and closes.
    bool _interactiveOnly = false;
    AddDialog *_successor = nullptr;
    QTabWidget *_tabs = nullptr;
    QLabel *_statusLabel = nullptr;
    QPushButton *_retryButton = nullptr;
    QLabel *_summaryLabel = nullptr;
    QPushButton *_addButton = nullptr;
    QPushButton *_addAllButton = nullptr;
    QPushButton *_cancelButton = nullptr;
    QTimer *_shellCloseTimer = nullptr;
    bool _anySuccess = false;
    bool _anyFailure = false;
    bool _closing = false;
    int _exitCode = 1;
};
