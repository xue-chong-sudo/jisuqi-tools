// cap_drv2.cpp —— 调整框的第二组场景（同样不劫持物理鼠标）
//
// 每个场景都是"重新触发一次截图"，所以场景 N 能跑通本身就证明了场景 N-1 之后
// **宿主进程还活着**（g.done 跳出循环没留 WM_QUIT，没有 PostQuitMessage 误杀宿主）。
//
//   S1 选区态按 ESC            → 取消：遮罩消失、剪贴板没有 CF_DIB
//   S2 调整态点"关闭"          → 取消：遮罩消失、剪贴板没有 CF_DIB
//   S3 拖左中把手越过右边      → 边界交换：选区 (400,300)-(800,700) 拖成 100 宽
//                               再点"保存" → CF_DIB = 100x400
//   S4 再触发一次              → 宿主仍存活（能出遮罩）
#include <windows.h>
#include <cstdio>

static const wchar_t* kHostClass = L"JisuQiShotHost";
static const wchar_t* kMaskClass = L"OCRCap";
static const wchar_t* kTbClass   = L"OCRCapTb";

static int g_vx = 0, g_vy = 0;
static int g_fail = 0;

static void Check(bool ok, const char* what) {
    printf("[drv2] %s %s\n", ok ? "OK  " : "FAIL", what);
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

// 工具栏按钮中心（客户区坐标）。ktbW=236, kTbBtnW=68, kTbGap=8, kTbBtnH=28, kTbH=40
//   按钮 id: x = 8 + (id-1)*(68+8)，宽 68 → 中心 x = 8+(id-1)*76+34；y 居中 = 20
static void ClickTb(HWND tb, int id) {
    int cx = 8 + (id - 1) * 76 + 34, cy = 20;
    SendMessage(tb, WM_MOUSEMOVE, 0, MAKELPARAM(cx, cy));
    SendMessage(tb, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(cx, cy));
    SendMessage(tb, WM_LBUTTONUP, 0, MAKELPARAM(cx, cy));
    Sleep(250);
}

static bool ReadClipboardDib(int& w, int& h) {
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

static DWORD g_pid = 0;

// 触发一次截图，等到遮罩出现
static HWND Trigger(HWND host, UINT trig) {
    if (OpenClipboard(nullptr)) { EmptyClipboard(); CloseClipboard(); }
    PostMessageW(host, trig, 0, 0);
    return WaitClass(kMaskClass, g_pid, 5000);
}

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_vy = GetSystemMetrics(SM_YVIRTUALSCREEN);

    HWND host = FindByClass(kHostClass, 0);
    if (!host) { printf("[drv2] FAIL 没找到宿主 JisuQiShotHost\n"); return 2; }
    GetWindowThreadProcessId(host, &g_pid);
    printf("[drv2] 宿主 pid=%lu\n", g_pid);
    UINT trig = RegisterWindowMessageW(L"JisuQiShotTrigger");
    

    // ---- S1：选区态 ESC = 取消 ----
    {
        HWND mask = Trigger(host, trig);
        Check(mask != nullptr, "S1 遮罩出现");
        if (mask) {
            Sleep(120);
            PostMessageW(mask, WM_KEYDOWN, VK_ESCAPE, 0);
            Sleep(350);
            Check(FindByClass(kMaskClass, g_pid) == nullptr, "S1 ESC 后遮罩销毁");
            int cw = 0, ch = 0;
            Check(!ReadClipboardDib(cw, ch), "S1 取消时剪贴板没被写（无 CF_DIB）");
        }
    }

    // ---- S2：调整态点"关闭" = 取消，不写剪贴板 ----
    {
        HWND mask = Trigger(host, trig);
        Check(mask != nullptr, "S2 遮罩出现");
        if (mask) {
            int x0 = g_vx + 300, y0 = g_vy + 250, x1 = g_vx + 900, y1 = g_vy + 650;
            DragTo(mask, x0, y0, x1, y1, 20);
            HWND tb = WaitClass(kTbClass, g_pid, 2000);
            Check(tb != nullptr, "S2 工具栏出现（进入调整态）");
            if (tb) {
                ClickTb(tb, 3);                      // 3 = 关闭
                Check(FindByClass(kMaskClass, g_pid) == nullptr, "S2 关闭后遮罩销毁");
                Check(FindByClass(kTbClass, g_pid) == nullptr, "S2 关闭后工具栏也销毁");
                int cw = 0, ch = 0;
                Check(!ReadClipboardDib(cw, ch), "S2 关闭不写剪贴板（无 CF_DIB）");
            }
        }
    }

    // ---- S3：把手拖越对面 → 边界交换；再点"保存" ----
    {
        HWND mask = Trigger(host, trig);
        Check(mask != nullptr, "S3 遮罩出现");
        if (mask) {
            int l = g_vx + 400, t = g_vy + 300, r = g_vx + 800, b = g_vy + 700;
            DragTo(mask, l, t, r, b, 20);
            HWND tb = WaitClass(kTbClass, g_pid, 2000);
            Check(tb != nullptr, "S3 工具栏出现");
            if (tb) {
                int my = (t + b) / 2;                // 左中把手中心 = (sel.left, 中线)
                DragTo(mask, l, my, g_vx + 900, my, 12);   // 400 -> 900，越过右边界 800
                Sleep(150);
                ClickTb(tb, 1);                      // 1 = 保存
                Sleep(300);
                Check(FindByClass(kMaskClass, g_pid) == nullptr, "S3 保存后遮罩销毁");
                int cw = 0, ch = 0;
                bool has = ReadClipboardDib(cw, ch);
                Check(has, "S3 剪贴板有 CF_DIB");
                // 越界后左右交换：left=800, right=900 → 宽 100；高仍是 400
                Check(has && cw == 100 && ch == 400, "S3 边界交换后尺寸 = 100x400");
                if (has) printf("[drv2]      实测 %dx%d\n", cw, ch);
            }
        }
    }

    // ---- S4：宿主仍活着 ----
    {
        HWND mask = Trigger(host, trig);
        Check(mask != nullptr, "S4 三轮之后宿主仍能响应触发");
        if (mask) { PostMessageW(mask, WM_KEYDOWN, VK_ESCAPE, 0); Sleep(300); }
    }

    printf("\n[drv2] %s（失败项 %d）\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
