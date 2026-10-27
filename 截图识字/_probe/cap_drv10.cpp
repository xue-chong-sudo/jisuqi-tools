// cap_drv10.cpp —— 右键菜单「1 复制文字 / 2 关闭」+ 2 秒复制提示
//
// 判据：
//   ① 认出词之后右键 → 弹出菜单窗（TrackPopupMenu 的类名是 "#32768"）
//   ② 选「1 复制文字」→ 剪贴板里是**文字**、没有图；屏幕出现 OCRCToast 提示窗
//   ③ 提示窗约 2 秒后自己消失
//   ④ 选「2 关闭」→ 遮罩消失、剪贴板没被写
#include <windows.h>
#include <cstdio>

static const wchar_t* kHostClass = L"JisuQiShotHost";
static const wchar_t* kMaskClass = L"OCRCap";
static const wchar_t* kTbClass   = L"OCRCapTb";
static const wchar_t* kToastClass = L"OCRCapToast";

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what);
    if (!ok) ++g_fail;
}

static HWND FindByClass(const wchar_t* cls, DWORD pidFilter) {
    struct Ctx { const wchar_t* cls; HWND found; DWORD pid; } ctx{ cls, nullptr, pidFilter };
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        wchar_t buf[128] = { 0 };
        GetClassNameW(h, buf, 128);
        if (wcscmp(buf, c->cls) != 0) return TRUE;
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        if (c->pid && pid != c->pid) return TRUE;
        c->found = h; return FALSE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}
static HWND WaitClass(const wchar_t* cls, DWORD pid, int ms) {
    for (int t = 0; t < ms; t += 20) {
        if (HWND h = FindByClass(cls, pid)) return h;
        Sleep(20);
    }
    return nullptr;
}

static bool ClipHasText(wchar_t* out, int cap) {
    if (out) out[0] = 0;
    if (!OpenClipboard(nullptr)) return false;
    bool ok = false;
    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) {
            const wchar_t* s = (const wchar_t*)GlobalLock(h);
            if (s) { if (out) wcsncpy(out, s, cap - 1); ok = true; GlobalUnlock(h); }
        }
    }
    CloseClipboard();
    return ok;
}
static bool ClipHasDib() {
    if (!OpenClipboard(nullptr)) return false;
    const bool ok = IsClipboardFormatAvailable(CF_DIB) != 0;
    CloseClipboard();
    return ok;
}

static HWND g_tw = nullptr;
static void MakeTextWindow(int x, int y, int w, int h) {
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
        wc.lpszClassName = L"Drv10Text";
        RegisterClassW(&wc);
        reg = true;
    }
    g_tw = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"Drv10Text", L"",
                           WS_POPUP, x, y, w, h, nullptr, nullptr,
                           GetModuleHandleW(nullptr), nullptr);
    ShowWindow(g_tw, SW_SHOWNOACTIVATE);
    UpdateWindow(g_tw);
    HDC dc = GetDC(g_tw);
    RECT rc{ 0, 0, w, h };
    HBRUSH b = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(dc, &rc, b);
    DeleteObject(b);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0, 0, 0));
    HFONT f = CreateFontW(-72, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                          DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
    HGDIOBJ of = SelectObject(dc, f);
    DrawTextW(dc, L"MENU 2024", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, of);
    DeleteObject(f);
    ReleaseDC(g_tw, dc);
    Sleep(150);
}

