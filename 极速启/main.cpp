#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#define _WIN32_IE 0x0600

#include <windows.h>
#include <gdiplus.h>
#include <fstream>
#include <string>
#include "App.h"
#include "resource.h"

namespace {

// Enable per-monitor DPI awareness v2 without depending on a specific
// SDK header version (falls back to the Vista-era API).
void EnableDpiAwareness() {
    typedef BOOL (WINAPI* SPADC)(HANDLE);
    typedef BOOL (WINAPI* SPDA)(int);
    HMODULE u = GetModuleHandleW(L"user32.dll");
    SPADC setCtx = reinterpret_cast<SPADC>(
        GetProcAddress(u, "SetProcessDpiAwarenessContext"));
    if (setCtx &&
        setCtx(reinterpret_cast<HANDLE>(-4))) { // PER_MONITOR_AWARE_V2
        return;
    }
    SPDA setDa = reinterpret_cast<SPDA>(
        GetProcAddress(u, "SetProcessDpiAwareness"));
    if (setDa) {
        setDa(2); // PROCESS_PER_MONITOR_DPI_AWARE
        return;
    }
    SetProcessDPIAware();
}

} // namespace

extern "C" int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE /*hPrev*/,
                   LPSTR /*lpCmdLine*/, int /*nCmdShow*/) {
    // ---- Single instance: a second launch just wakes the first one ----
    HANDLE mtx = CreateMutexW(nullptr, TRUE,
                              L"JisuQi_SingleInstance_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND prev = FindWindowW(L"JisuQiMainWindow", nullptr);
        if (prev) PostMessageW(prev, WM_APP_SHOW, 0, 0);
        if (mtx) CloseHandle(mtx);
        return 0;
    }

    EnableDpiAwareness();

    // OLE initialization is REQUIRED for RegisterDragDrop / IDropTarget.
    // Plain CoInitializeEx is not enough: the shell shows the "no drop"
    // (blocked) cursor when the OLE clipboard layer isn't initialized.
    HRESULT co = OleInitialize(nullptr);

    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gdiToken = 0;
    Gdiplus::Status gst = Gdiplus::GdiplusStartup(&gdiToken, &gsi, nullptr);

    int ret = 1;
    if (gst == Gdiplus::Ok) {
        JisuQi::App app;
        if (app.Init(hInstance)) {
            ret = app.Run();
        } else {
            DWORD err = app.GetInitError();
            wchar_t ep[MAX_PATH * 2] = {0};
            GetModuleFileNameW(nullptr, ep, MAX_PATH * 2);
            std::wstring es(ep);
            size_t sl = es.find_last_of(L"\\/");
            if (sl != std::wstring::npos) es = es.substr(0, sl);
            std::ofstream f((es + L"\\init_error.txt").c_str());
            if (f) {
                f << "JisuQi init failed. GetLastError=0x"
                  << std::hex << (unsigned long)err << "\n";
                f.close();
            }

            wchar_t msg[200];
            swprintf(msg, 200,
                     L"极速启 初始化失败。\n错误码: 0x%08X\n详情见 init_error.txt",
                     static_cast<unsigned int>(err));
            MessageBoxW(nullptr, msg, L"极速启", MB_ICONERROR);
        }
        Gdiplus::GdiplusShutdown(gdiToken);
    } else {
        MessageBoxW(nullptr, L"GDI+ 初始化失败。", L"极速启", MB_ICONERROR);
    }

    if (SUCCEEDED(co)) OleUninitialize();
    return ret;
}
