#include "dyslexia_reader/paragraph_rebuilder.hpp"
#include "dyslexia_reader/windows/direct2d_text_renderer.hpp"
#include "dyslexia_reader/windows/font_resource_loader.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#if defined(_MSC_VER)
#pragma comment(                                                               \
    linker,                                                                    \
    "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#include <cstdlib>
#include <iostream>
#include <optional>

namespace {

class RecordingSpeechEngine final : public dyslexia_reader::ISpeechEngine {
public:
    std::vector<dyslexia_reader::VoiceInfo> voices() const override {
        return {{"mock-en", "Mock Voice", "en-US"}};
    }
    dyslexia_reader::SpeechCapabilities capabilities() const override {
        return {true, true, true};
    }
    void start(const dyslexia_reader::SpeechRequest& request,
               dyslexia_reader::SpeechEventHandler handler) override {
        lastRequest = request;
        if (handler) {
            handler({dyslexia_reader::SpeechEventKind::Started, std::nullopt,
                     {}});
            handler({dyslexia_reader::SpeechEventKind::Completed, std::nullopt,
                     {}});
        }
    }
    void pause() override {}
    void resume() override {}
    void stop() override {}
    void set_rate(float) override {}

    std::optional<dyslexia_reader::SpeechRequest> lastRequest;
};

}  // namespace

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::cerr << "CHECK failed: " #cond " at " << __FILE__ << ":"   \
                      << __LINE__ << std::endl;                              \
            std::exit(1);                                                    \
        }                                                                    \
    } while (false)