// 走一遍：框选 → 框字 → 等识别 → 右键菜单 → 选第 item 项
static bool RunOnce(DWORD pid, UINT trig, HWND host, int item,
                    const char* tag, bool& menuSeen) {
    if (OpenClipboard(nullptr)) { EmptyClipboard(); CloseClipboard(); }
    PostMessageW(host, trig, 0, 0);
    HWND mask = WaitClass(kMaskClass, pid, 5000);
    if (!mask) { printf("[drv10] %s 遮罩没出现\n", tag); return false; }
    Sleep(250);

    POINT org{ 0, 0 };
    ClientToScreen(mask, &org);
    auto M = [&](UINT m, WPARAM b, int x, int y) {
        SendMessage(mask, m, b, MAKELPARAM(x - org.x, y - org.y));
    };
    // 框出文字窗口
    M(WM_LBUTTONDOWN, MK_LBUTTON, 310, 260);
    Sleep(60);
    M(WM_MOUSEMOVE, MK_LBUTTON, 890, 540);
    Sleep(60);
    M(WM_LBUTTONUP, 0, 890, 540);
    if (!WaitClass(kTbClass, pid, 3000)) { printf("[drv10] %s 工具栏没出现\n", tag); return false; }
    Sleep(200);

    // 默认工具=文字识别，直接在选区内拉子框
    M(WM_LBUTTONDOWN, MK_LBUTTON, 340, 300);
    Sleep(50);
    M(WM_MOUSEMOVE, MK_LBUTTON, 860, 500);
    Sleep(50);
    M(WM_LBUTTONUP, 0, 860, 500);
    Sleep(1500);                 // 等异步识别完成（引擎 ~0.4s，留足）

    // 右键 → 菜单
    // 必须 Post 不能 Send：处理函数里 TrackPopupMenu 是模态循环，不返回，
    // SendMessage 会一直卡住驱动（实测直接把进程挂死）。
    PostMessage(mask, WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(600 - org.x, 400 - org.y));
    Sleep(400);
    HWND menu = FindByClass(L"#32768", pid);
    menuSeen = (menu != nullptr);
    if (!menu) { printf("[drv10] %s 右键没弹出菜单\n", tag); return false; }

    // 必须用**真鼠标点**：菜单项的选中由菜单自己的模态循环处理，投递 WM_KEYDOWN
    // 给它没用（实测选不中）。先取菜单项矩形，再把光标挪到它中间点一下。
    RECT ir{};
    if (!GetMenuItemRect(nullptr, menu, (UINT)(item - 1), &ir)) {
        RECT mr{}; GetWindowRect(menu, &mr);          // 兜底：按两项均分
        const int ih = (mr.bottom - mr.top) / 2;
        ir.left = mr.left; ir.right = mr.right;
        ir.top = mr.top + (item - 1) * ih;
        ir.bottom = ir.top + ih;
    }
    RECT mr{}; GetWindowRect(menu, &mr);
    printf("[drv10]      菜单 (%ld,%ld)-(%ld,%ld) 项%d (%ld,%ld)-(%ld,%ld)\n",
           mr.left, mr.top, mr.right, mr.bottom, item,
           ir.left, ir.top, ir.right, ir.bottom);
    const int mx = (ir.left + ir.right) / 2, my = (ir.top + ir.bottom) / 2;
    SetCursorPos(mx, my);
    Sleep(120);
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    Sleep(60);
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    Sleep(700);
    return true;
}

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HWND host = FindByClass(kHostClass, 0);
    if (!host) { printf("[drv10] 未发现宿主\n"); return 2; }
    DWORD pid = 0; GetWindowThreadProcessId(host, &pid);
    UINT trig = RegisterWindowMessageW(L"JisuQiShotTrigger");
    MakeTextWindow(300, 250, 600, 300);

    // ---------- ① 复制文字 ----------
    bool seen1 = false;
    Check(RunOnce(pid, trig, host, 1, "复制文字", seen1), "① 右键弹出菜单");
    Check(seen1, "① 菜单窗真的出现过");

    wchar_t txt[512] = { 0 };
    const bool hasText = ClipHasText(txt, 512);
    printf("[drv10]      剪贴板文字 = %ls\n", hasText ? txt : L"(无)");
    Check(hasText, "① 复制文字后剪贴板里有文字");
    Check(!ClipHasDib(), "① 只要了文字，没有把图也塞进去");

    HWND toast = FindByClass(kToastClass, pid);
    Check(toast != nullptr, "① 出现「文字已复制到剪贴板」提示窗");
    if (toast) {
        RECT r{}; GetWindowRect(toast, &r);
        printf("[drv10]      提示窗 (%ld,%ld)-(%ld,%ld)\n", r.left, r.top, r.right, r.bottom);
    }
    Sleep(2400);
    Check(FindByClass(kToastClass, pid) == nullptr, "① 提示窗约 2 秒后自己消失");

    // ---------- ② 关闭 ----------
    if (OpenClipboard(nullptr)) { EmptyClipboard(); CloseClipboard(); }
    Sleep(300);
    bool seen2 = false;
    Check(RunOnce(pid, trig, host, 2, "关闭", seen2), "② 右键弹出菜单（第二次）");
    Check(FindByClass(kMaskClass, pid) == nullptr, "② 选「关闭」后遮罩消失");
    Check(!ClipHasText(nullptr, 0), "② 选「关闭」没有写剪贴板");

    if (g_tw) DestroyWindow(g_tw);
    printf("\n%s（失败项 %d）\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
