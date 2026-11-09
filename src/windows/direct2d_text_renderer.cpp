#include "dyslexia_reader/windows/direct2d_text_renderer.hpp"
#include "dyslexia_reader/windows/in_memory_font_collection.hpp"
#include "theme_palette.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dyslexia_reader::windows {
namespace {

[[noreturn]] void throw_hresult(ReaderErrorCode code, const char* operation,
                                HRESULT result) {
    throw ReaderError(
        code, std::string(operation) + " failed (HRESULT " +
                  std::to_string(static_cast<unsigned long>(result)) + ")");
}

void check_hresult(HRESULT result, const char* operation,
                   ReaderErrorCode code = ReaderErrorCode::RenderingFailed) {
    if (FAILED(result)) {
        throw_hresult(code, operation, result);
    }
}

float dip_from_points(float points) {
    return points * (96.0F / 72.0F);
}

void validate_profile(const ReadingProfile& profile) {
    const bool validFont =
        profile.fontFamily == FontFamily::Lexend ||
        profile.fontFamily == FontFamily::AtkinsonHyperlegible ||
        profile.fontFamily == FontFamily::OpenDyslexic;
    const bool validTheme =
        profile.theme == ThemeId::WarmCream ||
        profile.theme == ThemeId::Sepia ||
        profile.theme == ThemeId::DarkHighContrast;
    if (profile.schemaVersion != 1 || !validFont || !validTheme ||
        !std::isfinite(profile.fontSizePt) ||
        profile.fontSizePt < 14 || profile.fontSizePt > 48 ||
        !std::isfinite(profile.letterSpacingEm) ||
        profile.letterSpacingEm < 0 || profile.letterSpacingEm > 0.2F ||
        !std::isfinite(profile.wordSpacingEm) ||
        profile.wordSpacingEm < 0 || profile.wordSpacingEm > 0.5F ||
        !std::isfinite(profile.lineHeight) || profile.lineHeight < 1 ||
        profile.lineHeight > 2.5F || profile.lineWidthCh < 35 ||
        profile.lineWidthCh > 100) {
        throw ReaderError(ReaderErrorCode::InvalidProfile,
                          "Reading profile is outside schema version 1 bounds");
    }
}

const theme_palette::ThemeHex& theme_tokens(ThemeId theme) {
    switch (theme) {
    case ThemeId::WarmCream:
        return theme_palette::warmCream;
    case ThemeId::Sepia:
        return theme_palette::sepia;
    case ThemeId::DarkHighContrast:
        return theme_palette::darkHighContrast;
    }
    throw ReaderError(ReaderErrorCode::InvalidProfile,
                      "Reading profile uses an unknown color theme");
}

unsigned int hex_digit(char value) {
    if (value >= '0' && value <= '9') {
        return static_cast<unsigned int>(value - '0');
    }
    if (value >= 'A' && value <= 'F') {
        return static_cast<unsigned int>(value - 'A' + 10);
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<unsigned int>(value - 'a' + 10);
    }
    throw ReaderError(ReaderErrorCode::RenderingFailed,
                      "Theme contract contains an invalid color token");
}

D2D1_COLOR_F color_from_hex(std::string_view value) {
    if (value.size() != 7 || value.front() != '#') {
        throw ReaderError(ReaderErrorCode::RenderingFailed,
                          "Theme contract colors must use #RRGGBB values");
    }
    const auto channel = [&value](std::size_t offset) {
        return static_cast<float>(hex_digit(value[offset]) * 16U +
                                  hex_digit(value[offset + 1])) /
               255.0F;
    };
    return D2D1::ColorF(channel(1), channel(3), channel(5), 1.0F);
}

D2D1_COLOR_F color_from_rgba(std::uint32_t value) {
    constexpr float scale = 1.0F / 255.0F;
    return D2D1::ColorF(
        static_cast<float>((value >> 24) & 0xFF) * scale,
        static_cast<float>((value >> 16) & 0xFF) * scale,
        static_cast<float>((value >> 8) & 0xFF) * scale,
        static_cast<float>(value & 0xFF) * scale);
}

const wchar_t* font_family_label(FontFamily family) {
    switch (family) {
    case FontFamily::Lexend:
        return L"Lexend";
    case FontFamily::AtkinsonHyperlegible:
        return L"Atkinson Hyperlegible";
    case FontFamily::OpenDyslexic:
        return L"OpenDyslexic";
    }
    throw ReaderError(ReaderErrorCode::InvalidProfile,
                      "Reading profile uses an unknown font family");
}

UINT scale_dip(float value, UINT dpi) {
    return static_cast<UINT>(std::lround(value * static_cast<float>(dpi) /
                                         USER_DEFAULT_SCREEN_DPI));
}

std::wstring wide_from_utf8(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (size == 0) {
        throw ReaderError(ReaderErrorCode::SpeechUnavailable,
                          "Could not decode a Windows speech voice name");
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(),
                            size) != size) {
        throw ReaderError(ReaderErrorCode::SpeechUnavailable,
                          "Could not decode a Windows speech voice name");
    }
    return result;
}

void log_window_error(const std::exception& error) {
    OutputDebugStringA("Dyslexia Reader window error: ");
    OutputDebugStringA(error.what());
    OutputDebugStringA("\n");
}

}  // namespace

Direct2DTextRenderer::Direct2DTextRenderer(IFontLoader& fontLoader,
                                           ISpeechEngine* speechEngine)
    : speechEngine_(speechEngine),
      speechDispatch_(std::make_shared<SpeechDispatchState>()) {
    check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                    d2dFactory_.GetAddressOf()),
                  "D2D1CreateFactory");
    check_hresult(DWriteCreateFactory(
                      DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                      reinterpret_cast<IUnknown**>(
                          dwriteFactory_.GetAddressOf())),
                  "DWriteCreateFactory");
    fontCollection_ =
        std::make_unique<InMemoryFontCollection>(dwriteFactory_.Get(),
                                                 fontLoader);
}

Direct2DTextRenderer::~Direct2DTextRenderer() {
    textLayout1_.Reset();
    textLayout_.Reset();
    textFormat_.Reset();
    rulerBrush_.Reset();
    fontCollection_.reset();
    if (hwnd_ != nullptr && IsWindow(hwnd_)) {
        DestroyWindow(hwnd_);
    }
}

HWND Direct2DTextRenderer::create_reader_window(HINSTANCE instance,
                                                HWND owner) {
    if (hwnd_ != nullptr) {
        throw ReaderError(ReaderErrorCode::WindowCreationFailed,
                          "Reader window has already been created");
    }

    instance_ = instance;
    uiThreadId_ = GetCurrentThreadId();

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance_;
    windowClass.lpfnWndProc = &Direct2DTextRenderer::window_proc;
    windowClass.lpszClassName = kWindowClassName;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;

    SetLastError(ERROR_SUCCESS);
    if (RegisterClassExW(&windowClass) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        throw ReaderError(
            ReaderErrorCode::WindowCreationFailed,
            "Could not register the Direct2D reader window class");
    }

    hwnd_ = CreateWindowExW(
        0, kWindowClassName, L"Dyslexia Reader",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
        800, 500, owner, nullptr, instance_, this);
    if (hwnd_ == nullptr) {
        throw ReaderError(ReaderErrorCode::WindowCreationFailed,
                          "Could not create the reader window");
    }
    return hwnd_;
}

