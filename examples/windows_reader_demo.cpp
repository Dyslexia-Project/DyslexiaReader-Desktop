#include "dyslexia_reader/paragraph_rebuilder.hpp"
#include "dyslexia_reader/tesseract_ocr_engine.hpp"
#include "dyslexia_reader/windows/direct2d_text_renderer.hpp"
#include "dyslexia_reader/windows/font_resource_loader.hpp"
#include "dyslexia_reader/windows/platform_profile_client.hpp"
#include "dyslexia_reader/windows/profile_store.hpp"
#include "dyslexia_reader/windows/screen_capturer.hpp"
#include "dyslexia_reader/windows/speech_synthesizer.hpp"
#include "dyslexia_reader/windows/tray_shell.hpp"
#include "dyslexia_reader/windows/word_insights.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <winrt/base.h>

#if defined(_MSC_VER)
#pragma comment(                                                               \
    linker,                                                                    \
    "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace {

using namespace dyslexia_reader;
using namespace dyslexia_reader::windows;

constexpr wchar_t kCoordinatorClassName[] = L"DyslexiaReader.CoordinatorWindow";
constexpr UINT kMsgStartCapture = WM_APP + 0x51;
constexpr UINT kMsgOcrCompleted = WM_APP + 0x52;
constexpr UINT kMsgPlatformCompleted = WM_APP + 0x53;

std::wstring wide_from_utf8_lossy(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (size <= 0) {
        return L"Operation failed.";
    }
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        wide.data(), size);
    return wide;
}

ReadingDocument build_welcome_document() {
    return build_reading_document_from_utf16(
        u"Welcome to Dyslexia Reader Desktop.\n\n"
        u"Quick Actions:\n"
        u"Click Capture Screen in the toolbar or press Ctrl+Shift+R anywhere on "
        u"Windows to select text from your screen with offline Tesseract OCR.\n\n"
        u"Click Paste Text or press Ctrl+Shift+V (or use Tray -> Read Clipboard) "
        u"to load plain text copied to your clipboard.\n\n"
        u"Press Ctrl+Shift+D to show or hide this reader window. Closing the "
        u"window keeps it running in the system tray; use Tray -> Exit to quit.\n\n"
        u"Reading Comfort & Tools:\n"
        u"Adjust font family (Lexend, Atkinson Hyperlegible, OpenDyslexic), font "
        u"size, theme (Warm cream, Sepia, Dark high contrast), letter spacing, "
        u"line height, line width, Bionic text, Reading ruler (Alt+Up / Alt+Down), "
        u"and Focus Mode (Esc exits fullscreen). Changes save automatically.\n\n"
        u"Use Voice, Play, Speak Word, Stop, and Speed to listen offline with "
        u"synchronized word and sentence highlighting. Click any word to place "
        u"the cursor, move between words with the Arrow keys, double-click or "
        u"press Enter to pronounce just that word, or click Play to read from "
        u"the cursor (Esc clears the cursor).");
}

struct ApartmentGuard {
    ApartmentGuard() {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
    }
    ~ApartmentGuard() { winrt::uninit_apartment(); }
    ApartmentGuard(const ApartmentGuard&) = delete;
    ApartmentGuard& operator=(const ApartmentGuard&) = delete;
};

struct OcrJobResult {
    std::uint64_t jobId = 0;
    bool succeeded = false;
    ReadingDocument document;
    std::string errorMessage;
};

