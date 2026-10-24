//==============================================================
// 极速启 · 设置界面  settings_ui.cpp
// 纯 Win32 + GDI+ 自绘，无 Qt / Boost 等大库，单 exe 友好。
//
// 编译（可直接并入 build.bat，保持原有命令风格）：
//   cl /nologo /std:c++17 /O2 /MT /W4 /utf-8 settings_ui.cpp ^
//      /link user32.lib gdi32.lib gdiplus.lib comctl32.lib shell32.lib shlwapi.lib dwmapi.lib ^
//      /SUBSYSTEM:WINDOWS /OUT:jisuqi.exe
//
// 说明：
//  - 配置读写用 data\settings.ini（不写注册表），U 盘便携；
//  - GDI+ / DWM 均为系统组件，仅增加少量 DLL 导入，不增加 exe 体积；
//  - Win11 下自动获得圆角窗口 + 白色标题栏，旧系统静默降级无影响。
//==============================================================
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <shlwapi.h>
#include <stdio.h>
#include <vector>
#include <string>
#include "settings_ui.h"   // 共享接口（数据结构 + 导出函数声明）

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "dwmapi.lib")

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#define DWMWA_CAPTION_COLOR 34
#endif

// 让 MessageBox 等系统弹窗也启用视觉样式（公共控件 v6 清单）。
// 注意：必须用单行、整体转义双引号包裹，否则链接器解析 /manifestdependency 会报 LNK1276。
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace Gdiplus;

//---------------- 设置数据 ----------------
// 结构体定义见 settings_ui.h（由主程序与设置界面共享，避免重复定义）。
static Settings g_s;
static Settings g_snapshot;   // 打开设置窗口时的快照，供“取消”回滚本次改动

// 供主程序访问 / 应用设置（注册热键、挂中键钩子等由主程序在这里完成）
Settings& GetSettings() { return g_s; }

// 设置保存后回调主程序：重新注册热键 / 刷新中键监听 / 重置失焦隐藏计时 /
// 更新单击双击启动逻辑。界面自身不依赖 App，仅通过回调桥接。
static SettingsApplyCallback g_applyCb = nullptr;
static void (*g_addRecycleBinCb)() = nullptr;
static void (*g_applyAutoHideCb)() = nullptr;   // 轻量回调：仅重置失焦隐藏计时（步进器/数字编辑确认用，避免整链同步卡顿）
void SetSettingsApplyCallback(SettingsApplyCallback cb) { g_applyCb = cb; }
void SetSettingsAddRecycleBinCallback(void (*cb)())     { g_addRecycleBinCb = cb; }
void SetSettingsApplyAutoHideCallback(void (*cb)())     { g_applyAutoHideCb = cb; }
void ApplySettings() {
    if (g_applyCb) g_applyCb();
}

// 供主程序在启动时强制读 / 写 data\settings.ini。
// （Load / Save / IniPath 在文件下方定义，这里先前向声明。）
static void Load();
static void Save();
static std::wstring IniPath();
void LoadSettingsUI() { Load(); }
void SaveSettingsUI() { Save(); }
bool SettingsUIExists() {
    std::wstring f = IniPath();
    DWORD a = GetFileAttributesW(f.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

//---------------- 配置持久化：data\settings.ini ----------------
static std::wstring IniPath() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    PathRemoveFileSpecW(p);
    PathAppendW(p, L"data");
    CreateDirectoryW(p, nullptr);          // 已存在则失败，无影响
    PathAppendW(p, L"settings.ini");
    return p;
}
// 安全解析十进制整数：用 wcstoll 而非 swscanf("%d")。swscanf 在数字超出 int
// 范围时属于未定义行为（UB），而 INI 是可直接手改的文本；这里解析到 long long
// 再钳制，既避免 UB，又给出确定的边界语义。
// 返回：解析成功返回钳制到 [lo,hi] 的值；无数字可解析时返回 def。
static int ParseIntClamped(const wchar_t* s, int def, int lo, int hi) {
    if (!s) return def;
    wchar_t* end = nullptr;
    long long v = wcstoll(s, &end, 10);
    if (end == s) return def;               // 完全没有数字
    if (v < static_cast<long long>(lo)) return lo;
    if (v > static_cast<long long>(hi)) return hi;
    return static_cast<int>(v);
}

static void Load() {
    std::wstring f = IniPath();
    g_s.rememberSize = GetPrivateProfileIntW(L"window", L"remember", 1, f.c_str()) != 0;
    g_s.followMouse  = GetPrivateProfileIntW(L"window", L"followmouse", 0, f.c_str()) != 0;
    g_s.middleButton = true;   // 旧开关已由“鼠标按键弹出”下拉框替代，鼠标呼出常开
    g_s.hkMod        = GetPrivateProfileIntW(L"hotkey",  L"mod",     0, f.c_str());
    g_s.hkVk         = GetPrivateProfileIntW(L"hotkey",  L"vk",      0, f.c_str());
    // 主界面搜索框快捷键（默认 Alt+F），与截图/搜索页的全局快捷键无关。
    g_s.sboxMod      = GetPrivateProfileIntW(L"hotkey",  L"sboxmod", (UINT)MOD_ALT, f.c_str());
    g_s.sboxVk       = GetPrivateProfileIntW(L"hotkey",  L"sboxvk",  0x46, f.c_str());
    // 呼出鼠标键：0=中键 / 1=向后侧键 / 2=向前侧键 / 3=不使用鼠标呼出
    g_s.mouseBtn = GetPrivateProfileIntW(L"mouse", L"button", 0, f.c_str());
    if (g_s.mouseBtn < 0) g_s.mouseBtn = 0;
    if (g_s.mouseBtn > 3) g_s.mouseBtn = 3;
    // 长按毫秒：0=按下即触发；吸附到 0/125/250/375/500（0-500 分 4 段）
    {
        wchar_t hb[32] = {0};
        GetPrivateProfileStringW(L"mouse", L"hold", L"0", hb, 32, f.c_str());
        int hv = ParseIntClamped(hb, 0, 0, 500);
        hv = ((hv + 62) / 125) * 125;          // 就近吸附到 125 的倍数
        if (hv > 500) hv = 500;
        g_s.holdMs = hv;
    }
    // 失焦自动隐藏秒数：按字符串读取再解析，避免 GetPrivateProfileInt 在内容异常时
    // 直接回退默认值；0 是合法值（表示关闭计时），必须原样保留、不得被吞掉。
    {
        wchar_t hb[32] = {0};
        GetPrivateProfileStringW(L"misc", L"hide", L"5", hb, 32, f.c_str());
        g_s.hideSec = ParseIntClamped(hb, 5, 0, 60);
    }
    // 启动方式：单选 0=仅单击 / 1=仅双击。兼容旧版“两个独立复选框”字段做一次性迁移。
    int m = GetPrivateProfileIntW(L"launch", L"mode", -1, f.c_str());
    if (m < 0) {
        bool oldSingle = GetPrivateProfileIntW(L"launch", L"single", 1, f.c_str()) != 0;
        bool oldDouble = GetPrivateProfileIntW(L"launch", L"double", 0, f.c_str()) != 0;
        m = (oldDouble && !oldSingle) ? 1 : 0;   // 仅双击 → 1，其余（含仅单击/两者）→ 0
    }
    g_s.launchMode  = (m == 1) ? 1 : 0;

    // 开机自启：settings.ini 为权威来源。若键缺失（旧版迁移 / 首启），
    // 保留主程序在 LoadSettingsUI 前从 config.json 注入的 g_s.autoStart 作为回落。
    wchar_t aab[8] = {0};
    GetPrivateProfileStringW(L"window", L"autostart", L"", aab, 8, f.c_str());
    if (aab[0]) g_s.autoStart = (wcstol(aab, nullptr, 10) != 0);

    // ---- 截图分组（[screenshot] 段）----
    // 截图快捷键：mod/vk 分开存；vk=0 表示不启用截图快捷键。
    g_s.capHkMod = (UINT)GetPrivateProfileIntW(L"screenshot", L"capmod", 1 /*MOD_ALT*/, f.c_str());
    g_s.capHkVk  = (UINT)GetPrivateProfileIntW(L"screenshot", L"capvk",  0x41 /*'A'*/,   f.c_str());
    if (g_s.capHkMod & ~0xFu) g_s.capHkMod = 1;   // 异常值回落默认 Alt
    // 识图方式：0=启动识图（截图后弹出图片识图）1=直接截图（截图后直接复制图片）
    {
        wchar_t sb[16] = {0};
        GetPrivateProfileStringW(L"screenshot", L"mode", L"0", sb, 16, f.c_str());
        g_s.shotMode = ParseIntClamped(sb, 0, 0, 1);
    }
    // 换行模式：1=智能换行（默认）0=保留原始换行
    {
        wchar_t wb[16] = {0};
        GetPrivateProfileStringW(L"screenshot", L"wrap", L"1", wb, 16, f.c_str());
        g_s.shotWrapSmart = (ParseIntClamped(wb, 1, 0, 1) == 1);
    }
    // 退出方式：0=复制文字后直接退出（默认）1=复制完文字后 ESC 退出
    {
        wchar_t eb[16] = {0};
        GetPrivateProfileStringW(L"screenshot", L"exit", L"0", eb, 16, f.c_str());
        g_s.shotExitMode = ParseIntClamped(eb, 0, 0, 1);
    }

    // ---- 搜索分组（[search] 段）----
    // 开启搜索 / 开启预览：默认均为开（1）。
    {
        wchar_t b[16] = {0};
        GetPrivateProfileStringW(L"search", L"enabled", L"1", b, 16, f.c_str());
        g_s.searchEnabled = (ParseIntClamped(b, 1, 0, 1) != 0);
        GetPrivateProfileStringW(L"search", L"preview", L"1", b, 16, f.c_str());
        g_s.searchPreview = (ParseIntClamped(b, 1, 0, 1) != 0);
    }
    // 搜索快捷键：默认 Ctrl+F；vk=0 表示不启用。
    g_s.searchHkMod = (UINT)GetPrivateProfileIntW(L"search", L"hkmod", 2 /*MOD_CONTROL*/, f.c_str());
    g_s.searchHkVk  = (UINT)GetPrivateProfileIntW(L"search", L"hkvk",  0x46 /*'F'*/,        f.c_str());
    if (g_s.searchHkMod & ~0xFu) g_s.searchHkMod = 2;   // 异常值回落默认 Ctrl
}
static void Save() {
    std::wstring f = IniPath(); wchar_t b[32];
    WritePrivateProfileStringW(L"window", L"remember", g_s.rememberSize ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"window", L"followmouse", g_s.followMouse ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"window", L"autostart", g_s.autoStart ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"hotkey", L"middle",   g_s.middleButton ? L"1" : L"0", f.c_str());
    swprintf(b, 32, L"%u", g_s.hkMod);  WritePrivateProfileStringW(L"hotkey", L"mod",  b, f.c_str());
    swprintf(b, 32, L"%u", g_s.hkVk);   WritePrivateProfileStringW(L"hotkey", L"vk",   b, f.c_str());
    swprintf(b, 32, L"%u", g_s.sboxMod); WritePrivateProfileStringW(L"hotkey", L"sboxmod", b, f.c_str());
    swprintf(b, 32, L"%u", g_s.sboxVk);  WritePrivateProfileStringW(L"hotkey", L"sboxvk",  b, f.c_str());
    swprintf(b, 32, L"%d", g_s.launchMode); WritePrivateProfileStringW(L"launch", L"mode", b, f.c_str());
    swprintf(b, 32, L"%d", g_s.hideSec);    WritePrivateProfileStringW(L"misc",   L"hide", b, f.c_str());
    swprintf(b, 32, L"%d", g_s.mouseBtn);   WritePrivateProfileStringW(L"mouse",  L"button", b, f.c_str());
    swprintf(b, 32, L"%d", g_s.holdMs);     WritePrivateProfileStringW(L"mouse",  L"hold",   b, f.c_str());
    // 截图分组（[screenshot] 段）
    swprintf(b, 32, L"%u", g_s.capHkMod);        WritePrivateProfileStringW(L"screenshot", L"capmod", b, f.c_str());
    swprintf(b, 32, L"%u", g_s.capHkVk);         WritePrivateProfileStringW(L"screenshot", L"capvk",  b, f.c_str());
    swprintf(b, 32, L"%d", g_s.shotMode);        WritePrivateProfileStringW(L"screenshot", L"mode",   b, f.c_str());
    WritePrivateProfileStringW(L"screenshot", L"wrap", g_s.shotWrapSmart ? L"1" : L"0", f.c_str());
    swprintf(b, 32, L"%d", g_s.shotExitMode);    WritePrivateProfileStringW(L"screenshot", L"exit",   b, f.c_str());
    // 搜索分组（[search] 段）
    WritePrivateProfileStringW(L"search", L"enabled", g_s.searchEnabled ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"search", L"preview", g_s.searchPreview ? L"1" : L"0", f.c_str());
    swprintf(b, 32, L"%u", g_s.searchHkMod);     WritePrivateProfileStringW(L"search", L"hkmod", b, f.c_str());
    swprintf(b, 32, L"%u", g_s.searchHkVk);      WritePrivateProfileStringW(L"search", L"hkvk",  b, f.c_str());
    // 立即把 INI 缓存刷回磁盘：本程序常驻托盘、长期不退出，Windows 对 INI 的写带缓存，
    // 不显式 flush 时最近一次改动可能只存在于进程缓存中，一旦进程被杀 / 异常退出即丢失，
    // 表现为“保存后再次打开数值回退”。此调用保证改动即时落盘。
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, f.c_str());
}

