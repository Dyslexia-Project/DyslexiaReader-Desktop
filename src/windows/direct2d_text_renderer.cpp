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

COLORREF colorref_from_hex(std::string_view value) {
    if (value.size() != 7 || value.front() != '#') {
        return RGB(250, 246, 238);
    }
    const auto channel = [&value](std::size_t offset) -> BYTE {
        return static_cast<BYTE>(hex_digit(value[offset]) * 16U +
                                 hex_digit(value[offset + 1]));
    };
    return RGB(channel(1), channel(3), channel(5));
}

void try_set_window_theme(HWND target, const wchar_t* appName,
                          const wchar_t* subIdList) {
    if (target == nullptr) {
        return;
    }
    using SetWindowThemeFn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
    static const SetWindowThemeFn setWindowTheme = []() -> SetWindowThemeFn {
        HMODULE uxtheme = LoadLibraryW(L"uxtheme.dll");
        if (uxtheme == nullptr) {
            return nullptr;
        }
        return reinterpret_cast<SetWindowThemeFn>(
            GetProcAddress(uxtheme, "SetWindowTheme"));
    }();
    if (setWindowTheme != nullptr) {
        setWindowTheme(target, appName, subIdList);
    }
}

void try_set_immersive_dark_titlebar(HWND hwnd, bool enabled) {
    if (hwnd == nullptr) {
        return;
    }
    using DwmSetAttrFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    static const DwmSetAttrFn dwmSetAttr = []() -> DwmSetAttrFn {
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        if (dwm == nullptr) {
            return nullptr;
        }
        return reinterpret_cast<DwmSetAttrFn>(
            GetProcAddress(dwm, "DwmSetWindowAttribute"));
    }();
    if (dwmSetAttr != nullptr) {
        constexpr DWORD kDwmwaUseImmersiveDarkMode = 20;
        const BOOL value = enabled ? TRUE : FALSE;
        dwmSetAttr(hwnd, kDwmwaUseImmersiveDarkMode, &value, sizeof(value));
    }
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
    try {
        for (const auto& asset : fontLoader.load_fonts()) {
            if (asset.family == FontFamily::AtkinsonHyperlegible &&
                !asset.bytes.empty()) {
                DWORD count = 0;
                HANDLE handle = AddFontMemResourceEx(
                    const_cast<std::uint8_t*>(asset.bytes.data()),
                    static_cast<DWORD>(asset.bytes.size()), nullptr, &count);
                if (handle != nullptr) {
                    gdiUiFontResource_ = handle;
                    break;
                }
            }
        }
    } catch (...) {
        gdiUiFontResource_ = nullptr;
    }
}

Direct2DTextRenderer::~Direct2DTextRenderer() {
    textLayout1_.Reset();
    textLayout_.Reset();
    textFormat_.Reset();
    rulerBrush_.Reset();
    fontCollection_.reset();
    if (gdiUiFontResource_ != nullptr) {
        RemoveFontMemResourceEx(gdiUiFontResource_);
        gdiUiFontResource_ = nullptr;
    }
    if (hwnd_ != nullptr && IsWindow(hwnd_)) {
        DestroyWindow(hwnd_);
    }
    destroy_theme_resources();
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

    WNDCLASSEXW canvasClass{};
    canvasClass.cbSize = sizeof(canvasClass);
    canvasClass.style = CS_DBLCLKS;
    canvasClass.hInstance = instance_;
    canvasClass.lpfnWndProc = &Direct2DTextRenderer::canvas_window_proc;
    canvasClass.lpszClassName = kCanvasClassName;
    canvasClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    canvasClass.hbrBackground = nullptr;

    SetLastError(ERROR_SUCCESS);
    if (RegisterClassExW(&canvasClass) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        throw ReaderError(
            ReaderErrorCode::WindowCreationFailed,
            "Could not register the Direct2D canvas child window class");
    }

    RECT workArea{0, 0, 1280, 800};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    UINT systemDpi = GetDpiForSystem();
    if (systemDpi == 0) {
        systemDpi = USER_DEFAULT_SCREEN_DPI;
    }
    const int workW = std::max<int>(400, workArea.right - workArea.left);
    const int workH = std::max<int>(320, workArea.bottom - workArea.top);
    const int desiredW = std::clamp(static_cast<int>(scale_dip(960, systemDpi)),
                                    std::min(workW, 640), workW - 32);
    const int desiredH = std::clamp(static_cast<int>(scale_dip(640, systemDpi)),
                                    std::min(workH, 400), workH - 32);
    const int posX = workArea.left + (workW - desiredW) / 2;
    const int posY = workArea.top + (workH - desiredH) / 2;

    hwnd_ = CreateWindowExW(
        WS_EX_TOPMOST, kWindowClassName, L"Dyslexia Reader",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, posX, posY,
        desiredW, desiredH, owner, nullptr, instance_, this);
    if (hwnd_ == nullptr) {
        throw ReaderError(ReaderErrorCode::WindowCreationFailed,
                          "Could not create the reader window");
    }
    if (mode_ == ReaderMode::Focus) {
        apply_window_mode();
    }
    return hwnd_;
}

void Direct2DTextRenderer::commit_ui_profile(const ReadingProfile& nextProfile) {
    set_profile(nextProfile);
    if (onProfileChanged_) {
        onProfileChanged_(profile_);
    }
}

bool Direct2DTextRenderer::is_visible() const noexcept {
    return hwnd_ != nullptr && IsWindowVisible(hwnd_) != FALSE;
}

std::optional<TextRange> Direct2DTextRenderer::selected_word() const noexcept {
    if (!selectedWordIndex_.has_value() ||
        *selectedWordIndex_ >= document_.words.size()) {
        return std::nullopt;
    }
    return document_.words[*selectedWordIndex_].text;
}

std::pair<float, float> Direct2DTextRenderer::text_column_metrics() const {
    const float availableWidth =
        std::max(1.0F, viewport_.size.width - 2.0F * kTextPaddingDip);
    const float fontPx = dip_from_points(profile_.fontSizePt);
    const float chAdvance = fontPx * 0.55F;
    const float targetWidth =
        chAdvance * static_cast<float>(profile_.lineWidthCh);
    const float minReasonable = std::min(availableWidth, 240.0F);
    const float columnWidth =
        std::clamp(targetWidth, minReasonable, availableWidth);
    const float originX = kTextPaddingDip +
                          std::max(0.0F, (availableWidth - columnWidth) * 0.5F);
    return {originX, columnWidth};
}

float Direct2DTextRenderer::content_height_dip() const {
    if (textLayout_ == nullptr) {
        return 0.0F;
    }
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(textLayout_->GetMetrics(&metrics))) {
        return 0.0F;
    }
    return metrics.height;
}

float Direct2DTextRenderer::max_scroll_y() const {
    const float visibleHeight = std::max(1.0F, viewport_.size.height);
    const float totalHeight = content_height_dip() + 2.0F * kTextPaddingDip;
    return std::max(0.0F, totalHeight - visibleHeight);
}

void Direct2DTextRenderer::sync_scrollbar() const {
    if (canvasHwnd_ == nullptr) {
        return;
    }
    const float visibleHeight = std::max(1.0F, viewport_.size.height);
    const float totalHeight = content_height_dip() + 2.0F * kTextPaddingDip;
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL | SIF_DISABLENOSCROLL;
    si.nMin = 0;
    si.nPage = static_cast<UINT>(std::max(1L, std::lround(visibleHeight)));
    si.nMax = static_cast<int>(
        std::max<long>(static_cast<long>(si.nPage) - 1,
                       std::lround(totalHeight) - 1));
    si.nPos = static_cast<int>(
        std::clamp<long>(std::lround(viewport_.scrollY), 0,
                         std::max(0L, static_cast<long>(std::lround(max_scroll_y())))));
    SetScrollInfo(canvasHwnd_, SB_VERT, &si, TRUE);
}

void Direct2DTextRenderer::set_scroll_y(float scrollY) {
    const float clamped = std::clamp(scrollY, 0.0F, max_scroll_y());
    if (std::abs(clamped - viewport_.scrollY) < 0.25F && clamped == viewport_.scrollY) {
        return;
    }
    viewport_.scrollY = clamped;
    sync_scrollbar();
    invalidate();
}

void Direct2DTextRenderer::scroll_by_delta(float deltaDip) {
    set_scroll_y(viewport_.scrollY + deltaDip);
}

void Direct2DTextRenderer::update_focus_scroll(TextRange range) {
    const auto bounds = text_range_bounds(range);
    if (bounds.empty()) {
        return;
    }
    const float top = bounds.front().y;
    const float bottom = bounds.back().y + bounds.back().height;
    const float visibleHeight = std::max(1.0F, viewport_.size.height);
    const float viewTop = viewport_.scrollY;
    const float viewBottom = viewTop + visibleHeight - kTextPaddingDip;
    if (top < viewTop) {
        set_scroll_y(std::max(0.0F, top - kTextPaddingDip));
    } else if (bottom > viewBottom) {
        set_scroll_y(bottom - visibleHeight + kTextPaddingDip * 2.0F);
    }
}

