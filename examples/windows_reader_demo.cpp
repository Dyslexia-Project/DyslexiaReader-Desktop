#include "dyslexia_reader/paragraph_rebuilder.hpp"
#include "dyslexia_reader/tesseract_ocr_engine.hpp"
#include "dyslexia_reader/windows/direct2d_text_renderer.hpp"
#include "dyslexia_reader/windows/font_resource_loader.hpp"
#include "dyslexia_reader/windows/profile_store.hpp"
#include "dyslexia_reader/windows/screen_capturer.hpp"
#include "dyslexia_reader/windows/speech_synthesizer.hpp"
#include "dyslexia_reader/windows/tray_shell.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winrt/base.h>

#if defined(_MSC_VER)
#pragma comment(                                                               \
    linker,                                                                    \
    "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#include <atomic>
#include <cstdint>
#include <exception>
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

class AppCoordinator final {
public:
    AppCoordinator(Direct2DTextRenderer& renderer,
                   WindowsScreenCapturer& capturer,
                   TesseractOcrEngine& ocrEngine,
                   ParagraphRebuilder& rebuilder,
                   WindowsTrayShell& tray)
        : renderer_(renderer),
          capturer_(capturer),
          ocrEngine_(ocrEngine),
          rebuilder_(rebuilder),
          tray_(tray) {}

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

    void show_and_focus_reader() {
        renderer_.show(SW_SHOW);
        if (renderer_.hwnd() != nullptr) {
            SetForegroundWindow(renderer_.hwnd());
        }
    }

    void shutdown() noexcept {
        shuttingDown_.store(true);
        if (ocrThread_.joinable()) {
            ocrThread_.join();
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

    Direct2DTextRenderer& renderer_;
    WindowsScreenCapturer& capturer_;
    TesseractOcrEngine& ocrEngine_;
    ParagraphRebuilder& rebuilder_;
    WindowsTrayShell& tray_;

    HWND hwnd_ = nullptr;
    bool captureActive_ = false;
    std::atomic_bool ocrBusy_{false};
    std::atomic_bool shuttingDown_{false};
    std::uint64_t latestJobId_ = 0;
    std::mutex resultMutex_;
    std::optional<OcrJobResult> pendingResult_;
    std::thread ocrThread_;
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
                                   tray);

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
            [&profileStore](const ReadingProfile& updated) {
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