//---------------- 界面资源 ----------------
static ULONG_PTR gToken = 0;
static Font *F12 = nullptr, *F12B = nullptr, *F13 = nullptr, *F13B = nullptr;
static HFONT s_hFontUI = nullptr;   // GDI 字体：下拉框 / 数字编辑框等原生控件用

// 单行 EDIT 的文字是「顶端对齐」的：控件比字体高多少，多出来的空白就全堆在文字下方。
// 把 36px 高的 EDIT 套在 15px 的字体上，数字就会紧贴输入框上沿 —— 这不是样式没设对，
// 而是 EDIT 本身的行为。修法：控件高度贴合字体行高（上下各留 3px），再整体垂直居中。
static int SnugEditHeight(HFONT hf) {
    HDC dc = GetDC(nullptr);
    if (!dc) return 22;
    HGDIOBJ old = SelectObject(dc, hf ? (HGDIOBJ)hf : GetStockObject(DEFAULT_GUI_FONT));
    TEXTMETRICW tm;
    ZeroMemory(&tm, sizeof(tm));
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    return (int)tm.tmHeight + 6;
}

static void InitFonts() {
    // 选择可用字体族：优先“Microsoft YaHei UI”，回退“Microsoft YaHei”，
    // 再回退到 GDI+ 通用无衬线字体。注意 Gdiplus::FontFamily 的拷贝赋值运算符
    // 是私有的，因此用指针选择，而不是直接赋值给局部对象。
    FontFamily famUI(L"Microsoft YaHei UI");
    FontFamily famYaHei(L"Microsoft YaHei");
    const FontFamily* pfam = &famUI;
    if (!famUI.IsAvailable()) {
        if (famYaHei.IsAvailable()) pfam = &famYaHei;
        else pfam = FontFamily::GenericSansSerif();
    }
    // 字体放大两号（原 12/13px → 14/15px），界面同步加高重排。
    F12  = new Font(pfam, 14, FontStyleRegular, UnitPixel);
    F12B = new Font(pfam, 14, FontStyleBold,    UnitPixel);
    F13  = new Font(pfam, 15, FontStyleRegular, UnitPixel);
    F13B = new Font(pfam, 15, FontStyleBold,    UnitPixel);
    // 原生控件（下拉框 / 编辑框）配套 GDI 字体，与自绘文字大小一致。
    s_hFontUI = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                            L"Microsoft YaHei UI");
    if (!s_hFontUI) s_hFontUI = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

// 配色
static const Color C_TEXT  (255,  17,  24,  39);
static const Color C_SUB   (255, 107, 114, 128);
static const Color C_ACCENT(255,  37,  99, 235);
static const Color C_ACCENT_D(255, 29, 78, 216);
static const Color C_BORDER(255, 229, 231, 235);
static const Color C_WHITE (255, 255, 255, 255);
static const Color C_HOVER (255, 249, 250, 251);
static const Color C_KEYBG (255, 243, 244, 246);
static const Color C_KEYBD (255, 209, 213, 219);
static const Color C_TRACK (255, 229, 231, 235);

// 绘制辅助
static void FillRR(Graphics& g, const RectF& rc, float r, const Color& c) {
    GraphicsPath p; float d = 2 * r;
    p.AddArc(rc.X, rc.Y, d, d, 180, 90);
    p.AddArc(rc.GetRight() - d, rc.Y, d, d, 270, 90);
    p.AddArc(rc.GetRight() - d, rc.GetBottom() - d, d, d, 0, 90);
    p.AddArc(rc.X, rc.GetBottom() - d, d, d, 90, 90);
    p.CloseFigure();
    SolidBrush b(c); g.FillPath(&b, &p);
}
static void FrameRR(Graphics& g, const RectF& rc, float r, const Color& c, float w = 1.0f) {
    GraphicsPath p; float d = 2 * r;
    p.AddArc(rc.X, rc.Y, d, d, 180, 90);
    p.AddArc(rc.GetRight() - d, rc.Y, d, d, 270, 90);
    p.AddArc(rc.GetRight() - d, rc.GetBottom() - d, d, d, 0, 90);
    p.AddArc(rc.X, rc.GetBottom() - d, d, d, 90, 90);
    p.CloseFigure();
    Pen pen(c, w); g.DrawPath(&pen, &p);
}
static void Txt(Graphics& g, const wchar_t* s, Font& f, const Color& c,
                const RectF& rc, bool center) {
    SolidBrush b(c);
    StringFormat sf;
    if (center) sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(s, -1, &f, rc, &sf, &b);
}

// 快捷键显示
static std::wstring VkName(UINT vk) {
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    wchar_t b[64] = {0};
    if (sc) GetKeyNameTextW((LONG)(sc << 16), b, 64);
    if (!b[0]) swprintf(b, 64, L"Key %u", vk);
    return b;
}
static std::vector<std::wstring> HotkeyParts(UINT mod, UINT vk) {
    std::vector<std::wstring> v;
    if (mod & MOD_CONTROL) v.push_back(L"Ctrl");
    if (mod & MOD_ALT)     v.push_back(L"Alt");
    if (mod & MOD_SHIFT)   v.push_back(L"Shift");
    if (mod & MOD_WIN)     v.push_back(L"Win");
    if (vk != 0) v.push_back(VkName(vk));
    return v;
}
static float DrawKey(Graphics& g, Font& f, const wchar_t* t, float x, float y) {
    RectF bb; g.MeasureString(t, -1, &f, PointF(0, 0), &bb);
    float w = bb.Width + 24, h = 30;
    RectF rc(x, y, w, h);
    FillRR(g, rc, 7, C_KEYBG);
    FrameRR(g, rc, 7, C_KEYBD);
    Txt(g, t, f, C_TEXT, rc, true);
    return w;
}

