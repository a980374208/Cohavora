#include "whiteboard_panel.h"
#include "src/ui/app_theme.h"
#include "src/core/whiteboard/whiteboard_runtime.h"
#include <QtCore/QSaveFile>
#include <QtCore/QSignalBlocker>
#include <QtCore/QThread>
#include <QtCore/QUuid>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>
#include <memory>

namespace MeetingUI {
namespace wb = livekit::whiteboard;
namespace {
QHBoxLayout *toolbar(QVBoxLayout *outer, QWidget *parent) {
    auto *scroll = new QScrollArea(parent);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFixedHeight(64);
    auto *row = new QWidget(scroll);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 2, 0, 2);
    layout->setSpacing(6);
    layout->setSizeConstraint(QLayout::SetMinimumSize);
    scroll->setWidget(row);
    outer->addWidget(scroll);
    return layout;
}
QPushButton *button(QHBoxLayout *row, const QString &text, const char *name) {
    auto *result = new QPushButton(text);
    result->setObjectName(QString::fromLatin1(name));
    row->addWidget(result);
    return result;
}
}

WhiteboardPanel::WhiteboardPanel(QWidget *parent)
    : QWidget(parent), document_(QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString()) {
    setObjectName(QStringLiteral("localWhiteboard"));
    setAttribute(Qt::WA_StyledBackground);
    AppTheme::setTone(*this, AppTheme::Tone::Light);
    AppTheme::setStyleVariant(*this, "whiteboard-panel");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(4);
    auto *header = new QHBoxLayout;
    title_ = new QLabel(tr("Local whiteboard"), this);
    title_->setObjectName(QStringLiteral("whiteboardTitle"));
    header->addWidget(title_);
    mode_ = new QLabel(tr("Only visible to you"), this);
    mode_->setObjectName(QStringLiteral("whiteboardLocalNotice"));
    header->addWidget(mode_);
    header->addStretch();
    auto *back = button(header, tr("Back to meeting"), "whiteboardClose");
    connect(back, &QPushButton::clicked, this, [this] { canvas_->cancelInput(); emit closeRequested(); });
    layout->addLayout(header);

    canvas_ = new WhiteboardCanvas(document_, this);
    auto *tools = toolbar(layout, this);
    const QStringList names{tr("Pen"), tr("Highlighter"), tr("Line"), tr("Rectangle"), tr("Ellipse"),
        tr("Arrow"), tr("Text"), tr("Eraser"), tr("Laser"), tr("Pan")};
    auto *group = new QButtonGroup(this);
    for (int i = 0; i < names.size(); ++i) {
        auto *item = button(tools, names[i], "whiteboardTool");
        item->setProperty("toolId", i);
        item->setCheckable(true);
        item->setChecked(i == 0);
        if (i == 7) item->setToolTip(tr("Erase whole objects (cannot be undone)"));
        if (i == 8) item->setToolTip(tr("Temporary pointer; not included in exports"));
        group->addButton(item, i);
        connect(item, &QPushButton::clicked, this, [this, i] { canvas_->setTool(static_cast<WhiteboardCanvas::Tool>(i)); });
    }
    tools->addStretch();

    auto *options = toolbar(layout, this);
    auto *colors = new QComboBox(this);
    colors->setAccessibleName(tr("Ink color"));
    const std::pair<QString, QRgb> palette[]{{tr("Black"), 0x1f2329}, {tr("Red"), 0xf53f3f},
        {tr("Blue"), 0x1677ff}, {tr("Green"), 0x00a870}, {tr("Orange"), 0xff9f1a}, {tr("Purple"), 0x722ed1}};
    for (const auto &color : palette) {
        QPixmap swatch(14, 14);
        swatch.fill(QColor::fromRgb(color.second));
        colors->addItem(QIcon(swatch), color.first, color.second);
    }
    options->addWidget(colors);
    connect(colors, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, colors] {
        canvas_->setInkColor(QColor::fromRgb(colors->currentData().toUInt()));
    });
    options->addWidget(new QLabel(tr("Width"), this));
    auto *width = new QSpinBox(this);
    width->setRange(1, 32);
    width->setValue(4);
    width->setAccessibleName(tr("Ink width"));
    options->addWidget(width);
    connect(width, qOverload<int>(&QSpinBox::valueChanged), canvas_, &WhiteboardCanvas::setInkWidth);
    options->addWidget(new QLabel(tr("Text size"), this));
    auto *font = new QSpinBox(this);
    font->setRange(8, 96);
    font->setValue(28);
    font->setAccessibleName(tr("Text size"));
    options->addWidget(font);
    connect(font, qOverload<int>(&QSpinBox::valueChanged), canvas_, &WhiteboardCanvas::setTextSize);
    undo_ = button(options, tr("Undo"), "whiteboardUndo");
    redo_ = button(options, tr("Redo"), "whiteboardRedo");
    undo_->setToolTip(tr("Undo your last created object (Ctrl+Z)"));
    redo_->setToolTip(tr("Redo your last undone object (Ctrl+Y)"));
    connect(undo_, &QPushButton::clicked, this, [this] { canvas_->execute(wb::CommandKind::Undo); });
    connect(redo_, &QPushButton::clicked, this, [this] { canvas_->execute(wb::CommandKind::Redo); });
    clear_ = button(options, tr("Clear page"), "whiteboardClear");
    connect(clear_, &QPushButton::clicked, this, &WhiteboardPanel::confirmClear);
    lock_ = button(options, tr("Lock board"), "whiteboardLock");
    lock_->setCheckable(true);
    lock_->hide();
    connect(lock_, &QPushButton::toggled, this, &WhiteboardPanel::lockRequested);
    writers_ = button(options, tr("Participants can draw"), "whiteboardWriters");
    writers_->setCheckable(true);
    writers_->setChecked(true);
    writers_->hide();
    connect(writers_, &QPushButton::toggled, this, &WhiteboardPanel::writersOpenRequested);
    options->addStretch();

    layout->addWidget(canvas_, 1);
    auto *footer = toolbar(layout, this);
    pages_ = new QComboBox(this);
    pages_->setObjectName(QStringLiteral("whiteboardPages"));
    pages_->setAccessibleName(tr("Page"));
    footer->addWidget(pages_);
    connect(pages_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index >= 0) canvas_->execute(wb::CommandKind::SelectPage, pages_->itemData(index).toString().toStdString());
    });
    addPage_ = button(footer, tr("Add page"), "whiteboardAddPage");
    addPage_->setShortcut(QKeySequence::New);
    connect(addPage_, &QPushButton::clicked, this, [this] { canvas_->execute(wb::CommandKind::AddPage); });
    import_ = button(footer, tr("Import image"), "whiteboardImportImage");
    import_->setShortcut(QKeySequence::Open);
    import_->setToolTip(tr("Import PNG or JPEG as a new image page (Ctrl+O)"));
    connect(import_, &QPushButton::clicked, this, &WhiteboardPanel::importImage);
    zoom_ = new QComboBox(this);
    zoom_->setAccessibleName(tr("Zoom relative to page fit"));
    for (const auto percent : {50, 75, 100, 125, 150, 200}) zoom_->addItem(QString::number(percent) + '%', percent / 100.0);
    zoom_->setCurrentIndex(2);
    footer->addWidget(zoom_);
    connect(zoom_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { canvas_->setZoom(zoom_->currentData().toDouble()); });
    connect(canvas_, &WhiteboardCanvas::zoomChanged, this, [this](double value) {
        const QSignalBlocker blocker(zoom_);
        int index = zoom_->findText(QString::number(qRound(value * 100)) + '%');
        if (index < 0) {
            if (zoom_->count() > 6) zoom_->removeItem(6);
            zoom_->addItem(QString::number(qRound(value * 100)) + '%', value);
            index = 6;
        }
        zoom_->setCurrentIndex(index);
    });
    auto *fit = button(footer, tr("Fit page"), "whiteboardFit");
    fit->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
    connect(fit, &QPushButton::clicked, canvas_, &WhiteboardCanvas::fitPage);
    footer->addStretch();
    export_ = button(footer, tr("Export PNG"), "whiteboardExport");
    export_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
    connect(export_, &QPushButton::clicked, this, &WhiteboardPanel::exportPng);
    status_ = new QLabel(tr("Content is kept only for this meeting window. Export before leaving."), this);
    status_->setWordWrap(true);
    status_->setObjectName(QStringLiteral("whiteboardStatus"));
    layout->addWidget(status_);
    connect(canvas_, &WhiteboardCanvas::notice, status_, &QLabel::setText);
    connect(canvas_, &WhiteboardCanvas::documentChanged, this, &WhiteboardPanel::refreshControls);
    AppTheme::styleChoiceControls(*this, AppTheme::Tone::Light);
    refreshControls();
}

