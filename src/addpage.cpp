#include "addpage.h"
#include "config.h"
#include "darktheme.h"
#include "format.h"
#include "logger.h"
#include "passkey.h"
#include "worker.h"

#include <QApplication>
#include <QEvent>
#include <QFileIconProvider>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStyleOptionProgressBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QMap>

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

constexpr int kPollIntervalMs = 1000;
// Progress values are stored as 0..kProgressScale.
constexpr int kProgressScale = 1000;

// Per-item roles of the file tree, stored on the File column.
enum TreeDataRole {
    FileIndexRole        = Qt::UserRole,
    FileSizeRole         = Qt::UserRole + 1,
    PriorityRole         = Qt::UserRole + 2,
    OriginalPriorityRole = Qt::UserRole + 3,
    PrioBeforeUncheckRole= Qt::UserRole + 4,
    BaseNameRole         = Qt::UserRole + 5,
    ProgressRole         = Qt::UserRole + 6,
    RelPathRole          = Qt::UserRole + 7
};

class FileTreeItem : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;
    bool operator<(const QTreeWidgetItem &other) const override {
        const int col = treeWidget() ? treeWidget()->sortColumn() : 0;
        if (col == kColIndex) {
            int thisIdx = data(kColFile, FileIndexRole).toInt();
            int otherIdx = other.data(kColFile, FileIndexRole).toInt();
            if (thisIdx != otherIdx)
                return thisIdx < otherIdx;
        } else if (col == kColSize) {
            qint64 thisSz = data(kColFile, FileSizeRole).toLongLong();
            qint64 otherSz = other.data(kColFile, FileSizeRole).toLongLong();
            if (thisSz != otherSz)
                return thisSz < otherSz;
        } else if (col == kColProgress) {
            double thisProg = data(kColFile, ProgressRole).toDouble();
            double otherProg = other.data(kColFile, ProgressRole).toDouble();
            if (qAbs(thisProg - otherProg) > 1e-6)
                return thisProg < otherProg;
        } else if (col == kColPriority) {
            int thisPrio = data(kColFile, PriorityRole).toInt();
            int otherPrio = other.data(kColFile, PriorityRole).toInt();
            if (thisPrio != otherPrio)
                return thisPrio < otherPrio;
        } else {
            int cmp = text(col).localeAwareCompare(other.text(col));
            if (cmp != 0)
                return cmp < 0;
        }
        return text(kColFile).localeAwareCompare(other.text(kColFile)) < 0;
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

        const QModelIndex fileIndex = index.sibling(index.row(), kColFile);
        const QVariant progVal = fileIndex.data(ProgressRole);
        if (!progVal.isValid())
            return;

        double prog = progVal.toDouble();
        if (prog < 0.0) prog = 0.0;
        if (prog > 1.0) prog = 1.0;

        QStyleOptionProgressBar barOpt;
        barOpt.rect = opt.rect.adjusted(4, 2, -4, -2);
        barOpt.minimum = 0;
        barOpt.maximum = kProgressScale;
        barOpt.progress = qRound(prog * kProgressScale);
        QString text = (prog >= 1.0) ? QStringLiteral("100%")
                    : (prog <= 0.0) ? QStringLiteral("0%")
                    : QString::number(prog * 100.0, 'f', 1) + QLatin1Char('%');
        barOpt.textVisible = false;
        barOpt.palette = opt.palette;
        barOpt.fontMetrics = opt.fontMetrics;
        barOpt.state = QStyle::State_Enabled | QStyle::State_Horizontal;

        style->drawControl(QStyle::CE_ProgressBar, &barOpt, painter, widget);

        painter->save();
        painter->setFont(opt.font);
        bool darkTheme = DarkTheme::isDark();
        QColor unfilledColor = darkTheme ? Qt::white : QColor(30, 30, 30);
        QColor filledColor = Qt::white;

        int splitX = barOpt.rect.left() + qRound(barOpt.rect.width() * prog);
        QRect filledRect(barOpt.rect.left(), barOpt.rect.top(), splitX - barOpt.rect.left(), barOpt.rect.height());
        QRect unfilledRect(splitX, barOpt.rect.top(), barOpt.rect.right() - splitX + 1, barOpt.rect.height());

        if (!filledRect.isEmpty() && prog > 0.0) {
            painter->save();
            painter->setClipRect(filledRect, Qt::IntersectClip);
            painter->setPen(filledColor);
            painter->drawText(barOpt.rect, Qt::AlignCenter, text);
            painter->restore();
        }

        if (!unfilledRect.isEmpty() && prog < 1.0) {
            painter->save();
            painter->setClipRect(unfilledRect, Qt::IntersectClip);
            painter->setPen(unfilledColor);
            painter->drawText(barOpt.rect, Qt::AlignCenter, text);
            painter->restore();
        }
        painter->restore();
    }
};

} // namespace

AddPage::AddPage(const Config &cfg, const TorrentPayload &payload, bool quick, QWidget *parent)
    : QWidget(parent), _cfg(cfg), _payload(payload), _quick(quick)
{
    // Reload: another tab or the Settings dialog may have changed it since startup.
    try {
        _ownPk = Config::load().getRutrackerPasskey();
    } catch (const std::exception &ex) {
        Log::write(QStringLiteral("Failed to load rutracker passkey: %1").arg(QString::fromUtf8(ex.what())));
    }
    buildUi();
    QTimer::singleShot(0, this, &AddPage::startPrepare);
}