void Direct2DTextRenderer::set_mode(ReaderMode mode) {
    require_ui_thread();
    if (mode_ == mode && hwnd_ != nullptr) {
        return;
    }
    if (hwnd_ != nullptr && mode_ == ReaderMode::Floating &&
        mode == ReaderMode::Focus) {
        GetWindowRect(hwnd_, &floatingBounds_);
        hasFloatingBounds_ = true;
    }
    mode_ = mode;
    if (hwnd_ != nullptr) {
        apply_window_mode();
        sync_toolbar_controls();
        rebuild_text_layout();
        refresh_aid_visuals();
        refresh_highlight_visuals();
        invalidate();
    }
    if (onModeChanged_) {
        onModeChanged_(mode_);
    }
}

void Direct2DTextRenderer::apply_window_mode() {
    if (hwnd_ == nullptr) {
        return;
    }
    const DWORD visibleFlag = IsWindowVisible(hwnd_) ? WS_VISIBLE : 0;
    const DWORD baseFlags = WS_CLIPCHILDREN | visibleFlag;
    if (mode_ == ReaderMode::Focus) {
        SetWindowLongPtrW(hwnd_, GWL_STYLE,
                          static_cast<LONG_PTR>(WS_POPUP | baseFlags));
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE,
                          static_cast<LONG_PTR>(WS_EX_TOPMOST));
        HMONITOR monitor =
            MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (monitor != nullptr && GetMonitorInfoW(monitor, &mi)) {
            SetWindowPos(hwnd_, HWND_TOPMOST, mi.rcMonitor.left,
                         mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_FRAMECHANGED | SWP_NOACTIVATE);
        }
    } else {
        SetWindowLongPtrW(
            hwnd_, GWL_STYLE,
            static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW | baseFlags));
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE,
                          static_cast<LONG_PTR>(WS_EX_TOPMOST));
        if (hasFloatingBounds_) {
            SetWindowPos(hwnd_, HWND_TOPMOST, floatingBounds_.left,
                         floatingBounds_.top,
                         floatingBounds_.right - floatingBounds_.left,
                         floatingBounds_.bottom - floatingBounds_.top,
                         SWP_FRAMECHANGED | SWP_NOACTIVATE);
        } else {
            SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED |
                             SWP_NOACTIVATE);
        }
    }
}

void Direct2DTextRenderer::show(int showCommand) {
    require_ui_thread();
    if (hwnd_ == nullptr) {
        throw ReaderError(ReaderErrorCode::WindowCreationFailed,
                          "Reader window has not been created");
    }
    ShowWindow(hwnd_, showCommand);
    UpdateWindow(hwnd_);
    if (canvasHwnd_ != nullptr) {
        UpdateWindow(canvasHwnd_);
    }
}

void Direct2DTextRenderer::set_document(const ReadingDocument& document) {
    require_ui_thread();
    document_ = document;
    viewport_.scrollY = 0.0F;
    highlights_ = {};
    activeVisualLine_.reset();
    rulerAnchorOffset_.reset();
    selectedWordIndex_.reset();
    cursorTargetCenterX_ = -1.0F;
    rebuild_text_layout();
    sync_toolbar_controls();
    refresh_aid_visuals();
    invalidate();
}

void Direct2DTextRenderer::rebuild_ui_font() {
    const UINT effectiveDpi = dpiY_ == 0 ? USER_DEFAULT_SCREEN_DPI : dpiY_;
    const int fontHeight = -MulDiv(14, static_cast<int>(effectiveDpi), 96);
    HFONT nextFont = CreateFontW(
        fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS,
        gdiUiFontResource_ != nullptr ? L"Atkinson Hyperlegible" : L"Segoe UI");
    if (nextFont == nullptr) {
        return;
    }
    if (hwnd_ != nullptr) {
        EnumChildWindows(
            hwnd_,
            [](HWND child, LPARAM fontParam) -> BOOL {
                SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(fontParam),
                             FALSE);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(nextFont));
    }
    if (uiFont_ != nullptr) {
        DeleteObject(uiFont_);
    }
    uiFont_ = nextFont;
}