std::string utf8_from_wide(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
        nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                        static_cast<int>(wide.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

bool copy_utf8_to_clipboard(HWND owner, const std::string& utf8) {
    const std::wstring wide = wide_from_utf8_lossy(utf8);
    if (!OpenClipboard(owner)) {
        return false;
    }
    struct ClipboardCloser {
        ~ClipboardCloser() { CloseClipboard(); }
    } closer;
    if (!EmptyClipboard()) {
        return false;
    }
    const std::size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem == nullptr) {
        return false;
    }
    void* dst = GlobalLock(mem);
    if (dst == nullptr) {
        GlobalFree(mem);
        return false;
    }
    std::memcpy(dst, wide.c_str(), bytes);
    GlobalUnlock(mem);
    if (SetClipboardData(CF_UNICODETEXT, mem) == nullptr) {
        GlobalFree(mem);
        return false;
    }
    return true;
}

std::optional<std::string> try_clipboard_share_code(HWND owner) {
    const auto clipText = WindowsTrayShell::read_clipboard_utf16(owner);
    if (!clipText.has_value() || clipText->size() > 24) {
        return std::nullopt;
    }
    std::string utf8;
    utf8.reserve(clipText->size());
    for (char16_t ch : *clipText) {
        if (ch > 0x7F) {
            return std::nullopt;
        }
        utf8.push_back(static_cast<char>(ch));
    }
    try {
        std::string normalized =
            WindowsPlatformProfileClient::normalize_share_code(utf8);
        if (normalized.size() >= 6 && normalized.size() <= 12) {
            return normalized;
        }
    } catch (const ReaderError&) {
        return std::nullopt;
    }
    return std::nullopt;
}

struct ShareCodeDialogState {
    HWND editHwnd = nullptr;
    bool accepted = false;
    bool done = false;
    std::string code;
};

LRESULT CALLBACK share_code_dialog_proc(HWND hwnd, UINT msg, WPARAM wParam,
                                         LPARAM lParam) {
    auto* state = reinterpret_cast<ShareCodeDialogState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE: {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return TRUE;
    }
    case WM_COMMAND:
        if (state != nullptr) {
            const int id = LOWORD(wParam);
            if (id == IDOK) {
                wchar_t buf[64]{};
                if (state->editHwnd != nullptr) {
                    GetWindowTextW(state->editHwnd, buf,
                                   static_cast<int>(std::size(buf)));
                }
                state->code = utf8_from_wide(buf);
                state->accepted = true;
                state->done = true;
                return 0;
            }
            if (id == IDCANCEL) {
                state->accepted = false;
                state->done = true;
                return 0;
            }
        }
        break;
    case WM_CLOSE:
        if (state != nullptr) {
            state->accepted = false;
            state->done = true;
            return 0;
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

std::optional<std::string> prompt_share_code_dialog(
    HWND parent, const std::string& initialCode) {
    constexpr wchar_t kPromptClassName[] = L"DyslexiaReader.ShareCodePrompt";
    HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &share_code_dialog_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kPromptClassName;
    if (RegisterClassExW(&wc) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return std::nullopt;
    }

    constexpr int kDlgW = 390;
    constexpr int kDlgH = 165;
    int x = CW_USEDEFAULT;
    int y = CW_USEDEFAULT;
    RECT parentRect{};
    if (parent != nullptr && GetWindowRect(parent, &parentRect)) {
        x = parentRect.left + ((parentRect.right - parentRect.left - kDlgW) / 2);
        y = parentRect.top + ((parentRect.bottom - parentRect.top - kDlgH) / 2);
    }

    ShareCodeDialogState state;
    HWND dlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME, kPromptClassName, L"Import Profile by Share Code",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, kDlgW, kDlgH, parent, nullptr,
        instance, &state);
    if (dlg == nullptr) {
        return std::nullopt;
    }

    auto guiFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    auto applyFont = [guiFont](HWND child) {
        if (child != nullptr && guiFont != nullptr) {
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(guiFont),
                         FALSE);
        }
    };

    HWND label = CreateWindowExW(
        0, L"STATIC",
        L"Enter the Platform share code from the Web Playground:",
        WS_CHILD | WS_VISIBLE, 16, 16, 344, 20, dlg, nullptr, instance, nullptr);
    applyFont(label);

    const std::wstring initialWide = wide_from_utf8_lossy(initialCode);
    state.editHwnd = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", initialWide.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_UPPERCASE,
        16, 42, 342, 26, dlg, reinterpret_cast<HMENU>(101), instance, nullptr);
    applyFont(state.editHwnd);

    HWND okBtn = CreateWindowExW(
        0, L"BUTTON", L"Import",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 186, 84, 82, 28,
        dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), instance,
        nullptr);
    applyFont(okBtn);

    HWND cancelBtn = CreateWindowExW(
        0, L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 276, 84, 82, 28,
        dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), instance,
        nullptr);
    applyFont(cancelBtn);

    const bool parentWasEnabled =
        parent != nullptr && IsWindowEnabled(parent) != FALSE;
    if (parentWasEnabled) {
        EnableWindow(parent, FALSE);
    }

    ShowWindow(dlg, SW_SHOWNORMAL);
    UpdateWindow(dlg);
    if (state.editHwnd != nullptr) {
        SetFocus(state.editHwnd);
        SendMessageW(state.editHwnd, EM_SETSEL, 0, -1);
    }

    MSG msg{};
    bool rePostQuit = false;
    WPARAM quitCode = 0;
    while (!state.done) {
        const BOOL res = GetMessageW(&msg, nullptr, 0, 0);
        if (res <= 0) {
            if (res == 0) {
                rePostQuit = true;
                quitCode = msg.wParam;
            }
            break;
        }
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (parentWasEnabled) {
        EnableWindow(parent, TRUE);
    }
    DestroyWindow(dlg);
    if (parent != nullptr) {
        SetForegroundWindow(parent);
    }
    if (rePostQuit) {
        PostQuitMessage(static_cast<int>(quitCode));
    }
    if (!state.accepted) {
        return std::nullopt;
    }
    return state.code;
}

