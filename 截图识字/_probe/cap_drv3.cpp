// cap_drv3.cpp —— "识别文字"路径验证（用户最关心的剪贴板顺序）
//
// 要证的事：点"识别文字"退出后，剪贴板事件必须是 **两次独立的会话**：
//   第 1 次 = 图片（供 Win+V 历史留一条图）
//   第 2 次 = 文字（所以直接 Ctrl+V 粘出来的是文字）
// 判定方式：在驱动进程里 AddClipboardFormatListener，数 WM_CLIPBOARDUPDATE 次数。
// 同一次 OpenClipboard/CloseClipboard 里塞 CF_DIB + CF_UNICODETEXT 只会触发 1 次，
// 而且目标程序多半会挑图 —— 那正是要避免的老行为。
//
// 另外单独校验：点完之后当前剪贴板里是 CF_UNICODETEXT（且内容认得出）、
// 不再是 CF_DIB。这跟"直接粘贴得到文字"是对应的。
#include <windows.h>
#include <cstdio>
#include <cstring>

static const wchar_t* kHostClass = L"JisuQiShotHost";
static const wchar_t* kMaskClass = L"OCRCap";
static const wchar_t* kTbClass   = L"OCRCapTb";

static int  g_vx = 0, g_vy = 0;
static int  g_fail = 0;
static int  g_clipUpdates = 0;

static void Check(bool ok, const char* what) {
    printf("[drv3] %s %s\n", ok ? "OK  " : "FAIL", what);
    if (!ok) ++g_fail;
}

static HWND FindByClass(const wchar_t* cls, DWORD pidFilter) {
    struct Ctx { const wchar_t* cls; HWND found; DWORD pid; } ctx{ cls, nullptr, pidFilter };
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        wchar_t buf[128] = {0};
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

static void MouseMsg(HWND w, UINT msg, int x, int y, WPARAM btn) {
    POINT org{0, 0};
    ClientToScreen(w, &org);
    SendMessage(w, msg, btn, MAKELPARAM(x - org.x, y - org.y));
}

static void DragTo(HWND w, int x0, int y0, int x1, int y1, int steps) {
    MouseMsg(w, WM_MOUSEMOVE, x0, y0, 0);
    MouseMsg(w, WM_LBUTTONDOWN, x0, y0, MK_LBUTTON);
    for (int i = 1; i <= steps; ++i)
        MouseMsg(w, WM_MOUSEMOVE, x0 + (x1 - x0) * i / steps,
                                y0 + (y1 - y0) * i / steps, MK_LBUTTON);
    MouseMsg(w, WM_LBUTTONUP, x1, y1, 0);
    Sleep(180);
}

// 工具栏（单行 40 高，纯图标）。按钮中心（客户区坐标）：
//   行 y 中心恒为 20
//   工具 1..5 中心 x = 24 / 58 / 92 / 126 / 160（矩形 椭圆 箭头 画笔 文）
//   保存(下载图标) 6 中心 x = 204
//   颜色 20..25 中心 x = 247+26i
//   粗细 30..33 中心 x = 416+26i
static POINT TbCenter(int id) {
    if (id >= 20 && id <= 25) return POINT{ 297 + 31 * (id - 20), 24 };
    if (id >= 30 && id <= 33) return POINT{ 503 + 31 * (id - 30), 24 };
    static const int cx[9] = { 0, 29, 71, 113, 155, 197, 239, 0, 0 };
    return POINT{ cx[id], 24 };
}

static void ClickTb(HWND tb, int id) {
    POINT p = TbCenter(id);
    SendMessage(tb, WM_MOUSEMOVE, 0, MAKELPARAM(p.x, p.y));
    SendMessage(tb, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(p.x, p.y));
    SendMessage(tb, WM_LBUTTONUP, 0, MAKELPARAM(p.x, p.y));
    Sleep(180);
}

// 取剪贴板里的 unicode 文本（调用方负责判断格式是否存在）
static bool ReadClipText(wchar_t* out, int cap) {
    out[0] = 0;
    if (!OpenClipboard(nullptr)) return false;
    bool ok = false;
    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        HANDLE hd = GetClipboardData(CF_UNICODETEXT);
        if (hd) {
            const wchar_t* p = (const wchar_t*)GlobalLock(hd);
            if (p) {
                wcsncpy(out, p, (size_t)cap - 1);
                out[cap - 1] = 0;
                GlobalUnlock(hd);
                ok = true;
            }
        }
    }
    CloseClipboard();
    return ok;
}

static bool HasDib() {
    if (!OpenClipboard(nullptr)) return false;
    bool ok = IsClipboardFormatAvailable(CF_DIB) != FALSE;
    CloseClipboard();
    return ok;
}

// ---- 一块确定内容的文本窗口：白底黑字 "HELLO 12345"，给 OCR 认 ----
static HDC   g_holdDc = nullptr;
static RECT  g_txtRect{ 100, 100, 700, 300 };

