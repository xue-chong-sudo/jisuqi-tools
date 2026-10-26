// tb_shot.cpp —— 把工具栏窗口"真身"抠出来存成图，给人在终端里看
//
// 为什么不 PrintWindow：实测 PW_RENDERFULLCONTENT 抓这个工具栏会整体 ×0.82
// 偏暗（229,232,236 → 188,191,195），颜色判断全错。
// 为什么不 GetDC(hwnd)：拿到的是旧帧（连抓三次同一张）。
// 只有 GetDC(NULL) 按窗口矩形抠才是屏幕上真实颜色。
#include <windows.h>
#include <cstdio>
#include <vector>
#include <cstring>

static const wchar_t* kHostClass = L"JisuQiShotHost";
static const wchar_t* kMaskClass = L"OCRCap";
static const wchar_t* kTbClass   = L"OCRCapTb";

static HWND FindByClass(const wchar_t* cls, DWORD pidFilter) {
    struct Ctx { const wchar_t* cls; HWND found; DWORD pid; } ctx{ cls, nullptr, pidFilter };
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        wchar_t buf[128] = { 0 };
        GetClassNameW(h, buf, 128);
        if (wcscmp(buf, c->cls) != 0) return TRUE;
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        if (c->pid && pid != c->pid) return TRUE;
        c->found = h; return FALSE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

static HWND WaitClass(const wchar_t* cls, DWORD pid, int ms) {
    for (int t = 0; t < ms; t += 20) {
        HWND h = FindByClass(cls, pid);
        if (h) return h;
        Sleep(20);
    }
    return nullptr;
}

// 抓屏幕上一块矩形（屏幕坐标），返回 top-down BGRA
struct Shot { int w = 0, h = 0; std::vector<unsigned char> px; };
static Shot Grab(int x, int y, int w, int h) {
    Shot s; s.w = w; s.h = h;
    s.px.assign((size_t)w * h * 4, 0);
    HDC scr = GetDC(NULL);
    HDC mem = CreateCompatibleDC(scr);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hb = CreateDIBSection(scr, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (hb && bits) {
        HGDIOBJ ob = SelectObject(mem, hb);
        BitBlt(mem, 0, 0, w, h, scr, x, y, SRCCOPY);
        memcpy(s.px.data(), bits, (size_t)w * h * 4);
        SelectObject(mem, ob);
    }
    if (hb) DeleteObject(hb);
    DeleteDC(mem);
    ReleaseDC(NULL, scr);
    return s;
}

static void Click(HWND tb, int cx, int cy) {
    SendMessage(tb, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(cx, cy));
    Sleep(40);
    SendMessage(tb, WM_LBUTTONUP, 0, MAKELPARAM(cx, cy));
    Sleep(120);
}

static bool SaveBmp3(const char* path, const Shot rows[3], int margin) {
    const int w = rows[0].w + margin * 2;
    const int rowH = rows[0].h + margin;
    const int h = rowH * 3;
    const int rowBytes = w * 3;
    const int pad = (4 - (rowBytes % 4)) % 4;
    const int stride = rowBytes + pad;
    const int dataSize = stride * h;
    const int fileSize = 54 + dataSize;
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    unsigned char hdr[54] = { 0 };
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(&hdr[2], &fileSize, 4);
    int off = 54; memcpy(&hdr[10], &off, 4);
    int hs = 40;  memcpy(&hdr[14], &hs, 4);
    memcpy(&hdr[18], &w, 4);
    memcpy(&hdr[22], &h, 4);
    short planes = 1, bpp = 24;
    memcpy(&hdr[26], &planes, 2);
    memcpy(&hdr[28], &bpp, 2);
    memcpy(&hdr[34], &dataSize, 4);
    fwrite(hdr, 1, 54, f);
    std::vector<unsigned char> canvas((size_t)w * h * 3, 40);   // 深灰底，白条看得清
    for (int r = 0; r < 3; ++r) {
        const int oy = r * rowH + margin / 2;
        for (int y = 0; y < rows[r].h; ++y) {
            for (int x = 0; x < rows[r].w; ++x) {
                const unsigned char* p = &rows[r].px[((size_t)y * rows[r].w + x) * 4];
                unsigned char* d = &canvas[((size_t)(oy + y) * w + margin + x) * 3];
                d[0] = p[0]; d[1] = p[1]; d[2] = p[2];
            }
        }
    }
    std::vector<unsigned char> row(stride, 0);
    for (int y = h - 1; y >= 0; --y) {
        const unsigned char* src = &canvas[((size_t)y * w) * 3];
        for (int x = 0; x < w; ++x) {
            row[x * 3 + 0] = src[x * 3 + 0];
            row[x * 3 + 1] = src[x * 3 + 1];
            row[x * 3 + 2] = src[x * 3 + 2];
        }
        fwrite(row.data(), 1, stride, f);
    }
    fclose(f);
    return true;
}

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HWND host = FindByClass(kHostClass, 0);
    if (!host) { printf("[shot] 未发现宿主\n"); return 2; }
    DWORD pid = 0; GetWindowThreadProcessId(host, &pid);

    UINT trig = RegisterWindowMessageW(L"JisuQiShotTrigger");
    PostMessageW(host, trig, 0, 0);
    HWND mask = WaitClass(kMaskClass, pid, 5000);
    if (!mask) { printf("[shot] 遮罩没出现\n"); return 1; }
    Sleep(200);
    // 拖出一个选区，进入调整态才会出工具栏。
    // 坐标按虚拟屏算，减掉遮罩客户区原点再发（多屏/负坐标时窗口原点不是 0）。
    POINT org{ 0, 0 };
    ClientToScreen(mask, &org);
    auto Drag = [&](UINT msg, WPARAM btn, int x, int y) {
        SendMessage(mask, msg, btn, MAKELPARAM(x - org.x, y - org.y));
    };
    Drag(WM_LBUTTONDOWN, MK_LBUTTON, 300, 250);
    Sleep(60);
    Drag(WM_MOUSEMOVE, MK_LBUTTON, 700, 560);
    Sleep(60);
    Drag(WM_LBUTTONUP, 0, 700, 560);
    HWND tb = WaitClass(kTbClass, pid, 3000);
    if (!tb) { printf("[shot] 工具栏没出现\n"); return 1; }
    Sleep(400);                       // 等它画完、上屏

    Shot rows[3];
    auto Capture = [&](int i) {
        RECT r; GetWindowRect(tb, &r);
        rows[i] = Grab(r.left, r.top, r.right - r.left, r.bottom - r.top);
        printf("[shot] 行%d  %dx%d\n", i + 1, rows[i].w, rows[i].h);
    };
    Capture(0);                       // ① 静息：此时已有默认工具，只作基线
    Click(tb, 29, 24);                // ② 点「矩形」
    Capture(1);
    Click(tb, 596, 24);               // ③ 点最粗那档（圆心 503+31*3=596）
    Capture(2);

    if (SaveBmp3("_out_tb2.bmp", rows, 8)) printf("[shot] 已存 _out_tb2.bmp\n");

    // 收尾：右键取消
    SendMessage(mask, WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(500, 400));
    SendMessage(mask, WM_RBUTTONUP, 0, MAKELPARAM(500, 400));
    return 0;
}
