// cap_drv5.cpp —— 标注参数「记忆」的端到端验证
//
// 判据不靠看界面，靠两件硬事实：
//   ① 点完颜色/粗细之后，`shot_ui.ini` 里真的落了值；
//   ② 取消一次再重新截图，新的那一笔标注用的是**记忆里的颜色**（从剪贴板图数像素）。
// 第 ② 条才是用户要的东西 —— 第 ① 条只证明写了文件。
//
// 用法：cap_drv5.exe [shot_ui.ini 的完整路径]
//   不给参数时用部署目录下的默认路径。
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>

static const wchar_t* kHostClass = L"JisuQiShotHost";
static const wchar_t* kMaskClass = L"OCRCap";
static const wchar_t* kTbClass   = L"OCRCapTb";

// 工具/按钮 id 与中心见 cap_drv4 头注释
enum { TB_RECT = 1, TB_ELLIPSE = 2, TB_ARROW = 3, TB_PEN = 4, TB_TEXT = 5, TB_SAVE = 6 };

static const int kAnnColors[6][3] = {
    { 230,  60,  60}, { 245, 200,  40}, {  50, 130, 245},
    {  40, 180,  90}, {  35,  35,  35}, { 255, 255, 255},
};

static int  g_vx = 0, g_vy = 0, g_fail = 0;
static DWORD g_pid = 0;
static wchar_t g_ini[MAX_PATH] = {0};

static void Check(bool ok, const char* what) {
    printf("[drv5] %s %s\n", ok ? "OK  " : "FAIL", what);
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
    POINT org{ 0, 0 };
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
    Sleep(120);
}

