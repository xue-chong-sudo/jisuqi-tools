// dbg_paint.cpp —— 单状态渲染定位：谁在 (351,51) 画了白色把手？
#include "../src/capture.cpp"

const wchar_t* ShotIniPath() { return L""; }
bool CopyImageToClipboard(const CaptureImage&) { return false; }
bool ShotOcrAndCopy(const CaptureImage&) { return false; }

#include <cstdio>
#include <vector>

static const int VW = 360, VH = 240;
static std::vector<unsigned char> buf;
static void* bits = nullptr;
static HDC dc = nullptr;
static HBITMAP dib = nullptr;

static void Render(const OverlayState& s) {
    g.bits = bits; g.hDc = dc;
    const RECT full{0, 0, VW, VH};
    RestoreFromDim(full);
    PaintHole(full, s);
    PaintAnnos(full, s);
    PaintBorder(full, s);
    PaintHandles(full, s);
    PaintLabel(full, s);
}

int main() {
    g.vx = 0; g.vy = 0; g.vw = VW; g.vh = VH;
    g.screen.assign((size_t)VW * VH * 4, 0);
    g.dim.assign((size_t)VW * VH * 4, 0);
    for (int i = 0; i < VW * VH; ++i) {
        g.screen[(size_t)i * 4 + 0] = 200;
        g.screen[(size_t)i * 4 + 1] = 200;
        g.screen[(size_t)i * 4 + 2] = 200;
        g.screen[(size_t)i * 4 + 3] = 255;
        g.dim[(size_t)i * 4 + 0] = 40;
        g.dim[(size_t)i * 4 + 1] = 40;
        g.dim[(size_t)i * 4 + 2] = 40;
        g.dim[(size_t)i * 4 + 3] = 255;
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = VW;
    bi.bmiHeader.biHeight = -VH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC scr = GetDC(nullptr);
    dib = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    dc = CreateCompatibleDC(scr);
    ReleaseDC(nullptr, scr);
    SelectObject(dc, dib);

    // 先建标签字体
    { g.drag = true; g.sel = RECT{10,10,120,90}; g.cur = POINT{60,50}; BuildOverlayState(); g.drag = false; }

    OverlayState s;
    s.hole = RECT{339, 52, 343, 131};
    s.adjust = true;
    s.drag = false;
    s.labelValid = true;
    s.label = RECT{335, 26, 360, 51};
    std::strncpy(s.text, "4 x 79", sizeof(s.text) - 1);
    s.textW = 60; s.textH = 17;

    Render(s);

    const BYTE* p = (const BYTE*)bits;
    printf("把手矩形（窗口坐标）:\n");
    RECT hs[8];
    BuildHandleRects(s.hole, hs);
    for (int i = 0; i < 8; ++i)
        printf("  [%d] (%ld,%ld)-(%ld,%ld)\n", i, hs[i].left, hs[i].top, hs[i].right, hs[i].bottom);

    printf("footprint = (%ld,%ld)-(%ld,%ld)\n",
           HoleFootprint(s).left, HoleFootprint(s).top,
           HoleFootprint(s).right, HoleFootprint(s).bottom);

    printf("\n(345..356, 48..54) 区域像素（R 通道）:\n     ");
    for (int x = 345; x <= 356; ++x) printf("%3d ", x);
    printf("\n");
    for (int y = 48; y <= 54; ++y) {
        printf("%3d: ", y);
        for (int x = 345; x <= 356; ++x) {
            const BYTE* q = p + (size_t)(y * VW + x) * 4;
            printf("%3d ", q[2]);
        }
        printf("\n");
    }
    return 0;
}
