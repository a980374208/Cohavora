// Opt-in Windows M0 probe for the native Qt annotation overlay. The probe
// captures no output image and samples only its own color marker. It also
// restores the pointer after exercising real cross-process hit testing.
#include "desktop_capture.h"

#include "modules/desktop_capture/win/screen_capture_utils.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QStringList>
#include <QtGui/QInputMethodEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QScreen>
#include <QtGui/QWindow>
#include <QtPlugin>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QWidget>

#include <windows.h>
#include <imm.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)

namespace {

using namespace std::chrono_literals;

constexpr COLORREF kFixtureColor = RGB(34, 68, 102);

struct ScreenBinding {
    livekit::DesktopSource source;
    std::string deviceName;
    std::wstring deviceKey;
    HMONITOR monitor = nullptr;
    RECT physical{};
    QScreen *screen = nullptr;
};

std::string Utf8(const QString &value) {
    return value.toUtf8().constData();
}

std::string NormalizedDisplayName(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    const std::string prefix = "\\\\.\\";
    if (value.rfind(prefix, 0) == 0) value.erase(0, prefix.size());
    return value;
}

bool PumpUntil(const std::function<bool()> &done, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeoutMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        Sleep(5);
    }
    QApplication::processEvents();
    return done();
}

int ChildFixture(int argc, char **argv) {
    if (argc != 6) return 2;
    const int x = std::stoi(argv[2]);
    const int y = std::stoi(argv[3]);
    const int width = std::stoi(argv[4]);
    const int height = std::stoi(argv[5]);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto instance = GetModuleHandleW(nullptr);
    constexpr wchar_t kClassName[] = L"CohavoraM0ClickFixture";
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = [](HWND hwnd, UINT message, WPARAM wp, LPARAM lp) -> LRESULT {
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            const auto dc = BeginPaint(hwnd, &paint);
            RECT rect{};
            GetClientRect(hwnd, &rect);
            const auto brush = CreateSolidBrush(kFixtureColor);
            FillRect(dc, &rect, brush);
            DeleteObject(brush);
            EndPaint(hwnd, &paint);
            return 0;
        }
        if (message == WM_LBUTTONDOWN) {
            auto clicks = GetWindowLongPtrW(hwnd, GWLP_USERDATA) + 1;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, clicks);
            std::cout << "CLICK:" << clicks << std::endl;
            return 0;
        }
        if (message == WM_CLOSE) {
            DestroyWindow(hwnd);
            return 0;
        }
        if (message == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, message, wp, lp);
    };
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kClassName;
    RegisterClassW(&windowClass);

    const auto hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kClassName,
        L"M0 click-through fixture",
        WS_POPUP | WS_VISIBLE,
        x,
        y,
        width,
        height,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (!hwnd) return 3;
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, width, height, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    UpdateWindow(hwnd);
    std::cout << "READY:" << reinterpret_cast<std::uintptr_t>(hwnd) << std::endl;

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

class FixtureProcess final {
public:
    ~FixtureProcess() { stop(); }

    bool start(const RECT &rect) {
        process_.setProgram(QCoreApplication::applicationFilePath());
        process_.setArguments({
            QStringLiteral("--click-fixture"),
            QString::number(rect.left),
            QString::number(rect.top),
            QString::number(rect.right - rect.left),
            QString::number(rect.bottom - rect.top),
        });
        process_.setProcessChannelMode(QProcess::MergedChannels);
        process_.start();
        if (!process_.waitForStarted(5000)) return false;
        const bool ready = PumpUntil([this] {
            collect();
            return hwnd_ != nullptr || process_.state() == QProcess::NotRunning;
        }, 5000);
        return ready && hwnd_ != nullptr;
    }

    void stop() {
        if (process_.state() == QProcess::NotRunning) return;
        process_.terminate();
        if (!process_.waitForFinished(2000)) {
            process_.kill();
            process_.waitForFinished(2000);
        }
    }

    int clicks() {
        collect();
        return clicks_;
    }

