#include "adddialog.h"
#include "config.h"
#include "format.h"
#include "logger.h"
#include "worker.h"

#include <QCloseEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QMap>

static const char *kAppTitle = "qt-magnet";

namespace {
constexpr int kColWidthFile     = 300;
constexpr int kColWidthSize     = 95;
constexpr int kColWidthProgress = 75;
constexpr int kColWidthPriority = 110;
constexpr int kColWidthIndex    = 50;

class FileTreeItem : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;
    bool operator<(const QTreeWidgetItem &other) const override {
        int col = treeWidget() ? treeWidget()->sortColumn() : 0;
        bool thisIsFile = data(0, IsFileRole).toBool();
        bool otherIsFile = other.data(0, IsFileRole).toBool();
        if (!thisIsFile && otherIsFile) {
            bool desc = treeWidget() && treeWidget()->header()->sortIndicatorOrder() == Qt::DescendingOrder;
            return !desc;
        }
        if (thisIsFile && !otherIsFile) {
            bool desc = treeWidget() && treeWidget()->header()->sortIndicatorOrder() == Qt::DescendingOrder;
            return desc;
        }

        if (col == 1) {
            return data(0, FileSizeRole).toLongLong() < other.data(0, FileSizeRole).toLongLong();
        }
        if (col == 2) {
            return data(0, ProgressRole).toDouble() < other.data(0, ProgressRole).toDouble();
        }
        if (col == 3) {
            return data(0, PriorityRole).toInt() < other.data(0, PriorityRole).toInt();
        }
        if (col == 4) {
            return data(0, FileIndexRole).toInt() < other.data(0, FileIndexRole).toInt();
        }
        return text(col).localeAwareCompare(other.text(col)) < 0;
    }
};
} // namespace

AddDialog::AddDialog(const Config &cfg, const MagnetLink &link, bool quick, QWidget *parent)
    : QDialog(parent), _cfg(cfg), _link(link), _quick(quick)
{
    buildUi();
    QTimer::singleShot(0, this, &AddDialog::startPrepare);
}

AddDialog::~AddDialog()
{
    saveHeaderState();
    if (_worker) {
        _worker->requestCancel();
        _worker->wait(3000);
        delete _worker;
    }
}

void AddDialog::saveHeaderState()
{
    if (!_tree || !_tree->header())
        return;
    Config c = Config::load();
    c.treeHeaderState = QString::fromLatin1(_tree->header()->saveState().toBase64());
    try { c.save(); } catch (const std::exception &ex) { Log::write(QStringLiteral("Failed to save header state: %1").arg(QString::fromUtf8(ex.what()))); }
}

