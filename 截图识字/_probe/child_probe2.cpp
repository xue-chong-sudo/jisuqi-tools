// child_probe2.cpp —— 回答 team-lead 的补测问题：
//   「把三道过滤全部放开（只留可见性），一共能枚举到多少子窗？
//     其中多少个落在任务栏 / Shell 宿主里？」
// 目的不是测本机，而是测「过滤链在任何机器上的必要性与充分性」：
//   1) 放开过滤后垃圾子窗有多少、集中在哪几类宿主（若集中在 Shell，说明"只对 g_wins 成员
//      枚举子窗"这条硬约束本身就吃掉绝大部分垃圾，过滤链是第二道保险而非唯一防线）；
//   2) 逐级加过滤，看每一级各自砍掉多少（若某一级砍掉 0，那一级就是无收益的复杂度）。
//
// 与 child_probe.cpp 的区别：那个只报"过滤后剩下的"，这个报"过滤前后各多少 + 逐级存活数"。
#include <windows.h>
#include <dwmapi.h>
#include <vector>
#include <string>
#include <map>
#include <algorithm>
#include <cstdio>
#include <cwchar>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")

// ---- 与 src/capture.cpp:100-109 保持一致的 Shell 名单 ----
static const wchar_t* kShell[] = {
    L"Progman", L"WorkerW",                      // 桌面
    L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", // 任务栏 / 副屏任务栏
    L"NotifyIconOverflowWindow",                 // 托盘溢出
    L"DV2ControlHost",                           // 自动播放等 Shell 宿主
};
static bool IsShellChrome(const wchar_t* c) {
    for (const wchar_t* b : kShell) if (wcscmp(c, b) == 0) return true;
    return false;
}
static bool IsTray(const wchar_t* c) {
    return wcscmp(c, L"Shell_TrayWnd") == 0 || wcscmp(c, L"Shell_SecondaryTrayWnd") == 0
        || wcscmp(c, L"NotifyIconOverflowWindow") == 0;
}

static double Now() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return 1000.0 * c.QuadPart / f.QuadPart;
}

static RECT g_vs{};   // 虚拟屏
static int  g_maxDepth = 3;
static int  kMinKid = 24;
static double kMaxCover = 0.6;

// 逐级过滤的存活计数（对"候选顶层"集合统计）
struct Stage {
    long long vis = 0;      // 只留可见性（放开一切）
    long long sizeOk = 0;   // + 边长 >= 24 且矩形非退化
    long long onScr = 0;    // + 与虚拟屏相交
    long long inPar = 0;    // + 内含于父窗矩形（2px 外容忍）
    long long cloak0 = 0;   // + DWMWA_CLOAKED == 0
    long long cover = 0;    // + 面积 <= 0.6 x 父窗面积
};
struct TopRec {
    HWND hw{};
    std::string cls, title;
    RECT r{};
    bool shell = false;   // 宿主是 Shell 名单里的（桌面/任务栏/托盘溢出/DV2）
    bool tray = false;    // 宿主是任务栏类
    bool gwins = false;   // 是否通过 EnumWnd 的过滤（= 属于 g_wins，是合法截图目标）
    long long rawVis = 0; // 只留可见性的子窗数（放开一切）
    Stage keep;           // 逐级存活
};
static std::vector<TopRec> g_tops;
static TopRec* g_cur = nullptr;
static int g_depth = 0;
static double g_ms = 0;