    HWND hwnd() const { return hwnd_; }

private:
    void collect() {
        output_ += process_.readAll();
        for (;;) {
            const int newline = output_.indexOf('\n');
            if (newline < 0) break;
            const auto line = QString::fromUtf8(output_.left(newline)).trimmed();
            output_.remove(0, newline + 1);
            if (line.startsWith(QStringLiteral("READY:"))) {
                bool ok = false;
                const auto value = line.mid(6).toULongLong(&ok);
                if (ok) hwnd_ = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(value));
            } else if (line.startsWith(QStringLiteral("CLICK:"))) {
                bool ok = false;
                const int value = line.mid(6).toInt(&ok);
                if (ok) clicks_ = std::max(clicks_, value);
            }
        }
    }

    QProcess process_;
    QByteArray output_;
    HWND hwnd_ = nullptr;
    int clicks_ = 0;
};

class PointerRestore final {
public:
    PointerRestore() { valid_ = GetCursorPos(&position_) != FALSE; }
    ~PointerRestore() { if (valid_) SetCursorPos(position_.x, position_.y); }
private:
    POINT position_{};
    bool valid_ = false;
};

class ForegroundRestore final {
public:
    ForegroundRestore() : window_(GetForegroundWindow()) {}
    ~ForegroundRestore() {
        if (window_ && IsWindow(window_)) SetForegroundWindow(window_);
    }
private:
    HWND window_ = nullptr;
};

bool SendPointer(const POINT &point, bool drag) {
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (width <= 1 || height <= 1) return false;

    const auto absoluteX = static_cast<LONG>(std::llround(
        (point.x - left) * 65535.0 / (width - 1)));
    const auto absoluteY = static_cast<LONG>(std::llround(
        (point.y - top) * 65535.0 / (height - 1)));
    const auto makeInput = [](LONG x, LONG y, DWORD flags) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dx = x;
        input.mi.dy = y;
        input.mi.dwFlags = flags;
        return input;
    };

    std::vector<INPUT> inputs;
    inputs.push_back(makeInput(absoluteX, absoluteY,
        MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK));
    inputs.push_back(makeInput(0, 0, MOUSEEVENTF_LEFTDOWN));
    if (drag) {
        const POINT end{point.x + 80, point.y + 40};
        const auto endX = static_cast<LONG>(std::llround(
            (end.x - left) * 65535.0 / (width - 1)));
        const auto endY = static_cast<LONG>(std::llround(
            (end.y - top) * 65535.0 / (height - 1)));
        inputs.push_back(makeInput(endX, endY,
            MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK));
    }
    inputs.push_back(makeInput(0, 0, MOUSEEVENTF_LEFTUP));
    return SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT)) == inputs.size();
}

class ProbeOverlay final : public QWidget {
public:
    explicit ProbeOverlay(QScreen *screen)
        : QWidget(nullptr, Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint) {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoSystemBackground);
        setMouseTracking(true);
        createWinId();
        windowHandle()->setScreen(screen);
        setGeometry(screen->geometry());
        setPassThrough(false);
    }

    QRect markerRect() const {
        const int markerWidth = std::max(160, std::min(320, width() / 4));
        const int markerHeight = std::max(100, std::min(180, height() / 4));
        return QRect((width() - markerWidth) / 2, (height() - markerHeight) / 2,
                     markerWidth, markerHeight);
    }

    void setPassThrough(bool passThrough) {
        passThrough_ = passThrough;
        const auto hwnd = reinterpret_cast<HWND>(winId());
        auto style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        style |= WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
        if (passThrough) style |= WS_EX_TRANSPARENT;
        else style &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style);
        EnableWindow(hwnd, passThrough ? FALSE : TRUE);
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        update();
    }

    int strokePoints() const { return static_cast<int>(stroke_.size()); }

