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

    std::printf(g_fail ? "\n%d FAILED\n" : "\nall passed\n", g_fail);
    return g_fail ? 1 : 0;
}
