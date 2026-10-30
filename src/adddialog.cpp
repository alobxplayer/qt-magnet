#include "adddialog.h"
#include "settingsdialog.h"
#include "addpage.h"
#include "config.h"
#include "darktheme.h"
#include "logger.h"
#include "magnetlink.h"
#include "torrentfile.h"

#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QClipboard>
#include <QGuiApplication>
#include <QKeySequence>
#include <QList>
#include <QMimeData>
#include <QTabBar>
#include <QVBoxLayout>

namespace {
constexpr int kTabLabelMax = 24;
// The auto-close timer fires no sooner than this even with a tiny config value.
constexpr int kMinAutoCloseMs = 500;
}

AddDialog::AddDialog(const Config &cfg, const TorrentPayload &payload, bool quick, QWidget *parent)
    : QDialog(parent), _cfg(cfg), _quick(quick)
{
    init();

    if (_quick)
        setWindowTitle(tr("qt-magnet - Quick Mode"));
    // Compact while the metadata loads; the full file-selection window
    // opens separately when the file list arrives (init() hides the
    // Add/Add all buttons in quick mode).
    setMinimumSize(400, 150);
    resize(460, 170);

    addTorrent(payload);
    updateFooter();
}

AddDialog::AddDialog(const Config &cfg, QWidget *parent)
    : QDialog(parent), _cfg(cfg), _quick(false), _interactiveOnly(true)
{
    init();
    setMinimumSize(600, 420);
    resize(780, 620);
}

// The successor window is unparented; by the time this runs it has closed.
AddDialog::~AddDialog()
{
    qApp->removeEventFilter(this);
    delete _successor;
}

void AddDialog::init()
{
    qApp->installEventFilter(this);
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // One tab per torrent; the bar itself is shown only for a batch.
    _tabs = new QTabWidget(this);
    _tabs->setTabsClosable(true);
    _tabs->setDocumentMode(true);
    connect(_tabs, &QTabWidget::tabCloseRequested, this, &AddDialog::onTabCloseRequested);
    connect(_tabs, &QTabWidget::currentChanged, this, &AddDialog::onCurrentTabChanged);
    mainLayout->addWidget(_tabs, 1);

    // ---- shared footer: active tab status | retry | summary | Add | Add all | Cancel ----
    auto *footer = new QWidget(this);
    auto *footerRow = new QHBoxLayout(footer);
    footerRow->setContentsMargins(12, 6, 12, 8);
    footerRow->setSpacing(10);

    _statusLabel = new QLabel(footer);
    _statusLabel->setTextFormat(Qt::PlainText);
    _statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    footerRow->addWidget(_statusLabel, 1, Qt::AlignVCenter | Qt::AlignLeft);

    _retryButton = new QPushButton(tr("Retry"), footer);
    _retryButton->setAutoDefault(false);
    _retryButton->hide();
    connect(_retryButton, &QPushButton::clicked, this, [this] {
        if (AddPage *page = currentPage())
            page->retry();
    });
    footerRow->addWidget(_retryButton, 0, Qt::AlignVCenter);

    _settingsButton = new QPushButton(tr("Settings..."), footer);
    _settingsButton->setAutoDefault(false);
    connect(_settingsButton, &QPushButton::clicked, this, &AddDialog::onOpenSettings);
    footerRow->addWidget(_settingsButton, 0, Qt::AlignVCenter);

    _summaryLabel = new QLabel(footer);
    _summaryLabel->setTextFormat(Qt::PlainText);
    footerRow->addWidget(_summaryLabel, 0, Qt::AlignVCenter);

    _addButton = new QPushButton(tr("&Add"), footer);
    _addButton->setDefault(true);
    connect(_addButton, &QPushButton::clicked, this, &AddDialog::onAdd);
    footerRow->addWidget(_addButton, 0, Qt::AlignVCenter);

    _addAllButton = new QPushButton(tr("Add a&ll"), footer);
    _addAllButton->setAutoDefault(false);
    _addAllButton->hide();
    connect(_addAllButton, &QPushButton::clicked, this, &AddDialog::onAddAll);
    footerRow->addWidget(_addAllButton, 0, Qt::AlignVCenter);

    _cancelButton = new QPushButton(tr("&Cancel"), footer);
    _cancelButton->setAutoDefault(false);
    connect(_cancelButton, &QPushButton::clicked, this, &AddDialog::onCancel);
    footerRow->addWidget(_cancelButton, 0, Qt::AlignVCenter);
    mainLayout->addWidget(footer);

    if (_quick) {
        _addButton->hide();
        _addAllButton->hide();
    } else {
        setAcceptDrops(true);
    }
}