AddPage::~AddPage()
{
    saveHeaderState();
    if (_worker) {
        _worker->requestCancel();
        _worker->wait(3000);
        delete _worker;
    }
}

void AddPage::saveHeaderState()
{
    if (_quick || !_tree || !_tree->header())
        return;
    Config c = Config::load();
    c.treeHeaderState = QString::fromLatin1(_tree->header()->saveState().toBase64());
    try { c.save(); } catch (const std::exception &ex) { Log::write(QStringLiteral("Failed to save header state: %1").arg(QString::fromUtf8(ex.what()))); }
}

void AddPage::buildUi()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    auto *headerLayout = new QVBoxLayout();
    headerLayout->setContentsMargins(12, 10, 12, 4);
    headerLayout->setSpacing(2);

    _nameLabel = new QLabel(this);
    _nameLabel->setTextFormat(Qt::PlainText);
    _nameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    QFont f = _nameLabel->font();
    f.setBold(true);
    f.setPointSizeF(f.pointSizeF() + 1.5);
    _nameLabel->setFont(f);
    _nameLabel->setWordWrap(false);
    headerLayout->addWidget(_nameLabel);

    auto *metaRow = new QHBoxLayout();
    metaRow->setSpacing(8);
    _versionBadge = new QLabel(QStringLiteral("[%1]").arg(_payload.versionString()), this);
    QString tip = tr("Protocol: %1\nHash (v1): %2").arg(_payload.versionString(), _payload.hash());
    if (!_payload.hashV2().isEmpty())
        tip += tr("\nHash (v2): %1").arg(_payload.hashV2());
    _versionBadge->setToolTip(tip);
    metaRow->addWidget(_versionBadge, 0, Qt::AlignVCenter);
    _metaLabel = new QLabel(this);
    _metaLabel->setTextFormat(Qt::PlainText);
    _metaLabel->setWordWrap(false);
    metaRow->addWidget(_metaLabel, 1, Qt::AlignVCenter | Qt::AlignLeft);

    _learnPkButton = new QPushButton(tr("This is my passkey"), this);
    _learnPkButton->setToolTip(tr("Save this passkey as yours, so your rutracker "
                                  "upload is counted on your account."));
    _learnPkButton->hide();
    connect(_learnPkButton, &QPushButton::clicked, this, &AddPage::onLearnPasskey);
    metaRow->addWidget(_learnPkButton, 0, Qt::AlignVCenter);
    headerLayout->addLayout(metaRow);
    mainLayout->addLayout(headerLayout);
    updateMetaLine();
    updateNameLabel();

    _progress = new QProgressBar(this);
    _progress->setTextVisible(true);
    _progress->setAlignment(Qt::AlignCenter);
    _progress->setRange(0, 0);
    auto *progWrap = new QHBoxLayout();
    progWrap->setContentsMargins(12, 4, 12, 4);
    progWrap->addWidget(_progress);
    mainLayout->addLayout(progWrap);

    _pollTimer = new QTimer(this);
    _pollTimer->setInterval(kPollIntervalMs);
    connect(_pollTimer, &QTimer::timeout, this, &AddPage::onPollTick);

    _listBar = new QWidget(this);
    auto *barLayout = new QHBoxLayout(_listBar);
    barLayout->setContentsMargins(12, 4, 12, 4);
    barLayout->setSpacing(6);

    _chipScroll = new QScrollArea(_listBar);
    _chipScroll->setWidgetResizable(true);
    _chipScroll->setFrameShape(QFrame::NoFrame);
    _chipScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    _chipScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _chipScroll->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    auto *chipHost = new QWidget(_chipScroll);
    auto *chipLay = new QHBoxLayout(chipHost);
    chipLay->setContentsMargins(0, 0, 0, 0);
    chipLay->setSpacing(6);
    chipLay->addStretch(1);
    _chipLayout = chipLay;
    _chipScroll->setWidget(chipHost);
    _chipScroll->setFixedHeight(30);
    barLayout->addWidget(_chipScroll, 1);

    _allButton = new QPushButton(tr("All"), _listBar);
    connect(_allButton, &QPushButton::clicked, this, [this] { checkAll(true); });
    barLayout->addWidget(_allButton);

    _noneButton = new QPushButton(tr("None"), _listBar);
    connect(_noneButton, &QPushButton::clicked, this, [this] { checkAll(false); });
    barLayout->addWidget(_noneButton);

    _filterEdit = new QLineEdit(_listBar);
    _filterEdit->setPlaceholderText(tr("Filter files..."));
    _filterEdit->setClearButtonEnabled(true);
    _filterEdit->setFixedWidth(200);
    _filterEdit->setMinimumWidth(120);
    barLayout->addWidget(_filterEdit);
    mainLayout->addWidget(_listBar);

    connect(_filterEdit, &QLineEdit::textChanged, this, [this](const QString &) {
        applyRowVisibility();
    });

    _tree = new QTreeWidget(this);
    _tree->setHeaderLabels({tr("#"), tr("File"), tr("Size"), tr("Progress"), tr("Priority")});
    _tree->setRootIsDecorated(false);
    _tree->setIndentation(0);
    _tree->header()->setSectionsMovable(true);
    _tree->header()->setSectionResizeMode(QHeaderView::Interactive);
    _tree->header()->setStretchLastSection(false);
    _tree->header()->setContextMenuPolicy(Qt::CustomContextMenu);
    _tree->setSortingEnabled(true);
    _tree->setUniformRowHeights(true);
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

    // Enforce '#' as the first column and 'File' as the second.
    if (_tree->header()->visualIndex(kColIndex) != 0)
        _tree->header()->moveSection(_tree->header()->visualIndex(kColIndex), 0);
    if (_tree->header()->visualIndex(kColFile) != 1)
        _tree->header()->moveSection(_tree->header()->visualIndex(kColFile), 1);

    // By default, hide Progress and Priority until the server check
    // confirms the torrent exists.
    _tree->setColumnHidden(kColProgress, true);
    _tree->setColumnHidden(kColPriority, true);

    connect(_tree->header(), &QHeaderView::customContextMenuRequested, this, &AddPage::onHeaderContextMenu);
    connect(_tree, &QTreeWidget::customContextMenuRequested, this, &AddPage::onTreeContextMenu);
    connect(_tree, &QTreeWidget::itemChanged, this, &AddPage::onTreeItemChanged);
    connect(_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int column) {
        if (column == kColFile)
            item->setCheckState(kColFile, item->checkState(kColFile) == Qt::Checked ? Qt::Unchecked : Qt::Checked);
    });
    _tree->viewport()->installEventFilter(this);
    mainLayout->addWidget(_tree, 1);

    _tableMessage = new QLabel(_tree->viewport());
    _tableMessage->setAlignment(Qt::AlignCenter);
    _tableMessage->setWordWrap(true);
    _tableMessage->hide();

    _treeMenu = new QMenu(this);
    _treeMenu->addAction(tr("Download (Normal)"), this, [this]{ setPriority(1); });
    _actHigh = _treeMenu->addAction(tr("High"), this, [this]{ setPriority(6); });
    _actMaximum = _treeMenu->addAction(tr("Maximum"), this, [this]{ setPriority(7); });
    _treeMenu->addSeparator();
    _treeMenu->addAction(tr("Do not download"), this, [this]{ setPriority(0); });
    _treeMenu->addSeparator();
    _treeMenu->addAction(tr("Invert check (visible)"), this, [this]{ invertVisible(); });

    auto *optionsStrip = new QWidget(this);
    auto *optRow = new QHBoxLayout(optionsStrip);
    optRow->setContentsMargins(12, 6, 12, 6);
    optRow->setSpacing(8);

    auto makeCaption = [optionsStrip](const QString &text) {
        auto *l = new QLabel(text, optionsStrip);
        return l;
    };

    _categoryBox = new QComboBox(optionsStrip);
    _categoryBox->setEditable(true);
    _categoryBox->setPlaceholderText(tr("Category"));
    _tagsEdit = new QLineEdit(optionsStrip);
    _tagsEdit->setPlaceholderText(tr("Tags"));
    _tagsEdit->setClearButtonEnabled(true);
    _pathEdit = new QLineEdit(optionsStrip);
    _pathEdit->setPlaceholderText(tr("Save path"));
    _pathEdit->setClearButtonEnabled(true);
    _pathEdit->setToolTip(tr("Leave empty for the client's default path"));
    _forceBox = new QCheckBox(tr("Force start"), optionsStrip);
    _forceBox->setToolTip(tr("Start downloading immediately, bypassing queue limits"));
    _forceBox->setChecked(_cfg.forceStartDefault);

    optRow->addWidget(makeCaption(tr("Category:")));
    optRow->addWidget(_categoryBox, 2);
    optRow->addWidget(makeCaption(tr("Tags:")));
    optRow->addWidget(_tagsEdit, 2);
    optRow->addWidget(makeCaption(tr("Path:")));
    optRow->addWidget(_pathEdit, 4);
    optRow->addSpacing(12);
    optRow->addWidget(_forceBox);
    _optionsStrip = optionsStrip;
    mainLayout->addWidget(optionsStrip);

    _statusText = tr("Connecting...");

    if (_quick) {
        _listBar->hide();
        _tree->hide();
        optionsStrip->hide();
    } else {
        setControlsEnabled(false);
        // Compact while the metadata loads: the file table, the list bar and
        // the options strip appear with the file list.
        _compact = true;
        _listBar->hide();
        _tree->hide();
        _optionsStrip->hide();
    }
}

