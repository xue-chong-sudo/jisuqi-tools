// capture.cpp —— Win32 screenshot: light grey veil + transparent hole inside the selected area
// Workflow: 1) capture entire virtual screen into memory, 2) show a light grey
// overlay everywhere EXCEPT the hovered/dragged selection, 3) return the clean
// (non-dimmed) captured region. This matches screenshot tools like WeChat/QQ.
#include "ocraddon_internal.h"
#include <windows.h>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdlib>   // std::abs
#include <cstdio>    // std::snprintf

// GET_X_LPARAM/GET_Y_LPARAM normally live in <windowsx.h>, but that header
// tends to clash with other definitions. Define them locally to stay safe.
#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif

static bool g_dpi = false;
static void EnsureDpi() {
    if (g_dpi) return;
    // SetProcessDpiAwareness 位于 shcore.dll（不是 user32）。user32 上没有该导出，
    // 旧代码会静默失败并回退到 SetProcessDPIAware()，功能尚可但应直接找对模块。
    HMODULE sc = LoadLibraryA("shcore.dll");
    if (sc) {
        auto f = (HRESULT(WINAPI*)(int))GetProcAddress(sc, "SetProcessDpiAwareness");
        if (f) f(2 /*PROCESS_PER_MONITOR_DPI_AWARE*/);
        FreeLibrary(sc);
    } else {
        SetProcessDPIAware();
    }
    g_dpi = true;
}

// 截图遮罩层状态（提前定义，供 EnumWnd 使用虚拟屏幕尺寸过滤窗口）。
struct Cap {
    HWND w = nullptr;
    bool drag = false;
    POINT start = {0,0};
    POINT cur = {0,0}; // current mouse pos in virtual-screen coords
    RECT sel = {0,0,0,0};
    int vx = 0, vy = 0, vw = 0, vh = 0; // virtual-screen origin and size
    bool done = false, captured = false;
    HBITMAP hDib = nullptr;
    HDC hDc = nullptr;
    void* bits = nullptr;
    std::vector<unsigned char> screen; // clean original screen (BGRA, top-down)
    std::vector<unsigned char> dim;    // dimmed screen (BGRA, top-down), base for overlay
};
static Cap g;

// Enumerate visible top-level windows; used for hover-highlight and snap.
static std::vector<RECT> g_wins;

// DwmGetWindowAttribute 在整个会话里只 LoadLibrary 一次（枚举每个窗口都加载/释放
// 是毫无必要的开销，且 EnumWindows 回调里频繁加载 DLL 是不可取的做法）。
static HRESULT(WINAPI* g_dwmGetWindowAttribute)(HWND, DWORD, PVOID, DWORD) = nullptr;
static bool g_dwmReady = false;
static void EnsureDwm() {
    if (g_dwmReady) return;
    g_dwmReady = true;
    HMODULE dwm = LoadLibraryA("dwmapi.dll");
    if (dwm) {
        g_dwmGetWindowAttribute =
            (HRESULT(WINAPI*)(HWND, DWORD, PVOID, DWORD))GetProcAddress(dwm, "DwmGetWindowAttribute");
        // 不 FreeLibrary：保持模块映射到进程结束即可，避免重复加载。
    }
}

