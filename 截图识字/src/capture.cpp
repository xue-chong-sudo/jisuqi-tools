// capture.cpp —— Win32 screenshot: light grey veil + transparent hole inside the selected area
// Workflow: 1) capture entire virtual screen into memory, 2) show a light grey
// overlay everywhere EXCEPT the hovered/dragged selection, 3) return the clean
// (non-dimmed) captured region. This matches screenshot tools like WeChat/QQ.
#include "ocraddon_internal.h"
#include <windows.h>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <climits>
#include <cstring>
#include <cstdlib>   // std::abs
#include <cstdio>    // std::snprintf
#include <cmath>     // cos / sin / atan2 / lround（自写光栅化用）
#include <utility>   // std::move（OverlayState 里的标注 vector 每帧搬一次）
#include <string>
#include <mutex>     // 后台识别结果交给 UI 线程时的交接锁
#include <atomic>    // 识别"代号"：只认最新一代的结果
#include <gdiplus.h> // 工具栏抗锯齿（只画工具栏，绝不进 g.bits；wWinMain 已 GdiplusStartup）
#include <shlobj.h>  // SHGetFolderPathW：没有"上次保存目录"时默认落桌面
#include <cwctype>  // towlower：按扩展名判 png/jpg
namespace gp = Gdiplus;   // 与 view.cpp 同名简写，两个 TU 各自独立

// GET_X_LPARAM/GET_Y_LPARAM normally live in <windowsx.h>, but that header
// tends to clash with other definitions. Define them locally to stay safe.
#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif

static bool g_dpi = false;
static void EnsureDpi() {
    if (g_dpi) return;
    // SetProcessDpiAwareness 位于 shcore.dll（不是 user32）。user32 上没有该导出，
    // 旧代码会静默失败并回退到 SetProcessDPIAware()，功能尚可但应直接找对模块。
    HMODULE sc = LoadLibraryA("shcore.dll");
    if (sc) {
        auto f = (HRESULT(WINAPI*)(int))GetProcAddress(sc, "SetProcessDpiAwareness");
        if (f) f(2 /*PROCESS_PER_MONITOR_DPI_AWARE*/);
        FreeLibrary(sc);
    } else {
        SetProcessDPIAware();
    }
    g_dpi = true;
}

// 交互阶段（微信截图同款）：
//   Select —— 拖框选区（含吸附）；松手后有有效选区即转入 Adjust，不再销毁遮罩。
//   Adjust —— 选区已定，可拖八个把手微调；ESC / 保存 = 裁剪并写入剪贴板后退出。
enum class Phase { Select, Adjust };

// ---- 原地标注（矩形 / 椭圆 / 箭头 / 画笔 / 框选识别）----
// 坐标一律用**绝对虚拟屏坐标**，不是相对选区的坐标：截图内容是一张静止的画面，
// 标注必须"画在哪就钉在哪个屏幕像素"。这样之后拖把手改选区，标注不会跟着乱跑；
// 被缩掉的那部分在保存时自然被裁掉。
enum { AT_NONE = 0, AT_RECT, AT_ELLIPSE, AT_ARROW, AT_PEN, AT_TEXT };
constexpr int kAnnColors[6][3] = {   // 与 view.cpp 保持一致，别改数值
    { 230,  60,  60},   // 红（默认）
    { 245, 200,  40},   // 黄
    {  50, 130, 245},   // 蓝
    {  40, 180,  90},   // 绿
    {  35,  35,  35},   // 黑
    { 255, 255, 255},   // 白
};
constexpr int kAnnWidths[4] = { 2, 4, 6, 9 };

struct Anno {
    int tool = AT_NONE;
    int color = 0;              // kAnnColors 下标
    int widthIdx = 0;           // kAnnWidths 下标
    std::vector<POINT> pts;     // 绝对虚拟屏坐标；RECT/ELLIPSE/ARROW 恒 2 个点，PEN 是整条路径
};

// 截图遮罩层状态（提前定义，供 EnumWnd 使用虚拟屏幕尺寸过滤窗口）。
struct Cap {
    HWND w = nullptr;
    Phase phase = Phase::Select;
    int dragHandle = -1;     // Adjust 阶段正在拖的把手 0..7；-1 = 没在拖
    bool saveToClip = false;  // 退出时把裁剪结果写进剪贴板（ESC / 保存）
    bool ocrWanted  = false;  // 退出时对裁剪结果跑 OCR，并把文字也写进剪贴板（点"识别文字"）
    bool drag = false;
    // ---- 原地标注 ----
    std::vector<Anno> annos;   // 已完成的标注（绝对虚拟屏坐标）
    Anno  annCur;              // 正在画的那一条
    bool  drawingAnn = false;
    // 默认就是「文」（文字识别）：截完图一进调整态，拖一下就能框选识字（用户定案）。
    // 再点一次「文」会退回 AT_NONE（TbCommand 里的同键切换），这是对的。
    int   annTool = AT_TEXT;   // 当前选中的工具；AT_NONE = 不在标注模式
    int   annColor = 0;
    int   annWidthIdx = 0;
    // ---- 框选识别（「文」工具）----
    // textRect 是绝对虚拟屏坐标；words 也是绝对坐标 —— 刻意**不用**"相对选区"：
    // 词框锚定的是图像内容（屏幕像素），选区挪动时高亮必须跟着同一个像素走。
    // 相对坐标会让"选区一动、词框就整体偏移"（内容没动却错位）。
    RECT  textRect{};                  // 文字子框；空 = 没有
    std::vector<OcrWord> words;
    std::vector<unsigned char> wordSel; // 与 words 等长
    bool  textHasFocus = false;         // = !REmpty(textRect)，只为读代码方便
    bool  textDragging = false;         // 正在拉子框
    POINT textDragStart{0, 0};
    bool  wordDrag = false;             // 正在词上拖选
    POINT wordDragStart{0, 0};
    POINT wordDragCur{0, 0};
    bool  ocrBusy = false;              // 同步识别中：禁止再起笔（防重入）
    POINT start = {0,0};
    POINT cur = {0,0}; // current mouse pos in virtual-screen coords
    RECT sel = {0,0,0,0};
    int vx = 0, vy = 0, vw = 0, vh = 0; // virtual-screen origin and size
    bool done = false, captured = false;
    HBITMAP hDib = nullptr;
    HDC hDc = nullptr;
    void* bits = nullptr;
    std::vector<unsigned char> screen; // clean original screen (BGRA, top-down)
    std::vector<unsigned char> dim;    // dimmed screen (BGRA, top-down), base for overlay
};
static Cap g;

// 本轮是否已经把结果写进剪贴板。
// 调整态里点"保存"/"识别文字"就已经落盘了，ocraddon.cpp 据此跳过 ShowResultAndCopy
// —— 否则用户会看到"原地保存完，又弹出一个结果窗口"。
static bool g_shotCopied = false;
bool ShotAlreadyCopied() { return g_shotCopied; }

// ---- 颜色 / 粗细记忆 ----
// 存在 ShotIniPath() 同目录的 shot_ui.ini 里，**不写 settings.ini**：那是极速启自己的
// 配置文件，我们不进去加键（两边版本一错开就互相踩）。取不到目录（独立模式没传 ini
// 路径）时返回空串 = 这次不持久化，不当作错误。
static std::wstring ShotUiIni() {
    const wchar_t* ini = ShotIniPath();
    if (!ini || !*ini) return std::wstring();
    std::wstring p(ini);
    const size_t slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::wstring();
    p.resize(slash + 1);
    p += L"shot_ui.ini";
    return p;
}

// 每次截图开始时读一次即可（宿主常驻，但用户可能在外面改这个文件）。
static void LoadAnnoPrefs(int& color, int& width) {
    color = 0; width = 0;
    const std::wstring p = ShotUiIni();
    if (p.empty()) return;
    color = (int)GetPrivateProfileIntW(L"anno", L"color", 0, p.c_str());
    width = (int)GetPrivateProfileIntW(L"anno", L"width", 0, p.c_str());
    // 夹到合法范围：文件被手改坏了也不能越界去索引 kAnnColors/kAnnWidths
    if (color < 0) color = 0;
    if (color > 5) color = 5;
    if (width < 0) width = 0;
    if (width > 3) width = 3;
}

static void SaveAnnoPrefs() {
    const std::wstring p = ShotUiIni();
    if (p.empty()) return;
    wchar_t buf[8];
    wsprintfW(buf, L"%d", g.annColor);
    WritePrivateProfileStringW(L"anno", L"color", buf, p.c_str());
    wsprintfW(buf, L"%d", g.annWidthIdx);
    WritePrivateProfileStringW(L"anno", L"width", buf, p.c_str());
}

// Enumerate visible top-level windows; used for hover-highlight and snap.
//
// 用户反馈的三类现象，各自有明确根因（改动都在本段和下面的 Snap/FindHoveredWindow）：
//   ①"这里啥都没有，你怎么吸附了" —— 旧 Snap() 沿**无限长的边**吸附：只要
//     abs(p.x - w.left) < 10 就吸过去，完全不看 p.y 离这个窗口有多远。屏幕另一头
//     某个窗口的左边缘恰好与鼠标同列，鼠标就被拽走了。
//   ②"吸附了没出现的窗口" —— 没过滤 DWMWA_CLOAKED。UWP 挂起窗口、其他虚拟桌面的
//     窗口、被 Shell 隐藏的窗口，IsWindowVisible 一律为真，但屏幕上根本没画。
//   ③"小窗口截不到" —— FindHoveredWindow 用"面积最小"启发式而不是 Z 序首个命中，
//     会跳过压在上面的窗口去选底下那个小的。
// 因此这里存 HWND 而不只是 RECT：既要按 PID 排除自己，也要显式排 Z 序。
struct WinInfo {
    HWND hw = nullptr;
    RECT r{ 0, 0, 0, 0 };
    int  z = 0;             // Z 序序号，0 = 最靠上
};
static std::vector<WinInfo> g_wins;

// DwmGetWindowAttribute 在整个会话里只 LoadLibrary 一次（枚举每个窗口都加载/释放
// 是毫无必要的开销，且 EnumWindows 回调里频繁加载 DLL 是不可取的做法）。
static HRESULT(WINAPI* g_dwmGetWindowAttribute)(HWND, DWORD, PVOID, DWORD) = nullptr;
static bool g_dwmReady = false;
static void EnsureDwm() {
    if (g_dwmReady) return;
    g_dwmReady = true;
    HMODULE dwm = LoadLibraryA("dwmapi.dll");
    if (dwm) {
        g_dwmGetWindowAttribute =
            (HRESULT(WINAPI*)(HWND, DWORD, PVOID, DWORD))GetProcAddress(dwm, "DwmGetWindowAttribute");
        // 不 FreeLibrary：保持模块映射到进程结束即可，避免重复加载。
    }
}

// 外壳窗口：IsWindowVisible 为真，但不是"用户想截的窗口"。
// 桌面本体、任务栏（含副屏副本）、通知溢出/自动播放宿主等。
static bool IsShellChrome(const wchar_t* cls) {
    static const wchar_t* kBad[] = {
        L"Progman", L"WorkerW",                      // 桌面
        L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", // 任务栏 / 副屏任务栏
        L"NotifyIconOverflowWindow",                 // 托盘溢出
        L"DV2ControlHost",                           // 自动播放等 Shell 宿主
    };
    for (const wchar_t* b : kBad) if (wcscmp(cls, b) == 0) return true;
    return false;
}

static BOOL CALLBACK EnumWnd(HWND hw, LPARAM) {
    if (!IsWindowVisible(hw)) return TRUE;

    // 排除本进程自己的窗口：全屏遮罩层、宿主隐藏主窗、上一轮残留的结果窗。
    // 少了这条，遮罩层自己会成为一个"覆盖全屏的窗口"，把所有吸附都吃掉。
    DWORD pid = 0;
    GetWindowThreadProcessId(hw, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;

    const LONG style = GetWindowLong(hw, GWL_STYLE);
    if (style & WS_MINIMIZE) return TRUE;

    // 工具窗口（托盘气泡、tooltip、下拉面板）默认排除；但带标题栏的浮动面板
    // 是真实可见的小窗口，用户常要截它，所以保留。
    const LONG ex = GetWindowLong(hw, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) && (style & WS_CAPTION) != WS_CAPTION) return TRUE;

    wchar_t cls[64] = {0};
    if (GetClassNameW(hw, cls, 64) > 0 && IsShellChrome(cls)) return TRUE;

    EnsureDwm();
    // DWMWA_CLOAKED(14)：UWP 挂起、位于其他虚拟桌面、被 Shell 隐藏。
    // 这是"屏幕上根本没有这个窗口却吸附上了"的主因。
    if (g_dwmGetWindowAttribute) {
        int cloaked = 0;
        if (SUCCEEDED(g_dwmGetWindowAttribute(hw, 14 /*DWMWA_CLOAKED*/, &cloaked, sizeof(cloaked)))
            && cloaked) return TRUE;
    }

    RECT r; bool ok = false;
    if (g_dwmGetWindowAttribute) {
        RECT er;
        if (SUCCEEDED(g_dwmGetWindowAttribute(hw, 9 /*DWMWA_EXTENDED_FRAME_BOUNDS*/, &er, sizeof(er)))) {
            r = er; ok = true;
        }
    }
    if (!ok && !GetWindowRect(hw, &r)) return TRUE;
    if (r.right - r.left <= 1 || r.bottom - r.top <= 1) return TRUE;

    // 完全落在虚拟屏幕之外（已拔掉的显示器、负坐标幽灵窗口）也要排除。
    RECT vs{ g.vx, g.vy, g.vx + g.vw, g.vy + g.vh }, inter;
    if (!IntersectRect(&inter, &r, &vs)) return TRUE;

    // 不再按"面积大于虚拟屏幕 80%"过滤：那会把最大化窗口也排除掉，
    // 而"框住最大化窗口"恰恰是最常用的场景。

    g_wins.push_back(WinInfo{ hw, r, 0 });
    return TRUE;
}

// 显式排 Z 序，不依赖"EnumWindows 恰好从上往下枚举"这个没写进文档的行为。
// GetTopWindow(NULL) 返回 Z 序最顶端，GW_HWNDNEXT 依次往下。
static void SortWinsByZOrder() {
    std::unordered_map<HWND, int> rank;
    int i = 0;
    for (HWND h = GetTopWindow(NULL); h; h = GetWindow(h, GW_HWNDNEXT)) rank[h] = i++;
    for (auto& w : g_wins) {
        auto it = rank.find(w.hw);
        w.z = (it == rank.end()) ? INT_MAX : it->second;
    }
    std::sort(g_wins.begin(), g_wins.end(),
              [](const WinInfo& a, const WinInfo& b) { return a.z < b.z; });
}

static RECT Norm(POINT a, POINT b) {
    RECT r;
    r.left   = std::min(a.x, b.x); r.top    = std::min(a.y, b.y);
    r.right  = std::max(a.x, b.x); r.bottom = std::max(a.y, b.y);
    return r;
}

// Snap a point to nearby window edges (10px threshold).
//
// 两处关键修正（旧实现都错，是"吸附到空气"的直接原因）：
//   1) 投影区间判断：鼠标必须落在窗口 ±t 的包围盒内才考虑该窗口。旧实现把每条边
//      当成无限长直线——屏幕另一头某个窗口的左边缘与鼠标同列，鼠标就被拽过去。
//      包围盒（而非纯边内）允许四角斜向吸附，和微信一致。
//   2) 命中即返回，且不跨窗口混搭：g_wins 已按 Z 序排好，取最靠上那个窗口，x 和 y
//      都取自**同一个**矩形。旧实现遍历全部窗口并让最后一个生效，既可能吸到最底
//      层窗口，也可能 x 吸 A 窗口、y 吸 B 窗口，合成出一个不存在的点。
static void Snap(POINT& p) {
    const int t = 10;
    for (const WinInfo& wi : g_wins) {
        const RECT& w = wi.r;
        if (p.x < w.left - t || p.x > w.right  + t) continue;
        if (p.y < w.top  - t || p.y > w.bottom + t) continue;
        int sx = p.x, sy = p.y;
        if (std::abs(p.x - w.left)      <= t) sx = w.left;
        else if (std::abs(p.x - w.right) <= t) sx = w.right;
        if (std::abs(p.y - w.top)       <= t) sy = w.top;
        else if (std::abs(p.y - w.bottom) <= t) sy = w.bottom;
        if (sx != p.x || sy != p.y) { p.x = sx; p.y = sy; return; }
    }

    // 兜底：窗口都吸不上时，吸附到**显示器边缘**。
    //
    // 为什么必须有这条（用户："全屏不能吸附"）：桌面是 Shell 窗口，被 IsShellChrome
    // 排除了，所以"什么都没开、只有桌面"时 g_wins 是空的 —— 上面那个循环一次都进不去，
    // 拖到离屏幕边 7px 也吸不上，整屏永远框不准（实测 1893x1053，应为 1900x1060）。
    // 有全屏/最大化窗口时窗口边正好压在屏幕边上，本来就能吸，所以这个问题只在
    // "裸桌面"和"全屏应用没有可枚举窗口"时才暴露，更隐蔽。
    //
    // 用 rcMonitor 而不是 rcWork：截图常常要连任务栏一起框进去。
    HMONITOR hm = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfoW(hm, &mi)) return;
    const RECT& s = mi.rcMonitor;
    int sx = p.x, sy = p.y;
    if (std::abs(p.x - s.left)      <= t) sx = s.left;
    else if (std::abs(p.x - s.right) <= t) sx = s.right;
    if (std::abs(p.y - s.top)       <= t) sy = s.top;
    else if (std::abs(p.y - s.bottom) <= t) sy = s.bottom;
    p.x = sx; p.y = sy;
}

// 命中 Z 序最靠上的窗口（g_wins 已按 z 升序，第一个命中就是顶层那个）。
// 旧实现取"面积最小"，会跳过压在上面的窗口去选底下那个小的 —— 表现为
// "这里有个小窗口，但就是高亮/截取不到它"。
static RECT FindHoveredWindow(POINT p) {
    for (const WinInfo& wi : g_wins)
        if (PtInRect(&wi.r, p)) return wi.r;
    return RECT{ 0, 0, 0, 0 };
}

