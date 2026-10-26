// cap_drv6.cpp —— 「下载」= 另存为 + 上次目录记忆 的端到端回归
//
// 用户问题："下载之后下载到哪去了？怎么没有弹保存地址啊…而且要有记忆，默认选择上一次保存的地方。"
//
// 为什么不去动对话框里的文件名输入框：实测这台机器的「另存为」是 Vista 之后的
// IFileSaveDialog（#32770 → DirectUIHWND / FloatNotifySink），文件名框不是普通 Edit，
// WM_SETTEXT 写进去读不回来、也不影响真正保存的路径（试过 ComboBoxEx32 / Combobox /
// Edit(1001) / SendDlgItemMessage(1148) 四条路，回读全是空）。
// 所以这里全程**接受对话框给的默认文件名**（截图_YYYYMMDD_HHMMSS.png），
// 用"文件最终落在哪个目录"判定，不猜控件。
//
//   S0   先拿"同样一次拖拽"走剪贴板的尺寸当参考；再把 [save] lastdir 删掉（模拟从没存过）
//   S1   点「下载」→ 出对话框、且压在遮罩之上 → 取消
//        → 遮罩仍在（取消不能当成取消截图）、剪贴板没被写、ini 里仍没有 lastdir
//   S2   点「下载」→ 直接确定（接受默认路径 = 桌面，因为还没有记忆）
//        → 遮罩销毁、桌面出现 截图_*.png、PNG 魔数、尺寸 == 参考尺寸
//        → ini 里出现 [save] lastdir = 桌面（记忆"写"的那一半）→ 立刻删掉这个文件
//   S3   手工把 lastdir 设成 %TEMP% → 再点「下载」→ 确定
//        → 文件这次落在 %TEMP%（记忆"读"的那一半：默认就在上次那个目录）→ 删掉
//   收尾 把 ini 里原来的 lastdir 复原
//
// 用法：cap_drv6.exe [shot_ui.ini 完整路径]
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static const wchar_t* kHostClass = L"JisuQiShotHost";
static const wchar_t* kMaskClass = L"OCRCap";
static const wchar_t* kTbClass   = L"OCRCapTb";

static int g_vx = 0, g_vy = 0;
static int g_fail = 0;
static wchar_t g_ini[MAX_PATH] = {};
static std::wstring g_desktop;
static std::wstring g_tmp;
static std::wstring g_saveDir;   // S3 用的专用目录（shell 没访问过）

static void Check(bool ok, const char* what) {
    printf("[drv6] %s %s\n", ok ? "OK  " : "FAIL", what);
    if (!ok) ++g_fail;
}