static BOOL CALLBACK EnumWnd(HWND hw, LPARAM) {
    if (!IsWindowVisible(hw)) return TRUE;
    if (GetWindowLong(hw, GWL_STYLE) & WS_MINIMIZE) return TRUE;
    if (GetWindowLong(hw, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;

    // 过滤桌面窗口（Progman/WorkerW），避免鼠标在桌面上时高亮整个屏幕。
    wchar_t cls[64] = {0};
    if (GetClassNameW(hw, cls, 64) > 0) {
        if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0) return TRUE;
    }

    RECT r; bool ok = false;
    EnsureDwm();
    if (g_dwmGetWindowAttribute) {
        RECT er;
        if (SUCCEEDED(g_dwmGetWindowAttribute(hw, 9 /*DWMWA_EXTENDED_FRAME_BOUNDS*/, &er, sizeof(er)))) {
            r = er; ok = true;
        }
    }
    if (!ok && !GetWindowRect(hw, &r)) return TRUE;
    int rw = r.right - r.left, rh = r.bottom - r.top;
    if (rw <= 1 || rh <= 1) return TRUE;

    // 不再按"面积大于虚拟屏幕 80%"过滤窗口。原因：
    //   1) 真正导致"高亮整个屏幕"的是桌面外壳窗口 Progman/WorkerW，已在上方面按类名过滤；
    //   2) 若按面积过滤，会把"最大化"（接近全屏）的真实应用窗口也排除掉，导致鼠标
    //      悬停/单击无法选中最大化窗口——那恰恰是最常用的"框窗口"场景。
    //   真实全屏应用（如游戏）本就铺满屏幕，悬停高亮它=整屏高亮，符合直觉，无需排除。
    //   因此仅过滤桌面外壳窗口，保留全部真实顶层窗口供悬停识别与单击捕获。

    g_wins.push_back(r);
    return TRUE;
}

static RECT Norm(POINT a, POINT b) {
    RECT r;
    r.left   = std::min(a.x, b.x); r.top    = std::min(a.y, b.y);
    r.right  = std::max(a.x, b.x); r.bottom = std::max(a.y, b.y);
    return r;
}

// Snap a point to nearby window edges (10px threshold).
static void Snap(POINT& p) {
    const int t = 10;
    for (const RECT& w : g_wins) {
        if (std::abs(p.x - w.left)   < t) p.x = w.left;
        else if (std::abs(p.x - w.right) < t) p.x = w.right;
        if (std::abs(p.y - w.top)    < t) p.y = w.top;
        else if (std::abs(p.y - w.bottom) < t) p.y = w.bottom;
    }
}

// Find the smallest window that contains point p (innermost child-like region).
static RECT FindHoveredWindow(POINT p) {
    RECT best{0,0,0,0};
    bool found = false;
    long bestArea = LONG_MAX;
    for (const RECT& w : g_wins) {
        if (PtInRect(&w, p)) {
            long area = (long)(w.right - w.left) * (w.bottom - w.top);
            if (!found || area < bestArea) {
                best = w; bestArea = area; found = true;
            }
        }
    }
    return best;
}

// Direct BGRA pixel helpers: we need to control the alpha channel ourselves
// because GDI's DrawRectOutline only touches RGB and leaves alpha unchanged.
static inline void SetPixelBGRA(int x, int y, COLORREF c, BYTE a) {
    if (x < 0 || x >= g.vw || y < 0 || y >= g.vh) return;
    BYTE* p = (BYTE*)g.bits + ((size_t)y * g.vw + x) * 4;
    p[0] = GetBValue(c);
    p[1] = GetGValue(c);
    p[2] = GetRValue(c);
    p[3] = a;
}

static void DrawRectOutlineBGRA(const RECT& r, int xoff, int yoff,
                                COLORREF c, int width) {
    int left   = r.left   - xoff;
    int top    = r.top    - yoff;
    int right  = r.right  - xoff;   // exclusive
    int bottom = r.bottom - yoff;   // exclusive
    if (right <= left || bottom <= top) return;
    for (int x = left; x < right; ++x) {
        for (int w = 0; w < width; ++w) {
            SetPixelBGRA(x, top + w, c, 255);
            SetPixelBGRA(x, bottom - 1 - w, c, 255);
        }
    }
    for (int y = top; y < bottom; ++y) {
        for (int w = 0; w < width; ++w) {
            SetPixelBGRA(left + w, y, c, 255);
            SetPixelBGRA(right - 1 - w, y, c, 255);
        }
    }
}

static void FillRectBGRA(int x0, int y0, int x1, int y1, COLORREF c, BYTE a) {
    x0 = std::max(x0, 0); y0 = std::max(y0, 0);
    x1 = std::min(x1, g.vw); y1 = std::min(y1, g.vh);
    if (x1 <= x0 || y1 <= y0) return;
    BYTE b = GetBValue(c), gc = GetGValue(c), rc = GetRValue(c);
    for (int y = y0; y < y1; ++y) {
        BYTE* row = (BYTE*)g.bits + ((size_t)y * g.vw + x0) * 4;
        for (int x = x0; x < x1; ++x) {
            row[0] = b; row[1] = gc; row[2] = rc; row[3] = a;
            row += 4;
        }
    }
}

static void Redraw() {
    // 1. Start from the pre-computed light-grey veil.
    if (g.dim.empty()) return;
    std::memcpy(g.bits, g.dim.data(), (size_t)g.vw * g.vh * 4);

    // 2. Decide the "hole" region: current drag selection, or hovered window.
    RECT hole{0, 0, 0, 0};
    if (g.drag && g.sel.right > g.sel.left && g.sel.bottom > g.sel.top) {
        hole = g.sel;
    } else {
        hole = FindHoveredWindow(g.cur);
    }

    // 3. Punch a hole in the overlay so the original desktop shows through.
    // 直接把选区像素写成不透明的干净桌面原图（BGRA，Alpha=255），遮罩不再
    // 覆盖该区域。这正符合说明文档“选区内显示干净的桌面内容”。
    // （旧实现用 dst = src/255、Alpha=1 的整数除法：0~254 全部变 0，导致选区
    //  实际是黑斑，与“透出干净桌面”的意图相反。）
    if (hole.right > hole.left && hole.bottom > hole.top) {
        int x0 = std::max((int)(hole.left - g.vx), 0);
        int y0 = std::max((int)(hole.top - g.vy), 0);
        int x1 = std::min((int)(hole.right - g.vx), g.vw);
        int y1 = std::min((int)(hole.bottom - g.vy), g.vh);
        for (int y = y0; y < y1; ++y) {
            BYTE* dst = (BYTE*)g.bits + ((size_t)y * g.vw + x0) * 4;
            const BYTE* src = &g.screen[((size_t)y * g.vw + x0) * 4];
            for (int x = x0; x < x1; ++x) {
                dst[0] = src[0]; // B
                dst[1] = src[1]; // G
                dst[2] = src[2]; // R
                dst[3] = 255;    // A：不透明，选区透出干净桌面
                dst += 4; src += 4;
            }
        }
    }

    // 4. Draw the selection / hover outline (opaque RGBA, so it stays crisp).
    if (!g.drag) {
        if (hole.right > hole.left) {
            DrawRectOutlineBGRA(hole, g.vx, g.vy, RGB(0, 180, 255), 2);
        }
    } else if (g.sel.right > g.sel.left && g.sel.bottom > g.sel.top) {
        DrawRectOutlineBGRA(g.sel, g.vx, g.vy, RGB(230, 30, 30), 2);

        char sz[64];
        std::snprintf(sz, sizeof(sz), "%d x %d",
                      g.sel.right - g.sel.left, g.sel.bottom - g.sel.top);
        SetBkMode(g.hDc, TRANSPARENT);
        SetTextColor(g.hDc, RGB(255, 255, 255));
        HFONT f = CreateFontW(18, 0, 0, 0, FW_BOLD, 0, 0, 0,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                              DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HGDIOBJ oldf = SelectObject(g.hDc, f);
        int tx = g.sel.left - g.vx;
        int ty = g.sel.top - g.vy - 22;
        if (ty < 0) ty = g.sel.top - g.vy + 4;
        SIZE ts;
        GetTextExtentPoint32A(g.hDc, sz, (int)std::strlen(sz), &ts);
        const int pad = 4;
        // Dark, opaque label background so white text is readable over any desktop.
        FillRectBGRA(tx - pad, ty - pad,
                     tx + ts.cx + pad, ty + ts.cy + pad,
                     RGB(30, 30, 30), 255);
        TextOutA(g.hDc, tx, ty, sz, (int)std::strlen(sz));
        SelectObject(g.hDc, oldf);
        DeleteObject(f);
    }

    // 5. Push the overlay to screen using per-pixel alpha.
    POINT ppt = {g.vx, g.vy};
    SIZE szw = {g.vw, g.vh};
    POINT src = {0, 0};
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g.w, NULL, &ppt, &szw, g.hDc, &src, 0, &bf, ULW_ALPHA);
}

static LRESULT CALLBACK CapWnd(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_LBUTTONDOWN: {
            POINT p;
            p.x = GET_X_LPARAM(l) + g.vx;
            p.y = GET_Y_LPARAM(l) + g.vy;
            g.drag = true; g.start = p; g.cur = p;
            g.sel = Norm(g.start, g.start);
            SetCapture(h); Redraw(); break;
        }
        case WM_MOUSEMOVE: {
            POINT p;
            p.x = GET_X_LPARAM(l) + g.vx;
            p.y = GET_Y_LPARAM(l) + g.vy;
            g.cur = p;
            if (g.drag) {
                Snap(p);
                g.sel = Norm(g.start, p);
            }
            Redraw();
            break;
        }
        case WM_LBUTTONUP:
            if (g.drag) {
                g.drag = false; ReleaseCapture();
                int dx = std::abs(g.start.x - g.cur.x);
                int dy = std::abs(g.start.y - g.cur.y);
                if (dx < 5 && dy < 5) {
                    // Single click: capture the whole hovered window.
                    RECT hov = FindHoveredWindow(g.start);
                    if (hov.right > hov.left) {
                        g.sel = hov;
                        g.captured = true;
                    }
                } else {
                    // Manual drag: capture the selection box.
                    g.captured = true;
                }
                g.done = true; DestroyWindow(h);
            }
            break;
        case WM_RBUTTONDOWN:
            // Right-click cancels capture, just like ESC.
            g.captured = false; g.done = true; DestroyWindow(h);
            break;
        case WM_KEYDOWN:
            if (w == VK_ESCAPE) { g.captured = false; g.done = true; DestroyWindow(h); }
            break;
        case WM_DESTROY: PostQuitMessage(0); break;
    }
    return DefWindowProc(h, m, w, l);
}