// Direct BGRA pixel helpers: we need to control the alpha channel ourselves
// because GDI's DrawRectOutline only touches RGB and leaves alpha unchanged.
static inline void SetPixelBGRA(int x, int y, COLORREF c, BYTE a) {
    if (x < 0 || x >= g.vw || y < 0 || y >= g.vh) return;
    BYTE* p = (BYTE*)g.bits + ((size_t)y * g.vw + x) * 4;
    p[0] = GetBValue(c);
    p[1] = GetGValue(c);
    p[2] = GetRValue(c);
    p[3] = a;
}

static void DrawRectOutlineBGRA(const RECT& r, int xoff, int yoff,
                                COLORREF c, int width) {
    int left   = r.left   - xoff;
    int top    = r.top    - yoff;
    int right  = r.right  - xoff;   // exclusive
    int bottom = r.bottom - yoff;   // exclusive
    if (right <= left || bottom <= top) return;
    for (int x = left; x < right; ++x) {
        for (int w = 0; w < width; ++w) {
            SetPixelBGRA(x, top + w, c, 255);
            SetPixelBGRA(x, bottom - 1 - w, c, 255);
        }
    }
    for (int y = top; y < bottom; ++y) {
        for (int w = 0; w < width; ++w) {
            SetPixelBGRA(left + w, y, c, 255);
            SetPixelBGRA(right - 1 - w, y, c, 255);
        }
    }
}

static void FillRectBGRA(int x0, int y0, int x1, int y1, COLORREF c, BYTE a) {
    x0 = std::max(x0, 0); y0 = std::max(y0, 0);
    x1 = std::min(x1, g.vw); y1 = std::min(y1, g.vh);
    if (x1 <= x0 || y1 <= y0) return;
    BYTE b = GetBValue(c), gc = GetGValue(c), rc = GetRValue(c);
    for (int y = y0; y < y1; ++y) {
        BYTE* row = (BYTE*)g.bits + ((size_t)y * g.vw + x0) * 4;
        for (int x = x0; x < x1; ++x) {
            row[0] = b; row[1] = gc; row[2] = rc; row[3] = a;
            row += 4;
        }
    }
}

// ---------------------------------------------------------------------------
// 渲染：OverlayState + 脏区 band 上传（P0 重绘重构）
//
// 旧实现每帧做三件整屏的事：memcpy(dim→bits, 8.3MB)、逐像素挖洞、整窗
// UpdateLayeredWindow。实测 1080p 单帧 P50 8.77ms / P95 12.53ms（吸附本身只要
// 2.5µs），所以"卡"的根因全在这里。新实现：
//   · g.bits 持久化，只对"这一帧真正变了的那几个矩形"做恢复/重画/上传；
//   · 脏区由 OverlayState 前后状态推导（区域异或），不手工维护 → 残影在构造上
//     不可能（人肉维护"旧洞∪新洞∪旧框∪新框∪标签"必漏，标签常在洞外）；
//   · 每帧 1~4 个细矩形分别 UpdateLayeredWindowIndirect(prcDirty)；拖动时差集是
//     L 形，拆 2~3 条细矩形 0.16~0.23ms，而它的外接矩形要 4.7~6.4ms（差 30 倍），
//     所以禁止把脏区合并成外接矩形，只在成本模型判降时才合并；
//   · 状态没变直接 return：同一窗口内移动 0 成本（>90% 的帧走这条）；
//   · 三级回退：band 超过上限 → 贪心合并；ini render_full=1 或首帧 → 整窗 ULW，
//     与旧行为完全等价（prcDirty 文档只说 hint，某些 GPU/RDP 会整表上传）。
// 不变量：g.bits 中任何像素 alpha == 255（本项目没有合法半透明）。GDI 画字会把
// 字形像素的 alpha 写成 0（实测 396/1800，肉眼可见"文字镂空"），所以标签画完必须
// 把该矩形 alpha 重写回 255。持久化缓冲之后"漏修补"不再是闪一帧，而是永久透明斑。
struct OverlayState {
    RECT hole{0, 0, 0, 0};      // 透出干净桌面的区域（窗口坐标）
    RECT label{0, 0, 0, 0};     // 尺寸标签底框（窗口坐标）
    char text[40]{};            // 标签文字。**必须进状态**：拖动时标签框常常不动，
                                // 只有文字从 "300 x 200" 变成 "301 x 200"，
                                // 若相等比较只看矩形，这一帧会被判成"没变"直接
                                // return，标签就永远停在旧尺寸上。
    int  textW = 0, textH = 0;
    bool labelValid = false;
    bool drag = false;          // 决定边框颜色（蓝=悬停 / 红=拖拽）
    bool adjust = false;        // 调整态：画八个把手，且脏区 footprint 要外扩
    // ---- 原地标注 ----
    // 按值存放（不是指针/版本号）：SameOverlay 的判据是"画 a 与画 b 是否逐像素
    // 相同"，而标注的内容就是画面的一部分，逐条逐点比较才是那个判据本身。
    // 代价是每帧两次 vector 拷贝（BuildOverlayState 一次、g_prev = next 一次），
    // 一笔 5000 点的长笔画也就 40KB —— 比"漏比导致残影"便宜得多。
    std::vector<Anno> annos;    // 已完成的标注（绝对虚拟屏坐标）
    Anno annCur;                // 正在画的那一条
    bool drawingAnn = false;
    RECT annBox{0, 0, 0, 0};    // 所有标注的墨迹外接框（窗口坐标）∩ hole。
                                // 只用于算脏区（见 Redraw 的 extra），不进 StateRegion：
                                // 它按构造是 hole 的子集，而 hole 本来就在 StateRegion 里。
    // ---- 框选识别层 ----
    // 词框按值存（窗口坐标）：画师只读状态、不读全局，SameOverlay 才能"判等 ⟺ 像素相同"。
    bool textHasFocus = false;
    RECT textRect{0, 0, 0, 0};          // 子框（窗口坐标 ∩ hole）
    std::vector<OcrWord> words;          // 词框（窗口坐标）
    std::vector<unsigned char> wordSel;  // 与 words 等长；1 = 被选中
    int  wordCount = 0;                  // = words.size()，派生量，不参与判等（给日志/测试看）
    int  selCount  = 0;                  // = wordSel 里 1 的个数，派生量
};
static OverlayState g_prev;
static bool g_prevValid = false;
static bool g_renderFull = false;   // [screenshot] render_full=1 → 走旧的整窗上传
static HFONT g_labelFont = nullptr;

static inline bool REmpty(const RECT& r) {
    return r.right <= r.left || r.bottom <= r.top;
}
static inline RECT RIntersect(const RECT& a, const RECT& b) {
    RECT r;
    r.left = std::max(a.left, b.left);   r.top = std::max(a.top, b.top);
    r.right = std::min(a.right, b.right); r.bottom = std::min(a.bottom, b.bottom);
    if (REmpty(r)) { r = RECT{0, 0, 0, 0}; }
    return r;
}
static inline long long RArea(const RECT& r) {
    return REmpty(r) ? 0 : (long long)(r.right - r.left) * (r.bottom - r.top);
}
static inline RECT RUnion(const RECT& a, const RECT& b) {
    if (REmpty(a)) return b;
    if (REmpty(b)) return a;
    RECT r;
    r.left = std::min(a.left, b.left);   r.top = std::min(a.top, b.top);
    r.right = std::max(a.right, b.right); r.bottom = std::max(a.bottom, b.bottom);
    return r;
}
static bool SameAnno(const Anno& a, const Anno& b) {
    if (a.tool != b.tool || a.color != b.color || a.widthIdx != b.widthIdx) return false;
    if (a.pts.size() != b.pts.size()) return false;
    for (size_t i = 0; i < a.pts.size(); ++i) {
        if (a.pts[i].x != b.pts[i].x || a.pts[i].y != b.pts[i].y) return false;
    }
    return true;
}
static bool SameAnnos(const std::vector<Anno>& a, const std::vector<Anno>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!SameAnno(a[i], b[i])) return false;
    }
    return true;
}
// 框选识别层的"变没变"。只比**画师用得到的东西**：子框、词框几何、每个词的选中位。
// 词里的 text 故意不比 —— 画师只画框不画字，文字变了像素不会变，比了只会让
// "判不等但像素相同"变多（那条是非致命的，但也没必要自己制造）。
static bool SameTextLayer(const OverlayState& a, const OverlayState& b) {
    if (a.textHasFocus != b.textHasFocus) return false;
    if (a.textRect.left != b.textRect.left || a.textRect.top != b.textRect.top ||
        a.textRect.right != b.textRect.right || a.textRect.bottom != b.textRect.bottom) return false;
    if (a.words.size() != b.words.size()) return false;
    for (size_t i = 0; i < a.words.size(); ++i) {
        const OcrWord& x = a.words[i];
        const OcrWord& y = b.words[i];
        if (x.x != y.x || x.y != y.y || x.w != y.w || x.h != y.h) return false;
    }
    return a.wordSel == b.wordSel;
}

static bool SameOverlay(const OverlayState& a, const OverlayState& b) {
    if (a.drag != b.drag || a.labelValid != b.labelValid) return false;
    // 进入/离开调整态会整体出现或消失八个把手，且 footprint 也会变（按真实把手矩形求并）
    if (a.adjust != b.adjust) return false;
    if (a.hole.left != b.hole.left || a.hole.top != b.hole.top ||
        a.hole.right != b.hole.right || a.hole.bottom != b.hole.bottom) return false;
    if (a.label.left != b.label.left || a.label.top != b.label.top ||
        a.label.right != b.label.right || a.label.bottom != b.label.bottom) return false;
    // 标签矩形没动但文字变了（拖动时最常见：框没动、尺寸数字在跳），这一帧必须重画。
    if (a.labelValid && std::strncmp(a.text, b.text, sizeof(a.text)) != 0) return false;
    // 标注：逐条逐点比，不用哈希（哈希碰撞会让"变了"被误判成"没变"，那是永久残影）。
    // annBox 完全由 annos/annCur/hole 推出，不重复比较。
    if (a.drawingAnn != b.drawingAnn) return false;
    if (!SameAnnos(a.annos, b.annos)) return false;
    if (a.drawingAnn && !SameAnno(a.annCur, b.annCur)) return false;
    // 框选识别层：子框与每个词框都逐字段比，选中状态逐位比（vector<unsigned char>
    // 直接比就是逐位；不用哈希 —— 我们本来就持有全量数据，哈希只会引入碰撞风险）。
    // 词里的 text 不参与比较：画师只画框，不画字，文字变了像素也不会变。
    if (!SameTextLayer(a, b)) return false;
    return true;
}

// 标注的墨迹外接框（绝对虚拟屏坐标）。只加不减：框大了只是多画一块（幂等），
// 框小了就是漏画 → 屏幕上留残影。
static void GrowAnnoBox(RECT& box, const Anno& a) {
    if (a.pts.empty()) return;
    const int wi = (a.widthIdx < 0) ? 0 : (a.widthIdx > 3 ? 3 : a.widthIdx);
    const int w = kAnnWidths[wi];
    int pad = w / 2 + 1;                 // 半笔宽 + 1px 锯齿余量
    if (a.tool == AT_ARROW && a.pts.size() >= 2) {
        // 箭头两翼从尖端往回张开 28°，垂直于主干方向最多甩出 0.47·len；主干水平时
        // 这一截整个落在两点包围盒之外，必须显式补进来。
        pad += std::max(10, 5 * w);
    }
    for (size_t i = 0; i < a.pts.size(); ++i) {
        RECT r{ a.pts[i].x - pad, a.pts[i].y - pad,
                a.pts[i].x + pad + 1, a.pts[i].y + pad + 1 };
        box = RUnion(box, r);
    }
}

