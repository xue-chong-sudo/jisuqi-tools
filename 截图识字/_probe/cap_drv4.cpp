// cap_drv4.cpp —— 原地标注的端到端验证（不劫持物理鼠标）
//
// 做法：合成鼠标消息驱动遮罩，画完标注点"保存"，然后**从剪贴板的 CF_DIB 里数像素**，
// 用像素事实判断标注有没有真的被合成进图片、撤销有没有真的抹掉。
// 比"看截图觉得对"硬：能看到轮廓是不是空心、颜色对不对、位置对不对。
//
//   S1 选矩形工具 → 在选区内拖一个矩形 → 点"保存"
//      · 点工具后遮罩必须仍在（点工具不能顺手把截图保存掉 —— 这是最容易犯的错）
//      · 剪贴板图 600x400；标注色像素数在 [500, 6000]（空心轮廓，不是实心块）
//      · 上边中点像素是标注色、矩形正中不是标注色
//   S2 画一个矩形 → 点"撤销" → 点"保存"
//      · 标注色像素数 ≈ 0（撤销真的抹掉了）
//   S3 选椭圆工具画一个 → 保存
//      · 标注色像素数在 [500, 6000]（椭圆闭合、也是空心）
//
// 工具栏布局（与 capture.cpp 的规格一致）：
//   第 1 行 y=6..34（中心 y=20）：
//     矩形 8..52(中心 30) 椭圆 60..104(82) 箭头 112..156(134) 画笔 164..208(186)
//     撤销 224..268(246)  保存 284..352(318)  识别文字 360..428(394)  关闭 436..504(470)
//   第 2 行 色块 y=44..68（中心 56）：颜色 i x=8+30i（中心 20+30i）
//                                     粗细 i x=202+30i（中心 214+30i）
#include <windows.h>
#include <cstdio>
#include <vector>

static const wchar_t* kHostClass = L"JisuQiShotHost";
static const wchar_t* kMaskClass = L"OCRCap";
static const wchar_t* kTbClass   = L"OCRCapTb";

// 工具 id（与 capture.cpp 对齐）
enum { TB_RECT = 1, TB_ELLIPSE = 2, TB_ARROW = 3, TB_PEN = 4, TB_UNDO = 5,
       TB_SAVE = 6, TB_OCR = 7, TB_CLOSE = 8 };

// 默认标注色 = kAnnColors[0] = (230,60,60)
static const int kAnnR = 230, kAnnG = 60, kAnnB = 60;

static int g_vx = 0, g_vy = 0;
static int g_fail = 0;
static DWORD g_pid = 0;

