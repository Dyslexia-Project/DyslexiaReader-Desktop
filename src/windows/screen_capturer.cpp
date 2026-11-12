#include "dyslexia_reader/windows/screen_capturer.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace dyslexia_reader::windows {
namespace {

constexpr wchar_t kOverlayClassName[] = L"DyslexiaReaderScreenCaptureOverlay";
constexpr int kMinSelectionPixels = 6;

struct UniqueDc {
    HDC hdc = nullptr;
    ~UniqueDc() {
        if (hdc != nullptr) {
            DeleteDC(hdc);
        }
    }
    UniqueDc() = default;
    UniqueDc(const UniqueDc&) = delete;
    UniqueDc& operator=(const UniqueDc&) = delete;
};

struct UniqueBitmap {
    HBITMAP bmp = nullptr;
    ~UniqueBitmap() {
        if (bmp != nullptr) {
            DeleteObject(bmp);
        }
    }
    UniqueBitmap() = default;
    UniqueBitmap(const UniqueBitmap&) = delete;
    UniqueBitmap& operator=(const UniqueBitmap&) = delete;
};

struct UniqueBrush {
    HBRUSH brush = nullptr;
    ~UniqueBrush() {
        if (brush != nullptr) {
            DeleteObject(brush);
        }
    }
    explicit UniqueBrush(COLORREF color) : brush(CreateSolidBrush(color)) {}
    UniqueBrush(const UniqueBrush&) = delete;
    UniqueBrush& operator=(const UniqueBrush&) = delete;
};

struct DesktopSnapshot {
    int virtualX = 0;
    int virtualY = 0;
    int width = 0;
    int height = 0;
    UniqueDc rawDc;
    UniqueBitmap rawBmp;
    HGDIOBJ prevRawBmp = nullptr;
    std::uint8_t* rawPixels = nullptr;

    UniqueDc dimDc;
    UniqueBitmap dimBmp;
    HGDIOBJ prevDimBmp = nullptr;
    std::uint8_t* dimPixels = nullptr;

    UniqueDc backDc;
    UniqueBitmap backBmp;
    HGDIOBJ prevBackBmp = nullptr;