void Direct2DTextRenderer::show(int showCommand) {
    require_ui_thread();
    if (hwnd_ == nullptr) {
        throw ReaderError(ReaderErrorCode::WindowCreationFailed,
                          "Reader window has not been created");
    }
    ShowWindow(hwnd_, showCommand);
    UpdateWindow(hwnd_);
}

void Direct2DTextRenderer::set_document(const ReadingDocument& document) {
    require_ui_thread();
    document_ = document;
    highlights_ = {};
    activeVisualLine_.reset();
    rulerAnchorOffset_.reset();
    rebuild_text_layout();
    refresh_aid_visuals();
    invalidate();
}

void Direct2DTextRenderer::set_profile(const ReadingProfile& profile) {
    require_ui_thread();
    validate_profile(profile);
    profile_ = profile;
    rebuild_text_layout();
    sync_toolbar_controls();
    refresh_aid_visuals();
    invalidate();
}

void Direct2DTextRenderer::set_viewport(const Viewport& viewport) {
    require_ui_thread();
    viewport_ = viewport;
    rebuild_text_layout();
    refresh_aid_visuals();
    invalidate();
}

TextLayoutSnapshot Direct2DTextRenderer::layout_snapshot() const {
    TextLayoutSnapshot snapshot = line_layout_snapshot();
    if (textLayout_ == nullptr) {
        return snapshot;
    }

    for (const auto& word : document_.words) {
        VisualWord visualWord;
        visualWord.text = word.text;
        visualWord.fragments = text_range_bounds(word.text);
        snapshot.words.push_back(std::move(visualWord));
    }
    for (const auto& sentence : document_.sentences) {
        snapshot.sentences.push_back(
            {sentence, text_range_bounds(sentence)});
    }
    return snapshot;
}

TextLayoutSnapshot Direct2DTextRenderer::line_layout_snapshot() const {
    require_ui_thread();
    TextLayoutSnapshot snapshot;
    if (textLayout_ == nullptr) {
        return snapshot;
    }

    UINT32 lineCount = 0;
    const HRESULT lineCountResult =
        textLayout_->GetLineMetrics(nullptr, 0, &lineCount);
    if (lineCountResult != E_NOT_SUFFICIENT_BUFFER &&
        FAILED(lineCountResult)) {
        check_hresult(lineCountResult, "IDWriteTextLayout::GetLineMetrics");
    }

    if (lineCount > 0) {
        std::vector<DWRITE_LINE_METRICS> metrics(lineCount);
        check_hresult(textLayout_->GetLineMetrics(metrics.data(), lineCount,
                                                  &lineCount),
                      "IDWriteTextLayout::GetLineMetrics");

        UINT32 textStart = 0;
        float y = 0;
        for (const auto& metric : metrics) {
            const UINT32 visibleLength =
                metric.length - metric.newlineLength;
            snapshot.lines.push_back(
                {{textStart, visibleLength},
                 {kTextPaddingDip, kToolbarHeightDip + kTextPaddingDip + y,
                  std::max(0.0F,
                           viewport_.size.width - 2 * kTextPaddingDip),
                  metric.height}});
            textStart += metric.length;
            y += metric.height;
        }
    }
    return snapshot;
}

std::vector<RectF> Direct2DTextRenderer::text_range_bounds(
    TextRange range) const {
    std::vector<RectF> bounds;
    if (textLayout_ == nullptr || range.length == 0 ||
        range.start >= document_.text.size() ||
        range.length > document_.text.size() - range.start) {
        return bounds;
    }

    UINT32 hitCount = 0;
    const HRESULT countResult = textLayout_->HitTestTextRange(
        range.start, range.length, kTextPaddingDip,
        kToolbarHeightDip + kTextPaddingDip - viewport_.scrollY, nullptr, 0,
        &hitCount);
    if (countResult != E_NOT_SUFFICIENT_BUFFER && FAILED(countResult)) {
        check_hresult(countResult, "IDWriteTextLayout::HitTestTextRange");
    }
    if (hitCount == 0) {
        return bounds;
    }

    std::vector<DWRITE_HIT_TEST_METRIC> hits(hitCount);
    check_hresult(textLayout_->HitTestTextRange(
                      range.start, range.length, kTextPaddingDip,
                      kToolbarHeightDip + kTextPaddingDip -
                          viewport_.scrollY,
                      hits.data(), hitCount, &hitCount),
                  "IDWriteTextLayout::HitTestTextRange");
    bounds.reserve(hitCount);
    for (const auto& hit : hits) {
        bounds.push_back(
            {hit.left, hit.top + viewport_.scrollY, hit.width, hit.height});
    }
    return bounds;
}

void Direct2DTextRenderer::set_aid_visuals(const AidVisuals& visuals) {
    require_ui_thread();
    aidVisuals_ = visuals;
    if (textLayout_ != nullptr && !document_.text.empty()) {
        const DWRITE_TEXT_RANGE fullRange{
            0, static_cast<UINT32>(document_.text.size())};
        check_hresult(
            textLayout_->SetFontWeight(DWRITE_FONT_WEIGHT_NORMAL, fullRange),
            "IDWriteTextLayout::SetFontWeight");
        for (const auto& style : aidVisuals_.textStyles) {
            if (style.text.length == 0 ||
                style.text.start >= document_.text.size() ||
                style.text.length > document_.text.size() - style.text.start) {
                throw ReaderError(ReaderErrorCode::RenderingFailed,
                                  "Reading-aid style range is outside the text");
            }
            check_hresult(
                textLayout_->SetFontWeight(
                    static_cast<DWRITE_FONT_WEIGHT>(style.fontWeight),
                    DWRITE_TEXT_RANGE{style.text.start, style.text.length}),
                "IDWriteTextLayout::SetFontWeight");
        }
    }
    invalidate();
}

void Direct2DTextRenderer::set_highlights(const HighlightState& highlights) {
    require_ui_thread();
    const auto validRange = [this](const std::optional<TextRange>& range) {
        return !range ||
               (range->length > 0 && range->start < document_.text.size() &&
                range->length <= document_.text.size() - range->start);
    };
    if (!validRange(highlights.sentence) || !validRange(highlights.word)) {
        throw ReaderError(ReaderErrorCode::RenderingFailed,
                          "Speech highlight range is outside the document");
    }
    highlights_ = highlights;
    refresh_highlight_visuals();
}