void AddDialog::buildUi()
{
    setWindowTitle(QString::fromUtf8(kAppTitle));
    resize(720, 620);
    setMinimumSize(560, 480);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    _nameLabel = new QLabel(_link.prettyName(), this);
    _nameLabel->setTextFormat(Qt::PlainText);
    _nameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    QFont f = _nameLabel->font();
    f.setBold(true);
    f.setPointSizeF(f.pointSizeF() + 1.5);
    _nameLabel->setFont(f);
    _nameLabel->setContentsMargins(12, 10, 12, 4);
    _nameLabel->setWordWrap(true);
    _nameLabel->setMinimumWidth(1);
    mainLayout->addWidget(_nameLabel);

    _tree = new QTreeWidget(this);
    _tree->setHeaderLabels({tr("File"), tr("Size"), tr("Progress"), tr("Priority"), tr("#")});
    _tree->header()->setSectionsMovable(true);
    _tree->header()->setSectionResizeMode(QHeaderView::Interactive);
    _tree->header()->setStretchLastSection(false);
    _tree->header()->setContextMenuPolicy(Qt::CustomContextMenu);
    _tree->setSortingEnabled(true);
    _tree->sortByColumn(0, Qt::AscendingOrder);
    _tree->setRootIsDecorated(true);
    _tree->setAlternatingRowColors(true);
    _tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    _tree->setContextMenuPolicy(Qt::CustomContextMenu);

    _tree->setColumnWidth(0, kColWidthFile);
    _tree->setColumnWidth(1, kColWidthSize);
    _tree->setColumnWidth(2, kColWidthProgress);
    _tree->setColumnWidth(3, kColWidthPriority);
    _tree->setColumnWidth(4, kColWidthIndex);

    if (!_cfg.treeHeaderState.isEmpty()) {
        _tree->header()->restoreState(QByteArray::fromBase64(_cfg.treeHeaderState.toLatin1()));
    }

    connect(_tree->header(), &QHeaderView::customContextMenuRequested, this, &AddDialog::onHeaderContextMenu);
    connect(_tree, &QTreeWidget::customContextMenuRequested, this, &AddDialog::onTreeContextMenu);
    connect(_tree, &QTreeWidget::itemChanged, this, &AddDialog::onTreeItemChanged);
    connect(_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        if (item->data(0, IsFileRole).toBool())
            item->setCheckState(0, item->checkState(0) == Qt::Checked ? Qt::Unchecked : Qt::Checked);
    });
    mainLayout->addWidget(_tree, 1);

    _treeMenu = new QMenu(this);
    _treeMenu->addAction(tr("Download (Normal)"), this, [this]{ setPriority(1); });
    _treeMenu->addAction(tr("High"), this, [this]{ setPriority(6); });
    _treeMenu->addAction(tr("Maximum"), this, [this]{ setPriority(7); });
    _treeMenu->addSeparator();
    _treeMenu->addAction(tr("Do not download"), this, [this]{ setPriority(0); });
    _treeMenu->addSeparator();
    _treeMenu->addAction(tr("Check all"), this, [this]{ checkAll(true); });
    _treeMenu->addAction(tr("Uncheck all"), this, [this]{ checkAll(false); });
    _treeMenu->addAction(tr("Invert check"), this, [this]{
        _blockTreeSignals = true;
        forEachFileInTree(_tree, [](QTreeWidgetItem *item) {
            bool on = item->checkState(0) != Qt::Checked;
            setSubtreeCheck(item, on);
        });
        for (int i = 0; i < _tree->topLevelItemCount(); ++i)
            _tree->topLevelItem(i)->setCheckState(0, computeFolderState(_tree->topLevelItem(i)));
        _blockTreeSignals = false;
        updateSummary();
    });
    _treeMenu->addSeparator();
    _treeMenu->addAction(tr("Expand all"), _tree, &QTreeWidget::expandAll);
    _treeMenu->addAction(tr("Collapse all"), _tree, &QTreeWidget::collapseAll);

    auto *options = new QWidget(this);
    auto *optGrid = new QGridLayout(options);
    optGrid->setContentsMargins(12, 6, 12, 6);

    auto makeLabel = [this](const QString &text, QWidget *buddy) {
        auto *l = new QLabel(text, this);
        l->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        l->setBuddy(buddy);
        return l;
    };

    _categoryBox = new QComboBox(this);
    _categoryBox->setEditable(true);
    _categoryBox->setPlaceholderText(tr("Select or type category"));
    _tagsEdit = new QLineEdit(this);
    _tagsEdit->setPlaceholderText(tr("tag1, tag2"));
    _tagsEdit->setClearButtonEnabled(true);
    _pathEdit = new QLineEdit(this);
    _pathEdit->setPlaceholderText(tr("Leave empty for default"));
    _pathEdit->setClearButtonEnabled(true);
    _forceBox = new QCheckBox(tr("Force start"), this);
    _forceBox->setToolTip(tr("Start downloading immediately, bypassing queue limits"));
    _forceBox->setChecked(_cfg.forceStartDefault);

    optGrid->addWidget(makeLabel(tr("&Category:"), _categoryBox), 0, 0);
    optGrid->addWidget(_categoryBox, 0, 1);
    optGrid->addWidget(makeLabel(tr("&Tags:"), _tagsEdit), 1, 0);
    optGrid->addWidget(_tagsEdit, 1, 1);
    optGrid->addWidget(makeLabel(tr("&Save path:"), _pathEdit), 2, 0);
    optGrid->addWidget(_pathEdit, 2, 1);
    optGrid->addWidget(_forceBox, 3, 1);
    optGrid->setColumnStretch(1, 1);
    mainLayout->addWidget(options);

    _summaryLabel = new QLabel(this);
    _summaryLabel->setContentsMargins(12, 4, 12, 0);
    mainLayout->addWidget(_summaryLabel);

    auto *bottomLayout = new QHBoxLayout();
    bottomLayout->setContentsMargins(12, 8, 12, 10);

    auto *statusPanel = new QVBoxLayout();
    _statusLabel = new QLabel(tr("Connecting..."), this);
    _statusLabel->setTextFormat(Qt::PlainText);
    _progress = new QProgressBar(this);
    _progress->setTextVisible(false);
    _progress->setRange(0, 0);
    statusPanel->addWidget(_statusLabel);
    statusPanel->addWidget(_progress);
    bottomLayout->addLayout(statusPanel, 1);

    _okButton = new QPushButton(tr("Add"), this);
    _okButton->setEnabled(false);
    _okButton->setDefault(true);
    connect(_okButton, &QPushButton::clicked, this, &AddDialog::onOk);
    bottomLayout->addWidget(_okButton);

    _cancelButton = new QPushButton(tr("Cancel"), this);
    _cancelButton->setAutoDefault(false);
    connect(_cancelButton, &QPushButton::clicked, this, &AddDialog::onCancel);
    bottomLayout->addWidget(_cancelButton);

    mainLayout->addLayout(bottomLayout);

    if (_quick) {
        _tree->hide();
        options->hide();
        _summaryLabel->hide();
        _okButton->hide();
        setWindowTitle(tr("qt-magnet - Quick Mode"));
        resize(460, 170);
        setMinimumSize(380, 150);
    } else {
        setControlsEnabled(false);
    }
}

