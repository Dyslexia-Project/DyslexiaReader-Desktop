#pragma once

#include "dyslexia_reader/interfaces.hpp"
#include "dyslexia_reader/windows/reading_aids.hpp"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwrite_2.h>
#include <commctrl.h>
#include <deque>
#include <functional>
#include <utility>
#include <memory>
#include <mutex>
#include <wrl/client.h>

namespace dyslexia_reader::windows {

class InMemoryFontCollection;

class Direct2DTextRenderer final : public ITextRenderer {
public:
    explicit Direct2DTextRenderer(IFontLoader& fontLoader,
                                  ISpeechEngine* speechEngine = nullptr);
    ~Direct2DTextRenderer() override;

    Direct2DTextRenderer(const Direct2DTextRenderer&) = delete;
    Direct2DTextRenderer& operator=(const Direct2DTextRenderer&) = delete;

    // Create, show, and use the renderer on one UI thread.
    HWND create_reader_window(HINSTANCE instance, HWND owner = nullptr);
    void show(int showCommand);
    HWND hwnd() const noexcept { return hwnd_; }
    void set_mode(ReaderMode mode);
    ReaderMode mode() const noexcept { return mode_; }
    void set_hide_on_close(bool hideOnClose) noexcept {
        hideOnClose_ = hideOnClose;
    }
    bool is_visible() const noexcept;
    void set_on_profile_changed(std::function<void(const ReadingProfile&)> cb) {
        onProfileChanged_ = std::move(cb);
    }
    void set_on_mode_changed(std::function<void(ReaderMode)> cb) {
        onModeChanged_ = std::move(cb);
    }
    void set_on_capture_requested(std::function<void()> cb) {
        onCaptureRequested_ = std::move(cb);
        if (captureButton_ != nullptr) {
            EnableWindow(captureButton_, onCaptureRequested_ ? TRUE : FALSE);
        }
    }
    void set_on_clipboard_requested(std::function<void()> cb) {
        onClipboardRequested_ = std::move(cb);
        if (clipboardButton_ != nullptr) {
            EnableWindow(clipboardButton_,
                         onClipboardRequested_ ? TRUE : FALSE);
        }
    }
    void set_on_import_share_code_requested(std::function<void()> cb) {
        onImportShareCodeRequested_ = std::move(cb);
        sync_platform_button_state();
    }
    void set_on_share_profile_requested(std::function<void()> cb) {
        onShareProfileRequested_ = std::move(cb);
        sync_platform_button_state();
    }
    void set_on_open_web_playground_requested(std::function<void()> cb) {
        onOpenWebPlaygroundRequested_ = std::move(cb);
        sync_platform_button_state();
    }
    void set_on_define_word_requested(std::function<void()> cb) {
        onDefineWordRequested_ = std::move(cb);
        sync_toolbar_controls();
    }
    void set_on_word_cloud_requested(std::function<void()> cb) {
        onWordCloudRequested_ = std::move(cb);
        sync_toolbar_controls();
    }

    void set_document(const ReadingDocument& document) override;
    [[nodiscard]] const ReadingProfile& profile() const noexcept {
        return profile_;
    }
    [[nodiscard]] const ReadingDocument& document() const noexcept {
        return document_;
    }
    [[nodiscard]] std::optional<std::string> selected_word_utf8() const;
    void select_word_at_index(std::size_t wordIndex);
    void speak_selected_word();
    void speak_text_utf8(const std::string& utf8Text);

    void set_profile(const ReadingProfile& profile) override;
    void set_viewport(const Viewport& viewport) override;
    TextLayoutSnapshot layout_snapshot() const override;
    void set_aid_visuals(const AidVisuals& visuals) override;
    void set_highlights(const HighlightState& highlights) override;
    void invalidate() override;
    std::optional<TextRange> selected_word() const noexcept;

private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message,
                                       WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK toolbar_control_proc(
        HWND control, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR subclassId, DWORD_PTR referenceData);
    LRESULT handle_message(UINT message, WPARAM wParam, LPARAM lParam);

