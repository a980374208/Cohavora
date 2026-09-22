#include "src/ui/whiteboard/whiteboard_panel.h"
#include "src/ui/whiteboard/whiteboard_image_loader.h"
#include "src/ui/whiteboard/annotation_overlay_window.h"
#include "src/core/whiteboard/whiteboard_runtime.h"
#include "src/ui/app_theme.h"
#include "src/ui/app_translation.h"
#include "tests/support/test_check.h"
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtGui/QInputMethodEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QScreen>
#include <QtWidgets/QApplication>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtPlugin>
#include <algorithm>
#include <iostream>

Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)
Q_IMPORT_PLUGIN(QWindowsVistaStylePlugin)
Q_IMPORT_PLUGIN(QSvgPlugin)
Q_IMPORT_PLUGIN(QSvgIconPlugin)

using MeetingUI::WhiteboardCanvas;
using MeetingUI::WhiteboardPanel;
namespace wb = livekit::whiteboard;
namespace {
bool styleWarning = false;
void messages(QtMsgType, const QMessageLogContext &, const QString &text) {
    if (text.contains("style sheet", Qt::CaseInsensitive) || text.contains("Unknown property")) styleWarning = true;
}
void flush() { QApplication::processEvents(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }
void mouse(WhiteboardCanvas &canvas, QEvent::Type type, QPointF point, bool document = true) {
    if (document) point = canvas.documentToView(point);
    const auto button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    const auto buttons = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent event(type, point, button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &event);
}
void stroke(WhiteboardCanvas &canvas, WhiteboardCanvas::Tool tool, QPointF from, QPointF to) {
    canvas.setTool(tool);
    mouse(canvas, QEvent::MouseButtonPress, from);
    mouse(canvas, QEvent::MouseMove, to);
    mouse(canvas, QEvent::MouseButtonRelease, to);
    flush();
}
void toolsAndText(WhiteboardPanel &panel) {
    auto &canvas = *panel.canvas();
    for (int i = 0; i < 6; ++i) {
        stroke(canvas, static_cast<WhiteboardCanvas::Tool>(i), {100.0 + i * 260, 180}, {250.0 + i * 260, 320});
        TEST_CHECK(panel.document().page().objects.size() == i + 1);
        TEST_CHECK(static_cast<int>(panel.document().page().objects.back().kind) == i);
    }
    stroke(canvas, WhiteboardCanvas::Tool::Pen, {100, 500}, {100, 500});
    TEST_CHECK(canvas.exportImage().pixelColor(100, 500) != QColor(Qt::white));
    canvas.setTool(WhiteboardCanvas::Tool::Text);
    mouse(canvas, QEvent::MouseButtonPress, {400, 500});
    // Opening a text box must not turn the initiating release into a draw commit.
    mouse(canvas, QEvent::MouseMove, {410, 500});
    mouse(canvas, QEvent::MouseButtonRelease, {400, 500});
    auto *editor = canvas.findChild<QPlainTextEdit *>("whiteboardTextEditor");
    TEST_CHECK(editor && panel.document().page().objects.size() == 7);
    QInputMethodEvent preedit(QStringLiteral("zhongwen"), {});
    QApplication::sendEvent(editor, &preedit);
    TEST_CHECK(editor->toPlainText().isEmpty() && panel.document().page().objects.size() == 7);
    QInputMethodEvent commit;
    commit.setCommitString(QStringLiteral("中文白板\n会议批注"));
    QApplication::sendEvent(editor, &commit);
    canvas.findChild<QPushButton *>("whiteboardApplyText")->click();
    flush();
    TEST_CHECK(panel.document().page().objects.size() == 8);
    TEST_CHECK(panel.document().page().objects.back().text == "中文白板\n会议批注");
    panel.findChild<QPushButton *>("whiteboardUndo")->click();
    TEST_CHECK(panel.document().page().objects.size() == 7);
    panel.findChild<QPushButton *>("whiteboardRedo")->click();
    TEST_CHECK(panel.document().page().objects.back().kind == wb::ObjectKind::Text);
    const auto png = canvas.exportImage();
    TEST_CHECK(png.size() == QSize(1920, 1080));
    TEST_CHECK(png.pixelColor(0, 0) == QColor(Qt::white));
    int textPixels = 0;
    for (int y = 500; y < 700; ++y) for (int x = 400; x < 880; ++x)
        if (png.pixelColor(x, y) != QColor(Qt::white)) ++textPixels;
    TEST_CHECK(textPixels > 100);
}
void exportFromPanel(WhiteboardPanel &panel) {
    QTemporaryDir directory;
    TEST_CHECK(directory.isValid());
    const auto path = directory.filePath("page.png");
    QTimer::singleShot(0, &panel, [&] {
        auto *dialog = panel.findChild<QFileDialog *>();
        TEST_CHECK(dialog && dialog->testOption(QFileDialog::DontUseNativeDialog));
        dialog->selectFile(path);
        TEST_CHECK(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection));
    });
    auto *exportButton = panel.findChild<QPushButton *>("whiteboardExport");
    exportButton->click();
    QElapsedTimer wait; wait.start();
    while (!exportButton->isEnabled() && wait.elapsed() < 10000) { flush(); QThread::msleep(1); }
    TEST_CHECK(exportButton->isEnabled());
    const QImage loaded(path);
    TEST_CHECK(!loaded.isNull());
    TEST_CHECK(loaded.convertToFormat(QImage::Format_ARGB32_Premultiplied) == panel.canvas()->exportImage());
}
void cancellationAndEraser(WhiteboardPanel &panel) {
    auto &canvas = *panel.canvas();
    canvas.setTool(WhiteboardCanvas::Tool::Pen);
    mouse(canvas, QEvent::MouseButtonPress, {20, 20});
    canvas.execute(wb::CommandKind::Clear);
    mouse(canvas, QEvent::MouseButtonRelease, {80, 80});
    TEST_CHECK(panel.document().page().objects.empty());
    canvas.setTool(WhiteboardCanvas::Tool::Text);
    mouse(canvas, QEvent::MouseButtonPress, {30, 30});
    auto *editor = canvas.findChild<QPlainTextEdit *>("whiteboardTextEditor");
    TEST_CHECK(editor);
    editor->setPlainText(QString(3000, QChar('a')));
    TEST_CHECK(editor->toPlainText().size() == 2000);
    const auto firstPage = panel.document().page().id;
    canvas.execute(wb::CommandKind::AddPage);
    flush();
    TEST_CHECK(!canvas.findChild<QPlainTextEdit *>("whiteboardTextEditor"));
    canvas.execute(wb::CommandKind::SelectPage, firstPage);
    TEST_CHECK(panel.document().page().objects.empty());
    canvas.setTool(WhiteboardCanvas::Tool::Pen);
    mouse(canvas, QEvent::MouseButtonPress, {30, 30});
    panel.hide(); flush(); panel.show(); flush();
    mouse(canvas, QEvent::MouseButtonRelease, {60, 60});
    TEST_CHECK(panel.document().page().objects.empty());
    canvas.setInkColor(Qt::red);
    stroke(canvas, WhiteboardCanvas::Tool::Highlighter, {100, 100}, {500, 100});
    const auto highlight = canvas.exportImage();
    TEST_CHECK(highlight.pixelColor(300, 100).red() == 255);
    TEST_CHECK(highlight.pixelColor(300, 100).green() > 150 && highlight.pixelColor(300, 100).green() < 220);
    stroke(canvas, WhiteboardCanvas::Tool::Eraser, {300, 100}, {300, 100});
    TEST_CHECK(panel.document().page().objects.empty());
    TEST_CHECK(canvas.exportImage().pixelColor(300, 100) == QColor(Qt::white));
    TEST_CHECK(!panel.document().canUndo(panel.document().owner()));
    const auto count = panel.document().page().objects.size();
    stroke(canvas, WhiteboardCanvas::Tool::Laser, {300, 300}, {400, 400});
    TEST_CHECK(panel.document().page().objects.size() == count);
    canvas.setZoom(1.5);
    const auto original = QPointF(960, 540);
    const auto mapped = canvas.viewToDocument(canvas.documentToView(original));
    TEST_CHECK(mapped && QLineF(*mapped, original).length() < 0.001);
    canvas.fitPage();
    TEST_CHECK(!canvas.viewToDocument({0, 0}));
    stroke(canvas, WhiteboardCanvas::Tool::Line, {100, 100}, {500, 200});
    canvas.setTool(WhiteboardCanvas::Tool::Pen);
    mouse(canvas, QEvent::MouseButtonPress, {0, 0}, false);
    mouse(canvas, QEvent::MouseButtonRelease, {0, 0}, false);
    TEST_CHECK(panel.document().page().objects.size() == 1);
}
void clearConfirmationLayout(WhiteboardPanel &panel) {
    bool inspected = false;
    QTimer::singleShot(0, &panel, [&] {
        auto *box = panel.findChild<QMessageBox *>();
        TEST_CHECK(box);
        QTimer::singleShot(0, box, [&, box] {
            TEST_CHECK(box->isVisible());
            TEST_CHECK(box->width() >= 460 && box->height() >= 180);
            TEST_CHECK(!box->findChild<QScrollArea *>(QStringLiteral("adaptiveDialogScroll")));
            auto *cancel = box->button(QMessageBox::Cancel);
            TEST_CHECK(cancel && cancel->isVisible() && cancel->isEnabled());
            inspected = true;
            cancel->click();
        });
    });
    panel.findChild<QPushButton *>("whiteboardClear")->click();
    TEST_CHECK(inspected);
}
void bounded4kViewport() {
    wb::Document document("4k-view");
    WhiteboardCanvas canvas(document);
    canvas.setAttribute(Qt::WA_DontShowOnScreen);
    canvas.resize(3840, 2160);
    canvas.show(); flush();
    TEST_CHECK(canvas.size() == QSize(3840, 2160));
    TEST_CHECK(canvas.cacheBytes() > 0 && canvas.cacheBytes() <= 64 * 1024 * 1024);
    std::vector<double> milliseconds;
    canvas.setTool(WhiteboardCanvas::Tool::Pen);
    mouse(canvas, QEvent::MouseButtonPress, {50, 500});
    for (int i = 1; i <= 1000; ++i) {
        QElapsedTimer timer; timer.start();
        mouse(canvas, QEvent::MouseMove, {50 + i * 1.7, 500.0 + (i % 40)});
        flush();
        milliseconds.push_back(timer.nsecsElapsed() / 1000000.0);
    }
    mouse(canvas, QEvent::MouseButtonRelease, {1750, 500});
    for (const auto scale : {0.5, 1.0, 1.5, 2.0, 1.0}) { canvas.setZoom(scale); flush(); }
    TEST_CHECK(document.page().objects.size() == 1 && document.page().objects[0].points.size() > 900);
    TEST_CHECK(canvas.cacheBytes() <= 64 * 1024 * 1024);
    std::sort(milliseconds.begin(), milliseconds.end());
    std::cout << "4K_OFFSCREEN input+paint P95_ms=" << milliseconds[950]
              << " cache_bytes=" << canvas.cacheBytes() << " (not physical display latency)\n";
}
void annotationOverlaySurface() {
    auto *screen = QGuiApplication::primaryScreen();
    TEST_CHECK(screen != nullptr);
    const auto geometry = screen->geometry();
    livekit::ScreenBinding binding;
    binding.share_session_id = "share-test-overlay";
    binding.source_epoch = 1;
    binding.source_id = 1;
    binding.display_name = screen->name().toStdString();
    binding.device_key = L"test-display";
    binding.physical_x = geometry.x();
    binding.physical_y = geometry.y();
    binding.physical_width = geometry.width();
    binding.physical_height = geometry.height();
    binding.canonical_width = std::min(4096, geometry.width());
    binding.canonical_height = std::min(4096, geometry.height());
    MeetingUI::AnnotationOverlayWindow overlay(binding, false);
    auto &canvas = *overlay.canvas();
    TEST_CHECK(canvas.overlayMode());
    TEST_CHECK(!overlay.desktopMode() && overlay.interactionEnabled());
    stroke(canvas, WhiteboardCanvas::Tool::Pen, {40, 40}, {240, 120});
    TEST_CHECK(canvas.document().page().objects.size() == 1);
    const auto layer = canvas.exportImage();
    TEST_CHECK(layer.size() == QSize(binding.canonical_width, binding.canonical_height));
    TEST_CHECK(layer.pixelColor(0, 0).alpha() == 0);
    TEST_CHECK(layer.pixelColor(140, 80).alpha() > 0);

    // Desktop pass-through disables canvas pointer input, but toolbar commands
    // must remain authorized while the share itself is interactive.
    overlay.setDesktopMode(true);
    auto *undo = overlay.toolbar()->findChild<QPushButton *>("annotationUndo");
    auto *redo = overlay.toolbar()->findChild<QPushButton *>("annotationRedo");
    auto *clear = overlay.toolbar()->findChild<QPushButton *>("annotationClear");
    TEST_CHECK(undo && redo && clear && undo->isEnabled() && redo->isEnabled() && clear->isEnabled());
    undo->click();
    TEST_CHECK(canvas.document().page().objects.empty());
    redo->click();
    TEST_CHECK(canvas.document().page().objects.size() == 1);
    bool clearConfirmed = false;
    QTimer::singleShot(0, overlay.toolbar(), [&] {
        auto *box = overlay.toolbar()->findChild<QMessageBox *>();
        TEST_CHECK(box);
        QTimer::singleShot(0, box, [&, box] {
            TEST_CHECK(box->isVisible() && box->width() >= 460 && box->height() >= 180);
            clearConfirmed = true;
            box->button(QMessageBox::Yes)->click();
        });
    });
    clear->click();
    TEST_CHECK(clearConfirmed && canvas.document().page().objects.empty());
    overlay.setDesktopMode(false);
    stroke(canvas, WhiteboardCanvas::Tool::Pen, {40, 40}, {240, 120});
    TEST_CHECK(canvas.document().page().objects.size() == 1);

    canvas.setTool(WhiteboardCanvas::Tool::Pen);
    mouse(canvas, QEvent::MouseButtonPress, {300, 180});
    overlay.setInteractionEnabled(false);
    mouse(canvas, QEvent::MouseButtonRelease, {500, 240});
    TEST_CHECK(canvas.document().page().objects.size() == 1);
    TEST_CHECK(overlay.desktopMode() && !overlay.interactionEnabled());
    overlay.setInteractionEnabled(true);
    TEST_CHECK(overlay.desktopMode() && overlay.interactionEnabled());
    overlay.setDesktopMode(false);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &escape);
    TEST_CHECK(overlay.desktopMode());
    overlay.closeOverlay();
}
void comboPopupContrast() {
    QWidget darkMeeting;
    MeetingUI::AppTheme::setTone(darkMeeting, MeetingUI::AppTheme::Tone::Dark);
    darkMeeting.resize(1120, 700);
    darkMeeting.setAttribute(Qt::WA_DontShowOnScreen);
    WhiteboardPanel panel(&darkMeeting);
    panel.setGeometry(darkMeeting.rect());
    darkMeeting.show();
    panel.show();
    flush();
    const auto combos = panel.findChildren<QComboBox *>();
    TEST_CHECK(combos.size() == 3);
    for (auto *combo : combos) {
        combo->showPopup();
        flush();
        auto *view = combo->view();
        TEST_CHECK(view && view->viewport());
        const auto viewPalette = view->palette();
        const auto viewportPalette = view->viewport()->palette();
        const auto popupPalette = view->window()->palette();
        std::cout << "COMBO_POPUP " << combo->accessibleName().toStdString()
                  << " view_base=" << viewPalette.color(QPalette::Base).name().toStdString()
                  << " view_text=" << viewPalette.color(QPalette::Text).name().toStdString()
                  << " viewport_base=" << viewportPalette.color(QPalette::Base).name().toStdString()
                  << " popup_window=" << popupPalette.color(QPalette::Window).name().toStdString() << '\n';
        TEST_CHECK(combo->property("meetingUiTone").toByteArray() == "light");
        TEST_CHECK(view->property("meetingUiTone").toByteArray() == "light");
        TEST_CHECK(view->viewport()->property("meetingUiTone").toByteArray() == "light");
        TEST_CHECK(viewPalette.color(QPalette::Base) == QColor("#ffffff"));
        TEST_CHECK(viewPalette.color(QPalette::Text) == QColor("#1f2329"));
        TEST_CHECK(viewportPalette.color(QPalette::Base) == QColor("#ffffff"));
        TEST_CHECK(viewportPalette.color(QPalette::Text) == QColor("#1f2329"));
        TEST_CHECK(popupPalette.color(QPalette::Window) == QColor("#ffffff"));
        const auto pixels = view->viewport()->grab().toImage().convertToFormat(QImage::Format_RGB32);
        int lightPixels = 0, darkPixels = 0;
        for (int y = 0; y < pixels.height(); ++y) for (int x = 0; x < pixels.width(); ++x) {
            const auto color = pixels.pixelColor(x, y);
            const auto luminance = (color.red() * 299 + color.green() * 587 + color.blue() * 114) / 1000;
            if (luminance >= 200) ++lightPixels;
            else if (luminance <= 80) ++darkPixels;
        }
        TEST_CHECK(lightPixels > darkPixels * 4);
        combo->hidePopup();
        flush();
    }
}
void collaborativeProjection() {
    WhiteboardPanel panel;
    panel.resize(1120, 700);
    panel.setAttribute(Qt::WA_DontShowOnScreen);
    panel.show();
    panel.enableCollaboration();
    flush();
    int proposals = 0;
    wb::Command proposed;
    QObject::connect(&panel, &WhiteboardPanel::commandProposed, &panel,
        [&](const wb::Command &command) { ++proposals; proposed = command; });
    stroke(*panel.canvas(), WhiteboardCanvas::Tool::Line, {100, 100}, {300, 200});
    TEST_CHECK(proposals == 0 && panel.document().page().objects.empty());

    wb::Document projection("board-projection", "actor-host");
    wb::Command committed;
    committed.id = "committed-1";
    committed.actor = "actor-writer";
    committed.context = projection.context();
    committed.kind = wb::CommandKind::Add;
    committed.object.id = "object-committed-1";
    committed.object.author = committed.actor;
    committed.object.kind = wb::ObjectKind::Line;
    committed.object.points = {{100, 100}, {300, 200}};
    TEST_CHECK(projection.apply(committed).changed());
    const auto bytes = projection.toJson();
    panel.applyProjection(QByteArray(bytes.data(), static_cast<int>(bytes.size())), 1,
        static_cast<int>(wb::CollaborationState::Ready), QStringLiteral("host"),
        QStringLiteral("actor-writer"), false, true, true, false, {}, QStringLiteral("ready"));
    TEST_CHECK(panel.document().page().objects.size() == 1);
    stroke(*panel.canvas(), WhiteboardCanvas::Tool::Line, {400, 100}, {600, 200});
    TEST_CHECK(proposals == 1 && proposed.actor == "actor-writer");
    TEST_CHECK(panel.document().page().objects.size() == 1);

    panel.applyProjection(QByteArray(bytes.data(), static_cast<int>(bytes.size())), 2,
        static_cast<int>(wb::CollaborationState::ReadOnly), QStringLiteral("host"),
        QStringLiteral("actor-writer"), false, true, false, false, {}, QStringLiteral("read-only"));
    stroke(*panel.canvas(), WhiteboardCanvas::Tool::Line, {700, 100}, {900, 200});
    TEST_CHECK(proposals == 1);

    panel.applyProjection(QByteArray(bytes.data(), static_cast<int>(bytes.size())), 3,
        static_cast<int>(wb::CollaborationState::Ready), QStringLiteral("host"),
        QStringLiteral("actor-host"), false, true, true, true, {}, QStringLiteral("ready"));
    TEST_CHECK(panel.findChild<QPushButton *>("whiteboardLock")->isVisible());
    TEST_CHECK(panel.findChild<QPushButton *>("whiteboardWriters")->isVisible());
}
void imagePageLoadingAndProjection() {
    QTemporaryDir directory;
    TEST_CHECK(directory.isValid());
    QImage source(640, 360, QImage::Format_RGB32);
    source.fill(QColor(30, 120, 210));
    const auto disguised = directory.filePath("actual-content.jpg");
    TEST_CHECK(source.save(disguised, "PNG"));
    const auto loaded = MeetingUI::loadAndNormalizeWhiteboardImage(disguised);
    TEST_CHECK(loaded.ok() && loaded.asset.mime == "image/png");
    TEST_CHECK(loaded.asset.width == 640 && loaded.asset.height == 360);
    TEST_CHECK(wb::assetContentId(loaded.asset.bytes) == loaded.asset.id);

    wb::Document projection("image-projection", "actor-host");
    wb::Command page;
    page.id = "image-page-op";
    page.actor = "actor-host";
    page.context = projection.context();
    page.kind = wb::CommandKind::AddImagePage;
    page.target = "image-page";
    page.assetId = loaded.asset.id;
    page.pageWidth = loaded.asset.width;
    page.pageHeight = loaded.asset.height;
    TEST_CHECK(projection.apply(page).changed());

    WhiteboardPanel panel;
    panel.resize(1120, 700);
    panel.setAttribute(Qt::WA_DontShowOnScreen);
    panel.show();
    int proposals = 0;
    QObject::connect(&panel, &WhiteboardPanel::commandProposed, &panel,
        [&](const wb::Command &) { ++proposals; });
    const auto snapshot = projection.toJson();
    panel.applyProjection(QByteArray(snapshot.data(), static_cast<int>(snapshot.size())), 1,
        static_cast<int>(wb::CollaborationState::Ready), QStringLiteral("host"),
        QStringLiteral("actor-writer"), false, true, true, false, {}, QStringLiteral("ready"));
    stroke(*panel.canvas(), WhiteboardCanvas::Tool::Line, {100, 100}, {300, 200});
    TEST_CHECK(proposals == 0);

    QVariantMap assets;
    assets.insert(QString::fromStdString(loaded.asset.id),
        QByteArray(loaded.asset.bytes.data(), static_cast<int>(loaded.asset.bytes.size())));
    panel.applyProjection(QByteArray(snapshot.data(), static_cast<int>(snapshot.size())), 1,
        static_cast<int>(wb::CollaborationState::Ready), QStringLiteral("host"),
        QStringLiteral("actor-writer"), false, true, true, false, assets, QStringLiteral("ready"));
    QElapsedTimer wait;
    wait.start();
    while (!panel.canvas()->hasBackgroundImage(loaded.asset.id) && wait.elapsed() < 10000) {
        flush();
        QThread::msleep(1);
    }
    TEST_CHECK(panel.canvas()->hasBackgroundImage(loaded.asset.id));
    TEST_CHECK(panel.canvas()->exportImage().pixelColor(20, 20) == QColor(30, 120, 210));
    stroke(*panel.canvas(), WhiteboardCanvas::Tool::Line, {100, 100}, {300, 200});
    TEST_CHECK(proposals == 1);
    TEST_CHECK(panel.canvas()->backgroundCacheBytes() <= 96LL * 1024 * 1024);

    QFile oversized(directory.filePath("oversized.png"));
    TEST_CHECK(oversized.open(QIODevice::WriteOnly));
    TEST_CHECK(oversized.resize(static_cast<qint64>(wb::MaxAssetBytes + 1)));
    oversized.close();
    TEST_CHECK(MeetingUI::loadAndNormalizeWhiteboardImage(oversized.fileName()).error ==
        MeetingUI::WhiteboardImageError::InputTooLarge);
}
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    qInstallMessageHandler(messages);
    MeetingUI::AppTranslation::install(app, QLocale("zh_CN"));
    MeetingUI::AppTheme::install(app);
    WhiteboardPanel panel;
    panel.resize(1120, 700);
    const bool interactive = app.arguments().contains("--interactive");
    if (!interactive) panel.setAttribute(Qt::WA_DontShowOnScreen);
    panel.show(); flush();
    toolsAndText(panel);
    clearConfirmationLayout(panel);
    TEST_CHECK(panel.findChild<QLabel *>("whiteboardLocalNotice")->text() == QStringLiteral("仅自己可见"));
    if (interactive) return app.exec();
    if (app.arguments().contains("--preview")) {
        const auto index = app.arguments().indexOf("--preview");
        TEST_CHECK(index + 1 < app.arguments().size());
        TEST_CHECK(panel.grab().save(app.arguments()[index + 1], "PNG"));
    }
    exportFromPanel(panel);
    cancellationAndEraser(panel);
    comboPopupContrast();
    collaborativeProjection();
    imagePageLoadingAndProjection();
    bounded4kViewport();
    annotationOverlaySurface();
    TEST_CHECK(!styleWarning);
    std::cout << "WHITEBOARD_UI PASS: tools, IME events, cancellation, export, mapping, bounded cache\n";
}