void AddDialog::startPrepare()
{
    _worker = new Worker(_cfg, _link, _quick, nullptr);
    _worker->setTask(Worker::Prepare);
    connect(_worker, &Worker::status, this, &AddDialog::onStatus, Qt::QueuedConnection);
    connect(_worker, &Worker::prepareFinished, this, &AddDialog::onPrepareFinished, Qt::QueuedConnection);
    connect(_worker, &Worker::quickFinished, this, &AddDialog::onQuickFinished, Qt::QueuedConnection);
    connect(_worker, &Worker::applyFinished, this, &AddDialog::onApplyFinished, Qt::QueuedConnection);
    connect(_worker, &Worker::cleanupFinished, this, &AddDialog::onCleanupFinished, Qt::QueuedConnection);
    _worker->start();
}

void AddDialog::onStatus(const QString &text)
{
    _statusLabel->setText(text);
}

void AddDialog::onPrepareFinished(bool success, const QString &error)
{
    if (!success || _worker->wasCancelled()) {
        if (_worker->wasCancelled()) {
            cleanupAndClose();
        } else {
            _phase = Done;
            _cancelButton->setText(tr("Close"));
            _cancelButton->setDefault(true);
            showError(error);
        }
        return;
    }
    populateInteractive(_worker->files);
}

void AddDialog::onQuickFinished(bool success, const QString &error)
{
    _phase = Done;
    _exitCode = (success && _worker->resultOk) ? 0 : 1;
    _progress->setRange(0, 100);
    _progress->setValue(100);
    _statusLabel->setText(_worker->resultOk ? tr("Done.") : tr("Failed to force start."));
    _cancelButton->setText(tr("Close"));
    _cancelButton->setDefault(true);
    if (!error.isEmpty() && !success)
        showError(error);
    else if (_cfg.autoCloseOnSuccess && _worker->resultOk)
        scheduleClose();
}

void AddDialog::onApplyFinished(bool success, const QString &error)
{
    if (!success) {
        _phase = Ready;
        setControlsEnabled(true);
        _cancelButton->setEnabled(true);
        _progress->setRange(0, 100);
        _progress->setValue(0);
        showError(error);
        return;
    }

    _phase = Done;
    _exitCode = _worker->resultOk ? 0 : 1;
    _progress->setRange(0, 100);
    _progress->setValue(100);
    _statusLabel->setText(_worker->resultOk ? tr("Done.") : tr("Done (status unverified)."));
    _okButton->hide();
    _cancelButton->setText(tr("Close"));
    _cancelButton->setEnabled(true);
    _cancelButton->setDefault(true);
    if (_cfg.autoCloseOnSuccess && _worker->resultOk)
        scheduleClose();
}

void AddDialog::onCleanupFinished()
{
    _phase = Done;
    close();
}