void AddPage::updateNameLabel()
{
    if (!_nameLabel)
        return;
    if (_fullName.isEmpty())
        _fullName = _payload.prettyName();
    const int w = qMax(80, _nameLabel->width() - 8);
    const QString elided = _nameLabel->fontMetrics().elidedText(_fullName, Qt::ElideMiddle, w);
    _nameLabel->setText(elided);
    _nameLabel->setToolTip(elided == _fullName ? QString() : _fullName);
}

void AddPage::updateMetaLine()
{
    if (!_metaLabel)
        return;
    const QString source = _payload.isFile() ? tr("torrent file") : tr("magnet link");
    const qint64 totalSize = (_worker && _worker->torrentInfo) ? _worker->torrentInfo->totalSize : 0;
    QStringList parts;
    if (totalSize > 0)
        parts << Format::size(totalSize) << source;
    else
        parts << source << tr("%n tracker(s)", "", _payload.trackers().size());

    // Whose rutracker passkey the announce URLs carry — decides whose
    // account the uploaded traffic is credited to.
    int ruAnnouncers = 0;
    QSet<QString> ruPks;
    const QVector<PassKey::Found> pks = PassKey::extract(_payload.trackers());
    for (const QString &t : _payload.trackers()) {
        if (PassKey::isRutrackerHost(QUrl(t).host()))
            ++ruAnnouncers;
    }
    for (const PassKey::Found &f : pks) {
        if (f.isRutracker)
            ruPks.insert(f.value);
    }
    if (ruAnnouncers > 0) {
        if (!_ownPk.isEmpty()) {
            parts << tr("pk: yours");
        } else if (!ruPks.isEmpty()) {
            parts << tr("pk: unknown");
        } else {
            parts << tr("pk: none");
        }

        QStringList pkDetails;
        for (const PassKey::Found &f : pks)
            pkDetails << QStringLiteral("%1: %2=%3…").arg(f.host, f.param, f.value.left(8));
        if (pkDetails.isEmpty())
            pkDetails << tr("No passkey found in the announce URLs.");
        if (!_ownPk.isEmpty())
            pkDetails << tr("Announce URLs are rewritten with your passkey when adding.");
        else
            pkDetails << tr("Set your passkey in Settings so your upload counts.");
        _metaLabel->setToolTip(pkDetails.join(QLatin1Char('\n')));
    } else {
        _metaLabel->setToolTip(QString());
    }
    // Offer learning when exactly one unknown rutracker passkey is present.
    _learnPkButton->setVisible(!_quick && _ownPk.isEmpty() && ruPks.size() == 1);
    _metaLabel->setText(parts.join(QStringLiteral(" · ")));
}

