// cap_drv9.cpp —— 全屏吸附取证
//
// 用户："自动吸附窗口功能能再优化吗？现在全屏不能吸附"。
// 先不猜，直接造一个覆盖整个虚拟屏的窗口，测两条最可能的含义：
//   A. 单击（不拖）应当整窗选中 → 选区 = 整个虚拟屏
//   B. 拖到距屏幕边 6px 处松手 → 应吸附到边（否则永远框不准整屏）
// 判据都用剪贴板里 CF_DIB 的尺寸（唯一能从外部观测到的选区真相）。
#include <windows.h>
#include <cstdio>
#include <cstring>

static const wchar_t* kHostClass = L"JisuQiShotHost";
static const wchar_t* kMaskClass = L"OCRCap";
static const wchar_t* kTbClass   = L"OCRCapTb";

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
        HWND h = FindByClass(cls, pid);
        if (h) return h;
        Sleep(20);
    }
    return nullptr;
}

static bool ClipDibSize(int& w, int& h) {
    w = h = 0;
    if (!OpenClipboard(nullptr)) return false;
    bool ok = false;
    if (IsClipboardFormatAvailable(CF_DIB)) {
        HANDLE hd = GetClipboardData(CF_DIB);
        if (hd) {
            const BITMAPINFOHEADER* bi = (const BITMAPINFOHEADER*)GlobalLock(hd);
            if (bi) { w = (int)bi->biWidth; h = (int)bi->biHeight; ok = true; GlobalUnlock(hd); }
        }
    }
    CloseClipboard();
    return ok;
}

static void Msg(HWND mask, POINT org, UINT m, WPARAM btn, int x, int y) {
    SendMessage(mask, m, btn, MAKELPARAM(x - org.x, y - org.y));
}

int main(int argc, char** argv) {
    // 传 desktop = 不造全屏窗口，测"裸桌面"下的吸附（桌面是 Shell 窗口，被
    // IsShellChrome 排除，g_wins 里一个可吸附窗口都没有）。
    const bool desktopOnly = (argc > 1 && strcmp(argv[1], "desktop") == 0);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    printf("[drv9] 虚拟屏 (%d,%d) %dx%d\n", vx, vy, vw, vh);

    HWND host = FindByClass(kHostClass, 0);
    if (!host) { printf("[drv9] 未发现宿主\n"); return 2; }
    DWORD pid = 0; GetWindowThreadProcessId(host, &pid);

    // 造"全屏窗口"：盖住整个虚拟屏，纯白底（方便 OCR 无关，只为几何）
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
        wc.lpszClassName = L"Drv9Fullscreen";
        RegisterClassW(&wc);
        reg = true;
    }
    HWND fs = CreateWindowExW(WS_EX_TOPMOST, L"Drv9Fullscreen", L"",
                              WS_POPUP, vx, vy, vw, vh, nullptr, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
    if (!desktopOnly) ShowWindow(fs, SW_SHOWNOACTIVATE);
    if (!desktopOnly) UpdateWindow(fs);
    Sleep(200);

    UINT trig = RegisterWindowMessageW(L"JisuQiShotTrigger");

    // ---------- A. 单击应整窗选中 ----------
    if (OpenClipboard(nullptr)) { EmptyClipboard(); CloseClipboard(); }
    PostMessageW(host, trig, 0, 0);
    HWND mask = WaitClass(kMaskClass, pid, 5000);
    Check(mask != nullptr, "A 遮罩出现");
    if (!mask) return 1;
    Sleep(250);
    POINT org{ 0, 0 };
    ClientToScreen(mask, &org);

    const int cx = vx + vw / 2, cy = vy + vh / 2;
    Msg(mask, org, WM_MOUSEMOVE, 0, cx, cy);
    Sleep(120);
    Msg(mask, org, WM_LBUTTONDOWN, MK_LBUTTON, cx, cy);
    Sleep(60);
    Msg(mask, org, WM_LBUTTONUP, 0, cx, cy);
    Sleep(250);
    Check(WaitClass(kTbClass, pid, 2000) != nullptr, "A 单击后进入调整态（工具栏出现）");

    Msg(mask, org, WM_KEYDOWN, VK_ESCAPE, 0, 0);
    Msg(mask, org, WM_KEYUP, VK_ESCAPE, 0, 0);
    Sleep(700);
    int w1 = 0, h1 = 0;
    const bool got1 = ClipDibSize(w1, h1);
    printf("[drv9]      A 单击整窗 → 选区 %dx%d（期望 %dx%d）\n", w1, h1, vw, vh);
    Check(got1, "A 剪贴板拿到图");
    if (desktopOnly) {
        // 桌面并不是"空的"：屏幕上本来就有别的窗口，单击选中它们才是正确行为。
        // 这里只记录实测值，不判定（整屏选中由 B 的拖拽吸附覆盖）。
        printf("[drv9]      （桌面模式：单击命中了屏幕上真实存在的窗口，属正确行为，不作判定）\n");
    } else {
        Check(w1 == vw && h1 == vh, "A 单击全屏窗口 = 选中整个屏幕");
    }

    // ---------- B. 拖到距边 6px 应吸附到边 ----------
    Sleep(400);
    if (OpenClipboard(nullptr)) { EmptyClipboard(); CloseClipboard(); }
    PostMessageW(host, trig, 0, 0);
    mask = WaitClass(kMaskClass, pid, 5000);
    Check(mask != nullptr, "B 遮罩出现");
    if (!mask) return 1;
    Sleep(250);
    ClientToScreen(mask, &org);

    const int x0 = vx + 20, y0 = vy + 20;             // 起点
    const int x1 = vx + vw - 7, y1 = vy + vh - 7;     // 终点：距右下边 7px（在 10px 吸附阈值内）
    Msg(mask, org, WM_LBUTTONDOWN, MK_LBUTTON, x0, y0);
    Sleep(60);
    Msg(mask, org, WM_MOUSEMOVE, MK_LBUTTON, x1, y1);
    Sleep(80);
    Msg(mask, org, WM_LBUTTONUP, 0, x1, y1);
    Sleep(250);
    Msg(mask, org, WM_KEYDOWN, VK_ESCAPE, 0, 0);
    Msg(mask, org, WM_KEYUP, VK_ESCAPE, 0, 0);
    Sleep(700);
    int w2 = 0, h2 = 0;
    const bool got2 = ClipDibSize(w2, h2);
    printf("[drv9]      B 拖到距边7px → 选区 %dx%d（起点内缩 20，期望 %dx%d 若右/下吸附成功）\n",
           w2, h2, vw - 20, vh - 20);
    Check(got2, "B 剪贴板拿到图");
    Check(w2 == vw - 20 && h2 == vh - 20, "B 右下角吸附到了屏幕边");

    // （C 项"拖把手吸附"已由 handle_drag_test 的 CheckHandleSnap 用确定性单测覆盖：
    //   这里靠发鼠标消息点不准把手，信号不可信，故删除。）

    if (fs) DestroyWindow(fs);
    printf("\n%s（失败项 %d）\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
