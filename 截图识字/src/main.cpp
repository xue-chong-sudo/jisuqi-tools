// main.cpp —— 截图识字入口
//
// 两种运行模式：
//  1) 独立模式（旧行为）：双击运行，自带热键 Ctrl+Shift+S 触发截图识字。
//  2) 宿主模式（极速启第二版）：由极速启以
//        截图识字.exe --host <tessdata目录> <极速启settings.ini路径>
//     启动并常驻（无窗口、无控制台）。极速启自己注册截图快捷键，触发时向
//     本进程隐藏窗口 PostMessage(RegisterWindowMessage("JisuQiShotTrigger"))，
//     每次触发时从 settings.ini 的 [screenshot] 段读取最新设置：
//         mode=0 启动识图（截图后弹出结果窗口）/ 1 直接截图（图片静默进剪贴板）
//         wrap=1 智能换行 / 0 保留原始换行
#include "ocraddon.h"
#include <windows.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <string>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdiplus.lib")

namespace {

const wchar_t kTriggerName[] = L"JisuQiShotTrigger";

UINT g_msgTrigger = 0;          // RegisterWindowMessage 结果，与极速启一致
std::wstring g_iniPath;         // 极速启 data\settings.ini 绝对路径
bool g_busy = false;            // 截图/识别进行中，忽略重复触发

// 每次触发时读取最新设置（极速启保存设置即写 ini，无需重启本进程）
void ReadShotSettings(int& mode, int& wrap, int& exitMode) {
    mode = 0; wrap = 1; exitMode = 0;
    if (g_iniPath.empty()) return;
    mode = GetPrivateProfileIntW(L"screenshot", L"mode", 0, g_iniPath.c_str());
    wrap = GetPrivateProfileIntW(L"screenshot", L"wrap", 1, g_iniPath.c_str());
    exitMode = GetPrivateProfileIntW(L"screenshot", L"exit", 0, g_iniPath.c_str());
    if (mode != 0 && mode != 1) mode = 0;
    if (exitMode != 0 && exitMode != 1) exitMode = 0;
}

LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_msgTrigger && m == g_msgTrigger) {
        if (g_busy) return 0;                 // 上一轮还没结束，吞掉重复触发
        g_busy = true;
        int mode = 0, wrap = 1, exitMode = 0;
        ReadShotSettings(mode, wrap, exitMode);
        ocraddon_set_newline_smart(wrap);
        ocraddon_set_exit_mode(exitMode);
        if (mode == 1) ocraddon_capture_direct();       // 直接截图：图片进剪贴板
        else           ocraddon_capture_and_copy();     // 启动识图：截图 -> OCR -> 选词复制
        g_busy = false;
        return 0;
    }
    switch (m) {
        case WM_HOTKEY:                        // 独立模式：Ctrl+Shift+S 触发
            ocraddon_capture_and_copy();
            return 0;
        case WM_CLOSE: DestroyWindow(h); return 0;
        case WM_DESTROY: PostQuitMessage(0); break;
    }
    return DefWindowProc(h, m, w, l);
}

// 宿主模式主循环：隐藏窗口常驻，等极速启触发
int RunHost(const std::wstring& tessdata, const std::wstring& ini) {
    if (ocraddon_init(tessdata.c_str(), L"chi_sim+eng") != 0) {
        MessageBoxW(nullptr, L"OCR 初始化失败，请检查 tessdata 目录与语言包", L"截图识字", MB_ICONERROR);
        return 1;
    }
    g_iniPath = ini;
    g_msgTrigger = RegisterWindowMessageW(kTriggerName);

    WNDCLASSW wc{};
    wc.lpfnWndProc   = HostProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hIcon         = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"JisuQiShotHost";
    RegisterClassW(&wc);
    // 顶层隐藏窗口（不能用 message-only：FindWindow 找不到它，极速启无法投递）
    HWND hw = CreateWindowExW(0, L"JisuQiShotHost", L"截图识字宿主", WS_POPUP,
                              0, 0, 0, 0, nullptr, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    ocraddon_shutdown();
    return 0;
}

} // namespace

// 供 view.cpp 取配置目录（g_iniPath 在匿名命名空间里，外部 TU 看不到）。
const wchar_t* ShotIniPath() { return g_iniPath.c_str(); }

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    // GDI+ 必须在任何 GDI+ 对象（Graphics/Font/Brush 等）创建前初始化。
    // 结果窗的工具栏、标注提示都依赖它；不初始化会导致工具栏绘制失败、
    // 部分 UI 直接空白。
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gdiToken = 0;
    Gdiplus::Status gst = Gdiplus::GdiplusStartup(&gdiToken, &gsi, nullptr);

    // （单实例保护放在下面的独立模式分支内：宿主模式由极速启拉起、无弹窗，
    //   不能因用户另外双击了独立版而把宿主也拦住，否则极速启的截图识字会失效。）
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        if (gst == Gdiplus::Ok) Gdiplus::GdiplusShutdown(gdiToken);
        return 1;
    }

    int rc = 1;
    // 宿主模式：截图识字.exe --host <tessdata目录> <settings.ini路径>
    if (argc >= 4 && wcscmp(argv[1], L"--host") == 0) {
        rc = RunHost(argv[2], argv[3]);
    } else {
        // 独立模式单实例保护：重复双击时只保留一个实例，避免多个实例各自弹出
        // 「热键被占用」等提示（这就是「两个弹窗」的来源）。
        HANDLE hMutex = CreateMutexW(nullptr, FALSE, L"JisuQiShot_Standalone_SingleInstance");
        if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            LocalFree(argv);
            if (gst == Gdiplus::Ok) Gdiplus::GdiplusShutdown(gdiToken);
            return 0;
        }
        // 独立模式（旧行为）：可选 <tessdata目录> [语言]
        std::wstring data = L"tessdata";
        std::wstring lang = L"chi_sim+eng";
        if (argc > 1) data = argv[1];
        if (argc > 2) lang = argv[2];

        if (ocraddon_init(data.c_str(), lang.c_str()) != 0) {
            MessageBoxW(NULL, L"OCR 初始化失败，请检查 tessdata 目录与语言包", L"错误", MB_ICONERROR);
        } else {
            WNDCLASSW wc{};
            wc.lpfnWndProc = HostProc;
            wc.hInstance = GetModuleHandleW(NULL);
            wc.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1));
            wc.lpszClassName = L"OCRHot";
            RegisterClassW(&wc);
            HWND hw = CreateWindowEx(0, L"OCRHot", NULL, 0, 0, 0, 0, 0, NULL, NULL, GetModuleHandle(NULL), NULL);

            if (!RegisterHotKey(hw, 1, MOD_CONTROL | MOD_SHIFT, 'S')) {
                MessageBoxW(NULL, L"热键 Ctrl+Shift+S 注册失败（可能已被占用）", L"错误", MB_ICONERROR);
            }
            // 启动改为静默：不再弹「已启动」提示框，避免每次启动都打扰用户。

            MSG msg;
            while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }

            UnregisterHotKey(hw, 1); // 退出前注销热键，避免被其他程序占用
            ocraddon_shutdown();
            rc = 0;
        }
        if (hMutex) CloseHandle(hMutex);
    }

    LocalFree(argv);
    if (gst == Gdiplus::Ok) Gdiplus::GdiplusShutdown(gdiToken);
    return rc;
}
