#include "adddialog.h"
#include "config.h"
#include "format.h"
#include "logger.h"
#include "worker.h"

#include <QApplication>
#include <QCloseEvent>
#include <QFileIconProvider>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QStyleOptionProgressBar>
#include <QVBoxLayout>
#include <QMap>

static const char *kAppTitle = "qt-magnet";

namespace {
constexpr int kColIndex    = 0;
constexpr int kColFile     = 1;
constexpr int kColSize     = 2;
constexpr int kColProgress = 3;
constexpr int kColPriority = 4;

constexpr int kColWidthIndex    = 45;
constexpr int kColWidthFile     = 280;
constexpr int kColWidthSize     = 95;
constexpr int kColWidthProgress = 90;
constexpr int kColWidthPriority = 110;

class FileTreeItem : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;
    bool operator<(const QTreeWidgetItem &other) const override {
        int col = treeWidget() ? treeWidget()->sortColumn() : 0;
        bool isAsc = treeWidget() ? (treeWidget()->header()->sortIndicatorOrder() == Qt::AscendingOrder) : true;
        bool thisIsFile = data(kColFile, IsFileRole).toBool();
        bool otherIsFile = other.data(kColFile, IsFileRole).toBool();
        if (!thisIsFile && otherIsFile)
            return isAsc;
        if (thisIsFile && !otherIsFile)
            return !isAsc;

        if (col == kColIndex) {
            return data(kColFile, FileIndexRole).toInt() < other.data(kColFile, FileIndexRole).toInt();
        }
        if (col == kColSize) {
            return data(kColFile, FileSizeRole).toLongLong() < other.data(kColFile, FileSizeRole).toLongLong();
        }
        if (col == kColProgress) {
            return data(kColFile, ProgressRole).toDouble() < other.data(kColFile, ProgressRole).toDouble();
        }
        if (col == kColPriority) {
            return data(kColFile, PriorityRole).toInt() < other.data(kColFile, PriorityRole).toInt();
        }
        return text(col).localeAwareCompare(other.text(col)) < 0;
    }
};

class ProgressBarDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        opt.text.clear();
        const QWidget *widget = opt.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

        QModelIndex fileIndex = index.sibling(index.row(), kColFile);
        bool isFile = fileIndex.data(IsFileRole).toBool();
        QVariant progVal = fileIndex.data(ProgressRole);
        if (!isFile && !progVal.isValid()) {
            return;
        }

        double prog = progVal.toDouble();
        if (prog < 0.0) prog = 0.0;
        if (prog > 1.0) prog = 1.0;

        QStyleOptionProgressBar barOpt;
        barOpt.rect = opt.rect.adjusted(4, 2, -4, -2);
        barOpt.minimum = 0;
        barOpt.maximum = 1000;
        barOpt.progress = qRound(prog * 1000.0);
        QString text = (prog >= 1.0) ? QStringLiteral("100%")
                    : (prog <= 0.0) ? QStringLiteral("0%")
                    : QString::number(prog * 100.0, 'f', 1) + QLatin1Char('%');
        barOpt.text = text;
        barOpt.textVisible = true;
        barOpt.textAlignment = Qt::AlignCenter;
        barOpt.state = QStyle::State_Enabled | QStyle::State_Horizontal;

        style->drawControl(QStyle::CE_ProgressBar, &barOpt, painter, widget);

        painter->save();
        painter->setFont(opt.font);
        bool darkTheme = (opt.palette.window().color().lightness() < 128);
        QColor textColor = (opt.state & QStyle::State_Selected)
            ? opt.palette.highlightedText().color()
            : (darkTheme ? Qt::white : QColor(30, 30, 30));
        painter->setPen(textColor);
        painter->drawText(barOpt.rect, Qt::AlignCenter, text);
        painter->restore();
    }
};

} // namespace