class AppCoordinator final {
public:
    AppCoordinator(Direct2DTextRenderer& renderer,
                   WindowsScreenCapturer& capturer,
                   TesseractOcrEngine& ocrEngine,
                   ParagraphRebuilder& rebuilder,
                   WindowsTrayShell& tray,
                   JsonReadingProfileStore& profileStore)
        : renderer_(renderer),
          capturer_(capturer),
          ocrEngine_(ocrEngine),
          rebuilder_(rebuilder),
          tray_(tray),
          profileStore_(profileStore) {}

    ~AppCoordinator() { shutdown(); }

    AppCoordinator(const AppCoordinator&) = delete;
    AppCoordinator& operator=(const AppCoordinator&) = delete;

    bool create(HINSTANCE instance) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &AppCoordinator::wnd_proc;
        wc.hInstance = instance;
        wc.lpszClassName = kCoordinatorClassName;
        if (RegisterClassExW(&wc) == 0 &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
        hwnd_ = CreateWindowExW(0, kCoordinatorClassName,
                                L"Dyslexia Reader Coordinator", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, instance, this);
        return hwnd_ != nullptr;
    }

    void request_capture() const {
        if (hwnd_ != nullptr) {
            PostMessageW(hwnd_, kMsgStartCapture, 0, 0);
        }
    }

    void read_clipboard() {
        const auto text =
            WindowsTrayShell::read_clipboard_utf16(renderer_.hwnd());
        if (!text.has_value()) {
            tray_.show_notification(
                L"Dyslexia Reader",
                L"Clipboard does not contain readable plain text.", true);
            return;
        }
        renderer_.set_document(build_reading_document_from_utf16(*text));
        show_and_focus_reader();
    }

    void toggle_visibility() {
        if (renderer_.is_visible()) {
            renderer_.show(SW_HIDE);
        } else {
            show_and_focus_reader();
        }
    }

    void toggle_mode() {
        const ReaderMode next = renderer_.mode() == ReaderMode::Focus
                                    ? ReaderMode::Floating
                                    : ReaderMode::Focus;
        renderer_.set_mode(next);
        show_and_focus_reader();
    }

    void open_word_cloud() {
        const auto items = build_word_cloud(renderer_.document(), 45);
        wordCloudWindow_.show(
            renderer_.hwnd(), renderer_.profile().theme, items,
            [this](std::size_t wordIndex, const std::string& word) {
                renderer_.select_word_at_index(wordIndex);
                show_and_focus_reader();
                define_word_text(word);
            });
    }

    void define_selected_word() {
        const auto selected = renderer_.selected_word_utf8();
        if (!selected.has_value()) {
            tray_.show_notification(
                L"Dyslexia Reader",
                L"Click a word (or right-click any word) to look it up.",
                false);
            return;
        }
        define_word_text(*selected);
    }

    void define_word_text(const std::string& word) {
        wordDefWindow_.show_word(
            renderer_.hwnd(), renderer_.profile().theme, word,
            [this](const std::string& spoken) {
                renderer_.speak_text_utf8(spoken);
            });
    }

    void sync_insight_themes(ThemeId theme) {
        wordCloudWindow_.update_theme(theme);
        wordDefWindow_.update_theme(theme);
    }

