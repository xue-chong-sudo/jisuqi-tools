// handle_drag_test.cpp —— 调整框把手的几何 / 命中 / 拖拽（含拖过对面）回归测试
//
// 覆盖的是阶段 1a 里最容易写错的三块纯逻辑：
//   ① 八个把手的几何与编号顺序（0=左上 顺时针 到 7=左中）
//   ② HitHandle 命中（含"点在选区中间必须返回 -1"）
//   ③ 拖把手**越过对面**时的交换 + dragHandle 索引重映射 —— 只交换不重映射的话，
//      越过之后继续拖会去动错误的那条边（表现为"框突然朝反方向长"）。
// 直接 #include 真实 TU，测的是产品代码本身，不是副本。
//
// 用法：编译运行，退出码 0 = 全部通过。
#include "../src/capture.cpp"

// capture.cpp 里依赖外部 TU 的三个符号（main.cpp / view.cpp 提供），
// 本测试走不到这些路径，给空实现满足链接即可。
const wchar_t* ShotIniPath() { return L""; }
bool CopyImageToClipboard(const CaptureImage&) { return true; }
bool ShotOcrAndCopy(const CaptureImage&) { return false; }
bool ShotCopyImageThenText(const CaptureImage&, const std::string&) { return false; }
bool OcrEnsureEngine() { return false; }
std::vector<OcrWord> OcrImage(const CaptureImage&) { return {}; }
void OcrImageAsync(const CaptureImage&, std::function<void(std::vector<OcrWord>)>) {}
bool OcrGetUnloadAfterUse() { return false; }
void OcrSetUnloadAfterUse(bool) {}
void OcrRequestUnload() {}
std::string FormatSelection(const std::vector<OcrWord>&, NewlineMode) { return {}; }

#include <cstdio>

static int g_fail = 0;

static void Check(bool ok, const char* what) {
    if (!ok) { std::printf("[FAIL] %s\n", what); ++g_fail; }
    else     { std::printf("[ok]   %s\n", what); }
}
static bool Same(const RECT& a, int l, int t, int r, int b) {
    return a.left == l && a.top == t && a.right == r && a.bottom == b;
}

static void ResetSel(int l, int t, int r, int b) {
    g = Cap{};
    g.vx = 0; g.vy = 0; g.vw = 1920; g.vh = 1080;
    g.phase = Phase::Adjust;
    g.sel = { l, t, r, b };
}


// 把手拖到屏幕边必须吸附（用户："鼠标挪到边上依旧不吸附"—— 调整态拖把手
// 这条路径之前完全没调 Snap()，只有拖框那两处调了）。
static void CheckHandleSnap() {
    // Snap 的兜底用的是**真实显示器**矩形（MonitorFromPoint），所以这里也必须查真的，
    // 不能把屏幕写死成 1920x1080 —— 这台机器真实显示器不是这个尺寸，写死会得到假失败。
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
    const int SR = mi.rcMonitor.right, SB = mi.rcMonitor.bottom;
    std::printf("      真实显示器右下角 = (%d,%d)\n", SR, SB);
    g.vx = mi.rcMonitor.left; g.vy = mi.rcMonitor.top;
    g.vw = SR - mi.rcMonitor.left; g.vh = SB - mi.rcMonitor.top;
    g.sel = RECT{ 100, 100, 500, 400 };
    g.dragHandle = 4;                       // 右下
    ApplyHandleDrag(POINT{ SR - 7, SB - 7 });       // 距边 7px，在 10px 阈值内
    std::printf("      实测 sel = (%ld,%ld)-(%ld,%ld)\n", g.sel.left, g.sel.top, g.sel.right, g.sel.bottom);
    Check(g.sel.right == SR && g.sel.bottom == SB,
          "拖右下把手到距边 7px → 吸附到屏幕右下角");
    g.sel = RECT{ 100, 100, 500, 400 };
    g.dragHandle = 0;                       // 左上
    ApplyHandleDrag(POINT{ mi.rcMonitor.left + 6, mi.rcMonitor.top + 5 });
    Check(g.sel.left == mi.rcMonitor.left && g.sel.top == mi.rcMonitor.top, "拖左上把手到距角 6/5px → 吸附到屏幕左上角");
    g.dragHandle = -1;
}