protected:
    bool nativeEvent(const QByteArray &eventType, void *message, long *result) override {
        auto *native = static_cast<MSG *>(message);
        if (passThrough_ && native && native->message == WM_NCHITTEST) {
            *result = HTTRANSPARENT;
            return true;
        }
        return QWidget::nativeEvent(eventType, message, result);
    }

    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        // A zero-alpha pixel in a layered window is skipped by native hit
        // testing. Draw mode needs a minimally nonzero surface so blank areas
        // accept pen input; pass-through mode remains genuinely transparent.
        if (!passThrough_) painter.fillRect(rect(), QColor(0, 0, 0, 1));
        const auto marker = markerRect();
        painter.fillRect(marker.adjusted(0, 0, -marker.width() / 2, 0), QColor(255, 0, 255));
        painter.fillRect(marker.adjusted(marker.width() / 2, 0, 0, 0), QColor(0, 255, 0));
        painter.setPen(QPen(Qt::white, 6));
        painter.drawRect(marker.adjusted(3, 3, -3, -3));
        if (stroke_.size() > 1) {
            painter.setPen(QPen(QColor(255, 230, 0), 12, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.drawPolyline(stroke_.data(), static_cast<int>(stroke_.size()));
        }
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton) {
            drawing_ = true;
            stroke_.push_back(event->pos());
            update();
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        if (drawing_ && event->buttons().testFlag(Qt::LeftButton)) {
            stroke_.push_back(event->pos());
            update();
            event->accept();
            return;
        }
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
        if (drawing_ && event->button() == Qt::LeftButton) {
            drawing_ = false;
            stroke_.push_back(event->pos());
            update();
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

private:
    std::vector<QPoint> stroke_;
    bool passThrough_ = false;
    bool drawing_ = false;
};

std::optional<ScreenBinding> BindScreen(
        const livekit::DesktopSource &source,
        const webrtc::DesktopCapturer::SourceList &screenList,
        const std::vector<std::string> &deviceNames) {
    if (source.kind != livekit::DesktopSourceKind::Screen ||
        screenList.size() != deviceNames.size()) return std::nullopt;

    auto found = screenList.end();
    for (auto it = screenList.begin(); it != screenList.end(); ++it) {
        if (it->id == source.id) {
            if (found != screenList.end()) return std::nullopt;
            found = it;
        }
    }
    if (found == screenList.end()) return std::nullopt;
    const auto index = static_cast<std::size_t>(std::distance(screenList.begin(), found));

    ScreenBinding binding;
    binding.source = source;
    binding.deviceName = deviceNames[index];
    if (!webrtc::GetHmonitorFromDeviceIndex(source.id, &binding.monitor) || !binding.monitor ||
        !webrtc::IsMonitorValid(binding.monitor) ||
        !webrtc::IsScreenValid(source.id, &binding.deviceKey)) return std::nullopt;
    const auto rect = webrtc::GetScreenRect(source.id, binding.deviceKey);
    if (rect.is_empty()) return std::nullopt;
    binding.physical = {rect.left(), rect.top(), rect.right(), rect.bottom()};

    const auto wanted = NormalizedDisplayName(binding.deviceName);
    for (auto *candidate : QGuiApplication::screens()) {
        if (NormalizedDisplayName(Utf8(candidate->name())) == wanted) {
            if (binding.screen) return std::nullopt;
            binding.screen = candidate;
        }
    }
    if (!binding.screen) return std::nullopt;

    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(binding.monitor, &info) ||
        NormalizedDisplayName(Utf8(QString::fromWCharArray(info.szDevice))) != wanted) {
        return std::nullopt;
    }
    return binding;
}

bool MarkerPresent(const livekit::VideoFrame &frame, const QRect &logicalMarker, const QSize &logicalSize) {
    if (frame.type() != livekit::VideoBufferType::I420 || frame.width() < 32 || frame.height() < 32 ||
        logicalSize.width() <= 0 || logicalSize.height() <= 0) return false;
    const auto planes = frame.planeInfos();
    if (planes.size() < 3) return false;
    const auto sample = [&](double localX, double localY) {
        const int x = std::clamp(static_cast<int>(std::llround(
            localX * frame.width() / logicalSize.width())), 0, frame.width() - 1);
        const int y = std::clamp(static_cast<int>(std::llround(
            localY * frame.height() / logicalSize.height())), 0, frame.height() - 1);
        const auto *u = reinterpret_cast<const std::uint8_t *>(planes[1].data_ptr);
        const auto *v = reinterpret_cast<const std::uint8_t *>(planes[2].data_ptr);
        return std::pair<int, int>{
            u[(y / 2) * planes[1].stride + x / 2],
            v[(y / 2) * planes[2].stride + x / 2],
        };
    };
    const auto left = sample(logicalMarker.left() + logicalMarker.width() * 0.25,
                             logicalMarker.center().y());
    const auto right = sample(logicalMarker.left() + logicalMarker.width() * 0.75,
                              logicalMarker.center().y());
    return left.first > 165 && left.second > 175 &&
           right.first < 115 && right.second < 115;
}

bool VerifyCapture(const ScreenBinding &binding, ProbeOverlay &overlay) {
    std::atomic<int> frames{0};
    std::atomic<int> markerFrames{0};
    std::atomic<int> width{0};
    std::atomic<int> height{0};
    std::atomic<bool> ended{false};
    const auto marker = overlay.markerRect();
    const auto overlaySize = overlay.size();
    auto capture = livekit::CreateDesktopCapture();
    capture->Start(binding.source, [&](const livekit::VideoFrame &frame) {
        ++frames;
        width = frame.width();
        height = frame.height();
        if (MarkerPresent(frame, marker, overlaySize)) ++markerFrames;
    }, [&] { ended = true; });
    const bool matched = PumpUntil([&] { return ended || markerFrames >= 2; }, 8000);
    capture->Stop();
    std::cout << "[M0] overlay_capture " << (matched && !ended ? "PASS" : "FAIL")
              << " source_id=" << binding.source.id
              << " frames=" << frames.load()
              << " marker_frames=" << markerFrames.load()
              << " frame=" << width.load() << 'x' << height.load() << '\n';
    return matched && !ended;
}

RECT FixtureRect(const RECT &screen) {
    const int screenWidth = screen.right - screen.left;
    const int screenHeight = screen.bottom - screen.top;
    const int width = std::clamp(screenWidth / 5, 180, 320);
    const int height = std::clamp(screenHeight / 7, 110, 180);
    const int margin = std::max(24, std::min(screenWidth, screenHeight) / 24);
    return {screen.left + margin, screen.top + margin,
            screen.left + margin + width, screen.top + margin + height};
}

bool VerifyInput(const ScreenBinding &binding, ProbeOverlay &overlay) {
    FixtureProcess fixture;
    const auto fixtureRect = FixtureRect(binding.physical);
    if (!fixture.start(fixtureRect)) {
        std::cout << "[M0] mouse_passthrough FAIL reason=fixture_start\n";
        return false;
    }
    const POINT center{
        fixtureRect.left + (fixtureRect.right - fixtureRect.left) / 2,
        fixtureRect.top + (fixtureRect.bottom - fixtureRect.top) / 2,
    };
    if (WindowFromPoint(center) != fixture.hwnd()) {
        std::cout << "[M0] mouse_passthrough FAIL reason=fixture_not_topmost\n";
        return false;
    }

    PointerRestore restore;
    overlay.raise();
    overlay.setPassThrough(true);
    QApplication::processEvents();
    const bool sentThrough = SendPointer(center, false);
    const bool passed = sentThrough && PumpUntil([&] { return fixture.clicks() >= 1; }, 2000);
    const int clicksAfterPass = fixture.clicks();
    std::cout << "[M0] mouse_passthrough " << (passed ? "PASS" : "FAIL")
              << " cross_process=true fixture_clicks=" << clicksAfterPass << '\n';
    if (!passed) return false;

    overlay.setPassThrough(false);
    overlay.raise();
    QApplication::processEvents();
    const bool sentDraw = SendPointer(center, true);
    const bool drawn = sentDraw && PumpUntil([&] { return overlay.strokePoints() >= 2; }, 2000);
    PumpUntil([] { return false; }, 100);
    const bool blocked = fixture.clicks() == clicksAfterPass;
    std::cout << "[M0] draw_input " << (drawn && blocked ? "PASS" : "FAIL")
              << " points=" << overlay.strokePoints()
              << " underlying_clicks=" << fixture.clicks() << '\n';
    return drawn && blocked;
}

bool HasCjk(const QString &value) {
    static const QRegularExpression cjk(QStringLiteral("[\\x{3400}-\\x{9FFF}]") );
    return value.contains(cjk);
}

struct NativeImeResult {
    bool installed = false;
    bool attempted = false;
    bool composed = false;
    bool committed = false;
};

bool SendVirtualKey(WORD key) {
    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = key;
    inputs[1] = inputs[0];
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    return SendInput(2, inputs, sizeof(INPUT)) == 2;
}

NativeImeResult TryNativeChineseIme(QLineEdit &editor, HWND hwnd) {
    NativeImeResult result;
    const int count = GetKeyboardLayoutList(0, nullptr);
    std::vector<HKL> layouts(count > 0 ? count : 0);
    if (count > 0) GetKeyboardLayoutList(count, layouts.data());
    HKL chinese = nullptr;
    for (const auto layout : layouts) {
        const auto language = LOWORD(reinterpret_cast<ULONG_PTR>(layout));
        if (PRIMARYLANGID(language) == LANG_CHINESE && ImmIsIME(layout)) {
            chinese = layout;
            break;
        }
    }
    result.installed = chinese != nullptr;
    if (!chinese) return result;

    const auto originalLayout = GetKeyboardLayout(0);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);
    if (GetFocus() != hwnd || !ActivateKeyboardLayout(chinese, 0)) return result;
    result.attempted = true;
    PumpUntil([] { return false; }, 150);

    bool originalOpen = false;
    DWORD originalConversion = 0;
    DWORD originalSentence = 0;
    if (const auto context = ImmGetContext(hwnd)) {
        originalOpen = ImmGetOpenStatus(context) != FALSE;
        ImmGetConversionStatus(context, &originalConversion, &originalSentence);
        ImmSetOpenStatus(context, TRUE);
        ImmSetConversionStatus(context, originalConversion | IME_CMODE_NATIVE, originalSentence);
        ImmReleaseContext(hwnd, context);
    }

    editor.clear();
    constexpr char keys[] = "ZHONGWEN";
    bool sent = true;
    for (const char key : keys) {
        if (!key) break;
        sent = SendVirtualKey(static_cast<WORD>(key)) && sent;
        PumpUntil([] { return false; }, 20);
    }
    result.composed = sent && PumpUntil([&] {
        const auto context = ImmGetContext(hwnd);
        if (!context) return false;
        const auto bytes = ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0);
        ImmReleaseContext(hwnd, context);
        return bytes > 0;
    }, 2000);
    if (result.composed) {
        SendVirtualKey(VK_SPACE);
        result.committed = PumpUntil([&] { return HasCjk(editor.text()); }, 3000);
    }

    if (const auto context = ImmGetContext(hwnd)) {
        if (!result.committed) ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
        ImmSetConversionStatus(context, originalConversion, originalSentence);
        ImmSetOpenStatus(context, originalOpen ? TRUE : FALSE);
        ImmReleaseContext(hwnd, context);
    }
    ActivateKeyboardLayout(originalLayout, 0);
    PumpUntil([] { return false; }, 100);
    return result;
}

