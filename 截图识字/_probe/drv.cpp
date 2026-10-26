// 截图遮罩 P0 验证：触发 → 真实拖拽（多帧）→ 中途截屏 → 回拖 → 截屏 → 取消
#include <windows.h>
#include <psapi.h>
#include <cstdio>
#include <string>

#pragma comment(lib, "psapi.lib")

static FILE* L = nullptr;

static void GrabToFile(const wchar_t* path) {
    int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
    HDC sdc = GetDC(nullptr), mdc = CreateCompatibleDC(sdc);
    HBITMAP bmp = CreateCompatibleBitmap(sdc, w, h);
    HGDIOBJ old = SelectObject(mdc, bmp);
    BitBlt(mdc, 0, 0, w, h, sdc, 0, 0, SRCCOPY | CAPTUREBLT);
    BITMAPFILEHEADER fh; ZeroMemory(&fh, sizeof(fh));
    BITMAPINFOHEADER ih; ZeroMemory(&ih, sizeof(ih));
    ih.biSize = sizeof(ih); ih.biWidth = w; ih.biHeight = h;
    ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
    DWORD sz = (DWORD)w * h * 4;
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + sz;
    BYTE* buf = (BYTE*)malloc(sz);
    GetDIBits(mdc, bmp, 0, h, buf, (BITMAPINFO*)&ih, DIB_RGB_COLORS);
    FILE* f = _wfopen(path, L"wb");
    if (f) {
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&ih, sizeof(ih), 1, f);
        fwrite(buf, 1, sz, f);
        fclose(f);
    }
    free(buf);
    SelectObject(mdc, old); DeleteObject(bmp); DeleteDC(mdc); ReleaseDC(nullptr, sdc);
    fprintf(L, "grab -> %ls (%dx%d)\n", path, w, h);
}

static void Mouse(BOOL down, int x, int y) {
    SetCursorPos(x, y);
    INPUT in; ZeroMemory(&in, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    SendInput(1, &in, sizeof(INPUT));
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) { printf("usage: drv.exe <exe> <tessdata> <ini> [prefix]\n"); return 1; }
    const wchar_t* prefix = argc > 4 ? argv[4] : L"cap";
    L = _wfopen(L"drv.log", L"a");

    std::wstring cmd = std::wstring(L"\"") + argv[1] + L"\" --host \"" + argv[2] +
                       L"\" \"" + argv[3] + L"\"";
    STARTUPINFOW si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
    wchar_t cwd[MAX_PATH]; GetCurrentDirectoryW(MAX_PATH, cwd);
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, cwd, &si, &pi)) {
        printf("CreateProcess failed %lu\n", GetLastError());
        return 2;
    }
    HWND host = nullptr;
    for (int i = 0; i < 60 && !host; ++i) {
        host = FindWindowW(L"JisuQiShotHost", nullptr);
        if (host) { DWORD pid = 0; GetWindowThreadProcessId(host, &pid); if (pid != pi.dwProcessId) host = nullptr; }
        Sleep(100);
    }
    if (!host) { printf("no host\n"); TerminateProcess(pi.hProcess, 0); return 3; }
    Sleep(800);

    PostMessageW(host, RegisterWindowMessageW(L"JisuQiShotTrigger"), 0, 0);
    HWND cap = nullptr;
    for (int i = 0; i < 40 && !cap; ++i) { cap = FindWindowW(L"OCRCap", nullptr); Sleep(50); }
    printf("overlay=%p\n", (void*)cap);
    if (!cap) { TerminateProcess(pi.hProcess, 0); return 4; }
    Sleep(300);

    // 从 (300,250) 拖到 (900,650)，40 步产生足够多 WM_MOUSEMOVE
    Mouse(TRUE, 300, 250);
    for (int i = 1; i <= 40; ++i) {
        SetCursorPos(300 + (900 - 300) * i / 40, 250 + (650 - 250) * i / 40);
        Sleep(8);
    }
    Sleep(150);
    wchar_t p1[MAX_PATH]; swprintf(p1, MAX_PATH, L"%ls_out.bmp", prefix);
    GrabToFile(p1);                       // 展开态：看洞/边框/标签
    // 回拖到 (600,420)：检查旧边框有没有残影
    for (int i = 1; i <= 20; ++i) {
        SetCursorPos(900 + (600 - 900) * i / 20, 650 + (420 - 650) * i / 20);
        Sleep(8);
    }
    Sleep(150);
    wchar_t p2[MAX_PATH]; swprintf(p2, MAX_PATH, L"%ls_back.bmp", prefix);
    GrabToFile(p2);
    Mouse(FALSE, 600, 420);
    Sleep(500);
    HWND view = FindWindowW(L"OCRView", nullptr);
    fprintf(L, "view=%p\n", (void*)view);
    if (view) { PostMessageW(view, WM_KEYDOWN, VK_ESCAPE, 0); Sleep(400); }
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    fprintf(L, "done\n"); fflush(L); fclose(L);
    return 0;
}