WhiteboardPanel::~WhiteboardPanel() {
    // The bounded export owns a page copy and no widgets. Join before releasing
    // the QThread wrapper; no UI event or session callback is awaited here.
    if (exportWorker_) exportWorker_->wait();
    if (importWorker_) importWorker_->wait();
    if (decodeWorker_) decodeWorker_->wait();
}
void WhiteboardPanel::refreshControls() {
    const auto actor = localActor_.isEmpty() ? document_.owner() : localActor_.toStdString();
    undo_->setEnabled(canEdit_ && (collaborative_ || document_.canUndo(actor)));
    redo_->setEnabled(canEdit_ && (collaborative_ || document_.canRedo(actor)));
    const bool canManagePages = !collaborative_ || canAdmin_;
    addPage_->setEnabled(canEdit_ && canManagePages && document_.pages().size() < wb::Document::MaxPages);
    import_->setEnabled(canManagePages && !importWorker_);
    clear_->setEnabled(canEdit_ && canManagePages);
    pages_->setEnabled(canManagePages);
    const QSignalBlocker blocker(pages_);
    pages_->clear();
    int number = 0;
    for (const auto &page : document_.pages()) {
        ++number;
        QString label = page.backgroundAssetId.empty() ? tr("Page %1").arg(number) : tr("Image %1").arg(number);
        if (!page.backgroundAssetId.empty() && !canvas_->hasBackgroundImage(page.backgroundAssetId))
            label = tr("%1 (loading)").arg(label);
        pages_->addItem(label, QString::fromStdString(page.id));
    }
    pages_->setCurrentIndex(static_cast<int>(document_.activePageIndex()));
}

