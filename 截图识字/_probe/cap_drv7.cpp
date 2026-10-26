// cap_drv7.cpp —— 证明"识别期间界面不卡"（异步识别的回归）
//
// 背景：改成异步之前，松手那一刻整个遮罩会僵住 0.33~0.44 秒（引擎调用是同步的），
// 那段时间鼠标变等待圈、点什么都没反应。这条驱动就是钉住"不能僵"。
//
// 判据：框完字之后持续用 SendMessage(WM_NULL) 探针测量 UI 线程的响应延迟。
//   - 同步实现：只要识别在跑，探针会排在后面，延迟 ≈ 剩下的识别时间（几百 ms）
//   - 异步实现：探针随到随回，延迟是亚毫秒级
// 之后再按 ESC，确认文字确实被认出来了（异步不是"不认了"）。
#include <windows.h>
#include <cstdio>
#include <vector>

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

static LARGE_INTEGER g_freq;
static double NowMs() {
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)g_freq.QuadPart;
}

static bool ReadClipText(wchar_t* out, int cap) {
    out[0] = 0;
    if (!OpenClipboard(nullptr)) return false;
    bool ok = false;
    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) {
            const wchar_t* s = (const wchar_t*)GlobalLock(h);
            if (s) { wcsncpy(out, s, cap - 1); out[cap - 1] = 0; ok = true; GlobalUnlock(h); }
        }
    }
    CloseClipboard();
    return ok;
}

static HWND g_tw = nullptr;
// 造一块"屏幕上的文字"：顶层置顶小窗，里面一行大字
static void MakeTextWindow(int x, int y, int w, int h) {
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
        wc.lpszClassName = L"Drv7Text";
        RegisterClassW(&wc);
        reg = true;
    }
    g_tw = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"Drv7Text", L"",
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
    HFONT f = CreateFontW(-96, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                          DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
    HGDIOBJ of = SelectObject(dc, f);
    DrawTextW(dc, L"ASYNC 98765", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, of);
    DeleteObject(f);
    ReleaseDC(g_tw, dc);
    Sleep(120);
}

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    QueryPerformanceFrequency(&g_freq);

    HWND host = FindByClass(kHostClass, 0);
    if (!host) { printf("[drv7] 未发现宿主\n"); return 2; }
    DWORD pid = 0; GetWindowThreadProcessId(host, &pid);
    printf("[drv7] 宿主 pid=%lu\n", pid);

    if (OpenClipboard(nullptr)) { EmptyClipboard(); CloseClipboard(); }
    MakeTextWindow(300, 250, 600, 300);

    UINT trig = RegisterWindowMessageW(L"JisuQiShotTrigger");
    PostMessageW(host, trig, 0, 0);
    HWND mask = WaitClass(kMaskClass, pid, 5000);
    Check(mask != nullptr, "遮罩出现");
    if (!mask) return 1;
    Sleep(250);

    POINT org{ 0, 0 };
    ClientToScreen(mask, &org);
    auto Msg = [&](UINT m, WPARAM b, int x, int y) {
        SendMessage(mask, m, b, MAKELPARAM(x - org.x, y - org.y));
    };
    // 框出文字窗口
    Msg(WM_LBUTTONDOWN, MK_LBUTTON, 310, 260);
    Sleep(60);
    Msg(WM_MOUSEMOVE, MK_LBUTTON, 890, 540);
    Sleep(60);
    Msg(WM_LBUTTONUP, 0, 890, 540);

    HWND tb = WaitClass(kTbClass, pid, 3000);
    Check(tb != nullptr, "工具栏出现（进入调整态）");
    Sleep(250);

    // 默认工具 = 文字识别（用户要求），所以直接在选区里拉子框即可
    double tUp = 0;
    Msg(WM_LBUTTONDOWN, MK_LBUTTON, 340, 300);
    Sleep(50);
    Msg(WM_MOUSEMOVE, MK_LBUTTON, 860, 500);
    Sleep(50);
    tUp = NowMs();
    Msg(WM_LBUTTONUP, 0, 860, 500);

    // ---- 核心判据：识别在后台跑的同时，UI 线程必须还能立刻回消息 ----
    double worst = 0, sum = 0;
    int n = 0;
    const double deadline = NowMs() + 1200;
    while (NowMs() < deadline) {
        double s = NowMs();
        SendMessage(mask, WM_NULL, 0, 0);
        double ms = NowMs() - s;
        if (ms > worst) worst = ms;
        sum += ms;
        ++n;
        Sleep(15);
    }
    printf("[drv7]      UI 探针 %d 次：平均 %.2f ms，最差 %.1f ms\n", n, sum / n, worst);
    Check(worst < 120.0, "识别期间 UI 线程没有被冻住（最差延迟 < 120ms）");
    Check(NowMs() - tUp > 300, "探针覆盖区间确实跨过了识别耗时（>300ms 才有意义）");

    // ---- 异步不等于不认：ESC 收尾后剪贴板里要有文字 ----
    Msg(WM_KEYDOWN, VK_ESCAPE, 0, 0);
    Msg(WM_KEYUP, VK_ESCAPE, 0, 0);
    Sleep(1200);

    wchar_t txt[512] = { 0 };
    const bool hasText = ReadClipText(txt, 512);
    printf("[drv7]      剪贴板文字 = %ls\n", hasText ? txt : L"(无)");
    Check(hasText, "异步识别最终仍把文字写进了剪贴板");
    if (hasText) {
        bool hasDigit = wcschr(txt, L'8') != nullptr || wcschr(txt, L'9') != nullptr;
        Check(hasDigit, "认出的文字里含测试串的数字（说明认出的是那块字）");
    }

    if (g_tw) DestroyWindow(g_tw);
    printf("\n%s（失败项 %d）\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
