#include "dyslexia_reader/windows/tray_shell.hpp"

#include <shellapi.h>

#include <array>
#include <cwctype>
#include <utility>

namespace dyslexia_reader::windows {
namespace {

constexpr wchar_t kTrayClassName[] = L"DyslexiaReader.TrayHostWindow";
constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayCallbackMsg = WM_APP + 0x41;

constexpr int kHotkeyCaptureId = 1;
constexpr int kHotkeyToggleVisibilityId = 2;
constexpr int kHotkeyReadClipboardId = 3;

constexpr UINT kCmdCapture = 4001;
constexpr UINT kCmdReadClipboard = 4002;
constexpr UINT kCmdToggleVisibility = 4003;
constexpr UINT kCmdToggleMode = 4004;
constexpr UINT kCmdExit = 4005;
constexpr UINT kCmdImportShareCode = 4006;
constexpr UINT kCmdShareProfile = 4007;
constexpr UINT kCmdOpenWebPlayground = 4008;

struct HotkeySpec {
    int id;
    UINT modifiers;
    UINT vk;
    const char* label;
};

struct ClipboardGuard {
    bool open = false;
    ~ClipboardGuard() {
        if (open) {
            CloseClipboard();
        }
    }
};

struct GlobalLockGuard {
    HGLOBAL handle = nullptr;
    const void* ptr = nullptr;
    ~GlobalLockGuard() {
        if (handle != nullptr && ptr != nullptr) {
            GlobalUnlock(handle);
        }
    }
};

bool contains_non_whitespace(std::u16string_view text) {
    for (char16_t ch : text) {
        if (!std::iswspace(static_cast<wint_t>(ch))) {
            return true;
        }
    }
    return false;
}

}  // namespace

WindowsTrayShell::~WindowsTrayShell() { destroy(); }

bool WindowsTrayShell::create(HINSTANCE instance, TrayCallbacks callbacks) {
    if (hwnd_ != nullptr) {
        return true;
    }

    instance_ = instance != nullptr ? instance : GetModuleHandleW(nullptr);
    callbacks_ = std::move(callbacks);
    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &WindowsTrayShell::wnd_proc;
    wc.hInstance = instance_;
    wc.lpszClassName = kTrayClassName;
    if (RegisterClassExW(&wc) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    hwnd_ = CreateWindowExW(0, kTrayClassName, L"Dyslexia Reader Tray Host",
                            WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr,
                            instance_, this);
    if (hwnd_ == nullptr) {
        return false;
    }

    add_tray_icon();
    register_hotkeys();
    return true;
}

void WindowsTrayShell::destroy() noexcept {
    if (hwnd_ == nullptr) {
        return;
    }
    unregister_hotkeys();
    remove_tray_icon();
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

void WindowsTrayShell::register_hotkeys() {
    hotkeyWarnings_.clear();
    registeredHotkeyIds_.clear();

    const std::array<HotkeySpec, 3> specs = {{
        {kHotkeyCaptureId, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'R',
         "Ctrl+Shift+R (Capture Screen)"},
        {kHotkeyToggleVisibilityId, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'D',
         "Ctrl+Shift+D (Show/Hide Reader)"},
        {kHotkeyReadClipboardId, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'V',
         "Ctrl+Shift+V (Read Clipboard)"},
    }};

    for (const auto& spec : specs) {
        if (RegisterHotKey(hwnd_, spec.id, spec.modifiers, spec.vk)) {
            registeredHotkeyIds_.push_back(spec.id);
        } else {
            hotkeyWarnings_.push_back(
                std::string("Global hotkey unavailable (already in use): ") +
                spec.label);
        }
    }
}

void WindowsTrayShell::unregister_hotkeys() noexcept {
    if (hwnd_ == nullptr) {
        return;
    }
    for (int id : registeredHotkeyIds_) {
        UnregisterHotKey(hwnd_, id);
    }
    registeredHotkeyIds_.clear();
}

void WindowsTrayShell::add_tray_icon() const {
    if (hwnd_ == nullptr) {
        return;
    }
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kTrayCallbackMsg;
    nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcsncpy_s(nid.szTip,
              L"Dyslexia Reader (Ctrl+Shift+R: Capture, Ctrl+Shift+V: Clipboard)",
              _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void WindowsTrayShell::remove_tray_icon() const noexcept {
    if (hwnd_ == nullptr) {
        return;
    }
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void WindowsTrayShell::show_notification(const std::wstring& title,
                                           const std::wstring& message,
                                           bool isWarning) const {
    if (hwnd_ == nullptr) {
        return;
    }
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = isWarning ? NIIF_WARNING : NIIF_INFO;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, message.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void WindowsTrayShell::show_context_menu() const {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    const bool visible =
        callbacks_.isReaderVisible ? callbacks_.isReaderVisible() : true;
    const ReaderMode mode =
        callbacks_.currentMode ? callbacks_.currentMode() : ReaderMode::Floating;

    AppendMenuW(menu, MF_STRING, kCmdCapture,
                L"Capture Screen\tCtrl+Shift+R");
    AppendMenuW(menu, MF_STRING, kCmdReadClipboard,
                L"Read Clipboard\tCtrl+Shift+V");
    AppendMenuW(menu, MF_STRING, kCmdToggleVisibility,
                visible ? L"Hide Reader\tCtrl+Shift+D"
                        : L"Show Reader\tCtrl+Shift+D");
    AppendMenuW(menu, MF_STRING, kCmdToggleMode,
                mode == ReaderMode::Focus ? L"Switch to Floating Mode"
                                          : L"Switch to Focus Mode");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdImportShareCode,
                L"Import Profile by Share Code...");
    AppendMenuW(menu, MF_STRING, kCmdShareProfile,
                L"Share Profile to Platform");
    AppendMenuW(menu, MF_STRING, kCmdOpenWebPlayground,
                L"Open Web Playground");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdExit, L"Exit");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(hwnd_);
    const UINT cmd = TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, cursor.x,
        cursor.y, 0, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);

    switch (cmd) {
    case kCmdCapture:
        if (callbacks_.onCaptureRequested) {
            callbacks_.onCaptureRequested();
        }
        break;
    case kCmdReadClipboard:
        if (callbacks_.onClipboardRequested) {
            callbacks_.onClipboardRequested();
        }
        break;
    case kCmdToggleVisibility:
        if (callbacks_.onToggleVisibilityRequested) {
            callbacks_.onToggleVisibilityRequested();
        }
        break;
    case kCmdToggleMode:
        if (callbacks_.onToggleModeRequested) {
            callbacks_.onToggleModeRequested();
        }
        break;
    case kCmdImportShareCode:
        if (callbacks_.onImportShareCodeRequested) {
            callbacks_.onImportShareCodeRequested();
        }
        break;
    case kCmdShareProfile:
        if (callbacks_.onShareProfileRequested) {
            callbacks_.onShareProfileRequested();
        }
        break;
    case kCmdOpenWebPlayground:
        if (callbacks_.onOpenWebPlaygroundRequested) {
            callbacks_.onOpenWebPlaygroundRequested();
        }
        break;
    case kCmdExit:
        if (callbacks_.onExitRequested) {
            callbacks_.onExitRequested();
        }
        break;
    default:
        break;
    }
}

LRESULT CALLBACK WindowsTrayShell::wnd_proc(HWND hwnd, UINT msg, WPARAM wParam,
                                            LPARAM lParam) {
    auto* self = reinterpret_cast<WindowsTrayShell*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<WindowsTrayShell*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
        if (self != nullptr) {
            self->hwnd_ = hwnd;
        }
    }
    if (self != nullptr) {
        const LRESULT result = self->handle_message(msg, wParam, lParam);
        if (msg == WM_NCDESTROY) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            self->hwnd_ = nullptr;
        }
        return result;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT WindowsTrayShell::handle_message(UINT msg, WPARAM wParam,
                                         LPARAM lParam) {
    if (taskbarCreatedMsg_ != 0 && msg == taskbarCreatedMsg_) {
        add_tray_icon();
        return 0;
    }

    switch (msg) {
    case WM_HOTKEY:
        switch (static_cast<int>(wParam)) {
        case kHotkeyCaptureId:
            if (callbacks_.onCaptureRequested) {
                callbacks_.onCaptureRequested();
            }
            return 0;
        case kHotkeyToggleVisibilityId:
            if (callbacks_.onToggleVisibilityRequested) {
                callbacks_.onToggleVisibilityRequested();
            }
            return 0;
        case kHotkeyReadClipboardId:
            if (callbacks_.onClipboardRequested) {
                callbacks_.onClipboardRequested();
            }
            return 0;
        default:
            break;
        }
        break;
    case kTrayCallbackMsg:
        switch (LOWORD(lParam)) {
        case WM_LBUTTONDBLCLK:
            if (callbacks_.onToggleVisibilityRequested) {
                callbacks_.onToggleVisibilityRequested();
            }
            return 0;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            show_context_menu();
            return 0;
        default:
            break;
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

std::optional<std::u16string> WindowsTrayShell::read_clipboard_utf16(
    HWND owner) {
    ClipboardGuard clipboard;
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (OpenClipboard(owner)) {
            clipboard.open = true;
            break;
        }
        Sleep(10);
    }
    if (!clipboard.open) {
        return std::nullopt;
    }

    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        return std::nullopt;
    }

    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (handle == nullptr) {
        return std::nullopt;
    }

    GlobalLockGuard lock;
    lock.handle = static_cast<HGLOBAL>(handle);
    lock.ptr = GlobalLock(lock.handle);
    if (lock.ptr == nullptr) {
        return std::nullopt;
    }

    static_assert(sizeof(wchar_t) == sizeof(char16_t),
                  "Windows wchar_t must be 16-bit UTF-16");
    std::u16string text(reinterpret_cast<const char16_t*>(lock.ptr));
    if (!contains_non_whitespace(text)) {
        return std::nullopt;
    }
    return text;
}

}  // namespace dyslexia_reader::windows