void AddPage::startPrepare()
{
    if (_worker) {
        _worker->disconnect(this);
        _worker->requestCancel();
        _worker->deleteLater();
        _worker = nullptr;
    }
    _worker = new Worker(_cfg, _payload, _quick, nullptr);
    _worker->setTask(Worker::Prepare);
    connect(_worker, &Worker::status, this, &AddPage::onStatus, Qt::QueuedConnection);
    connect(_worker, &Worker::prepareFinished, this, &AddPage::onPrepareFinished, Qt::QueuedConnection);
    connect(_worker, &Worker::quickFinished, this, &AddPage::onQuickFinished, Qt::QueuedConnection);
    connect(_worker, &Worker::applyFinished, this, &AddPage::onApplyFinished, Qt::QueuedConnection);
    connect(_worker, &Worker::cleanupFinished, this, &AddPage::onCleanupFinished, Qt::QueuedConnection);
    connect(_worker, &Worker::pollFinished, this, &AddPage::onPollFinished, Qt::QueuedConnection);
    _worker->start();
}

void AddPage::onStatus(const QString &text)
{
    _statusText = text;
    _statusError = false;
    emit stateChanged();
}

void AddPage::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    updateNameLabel();
    updateTableMessageGeometry();
}

bool AddPage::eventFilter(QObject *obj, QEvent *e)
{
    // The file table consumes Return internally (item activation) and never
    // reaches the hosting dialog's default button; commit it explicitly.
    if (obj == _tree->viewport() && e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (_phase == Ready && (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter)) {
            commit();
            return true;
        }
    }
    if (obj == _tree->viewport() && e->type() == QEvent::Resize)
        updateTableMessageGeometry();
    return QWidget::eventFilter(obj, e);
}

void AddPage::onPrepareFinished(bool success, const QString &error)
{
    if (!success || _worker->wasCancelled()) {
        if (_worker->wasCancelled()) {
            cleanupAndClose();
        } else {
            _phase = Done;
            showError(error);
            emit stateChanged();
        }
        return;
    }
    populateInteractive(_worker->files);
}

void AddPage::onQuickFinished(bool success, const QString &error)
{
    _phase = Done;
    _exitCode = (success && _worker->resultOk) ? 0 : 1;
    _progress->setRange(0, 100);
    _progress->setValue(100);
    onStatus(_worker->resultOk ? tr("Done.") : tr("Failed to force start."));
    if (success && _worker->resultOk)
        Config::recordAdded(_worker->torrentInfo ? _worker->torrentInfo->totalSize : 0);
    if (!error.isEmpty() && !success)
        showError(error);
    emit stateChanged();
}

void AddPage::onApplyFinished(bool success, const QString &error)
{
    if (!success) {
        _phase = Ready;
        setControlsEnabled(true);
        setOptionsEnabled(true);
        _progress->setRange(0, 100);
        _progress->setValue(0);
        showError(error);
        emit stateChanged();
        return;
    }

    _phase = Done;
    _exitCode = _worker->resultOk ? 0 : 1;
    _worker->existed = true;
    if (_worker->resultOk)
        Config::recordAdded(selectedBytes());

    _tree->setColumnHidden(kColProgress, false);
    _tree->setColumnHidden(kColPriority, false);

    onStatus(_worker->resultOk ? tr("Done.") : tr("Done (status unverified)."));

    // Track download progress in the table.
    if (_pollTimer)
        _pollTimer->start();
    onPollTick();
    emit stateChanged();
}