static BOOL CALLBACK EnumKid(HWND hw, LPARAM) {
    if (!IsWindowVisible(hw)) return TRUE;
    TopRec& t = *g_cur;
    ++t.rawVis;
    ++t.keep.vis;

    RECT r{};
    if (!GetWindowRect(hw, &r)) return TRUE;
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w < kMinKid || h < kMinKid) return TRUE;
    ++t.keep.sizeOk;

    if (r.right <= g_vs.left || r.left >= g_vs.right || r.bottom <= g_vs.top || r.top >= g_vs.bottom)
        return TRUE;
    ++t.keep.onScr;

    // 内含于父窗（父窗即本次递归的上层；对 depth==0 是顶层窗口本身）
    HWND par = GetParent(hw);
    if (par) {
        RECT pr{};
        if (GetWindowRect(par, &pr)) {
            if (r.left < pr.left - 2 || r.top < pr.top - 2 ||
                r.right > pr.right + 2 || r.bottom > pr.bottom + 2) return TRUE;
        }
    }
    ++t.keep.inPar;

    int cl = 0;
    if (DwmGetWindowAttribute(hw, DWMWA_CLOAKED, &cl, sizeof(cl)) == S_OK && cl) return TRUE;
    ++t.keep.cloak0;

    if (par) {
        RECT pr{};
        if (GetWindowRect(par, &pr)) {
            double ap = (double)(pr.right - pr.left) * (pr.bottom - pr.top);
            double ak = (double)w * h;
            if (ap > 0 && ak > kMaxCover * ap) return TRUE;
        }
    }
    ++t.keep.cover;

    if (g_depth < g_maxDepth) {
        ++g_depth;
        EnumChildWindows(hw, EnumKid, 0);   // 注意：首个实参是刚枚举到的这个子窗，不是桌面
        --g_depth;
    }
    return TRUE;
}