AddPage *AddDialog::currentPage() const
{
    return qobject_cast<AddPage *>(_tabs->currentWidget());
}

QList<AddPage *> AddDialog::pages() const
{
    QList<AddPage *> result;
    for (int i = 0; i < _tabs->count(); ++i)
        if (auto *p = qobject_cast<AddPage *>(_tabs->widget(i)))
            result.append(p);
    return result;
}

void AddDialog::addTorrent(const TorrentPayload &payload)
{
    attachPage(new AddPage(_cfg, payload, _quick, nullptr));
}

void AddDialog::attachPage(AddPage *page)
{
    const int index = _tabs->addTab(page, QString());
    connect(page, &AddPage::stateChanged, this, &AddDialog::onPageStateChanged);
    connect(page, &AddPage::closeRequested, this, &AddDialog::onPageCloseRequested);
    // A passkey learned on one tab applies to every open tab.
    connect(page, &AddPage::passkeyLearned, this, [this](const QString &pk) {
        for (AddPage *p : pages())
            p->setOwnPk(pk);
    });
    _tabs->setCurrentIndex(index);
    _tabs->tabBar()->setVisible(_tabs->count() > 1);
    updateFooter();
}

void AddDialog::removeTab(int index)
{
    QWidget *w = _tabs->widget(index);
    if (!w)
        return;
    _tabs->removeTab(index);
    w->deleteLater();
    _tabs->tabBar()->setVisible(_tabs->count() > 1);
    if (_tabs->count() == 0) {
        if (!_closing) {
            _closing = true;
            recomputeExitCode();
        }
        close();
    } else {
        updateFooter();
    }
}

void AddDialog::updateTabLabels()
{
    for (int i = 0; i < _tabs->count(); ++i) {
        auto *page = qobject_cast<AddPage *>(_tabs->widget(i));
        if (!page)
            continue;
        QString name = page->displayName();
        if (name.isEmpty())
            name = tr("New torrent");
        QString label = name.length() > kTabLabelMax
                            ? name.left(kTabLabelMax / 2) + QStringLiteral("…") + name.right(kTabLabelMax / 2)
                            : name;
        // This runs on every state change; skip the setters when nothing
        // actually changed to avoid tab repaints.
        if (_tabs->tabText(i) != label)
            _tabs->setTabText(i, label);
        if (_tabs->tabToolTip(i) != name)
            _tabs->setTabToolTip(i, name);
    }
}

void AddDialog::updateFooter()
{
    AddPage *page = currentPage();
    const int n = _tabs->count();
    const bool allDone = n > 0 && allPagesTerminal();

    const QString statusText = page ? page->statusText() : QString();
    if (_statusLabel->text() != statusText)
        _statusLabel->setText(statusText);
    const bool isDark = DarkTheme::isDark();
    QPalette p = palette();
    if (page && page->statusIsError())
        p.setColor(QPalette::WindowText, isDark ? QColor(255, 110, 110) : Qt::darkRed);
    if (_statusLabel->palette() != p)
        _statusLabel->setPalette(p);
    _retryButton->setVisible(page && page->retryAvailable());
    _summaryLabel->setText(page ? page->summaryText() : QString());

    if (!_quick) {
        _addButton->setText(page ? page->actionLabel() : tr("&Add"));
        _addButton->setEnabled(page && page->canCommit());
        _addButton->setDefault(!allDone);
    }

    bool anyCommittable = false;
    for (AddPage *p : pages()) {
        if (p->canCommit()) {
            anyCommittable = true;
            break;
        }
    }
    _addAllButton->setVisible(!_quick && n > 1);
    _addAllButton->setEnabled(anyCommittable);

    if (_shellCloseTimer && _shellCloseTimer->isActive()) {
        _cancelButton->setText(tr("Close (%1s)").arg(_autoCloseRemainingSec));
        _cancelButton->setDefault(true);
        _statusLabel->setText(tr("Done. Closing in %1 s...").arg(_autoCloseRemainingSec));
    } else {
        _cancelButton->setText(allDone ? tr("Close") : tr("&Cancel"));
        _cancelButton->setDefault(allDone);
    }

    if (!_quick) {
        const QString title = page ? page->titleText() : QStringLiteral("qt-magnet");
        if (windowTitle() != title)
            setWindowTitle(title);
    }
    updateTabLabels();
}