void AddPage::onCleanupFinished()
{
    _phase = Done;
    emit stateChanged();
    emit closeRequested();
}

void AddPage::populateInteractive(const QVector<TorrentFile> &files)
{
    _phase = Ready;
    setControlsEnabled(true);
    _progress->setRange(0, 100);
    _progress->setValue(0);
    _retryVisible = false;
    _blindAdd = false;
    _compact = false;
    _optionsStrip->show();
    _tree->show();
    _tree->header()->setHidden(true);

    // Preserve anything the user typed into the options strip while the
    // metadata was being fetched (fields stay live during Prepare).
    const QString userCategory = _categoryBox->currentText().trimmed();
    const QString userTags = _tagsEdit->text().trimmed();
    const QString userPath = _pathEdit->text().trimmed();

    _serverCaps = _worker->caps;

    _categoryBox->clear();
    _categoryBox->addItem(QString());
    for (const auto &kv : _worker->categories)
        if (!kv.first.isEmpty())
            _categoryBox->addItem(kv.first);

    QString cat = (_worker->existed || !_worker->initialCategory.isEmpty()) ? _worker->initialCategory : _cfg.defaultCategory;
    if (!userCategory.isEmpty())
        cat = userCategory;
    _categoryBox->setCurrentText(_serverCaps.setCategory ? cat : QString());
    QString tags = (_worker->existed || !_worker->initialTags.isEmpty()) ? _worker->initialTags : _cfg.defaultTags;
    if (!userTags.isEmpty())
        tags = userTags;
    _tagsEdit->setText(_serverCaps.setTags ? tags : QString());
    QString path = _worker->initialSavePath;
    if (!userPath.isEmpty())
        path = userPath;
    _pathEdit->setText(path);

    // Options the server does not support are disabled, not erroring.
    _categoryBox->setToolTip(_serverCaps.setCategory ? QString() : tr("Not supported by this server."));
    _tagsEdit->setPlaceholderText(_serverCaps.setTags ? QString() : tr("Not supported"));
    _tagsEdit->setToolTip(_serverCaps.setTags ? QString() : tr("Not supported by this server."));
    setOptionsEnabled(true);

    if (_worker->torrentInfo && !_worker->torrentInfo->name.isEmpty())
        _fullName = _worker->torrentInfo->name;
    else
        _fullName = _payload.prettyName();
    updateNameLabel();
    updateMetaLine();

    const bool existed = _worker->existed;
    _tree->setColumnHidden(kColProgress, !existed);
    _tree->setColumnHidden(kColPriority, !existed);
    if (existed && _pollTimer) {
        _pollTimer->start();
    }

    if (_worker->existed) {
        if (files.isEmpty()) {
            onStatus(tr("Torrent already exists in client."));
            setTableMessage(tr("Files unavailable."));
            updateSummary();
            emit stateChanged();
            return;
        }
        buildFileTable(files);
        onStatus(tr("Torrent already exists. Trackers and settings updated."));
    } else {
        if (files.isEmpty()) {
            onStatus(tr("Metadata timeout. Adding without file list."));
            setTableMessage(tr("File list unavailable — the torrent will be added whole."));
            _blindAdd = true;
            _retryVisible = true;
            updateSummary();
            emit stateChanged();
            return;
        }
        buildFileTable(files);
        onStatus(tr("Metadata received."));
    }

    clearTableMessage();
    _tree->setFocus();

    const QSet<int> selectedIndices = _payload.selectedIndices();
    if (!selectedIndices.isEmpty() && !files.isEmpty()) {
        _blockTreeSignals = true;
        forEachFileInTree(_tree, [&selectedIndices](QTreeWidgetItem *item) {
            int idx = item->data(kColFile, FileIndexRole).toInt();
            bool wanted = selectedIndices.contains(idx);
            item->setCheckState(kColFile, wanted ? Qt::Checked : Qt::Unchecked);
            item->setData(kColFile, PriorityRole, wanted ? 1 : 0);
            updateFileText(item);
        });
        _blockTreeSignals = false;
    }

    updateSummary();
}

void AddPage::buildFileTable(const QVector<TorrentFile> &files)
{
    _listBar->show();
    _tree->header()->show();
    _blockTreeSignals = true;
    _tree->setSortingEnabled(false);
    _tree->clear();

    QFileIconProvider iconProvider;
    QIcon fallbackFileIcon = QApplication::style()->standardIcon(QStyle::SP_FileIcon);

    for (const TorrentFile &f : files) {
        QString path = f.name;
        path.replace(QLatin1Char('\\'), QLatin1Char('/'));
        const QString relPath = path.startsWith(QLatin1Char('/')) ? path.mid(1) : path;
        const QString baseName = relPath.section(QLatin1Char('/'), -1);

        auto *item = new FileTreeItem();
        item->setText(kColIndex, QString::number(f.index + 1));
        item->setText(kColFile, relPath);
        const auto setRole = [item](int role, const QVariant &value) {
            item->setData(kColFile, role, value);
        };
        setRole(FileIndexRole, f.index);
        setRole(FileSizeRole, f.size);
        setRole(PriorityRole, f.priority);
        setRole(OriginalPriorityRole, f.priority);
        setRole(PrioBeforeUncheckRole, f.priority == 0 ? 1 : f.priority);
        setRole(ProgressRole, f.progress);
        setRole(BaseNameRole, baseName);
        setRole(RelPathRole, relPath);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(kColFile, f.priority == 0 ? Qt::Unchecked : Qt::Checked);
        item->setToolTip(kColFile, relPath);

        QFileInfo fi(baseName);
        QIcon fiIcon = iconProvider.icon(fi);
        item->setIcon(kColFile, fiIcon.isNull() ? fallbackFileIcon : fiIcon);

        _tree->addTopLevelItem(item);
        updateFileText(item);
    }

    _tree->setSortingEnabled(true);
    _blockTreeSignals = false;
    applyRowVisibility();
    buildChips();
}

