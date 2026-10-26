// ocr_time_probe.cpp —— 量 OCR 的真实耗时，用来回答"预识别/增量识别能提速多少"
//
// 为什么要量而不是猜：结论直接决定要不要把"整选区预识别"接进产品，
// 而单次识别耗时随面积变化、引擎首次 Init 又另算，两者量级都不可忽略。
//
// 做法：GDI 画一张白底黑字的"假截图"，裁成几种典型尺寸，分别计时
//   ① OcrEnsureEngine（冷启动，只测第一次）
//   ② OcrImage（同一张图连测 3 次，取第 2、3 次当"热"值）
#include "../src/ocraddon.h"          // ocraddon_set_engine / set_unload_after_use
#include "../src/ocraddon_internal.h"  // CaptureImage / OcrWord / OcrImage / OcrEnsureEngine
#include <windows.h>
#include <cstdio>
#include <vector>

// ocraddon.cpp 为"极速启触发"那条链路还引了这几个产品侧符号；本探针只用到
// 它里面的 set_engine / set_unload_after_use，其余给空实现满足链接即可。
CaptureImage CaptureRegionInteractive() { return CaptureImage{}; }
void ShowResultAndCopy(CaptureImage) {}
void SetLineMode(NewlineMode) {}
void SetExitMode(int) {}
bool CopyImageToClipboard(const CaptureImage&) { return false; }
bool ShotAlreadyCopied() { return false; }

static LARGE_INTEGER g_freq;
static double NowMs() {
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)g_freq.QuadPart;
}

// 把一段中文渲染进 RGB 缓冲（top-down，每像素 3 字节，无行填充）
static void RenderText(CaptureImage& img, int w, int h, int fontSize) {
    img.width = w; img.height = h;
    img.rgb.assign((size_t)w * h * 3, 255);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;      // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC scr = GetDC(NULL);
    HBITMAP hb = CreateDIBSection(scr, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    HDC mem = CreateCompatibleDC(scr);
    HGDIOBJ ob = SelectObject(mem, hb);
    HBRUSH wb = CreateSolidBrush(RGB(255, 255, 255));
    RECT rc{ 0, 0, w, h };
    FillRect(mem, &rc, wb);
    DeleteObject(wb);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(0, 0, 0));
    HFONT f = CreateFontW(-fontSize, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                          DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
    HGDIOBJ of = SelectObject(mem, f);
    const wchar_t* lines[] = {
        L"极速启工具集 截图识字 速度测试 1234",
        L"The quick brown fox jumps over 5678",
        L"框选文字识别 预扫描 缓存 命中率",
        L"第二段中文内容 用来凑出行数 9012",
    };
    int y = 8;
    for (const wchar_t* s : lines) {
        if (y + fontSize > h) break;
        TextOutW(mem, 10, y, s, (int)wcslen(s));
        y += fontSize + 8;
    }
    SelectObject(mem, of);
    DeleteObject(f);
    GdiFlush();
    const DWORD* px = (const DWORD*)bits;
    for (int yy = 0; yy < h; ++yy) {
        for (int xx = 0; xx < w; ++xx) {
            DWORD v = px[(size_t)yy * w + xx];
            unsigned char* d = &img.rgb[((size_t)yy * w + xx) * 3];
            d[0] = (unsigned char)(v & 0xFF);
            d[1] = (unsigned char)((v >> 8) & 0xFF);
            d[2] = (unsigned char)((v >> 16) & 0xFF);
        }
    }
    SelectObject(mem, ob);
    DeleteObject(hb);
    DeleteDC(mem);
    ReleaseDC(NULL, scr);
}

struct Case { int w, h, fs; const char* label; };

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    QueryPerformanceFrequency(&g_freq);

    // tessdata 从部署目录取（仓库里没有语言包）
    const wchar_t* data = L"C:\\Users\\Administrator\\Desktop\\极速启\\screenshot\\tessdata";
    ocraddon_set_engine(data, L"chi_sim+eng");
    ocraddon_set_unload_after_use(0);      // 测量期间不要用完就卸

    printf("=== OCR 耗时实测（chi_sim+eng）===\n");
    double t0 = NowMs();
    const bool ok = OcrEnsureEngine();
    double tInit = NowMs() - t0;
    printf("引擎冷启动 Init        : %.0f ms  (%s)\n", tInit, ok ? "ok" : "FAIL");
    if (!ok) return 1;

    const Case cases[] = {
        { 300, 150, 16, "子框 300x150（小段文字）" },
        { 700, 400, 18, "选区 700x400（典型）" },
        { 1400, 800, 18, "选区 1400x800（大）" },
        { 1920, 1080, 18, "全屏 1920x1080" },
    };
    for (const Case& c : cases) {
        CaptureImage img;
        RenderText(img, c.w, c.h, c.fs);
        double best = 1e9, worst = 0, sum = 0;
        int words = 0;
        const int N = 3;
        for (int i = 0; i < N; ++i) {
            double s = NowMs();
            std::vector<OcrWord> ws = OcrImage(img);
            double ms = NowMs() - s;
            if (i == 0) words = (int)ws.size();
            if (ms < best) best = ms;
            if (ms > worst) worst = ms;
            sum += ms;
        }
        printf("%-22s: 最快 %6.0f ms  平均 %6.0f ms  最慢 %6.0f ms  认出 %d 词\n",
               c.label, best, sum / N, worst, words);
    }

    // 面积换算：给"整选区预识别 vs 只识别子框"的取舍一个量化依据
    printf("\n结论用的换算（按上面典型值）：\n");
    printf("  子框识别 ≈ 上面第一行；整选区预识别 ≈ 第二行。\n");
    return 0;
}