int main() {
    using namespace dyslexia_reader;
    using namespace dyslexia_reader::windows;

    HINSTANCE instance = GetModuleHandleW(nullptr);
    FontResourceLoader fontLoader(instance);
    RecordingSpeechEngine mockSpeech;
    Direct2DTextRenderer renderer(fontLoader, &mockSpeech);

    ReadingProfile profile;
    profile.lineWidthCh = 45;
    profile.fontSizePt = 22.0F;
    renderer.set_profile(profile);
    renderer.set_hide_on_close(true);

    HWND hwnd = renderer.create_reader_window(instance);
    CHECK(hwnd != nullptr);
    CHECK(renderer.hwnd() == hwnd);
    CHECK(!renderer.is_visible());

    const ReadingDocument doc = build_reading_document_from_utf8(
        "This is a longer sample paragraph designed to verify line wrapping, "
        "layout snapshots, and vertical scroll clamping across Floating and "
        "Focus reading modes in the Direct2D renderer.\n\n"
        "A second paragraph ensures multiple lines exist so scrolling and line "
        "metrics behave consistently when resizing or changing column width.");
    renderer.set_document(doc);

    const auto floatingLayout = renderer.layout_snapshot();
    CHECK(!floatingLayout.lines.empty());
    CHECK(!floatingLayout.words.empty());

    // Simulate resize / scrolling bounds.
    renderer.set_viewport({{1600.0F, 400.0F}, 0.0F});
    const auto wideLayout = renderer.layout_snapshot();
    CHECK(!wideLayout.lines.empty());
    // Centered column should keep line X strictly > kTextPaddingDip (24.0F) at 1600 DIP width with 45ch!
    CHECK(wideLayout.lines[0].bounds.x > 30.0F);
    CHECK(wideLayout.lines[0].bounds.width < 1500.0F);

    renderer.set_on_clipboard_requested([]() {});
    profile.theme = ThemeId::DarkHighContrast;
    profile.lineWidthCh = 38;
    renderer.set_profile(profile);
    profile.theme = ThemeId::Sepia;
    renderer.set_profile(profile);
    profile.theme = ThemeId::WarmCream;
    renderer.set_profile(profile);

    // Switch Floating <-> Focus and back without destroying or throwing.
    bool sawFocus = false;
    renderer.set_on_mode_changed([&sawFocus](ReaderMode mode) {
        if (mode == ReaderMode::Focus) {
            sawFocus = true;
        }
    });
    renderer.set_mode(ReaderMode::Focus);
    CHECK(sawFocus);
    CHECK(renderer.mode() == ReaderMode::Focus);
    CHECK(!renderer.is_visible());  // Hidden before show() stays hidden.

    renderer.set_mode(ReaderMode::Floating);
    CHECK(renderer.mode() == ReaderMode::Floating);
    CHECK((GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_VSCROLL) == 0);

    HWND canvas = FindWindowExW(
        hwnd, nullptr, L"DyslexiaReader.Direct2DCanvasWindow", nullptr);
    CHECK(canvas != nullptr);
    CHECK((GetWindowLongPtrW(canvas, GWL_STYLE) & WS_VSCROLL) != 0);
    CHECK(wideLayout.lines[0].bounds.y >= 20.0F &&
          wideLayout.lines[0].bounds.y < 60.0F);

    const auto pumpMessages = []() {
        MSG msg{};
        int count = 0;
        while (count < 1000 && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            ++count;
        }
        return count;
    };

    // Exercise live rendering, minimize/restore, and ensure WM_CTLCOLOR* does not spin.
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    UpdateWindow(canvas);
    CHECK(pumpMessages() < 1000);

    RECT client{};
    GetClientRect(hwnd, &client);
    RECT canvasScreen{};
    GetWindowRect(canvas, &canvasScreen);
    POINT canvasTopLeft{canvasScreen.left, canvasScreen.top};
    ScreenToClient(hwnd, &canvasTopLeft);
    CHECK(canvasTopLeft.y > 80);  // Toolbar strip remains separate above canvas.

    ShowWindow(hwnd, SW_MINIMIZE);
    CHECK(pumpMessages() < 1000);
    ShowWindow(hwnd, SW_RESTORE);
    UpdateWindow(hwnd);
    UpdateWindow(canvas);
    CHECK(pumpMessages() < 1000);

    // Exercise clickable/arrow-navigable word cursor and single-word speech.
    const auto renderedLayout = renderer.layout_snapshot();
    CHECK(renderedLayout.words.size() >= 6);
    CHECK(renderedLayout.lines.size() >= 2);
    CHECK(!renderer.selected_word().has_value());

    const UINT dpi = GetDpiForWindow(hwnd);
    const float dpiScale = static_cast<float>(dpi == 0 ? 96 : dpi) / 96.0F;
    const auto& word0Frag = renderedLayout.words[0].fragments.front();
    const int clickX =
        static_cast<int>((word0Frag.x + word0Frag.width * 0.5F) * dpiScale);
    const int clickY =
        static_cast<int>((word0Frag.y + word0Frag.height * 0.5F) * dpiScale);
    SendMessageW(canvas, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(clickX, clickY));
    SendMessageW(canvas, WM_LBUTTONUP, 0, MAKELPARAM(clickX, clickY));
    CHECK(renderer.selected_word().has_value());
    CHECK(renderer.selected_word()->start == renderedLayout.words[0].text.start);

    SendMessageW(canvas, WM_KEYDOWN, VK_RIGHT, 0);
    CHECK(renderer.selected_word()->start == renderedLayout.words[1].text.start);
    SendMessageW(canvas, WM_KEYDOWN, VK_LEFT, 0);
    CHECK(renderer.selected_word()->start == renderedLayout.words[0].text.start);

    SendMessageW(canvas, WM_KEYDOWN, VK_DOWN, 0);
    const TextOffset line0End =
        renderedLayout.lines[0].text.start + renderedLayout.lines[0].text.length;
    CHECK(renderer.selected_word()->start >= line0End);
    SendMessageW(canvas, WM_KEYDOWN, VK_UP, 0);
    CHECK(renderer.selected_word()->start < line0End);

    // Enter pronounces only the selected word.
    SendMessageW(canvas, WM_KEYDOWN, VK_RIGHT, 0);
    const TextRange expectedWord = *renderer.selected_word();
    SendMessageW(canvas, WM_KEYDOWN, VK_RETURN, 0);
    CHECK(pumpMessages() < 1000);
    CHECK(mockSpeech.lastRequest.has_value());
    CHECK(mockSpeech.lastRequest->selection.has_value());
    CHECK(mockSpeech.lastRequest->selection->start == expectedWord.start);
    CHECK(mockSpeech.lastRequest->selection->length == expectedWord.length);

    // Double-clicking a word pronounces just that word.
    const auto& word2Frag = renderedLayout.words[2].fragments.front();
    const int w2X =
        static_cast<int>((word2Frag.x + word2Frag.width * 0.5F) * dpiScale);
    const int w2Y =
        static_cast<int>((word2Frag.y + word2Frag.height * 0.5F) * dpiScale);
    SendMessageW(canvas, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(w2X, w2Y));
    CHECK(pumpMessages() < 1000);
    CHECK(renderer.selected_word()->start == renderedLayout.words[2].text.start);
    CHECK(mockSpeech.lastRequest->selection->start ==
          renderedLayout.words[2].text.start);
    CHECK(mockSpeech.lastRequest->selection->length ==
          renderedLayout.words[2].text.length);

    // Clicking Play reads from the cursor to the end of the document.
    SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(1010, BN_CLICKED), 0);
    CHECK(pumpMessages() < 1000);
    CHECK(mockSpeech.lastRequest->selection.has_value());
    CHECK(mockSpeech.lastRequest->selection->start ==
          renderedLayout.words[2].text.start);
    CHECK(mockSpeech.lastRequest->selection->length ==
          doc.text.size() - renderedLayout.words[2].text.start);

    // Escape in Floating mode clears the active word cursor.
    SendMessageW(canvas, WM_KEYDOWN, VK_ESCAPE, 0);
    CHECK(!renderer.selected_word().has_value());

    // Ensure repaint queue goes completely idle (no WM_CTLCOLOR -> Invalidate loop).
    RedrawWindow(hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    CHECK(pumpMessages() < 1000);
    CHECK(pumpMessages() == 0);

    // Verify hide-on-close sends SW_HIDE rather than quitting/destroying.
    SendMessageW(hwnd, WM_CLOSE, 0, 0);
    CHECK(IsWindow(hwnd) != FALSE);

    renderer.set_hide_on_close(false);
    DestroyWindow(hwnd);
    return 0;
}