void AddPage::buildChips()
{
    // Chips only exist for torrents with multiple top-level folders.
    if (!_chipLayout)
        return;

    // Remove previously built chips (everything except the trailing stretch).
    while (_chipLayout->count() > 1) {
        QLayoutItem *child = _chipLayout->takeAt(0);
        if (child->widget())
            child->widget()->deleteLater();
        delete child;
    }
    if (_chipGroup) {
        delete _chipGroup;
        _chipGroup = nullptr;
    }
    _scopePath.clear();

    QMap<QString, int> folderCounts;
    int rooted = 0;
    for (int i = 0; i < _tree->topLevelItemCount(); ++i) {
        const QString relPath = _tree->topLevelItem(i)->data(kColFile, RelPathRole).toString();
        const int slash = relPath.indexOf(QLatin1Char('/'));
        if (slash <= 0) {
            ++rooted;
            continue;
        }
        ++folderCounts[relPath.left(slash)];
    }
    const bool hasChips = folderCounts.size() >= 2;
    // Without chips the scroll area would be empty dead space: hide it so
    // the bulk controls pack against the left edge.
    _chipScroll->setVisible(hasChips);
    if (!hasChips)
        return;

    _chipGroup = new QButtonGroup(this);
    _chipGroup->setExclusive(true);

    auto addChip = [this](const QString &label, const QString &path) {
        auto *chip = new QToolButton(this);
        chip->setText(label);
        chip->setCheckable(true);
        chip->setToolTip(path.isEmpty() ? tr("Show all files") : tr("Show only this folder"));
        chip->setMaximumWidth(160);
        _chipGroup->addButton(chip);
        if (auto *box = dynamic_cast<QHBoxLayout *>(_chipLayout))
            box->insertWidget(box->count() - 1, chip);
        connect(chip, &QToolButton::toggled, this, [this, path](bool on) {
            if (!on)
                return;
            _scopePath = path;
            applyRowVisibility();
            updateFileColumnTexts();
        });
        return chip;
    };

    auto *allChip = addChip(tr("All files"), QString());
    for (auto it = folderCounts.constBegin(); it != folderCounts.constEnd(); ++it)
        addChip(it.key() + QStringLiteral(" · %1").arg(it.value()), it.key());
    allChip->setChecked(true);
}

void AddPage::applyRowVisibility()
{
    if (!_tree)
        return;
    const QString filter = _filterEdit ? _filterEdit->text().trimmed() : QString();
    for (int i = 0; i < _tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = _tree->topLevelItem(i);
        const QString relPath = item->data(kColFile, RelPathRole).toString();
        const bool inScope = _scopePath.isEmpty() || relPath.startsWith(_scopePath + QLatin1Char('/'));
        const bool matches = filter.isEmpty() || relPath.contains(filter, Qt::CaseInsensitive);
        item->setHidden(!(inScope && matches));
    }
}

void AddPage::updateFileColumnTexts()
{
    if (!_tree)
        return;
    const QString prefix = _scopePath.isEmpty() ? QString() : _scopePath + QLatin1Char('/');
    for (int i = 0; i < _tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = _tree->topLevelItem(i);
        const QString relPath = item->data(kColFile, RelPathRole).toString();
        item->setText(kColFile, relPath.startsWith(prefix) ? relPath.mid(prefix.length()) : relPath);
        item->setToolTip(kColFile, relPath);
    }
}

void AddPage::setTableMessage(const QString &text)
{
    if (!_tableMessage)
        return;
    _tableMessage->setText(text);
    _tableMessage->adjustSize();
    _tableMessage->show();
    _tableMessage->raise();
    updateTableMessageGeometry();
}

void AddPage::clearTableMessage()
{
    if (_tableMessage)
        _tableMessage->hide();
}

void AddPage::updateTableMessageGeometry()
{
    if (!_tableMessage || !_tableMessage->isVisible() || !_tree)
        return;
    const QSize vp = _tree->viewport()->size();
    _tableMessage->resize(qMin(vp.width() - 32, _tableMessage->sizeHint().width() + 16),
                          _tableMessage->sizeHint().height());
    _tableMessage->move((vp.width() - _tableMessage->width()) / 2,
                        qMax(24, (vp.height() - _tableMessage->height()) / 2));
}

void AddPage::onRetry()
{
    if (_phase != Ready)
        return;
    _phase = Preparing;
    _retryVisible = false;
    if (_pollTimer)
        _pollTimer->stop();
    _compact = true;
    _listBar->hide();
    _tree->hide();
    _tree->header()->setHidden(true);
    _optionsStrip->hide();
    setControlsEnabled(false);
    _progress->setRange(0, 0);
    onStatus(tr("Connecting..."));
    _tree->clear();
    buildChips();
    startPrepare();
    emit stateChanged();
}

