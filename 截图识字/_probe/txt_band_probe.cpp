// txt_band_probe.cpp —— 文字拾取高亮的取证探针（不是回归测试，是"出图给人看"）
//
// 验什么：
//   ① 同一行里高矮不一的词框，刷出来必须是**等高**的一条带（用户原话：
//      「蓝色示意图大大小小的，不统一」）；
//   ② 带内文字像素必须**一个字节都没动**（旧实现是只替换背景像素）；
//   ③ 暗色主题（深底浅字）按相反的判据工作，不能把字擦掉。
//
// 直接 #include 产品源、调产品的 PaintTextLayer，不重抄它的上色逻辑——
// 抄一遍就是用自己对产品的理解验收产品（这条是本项目踩过的坑）。
#include "../src/capture.cpp"

const wchar_t* ShotIniPath() { return L""; }
bool CopyImageToClipboard(const CaptureImage&) { return false; }
bool ShotOcrAndCopy(const CaptureImage&) { return false; }
bool ShotCopyImageThenText(const CaptureImage&, const std::string&) { return false; }
bool OcrEnsureEngine() { return false; }
std::vector<OcrWord> OcrImage(const CaptureImage&) { return {}; }
std::string FormatSelection(const std::vector<OcrWord>&, NewlineMode) { return {}; }

#include <cstdio>
#include <cstring>

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what);
    if (!ok) ++g_fail;
}

static const int W = 460, H = 150;
static std::vector<unsigned char> g_buf;   // 当前画面（BGRA）
static std::vector<unsigned char> g_orig;  // 上色前的原始画面，用来逐像素对照

static void Fill(int x0, int y0, int x1, int y1, unsigned r, unsigned gg, unsigned b) {
    for (int y = y0; y < y1; ++y) {
        if (y < 0 || y >= H) continue;
        unsigned char* p = &g_buf[((size_t)y * W) * 4];
        for (int x = x0; x < x1; ++x) {
            if (x < 0 || x >= W) continue;
            p[x * 4 + 0] = (unsigned char)b;
            p[x * 4 + 1] = (unsigned char)gg;
            p[x * 4 + 2] = (unsigned char)r;
            p[x * 4 + 3] = 255;
        }
    }
}

static bool SaveBmp(const char* path) {
    const int rowBytes = W * 3;
    const int pad = (4 - (rowBytes % 4)) % 4;
    const int stride = rowBytes + pad;
    const int dataSize = stride * H;
    const int fileSize = 54 + dataSize;
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    unsigned char hdr[54] = { 0 };
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(&hdr[2], &fileSize, 4);
    int off = 54; memcpy(&hdr[10], &off, 4);
    int hs = 40;  memcpy(&hdr[14], &hs, 4);
    memcpy(&hdr[18], &W, 4);
    memcpy(&hdr[22], &H, 4);
    short planes = 1, bpp = 24;
    memcpy(&hdr[26], &planes, 2);
    memcpy(&hdr[28], &bpp, 2);
    memcpy(&hdr[34], &dataSize, 4);
    fwrite(hdr, 1, 54, f);
    std::vector<unsigned char> row(stride, 0);
    for (int y = H - 1; y >= 0; --y) {           // BMP 自下而上
        const unsigned char* src = &g_buf[((size_t)y * W) * 4];
        for (int x = 0; x < W; ++x) {
            row[x * 3 + 0] = src[x * 4 + 0];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 2];
        }
        fwrite(row.data(), 1, stride, f);
    }
    fclose(f);
    return true;
}

// 某一竖线上被换成高亮色的像素 y 范围（用来量条带高度是否一致）
// 两种高亮色都要认：未选中 (214,230,250) 与选中 (170,205,245)。
// 判据用"偏蓝"而不是写死某个通道 —— 上一版只认 R<200，把未选中色漏了，
// 结果探针自己报了假 FAIL。
static bool IsHi(const unsigned char* p) {
    return p[0] >= 240 && (int)p[0] - (int)p[2] >= 20;   // BGRA：B 高且明显高于 R
}
static bool IsHiSel(const unsigned char* p) {
    return p[0] >= 240 && (int)p[0] - (int)p[2] >= 60;   // 更深的那档（选中）
}
static bool HighlightSpan(int x, int y0, int y1, int& top, int& bot) {
    top = bot = -1;
    for (int y = y0; y < y1; ++y) {
        const unsigned char* p = &g_buf[((size_t)y * W + x) * 4];
        if (IsHi(p)) { if (top < 0) top = y; bot = y + 1; }
    }
    return top >= 0;
}