//---------------- 控件外观 ----------------
static bool g_capturing = false;   // 是否正在录制快捷键
static int  g_capTarget = 0;       // 录制目标：0=呼出快捷键（启动页）, 1=截图快捷键（截图页）

static void DrawToggle(Graphics& g, const RectF& rc, bool on) {
    FillRR(g, rc, rc.Height / 2, on ? C_ACCENT : C_TRACK);
    float d = rc.Height - 6;
    float kx = on ? rc.GetRight() - d - 3 : rc.X + 3;
    FillRR(g, RectF(kx, rc.Y + 3, d, d), d / 2, C_WHITE);
}
static void DrawHotRow(Graphics& g, const RectF& rc, bool hot, int target) {
    // 本行对应的快捷键数据：
    //   0=弹出快捷键（启动页） 1=截图快捷键（截图页） 2=搜索快捷键（搜索页）
    //   3=搜索框快捷键（启动页，聚焦主界面搜索框）
    const UINT mod = (target == 1) ? g_s.capHkMod
                   : (target == 2) ? g_s.searchHkMod
                   : (target == 3) ? g_s.sboxMod : g_s.hkMod;
    const UINT vk  = (target == 1) ? g_s.capHkVk
                   : (target == 2) ? g_s.searchHkVk
                   : (target == 3) ? g_s.sboxVk : g_s.hkVk;
    const bool capturing = g_capturing && (g_capTarget == target);
    if (capturing) {
        FillRR(g, rc, 10, Color(255, 239, 246, 255));
        FrameRR(g, rc, 10, C_ACCENT);
        if (mod == 0 && vk == 0) {
            // 尚未按下任何键：提示
            Txt(g, L"按下新的组合键…    Esc 取消", *F13, C_ACCENT, rc, true);
        } else {
            // 实时显示已按下的键帽
            float x = rc.X + 18, ky = rc.Y + (rc.Height - 30) / 2;
            auto parts = HotkeyParts(mod, vk);
            for (size_t i = 0; i < parts.size(); i++) {
                if (i) { Txt(g, L"+", *F13, C_SUB, RectF(x, ky, 16, 30), true); x += 18; }
                x += DrawKey(g, *F12B, parts[i].c_str(), x, ky) + 8;
            }
        }
        return;
    }
    FillRR(g, rc, 10, hot ? C_HOVER : C_WHITE);
    FrameRR(g, rc, 10, C_BORDER);
    if (mod == 0 && vk == 0) {
        // 未设置：提示用户点击录制（空快捷键 = 不需要快捷键启动）
        Txt(g, L"未设置（点击录制）", *F13, C_SUB,
            RectF(rc.X + 18, rc.Y, rc.Width - 96, rc.Height), false);
    } else {
        float x = rc.X + 18, ky = rc.Y + (rc.Height - 30) / 2;
        auto parts = HotkeyParts(mod, vk);
        for (size_t i = 0; i < parts.size(); i++) {
            if (i) { Txt(g, L"+", *F13, C_SUB, RectF(x, ky, 16, 30), true); x += 18; }
            x += DrawKey(g, *F12B, parts[i].c_str(), x, ky) + 8;
        }
    }
    Txt(g, L"更改", *F13, C_ACCENT, RectF(rc.GetRight() - 76, rc.Y, 62, rc.Height), true);
}
static void DrawRadio(Graphics& g, const RectF& rc, bool sel, const wchar_t* text, bool hot) {
    if (hot) FillRR(g, rc, 8, C_HOVER);
    float cx = rc.X + 17, cy = rc.Y + rc.Height / 2;
    RectF o(cx - 9, cy - 9, 18, 18);
    FillRR(g, o, 9, C_WHITE);
    FrameRR(g, o, 9, sel ? C_ACCENT : C_KEYBD, sel ? 1.6f : 1.0f);
    if (sel) FillRR(g, RectF(cx - 5.5f, cy - 5.5f, 11, 11), 5.5f, C_ACCENT);
    Txt(g, text, *F13, C_TEXT, RectF(rc.X + 38, rc.Y, rc.Width - 50, rc.Height), false);
}
static void DrawStepper(Graphics& g, const RectF& rc, bool hot) {
    // 整体圆角矩形（白底 + 边框）
    FillRR(g, rc, 8, hot ? C_HOVER : C_WHITE);
    FrameRR(g, rc, 8, C_BORDER);
    // 左右方形：黑色 − / +（比灰色更醒目）
    Txt(g, L"−", *F13B, C_TEXT, RectF(rc.X, rc.Y, 40, rc.Height), true);
    Txt(g, L"+", *F13B, C_TEXT, RectF(rc.GetRight() - 40, rc.Y, 40, rc.Height), true);
    // 中间区域：数字上下左右居中（编辑时由 ID_EDIT 覆盖显示）
    wchar_t v[16]; swprintf(v, 16, L"%d", g_s.hideSec);
    Txt(g, v, *F13, C_TEXT, RectF(rc.X + 40, rc.Y, 66, rc.Height), true);
    // 三块之间的细微分隔线
    Pen pen(C_BORDER, 1.0f);
    g.DrawLine(&pen, rc.X + 40, rc.Y + 6, rc.X + 40, rc.GetBottom() - 6);
    g.DrawLine(&pen, rc.GetRight() - 40, rc.Y + 6, rc.GetRight() - 40, rc.GetBottom() - 6);
}
static void DrawSlider(Graphics& g, const RectF& rc) {
    // 0-500 毫秒滑杆：4 段（步进 125），只显示 0 / 250 毫秒 / 500 毫秒 三个刻度标签
    const int steps = 4;
    float pad = 16.0f;
    float span = rc.Width - 2 * pad;
    float ty = 14.0f;                        // 轨道中心 y
    float pos[5];
    for (int i = 0; i <= steps; i++) pos[i] = rc.X + pad + span * i / steps;
    // 轨道（灰） + 已填充段（蓝）
    FillRR(g, RectF(rc.X + pad, ty - 2, span, 4), 2, C_TRACK);
    float cur = rc.X + pad + span * (g_s.holdMs / 500.0f);
    if (cur > rc.X + pad + 0.5f)
        FillRR(g, RectF(rc.X + pad, ty - 2, cur - (rc.X + pad), 4), 2, C_ACCENT);
    // 中间刻度点（i=1..3）
    for (int i = 1; i < steps; i++) {
        RectF d(pos[i] - 2.5f, ty - 2.5f, 5, 5);
        FillRR(g, d, 2.5f, C_WHITE);
        FrameRR(g, d, 2.5f, C_BORDER, 1.0f);
    }
    // 滑块（白底蓝边圆点）
    RectF k(cur - 8, ty - 8, 16, 16);
    FillRR(g, k, 8, C_WHITE);
    FrameRR(g, k, 8, C_ACCENT, 2.0f);
    // 刻度标签：只显示 0 / 250 毫秒 / 500 毫秒
    Txt(g, L"0",        *F12, C_SUB, RectF(pos[0] - 40, 26, 80, 20), true);
    Txt(g, L"250 毫秒", *F12, C_SUB, RectF(pos[2] - 60, 26, 120, 20), true);
    Txt(g, L"500 毫秒", *F12, C_SUB, RectF(pos[4] - 60, 26, 120, 20), true);   // 原 60 → 120：中心偏左 30px 且可能折行
}

static void DrawBtn(Graphics& g, const RectF& rc, const wchar_t* t, bool primary, bool hot) {
    if (primary) {
        FillRR(g, rc, 8, hot ? C_ACCENT_D : C_ACCENT);
        Txt(g, t, *F13B, C_WHITE, rc, true);
    } else {
        FillRR(g, rc, 8, hot ? C_HOVER : C_WHITE);
        FrameRR(g, rc, 8, C_BORDER);
        Txt(g, t, *F13, C_TEXT, rc, true);
    }
}

//---------------- 窗口 ----------------
enum {
    ID_TG1 = 1001, ID_TG_AUTO, ID_HOT, ID_RAD1, ID_RAD2, ID_STEP, ID_EDIT,
    ID_COMBO, ID_SLIDER, ID_ADD, ID_CANCEL, ID_SAVE,
    ID_HOT_SHOT,      // 截图快捷键整行（截图分组）
    ID_RAD_SHOT1,     // 识图：启动识图（截图后弹出图片识图）
    ID_RAD_SHOT2,     // 识图：直接截图（截图后直接复制图片）
    ID_TG_WRAP,       // 换行模式开关（智能换行）
    ID_RAD_EXIT1,     // 退出方式：复制文字后直接退出
    ID_RAD_EXIT2,     // 退出方式：复制完文字后 ESC 退出
    ID_TG_SEARCH,     // 搜索页：开启搜索开关
    ID_TG_PREVIEW,    // 搜索页：开启预览开关
    ID_HOT_SEARCH,    // 搜索页：搜索快捷键整行
    ID_HOT_SBOX       // 启动页：搜索框快捷键整行（Alt+F 聚焦主界面搜索框）
};

// 记录目标(0=弹出 1=截图 2=搜索 3=搜索框) 对应的快捷键控件 ID，供各处理分支统一取句柄。
static int HotRowId(int t) {
    switch (t) {
        case 1:  return ID_HOT_SHOT;
        case 2:  return ID_HOT_SEARCH;
        case 3:  return ID_HOT_SBOX;
        default: return ID_HOT;
    }
}
static const int kW = 460, kH = 806;   // 启动页新增「搜索框快捷键」一行，窗口相应加高 98px