static BOOL CALLBACK EnumTop(HWND hw, LPARAM) {
    if (!IsWindowVisible(hw)) return TRUE;
    DWORD pid = 0; GetWindowThreadProcessId(hw, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;
    LONG style = GetWindowLong(hw, GWL_STYLE);
    if (style & WS_MINIMIZE) return TRUE;

    wchar_t clsW[64] = {0}; GetClassNameW(hw, clsW, 64);
    char cls[128] = {0}; GetClassNameA(hw, cls, 128);
    char title[160] = {0}; GetWindowTextA(hw, title, 160);

    RECT r{}; if (!GetWindowRect(hw, &r)) return TRUE;
    if (r.right - r.left <= 1 || r.bottom - r.top <= 1) return TRUE;

    TopRec t;
    t.hw = hw; t.cls = cls; t.title = title; t.r = r;
    t.shell = IsShellChrome(clsW);
    t.tray = IsTray(clsW);

    // 是否属于 g_wins：复刻 EnumWnd 的排除条件
    LONG ex = GetWindowLong(hw, GWL_EXSTYLE);
    bool toolNoCap = (ex & WS_EX_TOOLWINDOW) && (style & WS_CAPTION) != WS_CAPTION;
    int cl = 0; DwmGetWindowAttribute(hw, DWMWA_CLOAKED, &cl, sizeof(cl));
    t.gwins = !toolNoCap && !t.shell && cl == 0;

    g_tops.push_back(t);
    g_cur = &g_tops.back();
    g_depth = 0;
    double t0 = Now();
    EnumChildWindows(hw, EnumKid, 0);        // 关键：首个实参是顶层窗口本身
    g_ms += Now() - t0;
    return TRUE;
}

static long long Sum(const std::vector<TopRec>& v, long long Stage::*m) {
    long long s = 0; for (const TopRec& t : v) s += t.*m; return s;
}
static long long SumRaw(const std::vector<TopRec>& v) {
    long long s = 0; for (const TopRec& t : v) s += t.rawVis; return s;
}

static void Dump(const char* name, const std::vector<TopRec>& v) {
    printf("\n--- %s（顶层窗口 %zu 个）---\n", name, v.size());
    printf("  只留可见性              : %lld\n", SumRaw(v));
    printf("  + 边长>=%d 且非退化      : %lld\n", kMinKid, Sum(v, &Stage::sizeOk));
    printf("  + 与虚拟屏相交           : %lld\n", Sum(v, &Stage::onScr));
    printf("  + 内含于父窗(±2px)       : %lld\n", Sum(v, &Stage::inPar));
    printf("  + DWMWA_CLOAKED==0       : %lld\n", Sum(v, &Stage::cloak0));
    printf("  + 面积<=%.1fx父窗         : %lld   <== 最终可用子窗\n", kMaxCover, Sum(v, &Stage::cover));
}

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_vs.left   = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_vs.top    = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_vs.right  = g_vs.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_vs.bottom = g_vs.top  + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    printf("虚拟屏 = (%d,%d)-(%d,%d)  %dx%d\n", g_vs.left, g_vs.top, g_vs.right, g_vs.bottom,
           g_vs.right - g_vs.left, g_vs.bottom - g_vs.top);
    printf("过滤参数: maxDepth=%d  kMinKid=%d  kMaxKidCover=%.2f\n\n", g_maxDepth, kMinKid, kMaxCover);

    double t0 = Now();
    EnumWindows(EnumTop, 0);
    double total = Now() - t0;

    std::vector<TopRec> all = g_tops;
    std::vector<TopRec> shellHosts, trayHosts, otherHosts, gwinsOnly;
    for (const TopRec& t : g_tops) {
        if (t.tray)  trayHosts.push_back(t);
        if (t.shell) shellHosts.push_back(t);
        else         otherHosts.push_back(t);
        if (t.gwins) gwinsOnly.push_back(t);
    }

    Dump("A. 全部顶层窗口（含桌面/任务栏等 Shell 宿主）", all);
    Dump("B. 仅 Shell/桌面宿主（Progman/WorkerW/Tray/DV2/托盘溢出）", shellHosts);
    Dump("C. 仅任务栏类宿主（Shell_TrayWnd / SecondaryTray / 托盘溢出）", trayHosts);
    Dump("D. 非 Shell 宿主", otherHosts);
    Dump("E. 属于 g_wins 的顶层窗口（截图目标，Bβ 真正会枚举的集合）", gwinsOnly);

    printf("\n=== 关键比值 ===\n");
    long long ra = SumRaw(all), rs = SumRaw(shellHosts), rt = SumRaw(trayHosts);
    printf("放开过滤后子窗总数                = %lld\n", ra);
    printf("其中落在 Shell/桌面宿主下          = %lld  (%.1f%%)\n", rs, ra ? 100.0 * rs / ra : 0.0);
    printf("其中落在任务栏类宿主下             = %lld  (%.1f%%)\n", rt, ra ? 100.0 * rt / ra : 0.0);
    long long rg = SumRaw(gwinsOnly);
    printf("落在 g_wins 宿主下（Bβ 实际枚举范围）= %lld  (%.1f%%)\n", rg, ra ? 100.0 * rg / ra : 0.0);
    printf("g_wins 范围内最终通过全部过滤的子窗 = %lld\n", Sum(gwinsOnly, &Stage::cover));
    printf("\n结论口径：'只对 g_wins 成员枚举子窗'这一条约束，单独就吃掉了 %lld/%lld = %.1f%% 的放开后子窗；\n"
           "剩下 %lld 个再交给三道过滤处理。\n",
           ra - rg, ra, ra ? 100.0 * (ra - rg) / ra : 0.0, rg);

    printf("\n=== 放开过滤后子窗最多的宿主 Top 15 ===");
    std::vector<const TopRec*> byRaw;
    for (const TopRec& t : all) if (t.rawVis) byRaw.push_back(&t);
    std::sort(byRaw.begin(), byRaw.end(), [](const TopRec* a, const TopRec* b) {
        if (a->rawVis != b->rawVis) return a->rawVis > b->rawVis;
        return a->cls < b->cls;
    });
    int n = 0;
    for (const TopRec* t : byRaw) {
        if (n++ >= 15) break;
        printf("%s  raw=%-4lld 最终=%-3lld  %s cls=%-26s title=%.28s%s\n",
               t->shell ? "[Shell]" : "[     ]", t->rawVis, t->keep.cover,
               t->gwins ? "g_wins" : "  -   ", t->cls.c_str(), t->title.c_str(),
               t->tray ? "  <= 任务栏" : "");
    }

    printf("\n=== E 集合（g_wins）中最终存活的子窗明细 ===");
    int shown = 0;
    for (const TopRec& t : gwinsOnly) {
        if (t.keep.cover == 0) continue;
        ++shown;
        printf("  宿主 cls=%-26s title=%.24s  存活子窗=%lld（放开过滤时 %lld）\n",
               t.cls.c_str(), t.title.c_str(), t.keep.cover, t.rawVis);
    }
    if (!shown) printf("  （无：本机 g_wins 内没有任何子窗能通过全部过滤 —— 与上一轮结论一致）\n");

    printf("\n枚举全部顶层窗口及其子窗累计耗时 = %.3f ms（单次 EnumWindows 全流程 %.3f ms）\n", g_ms, total);
    return 0;
}
