// child_probe.cpp —— 验证"小窗口截不到"的假设：这些小窗口是不是**子窗口**？
// 顶层枚举（EnumWindows）天生看不见子窗口，所以小面板/小控件永远选不中。
// 本探针统计：每个顶层窗口下有多少个"可见且够大"的子窗口，以及枚举耗时。
#include <windows.h>
#include <dwmapi.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")

struct Kid { RECT r; std::string cls; int depth; };
static std::vector<Kid> g_kids;
static int g_curDepth = 0;
static int g_maxDepth = 3;
static double g_ms = 0;

static double Now() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return 1000.0 * c.QuadPart / f.QuadPart;
}

static BOOL CALLBACK EnumKid(HWND hw, LPARAM lp) {
    if (!IsWindowVisible(hw)) return TRUE;
    RECT r{};
    if (!GetWindowRect(hw, &r)) return TRUE;
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w < 24 || h < 24) return TRUE;              // 太小的控件不值得吸附
    char cls[128] = {0};
    GetClassNameA(hw, cls, 128);
    g_kids.push_back(Kid{ r, cls, g_curDepth });
    if (g_curDepth < g_maxDepth) {
        ++g_curDepth;
        EnumChildWindows(hw, EnumKid, lp);
        --g_curDepth;
    }
    return TRUE;
}

static BOOL CALLBACK EnumTop(HWND hw, LPARAM lp) {
    if (!IsWindowVisible(hw)) return TRUE;
    DWORD pid = 0; GetWindowThreadProcessId(hw, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;
    LONG style = GetWindowLong(hw, GWL_STYLE);
    if (style & WS_MINIMIZE) return TRUE;
    int cloaked = 0;
    DwmGetWindowAttribute(hw, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return TRUE;
    RECT r{}; if (!GetWindowRect(hw, &r)) return TRUE;
    if (r.right - r.left <= 1 || r.bottom - r.top <= 1) return TRUE;

    char cls[128] = {0}, title[200] = {0};
    GetClassNameA(hw, cls, 128); GetWindowTextA(hw, title, 200);

    int before = (int)g_kids.size();
    double t0 = Now();
    g_curDepth = 0;
    EnumChildWindows(hw, EnumKid, lp);
    double dt = Now() - t0;
    int added = (int)g_kids.size() - before;
    printf("顶层 cls=%-28s title=%.34s rect=(%4d,%4d)-(%4d,%4d)  可见子窗口=%3d  枚举耗时=%6.3f ms\n",
           cls, title, r.left, r.top, r.right, r.bottom, added, dt);
    g_ms += dt;
    return TRUE;
}

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    printf("=== 子窗口探测 (depth<=%d, 最小 24x24) ===\n", g_maxDepth);
    EnumWindows(EnumTop, 0);
    printf("\n可见子窗口总数 = %zu，全部顶层窗口枚举累计耗时 = %.3f ms\n", g_kids.size(), g_ms);
    printf("\n--- 面积最小的 15 个子窗口（这些就是'截不到的小窗口'）---\n");
    std::sort(g_kids.begin(), g_kids.end(), [](const Kid& a, const Kid& b) {
        long long aa = (long long)(a.r.right - a.r.left) * (a.r.bottom - a.r.top);
        long long bb = (long long)(b.r.right - b.r.left) * (b.r.bottom - b.r.top);
        return aa < bb;
    });
    int n = 0;
    for (const Kid& k : g_kids) {
        if (n++ >= 15) break;
        printf("  d=%d cls=%-30s rect=(%4d,%4d)-(%4d,%4d) %4dx%4d\n",
               k.depth, k.cls.c_str(), k.r.left, k.r.top, k.r.right, k.r.bottom,
               k.r.right - k.r.left, k.r.bottom - k.r.top);
    }
    return 0;
}