// 由当前交互状态算出本帧要画什么（窗口坐标：减去虚拟屏原点）
static OverlayState BuildOverlayState() {
    OverlayState s;
    s.drag = g.drag;
    s.adjust = (g.phase == Phase::Adjust);
    s.annos = g.annos;
    s.annCur = g.annCur;
    s.drawingAnn = g.drawingAnn;
    RECT hole = { 0, 0, 0, 0 };
    if (g.phase == Phase::Adjust) {
        // 调整态：洞就是选区本身，绝不能再跟着鼠标下面的窗口跑
        hole = g.sel;
    } else if (g.drag && g.sel.right > g.sel.left && g.sel.bottom > g.sel.top) {
        hole = g.sel;
    } else if (!g.drag) {
        hole = FindHoveredWindow(g.cur);
    }
    if (REmpty(hole)) return s;                     // 什么都没有：整屏纯遮罩
    hole.left -= g.vx; hole.right -= g.vx;
    hole.top -= g.vy;  hole.bottom -= g.vy;
    s.hole = RIntersect(hole, RECT{0, 0, g.vw, g.vh});
    if (REmpty(s.hole)) return s;

    // 尺寸标签在拖拽态与调整态都出现，位置与原实现一致（选区上方，放不下就压到选区内）
    if (g.drag || g.phase == Phase::Adjust) {
        // 文字写进状态本身：它是"这一帧要画什么"的一部分，不是渲染用的临时量。
        // 之前放在全局 g_labelText 里、又不参与 SameOverlay 比较，于是标签矩形不动
        // 而数字在跳的那些帧被判成"没变"，标签永远停在旧尺寸。
        std::snprintf(s.text, sizeof(s.text), "%d x %d",
                      g.sel.right - g.sel.left, g.sel.bottom - g.sel.top);
        if (!g_labelFont) {
            g_labelFont = CreateFontW(18, 0, 0, 0, FW_BOLD, 0, 0, 0,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                      CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                      DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        }
        HGDIOBJ oldf = SelectObject(g.hDc, g_labelFont);
        SIZE ts;
        ZeroMemory(&ts, sizeof(ts));
        GetTextExtentPoint32A(g.hDc, s.text, (int)std::strlen(s.text), &ts);
        SelectObject(g.hDc, oldf);
        s.textW = ts.cx; s.textH = ts.cy;

        const int pad = 4;
        int tx = s.hole.left;
        int ty = s.hole.top - 22;
        if (ty < 0) ty = s.hole.top + 4;
        RECT lb = { tx - pad, ty - pad, tx + ts.cx + pad, ty + ts.cy + pad };
        s.label = RIntersect(lb, RECT{0, 0, g.vw, g.vh});
        s.labelValid = !REmpty(s.label);
    }

    // 标注足迹。与洞求交，而不是"外扩"：墨迹是钳在选区里画的，裁剪也只会保留
    // 选区那一块，所以预览必须与"保存出来的那张"逐像素一致 —— 画到选区外的
    // 部分本来就存不下来，预览里也不该露出来。
    {
        RECT ab{ 0, 0, 0, 0 };
        for (size_t i = 0; i < g.annos.size(); ++i) GrowAnnoBox(ab, g.annos[i]);
        if (g.drawingAnn) GrowAnnoBox(ab, g.annCur);
        if (!REmpty(ab)) {
            ab.left -= g.vx; ab.right -= g.vx;
            ab.top  -= g.vy; ab.bottom -= g.vy;
            s.annBox = RIntersect(ab, s.hole);
        }
    }

    // 框选识别层：绝对坐标 → 窗口坐标。词框**不**与洞求交（高亮按整词画，
    // 溢出洞的部分由画师的 clip 裁掉，这里裁会把词框改小、脏区就跟着偏小）。
    // 正在拉子框时用"拖动矩形"顶上：虚线框实时跟着鼠标走，而且脏区走的是同一条
    // textRect 通道，不需要为"正在拖"再开一类状态。
    RECT trAbs = g.textRect;
    if (g.textDragging) trAbs = Norm(g.textDragStart, g.cur);
    if (!REmpty(trAbs)) {
        RECT tr = { trAbs.left - g.vx, trAbs.top - g.vy,
                    trAbs.right - g.vx, trAbs.bottom - g.vy };
        s.textRect = RIntersect(tr, s.hole);
        s.textHasFocus = !REmpty(s.textRect);
    }
    s.words.reserve(g.words.size());
    for (size_t i = 0; i < g.words.size(); ++i) {
        OcrWord w = g.words[i];
        w.x -= g.vx;
        w.y -= g.vy;
        s.words.push_back(w);
    }
    s.wordSel = g.wordSel;
    s.wordCount = (int)s.words.size();
    for (size_t i = 0; i < s.wordSel.size(); ++i) if (s.wordSel[i]) ++s.selCount;
    return s;
}

// 从 dim 底图按行恢复一个矩形（等价于旧实现的整屏 memcpy，但只做脏区那么多行）
static void RestoreFromDim(const RECT& band) {
    const int wBytes = (band.right - band.left) * 4;
    for (int y = band.top; y < band.bottom; ++y) {
        std::memcpy((BYTE*)g.bits + ((size_t)y * g.vw + band.left) * 4,
                    g.dim.data() + ((size_t)y * g.vw + band.left) * 4,
                    (size_t)wBytes);
    }
}

// 在 band 内把选区像素写成干净桌面（不透明）
static void PaintHole(const RECT& band, const OverlayState& s) {
    RECT r = RIntersect(band, s.hole);
    if (REmpty(r)) return;
    const int wBytes = (r.right - r.left) * 4;
    for (int y = r.top; y < r.bottom; ++y) {
        BYTE* dst = (BYTE*)g.bits + ((size_t)y * g.vw + r.left) * 4;
        const BYTE* src = g.screen.data() + ((size_t)y * g.vw + r.left) * 4;
        std::memcpy(dst, src, (size_t)wBytes);
        for (int x = 0; x < (r.right - r.left); ++x) dst[x * 4 + 3] = 255;
    }
}

// 边框：只画落在 band 内的那几段
static void PaintBorder(const RECT& band, const OverlayState& s) {
    if (REmpty(s.hole)) return;
    const COLORREF c = s.drag ? RGB(230, 30, 30) : RGB(0, 180, 255);
    const int w = 2;
    RECT edges[4] = {
        { s.hole.left, s.hole.top, s.hole.right, s.hole.top + w },                  // 上
        { s.hole.left, s.hole.bottom - w, s.hole.right, s.hole.bottom },            // 下
        { s.hole.left, s.hole.top, s.hole.left + w, s.hole.bottom },                // 左
        { s.hole.right - w, s.hole.top, s.hole.right, s.hole.bottom },              // 右
    };
    for (int i = 0; i < 4; ++i) {
        RECT r = RIntersect(band, edges[i]);
        if (REmpty(r)) continue;
        for (int y = r.top; y < r.bottom; ++y) {
            BYTE* row = (BYTE*)g.bits + ((size_t)y * g.vw + r.left) * 4;
            for (int x = r.left; x < r.right; ++x) {
                row[0] = GetBValue(c); row[1] = GetGValue(c); row[2] = GetRValue(c);
                row[3] = 255;
                row += 4;
            }
        }
    }
}

// 尺寸标签（底 + 字）。GDI 画字会破坏 alpha，所以画完立刻把标签矩形 alpha 修回 255。
static void PaintLabel(const RECT& band, const OverlayState& s) {
    if (!s.labelValid) return;
    RECT r = RIntersect(band, s.label);
    if (REmpty(r)) return;

    // 底色（RGB(30,30,30) 不透明）
    for (int y = r.top; y < r.bottom; ++y) {
        BYTE* row = (BYTE*)g.bits + ((size_t)y * g.vw + r.left) * 4;
        for (int x = r.left; x < r.right; ++x) {
            row[0] = 30; row[1] = 30; row[2] = 30; row[3] = 255;
            row += 4;
        }
    }
    // 文字：用 clip 限制在 band∩label 内，避免画到 band 之外污染持久缓冲
    if (s.textW > 0 && s.text[0]) {
        HGDIOBJ oldf = SelectObject(g.hDc, g_labelFont);
        int oldMode = SetBkMode(g.hDc, TRANSPARENT);
        COLORREF oldColor = SetTextColor(g.hDc, RGB(255, 255, 255));
        int saved = SaveDC(g.hDc);
        IntersectClipRect(g.hDc, r.left, r.top, r.right, r.bottom);
        const int tx = s.label.left + 4;    // pad
        const int ty = s.label.top + 4;
        TextOutA(g.hDc, tx, ty, s.text, (int)std::strlen(s.text));
        RestoreDC(g.hDc, saved);
        SetTextColor(g.hDc, oldColor);
        SetBkMode(g.hDc, oldMode);
        SelectObject(g.hDc, oldf);
    }
    // alpha 闭合：标签矩形内一律 255（修 GDI 画字把 alpha 写 0 导致的"文字镂空"）
    for (int y = r.top; y < r.bottom; ++y) {
        BYTE* row = (BYTE*)g.bits + ((size_t)y * g.vw + r.left) * 4;
        for (int x = r.left; x < r.right; ++x) { row[3] = 255; row += 4; }
    }
}

// ---- 调整态的八个把手 ----
// 编号：0=左上 1=上中 2=右上 3=右中 4=右下 5=下中 6=左下 7=左中
constexpr int kHandleSize = 8;      // 角把手边长（8x8）
constexpr int kEdgeHandleLen = 24;   // 边中点把手沿边方向的长度
// 注：不再有 kHandleH。足迹改成按真实把手矩形求并（见 HoleFootprint），
// 常数外扩在"选区比边中点把手还窄"时会漏掉甩出去的那几列像素。

// 入参与输出同一坐标系：绘制时传窗口坐标的 hole，命中测试时传虚拟屏坐标的 g.sel。
static void BuildHandleRects(const RECT& sel, RECT out[8]) {
    const int c = kHandleSize / 2;
    const int e = kEdgeHandleLen / 2;
    const int mx = (sel.left + sel.right) / 2;
    const int my = (sel.top + sel.bottom) / 2;
    out[0] = { sel.left  - c, sel.top    - c, sel.left  + c, sel.top    + c };
    out[1] = { mx - e,        sel.top    - c, mx + e,        sel.top    + c };
    out[2] = { sel.right - c, sel.top    - c, sel.right + c, sel.top    + c };
    out[3] = { sel.right - c, my - e,        sel.right + c, my + e         };
    out[4] = { sel.right - c, sel.bottom - c, sel.right + c, sel.bottom + c };
    out[5] = { mx - e,        sel.bottom - c, mx + e,        sel.bottom + c };
    out[6] = { sel.left  - c, sel.bottom - c, sel.left  + c, sel.bottom + c };
    out[7] = { sel.left  - c, my - e,         sel.left  + c, my + e         };
}

// p 为虚拟屏坐标；非调整态一律返回 -1。
static int HitHandle(POINT p) {
    if (g.phase != Phase::Adjust) return -1;
    RECT hs[8];
    BuildHandleRects(g.sel, hs);
    for (int i = 0; i < 8; ++i)
        if (PtInRect(&hs[i], p)) return i;
    return -1;
}

// 拖把手改选区：只动把手对应的那一/两条边，允许拖过对面（越过就交换，不夹死）。
// 交换后必须把 dragHandle 重新映射到几何上同一个角/边，否则接下来拖的是错误那条边。
static void ApplyHandleDrag(POINT p) {
    if (g.dragHandle < 0) return;
    int i = g.dragHandle;
    switch (i) {
        case 0: g.sel.left = p.x;  g.sel.top = p.y;    break;
        case 1:                     g.sel.top = p.y;    break;
        case 2: g.sel.right = p.x; g.sel.top = p.y;    break;
        case 3: g.sel.right = p.x;                     break;
        case 4: g.sel.right = p.x; g.sel.bottom = p.y; break;
        case 5:                     g.sel.bottom = p.y; break;
        case 6: g.sel.left = p.x;  g.sel.bottom = p.y; break;
        case 7: g.sel.left = p.x;                      break;
        default: break;
    }
    const bool swapX = g.sel.left > g.sel.right;
    const bool swapY = g.sel.top > g.sel.bottom;
    if (swapX) std::swap(g.sel.left, g.sel.right);
    if (swapY) std::swap(g.sel.top, g.sel.bottom);
    if (swapX) {
        if (i == 0) i = 2; else if (i == 2) i = 0;
        else if (i == 6) i = 4; else if (i == 4) i = 6;
        else if (i == 7) i = 3; else if (i == 3) i = 7;
    }
    if (swapY) {
        if (i == 0) i = 6; else if (i == 6) i = 0;
        else if (i == 2) i = 4; else if (i == 4) i = 2;
        else if (i == 1) i = 5; else if (i == 5) i = 1;
    }
    g.dragHandle = i;
    // 钳回虚拟屏：把手可以拖到屏幕边缘，但不能拖出去
    if (g.sel.left   < g.vx)       g.sel.left = g.vx;
    if (g.sel.top    < g.vy)       g.sel.top = g.vy;
    if (g.sel.right  > g.vx+ g.vw) g.sel.right = g.vx + g.vw;
    if (g.sel.bottom > g.vy+ g.vh) g.sel.bottom = g.vy + g.vh;
}

// 把手绘制。必须排在 PaintHole / PaintBorder **之后**：把手跨洞边界、一半落在洞内，
// 先画会被 PaintHole 用干净像素整块覆盖，只剩下洞外那半边。
static void PaintHandles(const RECT& band, const OverlayState& s) {
    if (!s.adjust || REmpty(s.hole)) return;
    RECT hs[8];
    BuildHandleRects(s.hole, hs);
    for (int i = 0; i < 8; ++i) {
        // 白底
        RECT r = RIntersect(band, hs[i]);
        if (!REmpty(r)) {
            for (int y = r.top; y < r.bottom; ++y) {
                BYTE* row = (BYTE*)g.bits + ((size_t)y * g.vw + r.left) * 4;
                for (int x = r.left; x < r.right; ++x) {
                    row[0] = 255; row[1] = 255; row[2] = 255; row[3] = 255;
                    row += 4;
                }
            }
        }
        // 1px 蓝描边（白块在浅色桌面上也要看得见）。同样按 band 裁剪，
        // 且统一写 alpha=255，守住"g.bits 里任何像素 alpha == 255"这条不变量。
        RECT edges[4] = {
            { hs[i].left,      hs[i].top,        hs[i].right,     hs[i].top + 1    },
            { hs[i].left,      hs[i].bottom - 1, hs[i].right,     hs[i].bottom     },
            { hs[i].left,      hs[i].top,        hs[i].left + 1,  hs[i].bottom     },
            { hs[i].right - 1, hs[i].top,        hs[i].right,     hs[i].bottom     },
        };
        for (int k = 0; k < 4; ++k) {
            RECT e = RIntersect(band, edges[k]);
            if (REmpty(e)) continue;
            for (int y = e.top; y < e.bottom; ++y) {
                BYTE* row = (BYTE*)g.bits + ((size_t)y * g.vw + e.left) * 4;
                for (int x = e.left; x < e.right; ++x) {
                    row[0] = 215; row[1] = 120; row[2] = 0; row[3] = 255;  // RGB(0,120,215)
                    row += 4;
                }
            }
        }
    }
}

// ---- 标注绘制：自写像素光栅化 ----
// 为什么不用 GDI 画：① GDI 会把线条/字形的 alpha 写成 0 或半透明，而 g.bits 的
// 不变量是"被画到的像素 alpha 一律 255"（漏修补是**永久**透明斑，不是闪一帧）；
// ② 同一套图形还要往最终截图（24bpp RGB、没有 alpha 通道）上再画一遍，用 GDI 就得
// 维护两套 DC/位图/DPI 缩放，两边像素对不齐；③ 逐像素写才能顺手把"只在 band 内、
// 只在洞内"的裁剪做进同一个函数，不会画脏持久缓冲。
struct Surf {
    BYTE* px = nullptr;
    int   stride = 0;          // 每行字节数
    int   bpp = 4;             // 4 = 遮罩层 BGRA；3 = 最终截图 RGB（无 alpha）
    bool  bgr = true;          // 4bpp 是 BGRA；3bpp 的 CaptureImage.rgb 是 RGB 顺序
    int   w = 0, h = 0;        // 像素尺寸
    int   ox = 0, oy = 0;      // 绘制坐标 → 像素坐标的偏移（绝对屏幕坐标 - ox/oy）
    RECT  clip{ 0, 0, 0, 0 };  // 绘制坐标系下的裁剪矩形（左闭右开）
};

static inline void SPut(Surf& s, int x, int y, const int c[3]) {
    if (x < s.clip.left || x >= s.clip.right || y < s.clip.top || y >= s.clip.bottom) return;
    const int sx = x - s.ox, sy = y - s.oy;
    if (sx < 0 || sy < 0 || sx >= s.w || sy >= s.h) return;
    BYTE* p = s.px + (size_t)sy * s.stride + (size_t)sx * s.bpp;
    if (s.bgr) { p[0] = (BYTE)c[2]; p[1] = (BYTE)c[1]; p[2] = (BYTE)c[0]; }
    else       { p[0] = (BYTE)c[0]; p[1] = (BYTE)c[1]; p[2] = (BYTE)c[2]; }
    if (s.bpp == 4) p[3] = 255;      // 不变量：被画到的像素一律不透明
}

// 粗线 = 沿 Bresenham 每步盖一个 w×w 的方章。用方章不用圆章：厚度精确等于 w
// （半径 w/2 的圆章画出来是 w+1 厚），且斜线上相邻方章必然重叠，不会留锯齿缺口。
static void SLine(Surf& s, int x0, int y0, int x1, int y1, int w, const int c[3]) {
    if (w < 1) w = 1;
    const int lo = -(w - 1) / 2;
    const int hi = lo + w - 1;
    // 整条线的包围盒都在裁剪区外 → 直接返回。长笔画配小 band 时这是主路径：
    // 省掉的是 len×w² 次逐像素判断，否则一条横跨全屏的画笔会把 8 个 band 全拖慢。
    if (std::max(x0, x1) + hi < s.clip.left || std::min(x0, x1) + lo >= s.clip.right ||
        std::max(y0, y1) + hi < s.clip.top  || std::min(y0, y1) + lo >= s.clip.bottom) return;
    int dx = std::abs(x1 - x0), sx = (x0 < x1) ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        for (int a = lo; a <= hi; ++a) {
            for (int b = lo; b <= hi; ++b) SPut(s, x0 + a, y0 + b, c);
        }
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void SRect(Surf& s, int x0, int y0, int x1, int y1, int w, const int c[3]) {
    const int l = std::min(x0, x1), r = std::max(x0, x1);
    const int t = std::min(y0, y1), b = std::max(y0, y1);
    SLine(s, l, t, r, t, w, c);
    SLine(s, l, b, r, b, w, c);
    SLine(s, l, t, l, b, w, c);
    SLine(s, r, t, r, b, w, c);
}

static void SEllipse(Surf& s, int x0, int y0, int x1, int y1, int w, const int c[3]) {
    const int l = std::min(x0, x1), r = std::max(x0, x1);
    const int t = std::min(y0, y1), b = std::max(y0, y1);
    const double cx = (l + r) * 0.5, cy = (t + b) * 0.5;
    const double rx = (r - l) * 0.5, ry = (b - t) * 0.5;
    if (rx < 1.0 || ry < 1.0) { SLine(s, l, t, r, b, w, c); return; }  // 太扁：退化成线段
    // 参数采样：周长 ≈ π·(rx+ry)，按每 1px 一个点取步数，下限 24 保证小椭圆也闭合。
    int steps = (int)((rx + ry) * 3.2);
    if (steps < 24) steps = 24;
    if (steps > 4096) steps = 4096;
    int px = 0, py = 0;
    for (int i = 0; i <= steps; ++i) {
        const double a = 6.283185307179586 * (double)i / (double)steps;
        const int qx = (int)std::lround(cx + rx * std::cos(a));
        const int qy = (int)std::lround(cy + ry * std::sin(a));
        if (i == 0) { px = qx; py = qy; continue; }
        if (qx != px || qy != py) SLine(s, px, py, qx, qy, w, c);   // 重复点跳过
        px = qx; py = qy;
    }
}

static void SArrow(Surf& s, int x0, int y0, int x1, int y1, int w, const int c[3]) {
    SLine(s, x0, y0, x1, y1, w, c);                       // 主干：(x0,y0) → 箭头尖
    const double ang = std::atan2((double)(y1 - y0), (double)(x1 - x0));
    const int len = std::max(10, 5 * w);                  // 头部长度随笔宽放大，细笔也看得见
    const double kA = 0.48869219;                         // 28°：两翼与主干的夹角
    for (int k = -1; k <= 1; k += 2) {
        const double a = ang + k * kA;
        const int hx = (int)std::lround(x1 - len * std::cos(a));   // 从尖端往回画
        const int hy = (int)std::lround(y1 - len * std::sin(a));
        SLine(s, x1, y1, hx, hy, w, c);
    }
}

static void PaintAnno(Surf& s, const Anno& a) {
    if (a.tool == AT_NONE || a.pts.empty()) return;
    const int ci = (a.color < 0) ? 0 : (a.color > 5 ? 5 : a.color);
    const int wi = (a.widthIdx < 0) ? 0 : (a.widthIdx > 3 ? 3 : a.widthIdx);
    const int* c = kAnnColors[ci];
    const int w = kAnnWidths[wi];
    if (a.tool == AT_PEN) {
        if (a.pts.size() == 1) {   // 只点了一下：点一个方点，别什么都不画
            SLine(s, a.pts[0].x, a.pts[0].y, a.pts[0].x, a.pts[0].y, w, c);
            return;
        }
        for (size_t i = 1; i < a.pts.size(); ++i) {
            SLine(s, a.pts[i - 1].x, a.pts[i - 1].y, a.pts[i].x, a.pts[i].y, w, c);
        }
        return;
    }
    if (a.pts.size() < 2) return;
    const POINT p0 = a.pts[0], p1 = a.pts[1];
    switch (a.tool) {
        case AT_RECT:    SRect(s, p0.x, p0.y, p1.x, p1.y, w, c);    break;
        case AT_ELLIPSE: SEllipse(s, p0.x, p0.y, p1.x, p1.y, w, c); break;
        case AT_ARROW:   SArrow(s, p0.x, p0.y, p1.x, p1.y, w, c);   break;
        default: break;
    }
}

// 把标注画到遮罩层上（band 内、洞内）。两处裁剪缺一不可：
//   · band：g.bits 是持久缓冲，画到 band 之外就是"没上传的脏像素"，下次别的 band
//     上传时会把它当成旧内容留着；
//   · hole：选区外是压暗的遮罩区，墨迹漏出去既穿帮、又存不进最终截图。
static void PaintAnnos(const RECT& band, const OverlayState& s) {
    if (REmpty(s.annBox)) return;
    RECT clip = { band.left + g.vx, band.top + g.vy, band.right + g.vx, band.bottom + g.vy };
    RECT hole = { s.hole.left + g.vx, s.hole.top + g.vy,
                  s.hole.right + g.vx, s.hole.bottom + g.vy };
    clip = RIntersect(clip, hole);
    if (REmpty(clip)) return;
    Surf su;
    su.px = (BYTE*)g.bits;
    su.stride = g.vw * 4;
    su.bpp = 4;
    su.bgr = true;
    su.w = g.vw; su.h = g.vh;
    su.ox = g.vx; su.oy = g.vy;
    su.clip = clip;
    for (size_t i = 0; i < s.annos.size(); ++i) PaintAnno(su, s.annos[i]);
    if (s.drawingAnn) PaintAnno(su, s.annCur);   // 正在画的那一条也画，所见即所得
}

// ---- 框选识别：绘制 ----
// 两层：
//   ① 行带高亮：Tesseract 的词框高矮胖瘦各异、词间还有缝，逐词上色看着"大大小小
//      不统一"（用户原话）。所以先把词按行合并成**等高条带**（垂直重叠 > 较小高度
//      的一半即同行，取并集），再整条按像素刷。
//      而且不整块叠色，只替换"背景类"像素、文字像素一个字节都不动 —— 否则字会被
//      染蓝、笔画变糊。判定用条带内亮度中位数决定是"浅底深字"还是"深底浅字"。
//   ② 子框虚线：1px 蓝，按 (x+y)%8<4 逐像素写。GDI 的 PS_DOT 依赖位图刷子与 DPI，
//      跨缩放不稳；自己算的虚线在哪个 DPI 下都一模一样。
// 写完的像素 alpha 一律 255（g.bits 的不变量，半透明会透出桌面）。
static void PaintTextLayer(const RECT& band, const OverlayState& s) {
    if (!s.textHasFocus && s.words.empty()) return;
    Surf su;
    su.px = (BYTE*)g.bits;
    su.stride = g.vw * 4;
    su.bpp = 4;
    su.bgr = true;
    su.w = g.vw; su.h = g.vh;
    su.ox = 0; su.oy = 0;               // 与 band 同一坐标系（窗口坐标），不需要偏移
    su.clip = RIntersect(band, s.hole); // 洞外不画：词框跨洞边界时不该漏到压暗区
    if (REmpty(su.clip)) return;

    // ---- ① 把词框合并成行带 ----
    // 合并规则抄自旧结果窗的 CollectLineBands：垂直重叠超过较矮者一半就算同一行。
    // 为什么必须合并：Tesseract 的词框是"紧贴墨迹"的，同一行里每个词的高矮、上下
    // 都不一致，逐词上色就是用户说的「蓝色示意图大大小小，不统一」。合并成行带后
    // 同一行只剩一条等高的带。
    struct LineBand { int x0, y0, x1, y1; std::vector<int> members; };
    std::vector<LineBand> lines;
    {
        std::vector<int> order;
        for (int i = 0; i < (int)s.words.size(); ++i) {
            if (s.words[i].w > 0 && s.words[i].h > 0) order.push_back(i);
        }
        std::sort(order.begin(), order.end(),
                  [&s](int a, int b) { return s.words[a].y < s.words[b].y; });
        for (int i : order) {
            const OcrWord& w = s.words[i];
            bool merged = false;
            for (LineBand& b : lines) {
                const int ovTop = std::max(b.y0, w.y);
                const int ovBot = std::min(b.y1, w.y + w.h);
                const int minH  = std::min(b.y1 - b.y0, w.h);
                if (ovBot - ovTop > minH / 2) {   // 同一行：并入并集
                    b.x0 = std::min(b.x0, w.x);
                    b.y0 = std::min(b.y0, w.y);
                    b.x1 = std::max(b.x1, w.x + w.w);
                    b.y1 = std::max(b.y1, w.y + w.h);
                    b.members.push_back(i);
                    merged = true;
                    break;
                }
            }
            if (!merged) {
                LineBand nb{ w.x, w.y, w.x + w.w, w.y + w.h };
                nb.members.push_back(i);
                lines.push_back(nb);
            }
        }
    }

    // ---- ② 逐条带刷像素：只换背景，文字原样保留 ----
    // 底色一律用"未选中"色；被选中的词再按**它自己的 x 区间 + 行带的 y 区间**
    // 叠"选中"色。两步必须在**同一个像素循环**里按列决定，不能涂两遍：
    // 涂两遍时第二遍会拿"已被涂成浅蓝"的像素重新判亮度，暗色主题的判据
    // （lum < 96）当场不成立——选中色永远上不去（这个 bug 是 txt_band_probe
    // 的暗色用例抓出来的）。
    // 也不能做成"带里任一词选中就整条变深色"：那样点一个词整行都变深，
    // 用户反而看不出自己点的是哪个词（等高条带治的是"大大小小"，不是抹掉选中态）。
    for (const LineBand& b : lines) {
        const RECT raw{ b.x0, b.y0, b.x1, b.y1 };
        const RECT rc = RIntersect(su.clip, raw);
        if (REmpty(rc)) continue;
        // 亮度中位数定深浅：浅底深字 vs 深底浅字（暗色主题），两者的"背景"判据相反
        int hist[256] = { 0 };
        int total = 0;
        for (int y = rc.top; y < rc.bottom; ++y) {
            const BYTE* p = (const BYTE*)g.bits + ((size_t)y * g.vw + rc.left) * 4;
            for (int x = rc.left; x < rc.right; ++x, p += 4) {
                ++hist[(p[2] * 299 + p[1] * 587 + p[0] * 114) / 1000];
                ++total;
            }
        }
        int med = 0, acc = 0;
        for (int v = 0; v < 256; ++v) {
            acc += hist[v];
            if (acc * 2 >= total) { med = v; break; }
        }
        const bool dark = (med < 128);
        // 按列预置"这一列落在被选中的词里"：行带宽度通常几百像素，逐列算一次
        // 比每个像素再遍历 members 便宜，也省掉"同一行里两个词重叠"的歧义。
        const int bw = b.x1 - b.x0;
        std::vector<unsigned char> colSel((size_t)bw, 0);
        for (int i : b.members) {
            if (i >= (int)s.wordSel.size() || !s.wordSel[i]) continue;
            const OcrWord& w = s.words[i];
            const int c0 = std::max(0, w.x - b.x0);
            const int c1 = std::min(bw, w.x + w.w - b.x0);
            for (int x = c0; x < c1; ++x) colSel[(size_t)x] = 1;
        }
        for (int y = rc.top; y < rc.bottom; ++y) {
            BYTE* p = (BYTE*)g.bits + ((size_t)y * g.vw + rc.left) * 4;
            for (int x = rc.left; x < rc.right; ++x, p += 4) {
                const int lum = (p[2] * 299 + p[1] * 587 + p[0] * 114) / 1000;
                const bool isBg = dark ? (lum < 96) : (lum >= 160);
                if (!isBg) continue;             // 文字像素：一个字节都不动
                const bool sel = colSel[(size_t)(x - b.x0)] != 0;
                // 选中 RGB(170,205,245) / 未选中 RGB(214,230,250)
                p[0] = sel ? 245 : 250;          // B
                p[1] = sel ? 205 : 230;          // G
                p[2] = sel ? 170 : 214;          // R
                p[3] = 255;                      // 遮罩层不变量
            }
        }
    }

    if (s.textHasFocus && !REmpty(s.textRect)) {
        const int kBlue[3] = { 0, 120, 215 };
        const RECT& t = s.textRect;
        for (int x = t.left; x < t.right; ++x) {
            if (((x + t.top) % 8) < 4)          SPut(su, x, t.top, kBlue);
            if (((x + t.bottom - 1) % 8) < 4)   SPut(su, x, t.bottom - 1, kBlue);
        }
        for (int y = t.top; y < t.bottom; ++y) {
            if (((t.left + y) % 8) < 4)         SPut(su, t.left, y, kBlue);
            if (((t.right - 1 + y) % 8) < 4)    SPut(su, t.right - 1, y, kBlue);
        }
    }
}

// ---- 框选识别：取像素与识别 ----
// 从干净整屏像素里裁一块（绝对坐标）。origin 带回实际裁剪原点，词框要用它换算。
static bool CropScreenRect(const RECT& a, CaptureImage& out, POINT& origin) {
    if (g.screen.empty() || REmpty(a)) return false;
    const int l = std::max((int)a.left, g.vx);
    const int t = std::max((int)a.top, g.vy);
    const int r = std::min((int)a.right, g.vx + g.vw);
    const int b = std::min((int)a.bottom, g.vy + g.vh);
    if (r <= l || b <= t) return false;
    origin.x = l; origin.y = t;
    out.width = r - l;
    out.height = b - t;
    out.rgb.resize((size_t)out.width * out.height * 3);
    for (int y = 0; y < out.height; ++y) {
        const unsigned char* src = &g.screen[((size_t)(t - g.vy + y) * g.vw + (l - g.vx)) * 4];
        unsigned char* dst = &out.rgb[(size_t)y * out.width * 3];
        for (int x = 0; x < out.width; ++x, src += 4, dst += 3) {
            dst[0] = src[2];   // R
            dst[1] = src[1];   // G
            dst[2] = src[0];   // B
        }
    }
    return true;
}

// 后台识别完成的通知（发给遮罩窗口，在 UI 线程里回填词表）。
static const UINT kMsgOcrDone = WM_APP + 1;
static std::mutex        g_ocrMu;              // 保护 g_ocrStage / g.w 的交接
static std::atomic<int>   g_ocrGen{ 0 };       // 识别"代号"：只有最新一代的结果才作数
static std::vector<OcrWord> g_ocrStage;        // 后台线程写、UI 线程取

// 对 g.textRect 跑一次识别。词框转成绝对坐标（锚定图像内容，不跟选区跑）。
//
// **异步**：识别本身 0.33~0.44 秒（实测：300x150 330ms / 700x400 435ms，
// 且几乎不随面积变化，主要花在引擎每次调用的固定开销上），同步跑会把遮罩
// 冻住半秒、鼠标变等待圈，那半秒里挪选区/换颜色全都没反应。
// 所以这里只做"裁像素"（几毫秒，必须在 UI 线程读 g.screen），识别丢给后台，
// 结果回来再补画高亮。用户松手就能继续操作。
//
// 为什么不做"从第一个点开始逐点增量识别"：实测每次调用都是 330ms 起，
// 拖动中每帧调一次等于把 CPU 打满，框会卡死——越优化越慢。
// 也不做"截完图就预识别整个选区"：整选区 435ms 反而比直接识别子框 330ms 更慢，
// 且子框的词要从整图筛，边缘会被切断，准确率下降。
static void RunTextOcr() {
    if (REmpty(g.textRect)) return;
    const int w = g.textRect.right - g.textRect.left;
    const int h = g.textRect.bottom - g.textRect.top;
    if (w < 6 || h < 6) return;          // 太小：直接不跑，别让引擎去认 3x4 的噪声

    CaptureImage img;
    POINT org{ 0, 0 };
    if (!CropScreenRect(g.textRect, img, org)) return;

    g.ocrBusy = true;
    const int myGen = ++g_ocrGen;        // 新一代：在途的旧结果回来会被丢弃
    OcrImageAsync(img, [myGen, org](std::vector<OcrWord> ws) {
        for (size_t i = 0; i < ws.size(); ++i) {
            ws[i].x += org.x;
            ws[i].y += org.y;
        }
        HWND target = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_ocrMu);
            if (myGen != g_ocrGen.load()) return;   // 旧一代结果，丢弃
            g_ocrStage = std::move(ws);
            // 句柄在锁内取：窗口销毁路径摘句柄也在同一把锁内，所以这里拿到的
            // 要么是尚未销毁的窗口，要么是 nullptr。
            target = g.w;
        }
        // PostMessage 放在锁外：将来一旦有人把它改成 SendMessage，持锁等 UI
        // 线程回调就是死锁。
        if (target) PostMessageW(target, kMsgOcrDone, 0, 0);
    });
}