    void ensure_render_target();
    void create_toolbar_controls();
    static LRESULT CALLBACK canvas_window_proc(HWND window, UINT message,
                                               WPARAM wParam, LPARAM lParam);
    LRESULT handle_canvas_message(UINT message, WPARAM wParam, LPARAM lParam);
    void create_canvas_window();
    void layout_toolbar_controls();
    void layout_canvas_window();
    void paint_toolbar(HDC hdc, const RECT& dirtyRect) const;
    bool handle_navigation_key(WPARAM key);
    void handle_mouse_wheel(WPARAM wParam);
    void sync_toolbar_controls();
    void handle_command(int controlId, int notificationCode);
    void handle_horizontal_scroll(HWND control);
    void initialize_speech_controls();
    void handle_speech_command(int controlId);
    void handle_speech_event(const SpeechEvent& event);
    TextLayoutSnapshot line_layout_snapshot() const;
    std::vector<RectF> text_range_bounds(TextRange range) const;
    AidVisuals compose_bimodal_highlights() const;
    void refresh_aid_visuals();
    void refresh_highlight_visuals();
    void select_ruler_line_at(int pixelY);
    void move_ruler_line(int direction);
    bool handle_ruler_key(WPARAM key);
    bool select_word_at_point(int pixelX, int pixelY);
    void move_word_cursor(int wordDelta);
    void move_word_cursor_line(int lineDelta);
    void clear_word_cursor();
    void start_speech_request(std::optional<TextRange> selection);
    void sync_ruler_to_word();
    void draw_word_cursor(bool underText) const;
    void sync_platform_button_state() const;
    void show_platform_menu();
    void draw_toolbar_button(const DRAWITEMSTRUCT* dis) const;
    void rebuild_text_layout();
    void discard_device_resources() noexcept;
    void paint();
    void resize_viewport();
    void require_ui_thread() const;

    static constexpr wchar_t kWindowClassName[] =
        L"DyslexiaReader.Direct2DReaderWindow";
    static constexpr wchar_t kCanvasClassName[] =
        L"DyslexiaReader.Direct2DCanvasWindow";
    static constexpr float kToolbarHeightDip = 128;
    static constexpr float kTextPaddingDip = 28;
    static constexpr int kFontFamilyControlId = 1001;
    static constexpr int kFontSizeDecreaseControlId = 1002;
    static constexpr int kFontSizeIncreaseControlId = 1003;
    static constexpr int kThemeControlId = 1004;
    static constexpr int kLetterSpacingControlId = 1005;
    static constexpr int kLineHeightControlId = 1006;
    static constexpr int kBionicReadingControlId = 1007;
    static constexpr int kReadingRulerControlId = 1008;
    static constexpr int kBimodalHighlightingControlId = 1013;
    static constexpr int kSpeechVoiceControlId = 1009;
    static constexpr int kSpeechPlayControlId = 1010;
    static constexpr int kSpeechStopControlId = 1011;
    static constexpr int kSpeechRateControlId = 1012;
    static constexpr int kFocusModeControlId = 1014;
    static constexpr int kCaptureControlId = 1015;
    static constexpr int kLineWidthControlId = 1016;
    static constexpr int kClipboardControlId = 1017;
    static constexpr int kSpeakWordControlId = 1018;
    static constexpr int kPlatformControlId = 1019;
    static constexpr int kDefineWordControlId = 1020;
    static constexpr int kWordCloudControlId = 1021;
    static constexpr UINT kSpeechEventMessage = WM_APP + 0x31;

    struct SpeechDispatchState {
        struct Event {
            std::uint64_t session = 0;
            SpeechEvent speech;
        };

        std::mutex mutex;
        HWND window = nullptr;
        std::deque<Event> pendingEvents;
    };

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND canvasHwnd_ = nullptr;
    DWORD uiThreadId_ = 0;
    UINT dpiX_ = USER_DEFAULT_SCREEN_DPI;
    UINT dpiY_ = USER_DEFAULT_SCREEN_DPI;

    Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwriteFactory_;
    std::unique_ptr<InMemoryFontCollection> fontCollection_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> renderTarget_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> rulerBrush_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> textFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> textLayout_;
    Microsoft::WRL::ComPtr<IDWriteTextLayout1> textLayout1_;

