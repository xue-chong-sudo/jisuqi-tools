// jq_key_probe.cpp —— 验证极速启主面板的数字键切分组
//
// 判据：读 data/config.json 里的 currentGroup。SetCurrentGroup() 会 cfg.Save()，
// 所以"按了键之后配置文件里的值变了"就是切成功了。
// 现有分组只有两个（常用 / 工具）：1、2 应生效；3 及以上应"吞掉但不做事"
// （既不切分组，也不能把数字漏进搜索框把列表筛空）。
#include <windows.h>
#include <cstdio>
#include <string>

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what);
    if (!ok) ++g_fail;
}

// 从 config.json 里抠 "currentGroup": "xxx"（值里不会有引号，简单扫就够）
static std::wstring ReadCurrentGroup(const wchar_t* path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) return L"";
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);

    const std::string key = "\"currentGroup\"";
    size_t p = s.find(key);
    if (p == std::string::npos) return L"";
    p = s.find('"', s.find(':', p));
    if (p == std::string::npos) return L"";
    size_t q = s.find('"', p + 1);
    if (q == std::string::npos) return L"";
    std::string v = s.substr(p + 1, q - p - 1);

    int wl = MultiByteToWideChar(CP_UTF8, 0, v.c_str(), (int)v.size(), nullptr, 0);
    std::wstring w((size_t)wl, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, v.c_str(), (int)v.size(), &w[0], wl);
    return w;
}

static HWND FindMain() {
    return FindWindowW(L"JisuQiMainWindow", nullptr);
}

static void Press(HWND h, int vk) {
    PostMessageW(h, WM_KEYDOWN, (WPARAM)vk, 0x00000001);
    PostMessageW(h, WM_KEYUP, (WPARAM)vk, 0xC0000001);
    Sleep(450);            // 等 SetCurrentGroup -> cfg.Save() 落盘
}


// 找主窗口里的搜索 EDIT（唯一的 Edit 子窗）
struct FindCtx { HWND found; };
static BOOL CALLBACK ChildEnum(HWND c, LPARAM lp) {
    wchar_t cls[64] = { 0 };
    GetClassNameW(c, cls, 64);
    if (wcscmp(cls, L"Edit") == 0) {
        reinterpret_cast<FindCtx*>(lp)->found = c;
        return FALSE;
    }
    return TRUE;
}
static HWND FindSearchEdit(HWND main) {
    FindCtx c{ nullptr };
    EnumChildWindows(main, ChildEnum, reinterpret_cast<LPARAM>(&c));
    return c.found;
}

// 路由验证：数字键的处理挂在**主窗口**的 WndProc 上，所以
//   ① 按键投给主窗口   -> 切分组
//   ② 按键投给搜索框 EDIT -> 完全不切分组（真实使用中，焦点在 EDIT 时按键就是走这里）
// 用真按键（keybd_event）做 ② 不可行：控制台占着前台，Windows 不允许后台进程
// SetForegroundWindow，键根本到不了目标窗口。所以这里直接对 EDIT 投递消息，
// 验证的是"处理函数归谁"这一件事——这正是本次改动唯一可能破坏的东西。
static void TestSearchFocus(HWND main) {
    HWND edit = FindSearchEdit(main);
    if (!edit) { printf("[probe] 没找到搜索框 EDIT，跳过本项\n"); return; }
    const std::wstring before = ReadCurrentGroup(
        L"C:////Users////Administrator////Desktop////极速启////data////config.json");

    SendMessageW(edit, WM_KEYDOWN, '1', 0x00000001);
    SendMessageW(edit, WM_CHAR, '1', 0x00000001);
    SendMessageW(edit, WM_KEYUP, '1', 0xC0000001);
    Sleep(400);

    wchar_t buf[128] = { 0 };
    GetWindowTextW(edit, buf, 128);
    printf("[probe] 按键投给搜索框后，框里 = \"%ls\"\n", buf);
    if (wcschr(buf, L'1')) Check(true, "数字确实被搜索框吃掉了（没有漏到主窗口）");
    else printf("[probe]     （搜索框没接收 WM_CHAR，下面只看分组是否变化）\n");

    const std::wstring after = ReadCurrentGroup(
        L"C:////Users////Administrator////Desktop////极速启////data////config.json");
    Check(after == before, "按键走搜索框时不切分组");

    SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L""));
}

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const wchar_t* ini = L"C:\\Users\\Administrator\\Desktop\\极速启\\data\\config.json";

    HWND h = FindMain();
    if (!h) { printf("[probe] 没找到 JisuQiMainWindow，先把极速启.exe 跑起来\n"); return 2; }
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    Sleep(300);

    const std::wstring g0 = ReadCurrentGroup(ini);
    // 控制台是 GBK，直接 %ls 打中文会变成空/乱码，统一转 UTF-8 再打
    auto U8 = [](const std::wstring& w) {
        if (w.empty()) return std::string("(空)");
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string s((size_t)n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
        return s;
    };
    printf("[probe] 初始 currentGroup = %s\n", U8(g0).c_str());
    Check(!g0.empty(), "能读到当前分组");

    // 先按 1 回到第一个分组，作为基线
    Press(h, '1');
    const std::wstring g1 = ReadCurrentGroup(ini);
    printf("[probe] 按 1 之后 = %s\n", U8(g1).c_str());

    Press(h, '2');
    const std::wstring g2 = ReadCurrentGroup(ini);
    printf("[probe] 按 2 之后 = %s\n", U8(g2).c_str());
    Check(g2 != g1, "数字 2 切到了第二个分组");
    Check(g2 == L"工具", "数字 2 切到的就是第二个分组「工具」");

    Press(h, '5');
    const std::wstring g5 = ReadCurrentGroup(ini);
    printf("[probe] 按 5 之后 = %s（应保持不变）\n", U8(g5).c_str());
    Check(g5 == g2, "分组不足时数字 5 不改变分组");

    Press(h, '1');
    const std::wstring g1b = ReadCurrentGroup(ini);
    printf("[probe] 再按 1 之后 = %s\n", U8(g1b).c_str());
    Check(g1b == g1, "数字 1 能切回第一个分组");
    Check(g1b == L"常用", "数字 1 切到的就是第一个分组「常用」");

    TestSearchFocus(h);

    printf("\n%s（失败项 %d）\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