void WhiteboardPanel::enableCollaboration() {
    if (collaborative_) return;
    collaborative_ = true;
    canEdit_ = false;
    projectedCanEdit_ = false;
    title_->setText(tr("Collaborative whiteboard"));
    mode_->setText(tr("Connecting..."));
    canvas_->setEditingEnabled(false);
    canvas_->setCommandHandler([this](const wb::Command &command) {
        emit commandProposed(command);
        return wb::Result{wb::Status::NoChange, {}};
    });
    refreshControls();
}

void WhiteboardPanel::applyProjection(
    const QByteArray &snapshot, quint64 sequence, int collaborationState,
    const QString &authorityIdentity, const QString &localActor, bool locked,
    bool writersOpen, bool canEdit, bool canAdmin, const QVariantMap &assets,
    const QString &status) {
    enableCollaboration();
    const auto restored = wb::Document::fromJson(
        std::string_view(snapshot.constData(), static_cast<std::size_t>(snapshot.size())));
    if (!restored) {
        status_->setText(tr("The whiteboard state could not be restored."));
        canEdit_ = false;
        canvas_->setEditingEnabled(false);
        refreshControls();
        return;
    }
    document_ = *restored;
    for (auto it = assets.begin(); it != assets.end(); ++it)
        assetBytes_.insert(it.key(), it.value());
    localActor_ = localActor;
    canvas_->setActor(localActor.toStdString());
    canvas_->refreshFromDocument();
    projectedCanEdit_ = canEdit;
    canAdmin_ = canAdmin;
    updateEditingState();
    {
        const QSignalBlocker lockBlocker(lock_);
        const QSignalBlocker writersBlocker(writers_);
        lock_->setChecked(locked);
        writers_->setChecked(writersOpen);
    }
    lock_->setVisible(canAdmin_);
    writers_->setVisible(canAdmin_);
    using State = wb::CollaborationState;
    const auto state = static_cast<State>(collaborationState);
    QString stateText = state == State::Ready ? (locked ? tr("Locked") : tr("Synced")) :
        state == State::Synchronizing ? tr("Synchronizing...") :
        state == State::ReadOnly ? tr("Connection interrupted - read-only") :
        state == State::Frozen ? tr("Host left - board frozen") : tr("Whiteboard closed");
    mode_->setText(tr("Host controlled"));
    mode_->setToolTip(tr("Authority: %1").arg(authorityIdentity));
    if (!document_.page().backgroundAssetId.empty() &&
        !canvas_->hasBackgroundImage(document_.page().backgroundAssetId))
        stateText = tr("Loading image - read-only");
    status_->setText(tr("%1 - revision %2").arg(stateText).arg(sequence));
    if (!status.isEmpty() && state != State::Ready) status_->setToolTip(status);
    ensureActiveBackground();
    refreshControls();
}
void WhiteboardPanel::confirmClear() {
    canvas_->cancelInput();
    QMessageBox box(QMessageBox::Question, tr("Clear page"), tr("Remove all objects on this page? This cannot be undone."),
        QMessageBox::Yes | QMessageBox::Cancel, this);
    box.setDefaultButton(QMessageBox::Cancel);
    box.setMinimumSize(460, 180);
    AppTheme::setTone(box, AppTheme::Tone::Light);
    if (box.exec() == QMessageBox::Yes) canvas_->execute(wb::CommandKind::Clear);
}
void WhiteboardPanel::exportPng() {
    if (exportWorker_) return;
    canvas_->cancelInput();
    QFileDialog dialog(this, tr("Export current page"));
    dialog.setOption(QFileDialog::DontUseNativeDialog);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setNameFilter(tr("PNG image (*.png)"));
    dialog.setDefaultSuffix(QStringLiteral("png"));
    dialog.selectFile(QStringLiteral("whiteboard-page-%1.png").arg(document_.activePageIndex() + 1));
    AppTheme::setTone(dialog, AppTheme::Tone::Light);
    AppTheme::styleChoiceControls(dialog, AppTheme::Tone::Light);
    AppTheme::makeDialogAdaptive(dialog, {760, 520});
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) return;
    const auto path = dialog.selectedFiles().front();
    const auto page = document_.page();
    const auto background = canvas_->currentBackgroundImage();
    auto succeeded = std::make_shared<bool>(false);
    export_->setEnabled(false);
    status_->setText(tr("Exporting page..."));
    exportWorker_ = QThread::create([page, background, path, succeeded] {
        try {
            const auto image = WhiteboardRendering::renderPage(page, background);
            QSaveFile file(path);
            *succeeded = !image.isNull() && file.open(QIODevice::WriteOnly) && image.save(&file, "PNG") && file.commit();
        } catch (...) { *succeeded = false; }
    });
    exportWorker_->setParent(this);
    connect(exportWorker_, &QThread::finished, this, [this, succeeded] {
        exportWorker_->wait();
        exportWorker_->deleteLater();
        exportWorker_ = nullptr;
        export_->setEnabled(true);
        status_->setText(*succeeded ? tr("Page exported.") : tr("Export failed. Check the destination and try again."));
    });
    exportWorker_->start();
}