void AddPage::onHeaderContextMenu(const QPoint &pos)
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

void AddPage::onTreeItemChanged(QTreeWidgetItem *item, int column)
{
    if (_blockTreeSignals || column != kColFile)
        return;

    _blockTreeSignals = true;
    int prio = item->data(kColFile, PriorityRole).toInt();
    if (item->checkState(kColFile) == Qt::Unchecked) {
        if (prio != 0) {
            item->setData(kColFile, PrioBeforeUncheckRole, prio);
            item->setData(kColFile, PriorityRole, 0);
        }
    } else {
        if (prio == 0) {
            int prev = item->data(kColFile, PrioBeforeUncheckRole).toInt();
            item->setData(kColFile, PriorityRole, prev > 0 ? prev : 1);
        }
    }
    updateFileText(item);
    _blockTreeSignals = false;
    updateSummary();
}

void AddPage::onTreeContextMenu(const QPoint &pos)
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

void AddPage::setPriority(int priority)
{
    auto items = _tree->selectedItems();
    if (items.isEmpty() && _tree->currentItem())
        items.append(_tree->currentItem());
    if (items.isEmpty())
        return;

    _blockTreeSignals = true;
    for (QTreeWidgetItem *item : items) {
        setFilePriority(item, priority);
    }
    _blockTreeSignals = false;
    updateSummary();
}

void AddPage::checkAll(bool on)
{
    // Bulk operations respect the active scope and text filter:
    // only rows the user can currently see are touched.
    _blockTreeSignals = true;
    for (int i = 0; i < _tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = _tree->topLevelItem(i);
        if (!item->isHidden())
            setFileCheck(item, on);
    }
    _blockTreeSignals = false;
    updateSummary();
}

void AddPage::invertVisible()
{
    _blockTreeSignals = true;
    for (int i = 0; i < _tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = _tree->topLevelItem(i);
        if (!item->isHidden()) {
            bool on = item->checkState(kColFile) != Qt::Checked;
            setFileCheck(item, on);
        }
    }
    _blockTreeSignals = false;
    updateSummary();
}

void AddPage::setFileCheck(QTreeWidgetItem *item, bool on)
{
    int prio = item->data(kColFile, PriorityRole).toInt();
    if (on) {
        if (prio == 0) {
            int prev = item->data(kColFile, PrioBeforeUncheckRole).toInt();
            item->setData(kColFile, PriorityRole, prev > 0 ? prev : 1);
        }
    } else {
        if (prio != 0)
            item->setData(kColFile, PrioBeforeUncheckRole, prio);
        item->setData(kColFile, PriorityRole, 0);
    }
    item->setCheckState(kColFile, on ? Qt::Checked : Qt::Unchecked);
    updateFileText(item);
}

void AddPage::setFilePriority(QTreeWidgetItem *item, int priority)
{
    item->setData(kColFile, PriorityRole, priority);
    if (priority != 0)
        item->setData(kColFile, PrioBeforeUncheckRole, priority);
    item->setCheckState(kColFile, priority == 0 ? Qt::Unchecked : Qt::Checked);
    updateFileText(item);
}

void AddPage::updateFileText(QTreeWidgetItem *item)
{
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

void AddPage::forEachFileInTree(QTreeWidget *tree, const std::function<void(QTreeWidgetItem *)> &fn)
{
    // The tree is flat: every file is a top-level item.
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        fn(tree->topLevelItem(i));
}

void AddPage::updateSummary()
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
    _summaryText = tr("Selected %1 / %2 (%3 / %4 files)")
                       .arg(Format::size(selected), Format::size(total))
                       .arg(selCount).arg(allCount);
    emit stateChanged();
}

qint64 AddPage::selectedBytes() const
{
    // What actually gets downloaded; falls back to the whole torrent when
    // no file list is available (blind add).
    qint64 selected = 0;
    int files = 0;
    forEachFileInTree(_tree, [&](QTreeWidgetItem *item) {
        if (item->data(kColFile, PriorityRole).toInt() != 0) {
            selected += item->data(kColFile, FileSizeRole).toLongLong();
            ++files;
        }
    });
    if (files == 0)
        return (_worker && _worker->torrentInfo) ? _worker->torrentInfo->totalSize : 0;
    return selected;
}

bool AddPage::commit()
{
    if (_phase != Ready)
        return false;

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
        // Reachable via Enter in the file table, which bypasses the
        // disabled Add button.
        _statusError = true;
        _statusText = tr("No files selected.");
        return false;
    }

    // .torrent files carry their announce inside the bencoded data; the
    // rewritten URL is merged as an extra tracker (magnets get the user's
    // passkey injected into their tr= URLs by the worker before the add).
    QStringList trackers = _payload.trackers();
    int pkRewritten = 0;
    if (_payload.isFile() && !_ownPk.isEmpty())
        trackers += PassKey::applyRutrackerPasskey(_payload.trackers(), _ownPk, &pkRewritten);

    saveHeaderState();
    _phase = Applying;
    setControlsEnabled(false);
    setOptionsEnabled(false);
    _progress->setRange(0, 0);
    if (pkRewritten > 0)
        onStatus(tr("Adding torrent — %n rutracker announce(s) rewritten with your passkey...",
                    "", pkRewritten));
    else
        onStatus(_worker && _worker->existed ? tr("Updating torrent...") : tr("Adding torrent..."));

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
    p.trackers = trackers;

    _worker->applyParams = p;
    if (_worker->isRunning()) {
        // Abort the in-flight poll instead of blocking the UI thread for a
        // full request timeout: cancellation aborts within ~100 ms.
        _worker->requestCancel();
        _worker->wait();
        _worker->resetCancel();
    }
    _worker->setTask(Worker::Apply);
    _worker->start();
    emit stateChanged();
    return true;
}

