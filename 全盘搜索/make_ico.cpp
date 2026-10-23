// make_ico.cpp - generate multi-size .ico from a jpg/png via GDI+
// usage: make_ico.exe <input.jpg> <output.ico>
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <gdiplus.h>
#include <shlwapi.h>
#include <cstdio>
#include <vector>
#include <cstdint>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "user32.lib")

using namespace Gdiplus;

static bool WriteIconSize(HBITMAP hbm, int size, FILE* f) {
    BITMAP bm{};
    GetObject(hbm, sizeof(bm), &bm);
    // force 32bpp top-down conversion via GetDIBits
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<uint8_t> px(size * size * 4);
    HDC hdc = GetDC(nullptr);
    int got = GetDIBits(hdc, hbm, 0, size, px.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, hdc);
    if (!got) return false;

    // ICO entry: BITMAPINFOHEADER with height*2, bottom-up BGRA + AND mask
    BITMAPINFOHEADER hdr{};
    hdr.biSize = sizeof(BITMAPINFOHEADER);
    hdr.biWidth = size;
    hdr.biHeight = size * 2;
    hdr.biPlanes = 1;
    hdr.biBitCount = 32;
    hdr.biCompression = BI_RGB;
    hdr.biSizeImage = (DWORD)(px.size());

    // convert top-down RGBA (GDI gives BGRA already in px) to bottom-up
    std::vector<uint8_t> flip(px.size());
    int rowBytes = size * 4;
    for (int y = 0; y < size; y++)
        memcpy(&flip[(size - 1 - y) * rowBytes], &px[y * rowBytes], rowBytes);

    uint32_t maskRow = ((size + 31) / 32) * 4;
    std::vector<uint8_t> mask(maskRow * size, 0);

    ICONDIRENTRY e{};
    e.bWidth = (BYTE)(size >= 256 ? 0 : size);
    e.bHeight = (BYTE)(size >= 256 ? 0 : size);
    e.bColorCount = 0;
    e.bReserved = 0;
    e.wPlanes = 1;
    e.wBitCount = 32;
    uint32_t dataLen = sizeof(hdr) + (uint32_t)flip.size() + (uint32_t)mask.size();
    e.dwBytesInRes = dataLen;
    e.dwImageOffset = 6 + 16 * 6; // 6 sizes

    fwrite(&e, sizeof(e), 1, f);
    fwrite(&hdr, sizeof(hdr), 1, f);
    fwrite(flip.data(), 1, flip.size(), f);
    fwrite(mask.data(), 1, mask.size(), f);
    return true;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) { wprintf(L"usage: make_ico <in> <out>\n"); return 1; }
    GdiplusStartupInput gsi;
    ULONG_PTR tok;
    GdiplusStartup(&tok, &gsi, nullptr);
    int ret = 1;
    do {
        Bitmap src(argv[1], FALSE);
        if (src.GetLastStatus() != Ok) { wprintf(L"load fail %d\n", src.GetLastStatus()); break; }
        int sizes[6] = { 256, 128, 64, 48, 32, 16 };
        FILE* f = nullptr;
        _wfopen_s(&f, argv[2], L"wb");
        if (!f) break;
        // header first (fixed offsets for 6 entries)
        WORD w; DWORD d;
        w = 0; fwrite(&w, 2, 1, f);
        w = 1; fwrite(&w, 2, 1, f);
        w = 6; fwrite(&w, 2, 1, f);
        long dataStart = 6 + 16 * 6;
        std::vector<std::vector<uint8_t>> blobs(6);
        std::vector<BITMAPINFOHEADER> hdrs(6);
        std::vector<ICONDIRENTRY> ents(6);
        bool ok = true;
        for (int i = 0; i < 6 && ok; i++) {
            int s = sizes[i];
            Bitmap bmp(s, s, PixelFormat32bppARGB);
            Graphics g(&bmp);
            g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            g.SetSmoothingMode(SmoothingModeHighQuality);
            // draw with transparent bg
            if (g.DrawImage(&src, 0, 0, s, s) != Ok) { ok = false; break; }
            HBITMAP hbm = nullptr;
            bmp.GetHBITMAP(Color(0, 0, 0, 0), &hbm);
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = s;
            bi.bmiHeader.biHeight = -s;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            std::vector<uint8_t> px(s * s * 4);
            HDC hdc = GetDC(nullptr);
            ok = GetDIBits(hdc, hbm, 0, s, px.data(), &bi, DIB_RGB_COLORS) != 0;
            ReleaseDC(nullptr, hdc);
            DeleteObject(hbm);
            if (!ok) break;
            BITMAPINFOHEADER hdr{};
            hdr.biSize = sizeof(BITMAPINFOHEADER);
            hdr.biWidth = s; hdr.biHeight = s * 2; hdr.biPlanes = 1;
            hdr.biBitCount = 32; hdr.biCompression = BI_RGB;
            hdr.biSizeImage = (DWORD)px.size();
            std::vector<uint8_t> flip(px.size());
            int rowB = s * 4;
            for (int y = 0; y < s; y++)
                memcpy(&flip[(s - 1 - y) * rowB], &px[y * rowB], rowB);
            uint32_t maskRow = ((s + 31) / 32) * 4;
            std::vector<uint8_t> mask(maskRow * s, 0);
            ICONDIRENTRY e{};
            e.bWidth = (BYTE)(s >= 256 ? 0 : s);
            e.bHeight = (BYTE)(s >= 256 ? 0 : s);
            e.bColorCount = 0; e.bReserved = 0;
            e.wPlanes = 1; e.wBitCount = 32;
            e.dwBytesInRes = sizeof(hdr) + (DWORD)flip.size() + (DWORD)mask.size();
            e.dwImageOffset = 0; // fix later
            hdrs[i] = hdr; ents[i] = e;
            blobs[i] = std::move(flip);
            blobs[i].insert(blobs[i].end(), mask.begin(), mask.end());
        }
        if (ok) {
            // write entries with correct offsets
            long off = dataStart;
            for (int i = 0; i < 6; i++) { ents[i].dwImageOffset = off; off += (long)blobs[i].size(); }
            for (int i = 0; i < 6; i++) fwrite(&ents[i], 16, 1, f);
            for (int i = 0; i < 6; i++) {
                fwrite(&hdrs[i], sizeof(BITMAPINFOHEADER), 1, f);
                fwrite(blobs[i].data(), 1, blobs[i].size(), f);
            }
            ret = 0;
        }
        fclose(f);
    } while (0);
    GdiplusShutdown(tok);
    return ret;
}