    void open_web_playground() {
        const std::wstring url =
            WindowsPlatformProfileClient::default_web_playground_url();
        const HINSTANCE result = ShellExecuteW(
            renderer_.hwnd(), L"open", url.c_str(), nullptr, nullptr,
            SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32) {
            tray_.show_notification(
                L"Dyslexia Reader",
                L"Could not open the Dyslexia Web Playground in your browser.",
                true);
        }
    }

    void share_profile_to_platform() {
        if (platformBusy_.exchange(true)) {
            tray_.show_notification(
                L"Dyslexia Reader",
                L"Platform sync is already in progress.", false);
            return;
        }
        if (platformThread_.joinable()) {
            platformThread_.join();
        }
        const ReadingProfile snapshot = renderer_.profile();
        tray_.show_notification(
            L"Dyslexia Reader",
            L"Sharing current reading profile to Platform...", false);

        platformThread_ = std::thread([this, snapshot]() {
            PlatformSyncResult result;
            result.kind = PlatformSyncResult::Kind::Share;
            try {
                WindowsPlatformProfileClient client;
                result.shareCode = client.share_profile(snapshot);
            } catch (const std::exception& error) {
                result.errorMessage = error.what();
            }
            {
                std::scoped_lock lock(platformMutex_);
                pendingPlatformResult_ = std::move(result);
            }
            if (!shuttingDown_.load() && hwnd_ != nullptr) {
                PostMessageW(hwnd_, kMsgPlatformCompleted, 0, 0);
            } else {
                platformBusy_.store(false);
            }
        });
    }

    void import_profile_by_share_code() {
        if (platformBusy_.load()) {
            tray_.show_notification(
                L"Dyslexia Reader",
                L"Platform sync is already in progress.", false);
            return;
        }
        const std::string initialCode =
            try_clipboard_share_code(renderer_.hwnd()).value_or("");
        const auto entered =
            prompt_share_code_dialog(renderer_.hwnd(), initialCode);
        if (!entered.has_value()) {
            return;
        }

        std::string normalizedCode;
        try {
            normalizedCode =
                WindowsPlatformProfileClient::normalize_share_code(*entered);
        } catch (const std::exception& error) {
            MessageBoxW(renderer_.hwnd(),
                        wide_from_utf8_lossy(error.what()).c_str(),
                        L"Invalid Share Code", MB_OK | MB_ICONWARNING);
            return;
        }

        if (platformBusy_.exchange(true)) {
            return;
        }
        if (platformThread_.joinable()) {
            platformThread_.join();
        }
        tray_.show_notification(
            L"Dyslexia Reader",
            L"Downloading reading profile from Platform...", false);

        platformThread_ = std::thread([this, normalizedCode]() {
            PlatformSyncResult result;
            result.kind = PlatformSyncResult::Kind::Import;
            result.shareCode = normalizedCode;
            try {
                WindowsPlatformProfileClient client;
                result.importedProfile =
                    client.fetch_shared_profile(normalizedCode);
            } catch (const std::exception& error) {
                result.errorMessage = error.what();
            }
            {
                std::scoped_lock lock(platformMutex_);
                pendingPlatformResult_ = std::move(result);
            }
            if (!shuttingDown_.load() && hwnd_ != nullptr) {
                PostMessageW(hwnd_, kMsgPlatformCompleted, 0, 0);
            } else {
                platformBusy_.store(false);
            }
        });
    }

    void show_and_focus_reader() {
        renderer_.show(SW_SHOW);
        if (renderer_.hwnd() != nullptr) {
            SetForegroundWindow(renderer_.hwnd());
        }
    }