AddDialog::AddDialog(const Config &cfg, const TorrentPayload &payload, bool quick, QWidget *parent)
    : QDialog(parent), _cfg(cfg), _payload(payload), _quick(quick)
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
    if (_quick || !_tree || !_tree->header())
        return;
    Config c = Config::load();
    c.treeHeaderState = QString::fromLatin1(_tree->header()->saveState().toBase64());
    try { c.save(); } catch (const std::exception &ex) { Log::write(QStringLiteral("Failed to save header state: %1").arg(QString::fromUtf8(ex.what()))); }
}

void AddDialog::buildUi()
{
    updateTitle();
    resize(720, 620);
    setMinimumSize(560, 480);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    auto *topBar = new QHBoxLayout();
    topBar->setContentsMargins(12, 10, 12, 2);

    _nameLabel = new QLabel(_payload.prettyName(), this);
    _nameLabel->setTextFormat(Qt::PlainText);
    _nameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    QFont f = _nameLabel->font();
    f.setBold(true);
    f.setPointSizeF(f.pointSizeF() + 1.5);
    _nameLabel->setFont(f);
    _nameLabel->setWordWrap(true);
    _nameLabel->setMinimumWidth(1);
    topBar->addWidget(_nameLabel, 1);

    _versionBadge = new QLabel(QStringLiteral("[%1]").arg(_payload.versionString()), this);
    _versionBadge->setStyleSheet(QStringLiteral(
        "QLabel { background-color: #2b5b84; color: #ffffff; border-radius: 4px; padding: 2px 7px; font-weight: bold; font-size: 11px; }"));
    QString tip = tr("Protocol: %1\nHash (v1): %2").arg(_payload.versionString(), _payload.hash());
    if (!_payload.hashV2().isEmpty())
        tip += tr("\nHash (v2): %1").arg(_payload.hashV2());
    _versionBadge->setToolTip(tip);
    topBar->addWidget(_versionBadge, 0, Qt::AlignVCenter);
    mainLayout->addLayout(topBar);

    _filterEdit = new QLineEdit(this);
    _filterEdit->setPlaceholderText(tr("Filter files..."));
    _filterEdit->setClearButtonEnabled(true);
    auto *filterWrap = new QHBoxLayout();
    filterWrap->setContentsMargins(12, 2, 12, 4);
    filterWrap->addWidget(_filterEdit);
    mainLayout->addLayout(filterWrap);

    connect(_filterEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        QString filter = text.trimmed();
        _blockTreeSignals = true;
        std::function<bool(QTreeWidgetItem *)> filterItem = [&](QTreeWidgetItem *item) -> bool {
            bool isFile = item->data(kColFile, IsFileRole).toBool();
            if (isFile) {
                QString name = item->data(kColFile, BaseNameRole).toString();
                bool matches = filter.isEmpty() || name.contains(filter, Qt::CaseInsensitive);
                item->setHidden(!matches);
                return matches;
            }
            bool anyChildVisible = false;
            for (int c = 0; c < item->childCount(); ++c) {
                if (filterItem(item->child(c)))
                    anyChildVisible = true;
            }
            item->setHidden(!anyChildVisible);
            if (anyChildVisible && !filter.isEmpty())
                item->setExpanded(true);
            return anyChildVisible;
        };
        for (int i = 0; i < _tree->topLevelItemCount(); ++i)
            filterItem(_tree->topLevelItem(i));
        _blockTreeSignals = false;
    });

    _tree = new QTreeWidget(this);
    _tree->setHeaderLabels({tr("#"), tr("File"), tr("Size"), tr("Progress"), tr("Priority")});
    _tree->setTreePosition(kColFile);
    _tree->header()->setSectionsMovable(true);
    _tree->header()->setSectionResizeMode(QHeaderView::Interactive);
    _tree->header()->setStretchLastSection(false);
    _tree->header()->setContextMenuPolicy(Qt::CustomContextMenu);
    _tree->setSortingEnabled(true);
    _tree->setRootIsDecorated(true);
    _tree->setAlternatingRowColors(true);
    _tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    _tree->setContextMenuPolicy(Qt::CustomContextMenu);

    _tree->setItemDelegateForColumn(kColProgress, new ProgressBarDelegate(_tree));

    _tree->setColumnWidth(kColIndex, kColWidthIndex);
    _tree->setColumnWidth(kColFile, kColWidthFile);
    _tree->setColumnWidth(kColSize, kColWidthSize);
    _tree->setColumnWidth(kColProgress, kColWidthProgress);
    _tree->setColumnWidth(kColPriority, kColWidthPriority);

    _tree->sortByColumn(kColIndex, Qt::AscendingOrder);

    // Enforce '#' as first column and 'File' as second column:
    if (_tree->header()->visualIndex(kColIndex) != 0)
        _tree->header()->moveSection(_tree->header()->visualIndex(kColIndex), 0);
    if (_tree->header()->visualIndex(kColFile) != 1)
        _tree->header()->moveSection(_tree->header()->visualIndex(kColFile), 1);

    // By default, hide Progress and Priority until server check confirms torrent exists:
    _tree->setColumnHidden(kColProgress, true);
    _tree->setColumnHidden(kColPriority, true);

    connect(_tree->header(), &QHeaderView::customContextMenuRequested, this, &AddDialog::onHeaderContextMenu);
    connect(_tree, &QTreeWidget::customContextMenuRequested, this, &AddDialog::onTreeContextMenu);
    connect(_tree, &QTreeWidget::itemChanged, this, &AddDialog::onTreeItemChanged);
    connect(_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        if (item->data(kColFile, IsFileRole).toBool())
            item->setCheckState(kColFile, item->checkState(kColFile) == Qt::Checked ? Qt::Unchecked : Qt::Checked);
    });
    mainLayout->addWidget(_tree, 1);

    _progress = new QProgressBar(this);
    _progress->setTextVisible(true);
    _progress->setAlignment(Qt::AlignCenter);
    _progress->setRange(0, 0);
    auto *progWrap = new QHBoxLayout();
    progWrap->setContentsMargins(12, 4, 12, 4);
    progWrap->addWidget(_progress);
    mainLayout->addLayout(progWrap);

    _pollTimer = new QTimer(this);
    _pollTimer->setInterval(1000);
    connect(_pollTimer, &QTimer::timeout, this, &AddDialog::onPollTick);

    _treeMenu = new QMenu(this);
    _treeMenu->addAction(tr("Download (Normal)"), this, [this]{ setPriority(1); });
    _actHigh = _treeMenu->addAction(tr("High"), this, [this]{ setPriority(6); });
    _actMaximum = _treeMenu->addAction(tr("Maximum"), this, [this]{ setPriority(7); });
    _treeMenu->addSeparator();
    _treeMenu->addAction(tr("Do not download"), this, [this]{ setPriority(0); });
    _treeMenu->addSeparator();
    _treeMenu->addAction(tr("Check all"), this, [this]{ checkAll(true); });
    _treeMenu->addAction(tr("Uncheck all"), this, [this]{ checkAll(false); });
    _treeMenu->addAction(tr("Invert check"), this, [this]{
        _blockTreeSignals = true;
        forEachFileInTree(_tree, [](QTreeWidgetItem *item) {
            bool on = item->checkState(kColFile) != Qt::Checked;
            setSubtreeCheck(item, on);
            refreshAncestors(item);
        });
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

    _statusLabel = new QLabel(tr("Connecting..."), this);
    _statusLabel->setTextFormat(Qt::PlainText);
    bottomLayout->addWidget(_statusLabel, 1, Qt::AlignVCenter | Qt::AlignLeft);

    auto *buttonsLayout = new QHBoxLayout();
    buttonsLayout->setSpacing(8);

    _okButton = new QPushButton(tr("Add"), this);
    _okButton->setEnabled(false);
    _okButton->setDefault(true);
    connect(_okButton, &QPushButton::clicked, this, &AddDialog::onOk);
    buttonsLayout->addWidget(_okButton);

    _cancelButton = new QPushButton(tr("Cancel"), this);
    _cancelButton->setAutoDefault(false);
    connect(_cancelButton, &QPushButton::clicked, this, &AddDialog::onCancel);
    buttonsLayout->addWidget(_cancelButton);

    bottomLayout->addLayout(buttonsLayout);
    mainLayout->addLayout(bottomLayout);

    if (_quick) {
        _tree->hide();
        if (_filterEdit)
            _filterEdit->hide();
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
    _worker = new Worker(_cfg, _payload, _quick, nullptr);
    _worker->setTask(Worker::Prepare);
    connect(_worker, &Worker::status, this, &AddDialog::onStatus, Qt::QueuedConnection);
    connect(_worker, &Worker::prepareFinished, this, &AddDialog::onPrepareFinished, Qt::QueuedConnection);
    connect(_worker, &Worker::quickFinished, this, &AddDialog::onQuickFinished, Qt::QueuedConnection);
    connect(_worker, &Worker::applyFinished, this, &AddDialog::onApplyFinished, Qt::QueuedConnection);
    connect(_worker, &Worker::cleanupFinished, this, &AddDialog::onCleanupFinished, Qt::QueuedConnection);
    connect(_worker, &Worker::pollFinished, this, &AddDialog::onPollFinished, Qt::QueuedConnection);
    _worker->start();
}

void AddDialog::onStatus(const QString &text)
{
    _statusLabel->setText(text);
}

void AddDialog::onPrepareFinished(bool success, const QString &error)
{
    updateTitle();
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
    _worker->existed = true;

    _tree->setColumnHidden(kColProgress, false);
    _tree->setColumnHidden(kColPriority, false);

    _statusLabel->setText(_worker->resultOk ? tr("Done.") : tr("Done (status unverified)."));
    _okButton->hide();
    _cancelButton->setText(tr("Close"));
    _cancelButton->setEnabled(true);
    _cancelButton->setDefault(true);

    if (_cfg.autoCloseOnSuccess && _worker->resultOk) {
        scheduleClose();
    } else {
        if (_pollTimer)
            _pollTimer->start();
        onPollTick();
    }
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

    if (_worker && _worker->torrentInfo && !_worker->torrentInfo->name.isEmpty()) {
        _nameLabel->setText(_worker->torrentInfo->name);
    } else {
        _nameLabel->setText(_payload.prettyName());
    }

    updateTitle();
    bool existed = (_worker && _worker->existed);
    _tree->setColumnHidden(kColProgress, !existed);
    _tree->setColumnHidden(kColPriority, !existed);
    if (existed && _pollTimer) {
        _pollTimer->start();
    }

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

    const QSet<int> selectedIndices = _payload.selectedIndices();
    if (!selectedIndices.isEmpty() && !files.isEmpty()) {
        _blockTreeSignals = true;
        forEachFileInTree(_tree, [&selectedIndices](QTreeWidgetItem *item) {
            int idx = item->data(kColFile, FileIndexRole).toInt();
            bool wanted = selectedIndices.contains(idx);
            item->setCheckState(kColFile, wanted ? Qt::Checked : Qt::Unchecked);
            int newPrio = wanted ? 1 : 0;
            item->setData(kColFile, PriorityRole, newPrio);
            item->setData(kColIndex, PriorityRole, newPrio);
            updateFileText(item);
        });
        for (int i = 0; i < _tree->topLevelItemCount(); ++i)
            refreshAncestors(_tree->topLevelItem(i));
        _blockTreeSignals = false;
    }

    updateSummary();
}

void AddDialog::buildTree(const QVector<TorrentFile> &files)
{
    _blockTreeSignals = true;
    _tree->setSortingEnabled(false);
    _tree->clear();
    QMap<QString, QTreeWidgetItem *> folders;

    QFileIconProvider iconProvider;
    QIcon folderIcon = QApplication::style()->standardIcon(QStyle::SP_DirIcon);
    QIcon fallbackFileIcon = QApplication::style()->standardIcon(QStyle::SP_FileIcon);

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
                item->setText(kColIndex, QString::number(f.index + 1));
                item->setText(kColFile, parts[i]);
                item->setData(kColFile, FileIndexRole, f.index);
                item->setData(kColIndex, FileIndexRole, f.index);
                item->setData(kColFile, FileSizeRole, f.size);
                item->setData(kColIndex, FileSizeRole, f.size);
                item->setData(kColFile, PriorityRole, f.priority);
                item->setData(kColIndex, PriorityRole, f.priority);
                item->setData(kColFile, OriginalPriorityRole, f.priority);
                item->setData(kColIndex, OriginalPriorityRole, f.priority);
                item->setData(kColFile, PrioBeforeUncheckRole, f.priority == 0 ? 1 : f.priority);
                item->setData(kColIndex, PrioBeforeUncheckRole, f.priority == 0 ? 1 : f.priority);
                item->setData(kColFile, ProgressRole, f.progress);
                item->setData(kColIndex, ProgressRole, f.progress);
                item->setData(kColProgress, ProgressRole, f.progress);
                item->setData(kColFile, IsFileRole, true);
                item->setData(kColIndex, IsFileRole, true);
                item->setData(kColFile, BaseNameRole, parts[i]);
                item->setData(kColIndex, BaseNameRole, parts[i]);
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(kColFile, f.priority == 0 ? Qt::Unchecked : Qt::Checked);

                QFileInfo fi(parts[i]);
                QIcon fiIcon = iconProvider.icon(fi);
                item->setIcon(kColFile, fiIcon.isNull() ? fallbackFileIcon : fiIcon);

                if (parent)
                    parent->addChild(item);
                else
                    _tree->addTopLevelItem(item);
                updateFileText(item);
            } else {
                QTreeWidgetItem *folder = folders.value(acc);
                if (!folder) {
                    folder = new FileTreeItem();
                    folder->setText(kColIndex, QString());
                    folder->setText(kColFile, parts[i]);
                    folder->setData(kColFile, IsFileRole, false);
                    folder->setData(kColIndex, IsFileRole, false);
                    folder->setData(kColFile, BaseNameRole, parts[i]);
                    folder->setData(kColIndex, BaseNameRole, parts[i]);
                    folder->setFlags(folder->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
                    folder->setCheckState(kColFile, Qt::Checked);
                    folder->setIcon(kColFile, folderIcon);
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
        if (!_tree->topLevelItem(i)->data(kColFile, IsFileRole).toBool())
            _tree->topLevelItem(i)->setCheckState(kColFile, computeFolderState(_tree->topLevelItem(i)));
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
    if (item->data(kColFile, IsFileRole).toBool()) {
        qint64 sz = item->data(kColFile, FileSizeRole).toLongLong();
        double p = item->data(kColFile, ProgressRole).toDouble();
        return { sz, 1, sz * p };
    }

    qint64 sum = 0;
    int fileCount = 0;
    double sumDone = 0.0;
    for (int i = 0; i < item->childCount(); ++i) {
        FolderStats childStats = computeFolder(item->child(i));
        sum += childStats.size;
        fileCount += childStats.fileCount;
        sumDone += childStats.doneBytes;
    }
    item->setData(kColFile, FileSizeRole, sum);
    item->setData(kColIndex, FileSizeRole, sum);
    double folderProg = (sum > 0) ? (sumDone / static_cast<double>(sum)) : 0.0;
    item->setData(kColFile, ProgressRole, folderProg);
    item->setData(kColIndex, ProgressRole, folderProg);
    item->setData(kColProgress, ProgressRole, folderProg);

    QString baseName = item->data(kColFile, BaseNameRole).toString();
    if (baseName.isEmpty())
        baseName = item->text(kColFile);
    const QString countStr = tr("%n file(s)", "", fileCount);
    item->setText(kColIndex, QString());
    item->setText(kColFile, baseName);
    item->setText(kColSize, Format::size(sum) + QStringLiteral(" (") + countStr + QStringLiteral(")"));
    item->setText(kColProgress, QString());
    item->setText(kColPriority, QString());
    return { sum, fileCount, sumDone };
}

void AddDialog::onHeaderContextMenu(const QPoint &pos)
{
    bool existed = (_worker && _worker->existed);
    QMenu menu(this);
    menu.setTitle(tr("Columns"));

    QHeaderView *hdr = _tree->header();
    int count = hdr->count();

    for (int i = 0; i < count; ++i) {
        if (!existed && (i == kColProgress || i == kColPriority))
            continue;
        QString title = _tree->headerItem()->text(i);
        if (title.isEmpty())
            continue;
        auto *act = menu.addAction(title);
        act->setCheckable(true);
        act->setChecked(!hdr->isSectionHidden(i));
        if (i == kColFile || i == kColIndex) {
            act->setEnabled(false);
        } else {
            connect(act, &QAction::toggled, this, [hdr, i](bool visible) {
                hdr->setSectionHidden(i, !visible);
            });
        }
    }

    menu.addSeparator();
    menu.addAction(tr("Reset columns"), this, [this, hdr, existed]() {
        for (int i = 0; i < hdr->count(); ++i) {
            bool hide = (!existed && (i == kColProgress || i == kColPriority));
            hdr->setSectionHidden(i, hide);
        }
        _tree->setColumnWidth(kColIndex, kColWidthIndex);
        _tree->setColumnWidth(kColFile, kColWidthFile);
        _tree->setColumnWidth(kColSize, kColWidthSize);
        _tree->setColumnWidth(kColProgress, kColWidthProgress);
        _tree->setColumnWidth(kColPriority, kColWidthPriority);
        if (hdr->visualIndex(kColIndex) != 0)
            hdr->moveSection(hdr->visualIndex(kColIndex), 0);
        if (hdr->visualIndex(kColFile) != 1)
            hdr->moveSection(hdr->visualIndex(kColFile), 1);
    });

    menu.exec(_tree->header()->mapToGlobal(pos));
}


void AddDialog::onTreeItemChanged(QTreeWidgetItem *item, int column)
{
    if (_blockTreeSignals || column != kColFile)
        return;

    _blockTreeSignals = true;
    bool isFile = item->data(kColFile, IsFileRole).toBool();
    if (isFile) {
        int prio = item->data(kColFile, PriorityRole).toInt();
        if (item->checkState(kColFile) == Qt::Unchecked) {
            if (prio != 0) {
                item->setData(kColFile, PrioBeforeUncheckRole, prio);
                item->setData(kColIndex, PrioBeforeUncheckRole, prio);
                item->setData(kColFile, PriorityRole, 0);
                item->setData(kColIndex, PriorityRole, 0);
            }
        } else {
            if (prio == 0) {
                int prev = item->data(kColFile, PrioBeforeUncheckRole).toInt();
                int newPrio = prev > 0 ? prev : 1;
                item->setData(kColFile, PriorityRole, newPrio);
                item->setData(kColIndex, PriorityRole, newPrio);
            }
        }
        updateFileText(item);
        refreshAncestors(item);
    } else {
        if (item->checkState(kColFile) != Qt::PartiallyChecked) {
            bool on = (item->checkState(kColFile) == Qt::Checked);
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
    bool existed = (_worker && _worker->existed);
    if (_actHigh) _actHigh->setVisible(existed);
    if (_actMaximum) _actMaximum->setVisible(existed);
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
    bool isFile = item->data(kColFile, IsFileRole).toBool();
    if (isFile) {
        int prio = item->data(kColFile, PriorityRole).toInt();
        if (on) {
            if (prio == 0) {
                int prev = item->data(kColFile, PrioBeforeUncheckRole).toInt();
                int newPrio = prev > 0 ? prev : 1;
                item->setData(kColFile, PriorityRole, newPrio);
                item->setData(kColIndex, PriorityRole, newPrio);
            }
        } else {
            if (prio != 0) {
                item->setData(kColFile, PrioBeforeUncheckRole, prio);
                item->setData(kColIndex, PrioBeforeUncheckRole, prio);
            }
            item->setData(kColFile, PriorityRole, 0);
            item->setData(kColIndex, PriorityRole, 0);
        }
        updateFileText(item);
    }
    item->setCheckState(kColFile, on ? Qt::Checked : Qt::Unchecked);
    for (int i = 0; i < item->childCount(); ++i)
        setSubtreeCheck(item->child(i), on);
}

void AddDialog::setSubtreePriority(QTreeWidgetItem *item, int priority)
{
    bool isFile = item->data(kColFile, IsFileRole).toBool();
    if (isFile) {
        item->setData(kColFile, PriorityRole, priority);
        item->setData(kColIndex, PriorityRole, priority);
        if (priority != 0) {
            item->setData(kColFile, PrioBeforeUncheckRole, priority);
            item->setData(kColIndex, PrioBeforeUncheckRole, priority);
        }
        item->setCheckState(kColFile, priority == 0 ? Qt::Unchecked : Qt::Checked);
        updateFileText(item);
    }
    for (int i = 0; i < item->childCount(); ++i)
        setSubtreePriority(item->child(i), priority);
    if (item->childCount() > 0)
        item->setCheckState(kColFile, computeFolderState(item));
}

void AddDialog::refreshAncestors(QTreeWidgetItem *item)
{
    QTreeWidgetItem *p = item->parent();
    while (p) {
        p->setCheckState(kColFile, computeFolderState(p));
        p = p->parent();
    }
}

Qt::CheckState AddDialog::computeFolderState(QTreeWidgetItem *folder)
{
    bool anyOn = false, anyOff = false;
    for (int i = 0; i < folder->childCount(); ++i) {
        QTreeWidgetItem *child = folder->child(i);
        Qt::CheckState st = child->childCount() > 0 ? computeFolderState(child) : child->checkState(kColFile);
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
    if (!item->data(kColFile, IsFileRole).toBool())
        return;
    QString baseName = item->data(kColFile, BaseNameRole).toString();
    if (baseName.isEmpty())
        baseName = item->text(kColFile);
    if (baseName.isEmpty())
        return;
    qint64 sz = item->data(kColFile, FileSizeRole).toLongLong();
    int prio = item->data(kColFile, PriorityRole).toInt();
    double prog = item->data(kColFile, ProgressRole).toDouble();
    int idx = item->data(kColFile, FileIndexRole).toInt();

    item->setText(kColIndex, QString::number(idx + 1));
    item->setText(kColFile, baseName);
    item->setText(kColSize, Format::size(sz));
    item->setText(kColProgress, prog <= 0.0 ? QStringLiteral("0%") : QString::number(qRound(prog * 100.0)) + QLatin1Char('%'));
    item->setText(kColPriority, Format::priorityName(prio));

    if (prio == 0) {
        item->setForeground(kColPriority, QBrush(QColor(140, 140, 140)));
    } else if (prio >= 6) {
        item->setForeground(kColPriority, QBrush(QColor(220, 120, 0)));
    } else {
        item->setData(kColPriority, Qt::ForegroundRole, QVariant());
    }
}

void AddDialog::forEachFile(QTreeWidgetItem *item, const std::function<void(QTreeWidgetItem *)> &fn)
{
    if (item->data(kColFile, IsFileRole).toBool())
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
        qint64 sz = item->data(kColFile, FileSizeRole).toLongLong();
        int prio = item->data(kColFile, PriorityRole).toInt();
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
        int prio = item->data(kColFile, PriorityRole).toInt();
        int orig = item->data(kColFile, OriginalPriorityRole).toInt();
        int idx = item->data(kColFile, FileIndexRole).toInt();
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
    p.initialTags = _worker->initialTags;
    p.trackers = _payload.trackers();

    _worker->applyParams = p;
    if (_worker->isRunning())
        _worker->wait();
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
    if (_pollTimer)
        _pollTimer->stop();
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
    if (_pollTimer)
        _pollTimer->stop();
    if (_autoCloseTimer)
        _autoCloseTimer->stop();

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
    if (_pollTimer)
        _pollTimer->stop();
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

void AddDialog::updateTitle()
{
    QString verTag;
    TorrentVersion ver = _payload.version();
    if (_worker && _worker->torrentInfo) {
        if (!_worker->torrentInfo->infoHashV2.isEmpty() && !_worker->torrentInfo->infoHashV1.isEmpty())
            ver = TorrentVersion::Hybrid;
        else if (!_worker->torrentInfo->infoHashV2.isEmpty())
            ver = TorrentVersion::V2;
    }
    switch (ver) {
    case TorrentVersion::V2:
        verTag = QStringLiteral("[v2]");
        break;
    case TorrentVersion::Hybrid:
        verTag = QStringLiteral("[Hybrid]");
        break;
    default:
        verTag = QStringLiteral("[v1]");
        break;
    }

    QString name = (_worker && _worker->torrentInfo && !_worker->torrentInfo->name.isEmpty())
                       ? _worker->torrentInfo->name
                       : _payload.prettyName();
    if (!name.isEmpty() && name != QLatin1String("torrent")) {
        setWindowTitle(QStringLiteral("qt-magnet %1 - %2").arg(verTag, name));
    } else {
        setWindowTitle(QStringLiteral("qt-magnet %1").arg(verTag));
    }
}

void AddDialog::onPollTick()
{
    if (!_worker || _worker->isRunning() || _worker->wasCancelled())
        return;
    if (!_worker->existed || (_worker->hash.isEmpty() && _payload.hash().isEmpty()))
        return;

    _worker->setTask(Worker::Poll);
    _worker->start();
}

void AddDialog::onPollFinished(bool success)
{
    if (!success || !_worker)
        return;

    if (_worker->pollInfo.has_value()) {
        const TorrentInfo &ti = _worker->pollInfo.value();
        double totalProg = ti.progress;
        _progress->setRange(0, 1000);
        _progress->setValue(qRound(totalProg * 1000.0));
        _progress->setFormat(QString::number(totalProg * 100.0, 'f', 1) + QLatin1Char('%'));

        QString stateStr = QbtClient::stateString(ti.state);
        _statusLabel->setText(QStringLiteral("%1 (%2)")
            .arg(stateStr, QString::number(totalProg * 100.0, 'f', 1) + QLatin1Char('%')));
    }

    if (!_worker->pollFiles.isEmpty()) {
        QMap<int, double> progressByIndex;
        for (const TorrentFile &f : _worker->pollFiles)
            progressByIndex[f.index] = f.progress;

        _blockTreeSignals = true;
        forEachFileInTree(_tree, [&](QTreeWidgetItem *item) {
            int idx = item->data(kColFile, FileIndexRole).toInt();
            if (progressByIndex.contains(idx)) {
                double p = progressByIndex.value(idx);
                item->setData(kColFile, ProgressRole, p);
                item->setData(kColIndex, ProgressRole, p);
                item->setData(kColProgress, ProgressRole, p);
                updateFileText(item);
            }
        });
        for (int i = 0; i < _tree->topLevelItemCount(); ++i)
            computeFolder(_tree->topLevelItem(i));
        _blockTreeSignals = false;
        _tree->viewport()->update();
    }
}