//---------------- 分页（启动 / 截图 / 搜索）----------------
// 顶部胶囊标签栏：选中页浅灰胶囊 + 深色字，未选中页灰字（与主面板标签风格一致）。
static int    g_page = 0;                 // 0=启动 1=截图 2=搜索
static RectF  g_tabRects[3];              // 三个标签的命中矩形（与绘制同一矩形）
static const wchar_t* kTabNames[3] = { L"启动", L"截图", L"搜索" };

static void DrawTabs(Graphics& g) {
    const Color kTabOn (255, 226, 228, 232);   // 选中胶囊底（#E2E4E8，同主面板激活标签）
    float x = 24, y = 12, w = 68, h = 30, gap = 8;
    for (int i = 0; i < 3; i++) {
        RectF rc(x, y, w, h);
        g_tabRects[i] = rc;
        if (g_page == i) {
            FillRR(g, rc, 15, kTabOn);
            Txt(g, kTabNames[i], *F13B, C_TEXT, rc, true);
        } else {
            Txt(g, kTabNames[i], *F13, C_SUB, rc, true);
        }
        x += w + gap;
    }
}

// 切页时显隐对应控件（底部 取消/保存 两键跨页共享，不参与）。
static void ApplyPageVisibility(HWND hwnd) {
    struct { int id; int page; } map[] = {
        { ID_TG_AUTO, 0 }, { ID_TG1, 0 }, { ID_COMBO, 0 }, { ID_SLIDER, 0 },
        { ID_HOT, 0 }, { ID_RAD1, 0 }, { ID_RAD2, 0 }, { ID_STEP, 0 },
        { ID_EDIT, 0 }, { ID_ADD, 0 }, { ID_HOT_SBOX, 0 },
        { ID_HOT_SHOT, 1 }, { ID_RAD_SHOT1, 1 }, { ID_RAD_SHOT2, 1 }, { ID_TG_WRAP, 1 },
        { ID_RAD_EXIT1, 1 }, { ID_RAD_EXIT2, 1 },
        { ID_TG_SEARCH, 2 }, { ID_TG_PREVIEW, 2 }, { ID_HOT_SEARCH, 2 },
    };
    for (auto& e : map) {
        HWND c = GetDlgItem(hwnd, e.id);
        if (c) ShowWindow(c, (e.page == g_page) ? SW_SHOW : SW_HIDE);
    }
}

// 仅刷新某个控件自身（避免整窗重绘造成闪烁），异步失效即可 —— 每个绘制入口都
// 在读 g_s 活值、没有任何绘制缓存，所以"失效的像素必定以最新值重画"。
// 原先这里用 RDW_UPDATENOW 同步强刷，是为了掩盖"第一次点击状态没刷新"；那个现象
// 的真正原因是 WM_COMMAND 未过滤通知码，一次点击被翻转两次（见 WM_COMMAND 注释）。
// 更关键的是：窗口当时带着 WS_EX_COMPOSITED（整窗重定向合成），而这里只请求
// 刷新单个子控件 —— 两个绘制目标共用一个合成表面、谁覆盖谁没有契约，
// 这正是"屏幕显示与真实设置反向"（最新一帧没上屏）的土壤。两处现已一并改掉。
static void PersistCtrl(HWND hwnd, int id) {
    Save();
    ApplySettings();
    HWND c = GetDlgItem(hwnd, id);
    if (c) InvalidateRect(c, nullptr, FALSE);
}
// 仅持久化“失焦隐藏秒数”的最小提交路径：只写 misc/hide 单键，并通过轻量回调
// 只重置隐藏计时（不重注册热键、不碰开机自启、不重绘主面板）。
static void PersistHideSecOnly(HWND hwndStep) {
    std::wstring f = IniPath(); wchar_t b[16];
    swprintf(b, 16, L"%d", g_s.hideSec);
    WritePrivateProfileStringW(L"misc", L"hide", b, f.c_str());
    if (g_applyAutoHideCb) g_applyAutoHideCb();
    if (hwndStep) InvalidateRect(hwndStep, nullptr, FALSE);
}
static WNDPROC s_editOldProc = nullptr; // 数字编辑框子类：原窗口过程
static WNDPROC s_stepOldProc = nullptr; // 步进器按钮子类：原窗口过程
static WNDPROC s_sliderOldProc = nullptr; // 滑杆子类：原窗口过程

// 数字编辑态：用显式标志当唯一状态源。
// 旧实现把 IsWindowVisible(EDIT) 兼作“是否在编辑”，并在 WM_KILLFOCUS 里隐藏窗口——
// 隐藏“当前焦点窗口”会再触发一次 WM_KILLFOCUS，重入与否取决于 ShowWindow 内部的
// 清标志与发焦点消息的先后（未规定），这是“偶尔重入 / 偶尔丢值”的来源。
// 改成显式标志 + 重入闸后，行为是确定的。
static bool s_editActive    = false;   // 正在编辑数字
static bool s_editMouseDown = false;   // 编辑框自己收到过 WM_LBUTTONDOWN
static bool s_inCommit      = false;   // 提交/取消重入闸
static const UINT WM_APP_BEGIN_HIDE_EDIT = WM_APP + 0x41;   // 延迟进入编辑（避开按钮消息栈）

// 进入数字编辑：显示编辑框、置于顶层、给焦点、全选（系统默认蓝底白字）。
// 由“步进器中段的第二次鼠标抬起”触发（见 StepSubProc），此时鼠标已经完全松开，
// 不存在“残留的松开事件把全选打掉”的路径 —— 旧实现里按 400ms 时间窗吞事件的
// hack 因此整段删除（它还会吞掉用户进编辑后 400ms 内的真实框内点击）。
static void BeginHideEdit(HWND hwnd) {
    HWND hed = GetDlgItem(hwnd, ID_EDIT);
    if (!hed || s_editActive) return;
    wchar_t v[16]; swprintf(v, 16, L"%d", g_s.hideSec);
    SetWindowTextW(hed, v);
    SendMessageW(hed, EM_SETLIMITTEXT, 8, 0);      // 限制长度，避免超长输入被静默截断
    SendMessageW(hed, EM_EMPTYUNDOBUFFER, 0, 0);   // 不让 Ctrl+Z 把旧值恢复回来
    SetWindowPos(hed, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    s_editActive    = true;
    s_editMouseDown = false;
    ShowWindow(hed, SW_SHOW);
    SetFocus(hed);
    SendMessageW(hed, EM_SETSEL, 0, -1);           // 全选，直接输入即覆盖
}
// 取消数字编辑（Esc / 非法输入 / 取消按钮）：丢弃输入、恢复原值显示，不写盘。
static void CancelHideEdit(HWND hwnd) {
    if (!s_editActive || s_inCommit) return;
    s_inCommit = true;
    s_editActive = false;
    s_editMouseDown = false;
    HWND hed = GetDlgItem(hwnd, ID_EDIT);
    if (hed && GetFocus() == hed) SetFocus(hwnd);  // 先交还焦点再隐藏，避免焦点落空
    if (hed) ShowWindow(hed, SW_HIDE);
    HWND st = GetDlgItem(hwnd, ID_STEP);
    if (st) InvalidateRect(st, nullptr, FALSE);
    s_inCommit = false;
}
// 提交数字编辑（回车 / 失焦 / 点框外 / 保存）：严格解析 → 越界钳制 → 写回并落盘。
// 规则：空 → 0（0 表示关闭该功能）；非数字或带多余字符 → 视为非法，丢弃并恢复原值；
//       <0 → 0；>60 → 60。
static void CommitHideEdit(HWND hwnd) {
    if (!s_editActive || s_inCommit) return;
    s_inCommit = true;
    HWND hed = GetDlgItem(hwnd, ID_EDIT);
    wchar_t buf[16] = {0};
    if (hed) GetWindowTextW(hed, buf, 16);
    int v = 0;
    if (buf[0] != 0) {
        // 严格整数解析：用 wcstoll 而非 swscanf("%d")（后者在超长数字上溢出为 UB）。
        wchar_t* end = nullptr;
        long long ll = wcstoll(buf, &end, 10);
        if (end == buf || *end != 0) {          // 非法输入 → 丢弃，恢复原值
            s_inCommit = false;
            CancelHideEdit(hwnd);
            return;
        }
        if (ll < 0)  ll = 0;                    // 越界钳制：-3 → 0
        if (ll > 60) ll = 60;                   // 越界钳制：999 → 60
        v = static_cast<int>(ll);
    }
    g_s.hideSec = v;
    s_editActive = false;
    s_editMouseDown = false;
    if (hed && GetFocus() == hed) SetFocus(hwnd);
    if (hed) ShowWindow(hed, SW_HIDE);
    PersistHideSecOnly(GetDlgItem(hwnd, ID_STEP));
    s_inCommit = false;
}
// 数字编辑框子类过程：回车提交，Esc 取消，失焦提交。
static LRESULT CALLBACK EditSubProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    // 吞掉回车/换行/制表/Esc 的字符消息，避免系统提示音。
    // 必须无条件前置：TranslateMessage 是把 WM_CHAR 投回队列而非立即派发，
    // 它到达时编辑框可能已经被隐藏，放进下面的状态闸里就等于失效。
    // （注意 WM_CHAR 的 wParam 是字符码，不是虚拟键码。）
    if (m == WM_CHAR && (wp == L'\r' || wp == L'\n' || wp == L'\t' || wp == 0x1B))
        return 0;
    if (!s_editActive) {
        return s_editOldProc ? CallWindowProcW(s_editOldProc, w, m, wp, lp)
                             : DefWindowProcW(w, m, wp, lp);
    }
    if (m == WM_KEYDOWN) {
        if (wp == VK_RETURN) { CommitHideEdit(GetParent(w)); return 0; }
        if (wp == VK_ESCAPE) { CancelHideEdit(GetParent(w)); return 0; }
    }
    if (m == WM_LBUTTONDOWN) {
        s_editMouseDown = true;                   // 编辑框内的真实点击
    } else if (m == WM_LBUTTONUP) {
        if (!s_editMouseDown) {                   // 没有对应按下的松开 → 残留事件，保住全选
            SendMessageW(w, EM_SETSEL, 0, -1);
            return 0;
        }
        s_editMouseDown = false;
    } else if (m == WM_KILLFOCUS) {
        // 顺序沿用原实现：先让编辑框完成默认处理，再提交（提交内用显式标志 + 重入闸，
        // 不再依赖 IsWindowVisible，重入是安全的）。
        LRESULT r = s_editOldProc ? CallWindowProcW(s_editOldProc, w, m, wp, lp)
                                  : DefWindowProcW(w, m, wp, lp);
        CommitHideEdit(GetParent(w));
        return r;
    }
    return s_editOldProc ? CallWindowProcW(s_editOldProc, w, m, wp, lp)
                         : DefWindowProcW(w, m, wp, lp);
}