void Direct2DTextRenderer::invalidate() {
    require_ui_thread();
    if (hwnd_ != nullptr) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

LRESULT CALLBACK Direct2DTextRenderer::window_proc(HWND window,
                                                   UINT message,
                                                   WPARAM wParam,
                                                   LPARAM lParam) {
    Direct2DTextRenderer* renderer =
        reinterpret_cast<Direct2DTextRenderer*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        const auto* create =
            reinterpret_cast<const CREATESTRUCTW*>(lParam);
        renderer =
            static_cast<Direct2DTextRenderer*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(renderer));
        renderer->hwnd_ = window;
    }

    if (renderer != nullptr) {
        LRESULT result = 0;
        try {
            result = renderer->handle_message(message, wParam, lParam);
        } catch (const std::exception& error) {
            log_window_error(error);
            result = message == WM_CREATE ? -1 : 0;
        }
        if (message == WM_NCDESTROY) {
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            renderer->hwnd_ = nullptr;
        }
        return result;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK Direct2DTextRenderer::toolbar_control_proc(
    HWND control, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR referenceData) {
    auto* renderer =
        reinterpret_cast<Direct2DTextRenderer*>(referenceData);
    if (message == WM_SYSKEYDOWN && renderer != nullptr) {
        try {
            if (renderer->handle_ruler_key(wParam)) {
                return 0;
            }
        } catch (const std::exception& error) {
            log_window_error(error);
            return 0;
        }
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(control,
                             &Direct2DTextRenderer::toolbar_control_proc,
                             subclassId);
    }
    return DefSubclassProc(control, message, wParam, lParam);
}

LRESULT Direct2DTextRenderer::handle_message(UINT message, WPARAM wParam,
                                            LPARAM lParam) {
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paintStruct{};
        BeginPaint(hwnd_, &paintStruct);
        struct PaintScope {
            HWND window;
            PAINTSTRUCT& state;
            ~PaintScope() { EndPaint(window, &state); }
        } paintScope{hwnd_, paintStruct};
        paint();
        return 0;
    }
    case WM_DESTROY:
        ++speechSession_;
        if (speechEngine_ != nullptr) {
            try {
                speechEngine_->stop();
            } catch (const ReaderError& error) {
                MessageBoxA(hwnd_, error.what(), "Speech playback",
                            MB_OK | MB_ICONWARNING);
            }
        }
        {
            std::lock_guard<std::mutex> lock(speechDispatch_->mutex);
            speechDispatch_->window = nullptr;
            speechDispatch_->pendingEvents.clear();
        }
        PostQuitMessage(0);
        return 0;
    case WM_CREATE:
        dpiX_ = GetDpiForWindow(hwnd_);
        if (dpiX_ == 0) {
            dpiX_ = USER_DEFAULT_SCREEN_DPI;
        }
        dpiY_ = dpiX_;
        {
            std::lock_guard<std::mutex> lock(speechDispatch_->mutex);
            speechDispatch_->window = hwnd_;
        }
        create_toolbar_controls();
        layout_toolbar_controls();
        sync_toolbar_controls();
        resize_viewport();
        rebuild_text_layout();
        refresh_aid_visuals();
        return 0;
    case WM_SIZE:
        if (renderTarget_ != nullptr) {
            const UINT width = LOWORD(lParam);
            const UINT height = HIWORD(lParam);
            if (width > 0 && height > 0) {
                const HRESULT result =
                    renderTarget_->Resize(D2D1::SizeU(width, height));
                if (FAILED(result)) {
                    discard_device_resources();
                }
            }
        }
        layout_toolbar_controls();
        resize_viewport();
        rebuild_text_layout();
        refresh_aid_visuals();
        return 0;
    case WM_DPICHANGED: {
        dpiX_ = LOWORD(wParam);
        dpiY_ = HIWORD(wParam);
        const auto* suggested =
            reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        discard_device_resources();
        layout_toolbar_controls();
        resize_viewport();
        rebuild_text_layout();
        refresh_aid_visuals();
        return 0;
    }
    case WM_COMMAND:
        handle_command(LOWORD(wParam), HIWORD(wParam));
        return 0;
    case WM_HSCROLL:
        handle_horizontal_scroll(reinterpret_cast<HWND>(lParam));
        return 0;
    case kSpeechEventMessage: {
        std::deque<SpeechDispatchState::Event> events;
        {
            std::lock_guard<std::mutex> lock(speechDispatch_->mutex);
            events.swap(speechDispatch_->pendingEvents);
        }
        for (const auto& event : events) {
            if (event.session == speechSession_) {
                handle_speech_event(event.speech);
            }
        }
        return 0;
    }
    case WM_SYSKEYDOWN:
        if (handle_ruler_key(wParam)) {
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    case WM_LBUTTONDOWN:
        if (profile_.aids.readingRuler) {
            SetFocus(hwnd_);
            select_ruler_line_at(
                static_cast<int>(static_cast<SHORT>(HIWORD(lParam))));
            SetCapture(hwnd_);
            return 0;
        }
        break;
    case WM_MOUSEMOVE:
        if (profile_.aids.readingRuler && GetCapture() == hwnd_ &&
            (wParam & MK_LBUTTON) != 0) {
            select_ruler_line_at(
                static_cast<int>(static_cast<SHORT>(HIWORD(lParam))));
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (GetCapture() == hwnd_) {
            ReleaseCapture();
            return 0;
        }
        break;
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        limits->ptMinTrackSize.x = static_cast<LONG>(scale_dip(680, dpiX_));
        limits->ptMinTrackSize.y = static_cast<LONG>(scale_dip(360, dpiY_));
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCDESTROY:
        {
            std::lock_guard<std::mutex> lock(speechDispatch_->mutex);
            speechDispatch_->window = nullptr;
        }
        discard_device_resources();
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    default:
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

void Direct2DTextRenderer::create_toolbar_controls() {
    INITCOMMONCONTROLSEX commonControls{};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_BAR_CLASSES;
    if (!InitCommonControlsEx(&commonControls)) {
        throw ReaderError(ReaderErrorCode::WindowCreationFailed,
                          "Could not initialize reader toolbar controls");
    }

    const auto create = [this](const wchar_t* className,
                               const wchar_t* label, DWORD style, int id) {
        HWND control = CreateWindowExW(
            0, className, label, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1,
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            instance_, nullptr);
        if (control == nullptr) {
            throw ReaderError(ReaderErrorCode::WindowCreationFailed,
                              "Could not create a reader typography control");
        }
        SendMessageW(control, WM_SETFONT,
                     reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),
                     TRUE);
        if (!SetWindowSubclass(
                control, &Direct2DTextRenderer::toolbar_control_proc, 1,
                reinterpret_cast<DWORD_PTR>(this))) {
            DestroyWindow(control);
            throw ReaderError(ReaderErrorCode::WindowCreationFailed,
                              "Could not enable reader keyboard shortcuts");
        }
        return control;
    };

    fontFamilyLabel_ = create(L"STATIC", L"Font:", SS_LEFT, 0);
    fontFamilyCombo_ =
        create(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
               kFontFamilyControlId);
    decreaseFontButton_ =
        create(L"BUTTON", L"-", BS_PUSHBUTTON | WS_TABSTOP,
               kFontSizeDecreaseControlId);
    fontSizeLabel_ = create(L"STATIC", L"", SS_CENTER, 0);
    increaseFontButton_ =
        create(L"BUTTON", L"+", BS_PUSHBUTTON | WS_TABSTOP,
               kFontSizeIncreaseControlId);
    themeLabel_ = create(L"STATIC", L"Theme:", SS_LEFT, 0);
    themeCombo_ =
        create(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
               kThemeControlId);
    letterSpacingLabel_ = create(L"STATIC", L"Letter:", SS_LEFT, 0);
    letterSpacingTrackbar_ =
        create(TRACKBAR_CLASSW, L"", TBS_NOTICKS | WS_TABSTOP,
               kLetterSpacingControlId);
    lineHeightCaption_ = create(L"STATIC", L"Line height:", SS_LEFT, 0);
    lineHeightTrackbar_ =
        create(TRACKBAR_CLASSW, L"", TBS_NOTICKS | WS_TABSTOP,
               kLineHeightControlId);
    lineHeightLabel_ = create(L"STATIC", L"", SS_LEFT, 0);
    bionicReadingCheckBox_ =
        create(L"BUTTON", L"Bionic emphasis",
               BS_AUTOCHECKBOX | WS_TABSTOP, kBionicReadingControlId);
    readingRulerCheckBox_ =
        create(L"BUTTON", L"Reading ruler",
               BS_AUTOCHECKBOX | WS_TABSTOP, kReadingRulerControlId);
    bimodalHighlightingCheckBox_ =
        create(L"BUTTON", L"Bimodal highlighting",
               BS_AUTOCHECKBOX | WS_TABSTOP,
               kBimodalHighlightingControlId);

    for (const wchar_t* family :
         {L"Lexend", L"Atkinson Hyperlegible", L"OpenDyslexic"}) {
        const LRESULT result =
            SendMessageW(fontFamilyCombo_, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(family));
        if (result == CB_ERR || result == CB_ERRSPACE) {
            throw ReaderError(ReaderErrorCode::WindowCreationFailed,
                              "Could not populate the font family control");
        }
    }
    for (const wchar_t* theme :
         {L"Warm cream", L"Sepia", L"Dark high contrast"}) {
        const LRESULT result = SendMessageW(
            themeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(theme));
        if (result == CB_ERR || result == CB_ERRSPACE) {
            throw ReaderError(ReaderErrorCode::WindowCreationFailed,
                              "Could not populate the theme control");
        }
    }
    SendMessageW(letterSpacingTrackbar_, TBM_SETRANGE, TRUE,
                 MAKELONG(0, 20));
    SendMessageW(lineHeightTrackbar_, TBM_SETRANGE, TRUE,
                 MAKELONG(10, 25));

    speechVoiceLabel_ = create(L"STATIC", L"Voice:", SS_LEFT, 0);
    speechVoiceCombo_ =
        create(WC_COMBOBOXW, L"",
               CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
               kSpeechVoiceControlId);
    speechPlayButton_ =
        create(L"BUTTON", L"Play", BS_PUSHBUTTON | WS_TABSTOP,
               kSpeechPlayControlId);
    speechStopButton_ =
        create(L"BUTTON", L"Stop", BS_PUSHBUTTON | WS_TABSTOP,
               kSpeechStopControlId);
    speechRateCaption_ = create(L"STATIC", L"Speed:", SS_LEFT, 0);
    speechRateTrackbar_ =
        create(TRACKBAR_CLASSW, L"", TBS_NOTICKS | WS_TABSTOP,
               kSpeechRateControlId);
    speechRateLabel_ = create(L"STATIC", L"1.0x", SS_LEFT, 0);
    SendMessageW(speechRateTrackbar_, TBM_SETRANGE, TRUE,
                 MAKELONG(50, 200));
    SendMessageW(speechRateTrackbar_, TBM_SETPOS, TRUE, 100);
    initialize_speech_controls();
}

void Direct2DTextRenderer::initialize_speech_controls() {
    if (speechEngine_ == nullptr) {
        return;
    }

    try {
        speechVoices_ = speechEngine_->voices();
    } catch (const ReaderError& error) {
        MessageBoxA(hwnd_, error.what(), "Offline speech unavailable",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    try {
        speechCapabilities_ = speechEngine_->capabilities();
    } catch (const ReaderError& error) {
        OutputDebugStringA("Speech timing capabilities unavailable: ");
        OutputDebugStringA(error.what());
        OutputDebugStringA("\n");
    }

    for (std::size_t index = 0; index < speechVoices_.size(); ++index) {
        const auto& voice = speechVoices_[index];
        const auto label = wide_from_utf8(
            voice.displayName + " (" + voice.locale + ")");
        const LRESULT item = SendMessageW(
            speechVoiceCombo_, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(label.c_str()));
        if (item == CB_ERR || item == CB_ERRSPACE ||
            SendMessageW(speechVoiceCombo_, CB_SETITEMDATA,
                         static_cast<WPARAM>(item),
                         static_cast<LPARAM>(index)) == CB_ERR) {
            throw ReaderError(ReaderErrorCode::WindowCreationFailed,
                              "Could not populate the speech voice control");
        }
    }
    if (!speechVoices_.empty() &&
        SendMessageW(speechVoiceCombo_, CB_SETCURSEL, 0, 0) == CB_ERR) {
        throw ReaderError(ReaderErrorCode::WindowCreationFailed,
                          "Could not select the default speech voice");
    }
    speechAvailable_ = !speechVoices_.empty();
}

void Direct2DTextRenderer::layout_toolbar_controls() {
    if (hwnd_ == nullptr || fontFamilyCombo_ == nullptr) {
        return;
    }
    const auto move = [this](HWND control, float x, float y, float width,
                             float height) {
        if (!MoveWindow(control, static_cast<int>(scale_dip(x, dpiX_)),
                        static_cast<int>(scale_dip(y, dpiY_)),
                        static_cast<int>(scale_dip(width, dpiX_)),
                        static_cast<int>(scale_dip(height, dpiY_)), TRUE)) {
            throw ReaderError(ReaderErrorCode::RenderingFailed,
                              "Could not position a reader toolbar control");
        }
    };

    move(fontFamilyLabel_, 12, 12, 42, 24);
    move(themeLabel_, 376, 12, 52, 24);
    move(letterSpacingLabel_, 12, 52, 94, 24);
    move(lineHeightCaption_, 326, 52, 78, 24);
    move(bionicReadingCheckBox_, 12, 84, 150, 26);
    move(readingRulerCheckBox_, 180, 84, 150, 26);
    move(bimodalHighlightingCheckBox_, 350, 84, 190, 26);
    move(fontFamilyCombo_, 56, 10, 164, 220);
    move(decreaseFontButton_, 228, 10, 30, 28);
    move(fontSizeLabel_, 262, 12, 64, 24);
    move(increaseFontButton_, 332, 10, 30, 28);
    move(themeCombo_, 430, 10, 200, 220);
    move(letterSpacingTrackbar_, 110, 48, 170, 28);
    move(lineHeightTrackbar_, 404, 48, 154, 28);
    move(lineHeightLabel_, 564, 52, 64, 24);
    move(speechVoiceLabel_, 12, 122, 42, 24);
    move(speechVoiceCombo_, 56, 120, 210, 220);
    move(speechPlayButton_, 274, 120, 70, 28);
    move(speechStopButton_, 350, 120, 50, 28);
    move(speechRateCaption_, 412, 122, 42, 24);
    move(speechRateTrackbar_, 458, 118, 124, 28);
    move(speechRateLabel_, 590, 122, 44, 24);
}

void Direct2DTextRenderer::sync_toolbar_controls() {
    if (fontFamilyCombo_ == nullptr) {
        return;
    }
    int familyIndex = 0;
    switch (profile_.fontFamily) {
    case FontFamily::Lexend:
        familyIndex = 0;
        break;
    case FontFamily::AtkinsonHyperlegible:
        familyIndex = 1;
        break;
    case FontFamily::OpenDyslexic:
        familyIndex = 2;
        break;
    }
    int themeIndex = 0;
    switch (profile_.theme) {
    case ThemeId::WarmCream:
        themeIndex = 0;
        break;
    case ThemeId::Sepia:
        themeIndex = 1;
        break;
    case ThemeId::DarkHighContrast:
        themeIndex = 2;
        break;
    }

    if (SendMessageW(fontFamilyCombo_, CB_SETCURSEL, familyIndex, 0) ==
            CB_ERR ||
        SendMessageW(themeCombo_, CB_SETCURSEL, themeIndex, 0) == CB_ERR) {
        throw ReaderError(ReaderErrorCode::RenderingFailed,
                          "Could not synchronize reader toolbar selections");
    }
    SendMessageW(letterSpacingTrackbar_, TBM_SETPOS, TRUE,
                 static_cast<LPARAM>(
                     std::lround(profile_.letterSpacingEm * 100.0F)));
    SendMessageW(lineHeightTrackbar_, TBM_SETPOS, TRUE,
                 static_cast<LPARAM>(
                     std::lround(profile_.lineHeight * 10.0F)));
    SendMessageW(bionicReadingCheckBox_, BM_SETCHECK,
                 profile_.aids.bionicReading ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(readingRulerCheckBox_, BM_SETCHECK,
                 profile_.aids.readingRuler ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(
        bimodalHighlightingCheckBox_, BM_SETCHECK,
        profile_.aids.bimodalHighlighting ? BST_CHECKED : BST_UNCHECKED, 0);

    std::wostringstream sizeText;
    sizeText << std::fixed << std::setprecision(1) << profile_.fontSizePt
             << L" pt";
    if (!SetWindowTextW(fontSizeLabel_, sizeText.str().c_str())) {
        throw ReaderError(ReaderErrorCode::RenderingFailed,
                          "Could not update the font size label");
    }
    std::wostringstream letterText;
    letterText << std::fixed << std::setprecision(2)
               << profile_.letterSpacingEm;
    if (!SetWindowTextW(letterSpacingLabel_,
                        (L"Letter: " + letterText.str()).c_str())) {
        throw ReaderError(ReaderErrorCode::RenderingFailed,
                          "Could not update the letter spacing label");
    }
    std::wostringstream lineText;
    lineText << std::fixed << std::setprecision(1) << profile_.lineHeight
             << L"x";
    if (!SetWindowTextW(lineHeightLabel_, lineText.str().c_str())) {
        throw ReaderError(ReaderErrorCode::RenderingFailed,
                          "Could not update the line height label");
    }
    SendMessageW(speechRateTrackbar_, TBM_SETPOS, TRUE,
                 static_cast<LPARAM>(std::lround(speechRate_ * 100.0F)));
    std::wostringstream speechRateText;
    speechRateText << std::fixed << std::setprecision(1) << speechRate_ << L"x";
    if (!SetWindowTextW(speechRateLabel_, speechRateText.str().c_str())) {
        throw ReaderError(ReaderErrorCode::RenderingFailed,
                          "Could not update the speech speed label");
    }
    if (speechPlayButton_ != nullptr) {
        const wchar_t* label =
            speechState_ == PlaybackState::Loading ? L"Loading" :
            speechState_ == PlaybackState::Playing ? L"Pause" :
            speechState_ == PlaybackState::Paused ? L"Resume" : L"Play";
        SetWindowTextW(speechPlayButton_, label);
    }
    EnableWindow(speechVoiceCombo_, speechAvailable_ ? TRUE : FALSE);
    EnableWindow(speechPlayButton_,
                 speechAvailable_ && speechState_ != PlaybackState::Loading
                     ? TRUE
                     : FALSE);
    EnableWindow(speechStopButton_, speechAvailable_ ? TRUE : FALSE);
    EnableWindow(speechRateTrackbar_, speechAvailable_ ? TRUE : FALSE);
}

void Direct2DTextRenderer::handle_command(int controlId,
                                          int notificationCode) {
    ReadingProfile updated = profile_;
    switch (controlId) {
    case kFontFamilyControlId:
        if (notificationCode != CBN_SELCHANGE) {
            return;
        }
        switch (SendMessageW(fontFamilyCombo_, CB_GETCURSEL, 0, 0)) {
        case 0:
            updated.fontFamily = FontFamily::Lexend;
            break;
        case 1:
            updated.fontFamily = FontFamily::AtkinsonHyperlegible;
            break;
        case 2:
            updated.fontFamily = FontFamily::OpenDyslexic;
            break;
        default:
            throw ReaderError(ReaderErrorCode::InvalidProfile,
                              "Select a supported reader font");
        }
        break;
    case kThemeControlId:
        if (notificationCode != CBN_SELCHANGE) {
            return;
        }
        switch (SendMessageW(themeCombo_, CB_GETCURSEL, 0, 0)) {
        case 0:
            updated.theme = ThemeId::WarmCream;
            break;
        case 1:
            updated.theme = ThemeId::Sepia;
            break;
        case 2:
            updated.theme = ThemeId::DarkHighContrast;
            break;
        default:
            throw ReaderError(ReaderErrorCode::InvalidProfile,
                              "Select a supported reader theme");
        }
        break;
    case kFontSizeDecreaseControlId:
    case kFontSizeIncreaseControlId:
        if (notificationCode != BN_CLICKED) {
            return;
        }
        updated.fontSizePt +=
            controlId == kFontSizeIncreaseControlId ? 2.0F : -2.0F;
        updated.fontSizePt = std::clamp(updated.fontSizePt, 14.0F, 48.0F);
        break;
    case kBionicReadingControlId:
        if (notificationCode != BN_CLICKED) {
            return;
        }
        updated.aids.bionicReading =
            SendMessageW(bionicReadingCheckBox_, BM_GETCHECK, 0, 0) ==
            BST_CHECKED;
        break;
    case kReadingRulerControlId:
        if (notificationCode != BN_CLICKED) {
            return;
        }
        updated.aids.readingRuler =
            SendMessageW(readingRulerCheckBox_, BM_GETCHECK, 0, 0) ==
            BST_CHECKED;
        break;
    case kBimodalHighlightingControlId:
        if (notificationCode != BN_CLICKED) {
            return;
        }
        updated.aids.bimodalHighlighting =
            SendMessageW(bimodalHighlightingCheckBox_, BM_GETCHECK, 0, 0) ==
            BST_CHECKED;
        break;
    case kSpeechVoiceControlId:
        return;
    case kSpeechPlayControlId:
    case kSpeechStopControlId:
        if (notificationCode != BN_CLICKED) {
            return;
        }
        handle_speech_command(controlId);
        return;
    default:
        return;
    }
    set_profile(updated);
}

void Direct2DTextRenderer::handle_horizontal_scroll(HWND control) {
    if (control == speechRateTrackbar_) {
        speechRate_ =
            static_cast<float>(SendMessageW(control, TBM_GETPOS, 0, 0)) /
            100.0F;
        if (speechAvailable_ && speechEngine_ != nullptr) {
            try {
                speechEngine_->set_rate(speechRate_);
            } catch (const ReaderError& error) {
                MessageBoxA(hwnd_, error.what(), "Speech speed",
                            MB_OK | MB_ICONWARNING);
            }
        }
        sync_toolbar_controls();
        return;
    }

    ReadingProfile updated = profile_;
    if (control == letterSpacingTrackbar_) {
        updated.letterSpacingEm =
            static_cast<float>(SendMessageW(control, TBM_GETPOS, 0, 0)) /
            100.0F;
    } else if (control == lineHeightTrackbar_) {
        updated.lineHeight =
            static_cast<float>(SendMessageW(control, TBM_GETPOS, 0, 0)) /
            10.0F;
    } else {
        return;
    }
    set_profile(updated);
}

void Direct2DTextRenderer::handle_speech_command(int controlId) {
    if (speechEngine_ == nullptr || !speechAvailable_) {
        return;
    }
    try {
        if (controlId == kSpeechStopControlId) {
            ++speechSession_;
            speechEngine_->stop();
            speechState_ = PlaybackState::Stopped;
            highlights_ = {};
            refresh_highlight_visuals();
            sync_toolbar_controls();
            return;
        }

        if (speechState_ == PlaybackState::Playing) {
            speechEngine_->pause();
            speechState_ = PlaybackState::Paused;
            sync_toolbar_controls();
            return;
        }
        if (speechState_ == PlaybackState::Paused) {
            speechEngine_->resume();
            speechState_ = PlaybackState::Playing;
            sync_toolbar_controls();
            return;
        }

        const LRESULT selected =
            SendMessageW(speechVoiceCombo_, CB_GETCURSEL, 0, 0);
        if (selected == CB_ERR) {
            throw ReaderError(ReaderErrorCode::SpeechUnavailable,
                              "Select a local speech voice before playback");
        }
        const auto voiceIndex = static_cast<std::size_t>(
            SendMessageW(speechVoiceCombo_, CB_GETITEMDATA,
                         static_cast<WPARAM>(selected), 0));
        if (voiceIndex >= speechVoices_.size()) {
            throw ReaderError(ReaderErrorCode::SpeechUnavailable,
                              "The selected local speech voice is invalid");
        }

        SpeechRequest request;
        request.text = document_.text;
        request.voiceId = speechVoices_[voiceIndex].id;
        request.rate = speechRate_;
        speechState_ = PlaybackState::Loading;
        highlights_ = {};
        highlights_.wordTimingReliable = speechCapabilities_.wordTiming;
        highlights_.sentenceTimingReliable =
            speechCapabilities_.sentenceTiming;
        refresh_highlight_visuals();
        sync_toolbar_controls();

        const auto session = ++speechSession_;
        const std::weak_ptr<SpeechDispatchState> dispatch = speechDispatch_;
        speechEngine_->start(
            request, [dispatch, session](const SpeechEvent& event) {
                const auto state = dispatch.lock();
                if (!state) {
                    return;
                }
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->window == nullptr) {
                    return;
                }
                state->pendingEvents.push_back({session, event});
                if (!PostMessageW(state->window, kSpeechEventMessage, 0, 0)) {
                    state->pendingEvents.clear();
                    OutputDebugStringA(
                        "Could not post a Windows speech event to the reader window\n");
                }
            });
    } catch (const ReaderError& error) {
        speechState_ = PlaybackState::Failed;
        highlights_ = {};
        refresh_highlight_visuals();
        sync_toolbar_controls();
        MessageBoxA(hwnd_, error.what(), "Speech playback",
                    MB_OK | MB_ICONWARNING);
    }
}

void Direct2DTextRenderer::handle_speech_event(const SpeechEvent& event) {
    switch (event.kind) {
    case SpeechEventKind::Started:
    case SpeechEventKind::Resumed:
        speechState_ = PlaybackState::Playing;
        break;
    case SpeechEventKind::Paused:
        speechState_ = PlaybackState::Paused;
        break;
    case SpeechEventKind::Stopped:
    case SpeechEventKind::Completed:
        speechState_ = event.kind == SpeechEventKind::Completed
                           ? PlaybackState::Completed
                           : PlaybackState::Stopped;
        highlights_ = {};
        break;
    case SpeechEventKind::MarkReached:
        if (event.mark) {
            const auto& range = event.mark->text;
            const bool validRange =
                range.length > 0 && range.start < document_.text.size() &&
                range.length <= document_.text.size() - range.start;
            if (!validRange) {
                if (event.mark->kind == SpeechMarkKind::Word) {
                    highlights_.word.reset();
                } else {
                    highlights_.sentence.reset();
                }
                OutputDebugStringA(
                    "Ignored an invalid speech timing range\n");
                break;
            }
            if (event.mark->kind == SpeechMarkKind::Word &&
                highlights_.wordTimingReliable) {
                highlights_.word = range;
            } else if (event.mark->kind == SpeechMarkKind::Sentence &&
                       highlights_.sentenceTimingReliable) {
                highlights_.sentence = range;
            }
        }
        break;
    case SpeechEventKind::Failed:
        speechState_ = PlaybackState::Failed;
        highlights_ = {};
        MessageBoxA(hwnd_,
                    event.message.empty() ? "Windows speech playback failed"
                                          : event.message.c_str(),
                    "Speech playback", MB_OK | MB_ICONERROR);
        break;
    }
    refresh_highlight_visuals();
    sync_toolbar_controls();
}

AidVisuals Direct2DTextRenderer::compose_bimodal_highlights() const {
    AidVisuals visuals;
    if (!profile_.aids.bimodalHighlighting) {
        return visuals;
    }

    TextLayoutSnapshot layout;
    if (highlights_.sentenceTimingReliable && highlights_.sentence) {
        layout.sentences.push_back(
            {*highlights_.sentence, text_range_bounds(*highlights_.sentence)});
    }
    if (highlights_.wordTimingReliable && highlights_.word) {
        layout.words.push_back(
            {*highlights_.word, text_range_bounds(*highlights_.word)});
    }
    ReadingAidContext context{document_, profile_, layout, highlights_,
                              activeVisualLine_};
    return bimodalHighlightingAid_.compose(context);
}

void Direct2DTextRenderer::refresh_aid_visuals() {
    const auto layout = line_layout_snapshot();
    if (!profile_.aids.readingRuler || layout.lines.empty()) {
        activeVisualLine_.reset();
    } else {
        std::size_t selected = 0;
        if (rulerAnchorOffset_.has_value()) {
            for (std::size_t index = 0; index < layout.lines.size(); ++index) {
                const auto& range = layout.lines[index].text;
                const TextOffset end = range.start + range.length;
                if (*rulerAnchorOffset_ >= range.start &&
                    *rulerAnchorOffset_ <= end) {
                    selected = index;
                    break;
                }
                if (range.start < *rulerAnchorOffset_) {
                    selected = index;
                } else if (range.start > *rulerAnchorOffset_) {
                    break;
                }
            }
        } else if (activeVisualLine_.has_value()) {
            selected = std::min<std::size_t>(
                *activeVisualLine_, layout.lines.size() - 1);
        }
        activeVisualLine_ = static_cast<std::uint32_t>(selected);
        rulerAnchorOffset_ = layout.lines[selected].text.start;

        if (viewport_.size.height > 0) {
            const auto& bounds = layout.lines[selected].bounds;
            const float visibleTop =
                kToolbarHeightDip + kTextPaddingDip;
            const float visibleBottom =
                kToolbarHeightDip + viewport_.size.height -
                kTextPaddingDip;
            if (bounds.y - viewport_.scrollY < visibleTop) {
                viewport_.scrollY = std::max(0.0F, bounds.y - visibleTop);
            } else if (bounds.y + bounds.height - viewport_.scrollY >
                       visibleBottom) {
                viewport_.scrollY =
                    std::max(0.0F, bounds.y + bounds.height - visibleBottom);
            }
        }
    }

    ReadingAidContext context{document_, profile_, layout, highlights_,
                              activeVisualLine_};
    AidVisuals combined = bionicReadingAid_.compose(context);
    AidVisuals ruler = readingRulerAid_.compose(context);
    AidVisuals bimodal = compose_bimodal_highlights();
    combined.overlays.insert(combined.overlays.end(),
                             bimodal.overlays.begin(),
                             bimodal.overlays.end());
    combined.overlays.insert(combined.overlays.end(), ruler.overlays.begin(),
                             ruler.overlays.end());
    set_aid_visuals(combined);
}

void Direct2DTextRenderer::refresh_highlight_visuals() {
    auto& overlays = aidVisuals_.overlays;
    overlays.erase(
        std::remove_if(overlays.begin(), overlays.end(),
                       [](const AidOverlay& overlay) {
                           return overlay.kind ==
                                      OverlayKind::SentenceHighlight ||
                                  overlay.kind == OverlayKind::WordHighlight;
                       }),
        overlays.end());
    AidVisuals bimodal = compose_bimodal_highlights();
    overlays.insert(overlays.end(), bimodal.overlays.begin(),
                    bimodal.overlays.end());
    invalidate();
}

void Direct2DTextRenderer::select_ruler_line_at(int pixelY) {
    if (!profile_.aids.readingRuler ||
        pixelY < static_cast<int>(scale_dip(kToolbarHeightDip, dpiY_))) {
        return;
    }
    const auto layout = line_layout_snapshot();
    if (layout.lines.empty()) {
        return;
    }

    const float yDip =
        static_cast<float>(pixelY) * 96.0F /
            static_cast<float>(dpiY_ == 0 ? USER_DEFAULT_SCREEN_DPI : dpiY_) +
        viewport_.scrollY;
    std::size_t nearest = 0;
    float nearestDistance = std::numeric_limits<float>::max();
    for (std::size_t index = 0; index < layout.lines.size(); ++index) {
        const auto& bounds = layout.lines[index].bounds;
        const float bottom = bounds.y + bounds.height;
        const float distance =
            yDip < bounds.y ? bounds.y - yDip
                            : yDip > bottom ? yDip - bottom : 0.0F;
        if (distance < nearestDistance) {
            nearest = index;
            nearestDistance = distance;
        }
    }
    activeVisualLine_ = static_cast<std::uint32_t>(nearest);
    rulerAnchorOffset_ = layout.lines[nearest].text.start;
    refresh_aid_visuals();
}

void Direct2DTextRenderer::move_ruler_line(int direction) {
    if (!profile_.aids.readingRuler) {
        return;
    }
    const auto layout = line_layout_snapshot();
    if (layout.lines.empty()) {
        return;
    }
    std::size_t selected = activeVisualLine_.value_or(0);
    if (direction < 0 && selected > 0) {
        --selected;
    } else if (direction > 0 && selected + 1 < layout.lines.size()) {
        ++selected;
    }
    activeVisualLine_ = static_cast<std::uint32_t>(selected);
    rulerAnchorOffset_ = layout.lines[selected].text.start;
    refresh_aid_visuals();
}

bool Direct2DTextRenderer::handle_ruler_key(WPARAM key) {
    if (!profile_.aids.readingRuler ||
        (GetKeyState(VK_MENU) & 0x8000) == 0) {
        return false;
    }
    if (key == VK_UP) {
        move_ruler_line(-1);
        return true;
    }
    if (key == VK_DOWN) {
        move_ruler_line(1);
        return true;
    }
    return false;
}

void Direct2DTextRenderer::ensure_render_target() {
    if (renderTarget_ != nullptr) {
        return;
    }

    RECT client{};
    if (!GetClientRect(hwnd_, &client)) {
        throw ReaderError(ReaderErrorCode::RenderingFailed,
                          "Could not determine reader client dimensions");
    }

    D2D1_RENDER_TARGET_PROPERTIES properties =
        D2D1::RenderTargetProperties();
    properties.dpiX = static_cast<float>(dpiX_);
    properties.dpiY = static_cast<float>(dpiY_);

    check_hresult(
        d2dFactory_->CreateHwndRenderTarget(
            properties,
            D2D1::HwndRenderTargetProperties(
                hwnd_,
                D2D1::SizeU(
                    static_cast<UINT>(client.right - client.left),
                    static_cast<UINT>(client.bottom - client.top))),
            renderTarget_.GetAddressOf()),
        "ID2D1Factory::CreateHwndRenderTarget");
    check_hresult(renderTarget_->CreateSolidColorBrush(
                      D2D1::ColorF(D2D1::ColorF::Black),
                      textBrush_.GetAddressOf()),
                  "ID2D1RenderTarget::CreateSolidColorBrush");
    check_hresult(renderTarget_->CreateSolidColorBrush(
                      D2D1::ColorF(D2D1::ColorF::Black),
                      rulerBrush_.GetAddressOf()),
                  "ID2D1RenderTarget::CreateSolidColorBrush");
}

void Direct2DTextRenderer::rebuild_text_layout() {
    if (dwriteFactory_ == nullptr) {
        return;
    }
    textLayout1_.Reset();
    textLayout_.Reset();
    textFormat_.Reset();

    const float fontSize = dip_from_points(profile_.fontSizePt);
    check_hresult(dwriteFactory_->CreateTextFormat(
                      font_family_label(profile_.fontFamily),
                      fontCollection_->get(),
                      DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                      DWRITE_FONT_STRETCH_NORMAL, fontSize, L"en-us",
                      textFormat_.GetAddressOf()),
                  "IDWriteFactory::CreateTextFormat");
    check_hresult(textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING),
                  "IDWriteTextFormat::SetTextAlignment");
    check_hresult(
        textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR),
        "IDWriteTextFormat::SetParagraphAlignment");
    check_hresult(textFormat_->SetLineSpacing(
                      DWRITE_LINE_SPACING_METHOD_UNIFORM,
                      fontSize * profile_.lineHeight, fontSize * 0.8F),
                  "IDWriteTextFormat::SetLineSpacing");

    const float width =
        std::max(1.0F, viewport_.size.width - 2 * kTextPaddingDip);
    constexpr float layoutHeight = 1'000'000.0F;
    if (document_.text.size() >
        static_cast<std::size_t>(std::numeric_limits<UINT32>::max())) {
        throw ReaderError(ReaderErrorCode::RenderingFailed,
                          "Reading text exceeds DirectWrite's supported length");
    }
    static_assert(sizeof(char16_t) == sizeof(WCHAR),
                  "Windows DirectWrite requires 16-bit UTF-16 code units");
    check_hresult(dwriteFactory_->CreateTextLayout(
                      reinterpret_cast<const WCHAR*>(document_.text.data()),
                      static_cast<UINT32>(document_.text.size()),
                      textFormat_.Get(), width, layoutHeight,
                      textLayout_.GetAddressOf()),
                  "IDWriteFactory::CreateTextLayout");
    check_hresult(textLayout_.As(&textLayout1_),
                  "QueryInterface(IDWriteTextLayout1)");
    if (!document_.text.empty() && profile_.letterSpacingEm > 0) {
        const float spacing = fontSize * profile_.letterSpacingEm;
        check_hresult(textLayout1_->SetCharacterSpacing(
                          spacing, 0, 0,
                          DWRITE_TEXT_RANGE{
                              0, static_cast<UINT32>(document_.text.size())}),
                      "IDWriteTextLayout1::SetCharacterSpacing");
    }
}

void Direct2DTextRenderer::discard_device_resources() noexcept {
    textBrush_.Reset();
    rulerBrush_.Reset();
    renderTarget_.Reset();
}

void Direct2DTextRenderer::paint() {
    if (hwnd_ == nullptr) {
        return;
    }
    try {
        ensure_render_target();
        textBrush_->SetColor(color_from_hex(theme_tokens(profile_.theme).text));
        renderTarget_->BeginDraw();
        renderTarget_->Clear(
            color_from_hex(theme_tokens(profile_.theme).background));
        renderTarget_->SetTransform(
            D2D1::Matrix3x2F::Translation(0, -viewport_.scrollY));
        const auto drawRulerOverlays = [this](bool behindText) {
            for (const auto& overlay : aidVisuals_.overlays) {
                if (overlay.kind != OverlayKind::ReadingRuler) {
                    continue;
                }
                const OverlayLayer layer =
                    behindText ? OverlayLayer::UnderText
                               : OverlayLayer::OverText;
                if (overlay.layer != layer) {
                    continue;
                }
                rulerBrush_->SetColor(color_from_rgba(overlay.rgba));
                rulerBrush_->SetOpacity(overlay.opacity);
                renderTarget_->FillRectangle(
                    D2D1::RectF(overlay.bounds.x, overlay.bounds.y,
                                overlay.bounds.x + overlay.bounds.width,
                                overlay.bounds.y + overlay.bounds.height),
                    rulerBrush_.Get());
            }
            rulerBrush_->SetOpacity(1.0F);
        };
        const auto drawSentenceHighlights = [this]() {
            for (const auto& overlay : aidVisuals_.overlays) {
                if (overlay.kind != OverlayKind::SentenceHighlight ||
                    overlay.layer != OverlayLayer::UnderText) {
                    continue;
                }
                rulerBrush_->SetColor(color_from_rgba(overlay.rgba));
                rulerBrush_->SetOpacity(overlay.opacity);
                renderTarget_->FillRectangle(
                    D2D1::RectF(overlay.bounds.x, overlay.bounds.y,
                                overlay.bounds.x + overlay.bounds.width,
                                overlay.bounds.y + overlay.bounds.height),
                    rulerBrush_.Get());
            }
            rulerBrush_->SetOpacity(1.0F);
        };
        const auto drawWordHighlights = [this]() {
            for (const auto& overlay : aidVisuals_.overlays) {
                if (overlay.kind != OverlayKind::WordHighlight ||
                    overlay.layer != OverlayLayer::OverText) {
                    continue;
                }
                rulerBrush_->SetColor(color_from_rgba(overlay.rgba));
                rulerBrush_->SetOpacity(overlay.opacity);
                D2D1_ROUNDED_RECT bounds{};
                bounds.rect = D2D1::RectF(
                    overlay.bounds.x, overlay.bounds.y,
                    overlay.bounds.x + overlay.bounds.width,
                    overlay.bounds.y + overlay.bounds.height);
                bounds.radiusX = 3.0F;
                bounds.radiusY = 3.0F;
                renderTarget_->DrawRoundedRectangle(
                    &bounds, rulerBrush_.Get(), 2.0F);
            }
            rulerBrush_->SetOpacity(1.0F);
        };
        drawRulerOverlays(true);
        drawSentenceHighlights();
        if (textLayout_ != nullptr && textBrush_ != nullptr) {
            renderTarget_->DrawTextLayout(
                D2D1::Point2F(kTextPaddingDip,
                              kToolbarHeightDip + kTextPaddingDip),
                textLayout_.Get(), textBrush_.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        drawRulerOverlays(false);
        drawWordHighlights();
        renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());
        const HRESULT result = renderTarget_->EndDraw();
        if (result == D2DERR_RECREATE_TARGET) {
            discard_device_resources();
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else {
            check_hresult(result, "ID2D1RenderTarget::EndDraw");
        }
    } catch (const ReaderError& error) {
        discard_device_resources();
        OutputDebugStringA("Dyslexia Reader paint error: ");
        OutputDebugStringA(error.what());
        OutputDebugStringA("\n");
    }
}

void Direct2DTextRenderer::resize_viewport() {
    if (hwnd_ == nullptr) {
        return;
    }
    RECT client{};
    if (!GetClientRect(hwnd_, &client)) {
        return;
    }
    const UINT effectiveDpiX =
        dpiX_ == 0 ? USER_DEFAULT_SCREEN_DPI : dpiX_;
    const UINT effectiveDpiY =
        dpiY_ == 0 ? USER_DEFAULT_SCREEN_DPI : dpiY_;
    const float pixelsToDipsX = 96.0F / static_cast<float>(effectiveDpiX);
    const float pixelsToDipsY = 96.0F / static_cast<float>(effectiveDpiY);
    viewport_.size = {
        static_cast<float>(client.right - client.left) * pixelsToDipsX,
        std::max(0.0F,
                 static_cast<float>(client.bottom - client.top) *
                         pixelsToDipsY -
                     kToolbarHeightDip)};
}

void Direct2DTextRenderer::require_ui_thread() const {
    if (uiThreadId_ != 0 && GetCurrentThreadId() != uiThreadId_) {
        throw ReaderError(ReaderErrorCode::RenderingFailed,
                          "Reader renderer methods must run on its UI thread");
    }
}

}  // namespace dyslexia_reader::windows
