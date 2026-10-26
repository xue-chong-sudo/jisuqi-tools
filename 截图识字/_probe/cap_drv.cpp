// cap_drv.cpp —— 调整框流程验证（不劫持物理鼠标）
//
// 用合成窗口消息直接投给遮罩窗口，走一遍：
//   拖框(300,250)->(900,650)  →  进入调整态（工具栏 OCRCapsTb 出现）
//   → 拖右中把手把右边推到 1000  →  选区变宽
//   → ESC（= 保存）  →  遮罩消失、剪贴板里是 CF_DIB，尺寸 = 调整后的选区
// 只验证保存路径；"识别文字"依赖 tessdata（仓库里没有），本机跑不了端到端。
#include <windows.h>
#include <cstdio>
#include <string>

static const wchar_t* kHostClass = L"JisuQiShotHost";
static const wchar_t* kMaskClass = L"OCRCap";
static const wchar_t* kTbClass   = L"OCRCapsTb";
static const wchar_t* kTbClassAlt = L"OCRCapTb";

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

// 朝遮罩窗口发一条鼠标消息。坐标为**虚拟屏坐标**，这里减掉窗口原点转客户区。
static void MouseMsg(HWND mask, UINT msg, int x, int y, WPARAM btn) {
    POINT org{0, 0};
    ClientToScreen(mask, &org);          // 客户区 (0,0) 的屏幕坐标
    int cx = x - org.x, cy = y - org.y;
    SendMessage(mask, msg, btn, MAKELPARAM(cx, cy));
}

static bool ReadClipboardDib(int& w, int& h) {
    w = h = 0;
    if (!OpenClipboard(nullptr)) return false;
    bool ok = false;
    if (IsClipboardFormatAvailable(CF_DIB)) {
        HANDLE hd = GetClipboardData(CF_DIB);
        if (hd) {
            const BITMAPINFOHEADER* bi =
                (const BITMAPINFOHEADER*)GlobalLock(hd);
            if (bi) { w = (int)bi->biWidth; h = (int)bi->biHeight; ok = true;
                      GlobalUnlock(hd); }
        }
    }
    CloseClipboard();
    return ok;
}

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);

    HWND host = FindByClass(kHostClass, 0);
    DWORD pid = 0;
    if (host) {
        GetWindowThreadProcessId(host, &pid);
        printf("[drv] 复用已存在的宿主 pid=%lu\n", pid);
    } else {
        printf("[drv] 未发现宿主，请先启动：screenshot_host.exe --host <tessdata> <ini>\n");
        return 2;
    }

    // 清空剪贴板，避免读到上一次的残留
    if (OpenClipboard(nullptr)) { EmptyClipboard(); CloseClipboard(); }

    UINT trig = RegisterWindowMessageW(L"JisuQiShotTrigger");
    PostMessageW(host, trig, 0, 0);

    HWND mask = WaitClass(kMaskClass, pid, 5000);
    if (!mask) { printf("[drv] FAIL 遮罩窗口没出现\n"); return 1; }
    printf("[drv] 遮罩已出现 %p\n", (void*)mask);
    Sleep(150);

    // ---- 1. 拖框 ----
    const int x0 = vx + 300, y0 = vy + 250;
    const int x1 = vx + 900, y1 = vy + 650;
    MouseMsg(mask, WM_MOUSEMOVE, x0, y0, 0);
    MouseMsg(mask, WM_LBUTTONDOWN, x0, y0, MK_LBUTTON);
    for (int i = 1; i <= 20; ++i) {
        int mx = x0 + (x1 - x0) * i / 20;
        int my = y0 + (y1 - y0) * i / 20;
        MouseMsg(mask, WM_MOUSEMOVE, mx, my, MK_LBUTTON);
    }
    MouseMsg(mask, WM_LBUTTONUP, x1, y1, 0);
    Sleep(200);

    // ---- 2. 调整态：工具栏应出现 ----
    HWND tb = WaitClass(kTbClass, pid, 2000);
    if (!tb) tb = WaitClass(kTbClassAlt, pid, 500);
    printf("[drv] %s 工具栏窗口\n", tb ? "OK   出现" : "FAIL 未出现");
    if (tb) {
        RECT tr; GetWindowRect(tb, &tr);
        printf("[drv]      工具栏 rect=(%ld,%ld)-(%ld,%ld) 尺寸 %ldx%ld\n",
               tr.left, tr.top, tr.right, tr.bottom,
               tr.right - tr.left, tr.bottom - tr.top);
        bool topmost = (GetWindowLong(tb, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        bool popup   = (GetWindowLong(tb, GWL_STYLE) & WS_CHILD) == 0;
        printf("[drv]      %s 独立顶层窗口（TOPMOST=%d, 非子窗=%d）\n",
               (topmost && popup) ? "OK   " : "FAIL", topmost, popup);
    }

    // ---- 3. 拖右中把手：右边 900 -> 1000 ----
    // 右中把手中心 = (sel.right, sel.top+sel.bottom)/2
    int hy = (y0 + y1) / 2;
    MouseMsg(mask, WM_MOUSEMOVE, x1, hy, 0);
    MouseMsg(mask, WM_LBUTTONDOWN, x1, hy, MK_LBUTTON);
    for (int i = 1; i <= 10; ++i)
        MouseMsg(mask, WM_MOUSEMOVE, x1 + i * 10, hy, MK_LBUTTON);
    MouseMsg(mask, WM_LBUTTONUP, x1 + 100, hy, 0);
    Sleep(200);

    // ---- 4. ESC = 保存 ----
    PostMessageW(mask, WM_KEYDOWN, VK_ESCAPE, 0);
    Sleep(400);

    bool maskGone = (FindByClass(kMaskClass, pid) == nullptr);
    printf("[drv] %s 遮罩已销毁（退出循环）\n", maskGone ? "OK   " : "FAIL 仍存在");

    int cw = 0, ch = 0;
    bool hasDib = ReadClipboardDib(cw, ch);
    printf("[drv] %s 剪贴板 CF_DIB = %dx%d\n",
           hasDib ? "OK   " : "FAIL 没有图片", cw, ch);

    // 期望：宽 = 1000-300 = 700（拖了把手），高 = 650-250 = 400
    bool dimOk = hasDib && (cw == 700) && (ch == 400);
    printf("[drv] %s 尺寸期望 700x400，实得 %dx%d\n",
           dimOk ? "OK   " : "FAIL", cw, ch);

    bool pass = tb && maskGone && dimOk;
    printf("\n[drv] %s\n", pass ? "ALL PASS" : "FAILED");
    return pass ? 0 : 1;
}