void AddDialog::onPageStateChanged()
{
    collectPageResults();
    updateFooter();
    if (!_quick && !_interactiveOnly) {
        for (AddPage *p : pages()) {
            if (!p->isCompact()) {
                openInteractiveWindow();
                return;
            }
        }
    }
    const bool shouldAutoClose = _quick || _cfg.autoCloseOnSuccess;
    if (shouldAutoClose && _tabs->count() > 0
        && allPagesTerminal() && allPagesSucceeded()) {
        scheduleShellClose();
    }
}

// The metadata arrived: open the full file-selection window, move the pages
// there and close this compact one. A fresh window instead of resizing in
// place keeps the transition calm and the loading window in place.
void AddDialog::openInteractiveWindow()
{
    QList<AddPage *> moving;
    while (_tabs->count() > 0) {
        auto *page = qobject_cast<AddPage *>(_tabs->widget(0));
        _tabs->removeTab(0);
        if (page) {
            page->disconnect(this);
            page->setParent(nullptr);
            moving.append(page);
        }
    }

    // Unparented: the two windows must not share visibility or lifetime.
    _successor = new AddDialog(_cfg, nullptr);
    for (AddPage *p : moving)
        _successor->attachPage(p);
    const QSize s = _successor->size();
    const QPoint center = geometry().center();
    _successor->move(center.x() - s.width() / 2, center.y() - s.height() / 2);
    _successor->show();

    _closing = true; // the pages are gone; no cancel/abort side effects
    close();
}

void AddDialog::onPageCloseRequested()
{
    AddPage *page = qobject_cast<AddPage *>(sender());
    if (!page)
        return;
    const int index = _tabs->indexOf(page);
    if (index >= 0)
        removeTab(index);
}

void AddDialog::onTabCloseRequested(int index)
{
    auto *page = qobject_cast<AddPage *>(_tabs->widget(index));
    if (!page)
        return;
    if (page->phase() == AddPage::Applying) {
        _statusLabel->setText(tr("This tab is busy; try again in a moment."));
        return;
    }
    if (page->isTerminal()) {
        removeTab(index);
        return;
    }
    page->requestCancel();
}

void AddDialog::onCurrentTabChanged(int)
{
    updateFooter();
}

void AddDialog::onAdd()
{
    AddPage *page = currentPage();
    if (page)
        page->commit();
    updateFooter();
}

void AddDialog::onAddAll()
{
    int started = 0;
    for (AddPage *p : pages()) {
        if (p->canCommit() && p->commit())
            ++started;
    }
    updateFooter();
    if (started > 0)
        _statusLabel->setText(tr("Adding %n torrent(s)...", "", started));
    else
        _statusLabel->setText(tr("Nothing to add: no tab is ready yet."));
}

void AddDialog::onCancel()
{
    if (_shellCloseTimer)
        _shellCloseTimer->stop();
    if (_closing)
        return;
    if (_tabs->count() > 0 && allPagesTerminal()) {
        _closing = true;
        recomputeExitCode();
        close();
        return;
    }
    // Abort everything still in flight; done pages simply drop their tabs.
    for (AddPage *p : pages())
        p->requestCancel();
}