static void Check(bool ok, const char* what) {
    printf("[drv4] %s %s\n", ok ? "OK  " : "FAIL", what);
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

// 工具栏（单行 40 高，纯图标）。按钮中心（客户区坐标）：
//   行 y 中心恒为 20
//   工具 1..5 中心 x = 24 / 58 / 92 / 126 / 160（矩形 椭圆 箭头 画笔 文）
//   保存(下载图标) 6 中心 x = 204
//   颜色 20..25 中心 x = 247+26i
//   粗细 30..33 中心 x = 416+26i
static POINT TbCenter(int id) {
    if (id >= 20 && id <= 25) return POINT{ 307 + 31 * (id - 20), 24 };
    if (id >= 30 && id <= 33) return POINT{ 511 + 31 * (id - 30), 24 };
    static const int cx[9] = { 0, 29, 71, 113, 155, 197, 249, 0, 0 };
    return POINT{ cx[id], 24 };
}

// 真按键：Ctrl+Z。GetKeyState 读的是线程输入状态，只能靠 keybd_event 影响。
static void UndoCtrlZ() {
    keybd_event(VK_CONTROL, 0, 0, 0);
    keybd_event('Z', 0, 0, 0);
    keybd_event('Z', 0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
    Sleep(250);
}

static void ClickTb(HWND tb, int id) {
    POINT p = TbCenter(id);
    SendMessage(tb, WM_MOUSEMOVE, 0, MAKELPARAM(p.x, p.y));
    SendMessage(tb, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(p.x, p.y));
    SendMessage(tb, WM_LBUTTONUP, 0, MAKELPARAM(p.x, p.y));
    Sleep(180);
}

// ---- 剪贴板 DIB：24bpp bottom-up，行 4 字节对齐 ----
struct Dib {
    int w = 0, h = 0, rowBytes = 0;
    std::vector<BYTE> px;    // 原始 bottom-up 数据
    bool ok = false;
};

static Dib ReadClipDib() {
    Dib d;
    if (!OpenClipboard(nullptr)) return d;
    if (IsClipboardFormatAvailable(CF_DIB)) {
        HANDLE hd = GetClipboardData(CF_DIB);
        if (hd) {
            const BITMAPINFOHEADER* bi = (const BITMAPINFOHEADER*)GlobalLock(hd);
            if (bi && bi->biBitCount == 24) {
                d.w = (int)bi->biWidth;
                d.h = (int)bi->biHeight;
                d.rowBytes = (int)bi->biSizeImage / (d.h > 0 ? d.h : 1);
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

// 图像坐标（top-down，左上原点）取 BGR
static bool PixAt(const Dib& d, int x, int y, int& r, int& g, int& b) {
    if (!d.ok || x < 0 || y < 0 || x >= d.w || y >= d.h) return false;
    int row = d.h - 1 - y;                       // bottom-up
    const BYTE* p = &d.px[(size_t)row * d.rowBytes + (size_t)x * 3];
    b = p[0]; g = p[1]; r = p[2];
    return true;
}

static bool IsAnnColor(int r, int g, int b) {
    return r > kAnnR - 45 && r < kAnnR + 45 &&
           g > kAnnG - 45 && g < kAnnG + 45 &&
           b > kAnnB - 45 && b < kAnnB + 45;
}

static long CountAnnColor(const Dib& d) {
    long n = 0;
    for (int y = 0; y < d.h; ++y) {
        int row = d.h - 1 - y;
        const BYTE* p = &d.px[(size_t)row * d.rowBytes];
        for (int x = 0; x < d.w; ++x, p += 3)
            if (IsAnnColor(p[2], p[1], p[0])) ++n;
    }
    return n;
}

// 触发一次截图并等到遮罩
static HWND Trigger(HWND host, UINT trig) {
    if (OpenClipboard(nullptr)) { EmptyClipboard(); CloseClipboard(); }
    PostMessageW(host, trig, 0, 0);
    return WaitClass(kMaskClass, g_pid, 5000);
}

// 拖出 (200,200)-(800,600) 的选区并返回工具栏
static HWND SelectFrame(HWND mask) {
    DragTo(mask, g_vx + 200, g_vy + 200, g_vx + 800, g_vy + 600, 20);
    return WaitClass(kTbClass, g_pid, 3000);
}

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_vy = GetSystemMetrics(SM_YVIRTUALSCREEN);

    HWND host = FindByClass(kHostClass, 0);
    if (!host) { printf("[drv4] FAIL 没找到宿主 JisuQiShotHost\n"); return 2; }
    GetWindowThreadProcessId(host, &g_pid);
    printf("[drv4] 宿主 pid=%lu\n", g_pid);
    UINT trig = RegisterWindowMessageW(L"JisuQiShotTrigger");

    // ============ S1：矩形工具 → 拖一个矩形 → 保存 ============
    {
        HWND mask = Trigger(host, trig);
        Check(mask != nullptr, "S1 遮罩出现");
        if (mask) {
            HWND tb = SelectFrame(mask);
            Check(tb != nullptr, "S1 工具栏出现");
            if (tb) {
                RECT tr; GetWindowRect(tb, &tr);
                // 单行图标工具栏定案尺寸 626x48（1.2 倍放大后；再之前是 512x40 / 512x72）。
                printf("[drv4]      工具栏 %ldx%ld（期望 626x48）\n",
                       tr.right - tr.left, tr.bottom - tr.top);
                Check(tr.right - tr.left == 626 && tr.bottom - tr.top == 48,
                      "S1 工具栏是单行 626x48");

                ClickTb(tb, TB_RECT);
                Check(FindByClass(kMaskClass, g_pid) != nullptr,
                      "S1 点「矩形」后遮罩仍在（工具键没有误触发保存）");

                // 选区内拖一个 200x150 的矩形：(300,300)->(500,450)
                DragTo(mask, g_vx + 300, g_vy + 300, g_vx + 500, g_vy + 450, 16);
                ClickTb(tb, TB_SAVE);
                Sleep(400);

                Check(FindByClass(kMaskClass, g_pid) == nullptr, "S1 保存后遮罩销毁");

                Dib d = ReadClipDib();
                Check(d.ok, "S1 剪贴板有 24bpp CF_DIB");
                if (d.ok) {
                    printf("[drv4]      图 %dx%d\n", d.w, d.h);
                    Check(d.w == 600 && d.h == 400, "S1 图片尺寸 = 600x400");

                    long n = CountAnnColor(d);
                    printf("[drv4]      标注色像素数 = %ld（空心轮廓期望 500~6000）\n", n);
                    Check(n > 500 && n < 6000, "S1 标注已合成进图片且是空心轮廓");

                    // 图像坐标：选区原点 (200,200)，矩形 (100,100)-(300,250)
                    int r = 0, g = 0, b = 0;
                    bool topMid = PixAt(d, 200, 101, r, g, b);
                    printf("[drv4]      上边中点(200,101) = (%d,%d,%d)\n", r, g, b);
                    Check(topMid && IsAnnColor(r, g, b), "S1 上边中点像素是标注色");

                    bool center = PixAt(d, 200, 175, r, g, b);
                    printf("[drv4]      矩形正中(200,175) = (%d,%d,%d)\n", r, g, b);
                    Check(center && !IsAnnColor(r, g, b), "S1 矩形正中不是标注色（空心）");
                }
            }
        }
    }

    // ============ S2：画矩形 → 撤销 → 保存 ============
    {
        HWND mask = Trigger(host, trig);
        Check(mask != nullptr, "S2 遮罩出现");
        if (mask) {
            HWND tb = SelectFrame(mask);
            if (tb) {
                ClickTb(tb, TB_RECT);
                DragTo(mask, g_vx + 300, g_vy + 300, g_vx + 500, g_vy + 450, 16);
                // 工具栏上已经没有撤销按钮（用户定案：Ctrl+Z），必须发真按键：
                // 产品的撤销判断读的是 GetKeyState(VK_CONTROL)，PostMessage 伪造不了。
                UndoCtrlZ();
                Check(FindByClass(kMaskClass, g_pid) != nullptr, "S2 撤销后遮罩仍在");
                ClickTb(tb, TB_SAVE);
                Sleep(400);

                Dib d = ReadClipDib();
                if (d.ok) {
                    long n = CountAnnColor(d);
                    printf("[drv4]      S2 撤销后标注色像素数 = %ld（期望 0）\n", n);
                    Check(n == 0, "S2 撤销真的抹掉了标注");
                } else {
                    Check(false, "S2 剪贴板有 CF_DIB");
                }
            } else {
                Check(false, "S2 工具栏出现");
            }
        }
    }

    // ============ S3：椭圆工具 → 保存（验证椭圆闭合）============
    {
        HWND mask = Trigger(host, trig);
        Check(mask != nullptr, "S3 遮罩出现");
        if (mask) {
            HWND tb = SelectFrame(mask);
            if (tb) {
                ClickTb(tb, TB_ELLIPSE);
                DragTo(mask, g_vx + 300, g_vy + 300, g_vx + 500, g_vy + 450, 16);
                ClickTb(tb, TB_SAVE);
                Sleep(400);

                Dib d = ReadClipDib();
                if (d.ok) {
                    long n = CountAnnColor(d);
                    printf("[drv4]      S3 椭圆标注色像素数 = %ld\n", n);
                    Check(n > 500 && n < 6000, "S3 椭圆已合成且是空心闭合轮廓");
                } else {
                    Check(false, "S3 剪贴板有 CF_DIB");
                }
            } else {
                Check(false, "S3 工具栏出现");
            }
        }
    }

    printf("\n[drv4] %s（失败项 %d）\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