static void RunCase(bool dark) {
    const unsigned bgR = dark ? 32 : 255, bgG = dark ? 33 : 255, bgB = dark ? 36 : 255;
    const unsigned fgR = dark ? 235 : 0, fgG = dark ? 238 : 0, fgB = dark ? 242 : 0;
    Fill(0, 0, W, H, bgR, bgG, bgB);
    // 两行"文字"：白底/深底上一堆竖条模拟字形笔画；两行笔画高度故意不同
    for (int x = 20; x < W - 20; x += 9) {
        Fill(x, 21, x + 4, 39, fgR, fgG, fgB);            // 第一行笔画（高 18）
        Fill(x, 81, x + 4, 95, fgR, fgG, fgB);            // 第二行笔画（高 14）
    }
    g_orig = g_buf;

    OverlayState s;
    s.hole = RECT{ 0, 0, W, H };
    s.textHasFocus = true;
    s.textRect = RECT{ 14, 14, W - 14, 104 };
    // 词框故意高矮/上下都不一致 —— 这正是用户抱怨的「大大小小」
    struct Box { int x, y, w, h; };
    const Box boxes[] = {
        { 18, 18, 60, 24 }, { 82, 16, 55, 26 }, { 141, 20, 48, 22 }, { 193, 19, 70, 25 },
        { 18, 78, 64, 19 }, { 86, 76, 50, 21 }, { 140, 79, 62, 18 },
    };
    for (const Box& b : boxes) {
        OcrWord wd;
        wd.x = b.x; wd.y = b.y; wd.w = b.w; wd.h = b.h;
        s.words.push_back(wd);
    }
    s.wordSel.assign(s.words.size(), 0);
    s.wordSel[1] = 1;      // 第一行中间那个词选中 → 整条带应走选中色

    PaintTextLayer(RECT{ 0, 0, W, H }, s);

    // ---- 判据 1：同一行内不同词框 → 同一条等高带 ----
    int t1 = 0, b1 = 0, t2 = 0, b2 = 0, t5 = 0, b5 = 0;
    const bool h1 = HighlightSpan(30, 8, 60, t1, b1);     // 词 0 内部
    const bool h2 = HighlightSpan(100, 8, 60, t2, b2);    // 词 1 内部（跨词界）
    const bool h5 = HighlightSpan(150, 8, 60, t5, b5);    // 词 2 内部
    Check(h1 && h2 && h5, "第一行的词都刷上了高亮");
    Check(t1 == t2 && b1 == b2 && t1 == t5 && b1 == b5,
          dark ? "暗底：同一行不同词框 → 同一条等高带"
               : "浅底：同一行不同词框 → 同一条等高带");
    printf("      第一行条带 y = %d..%d（高 %d）\n", t1, b1, b1 - t1);

    int t3 = 0, b3 = 0, t4 = 0, b4 = 0;
    const bool h3 = HighlightSpan(30, 68, 110, t3, b3);
    const bool h4 = HighlightSpan(160, 68, 110, t4, b4);
    Check(h3 && h4 && t3 == t4 && b3 == b4, "第二行也是等高条带");
    printf("      第二行条带 y = %d..%d（高 %d）\n", t3, b3, b3 - t3);

    // ---- 判据 2：原本是字色的像素，一个字节都没被改 ----
    int textPixels = 0, touched = 0;
    for (int i = 0; i < W * H; ++i) {
        const unsigned char* o = &g_orig[(size_t)i * 4];
        const unsigned char* n = &g_buf[(size_t)i * 4];
        if (o[2] == fgR && o[1] == fgG && o[0] == fgB) {
            ++textPixels;
            if (n[2] != o[2] || n[1] != o[1] || n[0] != o[0]) ++touched;
        }
    }
    printf("      文字像素 %d，被改动 %d\n", textPixels, touched);
    Check(textPixels > 500 && touched == 0, "文字像素零改动（只替换背景像素）");

    // ---- 判据 3：背景确实被替换成高亮色了（不能"什么都没变"也算过）----
    const unsigned char* c = &g_buf[((size_t)40 * W + 200) * 4];
    printf("      带内背景取样 (200,40) = (%d,%d,%d)\n", c[2], c[1], c[0]);
    Check(IsHi(c), "带内背景确实换成了高亮色");

    // ---- 判据 3：选中态必须仍能按"词"看出来 ----
    // 反例（旧实现）：行带里任一词选中 → 整条带都变深，用户看不出点的是哪个词。
    {
        const int ySample = 18;      // 第一行带的上沿（在字形之上，必是背景）
        const unsigned char* pSel = &g_buf[((size_t)ySample * W + 110) * 4]; // 词 1（选中）
        const unsigned char* pNor = &g_buf[((size_t)ySample * W + 40) * 4];  // 词 0（未选中）
        printf("      选中词取样 = (%d,%d,%d)，未选中词取样 = (%d,%d,%d)\n",
               pSel[2], pSel[1], pSel[0], pNor[2], pNor[1], pNor[0]);
        Check(IsHiSel(pSel) && IsHi(pNor) && !IsHiSel(pNor),
              "选中态仍按词区分（整行不会一起变深）");
    }

    // ---- 判据 4：alpha 必须仍是 255 ----
    int badAlpha = 0;
    for (int i = 0; i < W * H; ++i) if (g_buf[(size_t)i * 4 + 3] != 255) ++badAlpha;
    Check(badAlpha == 0, "alpha 全为 255（遮罩层不变量）");

    char path[256];
    snprintf(path, sizeof(path), "_out_txt_%s.bmp", dark ? "dark" : "light");
    if (SaveBmp(path)) printf("      已存图 %s\n", path);
}

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g.vx = 0; g.vy = 0; g.vw = W; g.vh = H;
    g_buf.assign((size_t)W * H * 4, 255);
    g.bits = g_buf.data();
    printf("=== 文字拾取高亮：按行等高条带 ===\n-- 浅底 --\n");
    RunCase(false);
    printf("-- 暗色主题 --\n");
    RunCase(true);
    printf("\n%s (failures=%d)\n", g_fail ? "FAIL" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
