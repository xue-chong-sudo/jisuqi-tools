// overlay_state_test.cpp —— OverlayState 的"隐藏输入"回归测试 + alpha 不变量断言
//
// 判据（比字段清单强，因为它可执行）：
//   SameOverlay(a,b) == true   ⟺  画 a 与画 b 产生逐字节相同的像素
// 只要存在"画家读了 OverlayState 之外的东西"（全局、静态、当前时间…），
// 这条测试就会挂。历史上它专门用来钉死两个已修的缺陷：
//   ① 标签文字原本存在全局 g_labelText、不参与 SameOverlay 比较
//      → 框被屏幕裁剪后不动、只有尺寸数字在跳的那一帧被判成"没变"
//      → 标签永远停在旧尺寸（"100 x 100" 拖到 101 宽仍显示 100）。
//   ② GDI 画字把字形像素 alpha 写成 0（实测 396/1800），AC_SRC_ALPHA 下
//      这些像素全透明 → 尺寸标签"文字镂空"透出桌面。
// 因此本测试同时断言：任意一帧结束后，整幅位图的 alpha 必须恒为 255。
//
// 用法：编译运行即可，退出码 0 = 全部通过。
//   直接 #include 真实 TU，测的是产品代码本身，不是副本。

#ifdef OST_BUGGY
#include "capture_buggy.cpp"   // 阴性对照：文字比较被移除的副本
#else
#include "../src/capture.cpp"
#endif

// capture.cpp 依赖的外部 TU 符号，测试里给空实现即可 —— 它们只出现在
// CaptureRegionInteractive 的退出路径上，不参与本测试覆盖的渲染/判等路径。
const wchar_t* ShotIniPath() { return L""; }              // main.cpp 提供
bool CopyImageToClipboard(const CaptureImage&) { return false; }   // view.cpp 提供
bool ShotOcrAndCopy(const CaptureImage&) { return false; }         // view.cpp 提供

#include <cstdio>
#include <random>
#include <algorithm>
#include <vector>

namespace {

const int VW = 360, VH = 240;          // 假屏尺寸（窗口坐标，vx=vy=0）
std::vector<unsigned char> bufA, bufB; // 两张待比对的目标缓冲
void* bitsA = nullptr;
void* bitsB = nullptr;
HDC   dcA = nullptr, dcB = nullptr;
HBITMAP dibA = nullptr, dibB = nullptr;

HDC MakeDib(int w, int h, void** bits, HBITMAP* outBmp) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;        // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC scr = GetDC(nullptr);
    HBITMAP hb = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, bits, nullptr, 0);
    HDC dc = CreateCompatibleDC(scr);
    ReleaseDC(nullptr, scr);
    SelectObject(dc, hb);
    *outBmp = hb;
    return dc;
}

// 按当前 Redraw 的 per-band 次序渲染一个完整状态（band = 整屏）
// 顺序必须与 Redraw 一致：RestoreFromDim → PaintHole → PaintAnnos → PaintBorder
// → PaintHandles → PaintLabel。少画一层，测试对这个维度就是瞎的。
void Render(const OverlayState& s, void* bits, HDC dc) {
    g.bits = bits;
    g.hDc = dc;
    const RECT full{0, 0, VW, VH};
    RestoreFromDim(full);
    PaintHole(full, s);
    PaintAnnos(full, s);
    PaintBorder(full, s);
    PaintHandles(full, s);
    PaintLabel(full, s);
}

// 不变量：整幅位图 alpha 恒 255（本项目没有合法半透明，也没有合法 alpha==0）
int CountAlphaViolations(const void* bits) {
    const BYTE* p = (const BYTE*)bits;
    int bad = 0;
    for (int i = 0; i < VW * VH; ++i) if (p[i * 4 + 3] != 255) ++bad;
    return bad;
}

struct Rng {
    std::mt19937 mt{12345};
    int in(int lo, int hi) { return (int)(mt() % (unsigned)(hi - lo + 1)) + lo; }
} rng;

RECT RandRect() {
    int x0 = rng.in(0, VW - 2), y0 = rng.in(0, VH - 2);
    int x1 = std::min(VW, x0 + rng.in(1, VW - x0));
    int y1 = std::min(VH, y0 + rng.in(1, VH - y0));
    return RECT{x0, y0, x1, y1};
}