// 保存时要写进剪贴板的文字：选中了就只取选中的，没选就取全部（按阅读顺序）。
// 换行模式用 Smart：g_lineMode 在 view.cpp 的匿名命名空间里取不到，而 Smart 就是
// 产品默认值；这里再开一个取模式的接口不值当。
static std::string PickedTextUtf8() {
    if (g.words.empty()) return std::string();
    std::vector<OcrWord> pick;
    bool any = false;
    for (size_t i = 0; i < g.words.size(); ++i) {
        if (i < g.wordSel.size() && g.wordSel[i]) { pick.push_back(g.words[i]); any = true; }
    }
    if (!any) pick = g.words;
    return FormatSelection(pick, NewlineMode::Smart);
}

// Shift = 正圆 / 正方形（只对矩形、椭圆；箭头与画笔收不成"正方"，也不该收）。
// 边长取两个方向里较大的绝对值；dx/dy 为 0 时 sign 按 +1（与"往右下拖"一致）。
static POINT ConstrainShapeEnd(POINT anchor, POINT cur, bool square) {
    if (!square) return cur;
    const int dx = cur.x - anchor.x;
    const int dy = cur.y - anchor.y;
    const int s = std::max(std::abs(dx), std::abs(dy));
    return POINT{ anchor.x + (dx < 0 ? -s : s), anchor.y + (dy < 0 ? -s : s) };
}

// 标注终点：先钳进选区（墨迹只在洞里可见），再按 Shift 收成正圆/正方。
// 收方之后边长可能把点顶出选区，而"钳回来"会破坏正方形 —— 所以把整条边按
// 超出的最大值等量缩回去，保证 s 在两个轴上始终相等。
static POINT AnnEndPoint(POINT anchor, POINT p, bool square) {
    POINT q = p;
    if (q.x < g.sel.left)       q.x = g.sel.left;
    if (q.x > g.sel.right - 1)  q.x = g.sel.right - 1;
    if (q.y < g.sel.top)        q.y = g.sel.top;
    if (q.y > g.sel.bottom - 1) q.y = g.sel.bottom - 1;
    if (!square) return q;
    POINT e = ConstrainShapeEnd(anchor, q, square);
    int over = 0;
    if (e.x < g.sel.left)       over = std::max(over, (int)(g.sel.left - e.x));
    if (e.x > g.sel.right - 1)  over = std::max(over, (int)(e.x - (g.sel.right - 1)));
    if (e.y < g.sel.top)        over = std::max(over, (int)(g.sel.top - e.y));
    if (e.y > g.sel.bottom - 1) over = std::max(over, (int)(e.y - (g.sel.bottom - 1)));
    if (over > 0) {
        e.x += (e.x < anchor.x) ? over : -over;
        e.y += (e.y < anchor.y) ? over : -over;
    }
    return e;
}

// 词选中：拖选矩形里"词中心"落进去的算选中。用中心而不是相交，避免擦到边就选中。
static void ApplyWordSel() {
    RECT r = Norm(g.wordDragStart, g.wordDragCur);
    g.wordSel.assign(g.words.size(), 0);
    for (size_t i = 0; i < g.words.size(); ++i) {
        const OcrWord& w = g.words[i];
        const int cx = w.x + w.w / 2;
        const int cy = w.y + w.h / 2;
        if (cx >= r.left && cx < r.right && cy >= r.top && cy < r.bottom) g.wordSel[i] = 1;
    }
}

// 洞的「绘制足迹」。这里用**一个**矩形表达（而不是 4 条 ring 带）：StateRegion 恒为
// 「1 个洞矩形 + 1 个标签矩形」，脏区是前后两个状态的 XOR，footprint 的 rect 数越少，
// XOR 出来的 rect 就越少，越不会因为超过 kBandCap 而走溢出合并。
//
// 调整态必须把八个把手整个罩住，因为它们是跨洞边界画的。
// 这里曾经写成"洞统一外扩 kHandleH=8"，前提是"把手最远只伸到 ±4"。那个前提是错的：
// 边中点把手沿边方向长 kEdgeHandleLen=24，选区一旦比 24 窄，它就会**横向甩出洞外**
// 超过 8px。实测（选区 4px 宽）：上中把手是 (329,48)-(353,56)，而按 8px 外扩得到的
// 足迹右边界只有 351 —— 那 2 列白块/蓝边永远进不了脏区，拖动时留在屏幕上。
// 所以按**真实把手矩形**求并，不再拍常数。仍是 1 个矩形，脏区复杂度不变。
static RECT HoleFootprint(const OverlayState& s) {
    if (!s.adjust) return s.hole;
    RECT hs[8];
    BuildHandleRects(s.hole, hs);
    RECT r = s.hole;
    for (int i = 0; i < 8; ++i) r = RUnion(r, hs[i]);
    return RIntersect(r, RECT{ 0, 0, g.vw, g.vh });
}

static HRGN StateRegion(const OverlayState& s) {
    HRGN r = CreateRectRgn(0, 0, 0, 0);
    RECT fp = HoleFootprint(s);
    if (!REmpty(fp)) {
        HRGN h = CreateRectRgnIndirect(&fp);
        CombineRgn(r, r, h, RGN_OR);
        DeleteObject(h);
    }
    if (s.labelValid && !REmpty(s.label)) {
        HRGN l = CreateRectRgnIndirect(&s.label);
        CombineRgn(r, r, l, RGN_OR);
        DeleteObject(l);
    }
    // 标注（s.annBox）故意不进这个区域：墨迹是按构造钳在洞内画的，annBox 是
    // RIntersect(墨迹框, hole) 的子集，而洞已经在里面了 —— 加进来一次元素都不多。
    // 标注的**内容**变化走 BuildDirtyBands 的 extra 参数（区域异或看不见它）。
    return r;
}

// 每帧脏区条数上限 = 8。
//
// ⚠️ 这里曾经定过 4，依据是"cap=4 → P50 0.35/P95 1.67；cap=6 → 0.78/3.05"，
// 即 4 与 6 之间存在阶跃悬崖。**该结论已被撤回**：那组数是各配置连跑 3 次取的，
// 批次间有系统漂移；改成交错 A/B/A/B 各 6 次重测后 —— cap4 中位 0.685/P95 2.88，
// cap6 0.770/2.86，cap8 0.665/2.81，三者 P95 全落在 2.81~2.88ms，**没有阶跃，
// cap8 的中位反而最快**。教训：这台机器上同一配置跨天重测 P50 能从 0.35 漂到
// 0.685（2 倍），任何非交错的单批次对比都不可信。
static const int kBandCap = 8;

// 成本模型（1080p 交错重测拟合，ms）：T ≈ 0.015·n + 3.0e-6·Σ面积
//   · 面积项 2.5~3.5 ns/px，取 3.0e-6
//   · 每次 UpdateLayeredWindowIndirect 调用固定开销 13~17µs，取 0.015ms
//     （早先估的 0.04ms/call 偏大 2~3 倍，会把"多切几条"判得过于昂贵）
// 选型结论不受这次撤回影响：L 形差集不合并成外接矩形、两个大矩形该合并。
static double BandCost(int n, long long area) {
    return 0.015 * n + 3.0e-6 * (double)area;
}