    void shutdown() noexcept {
        shuttingDown_.store(true);
        wordDefWindow_.close();
        wordCloudWindow_.close();
        if (ocrThread_.joinable()) {
            ocrThread_.join();
        }
        if (platformThread_.joinable()) {
            platformThread_.join();
        }
        if (hwnd_ != nullptr) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
    }

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wParam,
                                     LPARAM lParam) {
        auto* self = reinterpret_cast<AppCoordinator*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<AppCoordinator*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(self));
            if (self != nullptr) {
                self->hwnd_ = hwnd;
            }
        }
        if (self != nullptr) {
            const LRESULT res = self->handle_message(msg, wParam, lParam);
            if (msg == WM_NCDESTROY) {
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                self->hwnd_ = nullptr;
            }
            return res;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    LRESULT handle_message(UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case kMsgStartCapture:
            start_capture_workflow();
            return 0;
        case kMsgOcrCompleted:
            finish_ocr_workflow(static_cast<std::uint64_t>(wParam));
            return 0;
        case kMsgPlatformCompleted:
            finish_platform_workflow();
            return 0;
        default:
            return DefWindowProcW(hwnd_, msg, wParam, lParam);
        }
    }

    void start_capture_workflow() {
        if (shuttingDown_.load() || captureActive_ || ocrBusy_.load()) {
            if (ocrBusy_.load()) {
                tray_.show_notification(
                    L"Dyslexia Reader",
                    L"OCR is still processing your previous selection.", false);
            }
            return;
        }

        captureActive_ = true;
        const bool wasVisible = renderer_.is_visible();
        if (wasVisible) {
            renderer_.show(SW_HIDE);
            Sleep(80);
        }

        std::optional<CapturedImage> captured;
        std::string captureError;
        try {
            captured = capturer_.capture_selection();
        } catch (const std::exception& error) {
            captureError = error.what();
        }
        captureActive_ = false;

        if (!captureError.empty()) {
            if (wasVisible) {
                show_and_focus_reader();
            }
            tray_.show_notification(L"Capture Error",
                                    wide_from_utf8_lossy(captureError), true);
            return;
        }

        if (!captured.has_value()) {
            if (wasVisible) {
                show_and_focus_reader();
            }
            return;
        }

        if (ocrThread_.joinable()) {
            ocrThread_.join();
        }

        renderer_.set_document(build_reading_document_from_utf16(
            u"Recognizing captured text with Tesseract OCR...\n\n"
            u"Your formatted text will appear in a moment."));
        show_and_focus_reader();

        const std::uint64_t jobId = ++latestJobId_;
        ocrBusy_.store(true);
        CapturedImage imageCopy = std::move(*captured);

        ocrThread_ = std::thread([this, jobId,
                                  img = std::move(imageCopy)]() mutable {
            OcrJobResult result;
            result.jobId = jobId;
            try {
                const OcrResult ocr = ocrEngine_.recognize(img);
                result.document = rebuilder_.rebuild(ocr);
                result.succeeded = true;
            } catch (const std::exception& error) {
                result.succeeded = false;
                result.errorMessage = error.what();
            }

            {
                std::lock_guard<std::mutex> lock(resultMutex_);
                pendingResult_ = std::move(result);
            }
            if (!shuttingDown_.load() && hwnd_ != nullptr) {
                PostMessageW(hwnd_, kMsgOcrCompleted,
                             static_cast<WPARAM>(jobId), 0);
            }
        });
    }

    void finish_ocr_workflow(std::uint64_t jobId) {
        if (shuttingDown_.load()) {
            return;
        }
        std::optional<OcrJobResult> result;
        {
            std::lock_guard<std::mutex> lock(resultMutex_);
            if (pendingResult_.has_value() && pendingResult_->jobId == jobId) {
                result = std::move(pendingResult_);
                pendingResult_.reset();
            }
        }
        ocrBusy_.store(false);
        if (!result.has_value() || jobId != latestJobId_) {
            return;
        }

        if (!result->succeeded) {
            tray_.show_notification(
                L"OCR Failed", wide_from_utf8_lossy(result->errorMessage), true);
            renderer_.set_document(build_reading_document_from_utf8(
                "Could not extract text from the selected area.\n\nDetails: " +
                result->errorMessage));
            show_and_focus_reader();
            return;
        }

        if (result->document.text.empty()) {
            tray_.show_notification(
                L"No Text Detected",
                L"No readable text was found in that screen region.", true);
            renderer_.set_document(build_reading_document_from_utf16(
                u"No readable text was detected in the captured region.\n\n"
                u"Try capturing a slightly larger region containing clear text."));
            show_and_focus_reader();
            return;
        }

        renderer_.set_document(result->document);
        show_and_focus_reader();
    }

    void finish_platform_workflow() {
        std::optional<PlatformSyncResult> result;
        {
            std::scoped_lock lock(platformMutex_);
            result = std::move(pendingPlatformResult_);
            pendingPlatformResult_.reset();
        }
        platformBusy_.store(false);
        if (!result.has_value()) {
            return;
        }
        if (!result->errorMessage.empty()) {
            const std::wstring details = wide_from_utf8_lossy(result->errorMessage);
            tray_.show_notification(L"Platform Sync Unavailable", details, true);
            MessageBoxW(renderer_.hwnd(), details.c_str(),
                        L"Platform Sync Unavailable", MB_OK | MB_ICONWARNING);
            return;
        }

        if (result->kind == PlatformSyncResult::Kind::Share) {
            const bool copied =
                copy_utf8_to_clipboard(renderer_.hwnd(), result->shareCode);
            const std::wstring wideCode = wide_from_utf8_lossy(result->shareCode);
            const std::wstring msg =
                L"Share Code: " + wideCode +
                (copied ? L"\n\nCopied to clipboard. Use \"Load Code\" in the "
                          L"Dyslexia Web Playground to open this profile."
                        : L"\n\nUse \"Load Code\" in the Dyslexia Web "
                          L"Playground to open this profile.");
            tray_.show_notification(
                L"Profile Shared",
                L"Share code " + wideCode +
                    (copied ? L" copied to clipboard." : L" generated."),
                false);
            MessageBoxW(renderer_.hwnd(), msg.c_str(),
                        L"Profile Shared to Platform",
                        MB_OK | MB_ICONINFORMATION);
            return;
        }

        if (result->importedProfile.has_value()) {
            renderer_.set_profile(*result->importedProfile);
            try {
                profileStore_.save(*result->importedProfile);
            } catch (const std::exception& error) {
                OutputDebugStringA("Profile save warning: ");
                OutputDebugStringA(error.what());
                OutputDebugStringA("\n");
            }
            show_and_focus_reader();
            tray_.show_notification(
                L"Profile Imported",
                L"Applied Platform share code " +
                    wide_from_utf8_lossy(result->shareCode) + L".",
                false);
        }
    }

    struct PlatformSyncResult {
        enum class Kind { Import, Share };
        Kind kind = Kind::Import;
        std::string shareCode;
        std::optional<ReadingProfile> importedProfile;
        std::string errorMessage;
    };

    Direct2DTextRenderer& renderer_;
    WindowsScreenCapturer& capturer_;
    TesseractOcrEngine& ocrEngine_;
    ParagraphRebuilder& rebuilder_;
    WindowsTrayShell& tray_;
    JsonReadingProfileStore& profileStore_;

    HWND hwnd_ = nullptr;
    bool captureActive_ = false;
    std::atomic_bool ocrBusy_{false};
    std::atomic_bool shuttingDown_{false};
    std::uint64_t latestJobId_ = 0;
    std::mutex resultMutex_;
    std::optional<OcrJobResult> pendingResult_;
    std::thread ocrThread_;
    std::atomic_bool platformBusy_{false};
    std::mutex platformMutex_;
    std::optional<PlatformSyncResult> pendingPlatformResult_;
    std::thread platformThread_;
    std::shared_ptr<FastWordLookupClient> wordLookupClient_ =
        std::make_shared<FastWordLookupClient>();
    WordCloudWindow wordCloudWindow_;
    WordDefinitionWindow wordDefWindow_{wordLookupClient_};
};

