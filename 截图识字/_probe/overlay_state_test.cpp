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
void Render(const OverlayState& s, void* bits, HDC dc) {
    g.bits = bits;
    g.hDc = dc;
    const RECT full{0, 0, VW, VH};
    RestoreFromDim(full);
    PaintHole(full, s);
    PaintBorder(full, s);
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

OverlayState RandState() {
    OverlayState s;
    s.hole = RandRect();
    s.drag = (rng.in(0, 1) == 1);
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
                switch (rng.in(0, 4)) {
                    case 0: b.hole = RandRect(); break;
                    case 1: b.drag = !b.drag; break;
                    case 2: b.labelValid = !b.labelValid; break;
                    case 3: b.label = RandRect(); break;
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

    printf("\n%s (failures=%d)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
