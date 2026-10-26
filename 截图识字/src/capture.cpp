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

// ---- 原地标注（矩形 / 椭圆 / 箭头 / 画笔）----
// 坐标一律用**绝对虚拟屏坐标**，不是相对选区的坐标：截图内容是一张静止的画面，
// 标注必须"画在哪就钉在哪个屏幕像素"。这样之后拖把手改选区，标注不会跟着乱跑；
// 被缩掉的那部分在保存时自然被裁掉。
enum { AT_NONE = 0, AT_RECT, AT_ELLIPSE, AT_ARROW, AT_PEN };
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
    int   annTool = AT_NONE;   // 当前选中的工具；AT_NONE = 不在标注模式
    int   annColor = 0;
    int   annWidthIdx = 0;
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
static bool SameOverlay(const OverlayState& a, const OverlayState& b) {
    if (a.drag != b.drag || a.labelValid != b.labelValid) return false;
    // 进入/离开调整态会整体出现或消失八个把手，且 footprint 也会变（外扩 kHandleH）
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
constexpr int kHandleH = 8;         // 脏区 footprint 的外扩量（必须 ≥ 把手半边长 4）

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

// 洞的「绘制足迹」：调整态要外扩 kHandleH，因为八个把手是跨洞边界画的，
// 有一半在洞外。用**一个**外扩矩形表达（而不是 4 条 ring 带）是为了让
// StateRegion 恒为「1 个洞矩形 + 1 个标签矩形」：脏区是前后两个状态的 XOR，
// footprint 的 rect 数越少，XOR 出来的 rect 就越少，越不会因为超过 kBandCap
// 而走溢出合并。覆盖性由构造保证：角把手 x∈[left-4,left+4] ⊂ [left-8,right+8]。
static RECT HoleFootprint(const OverlayState& s) {
    if (!s.adjust) return s.hole;
    RECT r = { s.hole.left - kHandleH, s.hole.top - kHandleH,
               s.hole.right + kHandleH, s.hole.bottom + kHandleH };
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
                           const RECT& extra, RECT out[], int cap) {
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
    if (!REmpty(extra)) v.push_back(extra);

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
        // 标注的前后足迹并集：内容变了（新增/撤销/正在画）时区域异或选不出脏区，
        // 必须靠它兜底，理由见 BuildDirtyBands 里那段注释。
        RECT extra = RUnion(g_prev.annBox, next.annBox);
        // 同一类漏点，标签也有：尺寸数字变了但标签矩形一字不差时（"120 x 80" 之后
        // 全是 3 位数，字宽不变 → 矩形不变），异或为空 → 这帧被判成"没东西可画"，
        // 标签就永远停在第一次变宽的那个数字上。实测：拖出 600x400 的框，标签显示
        // "120 x 80"（那是字符数刚变成 8 的那一帧），后续几十帧全没重画。
        if (next.labelValid && std::strncmp(g_prev.text, next.text, sizeof(next.text)) != 0)
            extra = RUnion(extra, next.label);
        n = BuildDirtyBands(g_prev, next, extra, bands, kBandCap);
        if (n <= 0) { g_prev = std::move(next); return; }
    }

    for (int i = 0; i < n; ++i) {
        RestoreFromDim(bands[i]);
        PaintHole(bands[i], next);
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
// 两行、8 类按钮：
//   第一行  1=矩形 2=椭圆 3=箭头 4=画笔 │ 5=撤销 │ 6=保存 7=识别文字 8=关闭
//   第二行  20..25=颜色（红/黄/蓝/绿/黑/白）        30..33=粗细（2/4/6/9）
// 第一行宽度：8 + 4×44 + 3×8 + 16 + 44 + 16 + 3×68 + 2×8 + 8 = 512
constexpr int kTbToolW = 44;    // 工具按钮（放两个汉字）
constexpr int kTbBtnW  = 68;    // 文字按钮
constexpr int kTbBtnH  = 28;
constexpr int kTbGap   = 8;
constexpr int kTbPad   = 8;
constexpr int kTbSep   = 16;    // 分组之间的分隔缝
constexpr int kTbRowH  = 32;    // 第二行（颜色 / 粗细）高度
constexpr int kTbSwW   = 22, kTbSwPitch = 30;   // 色块边长 / 步距（间距 8）
constexpr int kTbWdX0  = kTbPad + 6 * kTbSwPitch + 14;   // 粗细组起点 = 202
constexpr int kTbW = kTbPad + kTbToolW * 4 + kTbGap * 3 + kTbSep + kTbToolW + kTbSep
                   + kTbBtnW * 3 + kTbGap * 2 + kTbPad;          // = 512
constexpr int kTbH = 40 + kTbRowH;                               // = 72
enum {
    kTbRect = 1, kTbEllipse, kTbArrow, kTbPen, kTbUndo, kTbSave, kTbOcr, kTbClose,
    kTbColor0 = 20, kTbColor5 = 25,
    kTbWidth0 = 30, kTbWidth3 = 33,
};
static HWND g_tb = nullptr;
static int  g_tbHot = 0;    // 悬停的按钮
static int  g_tbPress = 0;  // 已按下尚未松开的按钮
static LRESULT CALLBACK TbProc(HWND h, UINT m, WPARAM w, LPARAM l);  // TbCreate 需要

static RECT TbButtonRect(int id) {
    const int y1 = (40 - kTbBtnH) / 2;
    if (id >= kTbRect && id <= kTbPen) {
        const int x = kTbPad + (id - kTbRect) * (kTbToolW + kTbGap);
        return { x, y1, x + kTbToolW, y1 + kTbBtnH };
    }
    if (id == kTbUndo) {
        const int x = kTbPad + kTbToolW * 4 + kTbGap * 3 + kTbSep;
        return { x, y1, x + kTbToolW, y1 + kTbBtnH };
    }
    if (id >= kTbSave && id <= kTbClose) {
        const int x0 = kTbPad + kTbToolW * 4 + kTbGap * 3 + kTbSep + kTbToolW + kTbSep;
        const int x = x0 + (id - kTbSave) * (kTbBtnW + kTbGap);
        return { x, y1, x + kTbBtnW, y1 + kTbBtnH };
    }
    if (id >= kTbColor0 && id <= kTbColor5) {
        const int y = 40 + (kTbRowH - kTbSwW) / 2;
        const int x = kTbPad + (id - kTbColor0) * kTbSwPitch;
        return { x, y, x + kTbSwW, y + kTbSwW };
    }
    if (id >= kTbWidth0 && id <= kTbWidth3) {
        const int y = 40 + (kTbRowH - kTbSwW) / 2;
        const int x = kTbWdX0 + (id - kTbWidth0) * kTbSwPitch;
        return { x, y, x + kTbSwW, y + kTbSwW };
    }
    return { 0, 0, 0, 0 };
}
static int TbHitTest(POINT pt) {
    // 不能对 TbButtonRect 的临时返回值取地址（C2102），先落到局部变量
    for (int id = kTbRect; id <= kTbClose; ++id) {
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
    if (g.w) DestroyWindow(g.w);
}

// 工具栏命令分发：工具/颜色/粗细只改状态（选区与遮罩都不动），只有 6/7/8 收尾。
static void TbCommand(int id) {
    if (id >= kTbRect && id <= kTbPen) {
        const int t = AT_RECT + (id - kTbRect);
        g.annTool = (g.annTool == t) ? AT_NONE : t;   // 再点一次同一个工具 = 退出标注模式
        return;
    }
    if (id == kTbUndo) {
        if (!g.annos.empty()) { g.annos.pop_back(); Redraw(); }
        return;
    }
    if (id >= kTbColor0 && id <= kTbColor5) { g.annColor = id - kTbColor0; return; }
    if (id >= kTbWidth0 && id <= kTbWidth3) { g.annWidthIdx = id - kTbWidth0; return; }
    FinishAdjust(id);
}

// 工具栏跟随选区：优先贴在选区正下方居中，下方放不下改上方，再钳回工作区。
static void TbReposition() {
    if (!g_tb) return;
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
    g_tbHot = 0;
    g_tbPress = 0;
    TbReposition();
    ShowWindow(g_tb, SW_SHOWNOACTIVATE);
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
            HBRUSH bg = CreateSolidBrush(RGB(255, 255, 255));
            FillRect(mem, &rc, bg);
            DeleteObject(bg);
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(210, 214, 220));
            HGDIOBJ op = SelectObject(mem, pen);
            HGDIOBJ obr = SelectObject(mem, GetStockObject(NULL_BRUSH));
            Rectangle(mem, 0, 0, kTbW, kTbH);
            SelectObject(mem, obr);
            SelectObject(mem, op);
            DeleteObject(pen);

            // 行分隔 + 分组竖线：8 个工具按钮挤在一起时，靠这几条线才看得出分组
            HBRUSH sepB = CreateSolidBrush(RGB(228, 231, 236));
            RECT hline{ kTbPad, 40, kTbW - kTbPad, 41 };
            FillRect(mem, &hline, sepB);
            {
                RECT u = TbButtonRect(kTbUndo);
                RECT sv = TbButtonRect(kTbSave);
                RECT v1{ u.left - kTbSep / 2, 8, u.left - kTbSep / 2 + 1, 32 };
                RECT v2{ sv.left - kTbSep / 2, 8, sv.left - kTbSep / 2 + 1, 32 };
                FillRect(mem, &v1, sepB);
                FillRect(mem, &v2, sepB);
            }
            DeleteObject(sepB);

            HFONT f = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                  DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
            HGDIOBJ of = SelectObject(mem, f);
            SetBkMode(mem, TRANSPARENT);
            static const wchar_t* kLabel[9] = {
                L"", L"矩形", L"椭圆", L"箭头", L"画笔", L"撤销", L"保存", L"识别文字", L"关闭"
            };
            for (int id = kTbRect; id <= kTbClose; ++id) {
                RECT r = TbButtonRect(id);
                const bool on  = (g_tbPress == id) || (g_tbPress == 0 && g_tbHot == id);
                const bool pri = (id == kTbSave);
                bool active = false;
                if (id <= kTbPen)          active = (g.annTool == AT_RECT + (id - kTbRect));
                const bool emptyUndo = (id == kTbUndo && g.annos.empty());
                COLORREF fill, txt;
                if (pri)         { fill = on ? RGB(0, 95, 180) : RGB(0, 120, 215); txt = RGB(255, 255, 255); }
                else if (active) { fill = RGB(0, 120, 215); txt = RGB(255, 255, 255); }
                else             { fill = on ? RGB(225, 228, 232) : RGB(240, 242, 245); txt = RGB(60, 60, 60); }
                if (emptyUndo) txt = RGB(170, 174, 180);   // 没东西可撤销：灰掉
                HBRUSH bb = CreateSolidBrush(fill);
                FillRect(mem, &r, bb);
                DeleteObject(bb);
                SetTextColor(mem, txt);
                DrawTextW(mem, kLabel[id], -1, &r,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            SelectObject(mem, of);
            DeleteObject(f);

            // 第二行左：颜色色块（白/黄在白底上要靠描边才看得见，选中态用 2px 蓝框）
            for (int id = kTbColor0; id <= kTbColor5; ++id) {
                RECT r = TbButtonRect(id);
                const int ci = id - kTbColor0;
                const bool sel = (g.annColor == ci);
                HBRUSH bb = CreateSolidBrush(RGB(kAnnColors[ci][0], kAnnColors[ci][1], kAnnColors[ci][2]));
                FillRect(mem, &r, bb);
                DeleteObject(bb);
                HPEN p2 = CreatePen(PS_SOLID, sel ? 2 : 1,
                                    sel ? RGB(0, 120, 215) : RGB(205, 208, 214));
                HGDIOBJ op2 = SelectObject(mem, p2);
                HGDIOBJ ob2 = SelectObject(mem, GetStockObject(NULL_BRUSH));
                Rectangle(mem, r.left, r.top, r.right, r.bottom);
                SelectObject(mem, ob2);
                SelectObject(mem, op2);
                DeleteObject(p2);
            }
            // 第二行右：粗细（画一条等厚示范线；用灰/白而不是当前颜色，
            // 否则选了白色时这条线在白底按钮上根本看不见）
            for (int id = kTbWidth0; id <= kTbWidth3; ++id) {
                RECT r = TbButtonRect(id);
                const int wi = id - kTbWidth0;
                const bool sel = (g.annWidthIdx == wi);
                const bool on = (g_tbPress == id) || (g_tbPress == 0 && g_tbHot == id);
                HBRUSH bb = CreateSolidBrush(sel ? RGB(0, 120, 215)
                                                 : (on ? RGB(225, 228, 232) : RGB(240, 242, 245)));
                FillRect(mem, &r, bb);
                DeleteObject(bb);
                const int t = kAnnWidths[wi];
                const int cy = (r.top + r.bottom) / 2;
                RECT lr{ r.left + 5, cy - t / 2, r.right - 5, cy - t / 2 + t };
                HBRUSH lb = CreateSolidBrush(sel ? RGB(255, 255, 255) : RGB(70, 70, 70));
                FillRect(mem, &lr, lb);
                DeleteObject(lb);
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
            int hit = TbHitTest(pt);
            if (hit != g_tbHot) { g_tbHot = hit; InvalidateRect(h, NULL, FALSE); }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            g_tbPress = TbHitTest(pt);
            SetCapture(h);
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            int hit = TbHitTest(pt);
            int press = g_tbPress;
            g_tbPress = 0;
            ReleaseCapture();
            InvalidateRect(h, NULL, FALSE);
            if (press != 0 && press == hit) {
                TbCommand(press);
                // 选工具/颜色/粗细后要刷新按钮状态；收尾路径已经把 g_tb 销毁了
                if (g_tb) InvalidateRect(g_tb, NULL, FALSE);
            }
            return 0;
        }
        case WM_MOUSEACTIVATE:
            // 点击不抢焦点：焦点留在遮罩上，ESC / 方向键始终归遮罩
            return MA_NOACTIVATE;
    }
    return DefWindowProc(h, m, w, l);
}

static LRESULT CALLBACK CapWnd(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
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
            if (g.phase == Phase::Adjust && g.drawingAnn) {
                // 钳在选区内：墨迹只在洞里可见，也只会被存进最终截图
                POINT q = p;
                if (q.x < g.sel.left)             q.x = g.sel.left;
                if (q.x > g.sel.right - 1)        q.x = g.sel.right - 1;
                if (q.y < g.sel.top)              q.y = g.sel.top;
                if (q.y > g.sel.bottom - 1)       q.y = g.sel.bottom - 1;
                if (g.annCur.tool == AT_PEN) {
                    const POINT back = g.annCur.pts.back();
                    // 曼哈顿距离 ≥2 才收点：鼠标不动时 message 仍在刷，不去重会长出
                    // 上万个重复点（每帧都要全量比较 + 全量重画）
                    if (std::abs(q.x - back.x) + std::abs(q.y - back.y) >= 2)
                        g.annCur.pts.push_back(q);
                } else if (g.annCur.pts.size() >= 2) {
                    g.annCur.pts[1] = q;
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
                if (g.drawingAnn) {
                    g.drawingAnn = false;
                    ReleaseCapture();
                    // 外接框两边都 < 3px 的一笔当误点丢掉（手抖、点了没拖）
                    int l = 0, t = 0, r = 0, b = 0;
                    if (!g.annCur.pts.empty()) {
                        l = r = g.annCur.pts[0].x;
                        t = b = g.annCur.pts[0].y;
                        for (size_t k = 1; k < g.annCur.pts.size(); ++k) {
                            l = std::min(l, (int)g.annCur.pts[k].x);
                            r = std::max(r, (int)g.annCur.pts[k].x);
                            t = std::min(t, (int)g.annCur.pts[k].y);
                            b = std::max(b, (int)g.annCur.pts[k].y);
                        }
                    }
                    if (!(r - l < 3 && b - t < 3)) g.annos.push_back(g.annCur);
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
                    if (hov.right > hov.left) g.sel = hov;
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

CaptureImage CaptureRegionInteractive() {
    EnsureDpi();
    // 先取虚拟屏幕尺寸，供 EnumWnd 过滤桌面/过大窗口时使用。
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    g = Cap{};
    g_shotCopied = false;      // 新一轮：清空"已写剪贴板"标记
    g.vx = vx; g.vy = vy; g.vw = vw; g.vh = vh;
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

    // 调整态退出：裁剪只在这里做一次，然后按"保存"还是"识别文字"分流。
    // 两条路都算"已写过剪贴板"，由 ShotAlreadyCopied() 告诉 ocraddon.cpp
    // 不要再弹结果窗口（否则原地保存完还会再弹一次窗）。
    if (g.ocrWanted) {
        g_shotCopied = true;
        ShotOcrAndCopy(out);      // 内部：先写图、再跑 OCR、再写文字（两次独立剪贴板会话）
    } else if (g.saveToClip) {
        g_shotCopied = true;
        CopyImageToClipboard(out);
    }

    ReleaseScreenBuffers();
    g_wins.clear();
    return out;
}