// 步进器按钮子类：± 单击生效，中段“双击”进入数字编辑。
// 双击判定改成“中段两次抬起 + 时间/距离”，不再依赖系统 WM_LBUTTONDBLCLK：
//   系统双击矩形默认只有 4×4 物理像素（150% 缩放下≈2.7 逻辑像素），手稍抖就判不成
//   双击；而“抬起”事件在任何情况下都会到达，用它配对是确定的。
// 也因为编辑框是在“第二次抬起之后”才打开，出现时鼠标已经松开，不存在“残留的松开
// 事件把全选打掉”的路径，旧实现里按 400ms 吞事件的 hack 因此整段删除。
static DWORD s_stepDownTick      = 0;    // 最近一次按下的时刻（判双击 ± 用）
static int   s_stepDownRegion    = -1;   // 按下的分区：1=−左, 2=+右, 0=中段
static bool  s_stepDoublePending = false; // 本击是双击的第二击（同侧），松开时跳过 ±
static bool  s_stepUpArmed       = false; // 中段已记录过一次抬起
static DWORD s_stepUpTick        = 0;
static int   s_stepUpX           = -1;

static LRESULT CALLBACK StepSubProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    HWND parent = GetParent(w);
    int x = (int)(short)LOWORD(lp);             // 按钮客户区 x 坐标
    int region = (x < 40) ? 1 : (x >= 106 ? 2 : 0);
    bool beginEdit = false;

    if (m == WM_LBUTTONDOWN) {
        DWORD now = GetTickCount();
        // 与上一击同侧且间隔 < 双击阈值 → 判定为双击的第二击，松开时跳过一次 ±
        if (region != 0 && s_stepDownRegion == region && s_stepDownTick != 0 &&
            (now - s_stepDownTick) <= GetDoubleClickTime())
            s_stepDoublePending = true;
        s_stepDownTick = now; s_stepDownRegion = region;
        // 编辑态下点 ±：先把输入框里的值提交，否则提交时会用旧文本把 ± 的结果覆盖掉
        if (s_editActive && region != 0) CommitHideEdit(parent);
    } else if (m == WM_LBUTTONUP) {
        // 抬起时必须与“按下时的分区”一致才算数：按下在 −、拖到 + 再松手不能变成 +1
        bool sameRegion = (region == s_stepDownRegion);
        if (sameRegion && region == 1) {          // 左侧方形：−
            if (!s_stepDoublePending) {
                int v = g_s.hideSec - 1;
                if (v < 0) v = 0;
                if (v > 60) v = 60;
                g_s.hideSec = v;
                PersistHideSecOnly(GetDlgItem(parent, ID_STEP));
            } else s_stepDoublePending = false;
        } else if (sameRegion && region == 2) {   // 右侧方形：+
            if (!s_stepDoublePending) {
                int v = g_s.hideSec + 1;
                if (v < 0) v = 0;
                if (v > 60) v = 60;
                g_s.hideSec = v;
                PersistHideSecOnly(GetDlgItem(parent, ID_STEP));
            } else s_stepDoublePending = false;
        } else if (region == 0 && s_stepDownRegion == 0) {   // 中段：两次抬起 = 双击
            DWORD now = GetTickCount();
            int tol = GetSystemMetrics(SM_CXDOUBLECLK) / 2 + 2;
            if (tol < 8) tol = 8;                 // 手抖容差至少 8px
            int dx = x - s_stepUpX; if (dx < 0) dx = -dx;
            if (s_stepUpArmed && (now - s_stepUpTick) <= GetDoubleClickTime() && dx <= tol) {
                s_stepUpArmed = false;
                beginEdit = true;
            } else {
                s_stepUpArmed = true; s_stepUpTick = now; s_stepUpX = x;
            }
        } else {
            s_stepDoublePending = false;          // 跨区松手：什么都不做
        }
    } else if (m == WM_LBUTTONDBLCLK) {
        // 系统合成双击时第二击是 DBLCLK 而不是 DOWN：补上 ± 的双击门控，
        // 避免“双击 + 加两次”；中段不在这里进编辑（统一走上面的抬起配对）。
        if (region != 0) s_stepDoublePending = true;
    }
    // 先让按钮完成默认处理（释放鼠标捕获、更新按下态），再把“进入编辑”投递到消息
    // 循环处理，确保按钮状态机已经收尾，不会在消息栈深处把焦点抢回去。
    LRESULT r = (!s_stepOldProc) ? DefWindowProcW(w, m, wp, lp)
                                 : CallWindowProcW(s_stepOldProc, w, m, wp, lp);
    if (beginEdit) PostMessageW(parent, WM_APP_BEGIN_HIDE_EDIT, 0, 0);
    return r;
}

// 根据当前 holdMs 刷新下拉框选项文本（按下… / 长按… / 取消鼠标键呼出）
static void UpdateComboTexts(HWND hwnd) {
    HWND h = GetDlgItem(hwnd, ID_COMBO);
    if (!h) return;
    int sel = (int)SendMessageW(h, CB_GETCURSEL, 0, 0);
    static const wchar_t* press[3] = { L"按下鼠标中键", L"按下鼠标向后侧键", L"按下鼠标向前侧键" };
    static const wchar_t* hold [3] = { L"长按鼠标中键", L"长按鼠标向后侧键", L"长按鼠标向前侧键" };
    SendMessageW(h, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < 3; i++)
        SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)(g_s.holdMs > 0 ? hold[i] : press[i]));
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"取消鼠标键呼出");   // mouseBtn = 3
    SendMessageW(h, CB_SETCURSEL, (sel >= 0 && sel <= 3) ? sel : g_s.mouseBtn, 0);
}

// 由 x 坐标换算滑杆取值：0-500，就近吸附到 4 段（0/125/250/375/500）
static int SliderValueFromX(HWND w, int x) {
    RECT rc; GetClientRect(w, &rc);
    float pad = 16.0f;
    float span = (float)rc.right - 2 * pad;
    if (span <= 0) return g_s.holdMs;
    float frac = ((float)x - pad) / span;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    int step = (int)(frac * 4.0f + 0.5f);   // 0..4
    return step * 125;
}

// 滑杆子类过程：单击/拖动设置长按毫秒
static LRESULT CALLBACK SliderSubProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    bool drag = false;
    if (m == WM_LBUTTONDOWN) { SetCapture(w); drag = true; }
    else if (m == WM_MOUSEMOVE && (wp & MK_LBUTTON)) drag = true;
    else if (m == WM_LBUTTONUP) { if (GetCapture() == w) ReleaseCapture(); }
    if (drag) {
        HWND parent = GetParent(w);
        int x = (int)(short)LOWORD(lp);
        int v = SliderValueFromX(w, x);
        if (v != g_s.holdMs) {
            g_s.holdMs = v;
            // 仅写 mouse/hold 单键；钩子实时读 g_s.holdMs，无需通知主窗口重绘 / 重注册。
            std::wstring f = IniPath(); wchar_t b[16];
            swprintf(b, 16, L"%d", g_s.holdMs);
            WritePrivateProfileStringW(L"mouse", L"hold", b, f.c_str());
            UpdateComboTexts(parent);   // 同步"按下/长按"前缀
            RedrawWindow(w, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        }
    }
    if (!s_sliderOldProc) return DefWindowProcW(w, m, wp, lp);
    return CallWindowProcW(s_sliderOldProc, w, m, wp, lp);
}