CaptureImage CaptureRegionInteractive() {
    EnsureDpi();
    // 先取虚拟屏幕尺寸，供 EnumWnd 过滤桌面/过大窗口时使用。
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    g = Cap{};
    g.vx = vx; g.vy = vy; g.vw = vw; g.vh = vh;

    g_wins.clear();
    EnumWindows(EnumWnd, 0);

    if (g.vw <= 0 || g.vh <= 0) return CaptureImage{};

    // Capture the entire virtual screen BEFORE showing the overlay.
    {
        HDC hDisp = CreateDC(TEXT("DISPLAY"), NULL, NULL, NULL);
        HDC hMem = CreateCompatibleDC(hDisp);
        HBITMAP hBmp = CreateCompatibleBitmap(hDisp, g.vw, g.vh);
        HGDIOBJ old = SelectObject(hMem, hBmp);
        BOOL blitOK = BitBlt(hMem, 0, 0, g.vw, g.vh, hDisp, g.vx, g.vy, SRCCOPY);
        // BitBlt 在某些 DWM/远程桌面/高 DPI 组合下会返回 0 或得到空白。
        // 若失败，回退到 PrintWindow(EnumDisplayMonitors) 的方式：对每个顶层窗口
        // 截取客户区并合成到虚拟屏幕上。
        if (!blitOK) {
            RECT vr{ g.vx, g.vy, g.vx + g.vw, g.vy + g.vh };
            HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(hMem, &vr, black);
            DeleteObject(black);
            auto paintWnd = [&](HWND hw) -> BOOL {
                if (!IsWindowVisible(hw)) return TRUE;
                RECT wr; if (!GetWindowRect(hw, &wr)) return TRUE;
                RECT inter;
                if (!IntersectRect(&inter, &wr, &vr)) return TRUE;
                // 调整 DC 原点，使 PrintWindow 把窗口内容画到虚拟屏幕的对应位置。
                int orgX = wr.left - g.vx;
                int orgY = wr.top - g.vy;
                POINT oldOrg;
                SetWindowOrgEx(hMem, -orgX, -orgY, &oldOrg);
                // 使用 PW_RENDERFULLCONTENT 尽可能拿到 DWM 合成后的内容。
                PrintWindow(hw, hMem, 2 /*PW_RENDERFULLCONTENT*/);
                SetWindowOrgEx(hMem, oldOrg.x, oldOrg.y, NULL);
                return TRUE;
            };
            EnumWindows([](HWND hw, LPARAM lp) -> BOOL {
                return (*reinterpret_cast<decltype(paintWnd)*>(lp))(hw);
            }, reinterpret_cast<LPARAM>(&paintWnd));
        }

        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = g.vw;
        bi.bmiHeader.biHeight = -g.vh;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        g.screen.resize((size_t)g.vw * g.vh * 4);
        GetDIBits(hMem, hBmp, 0, g.vh, g.screen.data(), &bi, DIB_RGB_COLORS);

        SelectObject(hMem, old);
        DeleteObject(hBmp);
        DeleteDC(hMem);
        DeleteDC(hDisp);
    }
    if (g.screen.empty()) return CaptureImage{};

    // Pre-compute dimmed overlay base for the area OUTSIDE the selection.
    // We use per-pixel alpha with AC_SRC_ALPHA, so RGB must be premultiplied.
    // Outside: alpha=255 (fully opaque to this overlay), RGB = desktop * dimMul/255.
    // Blended result = desktop * dimMul/255, giving a clearly visible dark veil.
    g.dim = g.screen;
    unsigned char* dp = g.dim.data();
    const int dimMul = 140; // darken to 55%: result = desktop * 0.55
    for (int i = 0; i < g.vw * g.vh; i++) {
        dp[0] = (unsigned char)((dp[0] * dimMul) / 255); // B
        dp[1] = (unsigned char)((dp[1] * dimMul) / 255); // G
        dp[2] = (unsigned char)((dp[2] * dimMul) / 255); // R
        dp[3] = 255;                                     // A
        dp += 4;
    }

    // 32-bit DIB for the layered overlay (same size as virtual screen).
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = g.vw;
    bmi.bmiHeader.biHeight = -g.vh;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    HDC hScreen = GetDC(NULL);
    g.hDib = CreateDIBSection(hScreen, &bmi, DIB_RGB_COLORS, &g.bits, NULL, 0);
    g.hDc = CreateCompatibleDC(hScreen);
    SelectObject(g.hDc, g.hDib);
    ReleaseDC(NULL, hScreen);

    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = CapWnd;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_CROSS);
        wc.lpszClassName = L"OCRCap";
        RegisterClassW(&wc);
        reg = true;
    }
    g.w = CreateWindowEx(WS_EX_LAYERED | WS_EX_TOPMOST, L"OCRCap", NULL,
                         WS_POPUP, g.vx, g.vy, g.vw, g.vh,
                         NULL, NULL, GetModuleHandle(NULL), NULL);
    if (!g.w) {
        printf("[cap] CreateWindowEx failed, err=%lu\n", GetLastError());
        DeleteObject(g.hDib); DeleteDC(g.hDc);
        return CaptureImage{};
    }
    ShowWindow(g.w, SW_SHOW);
    SetForegroundWindow(g.w);
    Redraw();

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        if (g.done) break;
    }

    DeleteObject(g.hDib);
    DeleteDC(g.hDc);

    if (!g.captured) return CaptureImage{};

    // Clamp selection to virtual screen bounds.
    RECT s = g.sel;
    if (s.left   < g.vx) s.left   = g.vx;
    if (s.top    < g.vy) s.top    = g.vy;
    if (s.right  > g.vx + g.vw) s.right = g.vx + g.vw;
    if (s.bottom > g.vy + g.vh) s.bottom = g.vy + g.vh;
    int rw = s.right - s.left, rh = s.bottom - s.top;
    if (rw < 2 || rh < 2) return CaptureImage{};

    // Crop from the CLEAN original screen buffer (no overlay, no dimming).
    CaptureImage out;
    out.width = rw; out.height = rh;
    out.rgb.resize((size_t)rw * rh * 3);
    for (int y = 0; y < rh; y++) {
        int srcY = (s.top - g.vy) + y;
        const unsigned char* srcRow = &g.screen[(size_t)srcY * g.vw * 4];
        unsigned char* dstRow = &out.rgb[(size_t)y * rw * 3];
        for (int x = 0; x < rw; x++) {
            int srcX = (s.left - g.vx) + x;
            const unsigned char* src = srcRow + srcX * 4;
            dstRow[x * 3 + 0] = src[2]; // R
            dstRow[x * 3 + 1] = src[1]; // G
            dstRow[x * 3 + 2] = src[0]; // B
        }
    }

    g_wins.clear();
    return out;
}