const char* kTexts[] = {"300 x 200", "301 x 200", "1 x 1", "1024 x 768", "99 x 99"};

// 标注：窗口坐标下随便画一条（坐标系在测试里统一按 vx=vy=0 处理）
Anno RandAnno(const RECT& hole) {
    Anno a;
    a.tool = rng.in(AT_RECT, AT_PEN);
    a.color = rng.in(0, 5);
    a.widthIdx = rng.in(0, 3);
    int x0 = rng.in(hole.left, hole.right > hole.left ? hole.right : hole.left + 1);
    int y0 = rng.in(hole.top, hole.bottom > hole.top ? hole.bottom : hole.top + 1);
    int x1 = rng.in(hole.left, hole.right > hole.left ? hole.right : hole.left + 1);
    int y1 = rng.in(hole.top, hole.bottom > hole.top ? hole.bottom : hole.top + 1);
    a.pts.push_back(POINT{x0, y0});
    if (a.tool == AT_PEN) {
        int n = rng.in(1, 4);
        for (int i = 0; i < n; ++i)
            a.pts.push_back(POINT{rng.in(hole.left, hole.right > hole.left ? hole.right : hole.left + 1),
                                  rng.in(hole.top, hole.bottom > hole.top ? hole.bottom : hole.top + 1)});
    } else {
        a.pts.push_back(POINT{x1, y1});
    }
    return a;
}

// 与 BuildOverlayState 用**同一个** GrowAnnoBox，不在这里抄一遍：
// 抄一遍就等于"用我自己对足迹的理解去验收产品"，箭头两翼那种外扩一旦抄漏，
// 测试会在恰好该抓到的地方变绿。
void FillAnnBox(OverlayState& s) {
    RECT ab{ 0, 0, 0, 0 };
    for (const Anno& a : s.annos) GrowAnnoBox(ab, a);
    if (s.drawingAnn) GrowAnnoBox(ab, s.annCur);
    s.annBox = REmpty(ab) ? RECT{0, 0, 0, 0} : RIntersect(ab, s.hole);
}

OverlayState RandState() {
    OverlayState s;
    s.hole = RandRect();
    s.drag = (rng.in(0, 1) == 1);
    s.adjust = (rng.in(0, 3) == 0);
    s.labelValid = (rng.in(0, 4) != 0);
    if (s.labelValid) {
        int lw = rng.in(20, 90), lh = 17 + 8;
        int lx = (s.hole.left - 4 > 0) ? (s.hole.left - 4) : 0;
        int ly = (s.hole.top - 26 > 0) ? (s.hole.top - 26) : 0;
        s.label = RIntersect(RECT{lx, ly, lx + lw, ly + lh}, RECT{0, 0, VW, VH});
        s.labelValid = !REmpty(s.label);
    }
    std::strncpy(s.text, kTexts[rng.in(0, 4)], sizeof(s.text) - 1);
    s.textW = 60; s.textH = 17;
    // 0~3 条已完成标注，另有 1/3 概率带一条"正在画"
    int n = rng.in(0, 3);
    for (int i = 0; i < n; ++i) s.annos.push_back(RandAnno(s.hole));
    s.drawingAnn = (rng.in(0, 2) == 0);
    if (s.drawingAnn) s.annCur = RandAnno(s.hole);
    FillAnnBox(s);
    return s;
}

int g_fail = 0;
void Check(bool ok, const char* what) {
    if (!ok) { ++g_fail; printf("  [FAIL] %s\n", what); }
}

} // namespace