static void ClickTb(HWND tb, int id) {
    int x = 0, y = 24;
    if (id >= 20 && id <= 25)      x = 307 + 31 * (id - 20);
    else if (id >= 30 && id <= 33) x = 511 + 31 * (id - 30);
    else { static const int cx[9] = { 0, 29, 71, 113, 155, 197, 249, 0, 0 }; x = cx[id]; }
    SendMessage(tb, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
    SendMessage(tb, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
    SendMessage(tb, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
    Sleep(180);
}

// ---- 剪贴板 DIB（24bpp bottom-up） ----
struct Dib { int w = 0, h = 0, rowBytes = 0; std::vector<BYTE> px; bool ok = false; };

static Dib ReadClipDib() {
    Dib d;
    if (!OpenClipboard(nullptr)) return d;
    if (IsClipboardFormatAvailable(CF_DIB)) {
        HANDLE hd = GetClipboardData(CF_DIB);
        if (hd) {
            const BITMAPINFOHEADER* bi = (const BITMAPINFOHEADER*)GlobalLock(hd);
            if (bi && bi->biBitCount == 24) {
                d.w = (int)bi->biWidth; d.h = (int)bi->biHeight;
                d.rowBytes = ((d.w * 3 + 3) / 4) * 4;
                const BYTE* base = (const BYTE*)bi + sizeof(BITMAPINFOHEADER);
                d.px.assign(base, base + (size_t)d.rowBytes * d.h);
                d.ok = true;
            }
            if (bi) GlobalUnlock(hd);
        }
    }
    CloseClipboard();
    return d;
}

// 图里"最像哪个标注色"的像素计数
static long CountNear(const Dib& d, int r0, int g0, int b0, int tol) {
    long n = 0;
    for (int y = 0; y < d.h; ++y) {
        int row = d.h - 1 - y;
        const BYTE* p = &d.px[(size_t)row * d.rowBytes];
        for (int x = 0; x < d.w; ++x, p += 3) {
            int b = p[0], g = p[1], r = p[2];
            if (r > r0 - tol && r < r0 + tol && g > g0 - tol && g < g0 + tol &&
                b > b0 - tol && b < b0 + tol) ++n;
        }
    }
    return n;
}

static HWND Trigger(HWND host, UINT trig) {
    if (OpenClipboard(nullptr)) { EmptyClipboard(); CloseClipboard(); }
    PostMessageW(host, trig, 0, 0);
    return WaitClass(kMaskClass, g_pid, 5000);
}

static HWND SelectFrame(HWND mask) {
    DragTo(mask, g_vx + 200, g_vy + 200, g_vx + 800, g_vy + 600, 20);
    return WaitClass(kTbClass, g_pid, 3000);
}

static void Cancel(HWND mask) {
    POINT org{ 0, 0 }; ClientToScreen(mask, &org);
    SendMessage(mask, WM_RBUTTONDOWN, MK_RBUTTON,
                MAKELPARAM(g_vx + 400 - org.x, g_vy + 400 - org.y));
    Sleep(300);
}

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_vy = GetSystemMetrics(SM_YVIRTUALSCREEN);

    if (argc > 1) {
        MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, g_ini, MAX_PATH);
    } else {
        wcscpy(g_ini, L"C:\\Users\\Administrator\\Desktop\\极速启\\data\\shot_ui.ini");
    }
    // 不打印完整路径：宿主目录含中文，控制台宽度/码页不同会截断成人难读的乱码，
    // 这里只需要确认"用的是哪份文件"这个事实。
    printf("[drv5] 记忆文件 = shot_ui.ini（与 settings.ini 同目录）\n");

    HWND host = FindByClass(kHostClass, 0);
    if (!host) { printf("[drv5] FAIL 没找到宿主\n"); return 2; }
    GetWindowThreadProcessId(host, &g_pid);
    UINT trig = RegisterWindowMessageW(L"JisuQiShotTrigger");

    // 目标：颜色选索引 3（绿 40,180,90），粗细选索引 3（9px）
    const int kColorIdx = 3, kWidthIdx = 3;

    // ---- 第一轮：改成绿 + 最粗，然后取消 ----
    {
        HWND mask = Trigger(host, trig);
        Check(mask != nullptr, "第一轮遮罩出现");
        if (!mask) return 1;
        HWND tb = SelectFrame(mask);
        Check(tb != nullptr, "第一轮工具栏出现");
        if (!tb) return 1;
        ClickTb(tb, 20 + kColorIdx);          // 颜色
        ClickTb(tb, 30 + kWidthIdx);          // 粗细
        Sleep(200);

        // ① 文件里真的落了值
        int c = GetPrivateProfileIntW(L"anno", L"color", -1, g_ini);
        int w = GetPrivateProfileIntW(L"anno", L"width", -1, g_ini);
        printf("[drv5]      文件读出 color=%d width=%d（期望 %d / %d）\n",
               c, w, kColorIdx, kWidthIdx);
        Check(c == kColorIdx && w == kWidthIdx, "颜色/粗细已写进记忆文件");

        Cancel(mask);
        Check(FindByClass(kMaskClass, g_pid) == nullptr, "第一轮已取消");
    }

    // ---- 第二轮：不碰颜色，直接画一笔 → 必须用记忆里的绿 ----
    {
        HWND mask = Trigger(host, trig);
        Check(mask != nullptr, "第二轮遮罩出现");
        if (!mask) return 1;
        HWND tb = SelectFrame(mask);
        if (!tb) return 1;
        ClickTb(tb, TB_RECT);
        DragTo(mask, g_vx + 300, g_vy + 300, g_vx + 500, g_vy + 450, 16);
        ClickTb(tb, TB_SAVE);
        Sleep(400);

        Dib d = ReadClipDib();
        Check(d.ok, "第二轮拿到剪贴板图");
        if (d.ok) {
            long green = CountNear(d, kAnnColors[kColorIdx][0], kAnnColors[kColorIdx][1],
                                      kAnnColors[kColorIdx][2], 30);
            long red   = CountNear(d, kAnnColors[0][0], kAnnColors[0][1], kAnnColors[0][2], 30);
            printf("[drv5]      绿色像素 %ld，红色像素 %ld\n", green, red);
            Check(green > 300, "新一轮的标注用的是记忆里的绿色");
            Check(green > red, "绿色明显多于默认红（记忆真的生效）");
        }
    }

    // ---- 第三轮：改回红色，确认还能改（记忆不是单向锁死） ----
    {
        HWND mask = Trigger(host, trig);
        if (mask) {
            HWND tb = SelectFrame(mask);
            if (tb) {
                ClickTb(tb, 20 + 0);
                ClickTb(tb, TB_RECT);
                DragTo(mask, g_vx + 300, g_vy + 300, g_vx + 500, g_vy + 450, 16);
                ClickTb(tb, TB_SAVE);
                Sleep(400);
                Dib d = ReadClipDib();
                if (d.ok) {
                    long red = CountNear(d, kAnnColors[0][0], kAnnColors[0][1],
                                            kAnnColors[0][2], 30);
                    printf("[drv5]      改回红后红色像素 %ld\n", red);
                    Check(red > 300, "改回红色并立即生效（颜色仍可切换）");
                }
            }
        }
    }

    // 收尾：把记忆恢复成默认，免得影响用户
    WritePrivateProfileStringW(L"anno", L"color", L"0", g_ini);
    WritePrivateProfileStringW(L"anno", L"width", L"0", g_ini);

    printf("\n[drv5] %s（失败项 %d）\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