void Direct2DTextRenderer::update_theme_resources() {
    if (!hasThemedBrush_ || themedBrushId_ != profile_.theme) {
        if (toolbarBrush_ != nullptr) {
            DeleteObject(toolbarBrush_);
            toolbarBrush_ = nullptr;
        }
        toolbarBrush_ = CreateSolidBrush(
            colorref_from_hex(theme_tokens(profile_.theme).surface));
        themedBrushId_ = profile_.theme;
        hasThemedBrush_ = true;
    }
    if (hwnd_ == nullptr) {
        return;
    }
    if (hasAppliedWindowTheme_ && appliedWindowThemeId_ == profile_.theme) {
        return;
    }

    const bool themeChanged = hasAppliedWindowTheme_;
    const bool isDark = profile_.theme == ThemeId::DarkHighContrast;
    try_set_immersive_dark_titlebar(hwnd_, isDark);
    try_set_window_theme(hwnd_, isDark ? L"DarkMode_Explorer" : L"Explorer",
                         nullptr);
    if (canvasHwnd_ != nullptr) {
        try_set_window_theme(
            canvasHwnd_, isDark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    }

    for (HWND combo : {fontFamilyCombo_, themeCombo_, speechVoiceCombo_}) {
        try_set_window_theme(combo, isDark ? L"DarkMode_CFD" : L"Explorer",
                             nullptr);
    }
    for (HWND chk : {bionicReadingCheckBox_, readingRulerCheckBox_,
                     bimodalHighlightingCheckBox_}) {
        try_set_window_theme(chk, isDark ? L"" : L"Explorer",
                             isDark ? L"" : nullptr);
    }
    for (HWND slider : {letterSpacingTrackbar_, lineHeightTrackbar_,
                        lineWidthTrackbar_, speechRateTrackbar_}) {
        try_set_window_theme(slider, L"Explorer", nullptr);
    }

    hasAppliedWindowTheme_ = true;
    appliedWindowThemeId_ = profile_.theme;

    if (themeChanged) {
        EnumChildWindows(
            hwnd_,
            [](HWND child, LPARAM canvasParam) -> BOOL {
                if (child != reinterpret_cast<HWND>(canvasParam)) {
                    InvalidateRect(child, nullptr, TRUE);
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(canvasHwnd_));
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void Direct2DTextRenderer::destroy_theme_resources() noexcept {
    if (uiFont_ != nullptr) {
        DeleteObject(uiFont_);
        uiFont_ = nullptr;
    }
    if (toolbarBrush_ != nullptr) {
        DeleteObject(toolbarBrush_);
        toolbarBrush_ = nullptr;
    }
    hasThemedBrush_ = false;
    hasAppliedWindowTheme_ = false;
}

void Direct2DTextRenderer::set_profile(const ReadingProfile& profile) {
    require_ui_thread();
    validate_profile(profile);
    profile_ = profile;
    update_theme_resources();
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
            const auto [colOriginX, colWidth] = text_column_metrics();
            snapshot.lines.push_back(
                {{textStart, visibleLength},
                 {colOriginX, kTextPaddingDip + y,
                  colWidth,
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
        range.start, range.length, text_column_metrics().first,
        kTextPaddingDip - viewport_.scrollY, nullptr, 0,
        &hitCount);
    if (countResult != E_NOT_SUFFICIENT_BUFFER && FAILED(countResult)) {
        check_hresult(countResult, "IDWriteTextLayout::HitTestTextRange");
    }
    if (hitCount == 0) {
        return bounds;
    }

    std::vector<DWRITE_HIT_TEST_METRICS> hits(hitCount);
    check_hresult(textLayout_->HitTestTextRange(
                      range.start, range.length, text_column_metrics().first,
                      kTextPaddingDip - viewport_.scrollY,
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
    if (canvasHwnd_ != nullptr) {
        InvalidateRect(canvasHwnd_, nullptr, FALSE);
    } else if (hwnd_ != nullptr) {
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
    if (message == WM_MOUSEWHEEL && renderer != nullptr && renderer->hwnd_ != nullptr) {
        if (SendMessageW(control, CB_GETDROPPEDSTATE, 0, 0) == 0) {
            SendMessageW(renderer->hwnd_, WM_MOUSEWHEEL, wParam, lParam);
            return 0;
        }
    }
    if (message == WM_KEYDOWN && renderer != nullptr && renderer->hwnd_ != nullptr) {
        const bool isFocusEscape =
            (wParam == VK_ESCAPE && renderer->mode() == ReaderMode::Focus);
        const bool isPageOrEdgeKey =
            (wParam == VK_PRIOR || wParam == VK_NEXT || wParam == VK_HOME ||
             wParam == VK_END) &&
            control != renderer->letterSpacingTrackbar_ &&
            control != renderer->lineHeightTrackbar_ &&
            control != renderer->lineWidthTrackbar_ &&
            control != renderer->speechRateTrackbar_ &&
            SendMessageW(control, CB_GETDROPPEDSTATE, 0, 0) == 0;
        if (isFocusEscape || isPageOrEdgeKey) {
            SendMessageW(renderer->hwnd_, WM_KEYDOWN, wParam, lParam);
            return 0;
        }
    }
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

LRESULT CALLBACK Direct2DTextRenderer::canvas_window_proc(HWND window,
                                                          UINT message,
                                                          WPARAM wParam,
                                                          LPARAM lParam) {
    auto* renderer = reinterpret_cast<Direct2DTextRenderer*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        renderer = static_cast<Direct2DTextRenderer*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(renderer));
        renderer->canvasHwnd_ = window;
    }

    if (renderer != nullptr) {
        LRESULT result = 0;
        try {
            result = renderer->handle_canvas_message(message, wParam, lParam);
        } catch (const std::exception& error) {
            log_window_error(error);
            result = message == WM_CREATE ? -1 : 0;
        }
        if (message == WM_NCDESTROY) {
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            renderer->canvasHwnd_ = nullptr;
        }
        return result;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void Direct2DTextRenderer::create_canvas_window() {
    canvasHwnd_ = CreateWindowExW(
        0, kCanvasClassName, L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPSIBLINGS,
        0, 0, 1, 1, hwnd_, nullptr, instance_, this);
    if (canvasHwnd_ == nullptr) {
        throw ReaderError(ReaderErrorCode::WindowCreationFailed,
                          "Could not create the Direct2D canvas window");
    }
}

void Direct2DTextRenderer::layout_canvas_window() {
    if (hwnd_ == nullptr || canvasHwnd_ == nullptr) {
        return;
    }
    RECT client{};
    if (!GetClientRect(hwnd_, &client)) {
        return;
    }
    const int toolbarPx = static_cast<int>(scale_dip(kToolbarHeightDip, dpiY_));
    const int width = std::max(0L, client.right - client.left);
    const int height = std::max(0L, client.bottom - client.top - toolbarPx);
    SetWindowPos(canvasHwnd_, nullptr, 0, toolbarPx, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void Direct2DTextRenderer::draw_toolbar_button(
    const DRAWITEMSTRUCT* dis) const {
    if (dis == nullptr || dis->hDC == nullptr) {
        return;
    }
    HDC hdc = dis->hDC;
    const RECT rc = dis->rcItem;
    const auto& tokens = theme_tokens(profile_.theme);

    const COLORREF surfaceCol = colorref_from_hex(tokens.surface);
    const COLORREF bgCol = colorref_from_hex(tokens.background);
    const COLORREF borderCol = colorref_from_hex(tokens.border);
    const COLORREF textCol = colorref_from_hex(tokens.text);
    const COLORREF focusCol = colorref_from_hex(tokens.focus);
    const COLORREF softTintCol = colorref_from_hex(tokens.sentenceHighlight);

    const auto mix = [](COLORREF a, COLORREF b, float t) -> COLORREF {
        const float c = std::clamp(t, 0.0F, 1.0F);
        const auto ch = [c](int x, int y) -> BYTE {
            return static_cast<BYTE>(std::lround(
                static_cast<float>(x) * (1.0F - c) + static_cast<float>(y) * c));
        };
        return RGB(ch(GetRValue(a), GetRValue(b)),
                   ch(GetGValue(a), GetGValue(b)),
                   ch(GetBValue(a), GetBValue(b)));
    };

    const int id = static_cast<int>(dis->CtlID);
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    const bool focused = (dis->itemState & ODS_FOCUS) != 0;

    const bool isActiveToggle =
        (id == kFocusModeControlId && mode_ == ReaderMode::Focus) ||
        (id == kSpeechPlayControlId && speechState_ == PlaybackState::Playing);
    const bool isPrimary = (id == kCaptureControlId) || isActiveToggle;
    const bool isInsight = (id == kDefineWordControlId ||
                            id == kWordCloudControlId ||
                            id == kPlatformControlId);

    COLORREF fillCol = bgCol;
    COLORREF outlineCol = borderCol;
    COLORREF fgCol = textCol;

    if (disabled) {
        fillCol = surfaceCol;
        outlineCol = mix(surfaceCol, borderCol, 0.5F);
        fgCol = mix(surfaceCol, textCol, 0.42F);
    } else if (isPrimary) {
        fillCol = pressed ? mix(focusCol, textCol, 0.15F) : focusCol;
        outlineCol = fillCol;
        const int lum =
            (299 * GetRValue(fillCol) + 587 * GetGValue(fillCol) +
             114 * GetBValue(fillCol)) /
            1000;
        fgCol = lum > 150 ? RGB(18, 18, 18) : RGB(255, 255, 255);
    } else if (isInsight) {
        fillCol = mix(bgCol, softTintCol, 0.65F);
        if (pressed) {
            fillCol = mix(fillCol, focusCol, 0.2F);
        }
        outlineCol = mix(borderCol, focusCol, 0.45F);
    } else if (pressed) {
        fillCol = mix(bgCol, focusCol, 0.18F);
        outlineCol = focusCol;
    }

    if (focused && !disabled) {
        outlineCol = focusCol;
    }

    HBRUSH surfaceBrush = CreateSolidBrush(surfaceCol);
    if (surfaceBrush != nullptr) {
        FillRect(hdc, &rc, surfaceBrush);
        DeleteObject(surfaceBrush);
    }

    RECT pill = rc;
    InflateRect(&pill, -1, -1);
    HBRUSH fillBrush = CreateSolidBrush(fillCol);
    HPEN borderPen = CreatePen(PS_SOLID, focused ? 2 : 1, outlineCol);
    HGDIOBJ oldBrush = fillBrush != nullptr ? SelectObject(hdc, fillBrush) : nullptr;
    HGDIOBJ oldPen = borderPen != nullptr ? SelectObject(hdc, borderPen) : nullptr;
    RoundRect(hdc, pill.left, pill.top, pill.right, pill.bottom, 8, 8);
    if (oldPen != nullptr) {
        SelectObject(hdc, oldPen);
    }
    if (oldBrush != nullptr) {
        SelectObject(hdc, oldBrush);
    }
    if (borderPen != nullptr) {
        DeleteObject(borderPen);
    }
    if (fillBrush != nullptr) {
        DeleteObject(fillBrush);
    }

    wchar_t label[128]{};
    GetWindowTextW(dis->hwndItem, label, 128);
    HGDIOBJ oldFont = uiFont_ != nullptr ? SelectObject(hdc, uiFont_) : nullptr;
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, fgCol);
    RECT textRc = pill;
    if (pressed && !disabled) {
        OffsetRect(&textRc, 0, 1);
    }
    DrawTextW(hdc, label, -1, &textRc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (oldFont != nullptr) {
        SelectObject(hdc, oldFont);
    }
}

void Direct2DTextRenderer::paint_toolbar(HDC hdc,
                                         const RECT& dirtyRect) const {
    if (hwnd_ == nullptr) {
        return;
    }
    RECT client{};
    if (!GetClientRect(hwnd_, &client)) {
        return;
    }
    const auto& tokens = theme_tokens(profile_.theme);
    if (toolbarBrush_ != nullptr) {
        FillRect(hdc, &dirtyRect, toolbarBrush_);
    } else {
        HBRUSH fallback = CreateSolidBrush(colorref_from_hex(tokens.surface));
        if (fallback != nullptr) {
            FillRect(hdc, &dirtyRect, fallback);
            DeleteObject(fallback);
        }
    }
    const int toolbarPx = static_cast<int>(scale_dip(kToolbarHeightDip, dpiY_));
    if (toolbarPx <= 1) {
        return;
    }
    HPEN borderPen = CreatePen(PS_SOLID, 1, colorref_from_hex(tokens.border));
    if (borderPen == nullptr) {
        return;
    }
    HGDIOBJ oldPen = SelectObject(hdc, borderPen);
    MoveToEx(hdc, 0, toolbarPx - 1, nullptr);
    LineTo(hdc, client.right, toolbarPx - 1);
    const int div1X = static_cast<int>(scale_dip(301, dpiX_));
    MoveToEx(hdc, div1X, static_cast<int>(scale_dip(12, dpiY_)), nullptr);
    LineTo(hdc, div1X, static_cast<int>(scale_dip(36, dpiY_)));
    const int div2X = static_cast<int>(scale_dip(552, dpiX_));
    MoveToEx(hdc, div2X, static_cast<int>(scale_dip(50, dpiY_)), nullptr);
    LineTo(hdc, div2X, static_cast<int>(scale_dip(72, dpiY_)));
    const int div3X = static_cast<int>(scale_dip(392, dpiX_));
    MoveToEx(hdc, div3X, static_cast<int>(scale_dip(88, dpiY_)), nullptr);
    LineTo(hdc, div3X, static_cast<int>(scale_dip(112, dpiY_)));
    const int div4X = static_cast<int>(scale_dip(551, dpiX_));
    MoveToEx(hdc, div4X, static_cast<int>(scale_dip(88, dpiY_)), nullptr);
    LineTo(hdc, div4X, static_cast<int>(scale_dip(112, dpiY_)));
    const int div5X = static_cast<int>(scale_dip(696, dpiX_));
    MoveToEx(hdc, div5X, static_cast<int>(scale_dip(88, dpiY_)), nullptr);
    LineTo(hdc, div5X, static_cast<int>(scale_dip(112, dpiY_)));
    const int rowSep1Y = static_cast<int>(scale_dip(43, dpiY_));
    MoveToEx(hdc, static_cast<int>(scale_dip(12, dpiX_)), rowSep1Y, nullptr);
    LineTo(hdc, client.right - static_cast<int>(scale_dip(12, dpiX_)), rowSep1Y);
    const int rowSep2Y = static_cast<int>(scale_dip(80, dpiY_));
    MoveToEx(hdc, static_cast<int>(scale_dip(12, dpiX_)), rowSep2Y, nullptr);
    LineTo(hdc, client.right - static_cast<int>(scale_dip(12, dpiX_)), rowSep2Y);
    SelectObject(hdc, oldPen);
    DeleteObject(borderPen);
}

void Direct2DTextRenderer::handle_mouse_wheel(WPARAM wParam) {
    const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
    if (delta != 0) {
        const float stepDip =
            dip_from_points(profile_.fontSizePt) * profile_.lineHeight * 2.5F;
        scroll_by_delta(-static_cast<float>(delta) / WHEEL_DELTA * stepDip);
    }
}

bool Direct2DTextRenderer::handle_navigation_key(WPARAM key) {
    const float lineStep =
        dip_from_points(profile_.fontSizePt) * profile_.lineHeight;
    const float pageStep = std::max(lineStep, viewport_.size.height * 0.85F);
    switch (key) {
    case VK_ESCAPE:
        if (mode_ == ReaderMode::Focus) {
            set_mode(ReaderMode::Floating);
            return true;
        }
        if (selectedWordIndex_.has_value()) {
            clear_word_cursor();
            return true;
        }
        return false;
    case VK_PRIOR:
        scroll_by_delta(-pageStep);
        return true;
    case VK_NEXT:
        scroll_by_delta(pageStep);
        return true;
    case VK_HOME:
        set_scroll_y(0.0F);
        return true;
    case VK_END:
        set_scroll_y(max_scroll_y());
        return true;
    case VK_LEFT:
        move_word_cursor(-1);
        return true;
    case VK_RIGHT:
        move_word_cursor(1);
        return true;
    case VK_UP:
        if (selectedWordIndex_.has_value()) {
            move_word_cursor_line(-1);
        } else {
            scroll_by_delta(-lineStep);
        }
        return true;
    case VK_DOWN:
        if (selectedWordIndex_.has_value()) {
            move_word_cursor_line(1);
        } else {
            scroll_by_delta(lineStep);
        }
        return true;
    case VK_RETURN:
        if (selectedWordIndex_.has_value()) {
            speak_selected_word();
            return true;
        }
        return false;
    case 'D':
        if (onDefineWordRequested_ && selectedWordIndex_.has_value()) {
            onDefineWordRequested_();
            return true;
        }
        return false;
    case VK_SPACE:
        if (speechAvailable_ && speechEngine_ != nullptr) {
            handle_speech_command(kSpeechPlayControlId);
            return true;
        }
        return false;
    default:
        return false;
    }
}

LRESULT Direct2DTextRenderer::handle_canvas_message(UINT message, WPARAM wParam,
                                                    LPARAM lParam) {
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paintStruct{};
        BeginPaint(canvasHwnd_, &paintStruct);
        struct PaintScope {
            HWND window;
            PAINTSTRUCT& state;
            ~PaintScope() { EndPaint(window, &state); }
        } paintScope{canvasHwnd_, paintStruct};
        paint();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
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
        if (canvasHwnd_ != nullptr) {
            InvalidateRect(canvasHwnd_, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEWHEEL:
        handle_mouse_wheel(wParam);
        return 0;
    case WM_VSCROLL: {
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL;
        if (!GetScrollInfo(canvasHwnd_, SB_VERT, &si)) {
            return 0;
        }
        const float lineStep =
            dip_from_points(profile_.fontSizePt) * profile_.lineHeight;
        const float pageStep = std::max(lineStep, viewport_.size.height * 0.85F);
        switch (LOWORD(wParam)) {
        case SB_LINEUP:
            scroll_by_delta(-lineStep);
            break;
        case SB_LINEDOWN:
            scroll_by_delta(lineStep);
            break;
        case SB_PAGEUP:
            scroll_by_delta(-pageStep);
            break;
        case SB_PAGEDOWN:
            scroll_by_delta(pageStep);
            break;
        case SB_TOP:
            set_scroll_y(0.0F);
            break;
        case SB_BOTTOM:
            set_scroll_y(max_scroll_y());
            break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION:
            set_scroll_y(static_cast<float>(si.nTrackPos));
            break;
        default:
            break;
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        SetFocus(canvasHwnd_);
        const int clickX = static_cast<int>(static_cast<SHORT>(LOWORD(lParam)));
        const int clickY = static_cast<int>(static_cast<SHORT>(HIWORD(lParam)));
        const bool pickedWord = select_word_at_point(clickX, clickY);
        if (profile_.aids.readingRuler) {
            if (!pickedWord) {
                select_ruler_line_at(clickY);
            }
            SetCapture(canvasHwnd_);
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        SetFocus(canvasHwnd_);
        const int clickX = static_cast<int>(static_cast<SHORT>(LOWORD(lParam)));
        const int clickY = static_cast<int>(static_cast<SHORT>(HIWORD(lParam)));
        if (select_word_at_point(clickX, clickY) && onDefineWordRequested_) {
            onDefineWordRequested_();
        }
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        SetFocus(canvasHwnd_);
        const int clickX = static_cast<int>(static_cast<SHORT>(LOWORD(lParam)));
        const int clickY = static_cast<int>(static_cast<SHORT>(HIWORD(lParam)));
        if (select_word_at_point(clickX, clickY)) {
            speak_selected_word();
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (profile_.aids.readingRuler && GetCapture() == canvasHwnd_ &&
            (wParam & MK_LBUTTON) != 0) {
            select_ruler_line_at(
                static_cast<int>(static_cast<SHORT>(HIWORD(lParam))));
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (GetCapture() == canvasHwnd_) {
            ReleaseCapture();
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (handle_navigation_key(wParam)) {
            return 0;
        }
        break;
    case WM_SYSKEYDOWN:
        if (handle_ruler_key(wParam)) {
            return 0;
        }
        break;
    case WM_NCDESTROY:
        discard_device_resources();
        return DefWindowProcW(canvasHwnd_, message, wParam, lParam);
    default:
        return DefWindowProcW(canvasHwnd_, message, wParam, lParam);
    }
    return DefWindowProcW(canvasHwnd_, message, wParam, lParam);
}

LRESULT Direct2DTextRenderer::handle_message(UINT message, WPARAM wParam,
                                            LPARAM lParam) {
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paintStruct{};
        HDC hdc = BeginPaint(hwnd_, &paintStruct);
        paint_toolbar(hdc, paintStruct.rcPaint);
        EndPaint(hwnd_, &paintStruct);
        return 0;
    }
    case WM_CLOSE:
        if (hideOnClose_) {
            show(SW_HIDE);
            return 0;
        }
        break;
    case WM_MOUSEWHEEL:
        handle_mouse_wheel(wParam);
        return 0;
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
        rebuild_ui_font();
        create_toolbar_controls();
        create_canvas_window();
        update_theme_resources();
        layout_toolbar_controls();
        sync_toolbar_controls();
        resize_viewport();
        rebuild_text_layout();
        refresh_aid_visuals();
        return 0;
    case WM_DRAWITEM: {
        const auto* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (dis != nullptr && dis->CtlType == ODT_BUTTON) {
            draw_toolbar_button(dis);
            return TRUE;
        }
        break;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        const auto& tokens = theme_tokens(profile_.theme);
        SetTextColor(hdc, colorref_from_hex(tokens.text));
        SetBkColor(hdc, colorref_from_hex(tokens.surface));
        if (toolbarBrush_ != nullptr) {
            return reinterpret_cast<LRESULT>(toolbarBrush_);
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    }
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            layout_toolbar_controls();
            resize_viewport();
            rebuild_text_layout();
            refresh_aid_visuals();
            invalidate();
        }
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
        rebuild_ui_font();
        layout_toolbar_controls();
        resize_viewport();
        rebuild_text_layout();
        refresh_aid_visuals();
        invalidate();
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
    case WM_KEYDOWN:
        if (handle_navigation_key(wParam)) {
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    case WM_SYSKEYDOWN:
        if (handle_ruler_key(wParam)) {
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        limits->ptMinTrackSize.x = static_cast<LONG>(scale_dip(900, dpiX_));
        limits->ptMinTrackSize.y = static_cast<LONG>(scale_dip(420, dpiY_));
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
        destroy_theme_resources();
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
        SendMessageW(
            control, WM_SETFONT,
            reinterpret_cast<WPARAM>(uiFont_ != nullptr
                                         ? uiFont_
                                         : GetStockObject(DEFAULT_GUI_FONT)),
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

    captureButton_ =
        create(L"BUTTON", L"Capture Screen", BS_OWNERDRAW | WS_TABSTOP,
               kCaptureControlId);
    clipboardButton_ =
        create(L"BUTTON", L"Paste Text", BS_OWNERDRAW | WS_TABSTOP,
               kClipboardControlId);
    platformButton_ =
        create(L"BUTTON", L"Platform", BS_OWNERDRAW | WS_TABSTOP,
               kPlatformControlId);
    focusModeButton_ =
        create(L"BUTTON", L"Focus Mode", BS_OWNERDRAW | WS_TABSTOP,
               kFocusModeControlId);

    fontFamilyLabel_ = create(L"STATIC", L"Font:", SS_LEFT | SS_CENTERIMAGE, 0);
    fontFamilyCombo_ =
        create(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
               kFontFamilyControlId);
    decreaseFontButton_ =
        create(L"BUTTON", L"-", BS_OWNERDRAW | WS_TABSTOP,
               kFontSizeDecreaseControlId);
    fontSizeLabel_ = create(L"STATIC", L"", SS_CENTER | SS_CENTERIMAGE, 0);
    increaseFontButton_ =
        create(L"BUTTON", L"+", BS_OWNERDRAW | WS_TABSTOP,
               kFontSizeIncreaseControlId);
    themeLabel_ = create(L"STATIC", L"Theme:", SS_LEFT | SS_CENTERIMAGE, 0);
    themeCombo_ =
        create(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
               kThemeControlId);

    letterSpacingLabel_ =
        create(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE, 0);
    letterSpacingTrackbar_ =
        create(TRACKBAR_CLASSW, L"", TBS_NOTICKS | WS_TABSTOP,
               kLetterSpacingControlId);
    lineHeightLabel_ = create(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE, 0);
    lineHeightTrackbar_ =
        create(TRACKBAR_CLASSW, L"", TBS_NOTICKS | WS_TABSTOP,
               kLineHeightControlId);
    lineWidthLabel_ = create(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE, 0);
    lineWidthTrackbar_ =
        create(TRACKBAR_CLASSW, L"", TBS_NOTICKS | WS_TABSTOP,
               kLineWidthControlId);

    bionicReadingCheckBox_ =
        create(L"BUTTON", L"Bionic text", BS_AUTOCHECKBOX | WS_TABSTOP,
               kBionicReadingControlId);
    readingRulerCheckBox_ =
        create(L"BUTTON", L"Reading ruler", BS_AUTOCHECKBOX | WS_TABSTOP,
               kReadingRulerControlId);
    bimodalHighlightingCheckBox_ =
        create(L"BUTTON", L"Word sync", BS_AUTOCHECKBOX | WS_TABSTOP,
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
    SendMessageW(lineWidthTrackbar_, TBM_SETRANGE, TRUE,
                 MAKELONG(35, 100));

    speechVoiceLabel_ =
        create(L"STATIC", L"Voice:", SS_LEFT | SS_CENTERIMAGE, 0);
    speechVoiceCombo_ =
        create(WC_COMBOBOXW, L"",
               CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
               kSpeechVoiceControlId);
    speechPlayButton_ =
        create(L"BUTTON", L"Play", BS_OWNERDRAW | WS_TABSTOP,
               kSpeechPlayControlId);
    speakWordButton_ =
        create(L"BUTTON", L"Speak Word", BS_OWNERDRAW | WS_TABSTOP,
               kSpeakWordControlId);
    speechStopButton_ =
        create(L"BUTTON", L"Stop", BS_OWNERDRAW | WS_TABSTOP,
               kSpeechStopControlId);
    defineWordButton_ =
        create(L"BUTTON", L"Define", BS_OWNERDRAW | WS_TABSTOP,
               kDefineWordControlId);
    wordCloudButton_ =
        create(L"BUTTON", L"Word Cloud", BS_OWNERDRAW | WS_TABSTOP,
               kWordCloudControlId);
    speechRateLabel_ = create(L"STATIC", L"Speed: 1.0x",
                              SS_LEFT | SS_CENTERIMAGE, 0);
    speechRateTrackbar_ =
        create(TRACKBAR_CLASSW, L"", TBS_NOTICKS | WS_TABSTOP,
               kSpeechRateControlId);
    shortcutHintLabel_ =
        create(L"STATIC",
               L"D / R-Click: define  \x2022  Enter: speak  \x2022  Ctrl+Shift+R: capture",
               SS_RIGHT | SS_CENTERIMAGE | SS_ENDELLIPSIS, 0);
    readingStatsLabel_ =
        create(L"STATIC", L"0 words",
               SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS, 0);
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
    RECT client{};
    GetClientRect(hwnd_, &client);
    const UINT effDpiX = dpiX_ == 0 ? USER_DEFAULT_SCREEN_DPI : dpiX_;
    const float clientWidthDip = std::max(
        900.0F,
        static_cast<float>(client.right - client.left) * 96.0F /
            static_cast<float>(effDpiX));

    HDWP deferPos = BeginDeferWindowPos(31);
    const auto move = [this, &deferPos](HWND control, float x, float y,
                                        float width, float height) {
        if (control == nullptr) {
            return;
        }
        const int pxX = static_cast<int>(scale_dip(x, dpiX_));
        const int pxY = static_cast<int>(scale_dip(y, dpiY_));
        const int pxW = static_cast<int>(scale_dip(width, dpiX_));
        const int pxH = static_cast<int>(scale_dip(height, dpiY_));
        if (deferPos != nullptr) {
            HDWP next = DeferWindowPos(
                deferPos, control, nullptr, pxX, pxY, pxW, pxH,
                SWP_NOZORDER | SWP_NOACTIVATE);
            if (next != nullptr) {
                deferPos = next;
                return;
            }
        }
        if (!SetWindowPos(control, nullptr, pxX, pxY, pxW, pxH,
                          SWP_NOZORDER | SWP_NOACTIVATE)) {
            throw ReaderError(ReaderErrorCode::RenderingFailed,
                              "Could not position a reader toolbar control");
        }
    };

    // Row 1: Primary capture/mode actions + typography & theme + platform sync.
    move(captureButton_, 16, 10, 98, 28);
    move(clipboardButton_, 118, 10, 82, 28);
    move(focusModeButton_, 204, 10, 92, 28);

    move(fontFamilyLabel_, 306, 12, 36, 24);
    move(fontFamilyCombo_, 344, 11, 142, 220);
    move(decreaseFontButton_, 492, 10, 28, 28);
    move(fontSizeLabel_, 522, 12, 50, 24);
    move(increaseFontButton_, 574, 10, 28, 28);
    move(themeLabel_, 610, 12, 44, 24);
    move(themeCombo_, 656, 11, 136, 220);
    move(platformButton_, 800, 10, 76, 28);

    // Row 2: Spacing sliders (Letter, Line, Width) + visual reading aids.
    move(letterSpacingLabel_, 16, 50, 74, 22);
    move(letterSpacingTrackbar_, 92, 48, 92, 26);
    move(lineHeightLabel_, 196, 50, 66, 22);
    move(lineHeightTrackbar_, 264, 48, 92, 26);
    move(lineWidthLabel_, 368, 50, 78, 22);
    move(lineWidthTrackbar_, 448, 48, 92, 26);

    move(bionicReadingCheckBox_, 564, 50, 92, 22);
    move(readingRulerCheckBox_, 660, 50, 100, 22);
    move(bimodalHighlightingCheckBox_, 764, 50, 96, 22);

    // Row 3: Offline speech controls + right-aligned shortcut hint.
    move(speechVoiceLabel_, 16, 88, 38, 24);
    move(speechVoiceCombo_, 56, 86, 142, 220);
    move(speechPlayButton_, 202, 86, 56, 28);
    move(speakWordButton_, 262, 86, 78, 28);
    move(speechStopButton_, 344, 86, 46, 28);
    move(defineWordButton_, 394, 86, 64, 28);
    move(wordCloudButton_, 462, 86, 86, 28);
    move(speechRateLabel_, 554, 88, 64, 24);
    move(speechRateTrackbar_, 620, 86, 72, 26);
    move(readingStatsLabel_, 700, 88, 175, 24);
    move(shortcutHintLabel_, 884, 90,
         std::max(60.0F, clientWidthDip - 900.0F), 20);
    if (shortcutHintLabel_ != nullptr) {
        ShowWindow(shortcutHintLabel_, clientWidthDip >= 960.0F ? SW_SHOW : SW_HIDE);
    }
    if (deferPos != nullptr) {
        EndDeferWindowPos(deferPos);
    }
    layout_canvas_window();
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
    SendMessageW(lineWidthTrackbar_, TBM_SETPOS, TRUE,
                 static_cast<LPARAM>(profile_.lineWidthCh));
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
    letterText << L"Letter: " << std::fixed << std::setprecision(2)
               << profile_.letterSpacingEm;
    if (!SetWindowTextW(letterSpacingLabel_, letterText.str().c_str())) {
        throw ReaderError(ReaderErrorCode::RenderingFailed,
                          "Could not update the letter spacing label");
    }
    std::wostringstream lineText;
    lineText << L"Line: " << std::fixed << std::setprecision(1)
             << profile_.lineHeight << L"x";
    if (!SetWindowTextW(lineHeightLabel_, lineText.str().c_str())) {
        throw ReaderError(ReaderErrorCode::RenderingFailed,
                          "Could not update the line height label");
    }
    std::wostringstream widthText;
    widthText << L"Width: " << profile_.lineWidthCh << L"ch";
    SetWindowTextW(lineWidthLabel_, widthText.str().c_str());

    SendMessageW(speechRateTrackbar_, TBM_SETPOS, TRUE,
                 static_cast<LPARAM>(std::lround(speechRate_ * 100.0F)));
    std::wostringstream speechRateText;
    speechRateText << L"Speed: " << std::fixed << std::setprecision(1)
                   << speechRate_ << L"x";
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
    if (focusModeButton_ != nullptr) {
        SetWindowTextW(focusModeButton_, mode_ == ReaderMode::Focus
                                             ? L"Floating Mode"
                                             : L"Focus Mode");
    }
    if (captureButton_ != nullptr) {
        EnableWindow(captureButton_, onCaptureRequested_ ? TRUE : FALSE);
    }
    if (clipboardButton_ != nullptr) {
        EnableWindow(clipboardButton_, onClipboardRequested_ ? TRUE : FALSE);
    }
    sync_platform_button_state();
    EnableWindow(speechVoiceCombo_, speechAvailable_ ? TRUE : FALSE);
    EnableWindow(speechPlayButton_,
                 speechAvailable_ && speechState_ != PlaybackState::Loading
                     ? TRUE
                     : FALSE);
    if (speakWordButton_ != nullptr) {
        const bool hasSelectedWord =
            selectedWordIndex_.has_value() &&
            *selectedWordIndex_ < document_.words.size();
        EnableWindow(speakWordButton_,
                     speechAvailable_ && hasSelectedWord ? TRUE : FALSE);
    }
    EnableWindow(speechStopButton_, speechAvailable_ ? TRUE : FALSE);
    if (defineWordButton_ != nullptr) {
        const bool hasSelectedWord =
            selectedWordIndex_.has_value() &&
            *selectedWordIndex_ < document_.words.size();
        EnableWindow(defineWordButton_,
                     onDefineWordRequested_ && hasSelectedWord ? TRUE : FALSE);
    }
    if (wordCloudButton_ != nullptr) {
        EnableWindow(wordCloudButton_,
                     onWordCloudRequested_ && !document_.words.empty()
                         ? TRUE
                         : FALSE);
    }
    EnableWindow(speechRateTrackbar_, speechAvailable_ ? TRUE : FALSE);
    if (readingStatsLabel_ != nullptr) {
        const std::size_t wordCount = document_.words.size();
        std::wostringstream stats;
        if (wordCount == 0) {
            stats << L"0 words";
        } else {
            if (selectedWordIndex_.has_value() &&
                *selectedWordIndex_ < wordCount) {
                stats << L"Word " << (*selectedWordIndex_ + 1) << L"/"
                      << wordCount << L"  \x2022  ";
            } else {
                stats << wordCount
                      << (wordCount == 1 ? L" word  \x2022  " : L" words  \x2022  ");
            }
            const double mins = static_cast<double>(wordCount) / 130.0;
            if (mins < 0.8) {
                stats << L"< 1 min read";
            } else {
                stats << L"~" << std::fixed << std::setprecision(1) << mins
                      << L" min read";
            }
        }
        SetWindowTextW(readingStatsLabel_, stats.str().c_str());
    }
}

void Direct2DTextRenderer::sync_platform_button_state() const {
    if (platformButton_ != nullptr) {
        const bool enabled = static_cast<bool>(onImportShareCodeRequested_) ||
                             static_cast<bool>(onShareProfileRequested_) ||
                             static_cast<bool>(onOpenWebPlaygroundRequested_);
        EnableWindow(platformButton_, enabled ? TRUE : FALSE);
    }
}

void Direct2DTextRenderer::show_platform_menu() {
    if (platformButton_ == nullptr || hwnd_ == nullptr) {
        return;
    }
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    constexpr UINT kMenuImport = 1;
    constexpr UINT kMenuShare = 2;
    constexpr UINT kMenuOpenWeb = 3;

    AppendMenuW(menu,
                MF_STRING |
                    (onImportShareCodeRequested_ ? MF_ENABLED : MF_GRAYED),
                kMenuImport, L"Import Profile by Share Code...");
    AppendMenuW(menu,
                MF_STRING |
                    (onShareProfileRequested_ ? MF_ENABLED : MF_GRAYED),
                kMenuShare, L"Share Profile to Platform");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu,
                MF_STRING |
                    (onOpenWebPlaygroundRequested_ ? MF_ENABLED : MF_GRAYED),
                kMenuOpenWeb, L"Open Web Playground");

    RECT buttonRect{};
    GetWindowRect(platformButton_, &buttonRect);
    SetForegroundWindow(hwnd_);
    const UINT chosen = static_cast<UINT>(TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY,
        buttonRect.left, buttonRect.bottom + 2, 0, hwnd_, nullptr));
    DestroyMenu(menu);

    if (chosen == kMenuImport && onImportShareCodeRequested_) {
        onImportShareCodeRequested_();
    } else if (chosen == kMenuShare && onShareProfileRequested_) {
        onShareProfileRequested_();
    } else if (chosen == kMenuOpenWeb && onOpenWebPlaygroundRequested_) {
        onOpenWebPlaygroundRequested_();
    }
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
    case kFocusModeControlId:
        if (notificationCode == BN_CLICKED) {
            set_mode(mode_ == ReaderMode::Focus ? ReaderMode::Floating
                                                : ReaderMode::Focus);
        }
        return;
    case kCaptureControlId:
        if (notificationCode == BN_CLICKED && onCaptureRequested_) {
            onCaptureRequested_();
        }
        return;
    case kClipboardControlId:
        if (notificationCode == BN_CLICKED && onClipboardRequested_) {
            onClipboardRequested_();
        }
        return;
    case kPlatformControlId:
        if (notificationCode == BN_CLICKED) {
            show_platform_menu();
        }
        return;
    case kDefineWordControlId:
        if (notificationCode == BN_CLICKED && onDefineWordRequested_) {
            onDefineWordRequested_();
        }
        return;
    case kWordCloudControlId:
        if (notificationCode == BN_CLICKED && onWordCloudRequested_) {
            onWordCloudRequested_();
        }
        return;
    case kSpeechVoiceControlId:
        return;
    case kSpeechPlayControlId:
    case kSpeechStopControlId:
        if (notificationCode != BN_CLICKED) {
            return;
        }
        handle_speech_command(controlId);
        return;
    case kSpeakWordControlId:
        if (notificationCode == BN_CLICKED) {
            speak_selected_word();
        }
        return;
    default:
        return;
    }
    commit_ui_profile(updated);
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
    } else if (control == lineWidthTrackbar_) {
        updated.lineWidthCh = static_cast<std::uint32_t>(
            std::clamp<LRESULT>(SendMessageW(control, TBM_GETPOS, 0, 0), 35, 100));
    } else {
        return;
    }
    commit_ui_profile(updated);
}

void Direct2DTextRenderer::start_speech_request(
    std::optional<TextRange> selection) {
    if (speechEngine_ == nullptr || !speechAvailable_) {
        return;
    }
    try {
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

        speechTracksDocument_ = true;
        SpeechRequest request;
        request.text = document_.text;
        request.selection = selection;
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

std::optional<std::string> Direct2DTextRenderer::selected_word_utf8() const {
    if (!selectedWordIndex_.has_value() ||
        *selectedWordIndex_ >= document_.words.size()) {
        return std::nullopt;
    }
    const TextRange wordRange = document_.words[*selectedWordIndex_].text;
    if (wordRange.length == 0 || wordRange.start >= document_.text.size() ||
        wordRange.length > document_.text.size() - wordRange.start) {
        return std::nullopt;
    }
    const std::u16string_view slice(document_.text.data() + wordRange.start,
                                    wordRange.length);
    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, reinterpret_cast<const wchar_t*>(slice.data()),
        static_cast<int>(slice.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return std::nullopt;
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0,
                        reinterpret_cast<const wchar_t*>(slice.data()),
                        static_cast<int>(slice.size()), out.data(), needed,
                        nullptr, nullptr);
    return out;
}

void Direct2DTextRenderer::select_word_at_index(std::size_t wordIndex) {
    if (wordIndex >= document_.words.size()) {
        return;
    }
    selectedWordIndex_ = wordIndex;
    const auto bounds = text_range_bounds(document_.words[wordIndex].text);
    cursorTargetCenterX_ =
        bounds.empty() ? -1.0F
                       : bounds.front().x + bounds.front().width * 0.5F;
    update_focus_scroll(document_.words[wordIndex].text);
    sync_ruler_to_word();
    sync_toolbar_controls();
    invalidate();
}

void Direct2DTextRenderer::speak_text_utf8(const std::string& utf8Text) {
    if (!speechAvailable_ || speechEngine_ == nullptr || utf8Text.empty()) {
        return;
    }
    const int needed = MultiByteToWideChar(
        CP_UTF8, 0, utf8Text.data(), static_cast<int>(utf8Text.size()),
        nullptr, 0);
    if (needed <= 0) {
        return;
    }
    std::u16string u16(static_cast<std::size_t>(needed), u'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8Text.data(),
                        static_cast<int>(utf8Text.size()),
                        reinterpret_cast<wchar_t*>(u16.data()), needed);

    try {
        const LRESULT selected =
            SendMessageW(speechVoiceCombo_, CB_GETCURSEL, 0, 0);
        if (selected == CB_ERR) {
            return;
        }
        const auto voiceIndex = static_cast<std::size_t>(
            SendMessageW(speechVoiceCombo_, CB_GETITEMDATA,
                         static_cast<WPARAM>(selected), 0));
        if (voiceIndex >= speechVoices_.size()) {
            return;
        }

        speechTracksDocument_ = false;
        SpeechRequest request;
        request.text = std::move(u16);
        request.voiceId = speechVoices_[voiceIndex].id;
        request.rate = speechRate_;
        speechState_ = PlaybackState::Loading;
        highlights_ = {};
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
                PostMessageW(state->window, kSpeechEventMessage, 0, 0);
            });
    } catch (const ReaderError&) {
        speechState_ = PlaybackState::Failed;
        sync_toolbar_controls();
    }
}

void Direct2DTextRenderer::speak_selected_word() {
    if (!speechAvailable_ || speechEngine_ == nullptr ||
        !selectedWordIndex_.has_value() ||
        *selectedWordIndex_ >= document_.words.size()) {
        return;
    }
    const TextRange wordRange = document_.words[*selectedWordIndex_].text;
    if (wordRange.length == 0 || wordRange.start >= document_.text.size() ||
        wordRange.length > document_.text.size() - wordRange.start) {
        return;
    }
    start_speech_request(wordRange);
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

        std::optional<TextRange> rangeFromCursor;
        if (selectedWordIndex_.has_value() &&
            *selectedWordIndex_ < document_.words.size()) {
            const TextOffset start =
                document_.words[*selectedWordIndex_].text.start;
            if (start < document_.text.size()) {
                rangeFromCursor = TextRange{
                    start,
                    static_cast<TextOffset>(document_.text.size() - start)};
            }
        }
        start_speech_request(rangeFromCursor);
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
        if (!speechTracksDocument_) {
            break;
        }
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
                update_focus_scroll(range);
            } else if (event.mark->kind == SpeechMarkKind::Sentence &&
                       highlights_.sentenceTimingReliable) {
                highlights_.sentence = range;
                update_focus_scroll(range);
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
            const float visibleTop = kTextPaddingDip;
            const float visibleBottom =
                viewport_.size.height - kTextPaddingDip;
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
    if (!profile_.aids.readingRuler || pixelY < 0) {
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

void Direct2DTextRenderer::sync_ruler_to_word() {
    if (!profile_.aids.readingRuler || !selectedWordIndex_.has_value() ||
        *selectedWordIndex_ >= document_.words.size()) {
        return;
    }
    rulerAnchorOffset_ = document_.words[*selectedWordIndex_].text.start;
    refresh_aid_visuals();
}

void Direct2DTextRenderer::clear_word_cursor() {
    if (!selectedWordIndex_.has_value()) {
        return;
    }
    selectedWordIndex_.reset();
    cursorTargetCenterX_ = -1.0F;
    sync_toolbar_controls();
    invalidate();
}

bool Direct2DTextRenderer::select_word_at_point(int pixelX, int pixelY) {
    if (textLayout_ == nullptr || document_.words.empty()) {
        return false;
    }
    const UINT effDpiX = dpiX_ == 0 ? USER_DEFAULT_SCREEN_DPI : dpiX_;
    const UINT effDpiY = dpiY_ == 0 ? USER_DEFAULT_SCREEN_DPI : dpiY_;
    const float dipX =
        static_cast<float>(pixelX) * 96.0F / static_cast<float>(effDpiX);
    const float dipY =
        static_cast<float>(pixelY) * 96.0F / static_cast<float>(effDpiY) +
        viewport_.scrollY;

    const auto lines = line_layout_snapshot().lines;
    std::optional<std::size_t> matchedLine;
    float bestLineDistY = 6.0F;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto& b = lines[i].bounds;
        const float bottom = b.y + b.height;
        const float distY =
            dipY < b.y ? b.y - dipY : dipY > bottom ? dipY - bottom : 0.0F;
        if (distY <= bestLineDistY) {
            bestLineDistY = distY;
            matchedLine = i;
        }
    }
    if (!matchedLine.has_value()) {
        return false;
    }

    const auto& line = lines[*matchedLine];
    const float layoutX = dipX - text_column_metrics().first;
    const float layoutY = dipY - kTextPaddingDip;
    BOOL isTrailingHit = FALSE;
    BOOL isInside = FALSE;
    DWRITE_HIT_TEST_METRICS metrics{};
    if (FAILED(textLayout_->HitTestPoint(layoutX, layoutY, &isTrailingHit,
                                         &isInside, &metrics))) {
        return false;
    }

    const TextOffset lineStart = line.text.start;
    const TextOffset lineEnd = line.text.start + line.text.length;
    std::optional<std::size_t> bestWord;
    float bestDistX = 32.0F;
    float bestCenterX = -1.0F;

    for (std::size_t idx = 0; idx < document_.words.size(); ++idx) {
        const auto& w = document_.words[idx].text;
        const TextOffset wEnd = w.start + w.length;
        if (w.length == 0 || wEnd <= lineStart || w.start >= lineEnd) {
            continue;
        }
        const auto bounds = text_range_bounds(w);
        if (bounds.empty()) {
            continue;
        }
        const auto& rect = bounds.front();
        const float centerX = rect.x + rect.width * 0.5F;
        if (isInside && metrics.textPosition >= w.start &&
            metrics.textPosition < wEnd) {
            bestWord = idx;
            bestCenterX = centerX;
            break;
        }
        const float rectRight = rect.x + rect.width;
        const float distX =
            dipX < rect.x ? rect.x - dipX
                          : dipX > rectRight ? dipX - rectRight : 0.0F;
        if (distX <= bestDistX) {
            bestDistX = distX;
            bestWord = idx;
            bestCenterX = centerX;
        }
    }

    if (!bestWord.has_value()) {
        return false;
    }
    selectedWordIndex_ = bestWord;
    cursorTargetCenterX_ = bestCenterX;
    update_focus_scroll(document_.words[*bestWord].text);
    sync_ruler_to_word();
    sync_toolbar_controls();
    invalidate();
    return true;
}

void Direct2DTextRenderer::move_word_cursor(int wordDelta) {
    if (document_.words.empty()) {
        return;
    }
    std::size_t nextIndex = 0;
    if (!selectedWordIndex_.has_value()) {
        if (wordDelta >= 0) {
            for (std::size_t i = 0; i < document_.words.size(); ++i) {
                const auto bounds = text_range_bounds(document_.words[i].text);
                if (!bounds.empty() &&
                    bounds.front().y + bounds.front().height >=
                        viewport_.scrollY) {
                    nextIndex = i;
                    break;
                }
            }
        } else {
            nextIndex = document_.words.size() - 1;
        }
    } else {
        const std::ptrdiff_t maxIndex =
            static_cast<std::ptrdiff_t>(document_.words.size() - 1);
        const std::ptrdiff_t candidate = std::clamp<std::ptrdiff_t>(
            static_cast<std::ptrdiff_t>(*selectedWordIndex_) + wordDelta, 0,
            maxIndex);
        nextIndex = static_cast<std::size_t>(candidate);
    }

    selectedWordIndex_ = nextIndex;
    const auto bounds = text_range_bounds(document_.words[nextIndex].text);
    cursorTargetCenterX_ =
        bounds.empty() ? -1.0F
                       : bounds.front().x + bounds.front().width * 0.5F;
    update_focus_scroll(document_.words[nextIndex].text);
    sync_ruler_to_word();
    sync_toolbar_controls();
    invalidate();
}

void Direct2DTextRenderer::move_word_cursor_line(int lineDelta) {
    if (document_.words.empty()) {
        return;
    }
    if (!selectedWordIndex_.has_value() || lineDelta == 0) {
        move_word_cursor(lineDelta >= 0 ? 0 : -1);
        return;
    }
    const auto lines = line_layout_snapshot().lines;
    if (lines.empty()) {
        return;
    }

    const TextOffset curStart = document_.words[*selectedWordIndex_].text.start;
    std::size_t curLine = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const TextOffset lineEnd = lines[i].text.start + lines[i].text.length;
        if (curStart >= lines[i].text.start && curStart < lineEnd) {
            curLine = i;
            break;
        }
        if (lines[i].text.start <= curStart) {
            curLine = i;
        }
    }

    float targetX = cursorTargetCenterX_;
    if (targetX < 0.0F) {
        const auto curBounds =
            text_range_bounds(document_.words[*selectedWordIndex_].text);
        targetX = curBounds.empty()
                      ? 0.0F
                      : curBounds.front().x + curBounds.front().width * 0.5F;
        cursorTargetCenterX_ = targetX;
    }

    const int step = lineDelta > 0 ? 1 : -1;
    std::optional<std::size_t> bestWord;
    for (std::ptrdiff_t candidateLine = static_cast<std::ptrdiff_t>(curLine) + step;
         candidateLine >= 0 &&
         candidateLine < static_cast<std::ptrdiff_t>(lines.size());
         candidateLine += step) {
        const auto& line = lines[static_cast<std::size_t>(candidateLine)];
        const TextOffset lineStart = line.text.start;
        const TextOffset lineEnd = line.text.start + line.text.length;
        float bestDistX = std::numeric_limits<float>::max();
        for (std::size_t idx = 0; idx < document_.words.size(); ++idx) {
            const auto& w = document_.words[idx].text;
            const TextOffset wEnd = w.start + w.length;
            if (w.length == 0 || wEnd <= lineStart || w.start >= lineEnd) {
                continue;
            }
            const auto bounds = text_range_bounds(w);
            if (bounds.empty()) {
                continue;
            }
            const float centerX =
                bounds.front().x + bounds.front().width * 0.5F;
            const float dist = std::abs(centerX - targetX);
            if (dist < bestDistX) {
                bestDistX = dist;
                bestWord = idx;
            }
        }
        if (bestWord.has_value()) {
            break;
        }
    }

    if (!bestWord.has_value()) {
        return;
    }
    selectedWordIndex_ = bestWord;
    update_focus_scroll(document_.words[*bestWord].text);
    sync_ruler_to_word();
    sync_toolbar_controls();
    invalidate();
}

void Direct2DTextRenderer::draw_word_cursor(bool underText) const {
    if (!selectedWordIndex_.has_value() ||
        *selectedWordIndex_ >= document_.words.size() ||
        rulerBrush_ == nullptr || renderTarget_ == nullptr) {
        return;
    }
    const auto bounds =
        text_range_bounds(document_.words[*selectedWordIndex_].text);
    if (bounds.empty()) {
        return;
    }
    const auto& tokens = theme_tokens(profile_.theme);
    rulerBrush_->SetColor(color_from_hex(tokens.focus));
    if (underText) {
        rulerBrush_->SetOpacity(
            profile_.theme == ThemeId::DarkHighContrast ? 0.28F : 0.16F);
        for (const auto& rect : bounds) {
            D2D1_ROUNDED_RECT rounded{};
            rounded.rect = D2D1::RectF(
                rect.x - 2.0F, rect.y - 1.0F,
                rect.x + rect.width + 2.0F, rect.y + rect.height + 1.0F);
            rounded.radiusX = 3.0F;
            rounded.radiusY = 3.0F;
            renderTarget_->FillRoundedRectangle(&rounded, rulerBrush_.Get());
        }
    } else {
        rulerBrush_->SetOpacity(0.92F);
        for (const auto& rect : bounds) {
            D2D1_ROUNDED_RECT rounded{};
            rounded.rect = D2D1::RectF(
                rect.x - 2.0F, rect.y - 1.0F,
                rect.x + rect.width + 2.0F, rect.y + rect.height + 1.0F);
            rounded.radiusX = 3.0F;
            rounded.radiusY = 3.0F;
            renderTarget_->DrawRoundedRectangle(
                &rounded, rulerBrush_.Get(), 1.5F);
        }
        const auto& first = bounds.front();
        renderTarget_->FillRectangle(
            D2D1::RectF(first.x - 3.5F, first.y - 1.0F,
                        first.x - 1.0F, first.y + first.height + 1.0F),
            rulerBrush_.Get());
    }
    rulerBrush_->SetOpacity(1.0F);
}

void Direct2DTextRenderer::ensure_render_target() {
    if (renderTarget_ != nullptr) {
        return;
    }
    if (canvasHwnd_ == nullptr) {
        throw ReaderError(ReaderErrorCode::RenderingFailed,
                          "Reader canvas window has not been created");
    }

    RECT client{};
    if (!GetClientRect(canvasHwnd_, &client)) {
        throw ReaderError(ReaderErrorCode::RenderingFailed,
                          "Could not determine reader canvas dimensions");
    }

    D2D1_RENDER_TARGET_PROPERTIES properties =
        D2D1::RenderTargetProperties();
    properties.dpiX = static_cast<float>(dpiX_);
    properties.dpiY = static_cast<float>(dpiY_);

    check_hresult(
        d2dFactory_->CreateHwndRenderTarget(
            properties,
            D2D1::HwndRenderTargetProperties(
                canvasHwnd_,
                D2D1::SizeU(
                    static_cast<UINT>(std::max(1L, client.right - client.left)),
                    static_cast<UINT>(
                        std::max(1L, client.bottom - client.top)))),
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
        text_column_metrics().second;
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
    viewport_.scrollY = std::clamp(viewport_.scrollY, 0.0F, max_scroll_y());
    sync_scrollbar();
}

void Direct2DTextRenderer::discard_device_resources() noexcept {
    textBrush_.Reset();
    rulerBrush_.Reset();
    renderTarget_.Reset();
}

void Direct2DTextRenderer::paint() {
    if (canvasHwnd_ == nullptr || (hwnd_ != nullptr && IsIconic(hwnd_))) {
        return;
    }
    RECT canvasClient{};
    if (!GetClientRect(canvasHwnd_, &canvasClient) ||
        canvasClient.right - canvasClient.left <= 0 ||
        canvasClient.bottom - canvasClient.top <= 0) {
        return;
    }
    try {
        ensure_render_target();
        const auto& tokens = theme_tokens(profile_.theme);
        textBrush_->SetColor(color_from_hex(tokens.text));
        renderTarget_->BeginDraw();
        renderTarget_->Clear(color_from_hex(tokens.background));

        const auto [colOriginX, colWidth] = text_column_metrics();
        if (colOriginX > kTextPaddingDip + 12.0F &&
            viewport_.size.height > 0.0F) {
            const float pageLeft = std::max(0.0F, colOriginX - 24.0F);
            const float pageRight =
                std::min(viewport_.size.width, colOriginX + colWidth + 24.0F);
            const float viewBottom = viewport_.size.height;
            rulerBrush_->SetColor(color_from_hex(tokens.surface));
            rulerBrush_->SetOpacity(0.55F);
            renderTarget_->FillRectangle(
                D2D1::RectF(0.0F, 0.0F, pageLeft, viewBottom),
                rulerBrush_.Get());
            renderTarget_->FillRectangle(
                D2D1::RectF(pageRight, 0.0F, viewport_.size.width, viewBottom),
                rulerBrush_.Get());
            rulerBrush_->SetColor(color_from_hex(tokens.border));
            rulerBrush_->SetOpacity(0.65F);
            renderTarget_->DrawLine(
                D2D1::Point2F(pageLeft, 0.0F),
                D2D1::Point2F(pageLeft, viewBottom), rulerBrush_.Get(), 1.0F);
            renderTarget_->DrawLine(
                D2D1::Point2F(pageRight, 0.0F),
                D2D1::Point2F(pageRight, viewBottom), rulerBrush_.Get(), 1.0F);
            rulerBrush_->SetOpacity(1.0F);
        }

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
        draw_word_cursor(true);
        if (textLayout_ != nullptr && textBrush_ != nullptr) {
            renderTarget_->DrawTextLayout(
                D2D1::Point2F(text_column_metrics().first, kTextPaddingDip),
                textLayout_.Get(), textBrush_.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        drawRulerOverlays(false);
        drawWordHighlights();
        draw_word_cursor(false);
        renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());

        const HRESULT result = renderTarget_->EndDraw();
        if (result == D2DERR_RECREATE_TARGET) {
            discard_device_resources();
            InvalidateRect(canvasHwnd_, nullptr, FALSE);
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
    if (canvasHwnd_ == nullptr) {
        return;
    }
    RECT client{};
    if (!GetClientRect(canvasHwnd_, &client)) {
        return;
    }
    const UINT effectiveDpiX =
        dpiX_ == 0 ? USER_DEFAULT_SCREEN_DPI : dpiX_;
    const UINT effectiveDpiY =
        dpiY_ == 0 ? USER_DEFAULT_SCREEN_DPI : dpiY_;
    const float pixelsToDipsX = 96.0F / static_cast<float>(effectiveDpiX);
    const float pixelsToDipsY = 96.0F / static_cast<float>(effectiveDpiY);
    viewport_.size = {
        static_cast<float>(std::max(0L, client.right - client.left)) *
            pixelsToDipsX,
        static_cast<float>(std::max(0L, client.bottom - client.top)) *
            pixelsToDipsY};
}

void Direct2DTextRenderer::require_ui_thread() const {
    if (uiThreadId_ != 0 && GetCurrentThreadId() != uiThreadId_) {
        throw ReaderError(ReaderErrorCode::RenderingFailed,
                          "Reader renderer methods must run on its UI thread");
    }
}

}  // namespace dyslexia_reader::windows