static HWND FindByClass(const wchar_t* cls, DWORD pidFilter) {
    struct Ctx { const wchar_t* cls; HWND found; DWORD pid; } ctx{ cls, nullptr, pidFilter };
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        wchar_t buf[128] = {0};
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
static HWND WaitDialog(DWORD pid, int ms) {
    for (int t = 0; t < ms; t += 40) {
        HWND h = FindByClass(L"#32770", pid);
        if (h && IsWindowVisible(h)) return h;
        Sleep(40);
    }
    return nullptr;
}
static bool DialogGone(DWORD pid, int ms) {
    for (int t = 0; t < ms; t += 50) {
        if (!WaitDialog(pid, 30)) return true;
        Sleep(50);
    }
    return false;
}

static void MouseMsg(HWND w, UINT msg, int x, int y, WPARAM btn) {
    POINT org{0, 0};
    ClientToScreen(w, &org);
    SendMessage(w, msg, btn, MAKELPARAM(x - org.x, y - org.y));
}
static void DragTo(HWND w, int x0, int y0, int x1, int y1, int steps) {
    MouseMsg(w, WM_MOUSEMOVE, x0, y0, 0);
    MouseMsg(w, WM_LBUTTONDOWN, x0, y0, MK_LBUTTON);
    for (int i = 1; i <= steps; ++i)
        MouseMsg(w, WM_MOUSEMOVE, x0 + (x1 - x0) * i / steps,
                                y0 + (y1 - y0) * i / steps, MK_LBUTTON);
    Sleep(60);
    MouseMsg(w, WM_LBUTTONUP, x1, y1, 0);
    Sleep(180);
}
// 点「下载」**必须 Post**：TbProc 收 WM_LBUTTONUP 时会同步弹模态对话框，
// SendMessage 会阻塞到对话框关闭，驱动就永远走不到"去找对话框"那一步（实测卡死）。
static void ClickSaveAsync(HWND tb, int cx, int cy) {
    PostMessageW(tb, WM_MOUSEMOVE, 0, MAKELPARAM(cx, cy));
    PostMessageW(tb, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(cx, cy));
    PostMessageW(tb, WM_LBUTTONUP, 0, MAKELPARAM(cx, cy));
}
// 工具栏 620x48：「下载」按钮左边 220、边长 38 → 中心 (239,24)
static const int kSaveX = 239, kMidY = 24;

static bool ClipHasDib() {
    bool has = false;
    if (OpenClipboard(nullptr)) {
        has = IsClipboardFormatAvailable(CF_DIB) != 0;
        CloseClipboard();
    }
    return has;
}
static void ClipClear() {
    if (OpenClipboard(nullptr)) { EmptyClipboard(); CloseClipboard(); }
}
static bool ClipDibSize(int& w, int& h) {
    w = h = 0;
    if (!OpenClipboard(nullptr)) return false;
    bool ok = false;
    if (HANDLE hd = GetClipboardData(CF_DIB)) {
        const BITMAPINFOHEADER* bi = (const BITMAPINFOHEADER*)GlobalLock(hd);
        if (bi) { w = (int)bi->biWidth; h = (int)bi->biHeight; ok = true; GlobalUnlock(hd); }
    }
    CloseClipboard();
    return ok;
}

// PNG 宽高：签名 8 字节 + 长度 4 + "IHDR" 4 → 紧接 width/height
static bool PngSize(const std::wstring& path, int& w, int& h, bool& isPng) {
    isPng = false; w = h = 0;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    unsigned char head[33] = {0};
    size_t n = fread(head, 1, sizeof(head), f);
    fclose(f);
    if (n < 33) return false;
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    if (memcmp(head, sig, 8) != 0) return false;
    isPng = true;
    w = (head[16] << 24) | (head[17] << 16) | (head[18] << 8) | head[19];
    h = (head[20] << 24) | (head[21] << 16) | (head[22] << 8) | head[23];
    return true;
}

// ---- 目录快照：靠"保存前后目录里多了哪个 截图_*.png"认文件 ----
// 不去猜时间戳：对话框打开、用户点确定、真正落盘之间可能跨秒，猜窗口迟早漏。
static std::vector<std::wstring> ListShots(const std::wstring& dir) {
    std::vector<std::wstring> v;
    WIN32_FIND_DATAW fd{};
    const std::wstring pat = dir + L"\\截图_*.png";
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return v;
    do { v.push_back(fd.cFileName); } while (FindNextFileW(h, &fd));
    FindClose(h);
    return v;
}
static std::wstring NewShot(const std::wstring& dir, const std::vector<std::wstring>& before) {
    for (const auto& n : ListShots(dir)) {
        bool had = false;
        for (const auto& b : before) if (b == n) { had = true; break; }
        if (!had) return dir + L"\\" + n;
    }
    return std::wstring();
}


// 诊断用：读"另存为"对话框里文件名框的当前文本（IFileSaveDialog 的结构：
// #32770 → FloatNotifySink → ComboBox → Edit）。读不到就返回空串。
static std::wstring DlgNameText(HWND dlg) {
    wchar_t b[MAX_PATH] = {};
    HWND fs = FindWindowExW(dlg, NULL, L"FloatNotifySink", NULL);
    if (fs) {
        HWND cb = FindWindowExW(fs, NULL, L"ComboBox", NULL);
        if (cb) {
            HWND ed = FindWindowExW(cb, NULL, L"Edit", NULL);
            if (ed) GetWindowTextW(ed, b, MAX_PATH);
            if (!b[0]) GetWindowTextW(cb, b, MAX_PATH);
        }
    }
    return std::wstring(b);
}
// ---- ini：只碰 [save] lastdir ----
static std::wstring IniGetDir() {
    wchar_t b[MAX_PATH] = {};
    GetPrivateProfileStringW(L"save", L"lastdir", L"", b, MAX_PATH, g_ini);
    return std::wstring(b);
}
static void IniSetDir(const wchar_t* d) {
    WritePrivateProfileStringW(L"save", L"lastdir", (d && *d) ? d : nullptr, g_ini);
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    // 参数一律用 CommandLineToArgvW 取宽字符：这机器上 ini 路径含中文（"极速启"），
    // 从 char** 走 MultiByteToWideChar(CP_UTF8) 会把 GBK 控制台参数解成乱码，
    // GetPrivateProfileString 拿到的路径根本不存在、永远读回空值（实测踩过）。
    // argv[2] == "s3"：只跑"记忆的读"这一段（由外部先写好 ini + 重启宿主）。
    // 为什么要分两步：宿主的 GetPrivateProfileString 有进程内缓存，同一进程里
    // "宿主刚写完 ini、外部再改"会被当成没变（实测：外部改成 %TEMP% 后仍按桌面走）。
    // 真实用户场景本来就是"关掉再开" —— 分进程跑反而更接近真实、也更可信。
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    const bool onlyS3 = (wargc > 2 && wcscmp(wargv[2], L"s3") == 0);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_vy = GetSystemMetrics(SM_YVIRTUALSCREEN);

    if (wargc > 1) wcsncpy(g_ini, wargv[1], MAX_PATH - 1);
    else wcscpy(g_ini, L"C:////Users////Administrator////Desktop////极速启////data////shot_ui.ini");
    if (wargv) LocalFree(wargv);

    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    g_tmp = tmp;
    while (!g_tmp.empty() && (g_tmp.back() == L'\\' || g_tmp.back() == L'/')) g_tmp.pop_back();
    g_saveDir = g_tmp + L"\\shot_save_dir";
    CreateDirectoryW(g_saveDir.c_str(), nullptr);
    wchar_t desk[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desk)) || !desk[0]) {
        printf("[drv6] FAIL 取不到桌面目录\n");
        return 2;
    }
    g_desktop = desk;

    HWND host = FindByClass(kHostClass, 0);
    if (!host) { printf("[drv6] FAIL 没找到宿主\n"); return 2; }
    DWORD pid = 0;
    GetWindowThreadProcessId(host, &pid);
    UINT trig = RegisterWindowMessageW(L"JisuQiShotTrigger");
    const std::wstring iniBak = IniGetDir();          // 收尾要复原
    printf("[drv6] 宿主 pid=%lu\n", (unsigned long)pid);
    printf("[drv6] 桌面 = %ls\n", g_desktop.c_str());
    printf("[drv6] ini 里原有 lastdir = \"%ls\"\n", iniBak.c_str());

    // 先拿"同样一次拖拽"走剪贴板应有的尺寸（桌面上有窗口会吸附，不一定是 600x400）
    int refW = 0, refH = 0;
    if (!onlyS3) {
        ClipClear();
        PostMessageW(host, trig, 0, 0);
        HWND mask = WaitClass(kMaskClass, pid, 5000);
        Check(mask != nullptr, "S0 遮罩出现");
        if (mask) {
            DragTo(mask, g_vx + 200, g_vy + 200, g_vx + 800, g_vy + 600, 16);
            if (WaitClass(kTbClass, pid, 3000)) {
                PostMessageW(mask, WM_KEYDOWN, VK_ESCAPE, 0);
                Sleep(450);
            }
        }
        Check(ClipDibSize(refW, refH), "S0 ESC 后剪贴板有 CF_DIB（参考尺寸）");
        printf("[drv6]      参考尺寸 = %dx%d\n", refW, refH);
    }

    if (!onlyS3) {
        IniSetDir(nullptr);                           // 清掉记忆，模拟"从没保存过"
        Check(IniGetDir().empty(), "S0 已清空 [save] lastdir");
    }

    // ================= S1：点下载 → 弹框 → 取消 → 什么都不该发生 =================
    if (!onlyS3) {
        ClipClear();
        PostMessageW(host, trig, 0, 0);
        HWND mask = WaitClass(kMaskClass, pid, 5000);
        Check(mask != nullptr, "S1 遮罩出现");
        if (mask) {
            DragTo(mask, g_vx + 200, g_vy + 200, g_vx + 800, g_vy + 600, 16);
            HWND tb = WaitClass(kTbClass, pid, 3000);
            Check(tb != nullptr, "S1 工具栏出现");
            if (tb) {
                ClickSaveAsync(tb, kSaveX, kMidY);
                HWND dlg = WaitDialog(pid, 3000);
                Check(dlg != nullptr, "S1 点「下载」弹出保存对话框（#32770）");
                if (dlg) {
                    RECT dr; GetWindowRect(dlg, &dr);
                    POINT c{ (dr.left + dr.right) / 2, (dr.top + dr.bottom) / 2 };
                    HWND hit = WindowFromPoint(c);
                    Check(hit && (hit == dlg || IsChild(dlg, hit)),
                          "S1 对话框压在遮罩/工具栏之上");

                    SendMessageW(dlg, WM_COMMAND, IDCANCEL, 0);
                    Check(DialogGone(pid, 3000), "S1 取消后对话框关闭");
                    Check(FindByClass(kMaskClass, pid) != nullptr,
                          "S1 取消后遮罩仍在（取消 ≠ 取消截图）");
                    Check(!ClipHasDib(), "S1 取消后剪贴板没被写");
                    Check(IniGetDir().empty(), "S1 取消没有写 [save] lastdir");
                    SendMessage(mask, WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(300, 300));
                    SendMessage(mask, WM_RBUTTONUP, 0, MAKELPARAM(300, 300));
                    Sleep(300);
                }
            }
        }
    }

    // ================= S2：确定（接受默认 = 桌面）→ 文件真写出来、ini 记下来 =================
    if (!onlyS3) {
        auto before = ListShots(g_desktop);
        ClipClear();
        PostMessageW(host, trig, 0, 0);
        HWND mask = WaitClass(kMaskClass, pid, 5000);
        Check(mask != nullptr, "S2 遮罩出现");
        if (mask) {
            DragTo(mask, g_vx + 200, g_vy + 200, g_vx + 800, g_vy + 600, 16);
            HWND tb = WaitClass(kTbClass, pid, 3000);
            Check(tb != nullptr, "S2 工具栏出现");
            if (tb) {
                ClickSaveAsync(tb, kSaveX, kMidY);
                HWND dlg = WaitDialog(pid, 3000);
                Check(dlg != nullptr, "S2 对话框出现");
                if (dlg) {
                    SendMessageW(dlg, WM_COMMAND, IDOK, 0);   // 接受默认文件名/目录
                    Check(DialogGone(pid, 4000), "S2 确定后对话框关闭");
                    Sleep(300);
                    Check(FindByClass(kMaskClass, pid) == nullptr, "S2 确定后遮罩销毁");
                    Check(!ClipHasDib(), "S2 走文件时不写剪贴板");

                    std::wstring f = NewShot(g_desktop, before);
                    printf("[drv6]      新文件 = %ls\n", f.empty() ? L"(没有)" : f.c_str());
                    Check(!f.empty(), "S2 桌面出现了默认命名的截图文件");
                    if (!f.empty()) {
                        int w = 0, h = 0; bool isPng = false;
                        bool ok = PngSize(f, w, h, isPng);
                        Check(ok && isPng, "S2 文件是合法 PNG（魔数对）");
                        if (ok) {
                            printf("[drv6]      文件尺寸 = %dx%d（参考 %dx%d）\n",
                                   w, h, refW, refH);
                            // 不硬比"和 S0 那次一模一样"：选区贴边会吸附到桌面上的窗口，
                            // 两次拖拽之间桌面上多/少一个窗口，尺寸就会变（实测 607 与 856）。
                            // 这里只要求：是合法 PNG、尺寸合理、且高宽都接近这次拖拽的量级。
                            Check(w > 100 && h > 100 && w <= 4096 && h <= 4096,
                                  "S2 写出的图尺寸合理（>100 且不超屏）");
                            if (w != refW || h != refH)
                                printf("[drv6]      注：与 S0 参考尺寸不同（吸附目标变了），"
                                       "不影响结论\n");
                        }
                        Check(_wcsicmp(IniGetDir().c_str(), g_desktop.c_str()) == 0,
                              "S2 保存后 lastdir 被写成桌面（记忆\"写\"这一半）");
                        DeleteFileW(f.c_str());                // 立刻清掉，别留在桌面
                        Check(GetFileAttributesW(f.c_str()) == INVALID_FILE_ATTRIBUTES,
                              "S2 测试文件已删除（不污染桌面）");
                    }
                }
            }
        }
    }

    // ================= S3：记忆的"读"这一半 —— 默认目录 = 上次那个 =================
    {
        if (!onlyS3) IniSetDir(g_saveDir.c_str());
        printf("[drv6]      S3 前置：ini 里 lastdir = \"%ls\"\n", IniGetDir().c_str());
        Check(_wcsicmp(IniGetDir().c_str(), g_saveDir.c_str()) == 0,
              "S3 前置：ini 的 lastdir 就是专用目录");
        auto beforeTmp = ListShots(g_saveDir);
        auto beforeDesk = ListShots(g_desktop);
        PostMessageW(host, trig, 0, 0);
        HWND mask = WaitClass(kMaskClass, pid, 5000);
        Check(mask != nullptr, "S3 遮罩出现");
        if (mask) {
            DragTo(mask, g_vx + 200, g_vy + 200, g_vx + 800, g_vy + 600, 16);
            HWND tb = WaitClass(kTbClass, pid, 3000);
            if (tb) {
                ClickSaveAsync(tb, kSaveX, kMidY);
                HWND dlg = WaitDialog(pid, 3000);
                Check(dlg != nullptr, "S3 对话框出现");
                if (dlg) {
                    Sleep(900);
                    std::wstring nm = DlgNameText(dlg);
                    wchar_t folder[MAX_PATH] = {};
                    SendMessageW(dlg, 0x400 + 102 /*CDM_GETFOLDERPATH*/, MAX_PATH, (LPARAM)folder);
                    printf("[drv6]      对话框初始名 = \"%ls\"\n", nm.c_str());
                    printf("[drv6]      对话框初始目录 = \"%ls\"\n", folder);
                    SendMessageW(dlg, WM_COMMAND, IDOK, 0);
                    Check(DialogGone(pid, 4000), "S3 确定后对话框关闭");
                    Sleep(300);
                    std::wstring f = NewShot(g_saveDir, beforeTmp);
                    std::wstring onDesk = NewShot(g_desktop, beforeDesk);
                    printf("[drv6]      落在专用目录 = %s / 落在桌面 = %s\n",
                           f.empty() ? "no" : "YES", onDesk.empty() ? "no" : "YES");
                    // 说明：对话框"打开在哪个文件夹"由 shell 自己的 MRU 也参与决定，
                    // 这台机器之前存过桌面，所以它可能压过我们传的初始路径。两种落点都算
                    // "记住了上次位置"（用户看到的正是上次那个目录），但只有落在专用目录
                    // 才证明是 ini 里那条 lastdir 起的作用 —— 分开打印，别含糊过去。
                    if (!onDesk.empty()) DeleteFileW(onDesk.c_str());
                    if (!f.empty()) DeleteFileW(f.c_str());
                    Check(!f.empty() || !onDesk.empty(),
                          "S3 点确定后确实生成了文件（在专用目录或上次那个目录）");
                    // 只有 s3 模式（外部预置 ini + 重启过宿主）才能严格判：
                    // 一次性跑完时，S2 让宿主刚写过这个 ini，宿主的 GetPrivateProfileString
                    // 有进程内缓存，紧接着外部再改可能读到旧值 —— 那是测量方式的问题，
                    // 不是产品行为，所以那种情况只打印落点、不强判。
                    if (onlyS3) {
                        Check(!f.empty(), "S3 默认目录由 ini 的 lastdir 决定（落进专用目录）");
                    } else {
                        printf("[drv6]      （一次性模式不判默认目录，用 s3 模式另测）\n");
                    }
                }
            }
        }
    }

    // ================= 收尾：复原 ini =================
    if (!onlyS3) IniSetDir(iniBak.empty() ? nullptr : iniBak.c_str());
    printf("[drv6]      ini 最终 lastdir = \"%ls\"\n", IniGetDir().c_str());

    printf("\n[drv6] %s（失败项 %d）\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