bool has_minimized_flag(PWSTR cmdLine) {
    if (cmdLine == nullptr) {
        return false;
    }
    const std::wstring args(cmdLine);
    return args.find(L"--minimized") != std::wstring::npos ||
           args.find(L"--tray") != std::wstring::npos;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine,
                    int showCommand) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    try {
        ApartmentGuard apartment;

        FontResourceLoader fontLoader(instance);
        std::unique_ptr<WindowsSpeechEngine> speechEngine;
        try {
            speechEngine = std::make_unique<WindowsSpeechEngine>();
        } catch (const std::exception& error) {
            OutputDebugStringA("Speech engine initialization warning: ");
            OutputDebugStringA(error.what());
            OutputDebugStringA("\n");
        }

        JsonReadingProfileStore profileStore;
        const ReadingProfile initialProfile =
            profileStore.load().value_or(ReadingProfile{});
        const ReaderMode initialMode = profileStore.load_reader_mode();

        TesseractOcrEngine ocrEngine("en-US");
        ParagraphRebuilder rebuilder;
        WindowsScreenCapturer capturer;
        Direct2DTextRenderer renderer(fontLoader, speechEngine.get());
        WindowsTrayShell tray;
        AppCoordinator coordinator(renderer, capturer, ocrEngine, rebuilder,
                                   tray, profileStore);

        if (!coordinator.create(instance)) {
            MessageBoxW(nullptr,
                        L"Could not initialize the Dyslexia Reader message coordinator.",
                        L"Dyslexia Reader", MB_OK | MB_ICONERROR);
            return 1;
        }

        renderer.set_hide_on_close(true);
        renderer.set_profile(initialProfile);
        renderer.set_mode(initialMode);
        renderer.set_document(build_welcome_document());

        renderer.set_on_profile_changed(
            [&profileStore, &coordinator](const ReadingProfile& updated) {
                coordinator.sync_insight_themes(updated.theme);
                try {
                    profileStore.save(updated);
                } catch (const std::exception& error) {
                    OutputDebugStringA("Profile save warning: ");
                    OutputDebugStringA(error.what());
                    OutputDebugStringA("\n");
                }
            });
        renderer.set_on_mode_changed([&profileStore](ReaderMode mode) {
            try {
                profileStore.save_reader_mode(mode);
            } catch (const std::exception& error) {
                OutputDebugStringA("Mode save warning: ");
                OutputDebugStringA(error.what());
                OutputDebugStringA("\n");
            }
        });
        renderer.set_on_capture_requested(
            [&coordinator]() { coordinator.request_capture(); });
        renderer.set_on_clipboard_requested(
            [&coordinator]() { coordinator.read_clipboard(); });
        renderer.set_on_import_share_code_requested(
            [&coordinator]() { coordinator.import_profile_by_share_code(); });
        renderer.set_on_share_profile_requested(
            [&coordinator]() { coordinator.share_profile_to_platform(); });
        renderer.set_on_open_web_playground_requested(
            [&coordinator]() { coordinator.open_web_playground(); });
        renderer.set_on_define_word_requested(
            [&coordinator]() { coordinator.define_selected_word(); });
        renderer.set_on_word_cloud_requested(
            [&coordinator]() { coordinator.open_word_cloud(); });

        renderer.create_reader_window(instance);

        TrayCallbacks trayCallbacks;
        trayCallbacks.onCaptureRequested = [&coordinator]() {
            coordinator.request_capture();
        };
        trayCallbacks.onClipboardRequested = [&coordinator]() {
            coordinator.read_clipboard();
        };
        trayCallbacks.onToggleVisibilityRequested = [&coordinator]() {
            coordinator.toggle_visibility();
        };
        trayCallbacks.onToggleModeRequested = [&coordinator]() {
            coordinator.toggle_mode();
        };
        trayCallbacks.onImportShareCodeRequested = [&coordinator]() {
            coordinator.import_profile_by_share_code();
        };
        trayCallbacks.onShareProfileRequested = [&coordinator]() {
            coordinator.share_profile_to_platform();
        };
        trayCallbacks.onOpenWebPlaygroundRequested = [&coordinator]() {
            coordinator.open_web_playground();
        };
        trayCallbacks.onExitRequested = []() { PostQuitMessage(0); };
        trayCallbacks.isReaderVisible = [&renderer]() {
            return renderer.is_visible();
        };
        trayCallbacks.currentMode = [&renderer]() { return renderer.mode(); };

        if (!tray.create(instance, std::move(trayCallbacks))) {
            OutputDebugStringA(
                "Warning: System tray icon or global hotkeys could not be created.\n");
        }

        if (!tray.hotkey_warnings().empty()) {
            std::string combined;
            for (const auto& warning : tray.hotkey_warnings()) {
                OutputDebugStringA((warning + "\n").c_str());
                if (!combined.empty()) {
                    combined += "\n";
                }
                combined += warning;
            }
            tray.show_notification(L"Hotkey Conflict",
                                   wide_from_utf8_lossy(combined), true);
        }

        if (!has_minimized_flag(cmdLine)) {
            renderer.show(showCommand == SW_HIDE ? SW_SHOW : showCommand);
        }

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        coordinator.shutdown();
        tray.destroy();
        if (renderer.hwnd() != nullptr && IsWindow(renderer.hwnd())) {
            renderer.set_hide_on_close(false);
            DestroyWindow(renderer.hwnd());
        }
        return static_cast<int>(msg.wParam);
    } catch (const std::exception& error) {
        MessageBoxA(nullptr, error.what(), "Dyslexia Reader Error",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
}