    ReadingDocument document_;
    ReadingProfile profile_;
    Viewport viewport_;
    AidVisuals aidVisuals_;
    HighlightState highlights_;
    BionicReadingAid bionicReadingAid_;
    ReadingRulerAid readingRulerAid_;
    BimodalHighlightingAid bimodalHighlightingAid_;
    ISpeechEngine* speechEngine_ = nullptr;
    std::shared_ptr<SpeechDispatchState> speechDispatch_;
    std::vector<VoiceInfo> speechVoices_;
    SpeechCapabilities speechCapabilities_;
    std::uint64_t speechSession_ = 0;
    PlaybackState speechState_ = PlaybackState::Idle;
    float speechRate_ = 1.0F;
    bool speechAvailable_ = false;
    bool speechTracksDocument_ = true;
    std::optional<std::uint32_t> activeVisualLine_;
    std::optional<TextOffset> rulerAnchorOffset_;
    std::optional<std::size_t> selectedWordIndex_;
    float cursorTargetCenterX_ = -1.0F;

    HWND fontFamilyCombo_ = nullptr;
    HWND fontFamilyLabel_ = nullptr;
    HWND decreaseFontButton_ = nullptr;
    HWND increaseFontButton_ = nullptr;
    HWND fontSizeLabel_ = nullptr;
    HWND themeLabel_ = nullptr;
    HWND themeCombo_ = nullptr;
    HWND letterSpacingTrackbar_ = nullptr;
    HWND letterSpacingLabel_ = nullptr;
    HWND lineHeightTrackbar_ = nullptr;
    HWND lineHeightLabel_ = nullptr;
    HWND bionicReadingCheckBox_ = nullptr;
    HWND readingRulerCheckBox_ = nullptr;
    HWND bimodalHighlightingCheckBox_ = nullptr;
    HWND speechVoiceLabel_ = nullptr;
    HWND speechVoiceCombo_ = nullptr;
    HWND speechPlayButton_ = nullptr;
    HWND speechStopButton_ = nullptr;
    HWND speakWordButton_ = nullptr;
    HWND defineWordButton_ = nullptr;
    HWND wordCloudButton_ = nullptr;
    HWND speechRateTrackbar_ = nullptr;
    HWND speechRateLabel_ = nullptr;
    HWND focusModeButton_ = nullptr;
    HWND captureButton_ = nullptr;
    HWND clipboardButton_ = nullptr;
    HWND platformButton_ = nullptr;
    HWND lineWidthTrackbar_ = nullptr;
    HWND lineWidthLabel_ = nullptr;
    HWND shortcutHintLabel_ = nullptr;
    HWND readingStatsLabel_ = nullptr;

    HFONT uiFont_ = nullptr;
    HANDLE gdiUiFontResource_ = nullptr;
    HBRUSH toolbarBrush_ = nullptr;
    ThemeId themedBrushId_ = ThemeId::WarmCream;
    bool hasThemedBrush_ = false;
    bool hasAppliedWindowTheme_ = false;
    ThemeId appliedWindowThemeId_ = ThemeId::WarmCream;

    ReaderMode mode_ = ReaderMode::Floating;
    bool hideOnClose_ = false;
    bool hasFloatingBounds_ = false;
    RECT floatingBounds_{};
    std::function<void(const ReadingProfile&)> onProfileChanged_;
    std::function<void(ReaderMode)> onModeChanged_;
    std::function<void()> onCaptureRequested_;
    std::function<void()> onClipboardRequested_;
    std::function<void()> onImportShareCodeRequested_;
    std::function<void()> onShareProfileRequested_;
    std::function<void()> onOpenWebPlaygroundRequested_;
    std::function<void()> onDefineWordRequested_;
    std::function<void()> onWordCloudRequested_;

    std::pair<float, float> text_column_metrics() const;
    float content_height_dip() const;
    float max_scroll_y() const;
    void set_scroll_y(float scrollY);
    void scroll_by_delta(float deltaDip);
    void sync_scrollbar() const;
    void commit_ui_profile(const ReadingProfile& nextProfile);
    void apply_window_mode();
    void update_focus_scroll(TextRange range);
    void rebuild_ui_font();
    void update_theme_resources();
    void destroy_theme_resources() noexcept;
};

}  // namespace dyslexia_reader::windows
