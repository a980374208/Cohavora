#include "remote_input.h"
#include "src/core/remote_control/input_arbitration.h"
#include <windows.h>
#include <array>
#include <deque>
#include <mutex>
#include <set>
#include <thread>

namespace livekit::remote_control {
namespace {
constexpr ULONG_PTR InputMarker = 0x434f4852;
constexpr unsigned MouseToken(int button) { return 0x10000u + button; }
unsigned KeyToken(DWORD scan, DWORD flags) { return (scan & 0xff) | ((flags & LLKHF_EXTENDED) ? 0x100 : 0); }
bool InteractiveDesktop() {
    const auto input = OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS);
    if (!input) return false;
    std::array<wchar_t,256> name{}, own{};
    DWORD length = 0;
    const bool okay = GetUserObjectInformationW(input,UOI_NAME,name.data(),sizeof(name),&length) &&
        GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),UOI_NAME,
            own.data(),sizeof(own),&length) && std::wstring_view(name.data()) == own.data();
    CloseDesktop(input);
    return okay;
}
DWORD ButtonFlag(int code, bool down) {
    if (code == 1) return down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    if (code == 2) return down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    return down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
}
class WindowsInput final : public InputBackend {
public:
    WindowsInput() = default;
    ~WindowsInput() override { stop(); quitting_ = true; wake(); if (worker_.joinable()) worker_.join(); }
    bool start(const ScreenBinding& binding, std::shared_ptr<Lease> lease) override {
        std::lock_guard lock(mutex_);
        if ((worker_.joinable() && !ready_) || !keys_.empty() || !buttons_.empty() || (lease_ && lease_->valid)) return false;
        binding_ = binding; lease_ = std::move(lease); queue_.clear();
        arbitration_.reset();
        // Start the native owner only after explicit consent and Ready. The
        // owner installs both hooks before draining any queued input.
        if (!worker_.joinable()) {
            try { worker_ = std::thread([this] { run(); }); }
            catch (...) { lease_->valid = false; return false; }
        }
        wake();
        return true;
    }
    bool submit(const Input& input, uint64_t inputEpoch) override {
        std::lock_guard lock(mutex_);
        if (!lease_ || !lease_->permits(NowMs()) || !input.valid()) return false;
        if (!arbitration_.accepts(inputEpoch)) return true; // Paused/stale input is discarded, never replayed.
        // Consecutive motion can be superseded, never cross a button/key edge.
        if (input.kind == InputKind::Move && !queue_.empty() && queue_.back().input.kind == InputKind::Move)
            queue_.back() = {input,inputEpoch};
        else if (queue_.size() < 64) queue_.push_back({input,inputEpoch});
        else { lease_->valid = false; queue_.clear(); return false; }
        wake();
        return true;
    }
    void pause(bool value) override {
        std::lock_guard lock(mutex_);
        arbitration_.pause(value); queue_.clear(); wake();
    }
    InputStatus status() override { std::lock_guard lock(mutex_); return arbitration_.status(); }
    void stop() override {
        std::lock_guard lock(mutex_);
        if (lease_) lease_->valid = false;
        queue_.clear();
        wake();
    }
private:
    static thread_local WindowsInput* owner_;
    void wake() { const auto id = threadId_.load(); if (id) PostThreadMessageW(id,WM_APP,0,0); }
    static LRESULT CALLBACK MouseHook(int code, WPARAM w, LPARAM l) {
        if (code >= 0 && owner_) {
            const auto& event = *reinterpret_cast<MSLLHOOKSTRUCT*>(l);
            if (!(event.flags & LLMHF_INJECTED) || event.dwExtraInfo != InputMarker) {
                int button = 0; bool down = false;
                switch (w) {
                case WM_LBUTTONDOWN: down = true; [[fallthrough]];
                case WM_LBUTTONUP: button = 1; break;
                case WM_RBUTTONDOWN: down = true; [[fallthrough]];
                case WM_RBUTTONUP: button = 2; break;
                case WM_MBUTTONDOWN: down = true; [[fallthrough]];
                case WM_MBUTTONUP: button = 3; break;
                case WM_XBUTTONDOWN: down = true; [[fallthrough]];
                case WM_XBUTTONUP: button = HIWORD(event.mouseData) == XBUTTON1 ? 4 : 5; break;
                default: break;
                }
                owner_->localActivity(button ? MouseToken(button) : 0,down);
            }
        }
        return CallNextHookEx(nullptr,code,w,l);
    }
    static LRESULT CALLBACK KeyHook(int code, WPARAM w, LPARAM l) {
        if (code >= 0 && owner_) {
            const auto& event = *reinterpret_cast<KBDLLHOOKSTRUCT*>(l);
            if (!(event.flags & LLKHF_INJECTED) || event.dwExtraInfo != InputMarker) {
                const auto token = KeyToken(event.scanCode,event.flags);
                owner_->localActivity(token ? token : 0x20000u + event.vkCode,!(event.flags & LLKHF_UP));
            }
        }
        return CallNextHookEx(nullptr,code,w,l);
    }
    void localActivity(unsigned token, bool down) {
        std::lock_guard lock(mutex_);
        arbitration_.localActivity(NowMs(),token,down);
        queue_.clear();
        // Preserve an explicit local emergency exit after ordinary activity
        // changes from revocation to temporary priority.
        if (token == 0x58 && down &&
            (arbitration_.locallyHeld(0x1d) || arbitration_.locallyHeld(0x11d)) &&
            (arbitration_.locallyHeld(0x38) || arbitration_.locallyHeld(0x138))) {
            if (lease_) lease_->valid = false;
        }
        release(); // Release remote modifiers before forwarding the local event.
        wake();
    }
    bool execute(const Input& value) {
        if (value.kind == InputKind::Key && !value.down && !keys_.contains({value.code,value.extended})) return true;
        if (value.kind == InputKind::Button && !value.down && !buttons_.contains(value.code)) return true;
        INPUT input[2]{};
        UINT count = 1;
        if (value.kind == InputKind::Key) {
            input[0].type = INPUT_KEYBOARD;
            input[0].ki.wScan = WORD(value.code);
            input[0].ki.dwFlags = KEYEVENTF_SCANCODE | (value.extended ? KEYEVENTF_EXTENDEDKEY : 0) |
                (value.down ? 0 : KEYEVENTF_KEYUP);
            input[0].ki.dwExtraInfo = InputMarker;
        } else {
            const auto point = MapAbsolute(value.x,value.y,binding_,
                GetSystemMetrics(SM_XVIRTUALSCREEN),GetSystemMetrics(SM_YVIRTUALSCREEN),
                GetSystemMetrics(SM_CXVIRTUALSCREEN),GetSystemMetrics(SM_CYVIRTUALSCREEN));
            if (!point) return false;
            input[0].type = INPUT_MOUSE;
            input[0].mi.dx = point->x; input[0].mi.dy = point->y;
            input[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
            input[0].mi.dwExtraInfo = InputMarker;
            if (value.kind != InputKind::Move) {
                count = 2; input[1].type = INPUT_MOUSE; input[1].mi.dwExtraInfo = InputMarker;
                input[1].mi.dwFlags = value.kind == InputKind::Button
                    ? ButtonFlag(value.code,value.down) : MOUSEEVENTF_WHEEL;
                if (value.kind == InputKind::Wheel) input[1].mi.mouseData = DWORD(value.code);
            }
        }
        // Conservatively record a requested down before SendInput; a partial
        // insertion must also be released during teardown.
        const auto key = std::pair(value.code,value.extended);
        if (value.kind == InputKind::Key && value.down) keys_.insert(key);
        if (value.kind == InputKind::Button && value.down) buttons_.insert(value.code);
        if (SendInput(count,input,sizeof(INPUT)) != count) return false;
        if (value.kind == InputKind::Key && !value.down) keys_.erase(key);
        if (value.kind == InputKind::Button && !value.down) buttons_.erase(value.code);
        return true;
    }
    void release() {
        if (releasing_) return;
        releasing_ = true;
        // Retry failed releases while alive; never grant another controller
        // while an earlier injection has an unconfirmed release.
        for (auto it = keys_.begin(); it != keys_.end();) {
            if (arbitration_.locallyHeld(unsigned(it->first) | (it->second ? 0x100u : 0))) { ++it; continue; }
            INPUT i{}; i.type = INPUT_KEYBOARD; i.ki.wScan = WORD(it->first);
            i.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP | (it->second ? KEYEVENTF_EXTENDEDKEY : 0);
            i.ki.dwExtraInfo = InputMarker;
            if (SendInput(1,&i,sizeof(i)) == 1) it = keys_.erase(it); else ++it;
        }
        for (auto it = buttons_.begin(); it != buttons_.end();) {
            if (arbitration_.locallyHeld(MouseToken(*it))) { ++it; continue; }
            INPUT i{}; i.type = INPUT_MOUSE; i.mi.dwFlags = ButtonFlag(*it,false); i.mi.dwExtraInfo = InputMarker;
            if (SendInput(1,&i,sizeof(i)) == 1) it = buttons_.erase(it); else ++it;
        }
        releasing_ = false;
    }
    void run() {
        owner_ = this;
        MSG initial;
        PeekMessageW(&initial,nullptr,0,0,PM_NOREMOVE);
        threadId_ = GetCurrentThreadId();
        // Work in physical coordinates regardless of the calling Qt thread's
        // DPI context. This does not change the process-wide DPI policy.
        const auto dpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const auto mouse = SetWindowsHookExW(WH_MOUSE_LL,MouseHook,GetModuleHandleW(nullptr),0);
        const auto key = SetWindowsHookExW(WH_KEYBOARD_LL,KeyHook,GetModuleHandleW(nullptr),0);
        ready_ = dpi && mouse && key && InteractiveDesktop();
        // Account for a local key/button held before the hooks were installed.
        // Only physical state is retained, never text or an input history.
        if (ready_) {
            std::lock_guard lock(mutex_);
            for (unsigned vk = 8; vk < 256; ++vk) if (vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU &&
                (GetAsyncKeyState(int(vk)) & 0x8000)) {
                const auto scan = MapVirtualKeyW(vk,MAPVK_VK_TO_VSC_EX);
                arbitration_.localActivity(NowMs(),scan ? (scan & 0xff) | ((scan & 0xff00) ? 0x100 : 0)
                    : 0x20000u + vk,true);
            }
            const int mouseKeys[] = {VK_LBUTTON,VK_RBUTTON,VK_MBUTTON,VK_XBUTTON1,VK_XBUTTON2};
            for (int i = 0; i < 5; ++i) if (GetAsyncKeyState(mouseKeys[i]) & 0x8000)
                arbitration_.localActivity(NowMs(),MouseToken(i+1),true);
        }
        if (!ready_) { std::lock_guard lock(mutex_); if (lease_) lease_->valid = false; }
        while (!quitting_) {
            DWORD waitMs = 250;
            MSG msg;
            while (PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            {
                std::lock_guard lock(mutex_);
                if (lease_ && (!lease_->permits(NowMs()) || !InteractiveDesktop())) lease_->valid = false;
                if (!lease_ || !lease_->valid) { queue_.clear(); release(); }
                else {
                    if (arbitration_.status().paused) { queue_.clear(); release(); }
                    arbitration_.poll(NowMs(),keys_.empty() && buttons_.empty());
                    if (!queue_.empty()) {
                        const auto value = queue_.front(); queue_.pop_front();
                        if (arbitration_.accepts(value.epoch) && !execute(value.input)) {
                            lease_->valid = false; queue_.clear(); release();
                        }
                    }
                }
                if (lease_ && lease_->valid) waitMs = 5;
            }
            MsgWaitForMultipleObjects(0,nullptr,FALSE,waitMs,QS_ALLINPUT);
        }
        { std::lock_guard lock(mutex_); release(); }
        if (mouse) UnhookWindowsHookEx(mouse);
        if (key) UnhookWindowsHookEx(key);
        ready_ = false; owner_ = nullptr;
        threadId_ = 0;
    }
    // SendInput can re-enter a native hook on this owner thread.
    std::recursive_mutex mutex_;
    struct PendingInput { Input input; uint64_t epoch; };
    std::deque<PendingInput> queue_;
    InputArbitration arbitration_;
    bool releasing_ = false;
    std::set<std::pair<int,bool>> keys_;
    std::set<int> buttons_;
    ScreenBinding binding_;
    std::shared_ptr<Lease> lease_;
    std::atomic<bool> ready_{false}, quitting_{false};
    std::atomic<DWORD> threadId_{0};
    std::thread worker_;
};
thread_local WindowsInput* WindowsInput::owner_ = nullptr;
} // namespace
std::shared_ptr<InputBackend> CreateWindowsInputBackend() { return std::make_shared<WindowsInput>(); }
} // namespace livekit::remote_control