void WhiteboardPanel::importImage() {
    if (importWorker_ || (collaborative_ && !canAdmin_)) return;
    canvas_->cancelInput();
    const bool replaceCurrent = document_.pages().size() >= wb::Document::MaxPages;
    if (replaceCurrent) {
        QMessageBox box(QMessageBox::Warning, tr("Replace background"),
            tr("The page limit is reached. Importing will replace the current background and remove all annotations on this page."),
            QMessageBox::Yes | QMessageBox::Cancel, this);
        box.setDefaultButton(QMessageBox::Cancel);
        box.setMinimumSize(500, 210);
        AppTheme::setTone(box, AppTheme::Tone::Light);
        if (box.exec() != QMessageBox::Yes) return;
    }
    QFileDialog dialog(this, tr("Import image page"));
    dialog.setOption(QFileDialog::DontUseNativeDialog);
    dialog.setAcceptMode(QFileDialog::AcceptOpen);
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setNameFilter(tr("PNG or JPEG image (*.png *.jpg *.jpeg)"));
    AppTheme::setTone(dialog, AppTheme::Tone::Light);
    AppTheme::styleChoiceControls(dialog, AppTheme::Tone::Light);
    AppTheme::makeDialogAdaptive(dialog, {760, 520});
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) return;

    const auto path = dialog.selectedFiles().front();
    const auto decoded = std::make_shared<WhiteboardImageResult>();
    import_->setEnabled(false);
    status_->setText(tr("Importing image..."));
    importWorker_ = QThread::create([path, decoded] {
        *decoded = loadAndNormalizeWhiteboardImage(path);
    });
    importWorker_->setParent(this);
    connect(importWorker_, &QThread::finished, this, [this, decoded, replaceCurrent] {
        importWorker_->wait();
        importWorker_->deleteLater();
        importWorker_ = nullptr;
        if (!decoded->ok()) {
            status_->setText(imageErrorText(decoded->error));
            refreshControls();
            return;
        }
        canvas_->setBackgroundImage(decoded->asset.id, decoded->image);
        const auto pageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (collaborative_) {
            if (!canAdmin_) {
                status_->setText(tr("Only the host can import image pages."));
                refreshControls();
                return;
            }
            const QByteArray png(decoded->asset.bytes.data(), static_cast<int>(decoded->asset.bytes.size()));
            emit imageProposed(png, QString::fromStdString(decoded->asset.id),
                static_cast<int>(decoded->asset.width), static_cast<int>(decoded->asset.height),
                pageId, replaceCurrent);
            status_->setText(tr("Uploading image page..."));
        } else {
            const auto result = canvas_->applyImagePage(decoded->asset.id,
                decoded->asset.width, decoded->asset.height, pageId.toStdString(), replaceCurrent);
            status_->setText(result.changed() ? tr("Image page imported.") :
                tr("The image page could not be added."));
        }
        refreshControls();
    });
    importWorker_->start();
}

