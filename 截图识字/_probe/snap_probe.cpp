// snap_probe.cpp —— 验证 P0.5 吸附缺陷修复
//   1) 对比"旧过滤条件"与"新过滤条件"枚举到的窗口集合，列出被新规则剔除的幽灵窗口
//   2) 用合成窗口列表验证 Snap() 的投影区间判断（旧实现会误吸、新实现不会）
// 只读系统状态，不截图、不改任何东西。
#include <windows.h>
#include <dwmapi.h>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

static int g_vx, g_vy, g_vw, g_vh;

struct Rec {
    HWND hw; RECT r; std::string cls; std::string title; bool cloaked; bool toolwin;
    bool ownPid; bool offscreen; std::string why;   // 被排除的原因
};
static std::vector<Rec> g_old, g_new, g_dropped;

static bool IsShellChrome(const char* cls) {
    static const char* kBad[] = {
        "Progman", "WorkerW",
        "Shell_TrayWnd", "Shell_SecondaryTrayWnd",
        "NotifyIconOverflowWindow", "DV2ControlHost",
    };
    for (const char* b : kBad) if (strcmp(cls, b) == 0) return true;
    return false;
}

static std::string W2A(const wchar_t* w) {
    if (!w || !*w) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

static BOOL CALLBACK EnumRec(HWND hw, LPARAM) {
    Rec rec{}; rec.hw = hw;
    char cls[128] = {0}, title[256] = {0};
    GetClassNameA(hw, cls, 128);
    GetWindowTextA(hw, title, 256);
    rec.cls = cls; rec.title = title;

    DWORD pid = 0; GetWindowThreadProcessId(hw, &pid);
    rec.ownPid = (pid == GetCurrentProcessId());

    LONG style = GetWindowLong(hw, GWL_STYLE);
    LONG ex = GetWindowLong(hw, GWL_EXSTYLE);
    rec.toolwin = (ex & WS_EX_TOOLWINDOW) != 0;

    int cloaked = 0;
    DwmGetWindowAttribute(hw, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    rec.cloaked = cloaked != 0;

    RECT r{};
    if (!GetWindowRect(hw, &r)) return TRUE;
    rec.r = r;
    RECT vs{ g_vx, g_vy, g_vx + g_vw, g_vy + g_vh }, inter;
    rec.offscreen = !IntersectRect(&inter, &r, &vs);

    // ---- 旧规则（修复前）----
    bool oldKeep = IsWindowVisible(hw) && !(style & WS_MINIMIZE)
                   && !(ex & WS_EX_TOOLWINDOW)
                   && strcmp(cls, "Progman") != 0 && strcmp(cls, "WorkerW") != 0
                   && (r.right - r.left) > 1 && (r.bottom - r.top) > 1;
    if (oldKeep) g_old.push_back(rec);

    // ---- 新规则（修复后）----
    std::string why;
    bool keep = true;
    auto drop = [&](const char* w) { keep = false; if (why.empty()) why = w; };
    if (!IsWindowVisible(hw))                  drop("invisible");
    if (rec.ownPid)                            drop("own-pid");
    if (style & WS_MINIMIZE)                   drop("minimized");
    if (rec.toolwin && (style & WS_CAPTION) != WS_CAPTION) drop("toolwin-no-caption");
    if (IsShellChrome(cls))                    drop("shell-chrome");
    if (rec.cloaked)                           drop("cloaked");
    if ((r.right - r.left) <= 1 || (r.bottom - r.top) <= 1) drop("degenerate");
    if (rec.offscreen)                         drop("offscreen");
    rec.why = why;
    if (keep) g_new.push_back(rec);
    else if (oldKeep) g_dropped.push_back(rec);   // 旧规则放进来、新规则剔除掉的
    return TRUE;
}

// ---- Snap 逻辑（与 capture.cpp 中的实现逐行一致）----
struct Box { RECT r; };
static void SnapOld(POINT& p, const std::vector<Box>& ws) {
    const int t = 10;
    for (const Box& b : ws) {
        const RECT& w = b.r;
        if (std::abs(p.x - w.left)   < t) p.x = w.left;
        else if (std::abs(p.x - w.right) < t) p.x = w.right;
        if (std::abs(p.y - w.top)    < t) p.y = w.top;
        else if (std::abs(p.y - w.bottom) < t) p.y = w.bottom;
    }
}
static void SnapNew(POINT& p, const std::vector<Box>& ws) {
    const int t = 10;
    for (const Box& b : ws) {
        const RECT& w = b.r;
        if (p.x < w.left - t || p.x > w.right  + t) continue;
        if (p.y < w.top  - t || p.y > w.bottom + t) continue;
        int sx = p.x, sy = p.y;
        if (std::abs(p.x - w.left)      <= t) sx = w.left;
        else if (std::abs(p.x - w.right) <= t) sx = w.right;
        if (std::abs(p.y - w.top)       <= t) sy = w.top;
        else if (std::abs(p.y - w.bottom) <= t) sy = w.bottom;
        if (sx != p.x || sy != p.y) { p.x = sx; p.y = sy; return; }
    }
}

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    EnumWindows(EnumRec, 0);

    printf("=== 虚拟屏幕 %d,%d %dx%d ===\n", g_vx, g_vy, g_vw, g_vh);
    printf("旧规则窗口数 = %zu\n", g_old.size());
    printf("新规则窗口数 = %zu\n", g_new.size());
    printf("被新规则剔除(且旧规则会留下来) = %zu\n\n", g_dropped.size());
    printf("--- 剔除明细 ---\n");
    for (const Rec& r : g_dropped) {
        printf("  [%-20s] cls=%-34s title=%.40s rect=(%d,%d)-(%d,%d)\n",
               r.why.c_str(), r.cls.c_str(), r.title.c_str(),
               r.r.left, r.r.top, r.r.right, r.r.bottom);
    }
    printf("\n--- 保留下来的窗口(Z序) ---\n");
    for (const Rec& r : g_new) {
        printf("  cls=%-34s title=%.40s rect=(%d,%d)-(%d,%d)\n",
               r.cls.c_str(), r.title.c_str(), r.r.left, r.r.top, r.r.right, r.r.bottom);
    }

    // ---- Snap 投影判断对照 ----
    printf("\n=== Snap 投影判断对照 ===\n");
    std::vector<Box> ws;
    ws.push_back(Box{ RECT{ 500, 100, 900, 300 } });   // 一个远处的窗口
    struct Case { const char* name; POINT p; };
    Case cases[] = {
        { "鼠标竖直方向远离该窗口(应不吸附)", { 505, 800 } },
        { "鼠标在该窗口左边内部(应吸到 left)", { 503, 200 } },
        { "鼠标在该窗口左上角外 8px(应吸到角)", { 493, 93 } },
        { "鼠标在该窗口水平方向远离(应不吸附)", { 1200, 205 } },
    };
    int oldBad = 0;
    for (const Case& c : cases) {
        POINT a = c.p, b = c.p;
        SnapOld(a, ws);
        SnapNew(b, ws);
        bool oldSnapped = (a.x != c.p.x || a.y != c.p.y);
        bool newSnapped = (b.x != c.p.x || b.y != c.p.y);
        printf("  %-38s 输入(%4d,%4d)  旧->(%4d,%4d)%s  新->(%4d,%4d)%s\n",
               c.name, c.p.x, c.p.y, a.x, a.y, oldSnapped ? " 吸附" : " 不动",
               b.x, b.y, newSnapped ? " 吸附" : " 不动");
    }

    // 统计真实窗口下的误吸率：对每个"新规则"窗口，取它上下左右各 300px 外的点，
    // 看旧 Snap 会不会把它拽到该窗口边上
    std::vector<Box> boxOld, boxNew;
    for (const Rec& r : g_old) boxOld.push_back(Box{ r.r });
    for (const Rec& r : g_new) boxNew.push_back(Box{ r.r });
    int tot = 0, oldSnap = 0, newSnap = 0;
    (void)oldBad;
    for (const Rec& r : g_new) {
        POINT probes[4] = {
            { (r.r.left + r.r.right) / 2, r.r.top - 300 },
            { (r.r.left + r.r.right) / 2, r.r.bottom + 300 },
            { r.r.left - 300, (r.r.top + r.r.bottom) / 2 },
            { r.r.right + 300, (r.r.top + r.r.bottom) / 2 },
        };
        for (POINT p0 : probes) {
            if (p0.x < g_vx || p0.x >= g_vx + g_vw || p0.y < g_vy || p0.y >= g_vy + g_vh) continue;
            POINT a = p0, b = p0;
            SnapOld(a, boxOld);
            SnapNew(b, boxNew);
            ++tot;
            if (a.x != p0.x || a.y != p0.y) ++oldSnap;
            if (b.x != p0.x || b.y != p0.y) ++newSnap;
        }
    }
    printf("\n远离窗口 300px 的探针点 %d 个：旧规则误吸 %d 次，新规则误吸 %d 次\n",
           tot, oldSnap, newSnap);
    return 0;
}