void AddPage::requestCancel()
{
    if (_phase == Done) {
        emit closeRequested();
        return;
    }
    if (_phase == Applying)
        return;

    if (_worker)
        _worker->requestCancel();

    if (_phase == Ready) {
        cleanupAndClose();
    } else {
        onStatus(tr("Cancelling..."));
    }
}

void AddPage::cleanupAndClose()
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
        setOptionsEnabled(false);
        _progress->setRange(0, 0);
        onStatus(tr("Deleting torrent..."));

        _worker->resetCancel();
        _worker->setTask(Worker::Cleanup);
        _worker->start();
    } else {
        _phase = Done;
        emit stateChanged();
        emit closeRequested();
    }
}

void AddPage::setControlsEnabled(bool on)
{
    _tree->setEnabled(on);
    _listBar->setEnabled(on);
    emit stateChanged();
}

bool AddPage::summaryHasSelection() const
{
    for (int i = 0; i < _tree->topLevelItemCount(); ++i) {
        if (_tree->topLevelItem(i)->data(kColFile, PriorityRole).toInt() != 0)
            return true;
    }
    return false;
}

void AddPage::setOptionsEnabled(bool on)
{
    _categoryBox->setEnabled(on && _serverCaps.setCategory);
    _tagsEdit->setEnabled(on && _serverCaps.setTags);
    _pathEdit->setEnabled(on);
    _forceBox->setEnabled(on);
}

void AddPage::showError(const QString &msg)
{
    _progress->setRange(0, 100);
    _progress->setValue(0);
    _statusError = true;
    _statusText = msg;
    emit stateChanged();
}

void AddPage::onLearnPasskey()
{
    const QVector<PassKey::Found> pks = PassKey::extract(_payload.trackers());
    QString pk;
    for (const PassKey::Found &f : pks) {
        if (f.isRutracker) {
            pk = f.value;
            break;
        }
    }
    if (pk.isEmpty())
        return;

    try {
        Config c = Config::load();
        c.setRutrackerPasskey(pk);
        c.save();
        _ownPk = pk;
        onStatus(tr("RuTracker passkey saved."));
        emit passkeyLearned(pk);
        updateMetaLine();
        emit stateChanged();
    } catch (const std::exception &ex) {
        Log::write(QStringLiteral("Failed to save rutracker passkey: %1").arg(QString::fromUtf8(ex.what())));
        showError(tr("Failed to save passkey: %1").arg(QString::fromUtf8(ex.what())));
        emit stateChanged();
    }
}

void AddPage::setOwnPk(const QString &pk)
{
    if (_ownPk == pk)
        return;
    _ownPk = pk;
    updateMetaLine();
}

void AddPage::retry()
{
    onRetry();
}

QString AddPage::titleText() const
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

    const QString name = displayName();
    if (!name.isEmpty())
        return QStringLiteral("qt-magnet %1 - %2").arg(verTag, name);
    return QStringLiteral("qt-magnet %1").arg(verTag);
}

QString AddPage::displayName() const
{
    if (_worker && _worker->torrentInfo && !_worker->torrentInfo->name.isEmpty())
        return _worker->torrentInfo->name;
    return _payload.prettyName();
}

bool AddPage::canCommit() const
{
    if (_phase != Ready)
        return false;
    // With a file list present, Add stays disabled until something is
    // selected; a blind add (no file list) is always committable.
    return _tree->topLevelItemCount() == 0 || summaryHasSelection();
}

QString AddPage::actionLabel() const
{
    if (_worker && _worker->existed)
        return tr("&Update");
    if (_blindAdd)
        return tr("Add &anyway");
    return tr("&Add");
}

void AddPage::onPollTick()
{
    if (!_worker || _worker->isRunning() || _worker->wasCancelled())
        return;
    if (!_worker->existed || (_worker->hash.isEmpty() && _payload.hash().isEmpty()))
        return;

    _worker->setTask(Worker::Poll);
    _worker->start();
}

void AddPage::onPollFinished(bool success)
{
    if (!success || !_worker)
        return;

    if (_worker->pollInfo.has_value()) {
        const TorrentInfo &ti = _worker->pollInfo.value();
        double totalProg = ti.progress;
        _progress->setRange(0, kProgressScale);
        _progress->setValue(qRound(totalProg * kProgressScale));
        _progress->setFormat(QString::number(totalProg * 100.0, 'f', 1) + QLatin1Char('%'));

        QString stateStr = TorrentClient::stateString(ti.state);
        onStatus(QStringLiteral("%1 (%2)")
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
                item->setData(kColFile, ProgressRole, progressByIndex.value(idx));
                updateFileText(item);
            }
        });
        _blockTreeSignals = false;
        _tree->viewport()->update();
    }
}