bool VerifyIme(QScreen *screen) {
    QLineEdit editor;
    editor.setWindowFlags(Qt::Tool | Qt::WindowStaysOnTopHint);
    editor.setAttribute(Qt::WA_InputMethodEnabled, true);
    editor.resize(360, 52);
    const auto geometry = screen->availableGeometry();
    editor.move(geometry.center() - QPoint(editor.width() / 2, editor.height() / 2));
    editor.show();
    editor.raise();
    editor.activateWindow();
    editor.setFocus(Qt::OtherFocusReason);
    const bool focused = PumpUntil([&] { return editor.hasFocus(); }, 2000);

    const auto hwnd = reinterpret_cast<HWND>(editor.winId());
    const auto context = ImmGetContext(hwnd);
    const bool contextAvailable = context != nullptr;
    const bool imeOpen = context && ImmGetOpenStatus(context) != FALSE;
    if (context) ImmReleaseContext(hwnd, context);

    QInputMethodEvent preedit(QString::fromUtf8("中"), {});
    const bool preeditAccepted = QApplication::sendEvent(&editor, &preedit) && preedit.isAccepted();
    QInputMethodEvent commit;
    commit.setCommitString(QString::fromUtf8("中文批注"));
    const bool commitAccepted = QApplication::sendEvent(&editor, &commit) && commit.isAccepted();
    const bool committed = editor.text() == QString::fromUtf8("中文批注") && HasCjk(editor.text());
    const auto native = TryNativeChineseIme(editor, hwnd);

    std::cout << "[M0] ime_context " << (focused && contextAvailable ? "PASS" : "FAIL")
              << " focused=" << focused
              << " associated=" << contextAvailable
              << " open=" << imeOpen << '\n';
    std::cout << "[M0] ime_composition "
              << (preeditAccepted && commitAccepted && committed ? "PASS" : "FAIL")
              << " preedit=" << preeditAccepted
              << " commit=" << commitAccepted
              << " cjk=true\n";
    std::cout << "[M0] ime_native "
              << (!native.installed ? "NOT_RUN" :
                  (native.attempted && native.composed && native.committed ? "PASS" : "FAIL"))
              << " installed=" << native.installed
              << " attempted=" << native.attempted
              << " composition=" << native.composed
              << " cjk_commit=" << native.committed << '\n';
    std::cout << "[M0] ime_candidate_placement NOT_RUN reason=requires_visual_observation\n";
    editor.hide();
    const bool nativeAcceptable = !native.installed ||
        (native.attempted && native.composed && native.committed);
    return focused && contextAvailable && preeditAccepted && commitAccepted && committed && nativeAcceptable;
}