static HWND Btn(HWND parent, int id, int x, int y, int w, int h) {
    // 注意：这里不能带 BS_NOTIFY。带上它之后按钮除 BN_CLICKED 还会向父窗口发
    // BN_SETFOCUS / BN_KILLFOCUS，而 WM_COMMAND 是按控件 ID 分发的——"点击"会被
    // 当成两次执行（见 WM_COMMAND 顶部注释）。本窗口没有任何地方需要焦点通知。
    return CreateWindowExW(0, L"BUTTON", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_OWNERDRAW,
        x, y, w, h, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
}

static void CreateControls(HWND hwnd) {
    // ---- 启动页（原内容整体下移 48px，为顶部标签栏腾位）----
    Btn(hwnd, ID_TG_AUTO, 146, 98, 46, 26);   // 开机自启（与“记住窗口尺寸”同栏，置于首位）
    Btn(hwnd, ID_TG1,    360, 98, 46, 26);   // 记住窗口尺寸（与左侧文字垂直居中）
    // 呼出鼠标键下拉框（替代原“鼠标中键呼出”拨杆开关）
    HWND hCombo = CreateWindowExW(0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
        240, 176, 196, 200,
        hwnd, (HMENU)(INT_PTR)ID_COMBO, GetModuleHandleW(nullptr), nullptr);
    if (hCombo) {
        SendMessageW(hCombo, WM_SETFONT, (WPARAM)s_hFontUI, TRUE);
        UpdateComboTexts(hwnd);
        SendMessageW(hCombo, CB_SETCURSEL, g_s.mouseBtn, 0);
    }
    // 长按毫秒滑杆（0-500，4 段，步进 125；0 = 按下即触发）
    HWND hSlider = Btn(hwnd, ID_SLIDER, 24, 234, kW - 48, 48);
    if (hSlider) s_sliderOldProc = (WNDPROC)SetWindowLongPtrW(hSlider, GWLP_WNDPROC, (LONG_PTR)SliderSubProc);
    Btn(hwnd, ID_HOT,   24, 320, kW - 48,  54);    // 弹出快捷键整行（点击录键）
    Btn(hwnd, ID_HOT_SBOX, 24, 414, kW - 48, 54);  // 搜索框快捷键整行（默认 Alt+F）
    Btn(hwnd, ID_RAD1,  24, 508, kW - 48,  42);    // 左键单击启动应用
    Btn(hwnd, ID_RAD2,  24, 554, kW - 48,  42);    // 左键双击启动应用
    // 步进器：整体 146×36，左 40×36 方（−）、中 66 宽（数字）、右 40×36 方（+）
    // 与左侧“失焦自动隐藏”文字同一 y，垂直居中对齐。
    HWND hStep = Btn(hwnd, ID_STEP,  290, 632, 146, 36);
    if (hStep) s_stepOldProc = (WNDPROC)SetWindowLongPtrW(hStep, GWLP_WNDPROC, (LONG_PTR)StepSubProc);
    // 中间数字编辑框：默认隐藏，单击步进器中段数字区域即进入编辑；位于步进器中段
    // 不使用 ES_NUMBER：允许输入负号（如 -3 → 钳制为 0）与空值，交由严格解析校验
    // （空/非法 → 取消恢复原值；<0 → 0；>60 → 60），完整满足需求。
    // 高度贴合字体行高并在 36px 的步进器里垂直居中 —— 直接把 EDIT 拉高到 36px 会让
    // 数字紧贴上沿（单行 EDIT 顶端对齐，见 SnugEditHeight 注释）。
    const int kStepY = 632, kStepH = 36, kStepMidX = 330, kStepMidW = 66;
    int editH = SnugEditHeight(s_hFontUI);
    if (editH > kStepH) editH = kStepH;
    if (editH < 12) editH = 12;
    int editY = kStepY + (kStepH - editH) / 2;
    HWND hed = CreateWindowExW(0, L"EDIT", nullptr,
        WS_CHILD | WS_BORDER | ES_CENTER,
        kStepMidX, editY, kStepMidW, editH,
        hwnd, (HMENU)(INT_PTR)ID_EDIT, GetModuleHandleW(nullptr), nullptr);
    if (hed) {
        SendMessageW(hed, WM_SETFONT, (WPARAM)s_hFontUI, TRUE);
        ShowWindow(hed, SW_HIDE);
        s_editOldProc = (WNDPROC)SetWindowLongPtrW(hed, GWLP_WNDPROC, (LONG_PTR)EditSubProc);
    }
    Btn(hwnd, ID_ADD,   24, 680, 260, 36);         // 添加回收站

    // ---- 截图页 ----
    Btn(hwnd, ID_HOT_SHOT, 24, 96, kW - 48, 54);   // 截图快捷键整行（默认 Alt+A），上移补位
    Btn(hwnd, ID_RAD_SHOT1, 24, 210, kW - 48, 42); // 识图：启动识图
    Btn(hwnd, ID_RAD_SHOT2, 24, 256, kW - 48, 42); // 识图：直接截图
    Btn(hwnd, ID_TG_WRAP, 360, 336, 46, 26);       // 换行模式：智能换行开关
    Btn(hwnd, ID_RAD_EXIT1, 24, 426, kW - 48, 42); // 退出方式：复制后直接退出
    Btn(hwnd, ID_RAD_EXIT2, 24, 472, kW - 48, 42); // 退出方式：复制后 ESC 退出

    // ---- 搜索页 ----
    Btn(hwnd, ID_TG_SEARCH,  360, 98,  46, 26);     // 开启搜索开关
    Btn(hwnd, ID_TG_PREVIEW, 360, 148, 46, 26);     // 开启预览开关
    Btn(hwnd, ID_HOT_SEARCH, 24, 224, kW - 48, 54); // 搜索快捷键整行（默认 Ctrl+F）

    // ---- 跨页共享 ----
    Btn(hwnd, ID_CANCEL,kW - 232, 740, 96, 38);
    Btn(hwnd, ID_SAVE,  kW - 120, 740, 96, 38);

    ApplyPageVisibility(hwnd);   // 按当前页显隐控件
}

static void DrawSection(Graphics& g, float y, const wchar_t* name) {
    FillRR(g, RectF(24, y + 3, 4, 14), 2, C_ACCENT);
    Txt(g, name, *F13B, C_TEXT, RectF(36, y, 200, 20), false);
}

// 客户区静态内容绘制（分组标题、说明文字、按钮之间的分隔线）。
// 单独成函数的目的：让 Graphics 在函数返回时析构，保证它先于 EndPaint(hdc) 释放 ——
// 原先 Graphics 是 WM_PAINT 里的局部对象，析构发生在 return 处（EndPaint 之后），
// 而 EndPaint 之后该 HDC 已失效，属于“依赖实现细节”的灰区用法。
static void PaintClient(HDC hdc) {
    Graphics g(hdc); g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.Clear(C_WHITE);

    DrawTabs(g);   // 顶部「启动 / 截图」标签栏（两页共享）

    if (g_page == 0) {
        // ---- 启动页 ----
        DrawSection(g, 68,  L"常规");
        Txt(g, L"开机自启", *F13, C_TEXT, RectF(24, 96, 110, 30), false);
        Txt(g, L"窗口跟随鼠标", *F13, C_TEXT, RectF(214, 96, 130, 30), false);

        DrawSection(g, 144, L"呼出方式");
        Txt(g, L"设置鼠标键弹出", *F13, C_TEXT, RectF(24, 174, 210, 30), false);
        Txt(g, L"长按时间", *F13, C_TEXT, RectF(24, 214, 210, 24), false);
        Txt(g, L"弹出快捷键", *F13B, C_TEXT, RectF(24, 292, 260, 24), false);
        Txt(g, L"搜索框快捷键", *F13B, C_TEXT, RectF(24, 386, 260, 24), false);

        DrawSection(g, 482, L"应用启动方式");

        DrawSection(g, 606, L"其他");
        Txt(g, L"失焦自动隐藏（0-60 秒）", *F13, C_TEXT, RectF(24, 632, 250, 36), false);
    } else if (g_page == 1) {
        // ---- 截图页 ----
        DrawSection(g, 68,  L"截图快捷键");
        // 原「设置截图快捷键」提示行已删除：上方分组标题「截图快捷键」已表达同样含义，
        // 快捷键框直接跟在标题下（与搜索页一致，避免标题与提示重复）。

        DrawSection(g, 182, L"识图");

        DrawSection(g, 306, L"换行模式");
        Txt(g, L"智能换行", *F13, C_TEXT, RectF(24, 334, 250, 30), false);
        Txt(g, L"开启后自动合并识别出的换行碎片为段落；关闭则保留原始换行",
            *F12, C_SUB, RectF(24, 366, kW - 48, 40), false);

        DrawSection(g, 396, L"退出方式");
    } else {
        // ---- 搜索页 ----
        DrawSection(g, 68, L"搜索设置");
        Txt(g, L"开启搜索", *F13, C_TEXT, RectF(24, 96, 250, 30), false);
        Txt(g, L"开启预览", *F13, C_TEXT, RectF(24, 146, 250, 30), false);

        DrawSection(g, 196, L"搜索快捷键");
        Txt(g, L"默认 Ctrl+F 唤起全盘搜索；关闭后快捷键失效",
            *F12, C_SUB, RectF(24, 288, kW - 48, 24), false);
    }

    Pen pen(C_BORDER);
    g.DrawLine(&pen, 24.0f, 726.0f, (float)kW - 24.0f, 726.0f);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;   // 背景在 WM_PAINT 中完整重绘，避免闪烁
    case WM_APP_BEGIN_HIDE_EDIT:
        // 由 StepSubProc 在中段“双击”成立后投递过来：等按钮的消息处理完全收尾，
        // 再显示编辑框并切焦点，避免按钮的默认过程把焦点抢回去。
        BeginHideEdit(hwnd);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_NCLBUTTONDOWN:
        // 编辑态下点窗口空白处 / 标题栏 / 右键：先把输入提交掉。
        // Win32 事实：点击父窗口非控件区域不会转移键盘焦点，所以永远等不到
        // 编辑框的 WM_KILLFOCUS —— “点框外提交”必须在这里兜底。
        if (s_editActive) CommitHideEdit(hwnd);
        if (msg == WM_LBUTTONDOWN) {
            // 标签栏命中测试：命中且非当前页 -> 切页（同时结束录键态）
            POINT pt{ (short)LOWORD(lParam), (short)HIWORD(lParam) };
            for (int i = 0; i < 3; i++) {
                if (g_tabRects[i].Contains((float)pt.x, (float)pt.y)) {
                    if (g_page != i) {
                        g_page = i;
                        if (g_capturing) g_capturing = false;
                        ApplyPageVisibility(hwnd);
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    return 0;
                }
            }
        }
        break;      // 继续走 DefWindowProc（拖动、非客户区命中测试等默认行为不变）
    case WM_KILLFOCUS:
        // 窗口失去键盘焦点（Alt+Tab / 点别的程序）：结束录键状态，避免“录制态残留”
        if (g_capturing) {
            g_capturing = false;
            HWND hHot = GetDlgItem(hwnd, HotRowId(g_capTarget));
            if (hHot) InvalidateRect(hHot, nullptr, FALSE);
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        PaintClient(hdc);          // Graphics 在该函数内部构造并析构，先于 EndPaint
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* d = (DRAWITEMSTRUCT*)lParam;
        Graphics g(d->hDC); g.SetSmoothingMode(SmoothingModeAntiAlias);
        RectF rc((float)d->rcItem.left, (float)d->rcItem.top,
                 (float)(d->rcItem.right - d->rcItem.left),
                 (float)(d->rcItem.bottom - d->rcItem.top));
        // 每个自绘控件先把自己的矩形铺满白底：控件区域在父窗口那一遍绘制里被
        // WS_CLIPCHILDREN 挖掉了，不铺底就等于把自己的背景交给上一帧的残留像素决定。
        g.Clear(C_WHITE);
        bool hot = (d->itemState & ODS_HOTLIGHT) != 0;
        switch (d->CtlID) {
        case ID_TG1:   DrawToggle(g, rc, g_s.followMouse); break;
        case ID_TG_AUTO: DrawToggle(g, rc, g_s.autoStart); break;
        case ID_HOT:   DrawHotRow(g, rc, hot, 0); break;
        case ID_HOT_SHOT: DrawHotRow(g, rc, hot, 1); break;
        case ID_HOT_SEARCH: DrawHotRow(g, rc, hot, 2); break;
        case ID_HOT_SBOX: DrawHotRow(g, rc, hot, 3); break;
        case ID_TG_SEARCH:  DrawToggle(g, rc, g_s.searchEnabled); break;
        case ID_TG_PREVIEW: DrawToggle(g, rc, g_s.searchPreview); break;
        case ID_RAD1:  DrawRadio(g, rc, g_s.launchMode == 0, L"左键单击启动应用", hot); break;
        case ID_RAD2:  DrawRadio(g, rc, g_s.launchMode == 1, L"左键双击启动应用", hot); break;
        case ID_RAD_SHOT1: DrawRadio(g, rc, g_s.shotMode == 0, L"启动识图（截图后弹出图片识图）", hot); break;
        case ID_RAD_SHOT2: DrawRadio(g, rc, g_s.shotMode == 1, L"直接截图（截图后直接复制图片）", hot); break;
        case ID_TG_WRAP: DrawToggle(g, rc, g_s.shotWrapSmart); break;
        case ID_RAD_EXIT1: DrawRadio(g, rc, g_s.shotExitMode == 0, L"复制文字后直接退出", hot); break;
        case ID_RAD_EXIT2: DrawRadio(g, rc, g_s.shotExitMode == 1, L"复制完文字后 ESC 退出", hot); break;
        case ID_STEP:  DrawStepper(g, rc, hot); break;
        case ID_SLIDER: DrawSlider(g, rc); break;
        case ID_ADD:   Txt(g, L"＋  添加回收站到面板", *F13, C_ACCENT, rc, false); break;
        case ID_CANCEL:DrawBtn(g, rc, L"取消", false, hot); break;
        case ID_SAVE:  DrawBtn(g, rc, L"保存", true,  hot); break;
        }
        return TRUE;
    }
    case WM_COMMAND: {
        // ---- 通知码过滤（缺陷 B 的根因）----------------------------------------
        // 历史写法里 Btn() 带了 BS_NOTIFY，按钮除 BN_CLICKED 还会收到 BN_SETFOCUS /
        // BN_KILLFOCUS；而这里只按控件 ID 分发，于是一次点击被当成两次"点击"：
        //   WM_LBUTTONDOWN → 按钮取焦点 → BN_SETFOCUS → 翻转一次（画面上是新值）
        //   WM_LBUTTONUP   → BN_CLICKED   → 再翻转回去（画面回原状）
        // 净效果为 0，中间那帧新值就是"闪一下又恢复原状"；焦点从一个开关移到另一个
        // 开关时，前者还会收到 BN_KILLFOCUS 而自己翻转一次，这就是"显示与真值反向"。
        // ID_SAVE / ID_CANCEL 更严重：它们会在 BN_SETFOCUS 阶段就 DestroyWindow
        // （鼠标尚未抬起，且正在执行按钮自己的窗口过程）。
        // 现在已去掉 BS_NOTIFY，这里再按通知码过滤一次做纵深防御（防有人加回来）。
        // 只认 BN_CLICKED；下拉框（CBN_*）与数字编辑框（EN_*）的合法通知豁免。
        const UINT ctl  = static_cast<UINT>(LOWORD(wParam));
        const UINT code = static_cast<UINT>(HIWORD(wParam));
        if (code != BN_CLICKED && ctl != ID_COMBO && ctl != ID_EDIT) return 0;
        // 录键态下点了别的控件：结束录键（原来只有按键才结束，一旦点了别处，
        // 后续每个按键都会被当成录键，等于把用户的热键静默改掉）。
        if (g_capturing && ctl != ID_HOT && ctl != ID_HOT_SHOT &&
            ctl != ID_HOT_SEARCH && ctl != ID_HOT_SBOX) {
            g_capturing = false;
            HWND hHot0 = GetDlgItem(hwnd, HotRowId(g_capTarget));
            if (hHot0) InvalidateRect(hHot0, nullptr, FALSE);
        }
        switch (ctl) {
        case ID_TG1:  g_s.followMouse = !g_s.followMouse; PersistCtrl(hwnd, ID_TG1); return 0;
        case ID_TG_AUTO: g_s.autoStart = !g_s.autoStart; PersistCtrl(hwnd, ID_TG_AUTO); return 0;
        case ID_HOT:
            // 录键：只取键盘焦点，不 SetCapture。
            // 旧实现 SetCapture(hwnd) 会把“整窗鼠标”劫持给父窗口，而 g_capturing 只有
            // 按键/Esc 才清 —— 点过这一行之后再点任何控件、包括步进器，全部没反应，
            // 界面上却看不出异常。这是“双击数字毫无反应”最可能的现场。
            g_capturing = true; g_capTarget = 0;
            SetFocus(hwnd);          // 键盘消息要送到本窗口
            InvalidateRect(GetDlgItem(hwnd, ID_HOT), nullptr, FALSE);
            return 0;
        case ID_HOT_SHOT:            // 录制截图快捷键（截图页）
            g_capturing = true; g_capTarget = 1;
            SetFocus(hwnd);
            InvalidateRect(GetDlgItem(hwnd, ID_HOT_SHOT), nullptr, FALSE);
            return 0;
        case ID_HOT_SEARCH:          // 录制搜索快捷键（搜索页）
            g_capturing = true; g_capTarget = 2;
            SetFocus(hwnd);
            InvalidateRect(GetDlgItem(hwnd, ID_HOT_SEARCH), nullptr, FALSE);
            return 0;
        case ID_HOT_SBOX:            // 录制搜索框快捷键（启动页，默认 Alt+F）
            g_capturing = true; g_capTarget = 3;
            SetFocus(hwnd);
            InvalidateRect(GetDlgItem(hwnd, ID_HOT_SBOX), nullptr, FALSE);
            return 0;
        case ID_RAD1: g_s.launchMode = 0; PersistCtrl(hwnd, ID_RAD1); PersistCtrl(hwnd, ID_RAD2); return 0;
        case ID_RAD2: g_s.launchMode = 1; PersistCtrl(hwnd, ID_RAD1); PersistCtrl(hwnd, ID_RAD2); return 0;
        case ID_RAD_SHOT1: g_s.shotMode = 0; PersistCtrl(hwnd, ID_RAD_SHOT1); PersistCtrl(hwnd, ID_RAD_SHOT2); return 0;
        case ID_RAD_SHOT2: g_s.shotMode = 1; PersistCtrl(hwnd, ID_RAD_SHOT1); PersistCtrl(hwnd, ID_RAD_SHOT2); return 0;
        case ID_TG_WRAP: g_s.shotWrapSmart = !g_s.shotWrapSmart; PersistCtrl(hwnd, ID_TG_WRAP); return 0;
        case ID_TG_SEARCH:  g_s.searchEnabled = !g_s.searchEnabled; PersistCtrl(hwnd, ID_TG_SEARCH); return 0;
        case ID_TG_PREVIEW: g_s.searchPreview = !g_s.searchPreview; PersistCtrl(hwnd, ID_TG_PREVIEW); return 0;
        case ID_RAD_EXIT1: g_s.shotExitMode = 0; PersistCtrl(hwnd, ID_RAD_EXIT1); PersistCtrl(hwnd, ID_RAD_EXIT2); return 0;
        case ID_RAD_EXIT2: g_s.shotExitMode = 1; PersistCtrl(hwnd, ID_RAD_EXIT1); PersistCtrl(hwnd, ID_RAD_EXIT2); return 0;
        case ID_COMBO:                // 呼出鼠标键下拉框（0/1/2=对应键，3=取消鼠标呼出）
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                int sel = (int)SendMessageW(GetDlgItem(hwnd, ID_COMBO), CB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel <= 3) {
                    g_s.mouseBtn = sel;
                    Save(); ApplySettings();
                }
            }
            return 0;
        case ID_SLIDER:               // 滑杆点击/拖动已由 SliderSubProc 子类处理
            return 0;
        case ID_STEP:                 // 步进器点击/双击已由 StepSubProc 子类处理
            return 0;
        case ID_EDIT:                 // 编辑框的提交由子类过程处理（回车/失焦），此处吞掉 EN_CHANGE
            return 0;
        case ID_ADD:
            // 调用主程序的“添加回收站到面板”逻辑（由 SetSettingsAddRecycleBinCallback 注册）
            if (g_addRecycleBinCb) g_addRecycleBinCb();
            break;
        case ID_CANCEL:
            // 取消：丢弃正在编辑的数字（连同本次所有改动一起回滚到快照）
            CancelHideEdit(hwnd);
            // 回滚到打开时快照，并把原始值写回 settings.ini
            //（撤销本次所有改动），然后恢复运行时。
            g_s = g_snapshot;
            Save();
            ApplySettings();
            DestroyWindow(hwnd);
            return 0;
        case ID_SAVE:
            // 正在编辑的数字先提交，避免"输入了却没保存"（数据丢失级）
            CommitHideEdit(hwnd);
            Save();
            ApplySettings();
            DestroyWindow(hwnd);
            return 0;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (g_capturing) {           // 录制快捷键（弹出/截图/搜索/搜索框，按 g_capTarget 区分）
            UINT vk = (UINT)wParam;
            UINT& dstMod = (g_capTarget == 1) ? g_s.capHkMod
                         : (g_capTarget == 2) ? g_s.searchHkMod
                         : (g_capTarget == 3) ? g_s.sboxMod : g_s.hkMod;
            UINT& dstVk  = (g_capTarget == 1) ? g_s.capHkVk
                         : (g_capTarget == 2) ? g_s.searchHkVk
                         : (g_capTarget == 3) ? g_s.sboxVk : g_s.hkVk;
            HWND  hRow   = GetDlgItem(hwnd, HotRowId(g_capTarget));
            if (vk == VK_ESCAPE) {   // Esc：清空快捷键（空 = 不启用该快捷键）
                dstMod = 0; dstVk = 0;
                g_capturing = false; ReleaseCapture();
                Save(); ApplySettings();
                InvalidateRect(hRow, nullptr, FALSE);
                return 0;
            }
            // 依据当前键盘状态实时计算修饰键
            UINT mod = 0;
            if (GetKeyState(VK_CONTROL) & 0x8000) mod |= MOD_CONTROL;
            if (GetKeyState(VK_SHIFT)   & 0x8000) mod |= MOD_SHIFT;
            if (GetKeyState(VK_MENU)    & 0x8000) mod |= MOD_ALT;
            if ((GetKeyState(VK_LWIN) & 0x8000) ||
                (GetKeyState(VK_RWIN) & 0x8000)) mod |= MOD_WIN;
            // 纯修饰键：仅实时刷新显示，等待主键
            bool isMod = (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
                          vk == VK_SHIFT   || vk == VK_LSHIFT   || vk == VK_RSHIFT   ||
                          vk == VK_MENU    || vk == VK_LMENU    || vk == VK_RMENU    ||
                          vk == VK_LWIN    || vk == VK_RWIN);
            if (isMod) {
                dstMod = mod; dstVk = 0;
                InvalidateRect(hRow, nullptr, FALSE);
                return 0;
            }
            dstMod = mod; dstVk = vk;         // 主键：确定组合并结束录制（允许不带修饰键的单键）
            g_capturing = false; ReleaseCapture();
            Save(); ApplySettings();
            InvalidateRect(hRow, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_ESCAPE) {   // 非录制态下 Esc = 关闭并保存
            // 正在编辑数字时，Esc 只取消这次编辑，不关窗口（否则一行输入直接带走整个设置窗）
            if (s_editActive) { CancelHideEdit(hwnd); return 0; }
            Save(); ApplySettings();
            DestroyWindow(hwnd); return 0;
        }
        break;
    case WM_CLOSE:
        // 点击标题栏关闭 / Alt+F4：与保存等价（改动即保存）。
        // 先提交正在编辑的数字，否则用户输入的数值会静默丢失（只存在于 EDIT 控件里）。
        CommitHideEdit(hwnd);
        Save(); ApplySettings();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        // 嵌入主程序时不再 PostQuitMessage，避免误关整个应用；
        // 嵌套消息循环依靠 IsWindow(hwnd) 判断窗口已销毁而退出。
        // 复位跨窗口实例共享的交互状态：否则重开设置窗口后，上一窗口遗留的
        // “中段抬起已配对 / 双击待处理”会让第一次点击被判成双击。
        s_editActive = false; s_editMouseDown = false; s_inCommit = false;
        s_stepDownTick = 0; s_stepDownRegion = -1; s_stepDoublePending = false;
        s_stepUpArmed = false; s_stepUpTick = 0; s_stepUpX = -1;
        g_capturing = false; g_capTarget = 0; g_page = 0;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// 供主程序调用：打开设置窗口（hOwner 传主窗口句柄可实现模态效果）
void OpenSettingsWindow(HINSTANCE hInst, HWND hOwner) {
    // 重入守卫：本函数自带模态消息循环，循环期间仍会派发托盘的“设置”命令。
    // 不拦截就会再开一个设置窗口，并覆盖共享的 g_s / g_snapshot（A 窗改完 B 窗
    // 再保存会互相丢弃改动）。已有实例时只把它置前，不再新建。
    static HWND s_openWnd = nullptr;
    if (s_openWnd && IsWindow(s_openWnd)) {
        ShowWindow(s_openWnd, SW_SHOW);
        SetWindowPos(s_openWnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return;
    }
    // 无论从哪条路径退出，都把“已打开”标记清掉（RAII，含提前 return）。
    struct OpenGuard {
        HWND* slot;
        ~OpenGuard() { *slot = nullptr; }
    } openGuard{&s_openWnd};

    WNDCLASSW wc{};
    wc.style         = CS_DBLCLKS;   // 确保双击消息（如步进器中段）被正常派发
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"JisuQiSettings";
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    if (!GetClassInfoW(hInst, wc.lpszClassName, &wc)) RegisterClassW(&wc);

    Load();                // 每次打开都重新读配置
    g_snapshot = g_s;      // 快照，供“取消”回滚本次改动
    g_page = 0;            // 每次打开默认停在「启动」页

    // 嵌入主工程时由主程序负责 GDI+ 启动；字体只需初始化一次。
    // （独立调试入口 wWinMain 也会走这里，幂等。）
    static bool s_fontsReady = false;
    if (!s_fontsReady) { InitFonts(); s_fontsReady = true; }

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc{0, 0, kW, kH};
    AdjustWindowRect(&rc, style, FALSE);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;

    // 居中于屏幕工作区（任务栏不遮挡），而非跟随主窗口。
    RECT wa{0, 0, 0, 0};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = (wa.left + (wa.right - wa.left) - w) / 2;
    int y = (wa.top + (wa.bottom - wa.top) - h) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    // WS_EX_APPWINDOW：让被主窗口拥有的设置窗口也在任务栏出现，
    // 这样最小化后能在任务栏恢复。
    // 按需求：设置窗口与面板一样常驻最前（创建时带 WS_EX_TOPMOST，再显式抬到顶层）。
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_CONTROLPARENT | WS_EX_APPWINDOW | WS_EX_COMPOSITED,
        wc.lpszClassName, L"设置 — 极速启", style | WS_CLIPCHILDREN,
        x, y, w, h, hOwner, nullptr, hInst, nullptr);
    if (!hwnd) return;
    s_openWnd = hwnd;   // 标记“已打开”，供上面的重入守卫识别

    // Win11：圆角 + 白色标题栏；旧系统调用失败自动忽略
    INT cp = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cp, sizeof(cp));
    COLORREF cap = RGB(255, 255, 255);
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &cap, sizeof(cap));

    CreateControls(hwnd);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // 设置窗口全局置顶（与主面板一致；被面板拥有，天然显示在面板之上）。
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    MSG msg{};
    int got;
    // 编辑框句柄在控件建好后取一次：bypass 判据要用它，避免"没有焦点窗口时
    // GetFocus() == NULL 与 hEdit == NULL 相等"这种误判。
    HWND hEditFocus = GetDlgItem(hwnd, ID_EDIT);
    while ((got = static_cast<int>(GetMessageW(&msg, nullptr, 0, 0))) > 0) {
        // 录制快捷键 / 编辑框里按回车 / 任意 Esc 时绕过 IsDialogMessageW：
        //  - 录键：按键必须直达本窗口；
        //  - 回车：对话框管理器会把 VK_RETURN 当成“点默认按钮”消费掉，编辑框里
        //    那段“回车提交”永远收不到 —— 这是“回车提交不了”的根因；
        //  - Esc：本窗口自己的语义是取消编辑 / 关闭并保存。
        bool bypass = g_capturing ||
            (msg.message == WM_KEYDOWN &&
             (msg.wParam == VK_ESCAPE ||
              (msg.wParam == VK_RETURN && hEditFocus && GetFocus() == hEditFocus)));
        if (bypass || !IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!IsWindow(hwnd)) break;
    }
    // 收到 WM_QUIT（got == 0）时必须重新投回队列：本函数自带模态循环，若把 WM_QUIT
    // 吞掉，外层 App::Run() 的 GetMessageW 会永久阻塞——用户点“退出”后进程不死。
    if (got == 0) PostQuitMessage(static_cast<int>(msg.wParam));
}

// 独立调试入口；并入主工程后由 build.bat 定义 JISUQI_EMBEDDED 排除此入口，
// 避免与 main.cpp 的 WinMain 同时存在于同一 exe。
#ifndef JISUQI_EMBEDDED
int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    GdiplusStartupInput si{};
    GdiplusStartup(&gToken, &si, nullptr);
    InitFonts();
    OpenSettingsWindow(hInst, nullptr);
    delete F12; delete F12B; delete F13; delete F13B;
    GdiplusShutdown(gToken);
    return 0;
}
#endif