static int BuildDirtyBands(const OverlayState& a, const OverlayState& b,
                           const std::vector<RECT>& extra, RECT out[], int cap) {
    HRGN ra = StateRegion(a);
    HRGN rb = StateRegion(b);
    HRGN d = CreateRectRgn(0, 0, 0, 0);
    CombineRgn(d, ra, rb, RGN_XOR);
    // 边框颜色变了（悬停↔拖拽）或把手整体出现/消失（进出调整态）时，区域异或
    // 可能为空但必须重画。这里要并的是**足迹**而不是裸 hole：把手有一半在洞外。
    if ((a.drag != b.drag || a.adjust != b.adjust) && !REmpty(a.hole)) {
        RECT fp = HoleFootprint(a);
        if (!REmpty(fp)) {
            HRGN h = CreateRectRgnIndirect(&fp);
            CombineRgn(d, d, h, RGN_OR);
            DeleteObject(h);
        }
    }

    // 先把所有矩形收进 vector 再合并。旧实现直接往 out[] 里写、超限部分塞进
    // out[n-1]：n==0 时那是 out[-1] 越界写（UB）。虽然 GetRegionData 契约保证矩形
    // 非退化、当前走不到，但这是零成本的加固，且下面还要强行把数量压到上限，
    // 溢出合并会变成常态路径，不能再放在"靠契约不可达"上。
    std::vector<RECT> v;
    DWORD sz = GetRegionData(d, 0, nullptr);
    if (sz > 0) {
        std::vector<BYTE> buf(sz);
        RGNDATA* rd = reinterpret_cast<RGNDATA*>(buf.data());
        if (GetRegionData(d, sz, rd)) {
            const RECT* rr = reinterpret_cast<const RECT*>(rd->Buffer);
            const int cnt = (int)rd->rdh.nCount;
            for (int i = 0; i < cnt; ++i) {
                // 两分支，不能只写 continue：n==0 时 continue 会让后续迭代接着写
                // 越界位置。这里 push 语义天然安全，但仍保留退化矩形的显式跳过。
                if (!REmpty(rr[i])) v.push_back(rr[i]);
            }
        }
    }
    DeleteObject(ra); DeleteObject(rb); DeleteObject(d);

    // 标注变更必须显式并进来：区域异或只看得见"区域不同"，看不见"同一个区域里的
    // 像素不同"。矩形从小拖到大时两边外接框的并集没变、XOR 一个矩形都选不出来，
    // 新增的那几条边就画不上去；反过来从大拖小，旧外框的像素全落在"新旧框的交集"
    // 里，XOR 也是空 → 屏幕上的旧框永远留着。所以调用方把 RUnion(旧框, 新框) 传进来。
    // 代价是这一帧按外接框面积而不是按"线"来重画，一个占满选区的大标注拖动时约
    // 2~5ms/帧 —— 只在拖标注的那几十帧里出现，可以接受。
    for (const RECT& x : extra) if (!REmpty(x)) v.push_back(x);

    if (v.empty()) return 0;

    // 贪心合并：先按"总成本是否下降"自愿合并，数量仍超上限就强行压到 cap。
    // 比较的是**整帧总成本**（BandCost(n, Σ面积)），不是两条合并前后的边际差——
    // 后者只算这两条自己的面积增量，看不见"多留一条就多付一次整帧调用开销"。
    // （这里原写过"看不出 4→5 条那道阶跃悬崖"；那道悬崖已被撤回，见上。）
    int n = (int)v.size();
    for (;;) {
        if (n < 2) break;
        long long total = 0;
        for (int i = 0; i < n; ++i) total += RArea(v[i]);
        double bestGain = -1e18;
        int bi = -1, bj = -1;
        for (int i = 0; i < n; ++i) {
            for (int j = i + 1; j < n; ++j) {
                const long long merged = RArea(RUnion(v[i], v[j]));
                const double before = BandCost(n,     total);
                const double after  = BandCost(n - 1, total - RArea(v[i]) - RArea(v[j]) + merged);
                const double gain = before - after;
                if (gain > bestGain) { bestGain = gain; bi = i; bj = j; }
            }
        }
        if (bi < 0) break;
        if (n <= cap && bestGain <= 0.0) break;   // 数量已达标且不再划算 → 停
        v[bi] = RUnion(v[bi], v[bj]);
        v[bj] = v[--n];                            // swap-pop：末元素补到 bj 位
    }
    for (int i = 0; i < n; ++i) out[i] = v[i];
    return n;
}

static void PushBand(const RECT& band, bool full) {
    POINT ppt = { g.vx, g.vy };
    SIZE szw = { g.vw, g.vh };
    POINT src = { 0, 0 };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    static BOOL(WINAPI* pULWI)(HWND, const UPDATELAYEREDWINDOWINFO*) = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        pULWI = reinterpret_cast<decltype(pULWI)>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "UpdateLayeredWindowIndirect"));
    }
    if (full || !pULWI) {
        UpdateLayeredWindow(g.w, nullptr, &ppt, &szw, g.hDc, &src, 0, &bf, ULW_ALPHA);
        return;
    }
    UPDATELAYEREDWINDOWINFO info;
    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);
    info.pptDst = &ppt;
    info.psize = &szw;
    info.hdcSrc = g.hDc;
    info.pptSrc = &src;
    info.pblend = &bf;
    info.dwFlags = ULW_ALPHA;
    info.prcDirty = &band;
    pULWI(g.w, &info);
}

// 「必须重画」的兜底矩形集合：区域异或只看得见"区域不同"，看不见"同一区域里
// 像素不同"。三类漏点共用这一味药，三类都是实测拿到的，不是假想：
//
//   ① 标注：矩形从大拖小，旧外框的像素全落在"新旧外接框的交集"里，异或选不出来
//      → 屏幕上的旧框永远留着。
//   ② 标签：尺寸数字变了但字宽不变（"120 x 80" 之后全是 3 位数）时标签矩形一字
//      不差 → 异或为空 → 整帧被判成"没东西可画"，标签停在第一次变宽的数字上。
//      实测：拖出 600x400 的框，标签显示 "120 x 80"。
//   ③ 洞/边框/把手：**边框是画在洞内部的**。洞一移动，新边框就落进旧洞里（区域
//      交集），旧的边框又在旧洞边缘；调整态下八个把手还整个跟着走。区域异或只能
//      选中"一侧有一侧没有"的像素，交集里那些"内容变了但归属没变"的像素永远进不了
//      脏区。实测（overlay_state_test 第 3 节，1500 例）：只补 ① ② 时仍漏 3435 像素。
//
// 补法必须按类分开，不能用一个外接框糊住：整洞重画在 1080p 约 6ms/帧，而按"边框环"
// 补只有周长×2 的面积，成本可忽略。这也是为什么返回的是**一组**矩形而不是一个。
//
// 正确性依据：painter 是状态的纯函数，像素差异只可能落在「旧状态画过的像素 ∪
// 新状态画过的像素」之内；各类的足迹分别是 annBox / label / 洞(含边框、含把手)。
static void PushRing(std::vector<RECT>& out, const RECT& h) {
    const int w = 2;                       // 与 PaintBorder 的线宽一致
    if (REmpty(h)) return;
    out.push_back(RECT{ h.left, h.top, h.right, h.top + w });
    out.push_back(RECT{ h.left, h.bottom - w, h.right, h.bottom });
    out.push_back(RECT{ h.left, h.top, h.left + w, h.bottom });
    out.push_back(RECT{ h.right - w, h.top, h.right, h.bottom });
}

static std::vector<RECT> DirtyExtra(const OverlayState& a, const OverlayState& b) {
    std::vector<RECT> v;
    // ① 标注：并集一个矩形就够（墨迹 ⊆ annBox ⊆ 洞）
    v.push_back(RUnion(a.annBox, b.annBox));
    // ② 标签：只有"矩形没动但文字变了"才需要（矩形动了异或自己会选中）
    if (b.labelValid && std::strncmp(a.text, b.text, sizeof(b.text)) != 0)
        v.push_back(b.label);
    // ③ 洞几何变化
    const bool holeMoved = a.hole.left != b.hole.left || a.hole.top != b.hole.top ||
                           a.hole.right != b.hole.right || a.hole.bottom != b.hole.bottom;
    if (holeMoved) {
        // 调整态：足迹整体补（洞 + 边框 + 八个把手），一个矩形罩住，此时不必再补环
        if (a.adjust) v.push_back(HoleFootprint(a));
        if (b.adjust) v.push_back(HoleFootprint(b));
        // 非调整态：只补边框环。洞内部像素只依赖绝对坐标，交集里内容相同，不用重画。
        if (!a.adjust) PushRing(v, a.hole);
        if (!b.adjust) PushRing(v, b.hole);
    }
    // ④ 框选识别层：子框虚线与词高亮都画在洞内，"换了哪个词被选中/重新识别"时
    //    区域一个都不变 → 异或选不中。把旧子框 ∪ 新子框 ∪ 两侧所有词框并成一个矩形。
    //    只在**这一层真的变了**时才 push（否则每帧都白扩一大块）。
    if (!SameTextLayer(a, b)) {
        RECT box = RUnion(a.textRect, b.textRect);
        for (size_t i = 0; i < a.words.size(); ++i) {
            const OcrWord& w = a.words[i];
            box = RUnion(box, RECT{ w.x, w.y, w.x + w.w, w.y + w.h });
        }
        for (size_t i = 0; i < b.words.size(); ++i) {
            const OcrWord& w = b.words[i];
            box = RUnion(box, RECT{ w.x, w.y, w.x + w.w, w.y + w.h });
        }
        v.push_back(box);
    }
    std::vector<RECT> out;
    out.reserve(v.size());
    for (const RECT& r : v) if (!REmpty(r)) out.push_back(r);
    return out;
}

static void Redraw() {
    if (g.dim.empty() || !g.w || !g.bits) return;

    OverlayState next = BuildOverlayState();
    const bool firstFrame = !g_prevValid;
    // 状态没变 → 一个字都不画（同窗口内拖动/悬停的全部帧都走这里）
    if (!firstFrame && !g_renderFull && SameOverlay(g_prev, next)) {
        return;
    }

    RECT bands[kBandCap];
    int n = 0;
    if (firstFrame || g_renderFull) {
        bands[0] = RECT{ 0, 0, g.vw, g.vh };
        n = 1;
    } else {
        // 标注/标签/边框/把手的"内容变了但区域没变"必须靠 extra 兜底，理由见 DirtyExtra。
        std::vector<RECT> extra = DirtyExtra(g_prev, next);
        n = BuildDirtyBands(g_prev, next, extra, bands, kBandCap);
        if (n <= 0) { g_prev = std::move(next); return; }
    }

    for (int i = 0; i < n; ++i) {
        RestoreFromDim(bands[i]);
        PaintHole(bands[i], next);
        // 文字层紧跟 PaintHole：词块是"与干净桌面混合"，必须混在干净像素上，
        // 否则混到的是上一帧的标注/边框颜色，越混越黑。
        PaintTextLayer(bands[i], next);
        PaintAnnos(bands[i], next);     // 必须在 PaintHole 之后：否则被干净像素整块盖掉
        PaintBorder(bands[i], next);
        PaintHandles(bands[i], next);   // 必须在 PaintHole 之后：把手有一半在洞内
        PaintLabel(bands[i], next);
        PushBand(bands[i], firstFrame || g_renderFull);
    }
    g_prev = std::move(next);
    g_prevValid = true;
}

// ---- 调整态的工具栏（保存 / 关闭）----
// 两条硬约束决定了它只能是**独立顶层 popup**，绝不能是遮罩的子窗、也不能画进 g.bits：
//   ① 遮罩是 WS_EX_LAYERED 的 WS_POPUP，分层窗口不能有子窗口（子窗根本不会被绘制）；
//   ② 画进 g.bits 会同时撞上"GDI 画字把字形像素 alpha 写成 0"（要整块补写回 255）
//      和"多一条 band 吃掉 kBandCap 预算"两个坑。
// 所以：独立 WS_POPUP + WS_EX_TOPMOST 压在遮罩之上；点击不抢焦点
// （WM_MOUSEACTIVATE 返回 MA_NOACTIVATE），否则遮罩收不到 ESC。
// 单行、纯图标工具栏。水平布局（定案）：
//
//   pad=10 ┌矩形 椭圆 箭头 画笔 文┐ 4 ┌下载┐ 14 │ 颜色×6 │ 14 粗细×4
//          10   52   94  136 178     220       272       477
//
// 按钮 38x38（pitch 42，间隔 4）；颜色/粗细圆 pitch 31，圆点直径 22。
// 「下载」两侧的间距必须和其他图标一致：文按钮右边 216、下载左边 220（间隔 4）。
// 之前是 230（间隔 14），用户原话"下载的图标怎么和左右相离这么远"。
// 宽度 620 = 最后一档粗细圆心 596 + 选中垫底方块半宽 14 = 610，右边再留 10（与左对称）。
constexpr int kTbW    = 620;
constexpr int kTbH    = 48;    // 40 * 1.2
constexpr int kTbPad  = 10;
constexpr int kTbBtn  = 38;    // 按钮边长
constexpr int kTbStep = 42;    // 按钮 pitch（间隔 4）
constexpr int kTbSaveX = 220;  // 216 + 4：和工具按钮间距一致
constexpr int kTbSep1  = 272;  // 258 + 14
constexpr int kTbSep2  = 477;
constexpr int kTbSwX0  = 297;  // 圆心 297..452，最右 452+11=463
constexpr int kTbSwStep = 31;
constexpr int kTbSwR   = 11;   // 直径 22
constexpr int kTbWdX0  = 503;  // 圆心 503..596
constexpr int kTbWdStep = 31;
constexpr int kTbHit   = 26;   // 颜色/粗细点击区边长
constexpr int kTbBtnY  = (kTbH - kTbBtn) / 2;   // 5：按钮垂直居中
constexpr int kTbMidY  = kTbH / 2;              // 24：颜色/粗细圆心 y
// id：1..5 工具（矩形/椭圆/箭头/画笔/文）、6 保存。
// 7/8 是"识别文字/关闭"的历史编号，现在工具栏上没有按钮了（右键=取消，
// 识别由「文」工具包办），但**保留枚举**：截图尾部的分流逻辑与 ocraddon.cpp
// 还在引用名字，删了只会让别处变形。
enum {
    kTbRect = 1, kTbEllipse, kTbArrow, kTbPen, kTbText,   // 1..5
    kTbSave,                                              // 6 = 下载图标
    kTbOcr,                                               // 7 = 无按钮（保留）
    kTbClose,                                             // 8 = 无按钮（保留）
    kTbColor0 = 20, kTbColor5 = 25,
    kTbWidth0 = 30, kTbWidth3 = 33,
};
static HWND g_tb = nullptr;
static int  g_tbHot = 0;    // 悬停的按钮
static int  g_tbPress = 0;  // 按下的按钮（0 = 空白处/没有）
// ---- 长按拖动（用户："长按工具框是可以拖动的"）----
// 判定链：按下 → 起 300ms 定时器 → 到点还按着就转拖动；提前松开且几乎没动 = 点击。
// 拖动只 SetWindowPos，绝不动 g.bits、也不调 Redraw()：工具栏是独立窗口，
// 重画遮罩只会让整块选区白闪一帧。
static bool  g_tbPressDown = false;   // 已按下、尚未松开
static bool  g_tbArmed     = false;   // 已进入"拖动"（长按到点 或 位移超过阈值）
static bool  g_tbDrag      = false;   // 正在拖动中
static bool  g_tbUserMoved = false;   // 用户手动拖过 → TbReposition() 不再抢回选区下方
static POINT g_tbDownPt{ 0, 0 };      // 按下点（工具栏客户区坐标）
static POINT g_tbDragPt{ 0, 0 };      // 拖动锚点（屏幕坐标）
static POINT g_tbDragPos{ 0, 0 };     // 拖动开始时窗口左上角
static const UINT_PTR kTbDragTimer  = 2;
static const int      kTbDragDelayMs = 300;   // 长按 0.3 秒转拖动
static const int      kTbDragSlop    = 4;     // 位移 <= 4px 仍算点击
static LRESULT CALLBACK TbProc(HWND h, UINT m, WPARAM w, LPARAM l);  // TbCreate 需要

static RECT TbButtonRect(int id) {
    if (id >= kTbRect && id <= kTbText) {
        const int x = kTbPad + (id - kTbRect) * kTbStep;
        return { x, kTbBtnY, x + kTbBtn, kTbBtnY + kTbBtn };
    }
    if (id == kTbSave) return { kTbSaveX, kTbBtnY, kTbSaveX + kTbBtn, kTbBtnY + kTbBtn };
    if (id >= kTbColor0 && id <= kTbColor5) {
        const int cx = kTbSwX0 + (id - kTbColor0) * kTbSwStep;
        return { cx - kTbHit / 2, kTbMidY - kTbHit / 2, cx + kTbHit / 2, kTbMidY + kTbHit / 2 };
    }
    if (id >= kTbWidth0 && id <= kTbWidth3) {
        const int cx = kTbWdX0 + (id - kTbWidth0) * kTbWdStep;
        return { cx - kTbHit / 2, kTbMidY - kTbHit / 2, cx + kTbHit / 2, kTbMidY + kTbHit / 2 };
    }
    return { 0, 0, 0, 0 };
}
static int TbHitTest(POINT pt) {
    // 不能对 TbButtonRect 的临时返回值取地址（C2102），先落到局部变量
    for (int id = kTbRect; id <= kTbSave; ++id) {
        RECT r = TbButtonRect(id);
        if (PtInRect(&r, pt)) return id;
    }
    for (int id = kTbColor0; id <= kTbColor5; ++id) {
        RECT r = TbButtonRect(id);
        if (PtInRect(&r, pt)) return id;
    }
    for (int id = kTbWidth0; id <= kTbWidth3; ++id) {
        RECT r = TbButtonRect(id);
        if (PtInRect(&r, pt)) return id;
    }
    return 0;
}

// 收尾：销毁工具栏，置退出标志。裁剪与剪贴板统一在 CaptureRegionInteractive
// 尾部做（只裁一次，这里不重复）。
//   action = 0 取消 / kTbSave 只保存图片 / kTbOcr 识别文字（先图后字）
// 只有这两个 id 才是"拿到结果"，其余（工具/颜色/粗细/关闭）都不是。
static void FinishAdjust(int action) {
    if (g_tb) { DestroyWindow(g_tb); g_tb = nullptr; }
    // 只有"保存"和"识别文字"才算拿到结果；点"关闭"等于取消
    g.captured   = (action == kTbSave || action == kTbOcr);
    g.saveToClip = (action == kTbSave);
    g.ocrWanted  = (action == kTbOcr);
    g.done = true;
    // 摘句柄与后台回调取句柄共用一把锁：置空之后回调不会再往野句柄投消息。
    HWND mask = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_ocrMu);
        mask = g.w;
        g.w = nullptr;
    }
    if (mask) DestroyWindow(mask);
}

// 工具栏命令分发：工具/颜色/粗细只改状态，只有 kTbSave 收尾。
// 这里刻意**不调 Redraw()**：这些键只影响工具栏自身和"后续绘制用什么参数"，
// 调 Redraw 会让整块选区重画一遍（白闪一帧），纯浪费。
// ---- 「下载」：另存为 + 上次目录记忆 ----
// 用户："下载之后下载到哪去了？怎么没有弹保存地址啊，我选择保存的地方啊。而且要有记忆，
// 默认选择上一次保存的地方。" —— 之前点下载只把图塞进剪贴板，从来不弹框。
// 目录记在 shot_ui.ini 的 [save] lastdir，和颜色/粗细同一份文件（不碰 settings.ini）。
static std::wstring g_savePath;       // 用户确认后的目标路径
static bool         g_saveToFile = false;  // 这次截图要写文件（尾部按它分流）

