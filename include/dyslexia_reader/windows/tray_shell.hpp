#pragma once

#include "dyslexia_reader/domain.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace dyslexia_reader::windows {

struct TrayCallbacks {
    std::function<void()> onCaptureRequested;
    std::function<void()> onClipboardRequested;
    std::function<void()> onToggleVisibilityRequested;
    std::function<void()> onToggleModeRequested;
    std::function<void()> onImportShareCodeRequested;
    std::function<void()> onShareProfileRequested;
    std::function<void()> onOpenWebPlaygroundRequested;
    std::function<void()> onExitRequested;
    std::function<bool()> isReaderVisible;
    std::function<ReaderMode()> currentMode;
};

class WindowsTrayShell final {
public:
    WindowsTrayShell() = default;
    ~WindowsTrayShell();

    WindowsTrayShell(const WindowsTrayShell&) = delete;
    WindowsTrayShell& operator=(const WindowsTrayShell&) = delete;

    bool create(HINSTANCE instance, TrayCallbacks callbacks);
    void destroy() noexcept;

    void show_notification(const std::wstring& title,
                           const std::wstring& message,
                           bool isWarning = false) const;

    const std::vector<std::string>& hotkey_warnings() const noexcept {
        return hotkeyWarnings_;
    }

    HWND host_hwnd() const noexcept { return hwnd_; }

    static std::optional<std::u16string> read_clipboard_utf16(
        HWND owner = nullptr);

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wParam,
                                     LPARAM lParam);
    LRESULT handle_message(UINT msg, WPARAM wParam, LPARAM lParam);

    void register_hotkeys();
    void unregister_hotkeys() noexcept;
    void add_tray_icon() const;
    void remove_tray_icon() const noexcept;
    void show_context_menu() const;

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    TrayCallbacks callbacks_;
    UINT taskbarCreatedMsg_ = 0;
    std::vector<int> registeredHotkeyIds_;
    std::vector<std::string> hotkeyWarnings_;
};

}  // namespace dyslexia_reader::windows