bool AddDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (_shellCloseTimer && _shellCloseTimer->isActive() && event->type() == QEvent::MouseButtonPress) {
        auto *w = qobject_cast<QWidget *>(watched);
        if (w && (w == this || this->isAncestorOf(w))) {
            if (w != _cancelButton) {
                cancelAutoCloseTimer();
            }
        }
    }
    return QDialog::eventFilter(watched, event);
}

void AddDialog::changeEvent(QEvent *e)
{
    QDialog::changeEvent(e);
    if (e->type() == QEvent::PaletteChange || e->type() == QEvent::ThemeChange) {
        updateFooter();
    }
}

void AddDialog::closeEvent(QCloseEvent *e)
{
    if (_shellCloseTimer)
        _shellCloseTimer->stop();
    if (_closing) {
        e->accept();
        return;
    }
    bool anyActive = false;
    for (AddPage *p : pages()) {
        if (!p->isTerminal()) {
            p->requestCancel();
            anyActive = true;
        }
    }
    if (anyActive) {
        e->ignore();
        return;
    }
    _closing = true;
    recomputeExitCode();
    e->accept();
}

void AddDialog::reject()
{
    onCancel();
}

void AddDialog::keyPressEvent(QKeyEvent *e)
{
    // Enter commits the active tab from anywhere (the file table consumes
    // Return internally and never reaches this handler).
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        AddPage *page = currentPage();
        if (page && page->canCommit()) {
            onAdd();
            return;
        }
        // Fall through: let the default button (e.g. Close) handle it.
    }
    // Ctrl+V anywhere outside a text field starts a new tab from the clipboard.
    if (!_quick && e->matches(QKeySequence::Paste)
        && !qobject_cast<QLineEdit *>(focusWidget())) {
        pasteFromClipboard();
        return;
    }
    QDialog::keyPressEvent(e);
}

void AddDialog::dragEnterEvent(QDragEnterEvent *e)
{
    if (_quick)
        return;
    if (e->mimeData()->hasUrls() || e->mimeData()->hasText())
        e->acceptProposedAction();
}

void AddDialog::dropEvent(QDropEvent *e)
{
    if (_quick)
        return;
    handleMimeData(e->mimeData());
    e->acceptProposedAction();
}

void AddDialog::handleMimeData(const QMimeData *md)
{
    int added = 0;
    int dups = 0;
    QStringList problems;

    if (md->hasUrls()) {
        for (const QUrl &u : md->urls()) {
            if (u.isLocalFile()) {
                const QString path = u.toLocalFile();
                if (!path.endsWith(QLatin1String(".torrent"), Qt::CaseInsensitive))
                    continue;
                try {
                    const TorrentPayload tp = TorrentPayload::fromFile(path);
                    if (isAlreadyOpen(tp))
                        ++dups;
                    else {
                        addTorrent(tp);
                        ++added;
                    }
                } catch (const std::exception &ex) {
                    problems << tr("%1: %2").arg(QFileInfo(path).fileName(), QString::fromUtf8(ex.what()));
                }
            } else if (u.scheme().compare(QLatin1String("magnet"), Qt::CaseInsensitive) == 0) {
                try {
                    const TorrentPayload tp = TorrentPayload::fromMagnet(u.toString());
                    if (isAlreadyOpen(tp))
                        ++dups;
                    else {
                        addTorrent(tp);
                        ++added;
                    }
                } catch (const std::exception &ex) {
                    problems << QString::fromUtf8(ex.what());
                }
            }
        }
    }

    if (added == 0 && dups == 0 && problems.isEmpty() && md->hasText())
        addMagnetsFromText(md->text(), added, dups, problems);

    reportDropStatus(added, dups, problems);
}

void AddDialog::addMagnetsFromText(const QString &text, int &added, int &dups, QStringList &problems)
{
    for (const QString &m : MagnetLink::findMagnets(text)) {
        try {
            const TorrentPayload tp = TorrentPayload::fromMagnet(m);
            if (isAlreadyOpen(tp))
                ++dups;
            else {
                addTorrent(tp);
                ++added;
            }
        } catch (const std::exception &ex) {
            problems << QString::fromUtf8(ex.what());
        }
    }
}

