// ocr_psm_probe.cpp —— 量不同 PageSegMode 的耗时与识别结果差异
//
// 背景：产品固定用 PSM_SPARSE_TEXT（对 UI 稀疏文字最准）。但单次识别 300ms 起，
// 而这个值很可能被页面分割吃掉了。这里在同一张图上逐个 PSM 计时，给"换 PSM
// 能不能提速"一个定量依据。为了只比 PSM，图片直接按产品放大后的尺寸（2x）渲染。
#include "../src/ocraddon.h"
#include "../src/ocraddon_internal.h"
#include <windows.h>
#include <tesseract/baseapi.h>
#include <cstdio>
#include <vector>
#include <string>

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

static void RenderText(CaptureImage& img, int w, int h, int fs) {
    img.width = w; img.height = h;
    img.rgb.assign((size_t)w * h * 3, 255);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
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
    HFONT f = CreateFontW(-fs, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                          DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
    HGDIOBJ of = SelectObject(mem, f);
    const wchar_t* lines[] = {
        L"极速启工具集 截图识字 速度测试 1234",
        L"The quick brown fox jumps over 5678",
        L"框选文字识别 预扫描 缓存 命中率",
        L"第二段中文内容 用来凑出行数 9012",
    };
    int y = 16;
    for (const wchar_t* s : lines) {
        if (y + fs > h) break;
        TextOutW(mem, 20, y, s, (int)wcslen(s));
        y += fs + 16;
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

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    QueryPerformanceFrequency(&g_freq);
    const char* dir = "C:\\Users\\Administrator\\Desktop\\极速启\\screenshot\\tessdata";

    // 产品里 700x400 会被放大 2x 再喂给引擎，所以这里直接渲 1400x800 等价
    CaptureImage img;
    RenderText(img, 1400, 800, 36);

    struct V { tesseract::PageSegMode psm; const char* name; };
    const V vs[] = {
        { tesseract::PSM_AUTO,         "PSM_AUTO (3)" },
        { tesseract::PSM_SINGLE_BLOCK, "PSM_SINGLE_BLOCK (6)" },
        { tesseract::PSM_SPARSE_TEXT,  "PSM_SPARSE_TEXT (11) 现用" },
        { tesseract::PSM_RAW_LINE,     "PSM_RAW_LINE (13)" },
    };
    printf("=== PageSegMode 对比（同图 1400x800，产品放大后尺寸）===\n");
    // tessdata 路径含中文，Tesseract 的 Init(datapath) 打不开非 ASCII 路径。
    // 产品里是靠"切到该目录 + Init(nullptr)"绕过的（见 ocr_tesseract.cpp 的
    // IsAllAscii 分支），探针照做。
    // 必须用 Wide 版 API：SetCurrentDirectoryA 会把 UTF-8 字节当 ACP 解释，静默失败。
    if (!SetCurrentDirectoryW(L"C:\\Users\\Administrator\\Desktop\\极速启\\screenshot\\tessdata")) {
        printf("SetCurrentDirectoryW FAIL %lu\n", GetLastError());
        return 2;
    }
    for (const V& v : vs) {
        tesseract::TessBaseAPI api;
        if (api.Init(nullptr, "chi_sim+eng", tesseract::OEM_LSTM_ONLY) != 0) {
            printf("%s : Init FAIL\n", v.name);
            continue;
        }
        api.SetPageSegMode(v.psm);
        api.SetSourceResolution(300);
        // 预热一次（第一次含内部缓存建立，不代表稳态）
        api.SetImage(img.rgb.data(), img.width, img.height, 3, img.width * 3);
        api.Recognize(nullptr);
        double best = 1e9, sum = 0;
        int words = 0;
        std::string sample;
        const int N = 3;
        for (int i = 0; i < N; ++i) {
            api.SetImage(img.rgb.data(), img.width, img.height, 3, img.width * 3);
            double s = NowMs();
            if (api.Recognize(nullptr) != 0) break;
            double ms = NowMs() - s;
            if (ms < best) best = ms;
            sum += ms;
            if (i == 0) {
                tesseract::ResultIterator* ri = api.GetIterator();
                if (ri) {
                    do {
                        char* t = ri->GetUTF8Text(tesseract::RIL_WORD);
                        if (t && *t) {
                            ++words;
                            if (sample.size() < 28) { sample += t; sample += ' '; }
                        }
                        delete[] t;
                    } while (ri->Next(tesseract::RIL_WORD));
                    delete ri;
                }
            }
        }
        printf("%-24s: 最快 %6.0f ms 平均 %6.0f ms  词 %3d  样例「%s」\n",
               v.name, best, sum / N, words, sample.c_str());
        api.End();
    }
    return 0;
}