void AddDialog::populateInteractive(const QVector<TorrentFile> &files)
{
    _phase = Ready;
    setControlsEnabled(true);
    _progress->setRange(0, 100);
    _progress->setValue(0);
    _cancelButton->setEnabled(true);
    _cancelButton->setText(tr("Cancel"));

    _categoryBox->clear();
    _categoryBox->addItem(QString());
    if (_worker) {
        for (const auto &kv : _worker->categories)
            if (!kv.first.isEmpty())
                _categoryBox->addItem(kv.first);
    }

    QString cat = (_worker && (_worker->existed || !_worker->initialCategory.isEmpty())) ? _worker->initialCategory : _cfg.defaultCategory;
    _categoryBox->setCurrentText(cat);
    _tagsEdit->setText((_worker && (_worker->existed || !_worker->initialTags.isEmpty())) ? _worker->initialTags : _cfg.defaultTags);
    _pathEdit->setText(_worker ? _worker->initialSavePath : QString());

    if (_worker->existed) {
        _okButton->setText(tr("Update"));
        if (files.isEmpty()) {
            _statusLabel->setText(tr("Torrent already exists in client."));
            _summaryLabel->setText(tr("Files unavailable."));
            _okButton->setEnabled(true);
            _okButton->setFocus();
            return;
        }
        buildTree(files);
        _statusLabel->setText(tr("Torrent already exists. Trackers and settings updated."));
    } else {
        _okButton->setText(tr("Add"));
        if (files.isEmpty()) {
            _statusLabel->setText(tr("Metadata timeout. Adding without file list."));
            _summaryLabel->setText(tr("Files unavailable."));
            _okButton->setEnabled(true);
            _okButton->setFocus();
            return;
        }
        buildTree(files);
        _statusLabel->setText(tr("Metadata received."));
    }

    _okButton->setEnabled(true);
    _tree->setFocus();
    updateSummary();
}