static std::wstring LoadLastSaveDir() {
    const std::wstring p = ShotUiIni();
    if (p.empty()) return std::wstring();
    wchar_t buf[MAX_PATH] = {};
    GetPrivateProfileStringW(L"save", L"lastdir", L"", buf, MAX_PATH, p.c_str());
    if (!buf[0]) return std::wstring();
    // 目录可能已经被用户删了：不存在的就别当默认目录，回落桌面
    const DWORD at = GetFileAttributesW(buf);
    if (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY)) return std::wstring();
    return std::wstring(buf);
}

static void StoreLastSaveDir(const std::wstring& dir) {
    const std::wstring p = ShotUiIni();
    if (p.empty() || dir.empty()) return;
    WritePrivateProfileStringW(L"save", L"lastdir", dir.c_str(), p.c_str());
}

// 按 MIME 找 GDI+ 编码器（与 view.cpp 的 TbEncoderClsid 同一套写法，两个 TU 各一份）
static int TbEncoderClsid(const wchar_t* mime, CLSID* clsid) {
    UINT num = 0, bytes = 0;
    if (gp::GetImageEncodersSize(&num, &bytes) != gp::Ok || bytes == 0) return -1;
    std::vector<BYTE> buf(bytes);
    auto* info = reinterpret_cast<gp::ImageCodecInfo*>(buf.data());
    if (gp::GetImageEncoders(num, bytes, info) != gp::Ok) return -1;
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(info[i].MimeType, mime) == 0) { *clsid = info[i].Clsid; return (int)i; }
    }
    return -1;
}

// 把已经合成好标注的成品图（img.rgb：24bpp、RGB 顺序、行优先、**无行填充**）写盘。
// 注意按行 memcpy：gp::BitmapData.Stride 可能带 4 字节对齐，整块拷会错行。
static bool SaveImageToFile(const CaptureImage& img, const std::wstring& path) {
    const int W = img.width, H = img.height;
    if (W <= 0 || H <= 0 || img.rgb.size() < (size_t)W * H * 3) return false;
    std::wstring ext;
    const size_t dot = path.find_last_of(L'.');
    if (dot != std::wstring::npos) ext = path.substr(dot + 1);
    for (auto& c : ext) c = (wchar_t)towlower(c);
    const bool jpg = (ext == L"jpg" || ext == L"jpeg");
    CLSID clsid{};
    if (TbEncoderClsid(jpg ? L"image/jpeg" : L"image/png", &clsid) < 0) {
        MessageBoxW(g.w, L"系统缺少对应的图片编码器，无法保存。", L"下载", MB_ICONWARNING);
        return false;
    }
    gp::Bitmap bmp(W, H, PixelFormat24bppRGB);   // 宏，不能加 gp::（C2589）
    if (bmp.GetLastStatus() != gp::Ok) return false;
    {
        gp::Rect rc(0, 0, W, H);
        gp::BitmapData bd{};
        if (bmp.LockBits(&rc, gp::ImageLockModeWrite, PixelFormat24bppRGB, &bd) != gp::Ok)
            return false;
        for (int y = 0; y < H; ++y)
            memcpy((BYTE*)bd.Scan0 + (size_t)y * bd.Stride,
                   &img.rgb[(size_t)y * W * 3], (size_t)W * 3);
        bmp.UnlockBits(&bd);
    }
    if (bmp.Save(path.c_str(), &clsid, nullptr) != gp::Ok) {
        MessageBoxW(g.w, L"保存失败（路径只读、磁盘空间不足或被占用）。", L"下载", MB_ICONWARNING);
        return false;
    }
    return true;
}

// 弹「另存为」。返回 false = 用户取消；调用方必须**原地停着**，不能当成取消截图。
static bool AskSaveImagePath(std::wstring& out) {
    std::wstring dir = LoadLastSaveDir();
    if (dir.empty()) {
        wchar_t desk[MAX_PATH] = {};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desk)) || !desk[0])
            GetCurrentDirectoryW(MAX_PATH, desk);
        dir = desk;
    }
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t name[64] = {};
    swprintf(name, 64, L"截图_%04d%02d%02d_%02d%02d%02d.png",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    // 只把**文件名**放进 lpstrFile，目录交给 lpstrInitialDir：
    // 两者都塞全路径时，Vista 之后的 IFileSaveDialog 会以 shell 自己的 MRU 为准
    // （实测：ini 指向 A、上次存过桌面，对话框还是开在桌面）；拆开之后才对。
    std::vector<wchar_t> buf(MAX_PATH, 0);
    const size_t cp = wcslen(name) < (size_t)MAX_PATH - 1 ? wcslen(name) : (size_t)MAX_PATH - 1;
    memcpy(buf.data(), name, cp * sizeof(wchar_t));

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.w;      // 挂在遮罩上：模态对话框一定压在遮罩和工具栏之上
    ofn.lpstrFilter = L"PNG 图片 (*.png)\0*.png\0JPEG 图片 (*.jpg)\0*.jpg\0所有文件 (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"png";
    // **必须**显式给 lpstrInitialDir：只把目录拼进 lpstrFile 不够 —— Vista 之后的
    // IFileSaveDialog 会用 shell 自己的"上次访问文件夹"MRU 覆盖初始目录。实测：
    // ini 里 lastdir 指向 A 目录、上一次保存去过桌面，则对话框仍开在桌面。
    ofn.lpstrInitialDir = dir.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return false;
    out = buf.data();
    return !out.empty();
}

static void TbCommand(int id) {
    if (id >= kTbRect && id <= kTbText) {
        const int t = AT_RECT + (id - kTbRect);
        g.annTool = (g.annTool == t) ? AT_NONE : t;   // 再点一次同一个工具 = 退出标注模式
        return;
    }
    if (id >= kTbColor0 && id <= kTbColor5) {
        g.annColor = id - kTbColor0;
        SaveAnnoPrefs();                             // 立刻落盘，"记忆"要在重启后生效
        return;
    }
    if (id >= kTbWidth0 && id <= kTbWidth3) {
        g.annWidthIdx = id - kTbWidth0;
        SaveAnnoPrefs();
        return;
    }
    // 6 = 下载：先弹「另存为」，用户确认了才收尾；取消就停在调整态（遮罩、工具栏都还在）
    if (id == kTbSave) {
        std::wstring path;
        if (!AskSaveImagePath(path)) return;
        const size_t slash = path.find_last_of(L'\\');
        if (slash != std::wstring::npos && slash > 0) StoreLastSaveDir(path.substr(0, slash));
        g_savePath = path;
        g_saveToFile = true;
        FinishAdjust(kTbSave);
        return;
    }
    FinishAdjust(id);                                // 其余（关闭等）按老路收尾
}

