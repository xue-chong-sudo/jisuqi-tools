// test_main.cpp - Screenshot OCR test build (uses real Tesseract OCR)
// Hotkey Ctrl+Shift+S -> light grey veil everywhere except the selected area.
// -> hover highlights the single window under the cursor (blue border, no veil inside)
// -> click to capture the highlighted window, or drag to select (red box, no veil inside)
// -> on release, run OCR and show the captured image with real text boxes
// -> drag over the red boxes to select text and copy it to clipboard
// Build: run build_test.bat at project root (auto-detects VS and Tesseract).
#include "ocraddon_internal.h"
#include <windows.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <algorithm>

// Save CaptureImage (RGB, top-left origin) as a 24-bit BMP file.
static bool SaveBmp(const CaptureImage& img, const char* path) {
    if (img.width <= 0 || img.height <= 0) return false;
    const int w = img.width, h = img.height;
    const int rowBytes = ((w * 3 + 3) / 4) * 4; // 4-byte align
    const int dataSize = rowBytes * h;

    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42; // 'BM'
    fh.bfSize = (DWORD)(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + dataSize);
    fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);

    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(BITMAPINFOHEADER);
    ih.biWidth = w;
    ih.biHeight = h; // positive => bottom-up
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biCompression = BI_RGB;

    std::vector<unsigned char> buf((size_t)dataSize, 0);
    for (int y = 0; y < h; y++) {
        const unsigned char* src = &img.rgb[(size_t)(h - 1 - y) * w * 3]; // flip vertically
        unsigned char* dst = &buf[(size_t)y * rowBytes];
        for (int x = 0; x < w; x++) {
            dst[x * 3 + 0] = src[x * 3 + 2]; // B
            dst[x * 3 + 1] = src[x * 3 + 1]; // G
            dst[x * 3 + 2] = src[x * 3 + 0]; // R
        }
    }

    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fwrite(&fh, 1, sizeof(fh), f);
    fwrite(&ih, 1, sizeof(ih), f);
    fwrite(buf.data(), 1, (size_t)dataSize, f);
    fclose(f);
    return true;
}

// Copy the selected region to the clipboard as an image (CF_DIB, 32-bit BGRA).
static bool CopyToClipboard(const CaptureImage& img) {
    if (img.width <= 0 || img.height <= 0) return false;
    const int w = img.width, h = img.height;

    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(BITMAPINFOHEADER);
    ih.biWidth = w;
    ih.biHeight = h;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;

    const size_t pixSize = (size_t)w * h * 4;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + pixSize);
    if (!hMem) return false;

    unsigned char* base = (unsigned char*)GlobalLock(hMem);
    memcpy(base, &ih, sizeof(ih));
    unsigned char* pix = base + sizeof(BITMAPINFOHEADER);
    for (int y = 0; y < h; y++) {
        const unsigned char* src = &img.rgb[(size_t)(h - 1 - y) * w * 3];
        unsigned char* dst = &pix[(size_t)y * w * 4];
        for (int x = 0; x < w; x++) {
            dst[x * 4 + 0] = src[x * 3 + 2];
            dst[x * 4 + 1] = src[x * 3 + 1];
            dst[x * 4 + 2] = src[x * 3 + 0];
            dst[x * 4 + 3] = 0xFF;
        }
    }
    GlobalUnlock(hMem);

    if (!OpenClipboard(NULL)) { GlobalFree(hMem); return false; }
    EmptyClipboard();
    SetClipboardData(CF_DIB, hMem); // clipboard takes ownership
    CloseClipboard();
    return true;
}

// Find the tessdata directory at runtime.
// Order: 1) tessdata/ next to exe, 2) TESSDATA_PREFIX env var,
// 3) common Tesseract install locations.
static std::wstring FindTessdata() {
    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(NULL, exe, MAX_PATH) > 0) {
        wchar_t* slash = wcsrchr(exe, L'\\');
        if (slash) {
            std::wstring p(exe, slash - exe + 1);
            p += L"tessdata";
            if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
        }
    }
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"TESSDATA_PREFIX", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::wstring p(buf);
        if (!p.empty() && p.back() == L'\\') p.pop_back();
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    }
    const wchar_t* candidates[] = {
        L"C:\\Program Files\\Tesseract-OCR\\tessdata",
        L"C:\\Program Files (x86)\\Tesseract-OCR\\tessdata",
        L"D:\\Tesseract-OCR\\tessdata",
        L"D:\\Program Files\\Tesseract-OCR\\tessdata",
    };
    for (const wchar_t* c : candidates) {
        if (GetFileAttributesW(c) != INVALID_FILE_ATTRIBUTES) return c;
    }
    return L"";
}

static LRESULT CALLBACK HotProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_HOTKEY) {
        CaptureImage img = CaptureRegionInteractive();
        if (img.width > 0) {
            char path[MAX_PATH] = "capture_region.bmp";
            if (GetModuleFileNameA(NULL, path, MAX_PATH) > 0) {
                char* slash = strrchr(path, '\\');
                if (slash) strcpy(slash + 1, "capture_region.bmp");
            }
            bool saved = SaveBmp(img, path);
            bool copied = CopyToClipboard(img);
            printf("[OK] region %dx%d  saved=%d  copied=%d\n", img.width, img.height, (int)saved, (int)copied);
            // Show the image immediately; OCR runs in the background thread.
            ShowResultAndCopy(std::move(img));
        } else {
            printf("[cancel] user cancelled or no valid region selected\n");
        }
    } else if (m == WM_DESTROY) {
        PostQuitMessage(0);
    }
    return DefWindowProc(h, m, w, l);
}

int main() {
    std::wstring tessdata = FindTessdata();
    if (tessdata.empty()) {
        printf("[ERROR] tessdata not found.\n");
        printf("  Please install Tesseract and put chi_sim.traineddata + eng.traineddata\n");
        printf("  in a 'tessdata' folder next to this exe, or set TESSDATA_PREFIX env var.\n");
        return 1;
    }
    if (!OcrInit(tessdata.c_str(), L"chi_sim+eng")) {
        printf("[ERROR] OCR init failed. Check tessdata contains chi_sim.traineddata and eng.traineddata.\n");
        return 1;
    }
    printf("[OK] OCR initialized. tessdata=%S\n", tessdata.c_str());

    WNDCLASSW wc{};
    wc.lpfnWndProc = HotProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"OCRHot";
    RegisterClassW(&wc);
    HWND hw = CreateWindowExW(0, L"OCRHot", NULL, 0, 0, 0, 0, 0, NULL, NULL, GetModuleHandle(NULL), NULL);

    if (!RegisterHotKey(hw, 1, MOD_CONTROL | MOD_SHIFT, 'S')) {
        printf("[ERROR] hotkey Ctrl+Shift+S failed to register (maybe already taken)\n");
        OcrShutdown();
        return 1;
    }

    printf("========== Screenshot OCR test started ==========\n");
    printf("  Press Ctrl+Shift+S to start capture\n");
    printf("  Light grey veil outside selection ; inside the box = clean desktop\n");
    printf("  Click or drag to confirm the capture area\n");
    printf("  Then drag over the red boxes on the image to select text and copy\n");
    printf("  ESC / Right-click : cancel\n");
    printf("  Close this window / Ctrl+C to exit\n");
    printf("================================================\n");

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    OcrShutdown();
    return 0;
}