void AddDialog::reportDropStatus(int added, int dups, const QStringList &problems)
{
    QStringList parts;
    if (added > 0)
        parts << tr("%n torrent(s) added.", "", added);
    if (dups > 0)
        parts << tr("%n torrent(s) already open in another tab.", "", dups);
    if (added == 0 && dups == 0 && problems.isEmpty())
        parts << tr("No magnet links or .torrent files found.");
    if (!problems.isEmpty())
        parts << tr("Failed: %1").arg(problems.join(QStringLiteral("; ")));
    _statusLabel->setText(parts.join(QStringLiteral(" · ")));
    _statusLabel->setToolTip(problems.join(QLatin1Char('\n')));
}

bool AddDialog::isAlreadyOpen(const TorrentPayload &payload) const
{
    const QString hash = payload.hash();
    if (hash.isEmpty())
        return false;
    for (AddPage *p : pages()) {
        if (p->payload().kind == payload.kind && p->payload().hash() == hash)
            return true;
    }
    return false;
}

void AddDialog::pasteFromClipboard()
{
    const QMimeData *md = QGuiApplication::clipboard()->mimeData();
    if (md->hasText() || md->hasUrls())
        handleMimeData(md);
    else
        _statusLabel->setText(tr("Clipboard is empty."));
}

void AddDialog::scheduleShellClose()
{
    if (_shellCloseTimer && _shellCloseTimer->isActive())
        return;
    const int totalMs = qMax(kMinAutoCloseMs, _cfg.autoCloseMs);
    _autoCloseRemainingSec = qMax(1, (totalMs + 999) / 1000);

    auto updateCountdownUi = [this] {
        if (_statusLabel)
            _statusLabel->setText(tr("Done. Closing in %1 s...").arg(_autoCloseRemainingSec));
        if (_cancelButton)
            _cancelButton->setText(tr("Close (%1s)").arg(_autoCloseRemainingSec));
    };

    updateCountdownUi();

    if (!_shellCloseTimer) {
        _shellCloseTimer = new QTimer(this);
        connect(_shellCloseTimer, &QTimer::timeout, this, [this] {
            --_autoCloseRemainingSec;
            if (_autoCloseRemainingSec <= 0) {
                _shellCloseTimer->stop();
                _closing = true;
                recomputeExitCode();
                close();
            } else {
                if (_statusLabel)
                    _statusLabel->setText(tr("Done. Closing in %1 s...").arg(_autoCloseRemainingSec));
                if (_cancelButton)
                    _cancelButton->setText(tr("Close (%1s)").arg(_autoCloseRemainingSec));
            }
        });
    }
    _shellCloseTimer->start(1000);
}

void AddDialog::cancelAutoCloseTimer()
{
    if (_shellCloseTimer && _shellCloseTimer->isActive())
        _shellCloseTimer->stop();
    _autoCloseRemainingSec = 0;
    updateFooter();
}

void AddDialog::collectPageResults()
{
    for (AddPage *p : pages()) {
        if (!p->isTerminal())
            continue;
        if (p->result() == 0)
            _anySuccess = true;
        else if (p->result() == 1)
            _anyFailure = true;
    }
}

void AddDialog::recomputeExitCode()
{
    collectPageResults();
    _exitCode = _anyFailure ? 1 : (_anySuccess ? 0 : 3);
}

bool AddDialog::allPagesTerminal() const
{
    const QList<AddPage *> ps = pages();
    if (ps.isEmpty())
        return false;
    for (AddPage *p : ps)
        if (!p->isTerminal())
            return false;
    return true;
}

bool AddDialog::allPagesSucceeded() const
{
    for (AddPage *p : pages())
        if (p->result() != 0)
            return false;
    return true;
}

void AddDialog::onOpenSettings()
{
    if (!_settingsDialog) {
        _settingsDialog = new SettingsDialog(const_cast<Config &>(_cfg), this);
        _settingsDialog->setAttribute(Qt::WA_DeleteOnClose);
    }
    _settingsDialog->show();
    _settingsDialog->raise();
    _settingsDialog->activateWindow();
}