std::vector<ScreenBinding> DiscoverBindings() {
    webrtc::DesktopCapturer::SourceList helperScreens;
    std::vector<std::string> deviceNames;
    if (!webrtc::GetScreenList(&helperScreens, &deviceNames)) return {};

    std::vector<ScreenBinding> bindings;
    for (const auto &source : livekit::EnumerateDesktopSources()) {
        if (source.kind != livekit::DesktopSourceKind::Screen) continue;
        auto binding = BindScreen(source, helperScreens, deviceNames);
        if (!binding) {
            std::cout << "[M0] source_mapping FAIL source_id=" << source.id << '\n';
            continue;
        }
        const auto qt = binding->screen->geometry();
        std::cout << "[M0] source_mapping PASS source_id=" << source.id
                  << " display=" << NormalizedDisplayName(binding->deviceName)
                  << " physical=" << binding->physical.left << ',' << binding->physical.top
                  << ',' << binding->physical.right - binding->physical.left
                  << 'x' << binding->physical.bottom - binding->physical.top
                  << " qt=" << qt.x() << ',' << qt.y() << ',' << qt.width() << 'x' << qt.height()
                  << " dpr=" << binding->screen->devicePixelRatio() << '\n';
        bindings.push_back(std::move(*binding));
    }
    return bindings;
}

int RunProbe() {
    ForegroundRestore foregroundRestore;
    const auto bindings = DiscoverBindings();
    if (bindings.empty()) {
        std::cout << "[M0] RESULT NOT_RUN reason=no_unambiguous_screen_binding\n";
        return 2;
    }

    bool passed = true;
    for (const auto &binding : bindings) {
        ProbeOverlay overlay(binding.screen);
        overlay.show();
        overlay.raise();
        QApplication::processEvents();
        passed = VerifyCapture(binding, overlay) && passed;
        passed = VerifyInput(binding, overlay) && passed;
        overlay.hide();
        QApplication::processEvents();
    }
    passed = VerifyIme(bindings.front().screen) && passed;
    std::cout << "[M0] RESULT " << (passed ? "PASS" : "FAIL")
              << " screens=" << bindings.size()
              << " ime_candidate_placement=NOT_RUN\n";
    return passed ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
    if (argc > 1 && std::string(argv[1]) == "--click-fixture") {
        return ChildFixture(argc, argv);
    }
    if (argc > 1) {
        std::cout << "usage: test_annotation_overlay_runtime\n";
        return 2;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    return RunProbe();
}