// 把工具栏钳进当前显示器的工作区（拖动用，边距 8）
static void TbClampToWork() {
    if (!g_tb) return;
    RECT wr; GetWindowRect(g_tb, &wr);
    HMONITOR hm = MonitorFromWindow(g_tb, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    RECT wa;
    if (GetMonitorInfo(hm, &mi)) wa = mi.rcWork;
    else wa = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    const int w = (int)(wr.right - wr.left), h = (int)(wr.bottom - wr.top);
    const int x = std::max((int)wa.left + 8, std::min((int)wr.left,  (int)wa.right  - 8 - w));
    const int y = std::max((int)wa.top  + 8, std::min((int)wr.top,   (int)wa.bottom - 8 - h));
    if (x != wr.left || y != wr.top)
        SetWindowPos(g_tb, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// 以"按下点"为锚进入拖动：锚点用屏幕坐标记一次，之后只比绝对位移，不会累积漂移。
static void TbBeginDrag() {
    if (!g_tb || g_tbDrag) return;
    RECT wr; GetWindowRect(g_tb, &wr);
    g_tbDragPos = { wr.left, wr.top };
    g_tbDragPt  = { wr.left + g_tbDownPt.x, wr.top + g_tbDownPt.y };
    g_tbArmed = true;
    g_tbDrag  = true;
    SetCursor(LoadCursor(NULL, IDC_SIZEALL));
}

// 工具栏跟随选区：优先贴在选区正下方居中，下方放不下改上方，再钳回工作区。
// 用户手动拖过之后就不再跟随（否则"拖到别处、一动选区又跳回来"，等于白拖）。
static void TbReposition() {
    if (!g_tb) return;
    if (g_tbUserMoved) return;
    RECT s = g.sel;
    HMONITOR hm = MonitorFromRect(&s, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    RECT wa;
    if (GetMonitorInfo(hm, &mi)) wa = mi.rcWork;
    else wa = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    int x = (s.left + s.right) / 2 - kTbW / 2;
    int y = s.bottom + 8;
    if (y + kTbH > wa.bottom - 8) y = s.top - 8 - kTbH;
    if (y < wa.top + 8) y = wa.top + 8;
    // RECT 成员是 LONG、x 是 int，混用会让 std::min/max 模板推导失败（LONG≠int）
    x = std::max((int)wa.left + 8, std::min(x, (int)wa.right - 8 - kTbW));
    SetWindowPos(g_tb, NULL, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void TbCreate() {
    if (g_tb) { DestroyWindow(g_tb); g_tb = nullptr; }
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = TbProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
        wc.lpszClassName = L"OCRCapTb";
        RegisterClassW(&wc);
        reg = true;
    }
    g_tb = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"OCRCapTb", L"",
                            WS_POPUP, 0, 0, kTbW, kTbH,
                            NULL, NULL, GetModuleHandle(NULL), NULL);
    if (!g_tb) return;
    // 窗口自身也裁成圆角（半径 20 = 条底圆角 10 的两倍）：否则条底圆角之外那 4 个
    // 小三角是窗口底色（白），在深色桌面上会露出直角。view.cpp 的工具栏同款做法。
    SetWindowRgn(g_tb, CreateRoundRectRgn(0, 0, kTbW + 1, kTbH + 1, 20, 20), FALSE);
    g_tbHot = 0;
    g_tbPress = 0;
    // 新一轮截图：一切拖动状态归零，工具栏重新跟随选区
    g_tbPressDown = false;
    g_tbArmed = false;
    g_tbDrag = false;
    g_tbUserMoved = false;
    TbReposition();
    ShowWindow(g_tb, SW_SHOWNOACTIVATE);
}

// 圆角矩形路径（条底 / 选中底 / 矩形图标共用）。写法与 view.cpp 的 TbRoundPath 一致。
static void TbRoundPath(gp::GraphicsPath& p, const gp::RectF& r, float rad) {
    const float d = rad * 2;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}

// 图标全部用 GDI+ 画（抗锯齿）：不引图片资源（资源一多，单文件部署就要跟着改）。
// 线形图标统一 3.2f 圆头圆角；坐标以按钮中心 (cx, cy) 为基准、在 38x38 的框里设计
// （用户要求"图标也变高"：旧的是 18x18 里的 22x16 扁框）。
// 注意：这里只画工具栏自己的窗口 DC，绝不碰 g.bits —— 遮罩层要 alpha 恒 255。
static void TbDrawIcon(gp::Graphics& gr, int id, float cx, float cy,
                       const gp::Color& ink) {
    gp::Pen pen(ink, 3.2f);
    pen.SetStartCap(gp::LineCapRound);
    pen.SetEndCap(gp::LineCapRound);
    pen.SetLineJoin(gp::LineJoinRound);
    switch (id) {
        case kTbRect: {
            gp::GraphicsPath rp;
            TbRoundPath(rp, gp::RectF(cx - 13, cy - 11, 26, 22), 5.5f);
            gr.DrawPath(&pen, &rp);
            break;
        }
        case kTbEllipse:
            gr.DrawEllipse(&pen, gp::RectF(cx - 13, cy - 11, 26, 22));
            break;
        case kTbArrow: {
            const gp::PointF A(cx - 12, cy + 12);
            const gp::PointF B(cx + 12, cy - 12);
            gr.DrawLine(&pen, A, B);
            const double ang = std::atan2((double)(B.Y - A.Y), (double)(B.X - A.X));
            for (int k = -1; k <= 1; k += 2) {
                const double a2 = ang + 3.14159265358979 + k * 0.5;
                gr.DrawLine(&pen, B, gp::PointF(B.X + (gp::REAL)(11 * std::cos(a2)),
                                                B.Y + (gp::REAL)(11 * std::sin(a2))));
            }
            break;
        }
        case kTbPen: {
            // 微信铅笔：细长笔杆 + 圆弧尾端 + 实心三角笔尖（结构照抄 view.cpp，坐标按 38 框重算）
            const gp::PointF T(cx - 8.5f, cy + 10.5f);   // 笔尖顶点（左下）
            const gp::PointF E(cx + 9.5f, cy - 8.5f);    // 尾端中心（右上）
            const float dx = E.X - T.X, dy = E.Y - T.Y;
            const float len = std::sqrt(dx * dx + dy * dy);
            const float ux = dx / len, uy = dy / len;    // 轴向单位向量
            const float px = -uy, py = ux;               // 垂直向单位向量
            // w = 笔身半宽。用户："笔的图标上展示的那支笔的宽度稍微粗一点，大概 1.2 倍"：
            // 3.3 * 1.2 ≈ 4.0。线宽（3.2f）不变，只把笔身加粗。
            const float w = 4.0f, tip = 7.5f;
            const gp::PointF B1(T.X + ux * tip + px * w, T.Y + uy * tip + py * w);
            const gp::PointF B2(T.X + ux * tip - px * w, T.Y + uy * tip - py * w);
            const gp::PointF A2(E.X - px * w, E.Y - py * w);
            // 笔杆轮廓：起点 A1(E + 垂直*w，由下面那条半圆弧隐式给出) -- A2 -- B2 -- B1
            // （B1-B2 一侧开口，由实心笔尖盖住）
            gp::GraphicsPath body;
            const float a0 = (float)(std::atan2(py, px) * 180.0 / 3.14159265358979);
            body.AddArc(gp::RectF(E.X - w, E.Y - w, 2 * w, 2 * w), a0, -180.0f);
            body.AddLine(A2.X, A2.Y, B2.X, B2.Y);
            body.AddLine(B2.X, B2.Y, B1.X, B1.Y);
            body.CloseFigure();
            gr.DrawPath(&pen, &body);
            const gp::PointF tri[3] = { T, B1, B2 };
            gp::SolidBrush tb(ink);
            gr.FillPolygon(&tb, tri, 3);
            // 尾部（橡皮端）分隔线
            gr.DrawLine(&pen,
                        gp::PointF(E.X - ux * 7 + px * w, E.Y - uy * 7 + py * w),
                        gp::PointF(E.X - ux * 7 - px * w, E.Y - uy * 7 - py * w));
            break;
        }
        case kTbText: {
            // 微信「文字识别」同款：四角取景框 + 中间一个「文」。
            // 用户原话："文字识别能不能按照微信这个设计，一模一样就行"。
            // 取景框四个角是"圆头圆角的 L"：用三点路径 + 圆头圆角画，拐角自然带
            // 圆弧，不用另外拼 AddArc。中心「文」的取法见下面那段注释（用字体）。
            const float L = cx - 11.0f, T = cy - 11.0f;
            const float R = cx + 11.0f, B = cy + 11.0f;
            const float A = 7.0f;                       // 角臂长
            auto Bracket = [&](float x0, float y0, float x1, float y1,
                               float x2, float y2) {
                gp::GraphicsPath p;
                p.AddLine(x0, y0, x1, y1);
                p.AddLine(x1, y1, x2, y2);
                gr.DrawPath(&pen, &p);
            };
            Bracket(L, T + A, L, T, L + A, T);          // 左上
            Bracket(R - A, T, R, T, R, T + A);          // 右上
            Bracket(R, B - A, R, B, R - A, B);          // 右下
            Bracket(L + A, B, L, B, L, B - A);          // 左下

            // 中心「文」**用字体**，不手画。
            // 上一版我用 3.2/2.6px 的线手画四笔，在 13px 见方里四笔交叉成一颗星，
            // 用户直接说"怎么这么丑"。参考图里那个「文」本来就是印刷体字形——
            // 字形靠的是笔画的粗细对比和留白，手画等粗线恰好把它抹掉了。
            // 早前抱怨"字体丑"是因为当时用 26px 撑满整个按钮；13px 缩在取景框里
            // 就没有那个问题。
            // FontFamily 必须先于 Font 构造、晚于 Font 析构（/permissive 下不能对
            // 临时对象取地址），所以两个都提成 static。
            static gp::FontFamily ffText(L"Microsoft YaHei");
            // 用户："'文'不在整个框的中央，稍微加粗一点点，然后向左向下一点点"。
            // 加粗用 FontStyleBold（13px 下 Regular→Bold 大约每笔厚 0.5~1px，
            // 正好是"一点点"；再大就该改字号了）。位置偏移也做成常量，方便再微调。
            static gp::Font fText(&ffText, 13, gp::FontStyleBold, gp::UnitPixel);
            const float kTextDx = -1.0f;    // 负=左移
            const float kTextDy = +1.0f;    // 正=下移
            gr.SetTextRenderingHint(gp::TextRenderingHintAntiAlias);
            gp::SolidBrush tb(ink);
            gp::StringFormat sf;
            sf.SetAlignment(gp::StringAlignmentCenter);
            sf.SetLineAlignment(gp::StringAlignmentCenter);
            gp::RectF meas;
            gr.MeasureString(L"文", 1, &fText, gp::PointF(0.f, 0.f), &sf, &meas);
            // MeasureString 的结果带字面留白，按它自己的 X/Y 偏移做光学居中
            gr.DrawString(L"文", -1, &fText,
                          gp::PointF(cx + kTextDx - meas.Width / 2 - meas.X,
                                     cy + kTextDy - meas.Height / 2 - meas.Y), &sf, &tb);
            break;
        }
        case kTbSave: {
            // 「下载」：向下箭头 + 下方托盘。托盘两个下角用圆弧过渡、两端圆头。
            const gp::PointF tip(cx, cy + 3.5f);
            gr.DrawLine(&pen, gp::PointF(cx, cy - 9.5f), tip);
            gr.DrawLine(&pen, gp::PointF(cx - 7.5f, cy - 2.0f), tip);
            gr.DrawLine(&pen, gp::PointF(cx + 7.5f, cy - 2.0f), tip);
            const float bx0 = cx - 9.5f, bx1 = cx + 9.5f;
            const float bTop = cy + 5.5f, bBot = cy + 10.5f, rr = 3.8f;
            gp::GraphicsPath tray;
            tray.AddLine(bx0, bTop, bx0, bBot - rr);
            tray.AddArc(bx0, bBot - 2 * rr, 2 * rr, 2 * rr, 180, -90);
            tray.AddLine(bx0 + rr, bBot, bx1 - rr, bBot);
            tray.AddArc(bx1 - 2 * rr, bBot - 2 * rr, 2 * rr, 2 * rr, 90, -90);
            tray.AddLine(bx1, bBot - rr, bx1, bTop);
            gr.DrawPath(&pen, &tray);
            break;
        }
        default:
            break;
    }
}

static LRESULT CALLBACK TbProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc{ 0, 0, kTbW, kTbH };
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP mb = CreateCompatibleBitmap(dc, kTbW, kTbH);
            HGDIOBJ ob = SelectObject(mem, mb);
            // 先把整窗刷成纯白：GDI+ 是"叠在 DC 上"画，没画到的区域会露出黑
            HBRUSH bg = CreateSolidBrush(RGB(255, 255, 255));
            FillRect(mem, &rc, bg);
            DeleteObject(bg);

            {
                gp::Graphics gr(mem);
                gr.SetSmoothingMode(gp::SmoothingModeAntiAlias);
                const gp::Color ink(255, 62, 66, 72);

                // ---- 条底：白底圆角 + 1.2px 浅灰描边（圆角由 TbCreate 的窗口 Rgn 兜外沿）----
                {
                    gp::GraphicsPath path;
                    TbRoundPath(path, gp::RectF(0.0f, 0.0f, (float)kTbW, (float)kTbH), 10.0f);
                    gp::SolidBrush white(gp::Color(255, 255, 255, 255));
                    gr.FillPath(&white, &path);
                    gp::Pen border(gp::Color(255, 224, 227, 231), 1.2f);
                    gr.DrawPath(&border, &path);
                }
                // ---- 两条分组竖线：没有线就分不出"工具|保存|颜色|粗细" ----
                {
                    gp::Pen dv(gp::Color(255, 226, 229, 233), 1.4f);
                    dv.SetStartCap(gp::LineCapRound);
                    dv.SetEndCap(gp::LineCapRound);
                    gr.DrawLine(&dv, gp::PointF((float)kTbSep1 + 1, 12.0f),
                                     gp::PointF((float)kTbSep1 + 1, 36.0f));
                    gr.DrawLine(&dv, gp::PointF((float)kTbSep2 + 1, 12.0f),
                                     gp::PointF((float)kTbSep2 + 1, 36.0f));
                }

                // ---- 工具 + 保存：38x38 格子，选中/悬停时垫一层内缩 3px 的圆角底 ----
                for (int id = kTbRect; id <= kTbSave; ++id) {
                    RECT r = TbButtonRect(id);
                    const bool hover   = (g_tbHot == id);
                    const bool pressed = (g_tbPressDown && !g_tbArmed && g_tbPress == id);
                    const bool active  = (id <= kTbText) && (g.annTool == AT_RECT + (id - kTbRect));
                    // 保存键**没有**常驻底色了（用户："下载为什么默认有蓝色的背景色块？去掉"），
                    // 只有 hover/press 才画底，和工具按钮走同一套。
                    if (hover || pressed || active) {
                        gp::GraphicsPath path;
                        TbRoundPath(path,
                                    gp::RectF((float)r.left + 3, (float)r.top + 3,
                                              (float)kTbBtn - 6, (float)kTbBtn - 6), 8.0f);
                        gp::SolidBrush b(pressed ? gp::Color(255, 212, 216, 222)
                                         : active ? gp::Color(255, 229, 232, 236)
                                                  : gp::Color(255, 243, 244, 246));
                        gr.FillPath(&b, &path);
                    }
                    TbDrawIcon(gr, id, (r.left + r.right) / 2.0f,
                               (r.top + r.bottom) / 2.0f, ink);
                }

                // ---- 颜色圆点：6 个一律直径 22，不再给选中项套更大的环 ----
                // 用户说的"大大小小不统一"就是那圈环造成的；选中改成在圆点后面垫 28x28
                // 浅蓝圆角方块，圆点本身大小一个像素都不变。
                for (int id = kTbColor0; id <= kTbColor5; ++id) {
                    RECT r = TbButtonRect(id);
                    const float cx = (r.left + r.right) / 2.0f;
                    const float cy = (r.top + r.bottom) / 2.0f;
                    const int ci = id - kTbColor0;
                    const int cr = kAnnColors[ci][0], cg = kAnnColors[ci][1], cb = kAnnColors[ci][2];
                    if (g.annColor == ci) {
                        gp::GraphicsPath path;
                        TbRoundPath(path, gp::RectF(cx - 14, cy - 14, 28, 28), 8.0f);
                        gp::SolidBrush sb(gp::Color(255, 219, 229, 241));
                        gr.FillPath(&sb, &path);
                    }
                    gp::SolidBrush b(gp::Color(255, (BYTE)cr, (BYTE)cg, (BYTE)cb));
                    gr.FillEllipse(&b, cx - kTbSwR, cy - kTbSwR,
                                   2.0f * kTbSwR, 2.0f * kTbSwR);
                    // 亮度 > 160 的（白、黄）在白底上看不见，补一圈 1px 灰边
                    if ((cr * 299 + cg * 587 + cb * 114) / 1000 > 160) {
                        gp::Pen ep(gp::Color(255, 205, 208, 214), 1.0f);
                        gr.DrawEllipse(&ep, cx - kTbSwR, cy - kTbSwR,
                                       2.0f * kTbSwR, 2.0f * kTbSwR);
                    }
                }

                // ---- 粗细圆点：半径 5/7/9.5/12（旧的 8/12/16/20 直径 × 1.2）----
                static const float kDotR[4] = { 5.0f, 7.0f, 9.5f, 12.0f };
                for (int id = kTbWidth0; id <= kTbWidth3; ++id) {
                    RECT r = TbButtonRect(id);
                    const float cx = (r.left + r.right) / 2.0f;
                    const float cy = (r.top + r.bottom) / 2.0f;
                    const int wi = id - kTbWidth0;
                    if (g.annWidthIdx == wi) {
                        gp::GraphicsPath path;
                        TbRoundPath(path, gp::RectF(cx - 14, cy - 14, 28, 28), 8.0f);
                        gp::SolidBrush sb(gp::Color(255, 219, 229, 241));
                        gr.FillPath(&sb, &path);
                    }
                    gp::SolidBrush b(ink);
                    gr.FillEllipse(&b, cx - kDotR[wi], cy - kDotR[wi],
                                   2.0f * kDotR[wi], 2.0f * kDotR[wi]);
                }
            }

            BitBlt(dc, 0, 0, kTbW, kTbH, mem, 0, 0, SRCCOPY);
            SelectObject(mem, ob);
            DeleteObject(mb);
            DeleteDC(mem);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            if (g_tbDrag) {
                // 拖动态：用**绝对光标位置**算位移，不用 lParam。原因：
                // 拖动中每帧都在 SetWindowPos，Windows 会因窗口移动补发一批
                // WM_MOUSEMOVE，那些合成消息的客户区坐标相对的是移动后的窗口，
                // 拿它算增量会一路放大（实测第一帧 (86,668)、第二帧直接跳到 (1099,477)）。
                // GetCursorPos 是绝对坐标，合成消息和真实消息报的是同一个点，天然免疫。
                POINT sp; GetCursorPos(&sp);
                SetWindowPos(h, NULL,
                             g_tbDragPos.x + sp.x - g_tbDragPt.x,
                             g_tbDragPos.y + sp.y - g_tbDragPt.y,
                             0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                TbClampToWork();
                return 0;
            }
            if (g_tbPressDown && !g_tbArmed) {
                // 还没到 300ms 就拉了超过阈值 —— 当拖动处理（否则快速拖动会变成"点一下"）
                POINT sp; GetCursorPos(&sp);
                if (std::abs(sp.x - g_tbDragPt.x) > kTbDragSlop ||
                    std::abs(sp.y - g_tbDragPt.y) > kTbDragSlop) {
                    TbBeginDrag();
                    return 0;
                }
            }
            int hit = TbHitTest(pt);
            if (hit != g_tbHot) { g_tbHot = hit; InvalidateRect(h, NULL, FALSE); }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            g_tbPress = TbHitTest(pt);
            g_tbDownPt = pt;
            // 先把锚点记下来：WM_MOUSEMOVE 判"位移超过 4px"要用它，
            // 那时 g_tbDragPos/g_tbDragPt 还是上一轮的值（甚至是 0）。
            {
                RECT wr; GetWindowRect(h, &wr);
                g_tbDragPos = { wr.left, wr.top };
                g_tbDragPt  = { wr.left + pt.x, wr.top + pt.y };
            }
            g_tbPressDown = true;
            g_tbArmed = false;
            g_tbDrag = false;
            SetCapture(h);
            SetTimer(h, kTbDragTimer, (UINT)kTbDragDelayMs, NULL);   // 长按判定
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        case WM_TIMER:
            if (w == kTbDragTimer) {
                KillTimer(h, kTbDragTimer);
                if (g_tbPressDown && !g_tbArmed) TbBeginDrag();   // 按满 300ms → 转拖动
            }
            return 0;
        case WM_LBUTTONUP: {
            POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            int hit = TbHitTest(pt);
            int press = g_tbPress;
            // 先取状态再收尾：ReleaseCapture() 会同步发 WM_CAPTURECHANGED，
            // 那里会把 g_tbPressDown/g_tbDrag 清掉（view.cpp 同款坑，注释也抄过来了）
            const bool wasDrag  = g_tbDrag;
            const bool wasPress = g_tbPressDown && !g_tbArmed;
            KillTimer(h, kTbDragTimer);
            ReleaseCapture();
            g_tbPressDown = false;
            g_tbArmed = false;
            g_tbDrag = false;
            g_tbPress = 0;
            SetCursor(LoadCursor(NULL, IDC_ARROW));
            InvalidateRect(h, NULL, FALSE);
            if (wasDrag) {
                g_tbUserMoved = true;      // 拖过就别再自动跟回选区
            } else if (wasPress && press != 0 && press == hit) {
                TbCommand(press);
                // 选工具/颜色/粗细后要刷新按钮状态；收尾路径已经把 g_tb 销毁了
                if (g_tb) InvalidateRect(g_tb, NULL, FALSE);
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            // 焦点/捕获被抢走（比如弹了另存为对话框）：把拖动状态清干净，别留半个拖动态
            if ((HWND)l != h) {
                KillTimer(h, kTbDragTimer);
                g_tbPressDown = false;
                g_tbArmed = false;
                g_tbDrag = false;
                g_tbPress = 0;
            }
            return 0;
        case WM_MOUSEACTIVATE:
            // 点击不抢焦点：焦点留在遮罩上，ESC / 方向键始终归遮罩
            return MA_NOACTIVATE;
    }
    return DefWindowProc(h, m, w, l);
}

static LRESULT CALLBACK CapWnd(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case kMsgOcrDone: {
            // 后台识别回来了：在 UI 线程回填词表并重画。
            // 代号不符说明这期间用户又框了一次（或新拉了一个子框），旧结果作废。
            std::vector<OcrWord> ws;
            {
                std::lock_guard<std::mutex> lk(g_ocrMu);
                ws.swap(g_ocrStage);
            }
            g.ocrBusy = false;
            if (!ws.empty() || !g.words.empty()) {
                g.words = std::move(ws);
                g.wordSel.assign(g.words.size(), 0);
                Redraw();
            } else {
                Redraw();        // 一个词都没认出来：也要重画一次，把"识别中"的痕迹清掉
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            POINT p;
            p.x = GET_X_LPARAM(l) + g.vx;
            p.y = GET_Y_LPARAM(l) + g.vy;
            if (g.phase == Phase::Adjust) {
                // 调整态：命中把手则开始拖把手（把手优先 —— 点在选区边上就是微调，
                // 不该变成落笔）；点在选区内且选了工具才开始画标注；
                // 其余位置什么都不做（避免误触把刚调好的选区清掉）
                int i = HitHandle(p);
                if (i >= 0) { g.dragHandle = i; SetCapture(h); Redraw(); break; }
                // 识别已改异步：这里**不再**挡"识别中再起笔"。旧结果回来时靠
                // g_ocrGen 判代作废，用户想连着框第二次不用等上一次跑完。
                if (g.annTool == AT_NONE || !PtInRect(&g.sel, p)) break;
                if (g.annTool == AT_TEXT) {
                    // 子框**里面**按下 = 拖选词（不重新识别）；子框**外面**按下 = 拉新子框
                    // 并重新识别。这样"改选词"和"重新框一块"不会互相抢手势。
                    if (g.textHasFocus && !REmpty(g.textRect) && PtInRect(&g.textRect, p)) {
                        g.wordDrag = true;
                        g.wordDragStart = p;
                        g.wordDragCur = p;
                    } else {
                        g.textDragging = true;
                        g.textDragStart = p;
                        g.textRect = RECT{ 0, 0, 0, 0 };
                        g.textHasFocus = false;
                        g.words.clear();
                        g.wordSel.clear();
                        // 作废旧识别：上一次框的结果可能还在后台跑，回来时不该
                        // 盖到这次新拖的子框上。
                        ++g_ocrGen;
                        g.ocrBusy = false;
                    }
                    SetCapture(h);
                    Redraw();
                    break;
                }
                if (g.annTool != AT_NONE && PtInRect(&g.sel, p)) {
                    g.drawingAnn = true;
                    g.annCur = Anno{};
                    g.annCur.tool = g.annTool;
                    g.annCur.color = g.annColor;
                    g.annCur.widthIdx = g.annWidthIdx;
                    g.annCur.pts.push_back(p);
                    // 两点型（矩形/椭圆/箭头）先放两个相同的点，移动时只改第二个
                    if (g.annTool != AT_PEN) g.annCur.pts.push_back(p);
                    SetCapture(h);
                    Redraw();
                }
                break;
            }
            // 起点也吸附：从窗口角/边上起拖，整框才会跟着窗口对齐（微信同款）。
            // 之前只吸附移动中的那个点，起点留在鼠标原始位置，导致"一边对齐一边差几像素"。
            Snap(p);
            g.drag = true; g.start = p; g.cur = p;
            g.sel = Norm(g.start, g.start);
            SetCapture(h); Redraw(); break;
        }
        case WM_MOUSEMOVE: {
            POINT p;
            p.x = GET_X_LPARAM(l) + g.vx;
            p.y = GET_Y_LPARAM(l) + g.vy;
            g.cur = p;
            // 拉子框：虚线框由 BuildOverlayState 用 textDragStart/g.cur 现算，这里只管重画
            if (g.phase == Phase::Adjust && g.textDragging) { Redraw(); break; }
            // 拖选词：边拖边更新选中位（高亮实时变，看得见选了哪几个词）
            if (g.phase == Phase::Adjust && g.wordDrag) {
                g.wordDragCur = p;
                ApplyWordSel();
                Redraw();
                break;
            }
            if (g.phase == Phase::Adjust && g.drawingAnn) {
                // Shift 用 GetAsyncKeyState 而不是 GetKeyState：后者读的是"本线程消息队列
                // 里处理到的按键状态"，用户先按住 Shift 再按下鼠标时可能还没处理到那条
                // keydown；异步版直接读物理状态，行为更符合"按住就是按住"。
                const bool square = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 &&
                                    (g.annCur.tool == AT_RECT || g.annCur.tool == AT_ELLIPSE);
                if (g.annCur.tool == AT_PEN) {
                    // 钳在选区内：墨迹只在洞里可见，也只会被存进最终截图
                    POINT q = p;
                    if (q.x < g.sel.left)             q.x = g.sel.left;
                    if (q.x > g.sel.right - 1)        q.x = g.sel.right - 1;
                    if (q.y < g.sel.top)              q.y = g.sel.top;
                    if (q.y > g.sel.bottom - 1)       q.y = g.sel.bottom - 1;
                    const POINT back = g.annCur.pts.back();
                    // 曼哈顿距离 ≥2 才收点：鼠标不动时 message 仍在刷，不去重会长出
                    // 上万个重复点（每帧都要全量比较 + 全量重画）
                    if (std::abs(q.x - back.x) + std::abs(q.y - back.y) >= 2)
                        g.annCur.pts.push_back(q);
                } else if (g.annCur.pts.size() >= 2) {
                    g.annCur.pts[1] = AnnEndPoint(g.annCur.pts[0], p, square);
                }
                Redraw();
                break;
            }
            if (g.phase == Phase::Adjust && g.dragHandle >= 0) {
                ApplyHandleDrag(p);
                TbReposition();
                Redraw();
                break;
            }
            if (g.drag) {
                Snap(p);
                g.sel = Norm(g.start, p);
            }
            Redraw();
            break;
        }
        case WM_LBUTTONUP:
            if (g.phase == Phase::Adjust) {
                POINT up;
                up.x = GET_X_LPARAM(l) + g.vx;
                up.y = GET_Y_LPARAM(l) + g.vy;
                if (g.textDragging) {
                    g.textDragging = false;
                    ReleaseCapture();
                    g.cur = up;
                    RECT r = Norm(g.textDragStart, g.cur);
                    if (r.right - r.left >= 6 && r.bottom - r.top >= 6) {
                        g.textRect = r;
                        g.textHasFocus = true;
                        RunTextOcr();        // 同步识别（里面会把光标换成沙漏）
                    } else {
                        g.textRect = RECT{ 0, 0, 0, 0 };
                        g.textHasFocus = false;
                        g.words.clear();
                        g.wordSel.clear();
                    }
                    Redraw();
                    break;
                }
                if (g.wordDrag) {
                    g.wordDrag = false;
                    g.wordDragCur = up;
                    ApplyWordSel();          // 点一下没拖 = 全部取消选中 → 保存时取全部词
                    ReleaseCapture();
                    Redraw();
                    break;
                }
                if (g.drawingAnn) {
                    g.drawingAnn = false;
                    ReleaseCapture();
                    // 收尾也要过一遍约束：最后一次 MOVE 之后鼠标还可能再挪几像素
                    if (g.annCur.tool != AT_PEN && g.annCur.pts.size() >= 2) {
                        const bool square = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 &&
                                            (g.annCur.tool == AT_RECT || g.annCur.tool == AT_ELLIPSE);
                        g.annCur.pts[1] = AnnEndPoint(g.annCur.pts[0], up, square);
                    }
                    // 外接框两边都 < 3px 的一笔当误点丢掉（手抖、点了没拖）
                    int l2 = 0, t2 = 0, r2 = 0, b2 = 0;
                    if (!g.annCur.pts.empty()) {
                        l2 = r2 = g.annCur.pts[0].x;
                        t2 = b2 = g.annCur.pts[0].y;
                        for (size_t k = 1; k < g.annCur.pts.size(); ++k) {
                            l2 = std::min(l2, (int)g.annCur.pts[k].x);
                            r2 = std::max(r2, (int)g.annCur.pts[k].x);
                            t2 = std::min(t2, (int)g.annCur.pts[k].y);
                            b2 = std::max(b2, (int)g.annCur.pts[k].y);
                        }
                    }
                    if (!(r2 - l2 < 3 && b2 - t2 < 3)) g.annos.push_back(g.annCur);
                    g.annCur = Anno{};
                    Redraw();
                    break;
                }
                if (g.dragHandle >= 0) { g.dragHandle = -1; ReleaseCapture(); Redraw(); }
                break;
            }
            if (g.drag) {
                g.drag = false; ReleaseCapture();
                int dx = std::abs(g.start.x - g.cur.x);
                int dy = std::abs(g.start.y - g.cur.y);
                if (dx < 5 && dy < 5) {
                    // Single click: capture the whole hovered window.
                    RECT hov = FindHoveredWindow(g.start);
                    if (hov.right > hov.left) {
                        g.sel = hov;
                    } else {
                        // 空处单击（裸桌面：桌面是 Shell 窗口，被 IsShellChrome 排除，
                        // 所以 FindHoveredWindow 什么也找不到）——退一步选**整个显示器**。
                        // 这是"截整屏"最短的动作；比"什么都不发生"有用，且选区会立刻
                        // 铺满屏幕，用户一眼就知道选中了什么，误点了右键/ESC 即可退出。
                        HMONITOR hm = MonitorFromPoint(g.start, MONITOR_DEFAULTTONEAREST);
                        MONITORINFO mi{ sizeof(mi) };
                        if (GetMonitorInfoW(hm, &mi)) g.sel = mi.rcMonitor;
                    }
                }
                // 不再销毁遮罩：有有效选区就进入调整态（微信截图同款），
                // 由 ESC / 保存 决定最终落盘。选区太小（误点）则留在选区态。
                if (g.sel.right - g.sel.left >= 2 && g.sel.bottom - g.sel.top >= 2) {
                    g.phase = Phase::Adjust;
                    TbCreate();
                    Redraw();
                }
            }
            break;
        case WM_RBUTTONDOWN:
            // 右键 = 取消（选区态与调整态都是）
            FinishAdjust(false);
            break;
        case WM_KEYDOWN:
            if (w == VK_ESCAPE) {
                // 选区态 ESC = 取消；调整态 ESC = 保存（用户定案）
                if (g.phase == Phase::Adjust) FinishAdjust(kTbSave);
                else {
                    g.captured = false; g.saveToClip = false; g.ocrWanted = false;
                    g.done = true; DestroyWindow(h);
                }
                break;
            }
            if (w == VK_RETURN && g.phase == Phase::Adjust) { FinishAdjust(kTbSave); break; }
            // Ctrl+Z 撤销最后一笔标注（只在调整态有意义）
            if (g.phase == Phase::Adjust && w == 'Z' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                if (!g.annos.empty()) { g.annos.pop_back(); Redraw(); }
                break;
            }
            // 方向键微调：1px，按住 Shift 10px。吸附完还差一两个像素时不用再抖鼠标。
            {
                const int step = (GetKeyState(VK_SHIFT) & 0x8000) ? 10 : 1;
                int dx = 0, dy = 0;
                if      (w == VK_LEFT)  dx = -step;
                else if (w == VK_RIGHT) dx =  step;
                else if (w == VK_UP)    dy = -step;
                else if (w == VK_DOWN)  dy =  step;
                else break;
                if (g.phase == Phase::Adjust) {
                    // 调整态：整体平移选区（钳回虚拟屏）
                    OffsetRect(&g.sel, dx, dy);
                    if (g.sel.left   < g.vx)       { OffsetRect(&g.sel, g.vx - g.sel.left, 0); }
                    if (g.sel.right  > g.vx+ g.vw){ OffsetRect(&g.sel, g.vx + g.vw - g.sel.right, 0); }
                    if (g.sel.top    < g.vy)       { OffsetRect(&g.sel, 0, g.vy - g.sel.top); }
                    if (g.sel.bottom > g.vy+ g.vh){ OffsetRect(&g.sel, 0, g.vy + g.vh - g.sel.bottom); }
                    TbReposition();
                } else {
                    g.cur.x += dx; g.cur.y += dy;
                    if (g.drag) {
                        POINT q = g.cur;
                        Snap(q);
                        g.sel = Norm(g.start, q);
                    }
                }
                Redraw();
            }
            break;
        case WM_SETCURSOR: {
            if (g.phase == Phase::Adjust) {
                POINT sp;
                GetCursorPos(&sp);
                int i = HitHandle(sp);
                if (i >= 0) {
                    // IDC_* 是 TCHAR 中性的 MAKEINTRESOURCE，用 TCHAR* 承接才与
                    // 当前编译是否定义 UNICODE 一致
                    static const TCHAR* kCur[8] = {
                        IDC_SIZENWSE, IDC_SIZENS, IDC_SIZENESW, IDC_SIZEWE,
                        IDC_SIZENWSE, IDC_SIZENS, IDC_SIZENESW, IDC_SIZEWE,
                    };
                    SetCursor(LoadCursor(NULL, kCur[i]));
                    return TRUE;
                }
            }
            break;   // 其余交给 DefWindowProc（用类光标 IDC_CROSS）
        }
        case WM_DESTROY:
            // 遮罩没了工具栏也跟着走（正常路径走 FinishAdjust，这里是兜底）
            if (g_tb) { DestroyWindow(g_tb); g_tb = nullptr; }
            // 注意：这里**不能** PostQuitMessage。
            // 本窗口的消息循环是靠 g.done 跳出的（见 CaptureRegionInteractive 末尾），
            // 跳出时不会去取 WM_QUIT；残留的 WM_QUIT 会被外层的宿主主循环读到，
            // 于是"取消截图"会把这个常驻宿主进程一起杀掉 —— 下次截图又要重新启动
            // 进程、重新加载 OCR 引擎（用户感知就是"点一下要等好几秒")。
            break;
    }
    return DefWindowProc(h, m, w, l);
}

// 整屏干净像素（1080p ≈ 8.3MB / 4K ≈ 33MB / 双 4K ≈ 66MB）在 CaptureRegionInteractive
// 返回后本来会一直驻留到下一次截图。截图结束就还回去 —— OCR 阶段根本用不到它，
// 而且调整态一结束遮罩也该销毁（常驻分层表面本身就有空闲 CPU 开销）。
static void ReleaseScreenBuffers() {
    g.screen.clear(); g.screen.shrink_to_fit();
    g.dim.clear();    g.dim.shrink_to_fit();
}

// 遮罩/结果窗期间临时关掉"用完即卸"。
// 理由：用户一次截图里经常要框两次字（先框一大块、再框准一点），而引擎每次
// Init 要 238ms ——"识别完就卸"会让第二次多付这 238ms。改成遮罩存活期间不卸，
// 收尾再卸，静默内存照样回到 1~2MB，只是截图那几秒多占约 56MB（用户接受）。
// 用 RAII：这个函数里有 5 处 return，手工恢复必然漏掉某一条。
struct OcrKeepAlive {
    bool prev;
    OcrKeepAlive() : prev(OcrGetUnloadAfterUse()) { OcrSetUnloadAfterUse(false); }
    ~OcrKeepAlive() {
        OcrSetUnloadAfterUse(prev);
        if (prev) OcrRequestUnload();   // 排队卸载：不打断可能还在跑的识别
    }
};

CaptureImage CaptureRegionInteractive() {
    OcrKeepAlive keepAliveOcr;      // 必须最先构造：后面任何 return 都会恢复策略
    EnsureDpi();
    // 先取虚拟屏幕尺寸，供 EnumWnd 过滤桌面/过大窗口时使用。
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    g = Cap{};
    g_shotCopied = false;      // 新一轮：清空"已写剪贴板"标记
    // 「下载」的目标路径是"这一次截图"的临时状态，必须每轮清掉：
    // 否则上一轮存过的文件会一直挂着，下次点 ESC 保存也会跟着写一遍同一个路径。
    g_saveToFile = false;
    g_savePath.clear();
    // 上一轮可能还有识别在跑（用户在识别完成前就保存/取消了）。作废它，
    // 并清掉暂存区，免得旧词表漏到这一轮的画面上。
    {
        std::lock_guard<std::mutex> lk(g_ocrMu);
        g_ocrStage.clear();
        ++g_ocrGen;
    }
    g.vx = vx; g.vy = vy; g.vw = vw; g.vh = vh;
    // 颜色/粗细记忆：宿主是常驻进程，但用户在外部改 shot_ui.ini 也要生效，
    // 所以每次开新截图读一次（两次 GetPrivateProfileInt，代价可以忽略）。
    LoadAnnoPrefs(g.annColor, g.annWidthIdx);
    // 渲染状态复位：首帧走整窗上传；render_full=1 可强制回到旧的整窗路径
    g_prevValid = false;
    g_prev = OverlayState{};
    {
        const wchar_t* ini = ShotIniPath();
        g_renderFull = (ini && *ini &&
                        GetPrivateProfileIntW(L"screenshot", L"render_full", 0, ini) != 0);
    }

    g_wins.clear();
    EnumWindows(EnumWnd, 0);
    SortWinsByZOrder();

    if (g.vw <= 0 || g.vh <= 0) return CaptureImage{};

    // Capture the entire virtual screen BEFORE showing the overlay.
    {
        HDC hDisp = CreateDC(TEXT("DISPLAY"), NULL, NULL, NULL);
        HDC hMem = CreateCompatibleDC(hDisp);
        HBITMAP hBmp = CreateCompatibleBitmap(hDisp, g.vw, g.vh);
        HGDIOBJ old = SelectObject(hMem, hBmp);
        BOOL blitOK = BitBlt(hMem, 0, 0, g.vw, g.vh, hDisp, g.vx, g.vy, SRCCOPY);
        // BitBlt 在某些 DWM/远程桌面/高 DPI 组合下会返回 0 或得到空白。
        // 若失败，回退到 PrintWindow(EnumDisplayMonitors) 的方式：对每个顶层窗口
        // 截取客户区并合成到虚拟屏幕上。
        if (!blitOK) {
            RECT vr{ g.vx, g.vy, g.vx + g.vw, g.vy + g.vh };
            HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(hMem, &vr, black);
            DeleteObject(black);
            auto paintWnd = [&](HWND hw) -> BOOL {
                if (!IsWindowVisible(hw)) return TRUE;
                RECT wr; if (!GetWindowRect(hw, &wr)) return TRUE;
                RECT inter;
                if (!IntersectRect(&inter, &wr, &vr)) return TRUE;
                // 调整 DC 原点，使 PrintWindow 把窗口内容画到虚拟屏幕的对应位置。
                int orgX = wr.left - g.vx;
                int orgY = wr.top - g.vy;
                POINT oldOrg;
                SetWindowOrgEx(hMem, -orgX, -orgY, &oldOrg);
                // 使用 PW_RENDERFULLCONTENT 尽可能拿到 DWM 合成后的内容。
                PrintWindow(hw, hMem, 2 /*PW_RENDERFULLCONTENT*/);
                SetWindowOrgEx(hMem, oldOrg.x, oldOrg.y, NULL);
                return TRUE;
            };
            EnumWindows([](HWND hw, LPARAM lp) -> BOOL {
                return (*reinterpret_cast<decltype(paintWnd)*>(lp))(hw);
            }, reinterpret_cast<LPARAM>(&paintWnd));
        }

        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = g.vw;
        bi.bmiHeader.biHeight = -g.vh;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        g.screen.resize((size_t)g.vw * g.vh * 4);
        GetDIBits(hMem, hBmp, 0, g.vh, g.screen.data(), &bi, DIB_RGB_COLORS);

        SelectObject(hMem, old);
        DeleteObject(hBmp);
        DeleteDC(hMem);
        DeleteDC(hDisp);
    }
    if (g.screen.empty()) return CaptureImage{};

    // Pre-compute dimmed overlay base for the area OUTSIDE the selection.
    // We use per-pixel alpha with AC_SRC_ALPHA, so RGB must be premultiplied.
    // Outside: alpha=255 (fully opaque to this overlay), RGB = desktop * dimMul/255.
    // Blended result = desktop * dimMul/255, giving a clearly visible dark veil.
    g.dim = g.screen;
    unsigned char* dp = g.dim.data();
    const int dimMul = 140; // darken to 55%: result = desktop * 0.55
    // 用 256 项查找表代替每像素 3 次乘除法：结果与 (v*140/255) 完全等价，
    // 但把 3 次乘除降为 3 次查表，整屏（数百万像素）叠加时明显更快。
    unsigned char lut[256];
    for (int v = 0; v < 256; v++) lut[v] = (unsigned char)(v * dimMul / 255);
    for (int i = 0; i < g.vw * g.vh; i++) {
        dp[0] = lut[dp[0]]; // B
        dp[1] = lut[dp[1]]; // G
        dp[2] = lut[dp[2]]; // R
        dp[3] = 255;        // A
        dp += 4;
    }

    // 32-bit DIB for the layered overlay (same size as virtual screen).
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = g.vw;
    bmi.bmiHeader.biHeight = -g.vh;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    HDC hScreen = GetDC(NULL);
    g.hDib = CreateDIBSection(hScreen, &bmi, DIB_RGB_COLORS, &g.bits, NULL, 0);
    g.hDc = CreateCompatibleDC(hScreen);
    SelectObject(g.hDc, g.hDib);
    ReleaseDC(NULL, hScreen);

    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = CapWnd;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_CROSS);
        wc.lpszClassName = L"OCRCap";
        RegisterClassW(&wc);
        reg = true;
    }
    g.w = CreateWindowEx(WS_EX_LAYERED | WS_EX_TOPMOST, L"OCRCap", NULL,
                         WS_POPUP, g.vx, g.vy, g.vw, g.vh,
                         NULL, NULL, GetModuleHandle(NULL), NULL);
    if (!g.w) {
        printf("[cap] CreateWindowEx failed, err=%lu\n", GetLastError());
        DeleteObject(g.hDib); DeleteDC(g.hDc);
        return CaptureImage{};
    }
    ShowWindow(g.w, SW_SHOW);
    SetForegroundWindow(g.w);
    Redraw();

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        if (g.done) break;
    }

    DeleteObject(g.hDib);
    DeleteDC(g.hDc);

    if (!g.captured) { ReleaseScreenBuffers(); return CaptureImage{}; }

    // Clamp selection to virtual screen bounds.
    RECT s = g.sel;
    if (s.left   < g.vx) s.left   = g.vx;
    if (s.top    < g.vy) s.top    = g.vy;
    if (s.right  > g.vx + g.vw) s.right = g.vx + g.vw;
    if (s.bottom > g.vy + g.vh) s.bottom = g.vy + g.vh;
    int rw = s.right - s.left, rh = s.bottom - s.top;
    if (rw < 2 || rh < 2) { ReleaseScreenBuffers(); return CaptureImage{}; }

    // Crop from the CLEAN original screen buffer (no overlay, no dimming).
    CaptureImage out;
    out.width = rw; out.height = rh;
    out.rgb.resize((size_t)rw * rh * 3);
    for (int y = 0; y < rh; y++) {
        int srcY = (s.top - g.vy) + y;
        const unsigned char* srcRow = &g.screen[(size_t)srcY * g.vw * 4];
        unsigned char* dstRow = &out.rgb[(size_t)y * rw * 3];
        for (int x = 0; x < rw; x++) {
            int srcX = (s.left - g.vx) + x;
            const unsigned char* src = srcRow + srcX * 4;
            dstRow[x * 3 + 0] = src[2]; // R
            dstRow[x * 3 + 1] = src[1]; // G
            dstRow[x * 3 + 2] = src[0]; // B
        }
    }

    // 把标注合成进最终图片。用的是与遮罩层完全相同的光栅化函数（Surf 只换了 bpp
    // 和字节序），所以预览与成品逐像素一致 —— 不会出现"预览 4px、保存出来 3px"。
    // 图像坐标 = 绝对屏幕坐标 − 选区左上角；画到选区外的那部分本来就被裁掉了。
    if (!g.annos.empty()) {
        Surf su;
        su.px = out.rgb.data();
        su.stride = rw * 3;
        su.bpp = 3;
        su.bgr = false;                 // CaptureImage.rgb 是 RGB 顺序，不是 BGRA
        su.w = rw; su.h = rh;
        su.ox = s.left; su.oy = s.top;
        su.clip = RECT{ s.left, s.top, s.left + rw, s.top + rh };
        for (size_t i = 0; i < g.annos.size(); ++i) PaintAnno(su, g.annos[i]);
    }

    // 调整态退出：裁剪只在这里做一次，然后按"框选识别 / 整块识别 / 只存图"分流。
    // 前两条路都算"已写过剪贴板"，由 ShotAlreadyCopied() 告诉 ocraddon.cpp
    // 不要再弹结果窗口（否则原地保存完还会再弹一次窗）。
    //
    // 「文」工具框出来的词优先：有子框且认出了词 → 先图后文（两次独立会话），
    // 这样 Ctrl+V 得到文字、Win+V 里图和字两条都在，与 ShotOcrAndCopy 的语义一致。
    // 分流顺序：用户点了「下载」并选了路径 → 只写文件（图里已经合成好标注了，
    // 就是用户看到的成品，不要再画一遍）。这条必须排在最前，否则会被"有选中词"
    // 或"saveToClip"抢走。
    std::string picked = PickedTextUtf8();
    if (g_saveToFile && !g_savePath.empty()) {
        g_shotCopied = true;                 // 已经给了用户结果，不用再弹旧结果窗
        SaveImageToFile(out, g_savePath);
    } else if (!picked.empty()) {
        g_shotCopied = true;
        ShotCopyImageThenText(out, picked);
    } else if (g.ocrWanted) {         // 兜底：整块识别（工具栏上已无此按钮，留着不删）
        g_shotCopied = true;
        ShotOcrAndCopy(out);          // 内部：先写图、再跑 OCR、再写文字
    } else if (g.saveToClip) {
        g_shotCopied = true;
        CopyImageToClipboard(out);
    }

    ReleaseScreenBuffers();
    g_wins.clear();
    return out;
}