int main() {
    // ---- 搭一个最小可用的假环境（不走窗口、不走 ULW，只驱动画家）----
    g.vx = 0; g.vy = 0; g.vw = VW; g.vh = VH;
    g.screen.assign((size_t)VW * VH * 4, 0);
    g.dim.assign((size_t)VW * VH * 4, 0);
    for (int y = 0; y < VH; ++y) {
        for (int x = 0; x < VW; ++x) {
            BYTE* s = &g.screen[(size_t)(y * VW + x) * 4];
            s[0] = (BYTE)x; s[1] = (BYTE)y; s[2] = (BYTE)(x ^ y); s[3] = 255;
            BYTE* d = &g.dim[(size_t)(y * VW + x) * 4];
            d[0] = 40; d[1] = 40; d[2] = 40; d[3] = 255;   // 遮罩底
        }
    }
    dcA = MakeDib(VW, VH, &bitsA, &dibA);
    dcB = MakeDib(VW, VH, &bitsB, &dibB);

    // 触发一次 BuildOverlayState 以创建标签字体（画家要用）
    {
        g.drag = true;
        g.sel = RECT{10, 10, 120, 90};
        g.cur = POINT{60, 50};
        OverlayState warm = BuildOverlayState();
        (void)warm;
        g.drag = false;
    }

    printf("=== 1. 定向用例：文字变、几何不变（历史上那个「标签停在旧尺寸」）===\n");
    {
        OverlayState a{}, b{};
        // 框被"屏幕"右/下边界裁剪住，几何完全不变，只有尺寸数字在跳
        a.hole = RECT{VW - 120, 100, VW, 200};
        a.label = RECT{VW - 124, 74, VW - 40, 103};
        a.labelValid = true; a.drag = true;
        std::strncpy(a.text, "300 x 200", sizeof(a.text) - 1);
        a.textW = 60; a.textH = 17;
        b = a;
        std::strncpy(b.text, "301 x 200", sizeof(b.text) - 1);

        Check(!SameOverlay(a, b), "文字不同时必须判为「变了」（否则这一帧会被跳过、标签停在旧数字）");
        Render(a, bitsA, dcA);
        Render(b, bitsB, dcB);
        Check(std::memcmp(bitsA, bitsB, (size_t)VW * VH * 4) != 0,
              "文字不同则像素必须不同（证明画家确实依赖 text）");
        printf("  定向用例完成\n");
    }

    printf("=== 2. 随机用例：SameOverlay 判等 ⟺ 像素全等 ===\n");
    {
        const int N = 4000;
        int eqPairs = 0, neqPairs = 0, eqButDiff = 0, neqButSame = 0, alphaBad = 0;
        for (int i = 0; i < N; ++i) {
            OverlayState a = RandState();
            // b 有 50% 概率与 a 完全相同，50% 概率在某一个字段上做扰动
            OverlayState b = a;
            if (rng.in(0, 1)) {
                switch (rng.in(0, 6)) {
                    case 0: b.hole = RandRect(); FillAnnBox(b); break;
                    case 1: b.drag = !b.drag; break;
                    case 2: b.labelValid = !b.labelValid; break;
                    case 3: b.label = RandRect(); break;
                    case 4: b.adjust = !b.adjust; break;
                    case 5:   // 加/减一条标注
                        if (!b.annos.empty() && rng.in(0, 1)) b.annos.pop_back();
                        else b.annos.push_back(RandAnno(b.hole));
                        FillAnnBox(b);
                        break;
                    default: std::strncpy(b.text, kTexts[rng.in(0, 4)], sizeof(b.text) - 1); break;
                }
            }
            Render(a, bitsA, dcA);
            Render(b, bitsB, dcB);
            alphaBad += CountAlphaViolations(bitsA);
            const bool same = std::memcmp(bitsA, bitsB, (size_t)VW * VH * 4) == 0;
            if (SameOverlay(a, b)) {
                ++eqPairs;
                if (!same) ++eqButDiff;      // 严重：判等却画得不一样 → 存在隐藏输入
            } else {
                ++neqPairs;
                if (same) ++neqButSame;      // 仅提示：判不等但画得一样（比较字段偏多）
            }
        }
        printf("  判等对 %d，判不等对 %d\n", eqPairs, neqPairs);
        printf("  判等但像素不同 = %d   <-- 非 0 即存在隐藏输入\n", eqButDiff);
        printf("  判不等但像素相同 = %d  （提示性，非致命）\n", neqButSame);
        printf("  alpha!=255 的像素总数 = %d\n", alphaBad);
        Check(eqButDiff == 0, "存在判等但像素不同的用例");
        Check(alphaBad == 0, "存在 alpha != 255 的像素（GDI 破坏 alpha 或漏了闭合修补）");
    }

    // 第 3 节：这条测的是**脏区覆盖性**，与第 2 节管的是两件不同的事。
    //
    // 第 2 节断言"判等 ⟺ 像素相同"，它管"该不该画"；管不了"画在哪儿"。
    // 一个状态被判成"变了"之后，真正的 dirty rect 是 BuildDirtyBands 用两个
    // StateRegion 异或算出来的 —— 若像素差异落在异或选不中的地方（同一区域里
    // 内容变了），画布上就永远留着上一帧的残影。
    //
    // 这个缺陷在真实版本里出现过两次（都是实测抓的，不是推演）：
    //   ① 尺寸标签：数字变了但字宽不变 → 标签矩形一字不差 → 异或为空 → 整帧跳过
    //   ② 标注：矩形从大拖小 → 旧外框像素落在新旧框的交集里 → 异或选不出来
    // 判据：凡是渲染结果不同的像素，必须至少落在一条 band 里。
    printf("=== 3. 脏区覆盖性：每一处像素差异都必须被某个 band 覆盖 ===\n");
    {
        const int N = 1500;
        long long diffPixels = 0, uncovered = 0;
        int worst = 0;
        long long perKind[5] = {0, 0, 0, 0, 0};
        static const char* kKindName[5] = {
            "0 改 hole（选区/把手移动）", "1 改 text（尺寸标签）",
            "2 加一条标注", "3 正在画的标注在动", "4 撤销/删一条标注" };
        for (int i = 0; i < N; ++i) {
            OverlayState a = RandState();
            OverlayState b = a;
            // 只在"看起来会变"的维度上扰动，最接近真实交互
            const int kind = rng.in(0, 4);
            switch (kind) {
                case 0: b.hole = RandRect(); FillAnnBox(b); break;
                case 1: std::strncpy(b.text, kTexts[rng.in(0, 4)], sizeof(b.text) - 1); break;
                case 2: b.annos.push_back(RandAnno(b.hole)); FillAnnBox(b); break;
                case 3: b.annCur = RandAnno(b.hole); b.drawingAnn = true; FillAnnBox(b); break;
                default: if (!b.annos.empty()) { b.annos.pop_back(); FillAnnBox(b); } break;
            }
            Render(a, bitsA, dcA);
            Render(b, bitsB, dcB);

            RECT bands[kBandCap];
            const int n = BuildDirtyBands(a, b, DirtyExtra(a, b), bands, kBandCap);

            const BYTE* pa = (const BYTE*)bitsA;
            const BYTE* pb = (const BYTE*)bitsB;
            int bad = 0;
            for (int y = 0; y < VH; ++y) {
                for (int x = 0; x < VW; ++x) {
                    const size_t off = (size_t)(y * VW + x) * 4;
                    if (std::memcmp(pa + off, pb + off, 4) == 0) continue;
                    ++diffPixels;
                    bool cov = false;
                    for (int k = 0; k < n; ++k) {
                        if (x >= bands[k].left && x < bands[k].right &&
                            y >= bands[k].top  && y < bands[k].bottom) { cov = true; break; }
                    }
                    if (!cov) {
                        ++bad; ++uncovered;
                        if (bad <= 3)
                            printf("      漏 (x=%d,y=%d) a=(%d,%d,%d,%d) b=(%d,%d,%d,%d) adj=%d/%d a.hole=(%ld,%ld,%ld,%ld) b.hole=(%ld,%ld,%ld,%ld) a.lab=(%ld,%ld,%ld,%ld) b.lab=(%ld,%ld,%ld,%ld) n=%d\n",
                                   x, y,
                                   pa[off], pa[off+1], pa[off+2], pa[off+3],
                                   pb[off], pb[off+1], pb[off+2], pb[off+3],
                                   (int)a.adjust, (int)b.adjust,
                                   a.hole.left, a.hole.top, a.hole.right, a.hole.bottom,
                                   b.hole.left, b.hole.top, b.hole.right, b.hole.bottom,
                                   a.label.left, a.label.top, a.label.right, a.label.bottom,
                                   b.label.left, b.label.top, b.label.right, b.label.bottom, n);
                    }
                }
            }
            perKind[kind] += bad;
            worst = std::max(worst, bad);
        }
        printf("  差异像素总数 %lld，未被覆盖 %lld，单例最差 %d\n",
               diffPixels, uncovered, worst);
        for (int k = 0; k < 5; ++k)
            printf("    %s → 未覆盖 %lld\n", kKindName[k], perKind[k]);
        Check(uncovered == 0, "存在像素差异没有被任何脏区 band 覆盖（会留残影）");
    }

    printf("\n%s (failures=%d)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