void AddDialog::buildTree(const QVector<TorrentFile> &files)
{
    _blockTreeSignals = true;
    _tree->setSortingEnabled(false);
    _tree->clear();
    QMap<QString, QTreeWidgetItem *> folders;

    for (const TorrentFile &f : files) {
        QString path = f.name;
        path.replace(QLatin1Char('\\'), QLatin1Char('/'));
        QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        QTreeWidgetItem *parent = nullptr;
        QString acc;

        for (int i = 0; i < parts.size(); ++i) {
            if (acc.isEmpty())
                acc = parts[i];
            else
                acc = acc + QLatin1Char('/') + parts[i];

            bool isLast = (i == parts.size() - 1);
            if (isLast) {
                auto *item = new FileTreeItem();
                item->setText(0, parts[i]);
                item->setData(0, FileIndexRole, f.index);
                item->setData(0, FileSizeRole, f.size);
                item->setData(0, PriorityRole, f.priority);
                item->setData(0, OriginalPriorityRole, f.priority);
                item->setData(0, PrioBeforeUncheckRole, f.priority == 0 ? 1 : f.priority);
                item->setData(0, ProgressRole, f.progress);
                item->setData(0, IsFileRole, true);
                item->setData(0, BaseNameRole, parts[i]);
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(0, f.priority == 0 ? Qt::Unchecked : Qt::Checked);
                if (parent)
                    parent->addChild(item);
                else
                    _tree->addTopLevelItem(item);
                updateFileText(item);
            } else {
                QTreeWidgetItem *folder = folders.value(acc);
                if (!folder) {
                    folder = new FileTreeItem();
                    folder->setText(0, parts[i]);
                    folder->setData(0, IsFileRole, false);
                    folder->setData(0, BaseNameRole, parts[i]);
                    folder->setFlags(folder->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
                    folder->setCheckState(0, Qt::Checked);
                    if (parent)
                        parent->addChild(folder);
                    else
                        _tree->addTopLevelItem(folder);
                    folders[acc] = folder;
                }
                parent = folder;
            }
        }
    }

    for (int i = 0; i < _tree->topLevelItemCount(); ++i) {
        computeFolder(_tree->topLevelItem(i));
        if (!_tree->topLevelItem(i)->data(0, IsFileRole).toBool())
            _tree->topLevelItem(i)->setCheckState(0, computeFolderState(_tree->topLevelItem(i)));
    }

    if (_tree->topLevelItemCount() <= 2)
        _tree->expandAll();
    else
        _tree->expandToDepth(0);

    _tree->setSortingEnabled(true);
    _blockTreeSignals = false;
}

AddDialog::FolderStats AddDialog::computeFolder(QTreeWidgetItem *item)
{
    if (item->data(0, IsFileRole).toBool()) {
        return { item->data(0, FileSizeRole).toLongLong(), 1 };
    }

    qint64 sum = 0;
    int fileCount = 0;
    for (int i = 0; i < item->childCount(); ++i) {
        FolderStats childStats = computeFolder(item->child(i));
        sum += childStats.size;
        fileCount += childStats.fileCount;
    }
    item->setData(0, FileSizeRole, sum);
    QString baseName = item->data(0, BaseNameRole).toString();
    if (baseName.isEmpty())
        baseName = item->text(0);
    const QString countStr = tr("%n file(s)", "", fileCount);
    item->setText(0, baseName);
    item->setText(1, Format::size(sum) + QStringLiteral(" (") + countStr + QStringLiteral(")"));
    item->setText(2, QString());
    item->setText(3, QString());
    item->setText(4, QString());
    return { sum, fileCount };
}

void AddDialog::onHeaderContextMenu(const QPoint &pos)
{
    QMenu menu(this);
    menu.setTitle(tr("Columns"));

    QHeaderView *hdr = _tree->header();
    int count = hdr->count();

    for (int i = 0; i < count; ++i) {
        QString title = _tree->headerItem()->text(i);
        if (title.isEmpty())
            continue;
        auto *act = menu.addAction(title);
        act->setCheckable(true);
        act->setChecked(!hdr->isSectionHidden(i));
        if (i == 0) {
            act->setEnabled(false);
        } else {
            connect(act, &QAction::toggled, this, [hdr, i](bool visible) {
                hdr->setSectionHidden(i, !visible);
            });
        }
    }

    menu.addSeparator();
    menu.addAction(tr("Reset columns"), this, [this, hdr]() {
        for (int i = 0; i < hdr->count(); ++i)
            hdr->setSectionHidden(i, false);
        _tree->setColumnWidth(0, kColWidthFile);
        _tree->setColumnWidth(1, kColWidthSize);
        _tree->setColumnWidth(2, kColWidthProgress);
        _tree->setColumnWidth(3, kColWidthPriority);
        _tree->setColumnWidth(4, kColWidthIndex);
    });

    menu.exec(_tree->header()->mapToGlobal(pos));
}


void AddDialog::onTreeItemChanged(QTreeWidgetItem *item, int column)
{
    if (_blockTreeSignals || column != 0)
        return;

    _blockTreeSignals = true;
    bool isFile = item->data(0, IsFileRole).toBool();
    if (isFile) {
        int prio = item->data(0, PriorityRole).toInt();
        if (item->checkState(0) == Qt::Unchecked) {
            if (prio != 0) {
                item->setData(0, PrioBeforeUncheckRole, prio);
                item->setData(0, PriorityRole, 0);
            }
        } else {
            if (prio == 0) {
                int prev = item->data(0, PrioBeforeUncheckRole).toInt();
                item->setData(0, PriorityRole, prev > 0 ? prev : 1);
            }
        }
        updateFileText(item);
        refreshAncestors(item);
    } else {
        if (item->checkState(0) != Qt::PartiallyChecked) {
            bool on = (item->checkState(0) == Qt::Checked);
            setSubtreeCheck(item, on);
            refreshAncestors(item);
        }
    }
    _blockTreeSignals = false;
    updateSummary();
}

void AddDialog::onTreeContextMenu(const QPoint &pos)
{
    QTreeWidgetItem *item = _tree->itemAt(pos);
    if (item && !item->isSelected()) {
        _tree->clearSelection();
        item->setSelected(true);
        _tree->setCurrentItem(item);
    }
    _treeMenu->popup(_tree->viewport()->mapToGlobal(pos));
}

void AddDialog::setPriority(int priority)
{
    auto items = _tree->selectedItems();
    if (items.isEmpty() && _tree->currentItem())
        items.append(_tree->currentItem());
    if (items.isEmpty())
        return;

    _blockTreeSignals = true;
    for (QTreeWidgetItem *item : items) {
        setSubtreePriority(item, priority);
        refreshAncestors(item);
    }
    _blockTreeSignals = false;
    updateSummary();
}

void AddDialog::checkAll(bool on)
{
    _blockTreeSignals = true;
    for (int i = 0; i < _tree->topLevelItemCount(); ++i)
        setSubtreeCheck(_tree->topLevelItem(i), on);
    _blockTreeSignals = false;
    updateSummary();
}

void AddDialog::setSubtreeCheck(QTreeWidgetItem *item, bool on)
{
    bool isFile = item->data(0, IsFileRole).toBool();
    if (isFile) {
        int prio = item->data(0, PriorityRole).toInt();
        if (on) {
            if (prio == 0) {
                int prev = item->data(0, PrioBeforeUncheckRole).toInt();
                item->setData(0, PriorityRole, prev > 0 ? prev : 1);
            }
        } else {
            if (prio != 0)
                item->setData(0, PrioBeforeUncheckRole, prio);
            item->setData(0, PriorityRole, 0);
        }
        updateFileText(item);
    }
    item->setCheckState(0, on ? Qt::Checked : Qt::Unchecked);
    for (int i = 0; i < item->childCount(); ++i)
        setSubtreeCheck(item->child(i), on);
}

void AddDialog::setSubtreePriority(QTreeWidgetItem *item, int priority)
{
    bool isFile = item->data(0, IsFileRole).toBool();
    if (isFile) {
        item->setData(0, PriorityRole, priority);
        if (priority != 0)
            item->setData(0, PrioBeforeUncheckRole, priority);
        item->setCheckState(0, priority == 0 ? Qt::Unchecked : Qt::Checked);
        updateFileText(item);
    }
    for (int i = 0; i < item->childCount(); ++i)
        setSubtreePriority(item->child(i), priority);
    if (item->childCount() > 0)
        item->setCheckState(0, computeFolderState(item));
}

void AddDialog::refreshAncestors(QTreeWidgetItem *item)
{
    QTreeWidgetItem *p = item->parent();
    while (p) {
        p->setCheckState(0, computeFolderState(p));
        p = p->parent();
    }
}

Qt::CheckState AddDialog::computeFolderState(QTreeWidgetItem *folder)
{
    bool anyOn = false, anyOff = false;
    for (int i = 0; i < folder->childCount(); ++i) {
        QTreeWidgetItem *child = folder->child(i);
        Qt::CheckState st = child->childCount() > 0 ? computeFolderState(child) : child->checkState(0);
        if (st == Qt::PartiallyChecked)
            return Qt::PartiallyChecked;
        if (st == Qt::Checked) anyOn = true;
        else anyOff = true;
        if (anyOn && anyOff)
            return Qt::PartiallyChecked;
    }
    return anyOn ? Qt::Checked : Qt::Unchecked;
}

void AddDialog::updateFileText(QTreeWidgetItem *item)
{
    if (!item->data(0, IsFileRole).toBool())
        return;
    QString baseName = item->data(0, BaseNameRole).toString();
    if (baseName.isEmpty())
        baseName = item->text(0);
    if (baseName.isEmpty())
        return;
    qint64 sz = item->data(0, FileSizeRole).toLongLong();
    int prio = item->data(0, PriorityRole).toInt();
    double prog = item->data(0, ProgressRole).toDouble();
    int idx = item->data(0, FileIndexRole).toInt();

    item->setText(0, baseName);
    item->setText(1, Format::size(sz));
    item->setText(2, prog <= 0.0 ? QStringLiteral("0%") : QString::number(qRound(prog * 100.0)) + QLatin1Char('%'));
    item->setText(3, Format::priorityName(prio));
    item->setText(4, QString::number(idx + 1));
}

void AddDialog::forEachFile(QTreeWidgetItem *item, const std::function<void(QTreeWidgetItem *)> &fn)
{
    if (item->data(0, IsFileRole).toBool())
        fn(item);
    for (int i = 0; i < item->childCount(); ++i)
        forEachFile(item->child(i), fn);
}

void AddDialog::forEachFileInTree(QTreeWidget *tree, const std::function<void(QTreeWidgetItem *)> &fn)
{
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        forEachFile(tree->topLevelItem(i), fn);
}

void AddDialog::updateSummary()
{
    qint64 selected = 0, total = 0;
    int selCount = 0, allCount = 0;
    forEachFileInTree(_tree, [&](QTreeWidgetItem *item) {
        qint64 sz = item->data(0, FileSizeRole).toLongLong();
        int prio = item->data(0, PriorityRole).toInt();
        total += sz;
        allCount++;
        if (prio != 0) {
            selected += sz;
            selCount++;
        }
    });
    _summaryLabel->setText(tr("Selected %1 / %2 (%3 / %4 files)")
                                .arg(Format::size(selected), Format::size(total))
                                .arg(selCount).arg(allCount));
}

void AddDialog::onOk()
{
    if (_phase != Ready)
        return;

    QMap<int, QList<int>> byPriority;
    bool anyChanged = false;
    bool anySelected = false;

    forEachFileInTree(_tree, [&](QTreeWidgetItem *item) {
        int prio = item->data(0, PriorityRole).toInt();
        int orig = item->data(0, OriginalPriorityRole).toInt();
        int idx = item->data(0, FileIndexRole).toInt();
        if (prio != orig) {
            byPriority[prio].append(idx);
            anyChanged = true;
        }
        if (prio != 0)
            anySelected = true;
    });

    bool haveFiles = _tree->topLevelItemCount() > 0;
    if (haveFiles && !anySelected) {
        QMessageBox::warning(this, QString::fromUtf8(kAppTitle), tr("No files selected."));
        return;
    }

    saveHeaderState();
    _phase = Applying;
    setControlsEnabled(false);
    _cancelButton->setEnabled(false);
    _progress->setRange(0, 0);
    _statusLabel->setText(_worker && _worker->existed ? tr("Updating torrent...") : tr("Adding torrent..."));

    Worker::ApplyParams p;
    p.hash = _worker->hash;
    p.prioritiesByPrio = byPriority;
    p.anyChanged = anyChanged;
    p.category = _categoryBox->currentText().trimmed();
    p.tags = _tagsEdit->text().trimmed();
    QString path = _pathEdit->text().trimmed();
    if (path.length() >= 2 && path.startsWith(QLatin1Char('"')) && path.endsWith(QLatin1Char('"')))
        path = path.mid(1, path.length() - 2);
    p.savePath = path;
    p.forceStart = _forceBox->isChecked();
    p.initialCategory = _worker->initialCategory;
    p.initialSavePath = _worker->initialSavePath;
    p.trackers = _link.trackers;

    _worker->applyParams = p;
    _worker->setTask(Worker::Apply);
    _worker->start();
}

void AddDialog::onCancel()
{
    if (_phase == Done) {
        close();
        return;
    }
    if (_phase == Applying)
        return;

    if (_worker)
        _worker->requestCancel();

    if (_phase == Ready) {
        cleanupAndClose();
    } else {
        _statusLabel->setText(tr("Cancelling..."));
        _cancelButton->setEnabled(false);
    }
}

void AddDialog::cleanupAndClose()
{
    _exitCode = 3;
    if (_worker) {
        _worker->wait();
    }
    if (_worker && _worker->weAdded && _cfg.deleteOnCancel && !_worker->hash.isEmpty()) {
        _phase = Applying;
        setControlsEnabled(false);
        _okButton->setEnabled(false);
        _cancelButton->setEnabled(false);
        _progress->setRange(0, 0);
        _statusLabel->setText(tr("Deleting torrent..."));

        _worker->resetCancel();
        _worker->setTask(Worker::Cleanup);
        _worker->start();
    } else {
        _phase = Done;
        close();
    }
}

void AddDialog::closeEvent(QCloseEvent *e)
{
    if (_phase == Done) {
        e->accept();
        return;
    }
    if (_phase == Applying) {
        e->ignore();
        return;
    }
    e->ignore();
    onCancel();
}

void AddDialog::reject()
{
    if (_phase == Done) {
        QDialog::reject();
        return;
    }
    if (_phase == Applying) {
        return;
    }
    onCancel();
}

void AddDialog::scheduleClose()
{
    if (_autoCloseTimer)
        return;
    _autoCloseTimer = new QTimer(this);
    _autoCloseTimer->setSingleShot(true);
    connect(_autoCloseTimer, &QTimer::timeout, this, [this] {
        _phase = Done;
        close();
    });
    _autoCloseTimer->start(qMax(500, _cfg.autoCloseMs));
}

void AddDialog::setControlsEnabled(bool on)
{
    _tree->setEnabled(on);
    _categoryBox->setEnabled(on);
    _tagsEdit->setEnabled(on);
    _pathEdit->setEnabled(on);
    _forceBox->setEnabled(on);
    _okButton->setEnabled(on);
}

void AddDialog::showError(const QString &msg)
{
    _progress->setRange(0, 100);
    _progress->setValue(0);
    _statusLabel->setText(tr("Error: %1").arg(msg));
    _cancelButton->setEnabled(true);
    QMessageBox::critical(this, QString::fromUtf8(kAppTitle), msg);
}