    ~DesktopSnapshot() {
        if (backDc.hdc != nullptr && prevBackBmp != nullptr) {
            SelectObject(backDc.hdc, prevBackBmp);
        }
        if (dimDc.hdc != nullptr && prevDimBmp != nullptr) {
            SelectObject(dimDc.hdc, prevDimBmp);
        }
        if (rawDc.hdc != nullptr && prevRawBmp != nullptr) {
            SelectObject(rawDc.hdc, prevRawBmp);
        }
    }
};

bool create_topdown_dib(HDC referenceDc, int width, int height, UniqueDc& outDc,
                        UniqueBitmap& outBmp, HGDIOBJ& outPrevBmp,
                        std::uint8_t** outPixels) {
    outDc.hdc = CreateCompatibleDC(referenceDc);
    if (outDc.hdc == nullptr) {
        return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;  // Top-down DIB.
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    outBmp.bmp =
        CreateDIBSection(referenceDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (outBmp.bmp == nullptr || bits == nullptr) {
        return false;
    }
    outPrevBmp = SelectObject(outDc.hdc, outBmp.bmp);
    if (outPixels != nullptr) {
        *outPixels = static_cast<std::uint8_t*>(bits);
    }
    return true;
}

bool capture_virtual_desktop(DesktopSnapshot& snapshot) {
    snapshot.virtualX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    snapshot.virtualY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    snapshot.width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    snapshot.height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (snapshot.width <= 0 || snapshot.height <= 0) {
        return false;
    }

    HDC screenDc = GetDC(nullptr);
    if (screenDc == nullptr) {
        return false;
    }

    const bool okRaw =
        create_topdown_dib(screenDc, snapshot.width, snapshot.height,
                           snapshot.rawDc, snapshot.rawBmp, snapshot.prevRawBmp,
                           &snapshot.rawPixels);
    const bool okDim =
        okRaw && create_topdown_dib(screenDc, snapshot.width, snapshot.height,
                                    snapshot.dimDc, snapshot.dimBmp,
                                    snapshot.prevDimBmp, &snapshot.dimPixels);
    const bool okBack =
        okDim &&
        create_topdown_dib(screenDc, snapshot.width, snapshot.height,
                           snapshot.backDc, snapshot.backBmp,
                           snapshot.prevBackBmp, nullptr);

    bool copied = false;
    if (okBack) {
        copied = BitBlt(snapshot.rawDc.hdc, 0, 0, snapshot.width,
                        snapshot.height, screenDc, snapshot.virtualX,
                        snapshot.virtualY, SRCCOPY | CAPTUREBLT) != FALSE;
    }
    ReleaseDC(nullptr, screenDc);
    if (!copied) {
        return false;
    }

    const std::size_t pixelCount =
        static_cast<std::size_t>(snapshot.width) *
        static_cast<std::size_t>(snapshot.height);
    for (std::size_t i = 0; i < pixelCount; ++i) {
        const std::size_t offset = i * 4;
        snapshot.rawPixels[offset + 3] = 255;
        snapshot.dimPixels[offset + 0] =
            static_cast<std::uint8_t>((snapshot.rawPixels[offset + 0] * 45) / 100);
        snapshot.dimPixels[offset + 1] =
            static_cast<std::uint8_t>((snapshot.rawPixels[offset + 1] * 45) / 100);
        snapshot.dimPixels[offset + 2] =
            static_cast<std::uint8_t>((snapshot.rawPixels[offset + 2] * 45) / 100);
        snapshot.dimPixels[offset + 3] = 255;
    }

    return true;
}

void query_monitor_scale(const RECT& desktopRect, float& outScaleX,
                         float& outScaleY) {
    outScaleX = 1.0F;
    outScaleY = 1.0F;

    using GetDpiForMonitorFn =
        HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    HMODULE shcore = GetModuleHandleW(L"Shcore.dll");
    if (shcore == nullptr) {
        shcore = LoadLibraryW(L"Shcore.dll");
    }
    if (shcore != nullptr) {
        const auto getDpiForMonitor = reinterpret_cast<GetDpiForMonitorFn>(
            GetProcAddress(shcore, "GetDpiForMonitor"));
        HMONITOR monitor =
            MonitorFromRect(&desktopRect, MONITOR_DEFAULTTONEAREST);
        if (getDpiForMonitor != nullptr && monitor != nullptr) {
            UINT dpiX = 96;
            UINT dpiY = 96;
            constexpr int kMdtEffectiveDpi = 0;
            if (SUCCEEDED(
                    getDpiForMonitor(monitor, kMdtEffectiveDpi, &dpiX, &dpiY)) &&
                dpiX > 0 && dpiY > 0) {
                outScaleX = static_cast<float>(dpiX) / 96.0F;
                outScaleY = static_cast<float>(dpiY) / 96.0F;
                return;
            }
        }
    }

    HDC screenDc = GetDC(nullptr);
    if (screenDc != nullptr) {
        const int logPixelsX = GetDeviceCaps(screenDc, LOGPIXELSX);
        const int logPixelsY = GetDeviceCaps(screenDc, LOGPIXELSY);
        ReleaseDC(nullptr, screenDc);
        if (logPixelsX > 0) {
            outScaleX = static_cast<float>(logPixelsX) / 96.0F;
        }
        if (logPixelsY > 0) {
            outScaleY = static_cast<float>(logPixelsY) / 96.0F;
        }
    }
}

struct OverlayState {
    DesktopSnapshot* snapshot = nullptr;
    bool dragging = false;
    bool accepted = false;
    bool cancelled = false;
    POINT anchor{};
    POINT current{};

    RECT normalized_selection() const {
        if (snapshot == nullptr) {
            return {};
        }
        const int left =
            std::clamp(static_cast<int>(std::min(anchor.x, current.x)), 0,
                       snapshot->width);
        const int top =
            std::clamp(static_cast<int>(std::min(anchor.y, current.y)), 0,
                       snapshot->height);
        const int right =
            std::clamp(static_cast<int>(std::max(anchor.x, current.x)), 0,
                       snapshot->width);
        const int bottom =
            std::clamp(static_cast<int>(std::max(anchor.y, current.y)), 0,
                       snapshot->height);
        return {left, top, right, bottom};
    }

    void paint(HWND hwnd) const {
        PAINTSTRUCT ps{};
        HDC windowDc = BeginPaint(hwnd, &ps);
        if (windowDc == nullptr || snapshot == nullptr) {
            EndPaint(hwnd, &ps);
            return;
        }

        HDC backDc = snapshot->backDc.hdc;
        BitBlt(backDc, 0, 0, snapshot->width, snapshot->height,
               snapshot->dimDc.hdc, 0, 0, SRCCOPY);

        const RECT sel = normalized_selection();
        const int selW = sel.right - sel.left;
        const int selH = sel.bottom - sel.top;
        if (dragging && selW > 0 && selH > 0) {
            BitBlt(backDc, sel.left, sel.top, selW, selH, snapshot->rawDc.hdc,
                   sel.left, sel.top, SRCCOPY);

            UniqueBrush outerBrush(RGB(255, 255, 255));
            UniqueBrush innerBrush(RGB(56, 152, 236));
            RECT borderRect = sel;
            FrameRect(backDc, &borderRect, outerBrush.brush);
            InflateRect(&borderRect, -1, -1);
            if (borderRect.right > borderRect.left &&
                borderRect.bottom > borderRect.top) {
                FrameRect(backDc, &borderRect, innerBrush.brush);
            }
        }

        // Draw subtle instructions badge on the primary monitor top center.
        POINT primaryOrigin{0, 0};
        HMONITOR primaryMon =
            MonitorFromPoint(primaryOrigin, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (primaryMon != nullptr && GetMonitorInfoW(primaryMon, &mi)) {
            const int monCenterX =
                ((mi.rcMonitor.left + mi.rcMonitor.right) / 2) -
                snapshot->virtualX;
            const int monTop = (mi.rcMonitor.top - snapshot->virtualY) + 20;
            RECT badgeRect{monCenterX - 230, monTop, monCenterX + 230,
                           monTop + 34};
            UniqueBrush badgeBg(RGB(24, 24, 28));
            UniqueBrush badgeBorder(RGB(90, 165, 240));
            FillRect(backDc, &badgeRect, badgeBg.brush);
            FrameRect(backDc, &badgeRect, badgeBorder.brush);

            const int prevBkMode = SetBkMode(backDc, TRANSPARENT);
            const COLORREF prevColor =
                SetTextColor(backDc, RGB(244, 244, 246));
            DrawTextW(
                backDc,
                L"Drag over text to read  \x2022  Esc or Right-Click to cancel",
                -1, &badgeRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SetTextColor(backDc, prevColor);
            SetBkMode(backDc, prevBkMode);
        }

        BitBlt(windowDc, 0, 0, snapshot->width, snapshot->height, backDc, 0, 0,
               SRCCOPY);
        EndPaint(hwnd, &ps);
    }
};

LRESULT CALLBACK overlay_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam,
                                  LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    auto* state = reinterpret_cast<OverlayState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (state == nullptr) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            return TRUE;
        case WM_PAINT:
            state->paint(hwnd);
            return 0;
        case WM_LBUTTONDOWN:
            state->dragging = true;
            state->anchor = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            state->current = state->anchor;
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE:
            if (state->dragging) {
                state->current = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (state->dragging) {
                state->dragging = false;
                state->current = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                if (GetCapture() == hwnd) {
                    ReleaseCapture();
                }
                const RECT sel = state->normalized_selection();
                const int selW = sel.right - sel.left;
                const int selH = sel.bottom - sel.top;
                if (selW >= kMinSelectionPixels && selH >= kMinSelectionPixels) {
                    state->accepted = true;
                } else {
                    state->cancelled = true;
                }
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_RBUTTONUP:
        case WM_CANCELMODE:
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            state->dragging = false;
            state->cancelled = true;
            DestroyWindow(hwnd);
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                if (GetCapture() == hwnd) {
                    ReleaseCapture();
                }
                state->dragging = false;
                state->cancelled = true;
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_CAPTURECHANGED:
            if (state->dragging && !state->accepted && !state->cancelled) {
                state->dragging = false;
                state->cancelled = true;
                DestroyWindow(hwnd);
            }
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

bool ensure_overlay_class_registered(HINSTANCE instance) {
    WNDCLASSEXW existing{};
    existing.cbSize = sizeof(existing);
    if (GetClassInfoExW(instance, kOverlayClassName, &existing)) {
        return true;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = overlay_wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    wc.lpszClassName = kOverlayClassName;
    return RegisterClassExW(&wc) != 0;
}

}  // namespace

std::optional<CapturedImage> WindowsScreenCapturer::capture_selection() {
    DesktopSnapshot snapshot;
    if (!capture_virtual_desktop(snapshot)) {
        throw ReaderError(ReaderErrorCode::CaptureFailed,
                          "Could not capture the Windows desktop surface.");
    }

    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!ensure_overlay_class_registered(instance)) {
        throw ReaderError(ReaderErrorCode::CaptureFailed,
                          "Could not register screen capture overlay class.");
    }

    OverlayState state;
    state.snapshot = &snapshot;

    HWND overlay = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kOverlayClassName,
        L"Dyslexia Reader Screen Capture", WS_POPUP | WS_VISIBLE,
        snapshot.virtualX, snapshot.virtualY, snapshot.width, snapshot.height,
        nullptr, nullptr, instance, &state);
    if (overlay == nullptr) {
        throw ReaderError(ReaderErrorCode::CaptureFailed,
                          "Could not create screen capture overlay window.");
    }

    SetForegroundWindow(overlay);
    SetFocus(overlay);

    MSG msg{};
    bool sawQuit = false;
    WPARAM quitCode = 0;
    while (IsWindow(overlay)) {
        const BOOL res = GetMessageW(&msg, nullptr, 0, 0);
        if (res <= 0) {
            if (res == 0) {
                sawQuit = true;
                quitCode = msg.wParam;
            }
            if (IsWindow(overlay)) {
                DestroyWindow(overlay);
            }
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (sawQuit) {
        PostQuitMessage(static_cast<int>(quitCode));
    }

    if (!state.accepted || state.cancelled) {
        return std::nullopt;
    }

    const RECT sel = state.normalized_selection();
    const int selW = sel.right - sel.left;
    const int selH = sel.bottom - sel.top;
    if (selW < kMinSelectionPixels || selH < kMinSelectionPixels) {
        return std::nullopt;
    }

    const RECT desktopRect{snapshot.virtualX + sel.left,
                           snapshot.virtualY + sel.top,
                           snapshot.virtualX + sel.right,
                           snapshot.virtualY + sel.bottom};
    float scaleX = 1.0F;
    float scaleY = 1.0F;
    query_monitor_scale(desktopRect, scaleX, scaleY);

    CapturedImage image;
    image.width = static_cast<std::uint32_t>(selW);
    image.height = static_cast<std::uint32_t>(selH);
    image.strideBytes = static_cast<std::uint32_t>(selW * 4);
    image.pixelFormat = PixelFormat::Bgra8;
    image.scaleX = scaleX;
    image.scaleY = scaleY;
    image.desktopBounds = {desktopRect.left, desktopRect.top, selW, selH};
    image.pixels.resize(static_cast<std::size_t>(image.strideBytes) *
                        static_cast<std::size_t>(image.height));

    const std::size_t srcStride = static_cast<std::size_t>(snapshot.width) * 4;
    const std::size_t dstStride = static_cast<std::size_t>(image.strideBytes);
    for (int row = 0; row < selH; ++row) {
        const std::uint8_t* srcRow =
            snapshot.rawPixels +
            (static_cast<std::size_t>(sel.top + row) * srcStride) +
            (static_cast<std::size_t>(sel.left) * 4);
        std::uint8_t* dstRow =
            image.pixels.data() + (static_cast<std::size_t>(row) * dstStride);
        std::memcpy(dstRow, srcRow, dstStride);
    }

    return image;
}

}  // namespace dyslexia_reader::windows