void WhiteboardPanel::ensureActiveBackground() {
    const auto &id = document_.page().backgroundAssetId;
    if (id.empty() || canvas_->hasBackgroundImage(id) || decodeWorker_ ||
        failedAssets_.contains(QString::fromStdString(id))) {
        updateEditingState();
        return;
    }
    const auto key = QString::fromStdString(id);
    const auto encoded = assetBytes_.value(key).toByteArray();
    if (encoded.isEmpty()) {
        updateEditingState();
        return;
    }
    const auto decoded = std::make_shared<WhiteboardImageResult>();
    decodingAssetId_ = key;
    decodeWorker_ = QThread::create([key, encoded, decoded] {
        *decoded = decodeWhiteboardAssetBytes(key, encoded);
    });
    decodeWorker_->setParent(this);
    connect(decodeWorker_, &QThread::finished, this, [this, decoded, key] {
        decodeWorker_->wait();
        decodeWorker_->deleteLater();
        decodeWorker_ = nullptr;
        decodingAssetId_.clear();
        bool dimensionsMatch = decoded->ok();
        if (dimensionsMatch) {
            for (const auto &page : document_.pages()) {
                if (page.backgroundAssetId != decoded->asset.id) continue;
                if (qRound(page.width) != decoded->image.width() ||
                    qRound(page.height) != decoded->image.height()) dimensionsMatch = false;
            }
        }
        if (dimensionsMatch) {
            canvas_->setBackgroundImage(decoded->asset.id, decoded->image);
        } else {
            failedAssets_.insert(key);
            status_->setText(tr("The image background failed integrity validation."));
        }
        updateEditingState();
        ensureActiveBackground();
        refreshControls();
    });
    decodeWorker_->start();
    updateEditingState();
}

void WhiteboardPanel::updateEditingState() {
    const auto &id = document_.page().backgroundAssetId;
    const bool backgroundReady = id.empty() || canvas_->hasBackgroundImage(id);
    canEdit_ = projectedCanEdit_ && backgroundReady;
    canvas_->setEditingEnabled(canEdit_);
}

QString WhiteboardPanel::imageErrorText(WhiteboardImageError error) const {
    switch (error) {
    case WhiteboardImageError::InputTooLarge:
        return tr("The image file exceeds the 8 MiB limit.");
    case WhiteboardImageError::UnsupportedFormat:
        return tr("Only PNG and JPEG image content is supported.");
    case WhiteboardImageError::InvalidDimensions:
        return tr("The image dimensions are unsupported (320x180 to 4096x4096, at most 16 MP).");
    case WhiteboardImageError::OutputTooLarge:
        return tr("The standardized PNG exceeds the 8 MiB limit.");
    case WhiteboardImageError::IntegrityFailed:
        return tr("The image failed integrity validation.");
    case WhiteboardImageError::DecodeFailed:
        return tr("The image could not be decoded.");
    default:
        return tr("The image file could not be read.");
    }
}
} // namespace MeetingUI