// 悬停兜底：鼠标底下没有任何窗口（裸桌面）时，要亮**整个显示器**。
// 用户："鼠标挪到边上，还是不能自动吸附全屏" —— 他要的是悬停即亮。
static void CheckHoverFallback() {
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
    g.vx = mi.rcMonitor.left; g.vy = mi.rcMonitor.top;
    g.vw = mi.rcMonitor.right - g.vx; g.vh = mi.rcMonitor.bottom - g.vy;
    g.phase = Phase::Select;
    g.drag = false;
    g_wins.clear();                       // 没有任何可吸附窗口 = 裸桌面
    g.cur = POINT{ g.vx + g.vw / 2, g.vy + g.vh / 2 };
    OverlayState s = BuildOverlayState();
    std::printf("      悬停(空桌面) → hole = (%ld,%ld)-(%ld,%ld)  显示器 = (%ld,%ld)-(%ld,%ld)\n",
                s.hole.left, s.hole.top, s.hole.right, s.hole.bottom,
                mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom);
    Check(s.hole.left == 0 && s.hole.top == 0 &&
          s.hole.right == g.vw && s.hole.bottom == g.vh,
          "悬停空桌面 → 亮整个屏幕");

    // 有窗口时必须优先亮那个窗口，不能被兜底抢走
    RECT wr{ g.vx + 10, g.vy + 10, g.vx + 210, g.vy + 160 };
    g_wins.push_back(WinInfo{ nullptr, wr, 0 });
    g.cur = POINT{ g.vx + 50, g.vy + 50 };
    OverlayState s2 = BuildOverlayState();
    Check(s2.hole.right - s2.hole.left == 200 && s2.hole.bottom - s2.hole.top == 150,
          "有窗口时仍优先亮该窗口（不被整屏兜底抢走）");
    g_wins.clear();
    g.phase = Phase::Select;
}

