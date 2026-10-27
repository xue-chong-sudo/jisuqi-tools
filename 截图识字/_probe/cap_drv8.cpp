// cap_drv8.cpp —— 工具栏"长按 300ms 转拖动"的端到端回归
//
// 用户："截图的工具框怎么不能移动了。长按工具框是可以拖动的（哪怕是长按图标也可以拖动，
// 反正长按整个工具栏都是可以移动的）。"
//
// 三个判据（都是可量化的，不靠看界面）：
//   ① 短按仍是点击：点「矩形」(29,24) → 在选区里拖一笔 → ESC 存剪贴板 →
//      图里必须出现标注色像素（点一下如果变成拖动，就画不出东西）
//   ② 长按拖动：在 (300,24) 按下 → 等 500ms（不发任何 move，全靠 300ms 定时器转拖动态）
//      → 移到 (200,84) → 松开。窗口矩形应平移约 (-100,+60)（容差 ±10），尺寸不变 620x48
//   ③ 拖完不跳回：拖动之后再去动选区（拖左中把手 20px），工具栏必须留在拖后的位置
//
// 运行前请把 shot_ui.ini 复位成 [anno] color=0 width=0（默认红、2px），
// 否则 ① 的标注色像素数会因为别的颜色/更粗的笔而判错。
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>

static const wchar_t* kHostClass = L"JisuQiShotHost";
static const wchar_t* kMaskClass = L"OCRCap";
static const wchar_t* kTbClass   = L"OCRCapTb";

static const int kAnnR = 230, kAnnG = 60, kAnnB = 60;   // kAnnColors[0]
static const int kTbW = 620, kTbH = 48;

static int g_vx = 0, g_vy = 0;
static int g_fail = 0;
static DWORD g_pid = 0;

static void Check(bool ok, const char* what) {
    printf("[drv8] %s %s\n", ok ? "OK  " : "FAIL", what);
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
    Sleep(120);
}
// 工具栏按钮中心（客户区）：工具 29+42*(id-1)、保存 239、颜色 297+31i、粗细 503+31i，y=24
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