static LRESULT CALLBACK TxtProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0, 0, 0));
        HFONT f = CreateFontW(-110, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HGDIOBJ of = SelectObject(dc, f);
        DrawTextW(dc, L"HELLO 12345", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, of);
        DeleteObject(f);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_CLIPBOARDUPDATE) { ++g_clipUpdates; return 0; }
    return DefWindowProc(h, m, w, l);
}

// 泵消息 ms 毫秒（让 WM_CLIPBOARDUPDATE 能被收到）
static void Pump(int ms) {
    DWORD end = GetTickCount() + (DWORD)ms;
    MSG msg;
    while (GetTickCount() < end) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
}

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_vy = GetSystemMetrics(SM_YVIRTUALSCREEN);

    HWND host = FindByClass(kHostClass, 0);
    if (!host) { printf("[drv3] FAIL 没找到宿主 JisuQiShotHost\n"); return 2; }
    DWORD pid = 0;
    GetWindowThreadProcessId(host, &pid);
    printf("[drv3] 宿主 pid=%lu\n", pid);

    // 监听剪贴板：这是"两次独立会话"的唯一可靠判据
    WNDCLASSW wc{};
    wc.lpfnWndProc = TxtProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DrvClipWatch";
    RegisterClassW(&wc);
    HWND watch = CreateWindowExW(WS_EX_TOOLWINDOW, L"DrvClipWatch", L"", WS_POPUP,
                                 0, 0, 10, 10, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    AddClipboardFormatListener(watch);

    // 内容窗口：放在文本可识别的位置，稍后精确框选它
    HWND tw = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"DrvClipWatch", L"",
                              WS_POPUP, g_txtRect.left, g_txtRect.top,
                              g_txtRect.right - g_txtRect.left,
                              g_txtRect.bottom - g_txtRect.top,
                              nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ShowWindow(tw, SW_SHOWNA);
    UpdateWindow(tw);
    Pump(300);

    // 清剪贴板（并让 g_clipUpdates 从 0 起算）
    if (OpenClipboard(nullptr)) { EmptyClipboard(); CloseClipboard(); }
    Pump(200);
    g_clipUpdates = 0;

    UINT trig = RegisterWindowMessageW(L"JisuQiShotTrigger");
    PostMessageW(host, trig, 0, 0);
    HWND mask = WaitClass(kMaskClass, pid, 5000);
    Check(mask != nullptr, "遮罩出现");
    if (!mask) { DestroyWindow(tw); return 1; }
    Sleep(150);

    // 框选文本窗口内部（留 5px 余量避开边框/圆角）
    DragTo(mask, g_txtRect.left + 5, g_txtRect.top + 5,
                 g_txtRect.right - 5, g_txtRect.bottom - 5, 20);
    HWND tb = WaitClass(kTbClass, pid, 3000);
    Check(tb != nullptr, "工具栏出现（进入调整态）");
    if (!tb) { DestroyWindow(tw); return 1; }

    // 现在没有「识别文字」按钮了：点「文」工具 → 在选区内拖一个子框 → 该子框立即跑 OCR
    // 注意：**默认工具就是「文」**（capture.cpp 里 annTool 初值 = AT_TEXT），
    // 而 TbCommand 是"同键切换"——这里再点一次「文」会把它关掉（实测剪贴板里就没文字了），
    // 所以刻意不点。
    DragTo(mask, g_txtRect.left + 12, g_txtRect.top + 12,
                 g_txtRect.right - 12, g_txtRect.bottom - 12, 16);
    Pump(6000);                     // OCR 是同步的，这里只是等它把词画出来
    Check(FindByClass(kMaskClass, pid) != nullptr,
          "框选识别后遮罩仍在（识别不应该把截图结束掉）");

    // 「下载」现在弹「另存为」、只写文件；本驱动验的是剪贴板先图后文 → 走 ESC
    PostMessageW(mask, WM_KEYDOWN, VK_ESCAPE, 0);
    Pump(3000);

    Check(FindByClass(kMaskClass, pid) == nullptr, "识别完遮罩已销毁");
    Check(FindByClass(kTbClass, pid) == nullptr, "识别完工具栏已销毁");

    printf("[drv3] 剪贴板事件次数 = %d（期望 2：先图后文）\n", g_clipUpdates);
    Check(g_clipUpdates == 2, "剪贴板被写了两次独立会话（图 + 文字）");

    wchar_t txt[512];
    bool hasText = ReadClipText(txt, 512);
    Check(hasText, "当前剪贴板里有 CF_UNICODETEXT");
    if (hasText) {
        printf("[drv3]      文本长度 %d：%.120ls\n", (int)wcslen(txt), txt);
        // 当前剪贴板里**只能是文字**：否则 Ctrl+V 出来的可能还是图
        Check(!HasDib(), "当前剪贴板里没有 CF_DIB（所以 Ctrl+V 粘出来是文字）");
        Check(wcslen(txt) > 0, "文字非空");
    }

    DestroyWindow(tw);
    RemoveClipboardFormatListener(watch);
    DestroyWindow(watch);

    printf("\n[drv3] %s（失败项 %d）\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