int main() {
    // HoleFootprint 会按虚拟屏裁剪，先把假屏尺寸摆上（① 组还没走 ResetSel）
    g.vx = 0; g.vy = 0; g.vw = 1920; g.vh = 1080;

    // ---------- ① 几何 ----------
    {
        RECT hs[8];
        RECT sel{ 100, 100, 300, 200 };
        BuildHandleRects(sel, hs);
        Check(Same(hs[0],  96,  96, 104, 104), "0 左上 = 8x8 居中于角");
        Check(Same(hs[1], 188,  96, 212, 104), "1 上中 = 沿边 24 / 垂直 8");
        Check(Same(hs[2], 296,  96, 304, 104), "2 右上");
        Check(Same(hs[3], 296, 138, 304, 162), "3 右中");
        Check(Same(hs[4], 296, 196, 304, 204), "4 右下");
        Check(Same(hs[5], 188, 196, 212, 204), "5 下中");
        Check(Same(hs[6],  96, 196, 104, 204), "6 左下");
        Check(Same(hs[7],  96, 138, 104, 162), "7 左中");
        // 覆盖性：把手必须整个落在 HoleFootprint 里 —— 这才是脏区依赖的真正的量。
        // 旧断言写的是"把手最远只伸到 ±4，外扩 kHandleH=8 一定罩得住"，那个前提
        // 在"选区比边中点把手还窄"时是错的：24px 长的上中把手会横向甩出洞外 12px
        // （实测选区 4px 宽时把手是 (329,48)-(353,56)，而旧足迹右边界只有 351）。
        bool inside = true;
        {
            OverlayState st;
            st.hole = sel;
            st.adjust = true;
            RECT fp = HoleFootprint(st);
            for (int i = 0; i < 8; ++i) {
                if (hs[i].left < fp.left || hs[i].top < fp.top ||
                    hs[i].right > fp.right || hs[i].bottom > fp.bottom)
                    inside = false;
            }
        }
        Check(inside, "八个把手全部落在 HoleFootprint 内");

        // 反例守护：窄选区必须也能罩住（这正是上面那条旧断言漏掉的情况）
        {
            RECT narrow{ 339, 52, 343, 131 };      // 4px 宽
            RECT hn[8];
            BuildHandleRects(narrow, hn);
            OverlayState st;
            st.hole = narrow;
            st.adjust = true;
            RECT fp = HoleFootprint(st);
            bool ok = true;
            for (int i = 0; i < 8; ++i)
                if (hn[i].left < fp.left || hn[i].right > fp.right ||
                    hn[i].top < fp.top  || hn[i].bottom > fp.bottom) ok = false;
            Check(ok, "4px 窄选区的把手也在足迹内（旧 8px 常数外扩在这里漏 2 列像素）");
        }
    }

    // ---------- ② 命中 ----------
    {
        ResetSel(100, 100, 300, 200);
        Check(HitHandle({ 100, 100 }) == 0, "命中左上 (0)");
        Check(HitHandle({ 300, 200 }) == 4, "命中右下 (4)");
        Check(HitHandle({ 200, 100 }) == 1, "命中上中 (1)");
        Check(HitHandle({ 300, 150 }) == 3, "命中右中 (3)");
        Check(HitHandle({ 200, 150 }) == -1, "选区中心不命中 (-1)");
        Check(HitHandle({ 1500, 900 }) == -1, "选区外不命中 (-1)");
        g.phase = Phase::Select;
        Check(HitHandle({ 100, 100 }) == -1, "选区态不命中把手");
    }

    // ---------- ③ 拖过对面：左上 → 越过右下 ----------
    {
        ResetSel(100, 100, 300, 200);
        g.dragHandle = 0;                 // 拖左上
        ApplyHandleDrag({ 400, 300 });     // 拖到原选区的右下方之外
        Check(Same(g.sel, 300, 200, 400, 300), "左上拖过右下：交换后选区正确");
        Check(g.dragHandle == 4, "左上拖过右下：索引重映射为右下 (4)");
        ApplyHandleDrag({ 350, 250 });     // 继续拖：应动右下角，光标始终贴着这个角
        Check(Same(g.sel, 300, 200, 350, 250), "越过之后继续拖：动的是右下角");
    }

    // ---------- ③ 拖过对面：右边 → 越过左边 ----------
    {
        ResetSel(100, 100, 300, 200);
        g.dragHandle = 3;                  // 拖右中
        ApplyHandleDrag({ 50, 150 });      // 拖到原左边界之外
        Check(Same(g.sel, 50, 100, 100, 200), "右中拖过左边：交换后选区正确");
        Check(g.dragHandle == 7, "右中拖过左边：索引重映射为左中 (7)");
        ApplyHandleDrag({ 80, 150 });
        Check(Same(g.sel, 80, 100, 100, 200), "越过之后继续拖：动的是左边");
    }

    // ---------- ③ 单轴越面：下中 → 越过上边（左右不动）----------
    {
        ResetSel(100, 100, 300, 200);
        g.dragHandle = 5;                  // 拖下中
        ApplyHandleDrag({ 200, 60 });      // 只纵向越过
        Check(Same(g.sel, 100, 60, 300, 100), "下中拖过上边：左右边界不动");
        Check(g.dragHandle == 1, "下中拖过上边：索引重映射为上中 (1)");
    }

    // ---------- ④ 钳回虚拟屏 ----------
    {
        ResetSel(100, 100, 300, 200);
        g.vx = 0; g.vy = 0; g.vw = 500; g.vh = 400;
        g.dragHandle = 4;                  // 拖右下
        ApplyHandleDrag({ 900, 900 });     // 拖到屏幕外
        Check(Same(g.sel, 100, 100, 500, 400), "拖出屏幕被钳回虚拟屏边界");
    }

    // ---------- ⑤ 把手绘制：alpha 不变量 + 不外溢 ----------
    // g.bits 是持久缓冲，且必须与 AC_SRC_ALPHA 配合：任何被画到的像素 alpha 必须
    // 是 255，否则那一块会变成半透明/全透明（"漏修补"是永久透明斑而不是闪一帧）。
    {
        const int VW = 400, VH = 300;
        static std::vector<unsigned char> buf((size_t)VW * VH * 4, 0);  // 初始 alpha=0
        g = Cap{};
        g.vx = 0; g.vy = 0; g.vw = VW; g.vh = VH;
        g.bits = buf.data();
        std::fill(buf.begin(), buf.end(), (unsigned char)0);

        OverlayState s;
        s.hole = { 100, 100, 300, 200 };
        s.adjust = true;
        PaintHandles({ 0, 0, VW, VH }, s);

        RECT hs[8];
        BuildHandleRects(s.hole, hs);
        bool allOpaque = true, touched = false;
        for (int i = 0; i < 8; ++i) {
            for (int y = hs[i].top; y < hs[i].bottom; ++y) {
                for (int x = hs[i].left; x < hs[i].right; ++x) {
                    if (x < 0 || y < 0 || x >= VW || y >= VH) continue;
                    touched = true;
                    if (buf[((size_t)y * VW + x) * 4 + 3] != 255) allOpaque = false;
                }
            }
        }
        Check(touched && allOpaque, "把手覆盖到的每个像素 alpha == 255");

        // 外溢检查：把手矩形之外必须一个字节都没动（否则会污染持久缓冲）
        size_t painted = 0;
        for (size_t i = 3; i < buf.size(); i += 4) if (buf[i] != 0) ++painted;
        size_t expect = 0;
        for (int i = 0; i < 8; ++i)
            expect += (size_t)(hs[i].right - hs[i].left) * (hs[i].bottom - hs[i].top);
        Check(painted == expect, "把手没有画到自己的矩形之外");
        g.bits = nullptr;
    }

    CheckHandleSnap();
    CheckHoverFallback();
    std::printf(g_fail ? "\n%d FAILED\n" : "\nall passed\n", g_fail);
    return g_fail ? 1 : 0;
}