// ---- 剪贴板 DIB（24bpp bottom-up）----
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
                d.rowBytes = (int)(bi->biSizeImage / (d.h > 0 ? d.h : 1));
                if (d.rowBytes <= 0) d.rowBytes = ((d.w * 3 + 3) / 4) * 4;
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
static long CountAnn(const Dib& d) {
    long n = 0;
    for (int y = 0; y < d.h; ++y) {
        int row = d.h - 1 - y;
        const BYTE* p = &d.px[(size_t)row * d.rowBytes];
        for (int x = 0; x < d.w; ++x, p += 3) {
            int b = p[0], g = p[1], r = p[2];
            if (r > kAnnR - 45 && r < kAnnR + 45 &&
                g > kAnnG - 45 && g < kAnnG + 45 &&
                b > kAnnB - 45 && b < kAnnB + 45) ++n;
        }
    }
    return n;
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_vy = GetSystemMetrics(SM_YVIRTUALSCREEN);

    HWND host = FindByClass(kHostClass, 0);
    if (!host) { printf("[drv8] FAIL 没找到宿主\n"); return 2; }
    GetWindowThreadProcessId(host, &g_pid);
    UINT trig = RegisterWindowMessageW(L"JisuQiShotTrigger");
    printf("[drv8] 宿主 pid=%lu\n", (unsigned long)g_pid);

    HWND mask = nullptr, tb = nullptr;
    auto Enter = [&]() -> bool {
        PostMessageW(host, trig, 0, 0);
        mask = WaitClass(kMaskClass, g_pid, 5000);
        if (!mask) return false;
        DragTo(mask, g_vx + 200, g_vy + 200, g_vx + 800, g_vy + 600, 16);
        tb = WaitClass(kTbClass, g_pid, 3000);
        return tb != nullptr;
    };

    // ================= ① 短按仍然是点击 =================
    {
        Check(Enter(), "① 进入调整态（遮罩 + 工具栏）");
        if (tb) {
            RECT tr; GetWindowRect(tb, &tr);
            printf("[drv8]      工具栏初始 rect=(%ld,%ld) %ldx%ld\n",
                   tr.left, tr.top, tr.right - tr.left, tr.bottom - tr.top);

            ClickTb(tb, 1);                       // 「矩形」，短按 180ms 内松开
            Sleep(120);
            DragTo(mask, g_vx + 300, g_vy + 300, g_vx + 500, g_vy + 450, 12);
            if (OpenClipboard(nullptr)) { EmptyClipboard(); CloseClipboard(); }
            PostMessageW(mask, WM_KEYDOWN, VK_ESCAPE, 0);   // 调整态 ESC = 保存到剪贴板
            Sleep(500);
            Dib d = ReadClipDib();
            long n = d.ok ? CountAnn(d) : 0;
            printf("[drv8]      ① 标注色像素数 = %ld（180x150 的空心矩形，期望 > 400）\n", n);
            Check(d.ok, "① 短按后 ESC 拿到 CF_DIB");
            Check(n > 400, "① 短按「矩形」真的选中了工具并画出标注（不是拖动）");
        }
    }

    // ================= ② 长按 300ms 转拖动 =================
    {
        Sleep(400);
        Check(Enter(), "② 重新进入调整态");
        if (tb) {
            RECT r0; GetWindowRect(tb, &r0);
            const int w0 = (int)(r0.right - r0.left), h0 = (int)(r0.bottom - r0.top);

            // 在 (300,24) 按下——那里是颜色圆 0 的可点区，故意按在"图标上"
            SendMessage(tb, WM_MOUSEMOVE, 0, MAKELPARAM(300, 24));
            SendMessage(tb, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(300, 24));
            Sleep(500);                            // 什么都不发，靠 300ms 定时器转拖动
            // 产品按**绝对光标**算位移（原因见 capture.cpp 注释：窗口移动会让 Windows
            // 补发一批客户区坐标已经错位的合成 WM_MOUSEMOVE）。所以这里必须真的把
            // 光标挪过去，光发消息是不够的。
            SetCursorPos(r0.left + 200, r0.top + 84);
            Sleep(120);
            SendMessage(tb, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(200, 84));
            Sleep(80);
            {   // 诊断：拖动态第一帧之后窗口到哪了
                RECT rt; GetWindowRect(tb, &rt);
                printf("[drv8]      ② 移动一帧后 (%ld,%ld)\n", rt.left, rt.top);
            }
            SendMessage(tb, WM_LBUTTONUP, 0, MAKELPARAM(200, 84));
            Sleep(250);

            RECT r1; GetWindowRect(tb, &r1);
            const int dx = (int)(r1.left - r0.left), dy = (int)(r1.top - r0.top);
            const int w1 = (int)(r1.right - r1.left), h1 = (int)(r1.bottom - r1.top);
            printf("[drv8]      ② 拖动前 (%ld,%ld) 拖动后 (%ld,%ld)  位移 (%d,%d)\n",
                   r0.left, r0.top, r1.left, r1.top, dx, dy);
            Check(dx <= -90 && dx >= -110, "② 工具栏左移约 100px");
            Check(dy >= 50 && dy <= 70, "② 工具栏下移约 60px");
            Check(w1 == kTbW && h1 == kTbH, "② 拖动后尺寸仍是 620x48");

            // ================= ③ 拖完不跳回 =================
            // 去动选区：拖左中把手往左 20px（这会让 ApplyHandleDrag 调 TbReposition）
            const int my = (g_vy + 200 + g_vy + 600) / 2;
            DragTo(mask, g_vx + 200, my, g_vx + 180, my, 8);
            Sleep(300);
            RECT r2; GetWindowRect(tb, &r2);
            printf("[drv8]      ③ 动过选区之后 (%ld,%ld)\n", r2.left, r2.top);
            Check(r2.left == r1.left && r2.top == r1.top,
                  "③ 动选区后工具栏不自己跳回（保持拖后的位置）");

            SendMessage(mask, WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(300, 300));
            SendMessage(mask, WM_RBUTTONUP, 0, MAKELPARAM(300, 300));
            Sleep(250);
        }
    }

    printf("\n[drv8] %s（失败项 %d）\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
