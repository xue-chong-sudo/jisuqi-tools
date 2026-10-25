#define NOMINMAX
#define _WIN32_IE 0x0600
#include "MainWindow.h"
#include "App.h"
#include "resource.h"
#include "settings_ui.h"   // 运行时直接读取 data\settings.ini（新设置界面）
#include "Pinyin.h"
#include <gdiplus.h>
#include <algorithm>
#include <cmath>
#include <windowsx.h>
#include <imm.h>           // ImmAssociateContext：按字母快速启动要绕开中文输入法
#include <shellapi.h>      // NIN_SELECT / NIN_KEYSELECT（托盘 V4 事件兜底）
#include <commdlg.h>       // GetOpenFileNameW：「更换图标」文件选择框
#pragma comment(lib, "comdlg32.lib")

namespace JisuQi {

// 第7条需求：搜索框输路径 + 回车 = 直接打开（实现见 DrawItem 之前）
static bool TryOpenExistingPath(const std::wstring& raw);

namespace {

// Resolve GetDpiForWindow at runtime so we don't depend on a specific
// SDK version (falls back to 96 DPI).
int GetDpi(HWND hwnd) {
    typedef UINT (WINAPI* GDFW)(HWND);
    static GDFW f = reinterpret_cast<GDFW>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    return f ? static_cast<int>(f(hwnd)) : 96;
}

// ---- 输入法（IME）上下文开关 ----------------------------------------------
// 中文输入法会在 WM_KEYDOWN 之前截走字母键并弹出候选框，导致「按 A 一键启动」
// 变成「弹出一排拼音候选」。解决办法不是全局锁 CapsLock（那会污染用户的系统
// 输入状态、影响其他程序），而是把主窗口的 IME 上下文摘掉：字母键直接进
// WM_KEYDOWN，本窗口内输入法不介入，别处完全不受影响。
// 焦点进搜索框时再装回去，中文输入照常。即时生效，无需重启。
static HIMC g_savedImc = nullptr;   // 摘掉的原始上下文（可能为 NULL）
static HIMC g_poolImc  = nullptr;   // 兜底上下文：原上下文拿不到时用它恢复
static bool g_imeOff   = false;

static void SetImeEnabled(HWND hw, bool on) {
    if (!hw || !IsWindow(hw)) return;
    if (on) {
        if (!g_imeOff) return;
        // 恢复：优先装回原上下文；若原本就没有（窗口刚创建、系统还没分配），
        // 必须自己建一个 —— 否则搜索框会彻底失去中文输入能力。
        HIMC imc = g_savedImc;
        if (!imc) {
            if (!g_poolImc) g_poolImc = ImmCreateContext();
            imc = g_poolImc;
        }
        ImmAssociateContext(hw, imc);
        g_savedImc = nullptr;
        g_imeOff = false;
    } else {
        if (g_imeOff) return;
        g_savedImc = ImmAssociateContext(hw, nullptr);
        g_imeOff = true;
    }
}

// ---- 原生 EDIT 控件的排版辅助 -------------------------------------------------
// 坑点：Win32 的单行 EDIT 是「文字顶端对齐」的 —— 控件比字体高多少，多出来的空白
// 就全部堆在文字下方。把一个 36px 高的 EDIT 放在 15px 的字体上，字就会紧贴输入框
// 上沿（这不是样式没设对，而是控件本身的行为）。正确做法是让控件高度贴合字体行高
// 再在目标区域内垂直居中，而不是把控件拉高指望它自己居中。
// 返回「字体行高 + 上下各 3px 内边距」，即单行 EDIT 的合适高度。
int SnugEditHeight(HFONT hf) {
    HDC dc = GetDC(nullptr);
    if (!dc) return 22;
    HGDIOBJ old = SelectObject(dc, hf ? reinterpret_cast<HGDIOBJ>(hf)
                                      : GetStockObject(DEFAULT_GUI_FONT));
    TEXTMETRICW tm;
    ZeroMemory(&tm, sizeof(tm));
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    return static_cast<int>(tm.tmHeight) + 6;
}

// 用指定字体测量文字宽度（像素）；失败返回 0。
int MeasureTextWidth(HFONT hf, const std::wstring& text) {
    if (text.empty()) return 0;
    HDC dc = GetDC(nullptr);
    if (!dc) return 0;
    HGDIOBJ old = SelectObject(dc, hf ? reinterpret_cast<HGDIOBJ>(hf)
                                      : GetStockObject(DEFAULT_GUI_FONT));
    SIZE sz;
    ZeroMemory(&sz, sizeof(sz));
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &sz);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    return static_cast<int>(sz.cx);
}

// 重命名输入框的宽度：文字宽 + 左右各「一个字」的余量（名字 5 个字 → 能放下约 7 个
// 字；3 个字 → 约 5 个字），再夹在 [minW, maxW] 之间。maxW 传蓝框宽度即可保证输入框
// 永远不越出蓝色的选中示意框。
int FittedEditWidth(HFONT hf, const std::wstring& text, int minW, int maxW) {
    int tw = MeasureTextWidth(hf, text);
    int n = static_cast<int>(text.empty() ? 1 : text.size());
    int oneChar = tw > 0 ? (tw / n) : 12;
    if (oneChar < 6) oneChar = 6;          // 极窄字符（i、l、"1"）也至少留 6px
    int w = tw + oneChar * 2;              // 左右各一个字
    if (w < minW) w = minW;
    if (maxW > 0 && w > maxW) w = maxW;    // 不越出蓝框
    return w;
}

// File-level main-window handle used by the WH_KEYBOARD hook installed
// while a context menu is shown; the hook needs a window to forward F2 to.
HWND g_hwndMain = nullptr;

// 托盘菜单“暂停使用”开关（运行时状态，不落盘，重启后自动恢复可用）。
// 暂停快捷键：WM_HOTKEY 直接忽略；暂停鼠标：WM_APP_TOGGLE（长按计时触发）直接忽略。
static bool g_pauseHotkey = false;
static bool g_pauseMouse  = false;

// Which menu is currently driving the keyboard hook (0 = none,
// 1 = icon context menu, 2 = group-tab context menu, 3 = member menu).
// Shortcuts only fire while a menu is open, so they are NOT global hotkeys.
static int g_menuKind = 0;

// ===== 图标右键菜单：自绘 =====
// 系统菜单会在文字左侧固定预留一大块（check 区），没有任何 API 能改这个内边距。
// 用户要求「文字到左右边框的距离」缩为原来的一半、且左右相等 —— 只能整菜单
// 改成 owner-draw，由 WM_MEASUREITEM / WM_DRAWITEM 自己排版。
struct MenuItemText {
    std::wstring main;    // 左侧主文字
    std::wstring accel;   // 右侧快捷键说明（如 G）；空 = 没有
    bool submenu = false; // 有子菜单：右侧要有一个小三角
    bool sysPopup = false;// 真·系统子菜单（MF_POPUP）：三角由系统画，我们不要再画一遍
};
static std::vector<MenuItemText> g_menuText;   // itemData 存下标
static int  g_menuDpi = 96;
static HFONT g_menuFont = nullptr;
// 临时覆盖"文字到左右边框"的内边距（像素）。>0 时优先于默认值。
// 注意：托盘菜单已改回系统菜单（不用自绘），这里只服务于图标/分组/成员右键菜单。
static int  g_menuPadPx = 0;
// >0 时所有菜单项统一用这个宽度。owner-draw 菜单是按"每项实测宽度"布局的，
// 各项宽度不一致时会出现"悬停到哪项、哪项的内边距就变"的怪现象，必须统一。
static int  g_menuFixedW = 0;

// 文字到左右边框的距离：系统默认菜单左侧内边距约 20px，取一半并左右一致。
static int MenuPadX() {
    if (g_menuPadPx > 0) return g_menuPadPx;
    return static_cast<int>(10 * (g_menuDpi / 96.0));
}

static HFONT MenuFont() {
    if (!g_menuFont) {
        NONCLIENTMETRICSW ncm;
        ZeroMemory(&ncm, sizeof(ncm));
        ncm.cbSize = sizeof(ncm);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
            g_menuFont = CreateFontIndirectW(&ncm.lfMenuFont);
        if (!g_menuFont)
            g_menuFont = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }
    return g_menuFont;
}

// 主文字与右侧快捷键之间留多宽：用户要求「大约三个字符」的距离。
// 用菜单字体里 3 个 "0" 的实际宽度算，换字体/换 DPI 都会跟着变。
static int MenuGapX() {
    int w = 0;
    HDC dc = GetDC(nullptr);
    if (dc) {
        HGDIOBJ old = SelectObject(dc, MenuFont());
        SIZE sz;
        ZeroMemory(&sz, sizeof(sz));
        GetTextExtentPoint32W(dc, L"000", 3, &sz);
        w = sz.cx;
        SelectObject(dc, old);
        ReleaseDC(nullptr, dc);
    }
    if (w <= 0) w = static_cast<int>(21 * (g_menuDpi / 96.0));
    return w;
}

static void TextWidth(HDC dc, const std::wstring& t, int& w) {
    SIZE sz;
    ZeroMemory(&sz, sizeof(sz));
    if (!t.empty())
        GetTextExtentPoint32W(dc, t.c_str(), static_cast<int>(t.size()), &sz);
    w = sz.cx;
}

// 加一个自绘菜单项：文字存表，itemData 传下标（WM_MEASUREITEM/DRAWITEM 会原样带回）。
// sysPopup=true 时用 MF_POPUP 挂真正的子菜单（鼠标悬停由系统弹出、右侧三角也由
// 系统画），此时 id 必须传子菜单句柄 HMENU。
static void AddMenuText(HMENU h, UINT_PTR id, const std::wstring& main,
                        const std::wstring& accel = std::wstring(),
                        bool submenu = false, UINT flags = 0,
                        bool sysPopup = false) {
    MenuItemText t;
    t.main = main;
    t.accel = accel;
    t.submenu = submenu;
    t.sysPopup = sysPopup;
    g_menuText.push_back(t);
    const UINT f = MF_OWNERDRAW | flags | (sysPopup ? static_cast<UINT>(MF_POPUP) : 0u);
    AppendMenuW(h, f, id,
                reinterpret_cast<LPCWSTR>(static_cast<INT_PTR>(g_menuText.size() - 1)));
}

static const MenuItemText* MenuItemAt(LONG_PTR data) {
    const INT_PTR idx = static_cast<INT_PTR>(data);
    if (idx < 0 || idx >= static_cast<INT_PTR>(g_menuText.size())) return nullptr;
    return &g_menuText[static_cast<size_t>(idx)];
}

static bool HandleMenuMeasure(MEASUREITEMSTRUCT* m) {
    if (!m || m->CtlType != ODT_MENU) return false;
    const MenuItemText* t = MenuItemAt(static_cast<LONG_PTR>(m->itemData));
    int mainW = 60, accW = 0;
    if (t) {
        HDC dc = GetDC(nullptr);
        if (dc) {
            HGDIOBJ old = SelectObject(dc, MenuFont());
            TextWidth(dc, t->main, mainW);
            TextWidth(dc, t->accel, accW);
            SelectObject(dc, old);
            ReleaseDC(nullptr, dc);
        }
        if (t->submenu && accW == 0)
            accW = static_cast<int>(8 * (g_menuDpi / 96.0));
    }
    const int pad = MenuPadX();
    const int gap = MenuGapX();       // 主文字 → 快捷键，约三个字符
    int w = pad + mainW + (accW > 0 ? gap + accW : 0) + pad;
    if (g_menuFixedW > 0) w = g_menuFixedW;   // 统一宽度：避免逐项宽度不同导致布局跳动
    m->itemWidth  = static_cast<UINT>(w);
    m->itemHeight = static_cast<UINT>(GetSystemMetrics(SM_CYMENU));
    return true;
}

static bool HandleMenuDraw(DRAWITEMSTRUCT* d) {
    if (!d || d->CtlType != ODT_MENU) return false;
    const MenuItemText* t = MenuItemAt(static_cast<LONG_PTR>(d->itemData));
    const bool sel = (d->itemState & ODS_SELECTED) != 0;
    const bool dis = (d->itemState & ODS_GRAYED) != 0;
    HBRUSH br = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
    FillRect(d->hDC, &d->rcItem, br);   // 整行先铺白底
    DeleteObject(br);
    if (sel) {
        // 选中底色内缩 4px + 圆角。原来铺满整行会一直压到左右边框，看着像文字贴近边框、
        // 左右间距"变小了"（实测文字位置其实一直没变，是底色贴边造成的错觉）。
        const int In = 4;
        Gdiplus::RectF rb(static_cast<Gdiplus::REAL>(d->rcItem.left + In),
                          static_cast<Gdiplus::REAL>(d->rcItem.top + 1),
                          static_cast<Gdiplus::REAL>(d->rcItem.right - d->rcItem.left - In * 2),
                          static_cast<Gdiplus::REAL>(d->rcItem.bottom - d->rcItem.top - 2));
        Gdiplus::Graphics g(d->hDC);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::GraphicsPath p;
        JisuqUI::RoundRectPath(p, rb, 6.0f);
        Gdiplus::SolidBrush b(Gdiplus::Color(255, 0xE5, 0xF4, 0xFD));
        g.FillPath(&b, &p);
    }
    if (!t) return true;
    const int pad = MenuPadX();
    const int gap = MenuGapX();       // 主文字 → 快捷键，约三个字符
    const int obm = SetBkMode(d->hDC, TRANSPARENT);
    const COLORREF ofg = SetTextColor(d->hDC, dis ? RGB(0x9A, 0x9E, 0xA6)
                                                  : RGB(0x2B, 0x2F, 0x36));
    HGDIOBJ old = SelectObject(d->hDC, MenuFont());

    int accW = 0;
    if (!t->accel.empty()) {
        HDC dc = GetDC(nullptr);
        if (dc) {
            HGDIOBJ o = SelectObject(dc, MenuFont());
            TextWidth(dc, t->accel, accW);
            SelectObject(dc, o);
            ReleaseDC(nullptr, dc);
        }
    } else if (t->submenu) {
        accW = static_cast<int>(8 * (g_menuDpi / 96.0));
    }

    RECT r = d->rcItem;
    r.left += pad;
    r.right -= pad;
    if (accW > 0) r.right -= gap + accW;
    DrawTextW(d->hDC, t->main.c_str(), -1, &r,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    if (!t->accel.empty()) {
        RECT ar = d->rcItem;
        ar.left = d->rcItem.right - pad - accW;
        ar.right = d->rcItem.right - pad;
        DrawTextW(d->hDC, t->accel.c_str(), -1, &ar,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else if (t->submenu) {
        // 右侧小三角。真·系统子菜单（MF_POPUP）由系统自己画三角，我们绝不能
        // 再画一遍 —— 否则就是用户看到的「一个箭头变成两个」。
        if (t->sysPopup) {
            // 系统会在右侧画，这里留空即可
        } else {
            // 自绘子菜单（没有真 popup）才自己画。用 NULL_PEN 只填不描边。
            const int cy = (d->rcItem.top + d->rcItem.bottom) / 2;
            const int xr = d->rcItem.right - pad;
            const int aw = static_cast<int>(4 * (g_menuDpi / 96.0));   // 三角宽
            const int ah = static_cast<int>(4 * (g_menuDpi / 96.0));   // 三角半高
            POINT pts[3] = { {xr - aw, cy - ah}, {xr - aw, cy + ah}, {xr, cy} };
            HBRUSH ab = CreateSolidBrush(RGB(0x5E, 0x63, 0x6B));
            HGDIOBJ ob = SelectObject(d->hDC, ab);
            HGDIOBJ op = SelectObject(d->hDC, GetStockObject(NULL_PEN));
            Polygon(d->hDC, pts, 3);
            SelectObject(d->hDC, op);
            SelectObject(d->hDC, ob);
            DeleteObject(ab);
        }
    }

    SelectObject(d->hDC, old);
    SetTextColor(d->hDC, ofg);
    SetBkMode(d->hDC, obm);
    return true;
}

// Synthesize ESC to dismiss the active menu, then ask the main window to run
// the requested action (posted as a custom message so it runs after the menu
// has fully torn down).
static void PostMenuAction(UINT msg) {
    keybd_event(VK_ESCAPE, 0, 0, 0);
    keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
    if (g_hwndMain) PostMessageW(g_hwndMain, msg, 0, 0);
}

// 菜单打开期间，字母/功能键 -> 要投递给主窗口的消息（0 = 不处理）。
// 注意：带 Alt 的组合键产生的是 WM_SYSKEYDOWN，WH_KEYBOARD 钩子收不到它
// （只管 WM_KEYDOWN/WM_KEYUP），所以由下面的 WH_GETMESSAGE 钩子统一处理。
static UINT MenuKeyAction(int kind, WPARAM vk) {
    if (kind == 1) {                       // 图标右键菜单
        if (vk == VK_F2) return WM_APP_F2;
        if (vk == 'D' || vk == 'd') return WM_APP_DELETE;
        if (vk == 'G' || vk == 'g') return WM_APP_OPENLOC;
        if (vk == 'N' || vk == 'n') return WM_APP_NEWDRAWER;
        // 提示文字里已经没有「设置快捷键」的字母键，这里也不再映射 B，
        // 免得菜单打开时按 B 莫名弹出快捷键输入框。
        return 0;
    }
    if (kind == 3) {                       // 抽屉成员菜单
        if (vk == VK_F2) return WM_APP_F2;
        if (vk == 'D' || vk == 'd') return WM_APP_DELETE;
        if (vk == 'G' || vk == 'g') return WM_APP_OPENLOC;
        return 0;
    }
    if (kind == 2) {                       // 分组（标签）菜单
        if (vk == 'N' || vk == 'n') return WM_APP_GROUP_NEW;
        if (vk == VK_F2) return WM_APP_GROUP_REN;
        if (vk == 'D' || vk == 'd') return WM_APP_GROUP_DEL;
        return 0;
    }
    // kind == 4（托盘菜单）不再需要：托盘菜单是普通系统菜单，「退出(&D)」的 D
    // 由系统菜单自己处理。托盘弹出时本进程没有键盘焦点，钩子本来就收不到键。
    return 0;
}

static LRESULT CALLBACK KeyboardHookProc(int nCode, WPARAM wParam,
                                         LPARAM lParam) {
    if (nCode >= 0 && !(lParam & 0x80000000)) {
        // 图标菜单：F2 重命名 / G 打开目录 / D 删除 / N 新建抽屉（只在菜单打开时有效）
        // 抽屉成员菜单：F2 / D / G；分组菜单：N / F2 / D（只在菜单打开时有效）
        if (UINT msg = MenuKeyAction(g_menuKind, wParam)) {
            PostMenuAction(msg);
            return 1;
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// Alt+字母走 WM_SYSKEYDOWN，WH_KEYBOARD 看不到；用 WH_GETMESSAGE 兜住
// 所有入队消息（同时也覆盖 WM_KEYDOWN，两条路径不会重复触发：被 WH_KEYBOARD
// 吞掉的消息根本不会进队列）。
static LRESULT CALLBACK MenuGetMsgProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && g_menuKind != 0) {
        MSG* m = reinterpret_cast<MSG*>(lParam);
        if (m && (m->message == WM_SYSKEYDOWN || m->message == WM_KEYDOWN ||
                  m->message == WM_SYSKEYUP || m->message == WM_KEYUP)) {
            // 菜单打开期间先吞掉 Alt 本身：否则按下 Alt 会让菜单直接消失，
            // 后面的 Alt+G / Alt+D / Alt+B 就永远收不到。
            if ((m->message == WM_SYSKEYDOWN || m->message == WM_SYSKEYUP) &&
                (m->wParam == VK_MENU || m->wParam == VK_LMENU ||
                 m->wParam == VK_RMENU)) {
                m->message = WM_NULL;
                return 0;
            }
            if (m->message == WM_SYSKEYDOWN || m->message == WM_KEYDOWN) {
                if (UINT msg = MenuKeyAction(g_menuKind, m->wParam)) {
                    PostMenuAction(msg);
                    m->message = WM_NULL;   // 别让菜单自己再处理一次
                    return 0;
                }
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// Light theme palette (GDI+ objects have no runtime dependency, so they
// are safe as file-scope statics).
const Gdiplus::Color kBg(0xFF, 0xFF, 0xFF, 0xFF);        // white body
const Gdiplus::Color kTitle(0xFF, 0xF3, 0xF3, 0xF3);     // light gray bar
const Gdiplus::Color kText(0xFF, 0x33, 0x33, 0x33);      // dark gray text
const Gdiplus::Color kSubtle(0xFF, 0xD9, 0xD9, 0xD9);    // borders
const Gdiplus::Color kAccent(0xFF, 0x29, 0xA8, 0xE0);    // logo blue
const Gdiplus::Color kSelFill(0xFF, 0xE5, 0xF4, 0xFD);   // selection fill
const Gdiplus::Color kDrawer(0xFF, 0xEA, 0xF1, 0xF8);    // drawer cell fill
const Gdiplus::Color kDrawerBorder(0xFF, 0xC9, 0xD8, 0xE8);
// 新 UI：浅色标题栏 + 圆角卡片搜索框（配合 jisuq_ui_layout 的圆角语言）
const Gdiplus::Color kTitleBg(0xFF, 0xFA, 0xFA, 0xFB);   // title bar background
const Gdiplus::Color kTitleLine(0xFF, 0xEC, 0xEC, 0xEF); // title bottom hairline
const Gdiplus::Color kCardBorder(0xFF, 0xE3, 0xE3, 0xE6); // rounded card border
const Gdiplus::Color kIconGray(0xFF, 0x9A, 0x9E, 0xA6);   // magnifier / muted glyph
const Gdiplus::Color kTabActive(0xFF, 0xE2, 0xE4, 0xE8);   // active tab pill (soft gray, not black)
const Gdiplus::Color kTabActiveText(0xFF, 0x2B, 0x2F, 0x36); // active tab text (dark, on gray)

// Per-character width estimate for a group-tab label (Requirement 5a).
// CJK / full-width glyphs are ~2x as wide as Latin; a fixed padding is added
// on both sides. Relayout (hit area) and Paint (label) MUST agree on width,
// so both call this single function.
static int TabWidthOf(const std::wstring& name, double s) {
    int w = 0;
    for (wchar_t c : name) {
        if ((unsigned)c >= 0x2E80) w += static_cast<int>(15 * s);
        else                       w += static_cast<int>(8 * s);
    }
    w += static_cast<int>(24 * s);  // left + right padding (room for 12px font)
    return w;
}

} // namespace

MainWindow::MainWindow() {}
MainWindow::~MainWindow() {
    // 只删除“自建”字体：库存对象（GetStockObject）由系统持有、不可 DeleteObject；
    // m_font11 在创建失败时会别名到 m_font，重复删除同一句柄同样是错误的。
    if (m_fontOwned && m_font) DeleteObject(m_font);
    if (m_font11Owned && m_font11 && m_font11 != m_font) DeleteObject(m_font11);
    if (g_hwndMain == m_hwnd) g_hwndMain = nullptr;
}

bool MainWindow::Create(HINSTANCE hInst, App* app) {
    m_app = app;
    m_hInst = hInst;

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = hInst;
    wc.lpszClassName = L"JisuQiMainWindow";
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(IDC_ARROW));
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    if (!RegisterClassExW(&wc))
        m_createError = GetLastError();

    m_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"JisuQiMainWindow", L"极速启",
        WS_POPUP,
        CW_USEDEFAULT, 0, 380, 460,
        nullptr, nullptr, hInst, this);
    if (!m_hwnd && m_createError == 0)
        m_createError = GetLastError();
    if (m_hwnd) {
        g_hwndMain = m_hwnd;
    }
    return m_hwnd != nullptr;
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

LRESULT CALLBACK MainWindow::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lp);
        MainWindow* self = reinterpret_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
        // Store the real window handle here, BEFORE WM_CREATE runs.
        self->m_hwnd = hwnd;
    }
    MainWindow* self = reinterpret_cast<MainWindow*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->WndProc(msg, wp, lp);
}

LRESULT MainWindow::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            try {
            m_dpi = GetDpi(m_hwnd);
            double s = m_dpi / 96.0;
            m_font = CreateFontW(-static_cast<int>(13 * s), 0, 0, 0, FW_NORMAL,
                                 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            if (m_font) {
                m_fontOwned = true;
            } else {
                // 退回库存字体：系统对象，绝不能 DeleteObject。
                m_font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
                m_fontOwned = false;
            }
            m_font11 = CreateFontW(-static_cast<int>(11 * s), 0, 0, 0, FW_NORMAL,
                                   0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                   CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                   DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            if (m_font11) {
                m_font11Owned = true;
            } else {
                // 复用 m_font（可能正是库存对象）：标记为非自建，析构时不再重复删除。
                m_font11 = m_font;
                m_font11Owned = false;
            }

            m_editSearch = CreateWindowExW(
                0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT,
                10, 40, 340, 26, m_hwnd,
                reinterpret_cast<HMENU>(1), m_hInst, nullptr);
            if (m_editSearch) {
                if (m_font)
                    SendMessageW(m_editSearch, WM_SETFONT,
                                 reinterpret_cast<WPARAM>(m_font), TRUE);
                // 占位提示不走 EM_SETCUEBANNER：原生 user32 EDIT 不支持该消息
                // （实测设置后读回为空、界面不显示），改为在 Paint 里自绘
                // （见搜索卡片那段），这样提示文字也能跟着快捷键设置变化。
                m_oldSearchProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
                    m_editSearch, GWLP_WNDPROC,
                    reinterpret_cast<LONG_PTR>(SearchEditProc)));
            }

            m_drop = new DropTarget(m_app);
            RegisterDragDrop(m_hwnd, m_drop);
            if (m_editSearch) RegisterDragDrop(m_editSearch, m_drop);

            // Restore saved position (or center on first run / when size
            // memory is disabled).
            Config& cfg = m_app->GetConfig();
            RECT r;
            bool restore = cfg.winW > 0;   // 尺寸始终记住上次大小（rememberSize 开关已弃用，改作「窗口跟随鼠标」）
            if (restore) {
                r.left = cfg.winX; r.top = cfg.winY;
                r.right = cfg.winX + cfg.winW; r.bottom = cfg.winY + cfg.winH;
                SetWindowPos(m_hwnd, nullptr, r.left, r.top,
                             r.right - r.left, r.bottom - r.top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            } else {
                int w = static_cast<int>(380.0 * s);
                int h = static_cast<int>(460.0 * s);
                POINT c; GetCursorPos(&c);
                HMONITOR mon = MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi = {sizeof(mi)};
                if (mon && GetMonitorInfoW(mon, &mi)) {
                    int x = (mi.rcWork.left + mi.rcWork.right - w) / 2;
                    int y = (mi.rcWork.top + mi.rcWork.bottom - h) / 2;
                    SetWindowPos(m_hwnd, nullptr, x, y, w, h,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
                } else {
                    SetWindowPos(m_hwnd, nullptr, 100, 100, w, h,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
                }
            }

            Relayout();
            RebuildVisible();
            // 置顶看门狗：万一运行中 WS_EX_TOPMOST 被系统/其它软件弄丢，
            // 每 1.5 秒自检一次并补回（见 WM_TIMER case 5）。
            SetTimer(m_hwnd, TIMER_TOPMOST, 1500, nullptr);
            } catch (...) {
                return -1;
            }
            return 0;
        }

        case WM_ERASEBKGND:
            return 1; // fully painted in WM_PAINT

        case WM_CTLCOLOREDIT: {
            HDC hdcEdit = reinterpret_cast<HDC>(wp);
            SetTextColor(hdcEdit, RGB(0x33, 0x33, 0x33));
            SetBkColor(hdcEdit, RGB(0xFF, 0xFF, 0xFF));
            static HBRUSH s_editBrush =
                CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
            return reinterpret_cast<LRESULT>(s_editBrush);
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(m_hwnd, &ps);
            // Double-buffered paint: render everything to an offscreen DC
            // first, then blit once. This removes the flicker seen during
            // drag, scrollbar dragging and window resize (Bug C).
            RECT cr; GetClientRect(m_hwnd, &cr);
            int w = cr.right - cr.left, h = cr.bottom - cr.top;
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = (mem ? CreateCompatibleBitmap(hdc, w, h) : nullptr);
            if (mem && bmp) {
                HGDIOBJ old = SelectObject(mem, bmp);
                Paint(mem);
                BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
                SelectObject(mem, old);
                DeleteObject(bmp);
                DeleteDC(mem);
            } else {
                Paint(hdc);   // fallback: paint straight to the window DC
                if (mem) DeleteDC(mem);
            }
            EndPaint(m_hwnd, &ps);
            return 0;
        }

        case WM_SIZE:
            Relayout();
            InvalidateGrid();
            return 0;

        case WM_DPICHANGED: {
            m_dpi = LOWORD(wp);
            RECT* pr = reinterpret_cast<RECT*>(lp);
            SetWindowPos(m_hwnd, nullptr, pr->left, pr->top,
                         pr->right - pr->left, pr->bottom - pr->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            Relayout();
            InvalidateGrid();
            return 0;
        }

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize.x = 280;
            mmi->ptMinTrackSize.y = 320;
            return 0;
        }

        case WM_NCHITTEST: {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(m_hwnd, &pt);
            double s = m_dpi / 96.0;
            int edge = static_cast<int>(6 * s);
            RECT cr; GetClientRect(m_hwnd, &cr);
            // ---- Edge resize bands (6px, with 4 corners) ----
            if (pt.x < edge) {
                if (pt.y < edge) return HTTOPLEFT;
                if (pt.y > cr.bottom - edge) return HTBOTTOMLEFT;
                return HTLEFT;
            }
            if (pt.x > cr.right - edge) {
                if (pt.y < edge) return HTTOPRIGHT;
                if (pt.y > cr.bottom - edge) return HTBOTTOMRIGHT;
                return HTRIGHT;
            }
            if (pt.y < edge) return HTTOP;
            if (pt.y > cr.bottom - edge) return HTBOTTOM;

            // ---- Scrollbar ----
            if (m_maxScroll > 0) {
                RECT sb = ScrollbarRect();
                if (pt.x >= sb.left && pt.x <= sb.right &&
                    pt.y >= sb.top && pt.y <= sb.bottom)
                    return HTCLIENT; // scrollbar, not window drag
            }
            // ---- Title-bar controls ----
            if (InCloseButton(pt)) return HTCLIENT;
            if (PtInRect(&m_rcGear, pt)) return HTCLIENT;
            if (m_rcTabAdd.right > m_rcTabAdd.left && PtInRect(&m_rcTabAdd, pt))
                return HTCLIENT;
            {
                int gi = TabRectAt(pt);
                if (gi >= 0) return HTCLIENT;
            }
            if (m_openDrawer >= 0) {
                // Whole grid region is interactive (top half closes /
                // renames, bottom half launches members).
                if (pt.y >= m_rcGrid.top) return HTCLIENT;
                return HTCAPTION; // title bar (above grid) stays draggable
            }
            if (HitTestIconBody(pt) >= 0) return HTCLIENT;
            return HTCAPTION; // title bar + empty grid are draggable
        }

        case WM_MOUSEWHEEL: {
            POINT sp = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            POINT cp = sp; ScreenToClient(m_hwnd, &cp);
            if (cp.y < m_titleH) {
                const Config& cfg = m_app->GetConfig();
                int n = static_cast<int>(cfg.groups.size());
                if (n > 1) {
                    int cur = 0;
                    for (int gi = 0; gi < n; ++gi)
                        if (cfg.groups[gi] == cfg.currentGroup) { cur = gi; break; }
                    // Requirement 7: wheel direction reversed. Up => previous
                    // group, down => next group. No wrap-around: at the first or
                    // last group, the wheel does nothing (Requirement 5b).
                    int dir = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1;
                    int ni = cur + dir;
                    if (ni < 0 || ni >= n) return 0;  // hit either end -> stay
                    SetCurrentGroup(ni);
                }
                return 0;
            }
            if (m_maxScroll > 0) {
                int delta = GET_WHEEL_DELTA_WPARAM(wp);
                SetScroll(m_scrollY - (delta > 0 ? m_cellH * 2 : -m_cellH * 2));
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            // Requirement 9: drag the drawer divider when captured.
            if (m_drawerDragging) {
                Config& cfg = m_app->GetConfig();
                int top = m_rcGrid.top, bot = m_rcGrid.bottom;
                double ratio = (pt.y - top) / static_cast<double>(bot - top);
                if (ratio < 0.15) ratio = 0.15;
                if (ratio > 0.85) ratio = 0.85;
                cfg.settings.drawerDivider = ratio;   // live update (no Save per move)
                SetCursor(LoadCursor(nullptr, IDC_SIZENS));
                InvalidateGrid();
                return 0;
            }
            // Hover feedback over the divider (vertical resize cursor).
            if (m_openDrawer >= 0 && !m_dragging && !m_drawerDragging) {
                int splitY = DrawerSplitY();
                double hs = m_dpi / 96.0;
                int dy = pt.y - splitY; if (dy < 0) dy = -dy;
                if (pt.x >= m_rcGrid.left && pt.x <= m_rcGrid.right &&
                    dy <= static_cast<int>(4 * hs))
                    SetCursor(LoadCursor(nullptr, IDC_SIZENS));
            }
            if (m_dragScroll) {
                RECT track = ScrollbarRect();
                RECT thumb = ScrollbarThumbRect();
                int trackH = track.bottom - track.top;
                int thumbH = thumb.bottom - thumb.top;
                POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                int ty = pt.y - m_scrollDragOffset - track.top;
                if (ty < 0) ty = 0;
                if (ty > trackH - thumbH) ty = trackH - thumbH;
                if (trackH > thumbH)
                    SetScroll(static_cast<int>(
                        static_cast<double>(m_maxScroll) * ty / (trackH - thumbH)));
                return 0;
            }
            if (m_dragging) {
                POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                m_dragPt = pt;
                int gi = TabRectAt(pt);
                if (gi >= 0 && gi != m_dragOverTab) {
                    // Bug H-e: hovering a group tab mid-drag switches the
                    // active group live. The drag source is keyed by cfg
                    // index, so the m_visible rebuild never loses it.
                    SetCurrentGroup(gi);
                }
                m_dragOverTab = TabRectAt(pt);
                m_dragOverDrawer = DrawerCellAt(pt);
                InvalidateGrid();
                return 0;
            }
            // Begin an internal drag once the pointer has moved > 6px.
            if (m_downValid) {
                POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                int dx = pt.x - m_downPt.x, dy = pt.y - m_downPt.y;
                if (dx * dx + dy * dy > 36) {
                    if (m_openDrawer < 0) {
                        // Top-level item drag (source recorded by cfg index).
                        m_dragging = true;
                        m_dragSrcDrawer = -1;
                        m_dragSrcItem = m_visible[m_downIndex];
                        m_dragFrom = m_downIndex;
                    } else {
                        // Drawer-member drag (Bug H-c/d).
                        m_dragging = true;
                        m_dragSrcDrawer = m_openDrawer;
                        m_dragSrcMember = static_cast<size_t>(m_downIndex);
                        m_dragFrom = m_downIndex;
                    }
                    m_dragPt = pt;
                    m_dragOverTab = -1;
                    m_dragOverDrawer = -1;
                    SetCapture(m_hwnd);
                    InvalidateGrid();
                    return 0;
                }
            }
            // 悬浮态：记录当前鼠标所在的 item 下标，仅在「网格闭合」时生效；
            // 抽屉展开态下网格是预览，不做 hover 高亮。TrackMouseEvent 负责在
            // 鼠标离开窗口时补发 WM_MOUSELEAVE，把 hover 清掉。
            {
                POINT hpt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                int k = (m_openDrawer < 0) ? HitTestIconBody(hpt) : -1;
                if (k != m_hoverIndex) {
                    int old = m_hoverIndex;
                    m_hoverIndex = k;
                    if (old >= 0) {
                        RECT c = CellRect(old); c.top -= m_scrollY; c.bottom -= m_scrollY;
                        InvalidateRect(m_hwnd, &c, FALSE);
                    }
                    if (k >= 0) {
                        RECT c = CellRect(k); c.top -= m_scrollY; c.bottom -= m_scrollY;
                        InvalidateRect(m_hwnd, &c, FALSE);
                    } else {
                        InvalidateGrid();   // 从某个 item 移到空白处：整窗刷新一次即可
                    }
                }
                TRACKMOUSEEVENT tme;
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = m_hwnd;
                tme.dwHoverTime = 0;
                TrackMouseEvent(&tme);
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            if (m_hoverIndex != -1) { m_hoverIndex = -1; InvalidateGrid(); }
            return 0;

        case WM_LBUTTONDOWN: {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            double s = m_dpi / 96.0;
            // Requirement 4: if a rename edit is active, commit it (clicking
            // outside the box confirms the current name) before the click is
            // processed further.
            if (m_editRename) {
                wchar_t buf[512] = {0};
                GetWindowTextW(m_editRename, buf, 512);
                std::wstring t(buf);
                size_t a = t.find_first_not_of(L" \t\r\n");
                size_t b = t.find_last_not_of(L" \t\r\n");
                if (a == std::wstring::npos) t.clear();
                else t = t.substr(a, b - a + 1);
                CommitRename(t);
            }
            // Requirement 9: grab the drawer divider when dragging starts near
            // it (only while a drawer is open).
            if (!m_editRename && m_openDrawer >= 0 &&
                pt.x >= m_rcGrid.left && pt.x <= m_rcGrid.right) {
                int splitY = DrawerSplitY();
                int dy = pt.y - splitY; if (dy < 0) dy = -dy;
                if (dy <= static_cast<int>(4 * s)) {
                    m_drawerDragging = true;
                    SetCapture(m_hwnd);
                    return 0;
                }
            }
            // Scrollbar interaction (click track = jump, drag thumb = pan).
            if (m_maxScroll > 0) {
                RECT track = ScrollbarRect();
                if (pt.x >= track.left && pt.x <= track.right &&
                    pt.y >= track.top && pt.y <= track.bottom) {
                    RECT thumb = ScrollbarThumbRect();
                    int trackH = track.bottom - track.top;
                    int thumbH = thumb.bottom - thumb.top;
                    if (pt.y >= thumb.top && pt.y <= thumb.bottom) {
                        m_scrollDragOffset = pt.y - thumb.top;
                    } else {
                        int ty = pt.y - track.top - thumbH / 2;
                        if (ty < 0) ty = 0;
                        if (ty > trackH - thumbH) ty = trackH - thumbH;
                        if (trackH > thumbH)
                            SetScroll(static_cast<int>(
                                static_cast<double>(m_maxScroll) * ty /
                                (trackH - thumbH)));
                        m_scrollDragOffset = thumbH / 2;
                    }
                    m_dragScroll = true;
                    SetCapture(m_hwnd);
                    return 0;
                }
            }
            if (PtInRect(&m_rcGear, pt)) { m_app->ShowSettings(); return 0; }
            if (InCloseButton(pt)) { Hide(); return 0; }
            if (m_rcTabAdd.right > m_rcTabAdd.left && PtInRect(&m_rcTabAdd, pt)) {
                NewGroup(); return 0;   // 「+」新建分组
            }
            // Tab clicks switch the active group (also closes an open drawer).
            {
                int gi = TabRectAt(pt);
                if (gi >= 0) { SetCurrentGroup(gi); return 0; }
            }
            if (m_openDrawer >= 0) {
                int mi = HitTestMemberBody(pt);
                if (mi >= 0) {
                    m_downIndex = mi;
                    m_downPt = pt;
                    m_downValid = true;
                    m_selectedMember = mi;
                    InvalidateGrid();
                    return 0;
                }
                // Upper grid half: clicking the drawer name does nothing;
                // clicking elsewhere returns to the parent group. Title-bar
                // clicks (above the grid) fall through to allow dragging.
                int splitY = DrawerSplitY();
                if (pt.y >= m_rcGrid.top && pt.y < splitY) {
                    if (!PtInRect(&m_rcDrawerName, pt))
                        CloseDrawer();
                }
                return 0; // empty lower-half click
            }
            int i = HitTestIconBody(pt);
            if (i >= 0) {
                m_downIndex = i;
                m_downPt = pt;
                m_downValid = true;
                m_selected = i;
                InvalidateGrid();
            }
            return 0;
        }

        case WM_LBUTTONDBLCLK: {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (GetSettings().launchMode == 1) {
                // ---- 双击启动模式 ----
                // 单击开抽屉后，系统会紧跟一个 DBLCLK（落在原抽屉位置=上半区），
                // 若直接进改名/关闭逻辑会误触发；这里在「上半区且非抽屉名」时吞掉。
                if (m_openDrawer >= 0) {
                    int splitY = DrawerSplitY();
                    bool inUpper = (pt.y >= m_rcGrid.top && pt.y < splitY);
                    bool inName  = inUpper && PtInRect(&m_rcDrawerName, pt);
                    if (m_ignoreNextDbl && inUpper && !inName) {
                        m_ignoreNextDbl = false;
                        KillTimer(m_hwnd, TIMER_ICONDBL);
                        return 0;
                    }
                    if (inName) { StartDrawerNameRename(); return 0; }
                    if (inUpper) { CloseDrawer(); return 0; }
                    int mi = m_iconDblPending ? m_iconDblMember : HitTestMemberBody(pt);
                    if (mi >= 0) { ClearIconDblState(); LaunchDrawerMember(m_openDrawer, mi); }
                    return 0;
                }
                // 顶层（抽屉关闭）
                int i = m_iconDblPending ? m_iconDblIndex : HitTestIconBody(pt);
                if (i >= 0 && i < static_cast<int>(m_visible.size())) {
                    const Item& it = m_app->GetConfig().items[m_visible[i]];
                    if (it.isDrawer) {
                        if (GetKeyState(VK_CONTROL) & 0x8000) LaunchDrawerAll(i);
                        else OpenDrawer(static_cast<int>(m_visible[i]));
                    } else {
                        ClearIconDblState();
                        LaunchItem(i);
                    }
                }
                return 0;
            }
            // ---- 单击启动模式：保持原行为（双击抽屉=改名/关闭，双击顶层=无操作）----
            if (m_openDrawer >= 0) {
                int splitY = DrawerSplitY();
                if (pt.y >= m_rcGrid.top && pt.y < splitY &&
                    PtInRect(&m_rcDrawerName, pt))
                    StartDrawerNameRename();
                else if (pt.y >= m_rcGrid.top && pt.y < splitY)
                    CloseDrawer();
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            // Requirement 9: finish drawer divider drag, persisting the ratio.
            if (m_drawerDragging) {
                Config& cfg = m_app->GetConfig();
                cfg.Save();
                ReleaseCapture();
                m_drawerDragging = false;
                InvalidateGrid();
                return 0;
            }
            if (m_dragScroll) {
                m_dragScroll = false;
                ReleaseCapture();
                return 0;
            }
            if (m_dragging) {
                POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                m_dragPt = pt;
                DropInternalDrag(pt);
                return 0; // drag handled -> do not launch
            }
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (m_downValid) {
                int dx = pt.x - m_downPt.x, dy = pt.y - m_downPt.y;
                bool nearClick = (dx * dx + dy * dy) < 25;   // 同一次点击未拖动
                const int lm = GetSettings().launchMode;  // 0=单击启动 1=双击启动
                if (m_openDrawer >= 0) {
                    int mi = HitTestMemberBody(pt);
                    if (mi == m_downIndex && nearClick) {
                        if (lm == 1) {
                            // 双击模式：单击只选中，等第二击（由系统 WM_LBUTTONDBLCLK 触发）
                            m_iconDblPending = true;
                            m_iconDblIndex   = -1;
                            m_iconDblMember  = mi;
                            SetTimer(m_hwnd, TIMER_ICONDBL, GetDoubleClickTime(), nullptr);
                        } else {
                            LaunchDrawerMember(m_openDrawer, mi);
                        }
                    }
                } else {
                    int i = HitTestIconBody(pt);
                    if (i == m_downIndex && nearClick) {
                        const Item& it = m_app->GetConfig().items[m_visible[i]];
                        if (it.isDrawer) {
                            // 抽屉导航与「启动方式」无关：单击即开；Ctrl+单击整组启动
                            if (GetKeyState(VK_CONTROL) & 0x8000)
                                LaunchDrawerAll(i);
                            else {
                                OpenDrawer(static_cast<int>(m_visible[i]));
                                if (lm == 1) {   // 记下刚开抽屉，吞掉紧随的误双击
                                    m_ignoreNextDbl = true;
                                    SetTimer(m_hwnd, TIMER_ICONDBL, GetDoubleClickTime(), nullptr);
                                }
                            }
                        } else if (lm == 1) {
                            // 双击模式：单击只选中，等第二击
                            m_iconDblPending = true;
                            m_iconDblIndex   = i;
                            m_iconDblMember  = -1;
                            SetTimer(m_hwnd, TIMER_ICONDBL, GetDoubleClickTime(), nullptr);
                        } else {
                            LaunchItem(i);
                        }
                    }
                }
                m_downValid = false;
                m_downIndex = -1;
            }
            return 0;
        }

        case WM_CAPTURECHANGED:
            ClearIconDblState();   // 拖拽开始：放弃未决的双击启动
            if (m_dragging) {
                m_dragging = false;
                m_dragFrom = -1;
                m_dragSrcDrawer = -1;
                m_dragOverTab = -1;
                m_dragOverDrawer = -1;
                InvalidateGrid();
            }
            return 0;

        case WM_NCLBUTTONDOWN:
            // Requirement 4: a click on the non-client title bar confirms an
            // active rename before the default drag handling takes over.
            if (m_editRename) {
                wchar_t buf[512] = {0};
                GetWindowTextW(m_editRename, buf, 512);
                std::wstring t(buf);
                size_t a = t.find_first_not_of(L" \t\r\n");
                size_t b = t.find_last_not_of(L" \t\r\n");
                if (a == std::wstring::npos) t.clear();
                else t = t.substr(a, b - a + 1);
                CommitRename(t);
            }
            break;

        case WM_RBUTTONDOWN: {
            // Requirement 4: clicking (or right-clicking) outside the rename
            // box confirms the current name before the click is processed.
            if (m_editRename) {
                wchar_t buf[512] = {0};
                GetWindowTextW(m_editRename, buf, 512);
                std::wstring t(buf);
                size_t a = t.find_first_not_of(L" \t\r\n");
                size_t b = t.find_last_not_of(L" \t\r\n");
                if (a == std::wstring::npos) t.clear();
                else t = t.substr(a, b - a + 1);
                CommitRename(t);
            }
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            // Right-click anywhere on the header strip (title/tab/search row,
            // including the gear and close cells) opens the group menu, so
            // the whole grey title box behaves the same (user request).
            if (pt.y < m_rcGrid.top) {
                int gi = TabRectAt(pt);
                if (gi < 0) {
                    const Config& cfg = m_app->GetConfig();
                    for (int k = 0; k < static_cast<int>(cfg.groups.size()); ++k)
                        if (cfg.groups[k] == cfg.currentGroup) { gi = k; break; }
                }
                m_editGroup = gi;
                ShowTabMenu();
                return 0;
            }
            if (m_openDrawer >= 0) {
                int mi = HitTestMemberBody(pt);
                if (mi >= 0) { ShowMemberMenu(mi, pt); return 0; }
                return 0; // no menu for empty drawer area
            }
            int i = HitTestIconBody(pt);
            if (i >= 0) { ShowIconMenu(i, pt); return 0; }
            return 0;
        }

        case WM_NCRBUTTONDOWN: {
            // The header strip (above the grid) is HTCAPTION for dragging, so
            // right-clicks there arrive here as NON-client messages — the old
            // code hid the window, which is why right-clicking the empty grey
            // title area showed no group menu. Convert to client coordinates
            // and open the group menu anywhere on the header; anything else
            // (resize bands) keeps the old hide behaviour.
            // Requirement 4: commit an active rename before handling the click.
            if (m_editRename) {
                wchar_t buf[512] = {0};
                GetWindowTextW(m_editRename, buf, 512);
                std::wstring t(buf);
                size_t a = t.find_first_not_of(L" \t\r\n");
                size_t b = t.find_last_not_of(L" \t\r\n");
                if (a == std::wstring::npos) t.clear();
                else t = t.substr(a, b - a + 1);
                CommitRename(t);
            }
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(m_hwnd, &pt);
            if (pt.y < m_rcGrid.top) {
                const Config& cfg = m_app->GetConfig();
                int gi = TabRectAt(pt);
                if (gi < 0) {
                    for (int k = 0;
                         k < static_cast<int>(cfg.groups.size()); ++k)
                        if (cfg.groups[k] == cfg.currentGroup) { gi = k; break; }
                }
                m_editGroup = gi;
                ShowTabMenu();
                return 0;
            }
            Hide();
            return 0;
        }

        case WM_KEYDOWN:
            // Requirement 1: no shortcuts work on the main panel. Shortcuts
            // (F2/G/D/N for the icon menu, N/F2/D for the group menu) are only
            // active while the corresponding context menu is open.
            if (wp == VK_ESCAPE) {
                if (m_editRename) CancelRename();
                else if (m_openDrawer >= 0) CloseDrawer();
                else if (!GetSearchText().empty()) {
                    // Requirement 2: clear the search query first, then the
                    // next Esc will hide the window.
                    if (m_editSearch) SetWindowTextW(m_editSearch, L"");
                    RebuildVisible();
                    InvalidateGrid();
                } else Hide();
                return 0;
            }
            // 搜索框快捷键（不带 Alt 的组合，例如 Ctrl+F）：把焦点送进搜索框
            if (MatchSearchBoxHotkey(wp)) {
                if (m_editSearch) SetFocus(m_editSearch);
                return 0;
            }
            // 字母键：命中已绑定图标 -> 直接启动（LaunchItem 内部会隐藏面板）；
            // 未绑定 -> 落到搜索框并把这个字母打进去，等于一键开始搜索。
            if (wp >= 'A' && wp <= 'Z') {
                if (TryLaunchByShortcut(static_cast<wchar_t>(wp))) return 0;
                if (m_editSearch) {
                    SetFocus(m_editSearch);
                    wchar_t ch[2] = { static_cast<wchar_t>(wp), 0 };
                    SendMessageW(m_editSearch, EM_REPLACESEL, TRUE,
                                 reinterpret_cast<LPARAM>(ch));
                }
                return 0;
            }
            return 0;

        case WM_SYSKEYDOWN:
            // Alt 组合（搜索框快捷键默认就是 Alt+F）走这里，不能吞掉 Alt+F4 之类，
            // 所以未命中时交回默认过程。
            if (MatchSearchBoxHotkey(wp)) {
                if (m_editSearch) SetFocus(m_editSearch);
                return 0;
            }
            break;

        // 右键菜单是自绘的（为了控制文字左右内边距），这两条必须自己处理：
        // 不返回 TRUE 的话系统会把菜单项画成空白。
        case WM_MEASUREITEM:
            if (HandleMenuMeasure(reinterpret_cast<MEASUREITEMSTRUCT*>(lp))) return TRUE;
            break;
        case WM_DRAWITEM:
            if (HandleMenuDraw(reinterpret_cast<DRAWITEMSTRUCT*>(lp))) return TRUE;
            break;

        case WM_COMMAND: {
            if (HIWORD(wp) == EN_UPDATE &&
                reinterpret_cast<HWND>(lp) == m_editSearch) {
                if (!GetSearchText().empty() && m_openDrawer >= 0)
                    CloseDrawer();
                RebuildVisible();
                InvalidateGrid();
                // 占位提示画在 EDIT 自身（见 SearchEditProc 的 WM_PAINT），
                // 文本变化时 EDIT 会自行重绘，这里不需要再刷父窗口。
                return 0;
            }
            // "Move to" submenu items (cross-group relocation). Only returns
            // when the command actually matches a move-to entry; otherwise it
            // falls through to the switch below so that the other context
            // menu commands are not swallowed.
            {
                int id = LOWORD(wp);
                Config& c = m_app->GetConfig();
                int gi = id - ID_CTX_MOVETO_BASE;
                if (id >= ID_CTX_MOVETO_BASE && gi >= 0 && gi < static_cast<int>(c.groups.size())) {
                    // 抽屉成员：移出抽屉 → 变成该分组下的顶层图标
                    if (m_openDrawer >= 0 && m_selectedMember >= 0 &&
                        m_openDrawer < static_cast<int>(c.items.size())) {
                        Item& d = c.items[m_openDrawer];
                        if (m_selectedMember < static_cast<int>(d.members.size())) {
                            Item mv = d.members[m_selectedMember];
                            mv.group = c.groups[gi];
                            d.members.erase(d.members.begin() + m_selectedMember);
                            c.items.push_back(mv);
                            c.Save();
                            m_selectedMember = -1;
                            RebuildVisible();
                            InvalidateGrid();
                        }
                        return 0;
                    }
                    // 顶层图标：改分组
                    if (m_selected >= 0 && m_selected < static_cast<int>(m_visible.size())) {
                        c.items[m_visible[m_selected]].group = c.groups[gi];
                        c.Save();
                        RebuildVisible();
                        InvalidateGrid();
                        return 0;
                    }
                }
            }
            // 图标字母快捷键：弹窗输入单个 A-Z
            {
                const int id = LOWORD(wp);
                if (id == ID_CTX_SHORTCUT_SET) {
                    if (m_openDrawer >= 0 && m_selectedMember >= 0) {
                        // 抽屉里的成员也允许绑定：用成员下标走同一条路径
                        // （SetItemShortcut 只认 m_visible，这里单独处理成员）
                        ShowShortcutDialog(-1);
                    } else if (m_selected >= 0) {
                        ShowShortcutDialog(m_selected);
                    }
                    return 0;
                }
                // 兼容旧配置里残留的命令 ID（不会再有菜单项产生，纯保险）
                if (id >= ID_CTX_SHORTCUT_BASE && id < ID_CTX_SHORTCUT_BASE + 26) {
                    SetItemShortcut(m_selected,
                                    static_cast<wchar_t>(L'A' + (id - ID_CTX_SHORTCUT_BASE)));
                    return 0;
                }
                if (id == ID_CTX_SHORTCUT_CLEAR) {
                    SetItemShortcut(m_selected, 0);
                    return 0;
                }
            }
            switch (LOWORD(wp)) {
                case ID_CTX_RENAME:
                    if (m_openDrawer >= 0 && m_selectedMember >= 0)
                        StartMemberRename();
                    else if (m_selected >= 0) StartRename(m_selected);
                    break;
                case ID_CTX_DELETE:
                    if (m_openDrawer >= 0 && m_selectedMember >= 0)
                        DeleteMember(m_selectedMember);
                    else if (m_selected >= 0) DeleteItem(m_selected);
                    break;
                case ID_CTX_OPENLOC:
                    if (m_openDrawer >= 0 && m_selectedMember >= 0)
                        OpenLocationMember(m_selectedMember);
                    else if (m_selected >= 0) OpenLocation(m_selected);
                    break;
                case ID_CTX_NEWDRAWER: CreateDrawer(); break;
                // 「更换图标」要弹模态文件对话框，延后到菜单完全拆掉之后再执行。
                // 不能用 PostMenuAction —— 它合成的那次 ESC 会把刚弹出的对话框关掉。
                case ID_CTX_CHANGEICON:  PostMessageW(m_hwnd, WM_APP_CHANGEICON, 0, 0); break;
                case ID_GROUP_NEW:
                    NewGroup();
                    break;
                case ID_GROUP_REN:
                    if (m_editGroup >= 0 &&
                        m_editGroup < static_cast<int>(m_app->GetConfig().groups.size()))
                        StartTabRename(m_editGroup, false);
                    break;
                case ID_GROUP_DEL:
                    ConfirmDeleteGroup(m_editGroup);
                    break;
                case ID_TRAY_SETTINGS: m_app->ShowSettings(); break;
                case ID_TRAY_SHOW:  m_app->ShowWindow(); break;
                case ID_TRAY_EXIT:  m_app->Quit(); break;
                case ID_TRAY_PAUSE_HOTKEY:      // 暂停/恢复呼出快捷键
                    g_pauseHotkey = !g_pauseHotkey;
                    break;
                case ID_TRAY_PAUSE_MOUSE: {     // 暂停/恢复鼠标呼出
                    g_pauseMouse = !g_pauseMouse;
                    if (g_pauseMouse) KillTimer(m_hwnd, TIMER_MOUSEHOLD);  // 取消未决的长按计时
                    break;
                }
            }
            return 0;
        }

        case WM_TRAYICON: {
            // 托盘固定为「左键单击呼出」面板。启动方式（单击/双击）现在只管
            // 面板图标，不再影响托盘（见设置项 launchMode 的语义迁移）。
            // NOTIFYICON_VERSION_4：左键以 NIN_SELECT / NIN_KEYSELECT 上报，
            // 右键为 WM_CONTEXTMENU；经典版本左键为 WM_LBUTTONUP。
            switch (LOWORD(lp)) {
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU:              // V4 / 右键
                    ShowTrayMenu();
                    return 0;

                case WM_LBUTTONUP:                // 经典版本左键单击
                case NIN_SELECT:                  // V4 左键单击
                case NIN_KEYSELECT: {            // V4 键盘选择
                    DWORD now = GetMessageTime();
                    // 同一次物理点击可能同时投递多种事件（如 V4 下既有 NIN_SELECT
                    // 也可能附带 WM_LBUTTONUP），间隔极小；30ms 内重复事件去重，
                    // 避免把一次单击误判成双击。ShowWindow 幂等，重复无害。
                    if (m_trayLastEventTick != 0 &&
                        (now - m_trayLastEventTick) < 30) return 0;
                    m_trayLastEventTick = now;
                    m_app->ShowWindow();          // 固定：左键单击呼出
                    return 0;
                }
            }
            return 0;
        }

        case WM_HOTKEY:
            if (wp == ID_HOTKEY_TOGGLE && !g_pauseHotkey) m_app->ToggleWindow();
            else if (wp == ID_HOTKEY_SHOT && !g_pauseHotkey) m_app->TriggerScreenshot();
            return 0;

        case WM_APP_TOGGLE:
            if (g_pauseMouse) return 0;
            m_app->ToggleWindow();
            return 0;
        case WM_APP_SHOW:       m_app->ShowWindow(); return 0;
        case WM_APP_SEARCH_ENTER: {
            // 先看是不是一个真实路径（第7条需求）：是就直接打开，不再走搜索
            const std::wstring q = GetSearchText();
            if (TryOpenExistingPath(q)) { Hide(); return 0; }
            if (!m_visible.empty()) LaunchItem(0);
            return 0;
        }
        case WM_APP_HIDE:       Hide(); return 0;
        case WM_APP_F2: {
            // Requirement 1: F2 renames the selected icon / drawer member (or
            // the first icon). Mirrors the WM_KEYDOWN F2 handler so both the
            // search-box and the icon/member context menu behave identically.
            if (m_editRename) return 0;
            if (m_openDrawer >= 0 && m_selectedMember >= 0) StartMemberRename();
            else if (m_selected >= 0) StartRename(m_selected);
            else if (!m_visible.empty()) StartRename(0);
            return 0;
        }
        case WM_APP_NEWDRAWER:
            // 图标右键菜单的 N 键：新建一个空抽屉（此前该消息没有 case，
            // 被 PostMenuAction 投递后直接丢弃，等于“按 N 建抽屉”是死键）。
            CreateDrawer();
            return 0;
        case WM_APP_CHANGEICON:
            ChangeItemIcon(m_selected);
            return 0;
        case WM_APP_GROUP_NEW:
            NewGroup();
            return 0;
        case WM_APP_GROUP_REN: {
            // The group that was right-clicked to open the menu is stored in
            // m_editGroup. Fall back to the current group if it became invalid.
            const Config& cfg = m_app->GetConfig();
            int gi = m_editGroup;
            if (gi < 0 || gi >= static_cast<int>(cfg.groups.size())) {
                gi = 0;
                for (int k = 0; k < static_cast<int>(cfg.groups.size()); ++k)
                    if (cfg.groups[k] == cfg.currentGroup) { gi = k; break; }
            }
            StartTabRename(gi, false);
            return 0;
        }
        case WM_APP_GROUP_DEL: {
            const Config& cfg = m_app->GetConfig();
            int gi = m_editGroup;
            if (gi < 0 || gi >= static_cast<int>(cfg.groups.size())) {
                gi = 0;
                for (int k = 0; k < static_cast<int>(cfg.groups.size()); ++k)
                    if (cfg.groups[k] == cfg.currentGroup) { gi = k; break; }
            }
            ConfirmDeleteGroup(gi);
            return 0;
        }
        case WM_APP_DELETE:
            if (m_openDrawer >= 0 && m_selectedMember >= 0)
                DeleteMember(m_selectedMember);
            else if (m_selected >= 0) DeleteItem(m_selected);
            return 0;
        case WM_APP_OPENLOC:
            if (m_openDrawer >= 0 && m_selectedMember >= 0)
                OpenLocationMember(m_selectedMember);
            else if (m_selected >= 0) OpenLocation(m_selected);
            return 0;
        case WM_APP_SHORTCUT_SET:
            // 菜单里的 (B) 快捷键：等菜单真正关闭后再弹设置框
            if (m_selected >= 0) ShowShortcutDialog(m_selected);
            return 0;
        case WM_APP_SETTINGS_CHANGED: {
            KillTimer(m_hwnd, TIMER_AUTOHIDE);
            ClearIconDblState();   // 设置变更，放弃未决的双击启动
            // 设置窗口开着时绝不能启动自动隐藏计时：设置窗被本窗口 owned，
            // 藏主面板会连它一起藏掉（否则用户点两下设置、hideSec 秒后整个设置窗消失）。
            if (!m_app->IsSettingsVisible() && IsVisible() &&
                GetForegroundWindow() != m_hwnd && GetSettings().hideSec > 0)
                SetTimer(m_hwnd, TIMER_AUTOHIDE, GetSettings().hideSec * 1000, nullptr);
            // wParam==1：仅“失焦自动隐藏秒数”变化（来自设置界面轻量路径），只重置计时、
            // 不重绘整面板，避免每次点 +/−、拖滑杆、确认数字都触发 N 个图标全量重绘而卡顿。
            if (wp == 0) InvalidateGrid();
            // 「搜索框快捷键」可能刚被改过：立刻刷新占位提示里的键名。
            UpdateSearchCueBanner();
            return 0;
        }

        case WM_ACTIVATEAPP:
            if (wp) {
                KillTimer(m_hwnd, TIMER_AUTOHIDE);
            } else {
                // Bug 1: while the settings window is open, never start the
                // auto-hide timer. It is owned by this window, so hiding us
                // would hide it too.
                if (m_app->IsSettingsVisible()) return 0;
                if (GetSettings().hideSec > 0)
                    SetTimer(m_hwnd, TIMER_AUTOHIDE, GetSettings().hideSec * 1000, nullptr);
            }
            return 0;

        case WM_TIMER:
            if (wp == TIMER_AUTOHIDE) { KillTimer(m_hwnd, TIMER_AUTOHIDE); Hide(); }
            if (wp == TIMER_ICONDBL) { ClearIconDblState(); }   // 面板双击门控兜底：第二击未到则复位
            if (wp == TIMER_MOUSEHOLD) {   // 鼠标长按计时到点：按键仍被按住才触发呼出
                KillTimer(m_hwnd, TIMER_MOUSEHOLD);
                // 用钩子自维护的按下状态判定（GetAsyncKeyState 对侧键 XBUTTON1/2 不可靠，
                // 正是"长按呼出失效"的根因）。
                if (m_app->IsMouseHeld())
                    PostMessageW(m_hwnd, WM_APP_TOGGLE, 0, 0);
            }
            if (wp == TIMER_HOOKCHECK) { m_app->ReinstallMouseHook(); }  // 鼠标钩子看门狗：定期重装自愈
            if (wp == TIMER_TRIM) {      // 隐藏 3 秒后：把工作集还给系统（内存占用回落）
                KillTimer(m_hwnd, TIMER_TRIM);
                if (!IsWindowVisible(m_hwnd))
                    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
            }
            if (wp == TIMER_TOPMOST) {   // 置顶看门狗：面板可见但置顶样式丢失时自动补回
                if (IsWindowVisible(m_hwnd) &&
                    !(GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
                    SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE,
                        GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE) | WS_EX_TOPMOST);
                    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                }
            }
            return 0;

        case WM_CLOSE:
            Hide();
            return 0;

        case WM_DESTROY:
            KillTimer(m_hwnd, TIMER_AUTOHIDE);
            KillTimer(m_hwnd, TIMER_TRIM);
            if (m_drop) {
                RevokeDragDrop(m_hwnd);
                if (m_editSearch) RevokeDragDrop(m_editSearch);
                m_drop->Release();
                m_drop = nullptr;
            }
            return 0;
    }
    return DefWindowProcW(m_hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Layout & drawing
// ---------------------------------------------------------------------------

void MainWindow::Relayout() {
    RECT cr; GetClientRect(m_hwnd, &cr);
    int W = cr.right - cr.left, H = cr.bottom - cr.top;
    double s = m_dpi / 96.0;

    // 所有尺寸收敛到一个出口（jisuq_ui_layout::GetMetrics），以后想调间距只改这一处。
    JisuqUI::Metrics M = JisuqUI::GetMetrics(m_dpi);
    m_ui = M;

    m_titleH = M.titleH;
    int searchH = M.searchH;
    int sMarginX = static_cast<int>(12 * s);
    int sMarginY = static_cast<int>(10 * s);
    int searchY = m_titleH + sMarginY;
    m_rcSearch.left = sMarginX;   m_rcSearch.top = searchY;
    m_rcSearch.right = W - sMarginX; m_rcSearch.bottom = searchY + searchH;

    int gridTop = searchY + searchH + static_cast<int>(10 * s);
    // 网格边距/列宽/行高/图标尺寸全部来自 Metrics；m_cellW / m_cellH 按「格宽 + 间距」
    // 作为步距存储，CellRect / HitTest / IconBodyRect 等复用这两个值，间距自然落在格与
    // 格之间（与旧实现的 9*s 边距等价，只是统一由 GetMetrics 管理）。
    m_gridMargin = M.margin;
    m_iconSize  = M.icon;
    m_scrollBarW = static_cast<int>(8 * s);
    m_cellW = M.cellW + M.colGap;
    m_cellH = M.rowH + M.rowGap;
    int pad = m_gridMargin;
    m_rcGrid.left = pad;   m_rcGrid.top = gridTop;
    m_rcGrid.right = W - pad; m_rcGrid.bottom = H - m_gridMargin;
    JisuqUI::CalcGrid(M, m_rcGrid.left, m_rcGrid.top, m_rcGrid.right, m_rcGrid.bottom, m_cols);

    int closeS = static_cast<int>(22 * s);
    m_rcClose.left = W - static_cast<int>(30 * s);
    m_rcClose.top = static_cast<int>(5 * s);
    m_rcClose.right = W - static_cast<int>(8 * s);
    m_rcClose.bottom = m_rcClose.top + closeS;

    if (m_editSearch) {
        // 搜索框同理：单行 EDIT 文字顶端对齐，高度必须贴合字体行高再垂直居中。
        // 这里把高度收紧到 tmHeight+3（原 +6 会多出下沿空白，导致文字比放大镜
        // 偏上/偏下），并记录实际矩形，放大镜直接按这个矩形的垂直中心对齐。
        int snug = SnugEditHeight(m_font) - 3;
        if (snug > searchH) snug = searchH;
        if (snug < 12) snug = 12;
        int ey = m_rcSearch.top + (searchH - snug) / 2;
        int ex  = m_rcSearch.left + static_cast<int>(20 * s);   // 放大镜占位
        int ex2 = m_rcSearch.right - static_cast<int>(4 * s);   // 右侧圆角留白
        SetWindowPos(m_editSearch, nullptr, ex, ey, ex2 - ex, snug,
                     SWP_NOZORDER);
        m_rcSearchEdit.left = ex;   m_rcSearchEdit.top = ey;
        m_rcSearchEdit.right = ex2; m_rcSearchEdit.bottom = ey + snug;
    }

    // White background card behind the search box.
    int sbPadX = static_cast<int>(6 * s);
    int sbPadY = static_cast<int>(4 * s);
    m_rcSearchBg.left   = m_rcSearch.left - sbPadX;
    m_rcSearchBg.top    = m_rcSearch.top - sbPadY;
    m_rcSearchBg.right  = m_rcSearch.right + sbPadX;
    m_rcSearchBg.bottom = m_rcSearch.bottom + sbPadY;

    // ---- Tab bar (multi-group), with horizontal scroll for overflow ----
    m_tabRects.clear();
    const Config& cfg = m_app->GetConfig();
    int tabStartX = static_cast<int>(12 * s);  // Requirement 8: align with search box
    int gap = static_cast<int>(4 * s);
    int tabH = m_titleH - static_cast<int>(4 * s);
    int tabY = (m_titleH - tabH) / 2;
    int gearLeft = W - static_cast<int>(64 * s);
    // 标签右侧留一个「+ 新建分组」按钮，标签区在它左边结束（避免标签盖住 +）。
    int addW = static_cast<int>(22 * s);
    int addLeft = gearLeft - addW;
    int addGap = static_cast<int>(8 * s);   // 标签与「+」之间留白，避免 + 压住组名
    m_rcTabAdd.left   = addLeft;
    m_rcTabAdd.top    = tabY;
    m_rcTabAdd.right  = gearLeft;
    m_rcTabAdd.bottom = tabY + tabH;
    int avail = addLeft - addGap - tabStartX;
    // Total content width across all groups (drives the scroll range).
    int tabContentW = 0;
    for (size_t gi = 0; gi < cfg.groups.size(); ++gi) {
        const std::wstring& name = cfg.groups[gi];
        int w = TabWidthOf(name, s);
        tabContentW += w;
        if (gi + 1 < cfg.groups.size()) tabContentW += gap;
    }
    m_tabMaxScroll = (tabContentW > avail) ? (tabContentW - avail) : 0;
    if (m_tabScroll > m_tabMaxScroll) m_tabScroll = m_tabMaxScroll;
    if (m_tabScroll < 0) m_tabScroll = 0;
    // Lay out every tab's screen rect, offset by the scroll position.
    int offset = 0;
    for (size_t gi = 0; gi < cfg.groups.size(); ++gi) {
        const std::wstring& name = cfg.groups[gi];
        int w = TabWidthOf(name, s);
        int x = tabStartX + offset - m_tabScroll;
        RECT r = {x, tabY, x + w, tabY + tabH};
        m_tabRects.push_back(r);
        offset += w + gap;
    }
    m_rcGear.left   = gearLeft;
    m_rcGear.top    = static_cast<int>(5 * s);
    m_rcGear.right  = W - static_cast<int>(36 * s);
    m_rcGear.bottom = static_cast<int>(27 * s);

    UpdateScrollInfo();
    UpdateTabScroll();   // keep the active group's tab fully visible
}

// ---------------------------------------------------------------------------
// Scrollbar support (custom-drawn, no standard scrollbar on the popup)
// ---------------------------------------------------------------------------

int MainWindow::DrawerSplitY() const {
    // Requirement 9: position from the persisted drawer divider ratio.
    const Config& cfg = m_app->GetConfig();
    int top = m_rcGrid.top, bot = m_rcGrid.bottom;
    return top + static_cast<int>((bot - top) * cfg.settings.drawerDivider);
}

int MainWindow::MemberOffset() const {
    double s = m_dpi / 96.0;
    return (DrawerSplitY() + static_cast<int>(8 * s)) - m_rcGrid.top;
}

RECT MainWindow::MemberCellRect(int k) const {
    RECT r = CellRect(k);
    int off = MemberOffset();
    r.top += off;
    r.bottom += off;
    return r;
}

int MainWindow::GridContentHeight() const {
    int n = static_cast<int>(m_visible.size());
    if (m_openDrawer >= 0)
        n = static_cast<int>(m_app->GetConfig().items[m_openDrawer].members.size());
    int rows = (n + m_cols - 1) / std::max(1, m_cols);
    return rows * m_cellH;
}

void MainWindow::UpdateScrollInfo() {
    if (!m_hwnd) return;
    int viewH = m_rcGrid.bottom - m_rcGrid.top;
    if (m_openDrawer >= 0) {
        double s = m_dpi / 96.0;
        viewH = m_rcGrid.bottom - (DrawerSplitY() + static_cast<int>(8 * s));
    }
    int contentH = GridContentHeight();
    m_maxScroll = contentH > viewH ? contentH - viewH : 0;
    if (m_scrollY > m_maxScroll) m_scrollY = m_maxScroll;
    if (m_scrollY < 0) m_scrollY = 0;
}

void MainWindow::SetScroll(int y) {
    if (y < 0) y = 0;
    if (y > m_maxScroll) y = m_maxScroll;
    if (y == m_scrollY) return;
    m_scrollY = y;
    InvalidateGrid();
}

RECT MainWindow::ScrollbarRect() const {
    RECT cr; GetClientRect(m_hwnd, &cr);
    RECT r;
    r.right = cr.right - m_gridMargin / 2;
    r.left = r.right - m_scrollBarW;
    r.top = m_rcGrid.top;
    r.bottom = m_rcGrid.bottom;
    return r;
}

RECT MainWindow::ScrollbarThumbRect() const {
    RECT track = ScrollbarRect();
    RECT zero = {0, 0, 0, 0};
    if (m_maxScroll <= 0) return zero;
    int trackH = track.bottom - track.top;
    int viewH = trackH; // viewport == grid area height
    int contentH = GridContentHeight();
    int thumbH = std::max(static_cast<int>(24 * m_dpi / 96.0),
                          static_cast<int>(
                              static_cast<double>(trackH) * viewH / contentH));
    if (thumbH > trackH) thumbH = trackH;
    int ty = track.top + static_cast<int>(
        static_cast<double>(trackH - thumbH) * m_scrollY / m_maxScroll);
    RECT r = {track.left, ty, track.right, ty + thumbH};
    return r;
}

RECT MainWindow::CellRect(int k) const {
    int row = k / m_cols, col = k % m_cols;
    int x = m_rcGrid.left + col * m_cellW;
    int y = m_rcGrid.top + row * m_cellH;
    RECT r = {x, y, x + m_cellW, y + m_cellH};
    return r;
}

int MainWindow::HitTestIcon(POINT pt) const {
    pt.y += m_scrollY; // drawing is offset up by m_scrollY; invert it here
    for (int k = 0; k < static_cast<int>(m_visible.size()); ++k) {
        RECT r = CellRect(k);
        if (pt.x >= r.left && pt.x < r.right &&
            pt.y >= r.top && pt.y < r.bottom)
            return k;
    }
    return -1;
}

RECT MainWindow::IconBodyRect(int vi) const {
    RECT cell = CellRect(vi);
    cell.top -= m_scrollY;
    cell.bottom -= m_scrollY;
    const Item& it = m_app->GetConfig().items[m_visible[vi]];
    // 命中区 == 绘制端的高亮块矩形（蓝色示意框），两者共用 ItemTileRect，
    // 保证「点空白不选中、点蓝框才选中」。抽屉同样适用。
    Gdiplus::RectF cf(static_cast<Gdiplus::REAL>(cell.left),
                      static_cast<Gdiplus::REAL>(cell.top),
                      static_cast<Gdiplus::REAL>(cell.right - cell.left),
                      static_cast<Gdiplus::REAL>(cell.bottom - cell.top));
    Gdiplus::RectF t = JisuqUI::ItemTileRect(m_ui, cf, !it.name.empty());
    RECT r = { static_cast<LONG>(t.X), static_cast<LONG>(t.Y),
               static_cast<LONG>(t.X + t.Width), static_cast<LONG>(t.Y + t.Height) };
    return r;
}

int MainWindow::HitTestIconBody(POINT pt) const {
    for (int k = 0; k < static_cast<int>(m_visible.size()); ++k) {
        RECT r = IconBodyRect(k);
        if (pt.x >= r.left && pt.x < r.right &&
            pt.y >= r.top && pt.y < r.bottom)
            return k;
    }
    return -1;
}

RECT MainWindow::MemberBodyRect(int mi) const {
    RECT cell = MemberCellRect(mi);
    cell.top -= m_scrollY;
    cell.bottom -= m_scrollY;
    bool hasName = true;
    if (m_openDrawer >= 0) {
        const auto& d = m_app->GetConfig().items[m_openDrawer];
        if (mi >= 0 && mi < static_cast<int>(d.members.size()))
            hasName = !d.members[mi].name.empty();
    }
    Gdiplus::RectF cf(static_cast<Gdiplus::REAL>(cell.left),
                      static_cast<Gdiplus::REAL>(cell.top),
                      static_cast<Gdiplus::REAL>(cell.right - cell.left),
                      static_cast<Gdiplus::REAL>(cell.bottom - cell.top));
    Gdiplus::RectF t = JisuqUI::ItemTileRect(m_ui, cf, hasName);
    RECT r = { static_cast<LONG>(t.X), static_cast<LONG>(t.Y),
               static_cast<LONG>(t.X + t.Width), static_cast<LONG>(t.Y + t.Height) };
    return r;
}

int MainWindow::HitTestMemberBody(POINT pt) const {
    if (m_openDrawer < 0) return -1;
    const auto& d = m_app->GetConfig().items[m_openDrawer];
    for (int k = 0; k < static_cast<int>(d.members.size()); ++k) {
        RECT r = MemberBodyRect(k);
        if (pt.x >= r.left && pt.x < r.right &&
            pt.y >= r.top && pt.y < r.bottom)
            return k;
    }
    return -1;
}

bool MainWindow::InCloseButton(POINT pt) const {
    return pt.x >= m_rcClose.left && pt.x <= m_rcClose.right &&
           pt.y >= m_rcClose.top && pt.y <= m_rcClose.bottom;
}

// 第7条需求：搜索框里直接输一个已存在的路径，回车就打开它（文件夹用资源
// 管理器打开，文件用它的默认程序打开）。识别不出来就返回 false，回落到原
// 来的「启动第一个搜索结果」逻辑，不影响原有搜索行为。
static bool TryOpenExistingPath(const std::wstring& raw) {
    std::wstring p = raw;
    // 去掉首尾空白与成对引号（从资源管理器「复制为路径」粘过来会带引号）
    auto trim = [](std::wstring& s) {
        while (!s.empty() && (s.front() == L' ' || s.front() == L'\t')) s.erase(s.begin());
        while (!s.empty() && (s.back() == L' ' || s.back() == L'\t')) s.pop_back();
    };
    trim(p);
    if (p.size() >= 2 && p.front() == L'"' && p.back() == L'"')
        p = p.substr(1, p.size() - 2);
    trim(p);
    if (p.size() < 3) return false;

    // 支持 %USERPROFILE%\Desktop 这类环境变量写法
    wchar_t expanded[MAX_PATH * 2] = {};
    if (ExpandEnvironmentStringsW(p.c_str(), expanded, MAX_PATH * 2) > 0)
        p = expanded;

    // 必须像个路径：要么是 X: 开头，要么含目录分隔符（避免把普通关键词当路径）
    const bool driveAbs = p.size() >= 2 && p[1] == L':' &&
                          ((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z'));
    const bool hasSep = p.find(L'\\') != std::wstring::npos ||
                        p.find(L'/') != std::wstring::npos;
    if (!driveAbs && !hasSep) return false;

    const DWORD attr = GetFileAttributesW(p.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return false;

    std::wstring workDir;
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        workDir = p;                       // 文件夹：直接用资源管理器打开
    } else {
        size_t sl = p.find_last_of(L"\\/");
        if (sl != std::wstring::npos) workDir = p.substr(0, sl);
    }
    HINSTANCE r = ShellExecuteW(nullptr, L"open", p.c_str(), nullptr,
                                workDir.empty() ? nullptr : workDir.c_str(),
                                SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(r) > 32;
}

void MainWindow::DrawItem(Gdiplus::Graphics& g, const Item& it, RECT cell,
                          bool selected, bool skipName, double /*s*/, bool hover) {
    Gdiplus::RectF cf(static_cast<Gdiplus::REAL>(cell.left),
                      static_cast<Gdiplus::REAL>(cell.top),
                      static_cast<Gdiplus::REAL>(cell.right - cell.left),
                      static_cast<Gdiplus::REAL>(cell.bottom - cell.top));
    if (it.isDrawer) {
        Gdiplus::Bitmap* mb[4] = {nullptr, nullptr, nullptr, nullptr};
        for (int i = 0; i < 4 && i < static_cast<int>(it.members.size()); ++i)
            mb[i] = m_app->GetIconManager().GetBitmap(it.members[i].icon);
        JisuqUI::DrawDrawer(g, m_ui, mb, it.name, cf, selected, hover, it.shortcut);
        return;
    }
    Gdiplus::Bitmap* bmp = m_app->GetIconManager().GetBitmap(it.icon);
    JisuqUI::DrawAppItem(g, m_ui, bmp, it.name, cf, selected, hover, !skipName, it.shortcut);
}

void MainWindow::Paint(HDC hdc) {
    RECT cr; GetClientRect(m_hwnd, &cr);
    double s = m_dpi / 96.0;

    Gdiplus::Graphics g(hdc);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

    Gdiplus::SolidBrush txt(kText);
    Gdiplus::SolidBrush bg(kBg);
    g.FillRectangle(&bg, 0, 0, cr.right, cr.bottom);

    bool open = (m_openDrawer >= 0);
    const Config& cfg = m_app->GetConfig();

    // ---- Icon grid ----
    if (open) {
        int splitY = DrawerSplitY();
        int botTop = splitY + static_cast<int>(8 * s);

        // Upper half: dimmed parent grid preview.
        {
            Gdiplus::RectF clipT(static_cast<Gdiplus::REAL>(m_rcGrid.left),
                                 static_cast<Gdiplus::REAL>(m_rcGrid.top),
                                 static_cast<Gdiplus::REAL>(m_rcGrid.right - m_rcGrid.left),
                                 static_cast<Gdiplus::REAL>(splitY - m_rcGrid.top));
            g.SetClip(clipT);
            for (int k = 0; k < static_cast<int>(m_visible.size()); ++k) {
                RECT cell = CellRect(k);
                cell.top -= m_scrollY; cell.bottom -= m_scrollY;
                if (cell.bottom < m_rcGrid.top || cell.top > splitY) continue;
                DrawItem(g, cfg.items[m_visible[k]], cell,
                         (k == m_selected), (k == m_renamingIndex), s, (k == m_hoverIndex));
                // Drag hover feedback (Bug H): accent border on a hovered
                // drawer cell; accent insertion bar when a drawer member is
                // dropped onto a parent-grid cell.
                if (m_dragging && k == m_dragOverDrawer) {
                    Gdiplus::Pen dragOverPen(kAccent,
                        static_cast<Gdiplus::REAL>(2 * s));
                    g.DrawRectangle(&dragOverPen,
                        cell.left + 1, cell.top + 1, m_cellW - 2, m_cellH - 2);
                }
                if (m_dragging && m_dragSrcDrawer >= 0) {
                    int ph = HitTestIcon(m_dragPt);
                    if (ph == k && ph != m_dragOverDrawer) {
                        Gdiplus::Pen drawerInsPen(kAccent,
                            static_cast<Gdiplus::REAL>(2 * s));
                        g.DrawLine(&drawerInsPen,
                            static_cast<int>(cell.left + 1),
                            static_cast<int>(cell.top + 4),
                            static_cast<int>(cell.left + 1),
                            static_cast<int>(cell.bottom - 4));
                    }
                }
            }
            g.ResetClip();
            // ~65% white veil over the upper half.
            Gdiplus::SolidBrush veilBrush(
                Gdiplus::Color(0xA8, 0xFF, 0xFF, 0xFF));
            g.FillRectangle(&veilBrush,
                static_cast<int>(m_rcGrid.left),
                static_cast<int>(m_rcGrid.top),
                static_cast<int>(m_rcGrid.right - m_rcGrid.left),
                static_cast<int>(splitY - m_rcGrid.top));
            // Subtle divider at the split.
            Gdiplus::Pen splitLinePen(kSubtle, static_cast<Gdiplus::REAL>(1 * s));
            g.DrawLine(&splitLinePen,
                       m_rcGrid.left, splitY, m_rcGrid.right, splitY);
            // Drawer name (centered in the upper half), recorded for hit-test.
            int dw = static_cast<int>(160 * s), dh = static_cast<int>(28 * s);
            int cx = (m_rcGrid.left + m_rcGrid.right) / 2;
            int cyTop = (m_rcGrid.top + splitY) / 2;
            m_rcDrawerName.left = cx - dw / 2;
            m_rcDrawerName.top = cyTop - dh / 2;
            m_rcDrawerName.right = cx + dw / 2;
            m_rcDrawerName.bottom = cyTop + dh / 2;
            if (m_editMode != 2) {
                Gdiplus::Font dnameFont(L"Segoe UI",
                                        static_cast<Gdiplus::REAL>(13 * s),
                                        Gdiplus::FontStyleRegular,
                                        Gdiplus::UnitPixel);
                Gdiplus::StringFormat cf;
                cf.SetAlignment(Gdiplus::StringAlignmentCenter);
                cf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
                Gdiplus::RectF drf(static_cast<Gdiplus::REAL>(m_rcDrawerName.left),
                                   static_cast<Gdiplus::REAL>(m_rcDrawerName.top),
                                   static_cast<Gdiplus::REAL>(m_rcDrawerName.right - m_rcDrawerName.left),
                                   static_cast<Gdiplus::REAL>(m_rcDrawerName.bottom - m_rcDrawerName.top));
                g.DrawString(cfg.items[m_openDrawer].name.c_str(), -1,
                             &dnameFont, drf, &cf, &txt);
            }
        }

        // Lower half: drawer members.
        {
            Gdiplus::RectF clipB(static_cast<Gdiplus::REAL>(m_rcGrid.left),
                                 static_cast<Gdiplus::REAL>(splitY),
                                 static_cast<Gdiplus::REAL>(m_rcGrid.right - m_rcGrid.left),
                                 static_cast<Gdiplus::REAL>(m_rcGrid.bottom - splitY));
            g.SetClip(clipB);
            const auto& d = cfg.items[m_openDrawer];
            int count = static_cast<int>(d.members.size());
            for (int k = 0; k < count; ++k) {
                RECT cell = MemberCellRect(k);
                cell.top -= m_scrollY; cell.bottom -= m_scrollY;
                if (cell.bottom < botTop || cell.top > m_rcGrid.bottom) continue;
                // Dim the dragged member source (Bug H).
                if (m_dragging && m_dragSrcDrawer == m_openDrawer &&
                    k == static_cast<int>(m_dragSrcMember)) {
                    Gdiplus::Rect sel(cell.left + 2, cell.top + 2,
                                      m_cellW - 4, m_cellH - 4);
                    Gdiplus::SolidBrush memberSelBrush(
                        Gdiplus::Color(0xC8, 0xFF, 0xFF, 0xFF));
                    g.FillRectangle(&memberSelBrush, sel);
                }
                DrawItem(g, d.members[k], cell,
                         (k == m_downIndex), (k == m_renamingMember), s);
            }
            // Member reorder insertion bar (lower half), Bug H-c.
            if (m_dragging && m_dragSrcDrawer >= 0) {
                int mi = HitTestMember(m_dragPt);
                if (mi >= 0) {
                    RECT mc = MemberCellRect(mi);
                    mc.top -= m_scrollY; mc.bottom -= m_scrollY;
                    Gdiplus::Pen memberDropPen(kAccent,
                        static_cast<Gdiplus::REAL>(2 * s));
                    g.DrawLine(&memberDropPen,
                        static_cast<int>(mc.left + 1),
                        static_cast<int>(mc.top + 4),
                        static_cast<int>(mc.left + 1),
                        static_cast<int>(mc.bottom - 4));
                }
            }
            g.ResetClip();
        }
    } else {
        // Closed grid: current group (or search results across all groups).
        Gdiplus::RectF clipR(static_cast<Gdiplus::REAL>(m_rcGrid.left),
                             static_cast<Gdiplus::REAL>(m_rcGrid.top),
                             static_cast<Gdiplus::REAL>(m_rcGrid.right - m_rcGrid.left),
                             static_cast<Gdiplus::REAL>(m_rcGrid.bottom - m_rcGrid.top));
        g.SetClip(clipR);
        int count = static_cast<int>(m_visible.size());
        // The insertion bar MUST be computed with the same function the drop
        // handler uses (InsertIndexAt); previously it used a bare HitTestIcon,
        // so the blue bar and the actual drop position could disagree
        // ("icon does not follow the blue bar").
        int insVi = (m_dragging && m_dragSrcDrawer < 0)
                        ? InsertIndexAt(m_dragPt) : -1;
        bool crossDrag = false;
        if (m_dragging && m_dragSrcDrawer < 0 &&
            m_dragSrcItem < cfg.items.size())
            crossDrag = (cfg.items[m_dragSrcItem].group != cfg.currentGroup);
        for (int k = 0; k < count; ++k) {
            RECT cell = CellRect(k);
            cell.top -= m_scrollY; cell.bottom -= m_scrollY;
            if (cell.bottom < m_rcGrid.top || cell.top > m_rcGrid.bottom) continue;
            // Drag feedback: dim the dragged source, highlight the drop target.
            if (m_dragging && m_dragSrcDrawer < 0 && k == m_dragFrom) {
                Gdiplus::Rect sel(cell.left + 2, cell.top + 2,
                                  m_cellW - 4, m_cellH - 4);
                Gdiplus::SolidBrush gridSelBrush(
                    Gdiplus::Color(0xC8, 0xFF, 0xFF, 0xFF));
                g.FillRectangle(&gridSelBrush, sel);
            }
            bool skip = (k == m_renamingIndex);
            DrawItem(g, cfg.items[m_visible[k]], cell,
                     (k == m_selected && !m_dragging), skip, s, (k == m_hoverIndex));
            if (insVi >= 0) {
                // Bar sits exactly in the gap where the item will land:
                // left edge of the cell at the insert index, or the right
                // edge of the last cell for an end insertion.
                bool barHere = (insVi < count) ? (k == insVi)
                                               : (k == count - 1);
                bool atSrc = !crossDrag &&
                             (insVi == m_dragFrom || insVi == m_dragFrom + 1);
                if (barHere && !atSrc) {
                    int bx = (insVi < count) ? cell.left + 1
                                             : cell.right - 1;
                    int ly0 = cell.top + 4, ly1 = cell.bottom - 4;
                    Gdiplus::Pen insBarPen(kAccent,
                               static_cast<Gdiplus::REAL>(2 * s));
                    g.DrawLine(&insBarPen,
                               bx, ly0, bx, ly1);
                }
            }
        }
        g.ResetClip();
    }

    // ---- Custom scrollbar (only when content overflows the viewport). ----
    if (m_maxScroll > 0) {
        RECT track = ScrollbarRect();
        RECT thumb = ScrollbarThumbRect();
        Gdiplus::SolidBrush trackBrush(Gdiplus::Color(0xFF, 0xF0, 0xF0, 0xF0));
        g.FillRectangle(&trackBrush,
                        static_cast<int>(track.left),
                        static_cast<int>(track.top),
                        static_cast<int>(track.right - track.left),
                        static_cast<int>(track.bottom - track.top));
        Gdiplus::SolidBrush thumbBrush(Gdiplus::Color(0xFF, 0xC4, 0xC4, 0xC4));
        g.FillRectangle(&thumbBrush,
                        static_cast<int>(thumb.left),
                        static_cast<int>(thumb.top),
                        static_cast<int>(thumb.right - thumb.left),
                        static_cast<int>(thumb.bottom - thumb.top));
    }

    // ===== Title bar + search card (drawn last, on top). =====
    // 浅灰标题栏 + 底部 1px 分隔线（原来是 #F3F3F3 的硬边色块，显得旧）。
    Gdiplus::SolidBrush titleBgBrush(kTitleBg);
    g.FillRectangle(&titleBgBrush, 0, 0, cr.right, m_titleH);
    Gdiplus::SolidBrush titleLineBrush(kTitleLine);
    g.FillRectangle(&titleLineBrush, 0, m_titleH - 1, cr.right, 1);

    // Tabs (group names). Clip to the label region [tabStartX .. gearLeft] so a
    // tab whose width estimate is slightly too small can NEVER paint over the
    // gear / close controls (Requirement 5a).
    {
        int tabClipX = static_cast<int>(12 * s);
        int tabClipRight = (m_rcTabAdd.left > tabClipX) ? m_rcTabAdd.left
                                                        : (cr.right - static_cast<int>(64 * s));
        Gdiplus::RectF tabClip(static_cast<Gdiplus::REAL>(tabClipX),
                               static_cast<Gdiplus::REAL>(0),
                               static_cast<Gdiplus::REAL>(tabClipRight - tabClipX),
                               static_cast<Gdiplus::REAL>(m_titleH));
        g.SetClip(tabClip);
        Gdiplus::Font tabFont(L"Microsoft YaHei UI",
                              static_cast<Gdiplus::REAL>(12 * s),
                              Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::StringFormat tf;
        tf.SetAlignment(Gdiplus::StringAlignmentCenter);
        tf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        for (int gi = 0; gi < static_cast<int>(m_tabRects.size()); ++gi) {
            RECT tr = m_tabRects[gi];
            bool cur = (cfg.groups[gi] == cfg.currentGroup);
            bool renaming = (m_editMode == 1 && m_editGroup == gi);
            if (cur) {
                // 选中分组画成「深色圆角药丸 + 白字」，对齐参考稿的标签样式。
                Gdiplus::RectF pill(static_cast<Gdiplus::REAL>(tr.left) + 1,
                                    static_cast<Gdiplus::REAL>(tr.top) + 2,
                                    static_cast<Gdiplus::REAL>(tr.right - tr.left) - 2,
                                    static_cast<Gdiplus::REAL>(tr.bottom - tr.top) - 4);
                Gdiplus::GraphicsPath pillPath;
                JisuqUI::RoundRectPath(pillPath, pill, static_cast<Gdiplus::REAL>(10 * s));
                Gdiplus::SolidBrush pillBrush(kTabActive);
                g.FillPath(&pillBrush, &pillPath);
            }
            if (!renaming) {
                Gdiplus::SolidBrush* tbr = cur
                    ? new Gdiplus::SolidBrush(kTabActiveText)
                    : new Gdiplus::SolidBrush(kText);
                Gdiplus::RectF rf(static_cast<Gdiplus::REAL>(tr.left),
                                  static_cast<Gdiplus::REAL>(tr.top),
                                  static_cast<Gdiplus::REAL>(tr.right - tr.left),
                                  static_cast<Gdiplus::REAL>(tr.bottom - tr.top));
                g.DrawString(cfg.groups[gi].c_str(), -1, &tabFont, rf, &tf, tbr);
                delete tbr;
            }
            // Accent border while dragging an icon over this tab.
            if (m_dragOverTab == gi) {
                Gdiplus::Pen tabDragPen(kAccent, static_cast<Gdiplus::REAL>(2 * s));
                g.DrawRectangle(&tabDragPen,
                                static_cast<int>(tr.left),
                                static_cast<int>(tr.top),
                                static_cast<int>(tr.right - tr.left - 1),
                                static_cast<int>(tr.bottom - tr.top - 1));
            }
        }
        g.ResetClip();
    }

    // 「+」新建分组按钮（标签区右侧，点击 → NewGroup）。
    if (m_rcTabAdd.right > m_rcTabAdd.left) {
        Gdiplus::REAL cxA = (m_rcTabAdd.left + m_rcTabAdd.right) / 2.0f;
        Gdiplus::REAL cyA = (m_rcTabAdd.top + m_rcTabAdd.bottom) / 2.0f;
        Gdiplus::REAL rA = static_cast<Gdiplus::REAL>(5 * s);
        Gdiplus::Pen plusPen(kText, static_cast<Gdiplus::REAL>(1.6f * s));
        plusPen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound,
                           Gdiplus::DashCapRound);
        g.DrawLine(&plusPen, cxA - rA, cyA, cxA + rA, cyA);
        g.DrawLine(&plusPen, cxA, cyA - rA, cxA, cyA + rA);
    }

    // Requirement 6: thin accent indicator at the bottom of the title bar that
    // tracks the horizontal scroll position of the (possibly overflowing) group
    // tabs.
    if (m_tabMaxScroll > 0) {
        int indStartX = static_cast<int>(12 * s);
        int indGearLeft = (m_rcTabAdd.left > indStartX) ? m_rcTabAdd.left
                                                        : (cr.right - static_cast<int>(64 * s));
        int indAvail = indGearLeft - indStartX;
        int indContentW = indAvail + m_tabMaxScroll;
        double frac = static_cast<double>(m_tabScroll) / m_tabMaxScroll;
        int lineY = m_titleH - static_cast<int>(2 * s);
        int lineH = static_cast<int>(2 * s);
        int lineX = indStartX + static_cast<int>(frac * indAvail);
        int lineW = static_cast<int>((static_cast<double>(indAvail) / indContentW) * indAvail);
        Gdiplus::Pen indPen(kAccent, static_cast<Gdiplus::REAL>(lineH));
        indPen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound,
                          Gdiplus::DashCapFlat);
        g.DrawLine(&indPen, lineX, lineY, lineX + lineW, lineY);
    }

    // 搜索卡片：单个圆角白底 + 1px 淡边 + 左侧放大镜。
    // 原来画了内外两层矩形（像双边框），现在合并成一张圆角卡片；EDIT 无边框，
    // 且在 Relayout 里左右各让出一点，圆角能正常露出。
    {
        Gdiplus::RectF card(static_cast<Gdiplus::REAL>(m_rcSearchBg.left),
                            static_cast<Gdiplus::REAL>(m_rcSearchBg.top),
                            static_cast<Gdiplus::REAL>(m_rcSearchBg.right - m_rcSearchBg.left),
                            static_cast<Gdiplus::REAL>(m_rcSearchBg.bottom - m_rcSearchBg.top));
        Gdiplus::GraphicsPath cardPath;
        JisuqUI::RoundRectPath(cardPath, card, static_cast<Gdiplus::REAL>(9 * s));
        g.FillPath(&bg, &cardPath);
        Gdiplus::Pen cardBorderPen(kCardBorder, static_cast<Gdiplus::REAL>(1.0f));
        g.DrawPath(&cardBorderPen, &cardPath);

        // 放大镜（左侧图标区；EDIT 从该区域右侧开始，见 Relayout）。
        Gdiplus::REAL mx = card.X + static_cast<Gdiplus::REAL>(12 * s);
        // 单行 EDIT 文字是「顶对齐」的：文字的垂直中心 = 控件顶 + 字体行高/2，
        // 不是控件中线。用字体行高算，放大镜才能和文字真正对齐。
        Gdiplus::REAL my;
        if (m_rcSearchEdit.bottom > m_rcSearchEdit.top) {
            int tmH = SnugEditHeight(m_font) - 6;   // 还原真实字体行高
            if (tmH < 8) tmH = 8;
            my = static_cast<Gdiplus::REAL>(m_rcSearchEdit.top) + tmH / 2.0f;
        } else {
            my = card.Y + card.Height / 2;
        }
        Gdiplus::REAL mr = static_cast<Gdiplus::REAL>(4.2 * s);
        Gdiplus::Pen mag(kIconGray, static_cast<Gdiplus::REAL>(1.6f * s));
        mag.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
        g.DrawEllipse(&mag, mx - mr, my - mr, mr * 2, mr * 2);
        g.DrawLine(&mag, mx + mr * 0.7f, my + mr * 0.7f,
                   mx + mr * 1.9f, my + mr * 1.9f);
    }

    // Outer border for the borderless window.
    Gdiplus::Pen outerBorderPen(Gdiplus::Color(0xFF, 0xE0, 0xE0, 0xE0),
                                 static_cast<Gdiplus::REAL>(1 * s));
    g.DrawRectangle(&outerBorderPen,
                    0, 0, cr.right - 1, cr.bottom - 1);

    // Settings entry: a gear glyph (replaces the old "设置" text label).
    {
        Gdiplus::REAL gcx = (m_rcGear.left + m_rcGear.right) / 2.0f;
        Gdiplus::REAL gcy = (m_rcGear.top + m_rcGear.bottom) / 2.0f;
        Gdiplus::REAL R   = static_cast<Gdiplus::REAL>(6.6 * s);
        Gdiplus::REAL th  = static_cast<Gdiplus::REAL>(1.7 * s);
        Gdiplus::Pen gp(kText, th);
        gp.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound,
                      Gdiplus::DashCapRound);
        // 8 颗齿（外圈 + 径向短线）
        const double PI = 3.14159265358979323846;
        for (int i = 0; i < 8; ++i) {
            double a = i * PI / 4.0;
            Gdiplus::REAL ca = static_cast<Gdiplus::REAL>(std::cos(a));
            Gdiplus::REAL sa = static_cast<Gdiplus::REAL>(std::sin(a));
            g.DrawLine(&gp, gcx + ca * R, gcy + sa * R,
                             gcx + ca * (R + th * 1.1f), gcy + sa * (R + th * 1.1f));
        }
        g.DrawEllipse(&gp, gcx - R, gcy - R, R * 2, R * 2);          // 外环
        Gdiplus::REAL rh = static_cast<Gdiplus::REAL>(2.6 * s);
        g.DrawEllipse(&gp, gcx - rh, gcy - rh, rh * 2, rh * 2);      // 中心孔
    }

    // Close button (an "x")
    int cx = (m_rcClose.left + m_rcClose.right) / 2;
    int cy = (m_rcClose.top + m_rcClose.bottom) / 2;
    int rr = std::min(m_rcClose.right - m_rcClose.left,
                      m_rcClose.bottom - m_rcClose.top) / 2 -
             static_cast<int>(2 * s);
    Gdiplus::Pen pen(Gdiplus::Color(0xFF, 0x66, 0x66, 0x66),
                     static_cast<Gdiplus::REAL>(2 * s));
    g.DrawLine(&pen, cx - rr, cy - rr, cx + rr, cy + rr);
    g.DrawLine(&pen, cx - rr, cy + rr, cx + rr, cy - rr);
    // 搜索框占位提示不在这里画：EDIT 是子窗口，永远盖在父窗口之上，
    // 父窗口画的提示会被 EDIT 自身挡掉。改在 SearchEditProc 的 WM_PAINT 里画。
}

void MainWindow::InvalidateGrid() {
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void MainWindow::RebuildVisible() {
    m_visible.clear();
    std::wstring q = GetSearchText();
    const Config& cfg = m_app->GetConfig();
    if (q.empty()) {
        // No query: show only the current group's items.
        for (size_t i = 0; i < cfg.items.size(); ++i)
            if (cfg.items[i].group == cfg.currentGroup)
                m_visible.push_back(i);
    } else {
        // Query present: search across ALL groups by name/sub-pinyin.
        for (size_t i = 0; i < cfg.items.size(); ++i)
            if (NameMatch(cfg.items[i].name, q))
                m_visible.push_back(i);
    }
    if (m_selected >= static_cast<int>(m_visible.size()))
        m_selected = m_visible.empty() ? -1 :
                     static_cast<int>(m_visible.size()) - 1;
    UpdateScrollInfo();
}

std::wstring MainWindow::GetSearchText() const {
    wchar_t buf[256] = {0};
    if (m_editSearch) GetWindowTextW(m_editSearch, buf, 256);
    return std::wstring(buf);
}

void MainWindow::SavePosition() {
    RECT r;
    if (GetWindowRect(m_hwnd, &r)) {
        Config& cfg = m_app->GetConfig();
        cfg.winX = r.left; cfg.winY = r.top;
        cfg.winW = r.right - r.left; cfg.winH = r.bottom - r.top;
        cfg.Save();
    }
}

// ---------------------------------------------------------------------------
// Show / hide / lifecycle
// ---------------------------------------------------------------------------

// 可靠地把窗口抢到前台。从全局鼠标钩子 / 托盘呼出时，当前进程通常不是前台进程，
// 直接 SetForegroundWindow 会被系统的前台锁（foregound lock）限制而静默失败；
// 临时 AttachThreadInput 到前台线程可解除该限制。
static void ForceForeground(HWND h) {
    HWND fore = GetForegroundWindow();
    if (fore) {
        DWORD ft = GetWindowThreadProcessId(fore, nullptr);
        DWORD mt = GetCurrentThreadId();
        if (ft && ft != mt && AttachThreadInput(ft, mt, TRUE)) {
            SetForegroundWindow(h);
            AttachThreadInput(ft, mt, FALSE);
            return;
        }
    }
    SetForegroundWindow(h);
}

void MainWindow::Show() {
    AllowSetForegroundWindow(ASFW_ANY);
    KillTimer(m_hwnd, TIMER_TRIM);   // 又要显示了：取消待执行的"隐藏后回收工作集"

    // ① 确认 WS_EX_TOPMOST 扩展样式在位（若运行时被意外摘掉，就地补回）。
    LONG_PTR ex = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
    if ((ex & WS_EX_TOPMOST) == 0)
        SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, ex | WS_EX_TOPMOST);

    ShowWindow(m_hwnd, SW_SHOW);

    // 「窗口跟随鼠标」：开启时每次呼出把面板中心放到鼠标位置（并夹在显示器工作区内）；
    // 关闭时保持原位置（窗口不销毁，即上次拖动的位置）。
    if (GetSettings().followMouse) {
        RECT wr; GetWindowRect(m_hwnd, &wr);
        int w = wr.right - wr.left, h = wr.bottom - wr.top;
        POINT cp; GetCursorPos(&cp);
        int nx = cp.x - w / 2, ny = cp.y - h / 2;
        HMONITOR mon = MonitorFromPoint(cp, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        if (mon && GetMonitorInfoW(mon, &mi)) {
            if (nx < mi.rcWork.left) nx = mi.rcWork.left;
            if (ny < mi.rcWork.top) ny = mi.rcWork.top;
            if (nx + w > mi.rcWork.right) nx = mi.rcWork.right - w;
            if (ny + h > mi.rcWork.bottom) ny = mi.rcWork.bottom - h;
        }
        SetWindowPos(m_hwnd, nullptr, nx, ny, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // ② 每次呼出都抬到最顶层（HWND_TOPMOST 同时会把窗口排到置顶组的顶端）。
    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
    BringWindowToTop(m_hwnd);

    // ③ 抢前台焦点。注意：焦点给主窗口本身，而不是搜索框 ——
    //    主面板上的字母键要用于「一键启动已绑定 A-Z 的图标」，焦点若落在
    //    EDIT 里，字母会被直接吃进输入框。要打字搜索请按「搜索框快捷键」
    //    （默认 Alt+F），或直接按一个未绑定的字母（会自动落到搜索框）。
    ForceForeground(m_hwnd);
    SetFocus(m_hwnd);
    // ④ 摘掉主窗口的输入法上下文：否则中文输入法会先截走字母键，弹出一个
    //    拼音候选框（"按 A 弹出候选条"就是这个原因），快速启动直接失效。
    //    这里只影响本窗口、不动系统 CapsLock 状态，用户在其他程序照常打字。
    //    焦点进搜索框时会自动恢复（见 SearchEditProc 的 WM_SETFOCUS）。
    SetImeEnabled(m_hwnd, false);
    UpdateSearchCueBanner();   // 让占位提示里的快捷键跟着设置实时变化
}

void MainWindow::ClearIconDblState() {
    // 复位「面板双击启动」门控状态。第二击未到（定时器超时）、开始拖拽、切换
    // 分组、隐藏窗口或设置变更时调用，避免误触发或状态悬留。
    m_iconDblPending = false;
    m_iconDblIndex   = -1;
    m_iconDblMember  = -1;
    m_ignoreNextDbl  = false;
    KillTimer(m_hwnd, TIMER_ICONDBL);
}

void MainWindow::Hide() {
    ClearIconDblState();   // 窗口隐藏，放弃未决的双击启动
    CloseDrawer();   // return to the group level for next open
    SavePosition();
    CancelRename();
    // Any hide (manual / auto-hide / Esc) leaves search mode: clear the query
    // so the next open returns to the normal group page instead of stale
    // cross-group search results (Requirement 2).
    if (m_editSearch && GetWindowTextLengthW(m_editSearch) > 0) {
        SetWindowTextW(m_editSearch, L"");
        RebuildVisible();
    }
    ShowWindow(m_hwnd, SW_HIDE);
    // 面板常驻、但多数时间并不显示：隐藏 3 秒后把工作集还给系统
    // （任务管理器「内存」列会掉到 1MB 量级）。只回收工作集、不动已提交内存，
    // 因此不影响任何功能；再次呼出时页面从 standby 取回，基本无感。
    // 用延迟定时器而不是立刻回收：连续呼出/隐藏时不会反复抖动。
    SetTimer(m_hwnd, TIMER_TRIM, 3000, nullptr);
}

bool MainWindow::IsVisible() const {
    return IsWindowVisible(m_hwnd) != FALSE;
}

void MainWindow::OnItemsChanged() {
    RebuildVisible();
    InvalidateGrid();
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void MainWindow::LaunchItem(int vi) {
    if (vi < 0 || vi >= static_cast<int>(m_visible.size())) return;
    const Item& it = m_app->GetConfig().items[m_visible[vi]];
    ShellExecuteW(nullptr, L"open", it.path.c_str(),
                  nullptr, nullptr, SW_SHOWNORMAL);
    Hide(); // launch -> auto-hide
}

// ---------------------------------------------------------------------------
// 图标字母快捷键（A-Z 快速启动） + 搜索框快捷键（默认 Alt+F）
// ---------------------------------------------------------------------------

// 把 (mods, vk) 渲染成 "Ctrl+Alt+F" 这样的可读文本；返回空串表示未设置。
static std::wstring HotkeyText(UINT mods, UINT vk) {
    if (!vk) return std::wstring();
    std::wstring s;
    if (mods & MOD_CONTROL) s += L"Ctrl+";
    if (mods & MOD_ALT)     s += L"Alt+";
    if (mods & MOD_SHIFT)   s += L"Shift+";
    if (mods & MOD_WIN)     s += L"Win+";
    wchar_t name[64] = {0};
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        // 字母/数字直接用字符，避免不同键盘布局把名字翻译成别的语言
        s += static_cast<wchar_t>(vk);
    } else if (GetKeyNameTextW(static_cast<LONG>(sc << 16), name, 64) > 0 && name[0]) {
        s += name;
    } else {
        swprintf_s(name, 64, L"%u", vk);
        s += name;
    }
    return s;
}

bool MainWindow::MatchSearchBoxHotkey(WPARAM vk) const {
    const ::Settings& s = GetSettings();
    if (s.sboxVk == 0) return false;              // 未设置 => 不启用
    if (static_cast<UINT>(vk) != s.sboxVk) return false;
    // 修饰键必须完全吻合：多按/少按都不触发（避免 Alt+F 与 Alt+Shift+F 混淆）
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const bool win = ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) != 0;
    const bool wantCtrl = (s.sboxMod & MOD_CONTROL) != 0;
    const bool wantShift = (s.sboxMod & MOD_SHIFT) != 0;
    const bool wantAlt = (s.sboxMod & MOD_ALT) != 0;
    const bool wantWin = (s.sboxMod & MOD_WIN) != 0;
    return ctrl == wantCtrl && shift == wantShift && alt == wantAlt && win == wantWin;
}

void MainWindow::UpdateSearchCueBanner() {
    // 占位提示是自绘的（见 Paint 里的搜索卡片），所以这里只要让卡片区域重画。
    // 保留这个函数名，是因为它表达的是「提示内容需要刷新」这件事。
    if (!m_hwnd) return;
    InvalidateRect(m_hwnd, &m_rcSearchBg, FALSE);
}

bool MainWindow::TryLaunchByShortcut(wchar_t ch) {
    if (ch >= L'a' && ch <= L'z') ch = static_cast<wchar_t>(ch - L'a' + L'A');
    if (ch < L'A' || ch > L'Z') return false;
    Config& cfg = m_app->GetConfig();
    // 顶层图标优先；抽屉成员也参与（抽屉常开时同样能一键启动）。
    for (int vi = 0; vi < static_cast<int>(m_visible.size()); ++vi) {
        size_t idx = m_visible[vi];
        if (idx >= cfg.items.size()) continue;
        if (cfg.items[idx].shortcut == ch) { LaunchItem(vi); return true; }
    }
    for (size_t i = 0; i < cfg.items.size(); ++i) {
        if (cfg.items[i].shortcut == ch) {
            // 不在当前分组：直接启动（快捷键是基于全局的唯一绑定）
            ShellExecuteW(nullptr, L"open", cfg.items[i].path.c_str(),
                          nullptr, nullptr, SW_SHOWNORMAL);
            Hide();
            return true;
        }
        for (const auto& m : cfg.items[i].members) {
            if (m.shortcut == ch) {
                ShellExecuteW(nullptr, L"open", m.path.c_str(),
                              nullptr, nullptr, SW_SHOWNORMAL);
                Hide();
                return true;
            }
        }
    }
    return false;
}

std::wstring MainWindow::FindShortcutOwnerName(wchar_t ch) const {
    const Config& cfg = m_app->GetConfig();
    for (const auto& it : cfg.items) {
        if (it.shortcut == ch) return it.name;
        for (const auto& m : it.members)
            if (m.shortcut == ch) return m.name;
    }
    return std::wstring();
}

void MainWindow::SetItemShortcut(int vi, wchar_t ch) {
    if (vi < 0 || vi >= static_cast<int>(m_visible.size())) return;
    if (ch >= L'a' && ch <= L'z') ch = static_cast<wchar_t>(ch - L'a' + L'A');
    if (ch != 0 && (ch < L'A' || ch > L'Z')) return;
    Config& cfg = m_app->GetConfig();
    const size_t target = m_visible[vi];
    if (target >= cfg.items.size()) return;
    // 同一个字母全局唯一：先把别处的绑定清掉，再写本项。
    if (ch != 0) {
        for (auto& it : cfg.items) {
            if (it.shortcut == ch) it.shortcut = 0;
            for (auto& m : it.members)
                if (m.shortcut == ch) m.shortcut = 0;
        }
    }
    cfg.items[target].shortcut = ch;
    cfg.Save();
    InvalidateGrid();
}

void MainWindow::OpenLocation(int vi) {
    if (vi < 0 || vi >= static_cast<int>(m_visible.size())) return;
    const Item& it = m_app->GetConfig().items[m_visible[vi]];
    std::wstring cmd = L"/select,\"" + it.path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe",
                  cmd.c_str(), nullptr, SW_SHOWNORMAL);
}

void MainWindow::OpenLocationMember(int mi) {
    if (m_openDrawer < 0) return;
    const Item& d = m_app->GetConfig().items[m_openDrawer];
    if (mi < 0 || mi >= static_cast<int>(d.members.size())) return;
    std::wstring cmd = L"/select,\"" + d.members[mi].path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe",
                  cmd.c_str(), nullptr, SW_SHOWNORMAL);
}

void MainWindow::DeleteItem(int vi) {
    if (vi < 0 || vi >= static_cast<int>(m_visible.size())) return;
    size_t idx = m_visible[vi];
    Config& cfg = m_app->GetConfig();
    if (m_openDrawer == static_cast<int>(idx)) m_openDrawer = -1;
    cfg.items.erase(cfg.items.begin() + idx);
    cfg.Save();
    m_selected = -1;
    m_selectedMember = -1;
    CancelRename();
    RebuildVisible();
    InvalidateGrid();
}

void MainWindow::DeleteMember(int mi) {
    if (m_openDrawer < 0) return;
    Config& cfg = m_app->GetConfig();
    if (mi < 0 || mi >= static_cast<int>(cfg.items[m_openDrawer].members.size()))
        return;
    cfg.items[m_openDrawer].members.erase(
        cfg.items[m_openDrawer].members.begin() + mi);
    cfg.Save();
    m_selectedMember = -1;
    m_renamingMember = -1;
    CancelRename();
    UpdateScrollInfo();
    InvalidateGrid();
}

HWND MainWindow::CreateRenameEdit(RECT nr, const std::wstring& initial) {
    // Tear down any previous edit box (but leave the rename-mode state to
    // the caller, which sets it after this returns).
    if (m_editRename) {
        SetWindowLongPtrW(m_editRename, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(m_oldRenameProc));
        DestroyWindow(m_editRename);
        m_editRename = nullptr;
    }
    // 单行 EDIT 文字顶端对齐 —— 高度贴合字体行高 + 在传入矩形内垂直居中，
    // 文字的上下留白才会均衡（详见文件顶部 SnugEditHeight 的说明）。
    HFONT fnt = m_font11 ? m_font11 : m_font;
    int boxH = nr.bottom - nr.top;
    int snug = SnugEditHeight(fnt);
    if (snug > boxH) snug = boxH;
    if (snug < 12) snug = 12;          // 兜底：字体测量异常时不至于塌成一条线
    int editY = nr.top + (boxH - snug) / 2;
    m_editRename = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | ES_LEFT | ES_CENTER,
        nr.left, editY, nr.right - nr.left, snug,
        m_hwnd, reinterpret_cast<HMENU>(2), m_hInst, nullptr);
    if (m_font11)
        SendMessageW(m_editRename, WM_SETFONT,
                     reinterpret_cast<WPARAM>(m_font11), TRUE);
    else if (m_font)
        SendMessageW(m_editRename, WM_SETFONT,
                     reinterpret_cast<WPARAM>(m_font), TRUE);
    SetWindowTextW(m_editRename, initial.c_str());
    SendMessageW(m_editRename, EM_SETSEL, 0, -1);
    m_oldRenameProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        m_editRename, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(RenameEditProc)));
    SetFocus(m_editRename);
    InvalidateGrid();
    return m_editRename;
}

void MainWindow::StartRename(int vi) {
    if (vi < 0 || vi >= static_cast<int>(m_visible.size())) return;
    RECT cell = CellRect(vi);
    cell.top -= m_scrollY; cell.bottom -= m_scrollY;
    double s = m_dpi / 96.0;
    // 与绘制端同一套几何（jisuq_ui_layout 的 LayoutBlock）：图标在视觉单元格里
    // 垂直居中，名字紧贴图标下方 nameGap 处。
    int contentH = m_ui.icon + m_ui.nameGap + m_ui.nameH;
    int topPad = (m_ui.rowH - contentH) / 2;
    if (topPad < 0) topPad = 0;
    int iy = cell.top + topPad;
    int nameY = iy + m_ui.icon + m_ui.nameGap;
    // 宽度按名称长度自适应：文字宽 + 左右各一个字（5 个字的名字 → 约能放下 7 个字），
    // 上限是高亮块宽度（块已比单元格收窄），所以输入框不会再越出蓝框；整体水平居中。
    const std::wstring& nm = m_app->GetConfig().items[m_visible[vi]].name;
    int maxW = m_ui.tileW;
    int bw = FittedEditWidth(m_font11 ? m_font11 : m_font, nm,
                             static_cast<int>(40 * s), maxW);
    int bx = cell.left + (m_ui.cellW - bw) / 2;
    RECT nr = {bx, nameY, bx + bw, nameY + static_cast<int>(26 * s)};
    CreateRenameEdit(nr, nm);
    m_editMode = 0; m_editGroup = -1; m_editMember = -1; m_renamingMember = -1;
    m_renamingIndex = vi;
    InvalidateGrid();
}

void MainWindow::StartTabRename(int gi, bool /*isNew*/) {
    if (gi < 0 || gi >= static_cast<int>(m_app->GetConfig().groups.size()))
        return;
    if (gi >= static_cast<int>(m_tabRects.size())) return; // off-screen tab
    CreateRenameEdit(m_tabRects[gi], m_app->GetConfig().groups[gi]);
    m_editMode = 1; m_editGroup = gi;
    m_editMember = -1; m_renamingIndex = -1; m_renamingMember = -1;
    InvalidateGrid();
}

void MainWindow::StartDrawerNameRename() {
    if (m_openDrawer < 0) return;
    CreateRenameEdit(m_rcDrawerName, m_app->GetConfig().items[m_openDrawer].name);
    m_editMode = 2; m_editGroup = -1;
    m_editMember = -1; m_renamingIndex = -1; m_renamingMember = -1;
    InvalidateGrid();
}

void MainWindow::StartMemberRename() {
    if (m_openDrawer < 0 || m_selectedMember < 0) return;
    int mi = m_selectedMember;
    const auto& d = m_app->GetConfig().items[m_openDrawer];
    if (mi < 0 || mi >= static_cast<int>(d.members.size())) return;
    RECT cell = MemberCellRect(mi);
    cell.top -= m_scrollY; cell.bottom -= m_scrollY;
    double s = m_dpi / 96.0;
    int contentH = m_ui.icon + m_ui.nameGap + m_ui.nameH;
    int topPad = (m_ui.rowH - contentH) / 2;
    if (topPad < 0) topPad = 0;
    int iy = cell.top + topPad;
    int nameY = iy + m_ui.icon + m_ui.nameGap;
    // 同 StartRename：宽度随名称长度自适应，且不越出（已收窄的）高亮块。
    int maxW = m_ui.tileW;
    int bw = FittedEditWidth(m_font11 ? m_font11 : m_font, d.members[mi].name,
                             static_cast<int>(40 * s), maxW);
    int bx = cell.left + (m_ui.cellW - bw) / 2;
    RECT nr = {bx, nameY, bx + bw, nameY + static_cast<int>(26 * s)};
    CreateRenameEdit(nr, d.members[mi].name);
    m_editMode = 3; m_editMember = mi; m_renamingMember = mi;
    m_editGroup = -1; m_renamingIndex = -1;
    InvalidateGrid();
}

void MainWindow::CommitRename(const std::wstring& name) {
    Config& cfg = m_app->GetConfig();
    if (name.empty()) { CancelRename(); return; }
    switch (m_editMode) {
            case 1: { // group tab rename / new
            int gi = m_editGroup;
            if (gi < 0 || gi >= static_cast<int>(cfg.groups.size())) {
                CancelRename(); return;
            }
            std::wstring old = cfg.groups[gi];
            cfg.groups[gi] = name;
            for (auto& it : cfg.items)
                if (it.group == old) it.group = name;
            if (cfg.currentGroup == old) cfg.currentGroup = name;
            cfg.Save();
            Relayout();   // tab widths follow the new group name
            CancelRename();
            RebuildVisible();
            InvalidateGrid();
            return;
        }
        case 2: { // drawer name rename
            if (m_openDrawer >= 0) {
                cfg.items[m_openDrawer].name = name;
                cfg.Save();
            }
            CancelRename();
            InvalidateGrid();
            return;
        }
        case 3: { // drawer member rename
            if (m_openDrawer >= 0 && m_editMember >= 0 &&
                m_editMember < static_cast<int>(cfg.items[m_openDrawer].members.size())) {
                cfg.items[m_openDrawer].members[m_editMember].name = name;
                cfg.Save();
            }
            CancelRename();
            InvalidateGrid();
            return;
        }
        default: { // 0: icon name rename
            if (m_renamingIndex < 0 ||
                m_renamingIndex >= static_cast<int>(m_visible.size())) {
                CancelRename(); return;
            }
            cfg.items[m_visible[m_renamingIndex]].name = name;
            cfg.Save();
            CancelRename();
            RebuildVisible();
            InvalidateGrid();
            return;
        }
    }
}

void MainWindow::CancelRename() {
    if (m_editRename) {
        SetWindowLongPtrW(m_editRename, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(m_oldRenameProc));
        DestroyWindow(m_editRename);
        m_editRename = nullptr;
    }
    m_editMode = 0;
    m_editGroup = -1;
    m_editMember = -1;
    m_renamingIndex = -1;
    m_renamingMember = -1;
    InvalidateGrid();
}

// ---------------------------------------------------------------------------
// Drawer support (v3 split view)
// ---------------------------------------------------------------------------

void MainWindow::CreateDrawer() {
    Config& cfg = m_app->GetConfig();
    int n = 1;
    for (const auto& it : cfg.items) if (it.isDrawer) ++n;
    Item d;
    d.isDrawer = true;
    d.name = L"抽屉" + std::to_wstring(n);
    d.group = cfg.currentGroup;
    cfg.items.push_back(d);
    cfg.Save();
    m_openDrawer = static_cast<int>(cfg.items.size()) - 1;
    m_selected = -1;
    m_selectedMember = -1;
    CancelRename();
    RebuildVisible();
    InvalidateGrid();
    if (m_editSearch) SetFocus(m_editSearch);
}

void MainWindow::OpenDrawer(int cfgIndex) {
    m_openDrawer = cfgIndex;
    m_selected = -1;
    m_selectedMember = -1;
    CancelRename();
    UpdateScrollInfo();
    InvalidateGrid();
}

void MainWindow::CloseDrawer() {
    // Bug G: if a rename edit box was open (e.g. double-clicking the drawer
    // name then clicking blank space to close), tear it down first so the
    // edit control doesn't linger as a stray window.
    if (m_editRename) CancelRename();
    m_openDrawer = -1;
    m_selected = -1;
    m_selectedMember = -1;
    m_renamingMember = -1;
    UpdateScrollInfo();
    InvalidateGrid();
}

void MainWindow::LaunchDrawerAll(int vi) {
    if (vi < 0 || vi >= static_cast<int>(m_visible.size())) return;
    size_t idx = m_visible[vi];
    const Item& d = m_app->GetConfig().items[idx];
    if (!d.isDrawer) { LaunchItem(vi); return; }
    for (const auto& mem : d.members)
        ShellExecuteW(nullptr, L"open", mem.path.c_str(),
                      nullptr, nullptr, SW_SHOWNORMAL);
    Hide();
}

void MainWindow::LaunchDrawerMember(int drawerCfgIndex, int memberIndex) {
    if (drawerCfgIndex < 0) return;
    const Item& d = m_app->GetConfig().items[drawerCfgIndex];
    if (memberIndex < 0 ||
        memberIndex >= static_cast<int>(d.members.size())) return;
    ShellExecuteW(nullptr, L"open", d.members[memberIndex].path.c_str(),
                  nullptr, nullptr, SW_SHOWNORMAL);
    Hide();
}

int MainWindow::DrawerCellAt(POINT pt) const {
    pt.y += m_scrollY;
    for (int k = 0; k < static_cast<int>(m_visible.size()); ++k) {
        if (!m_app->GetConfig().items[m_visible[k]].isDrawer) continue;
        RECT r = CellRect(k);
        if (pt.x >= r.left && pt.x < r.right &&
            pt.y >= r.top && pt.y < r.bottom)
            return k;
    }
    return -1;
}

int MainWindow::HitTestMember(POINT pt) const {
    if (m_openDrawer < 0) return -1;
    const auto& d = m_app->GetConfig().items[m_openDrawer];
    for (int k = 0; k < static_cast<int>(d.members.size()); ++k) {
        RECT r = MemberCellRect(k);
        r.top -= m_scrollY;
        r.bottom -= m_scrollY;
        if (pt.x >= r.left && pt.x < r.right &&
            pt.y >= r.top && pt.y < r.bottom)
            return k;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Internal drag-reorder helpers (Bug H)
// ---------------------------------------------------------------------------

size_t MainWindow::GroupAppendPos() const {
    const Config& cfg = m_app->GetConfig();
    size_t last = cfg.items.size();
    for (size_t i = 0; i < cfg.items.size(); ++i)
        if (cfg.items[i].group == cfg.currentGroup) last = i + 1;
    return last;
}

bool MainWindow::MoveTopItemToInsert(size_t srcCfg, int insertVi) {
    Config& cfg = m_app->GetConfig();
    if (srcCfg >= cfg.items.size()) return false;
    int n = static_cast<int>(m_visible.size());
    size_t insCfg = (insertVi >= n) ? GroupAppendPos() : m_visible[insertVi];
    if (insCfg == srcCfg) return false; // dropped on itself -> no-op
    Item src = cfg.items[srcCfg];
    cfg.items.erase(cfg.items.begin() + srcCfg);
    if (insCfg > srcCfg) --insCfg;
    if (insCfg > cfg.items.size()) insCfg = cfg.items.size();
    cfg.items.insert(cfg.items.begin() + insCfg, src);
    return true;
}

void MainWindow::InsertItemToGrid(const Item& src, int insertVi) {
    Config& cfg = m_app->GetConfig();
    int n = static_cast<int>(m_visible.size());
    size_t insCfg = (insertVi >= n) ? GroupAppendPos() : m_visible[insertVi];
    if (insCfg > cfg.items.size()) insCfg = cfg.items.size();
    cfg.items.insert(cfg.items.begin() + insCfg, src);
    // 插入后 cfg.items 中插入点及其后面的元素整体后移一位。m_openDrawer 是
    // cfg.items 的下标，若它位于插入点之后却不同步，打开的抽屉就会指向错误
    // 的项（改名会改到别人、拖文件进去被静默丢弃）。这里同步 +1 修好它。
    if (m_openDrawer >= 0 && static_cast<size_t>(m_openDrawer) >= insCfg)
        ++m_openDrawer;
}

int MainWindow::MemberInsertIndexAt(POINT pt) const {
    int hit = HitTestMember(pt);
    if (hit >= 0) {
        RECT r = MemberCellRect(hit);
        int cx = (r.left + r.right) / 2;
        return (pt.x < cx) ? hit : hit + 1;
    }
    if (m_openDrawer < 0) return 0;
    return static_cast<int>(m_app->GetConfig().items[m_openDrawer].members.size());
}

void MainWindow::DropInternalDrag(POINT pt) {
    Config& cfg = m_app->GetConfig();
    bool changed = false;

    if (m_dragSrcDrawer < 0) {
        // ---- dragged a top-level item ----
        if (m_dragSrcItem < cfg.items.size()) {
            size_t srcCfg = m_dragSrcItem;
            std::wstring origGroup = cfg.items[srcCfg].group;
            int gi = TabRectAt(pt);
            int dv = DrawerCellAt(pt);
            if (dv >= 0 && (size_t)dv < m_visible.size() && m_visible[dv] != srcCfg) {
                // (a) drop onto a drawer cell -> move into the drawer.
                size_t dCfg = m_visible[dv];
                Item src = cfg.items[srcCfg];
                // 限制1：抽屉不能放进抽屉（禁止套娃）。
                // 限制2：目标抽屉已有同一图标（同路径+同类型）→ 不重复添加（否则会复制出快捷键冲突的副本）。
                bool dup = false;
                for (const auto& m : cfg.items[dCfg].members)
                    if (m.path == src.path && m.type == src.type) { dup = true; break; }
                if (!src.isDrawer && !dup) {
                    // Push into the drawer FIRST (members vector is independent of
                    // the outer items indices), THEN erase the source so dCfg
                    // stays valid (Bug 2).
                    cfg.items[dCfg].members.push_back(src);
                    cfg.items.erase(cfg.items.begin() + srcCfg);
                    changed = true;
                }
            } else {
                bool cross = (gi >= 0) || (m_dragOverTab >= 0) ||
                             (origGroup != cfg.currentGroup);
                if (cross) {
                    // Land in the currently displayed group (already switched
                    // live while hovering a tab, Bug H-e / Bug 3): reassign
                    // group and insert at the dropped position within it.
                    Item src = cfg.items[srcCfg];
                    src.group = cfg.currentGroup;
                    cfg.items.erase(cfg.items.begin() + srcCfg);
                    int insertVi = InsertIndexAt(pt);
                    size_t insCfg;
                    if (insertVi >= static_cast<int>(m_visible.size())) {
                        // Computed AFTER the erase -> already a valid index.
                        insCfg = GroupAppendPos();
                    } else {
                        // m_visible still holds PRE-erase cfg indexes: every
                        // index past the erased slot has shifted down by one.
                        // Without this correction the item lands one slot too
                        // late (the "icon does not follow the blue bar" bug).
                        insCfg = m_visible[insertVi];
                        if (insCfg > srcCfg) --insCfg;
                    }
                    if (insCfg > cfg.items.size()) insCfg = cfg.items.size();
                    cfg.items.insert(cfg.items.begin() + insCfg, src);
                    changed = true;
                } else {
                    // (b) in-group reorder: keep current group, insert by point.
                    changed = MoveTopItemToInsert(srcCfg, InsertIndexAt(pt));
                }
            }
        }
    } else {
        // ---- dragged a drawer member ----
        int srcDrawer = m_dragSrcDrawer;
        size_t srcMem = m_dragSrcMember;
        if (srcDrawer >= 0 && srcDrawer < static_cast<int>(cfg.items.size()) &&
            srcMem < cfg.items[srcDrawer].members.size()) {
            bool memberReorder = (m_openDrawer >= 0 && pt.y >= DrawerSplitY());
            if (memberReorder) {
                // (c) reorder within the drawer's lower half.
                int insertMi = MemberInsertIndexAt(pt);
                auto& mm = cfg.items[srcDrawer].members;
                Item src = mm[srcMem];
                mm.erase(mm.begin() + srcMem);
                if (insertMi > static_cast<int>(srcMem)) --insertMi;
                if (insertMi < 0) insertMi = 0;
                if (insertMi > static_cast<int>(mm.size())) insertMi = static_cast<int>(mm.size());
                mm.insert(mm.begin() + insertMi, src);
                changed = true;
            } else {
                // (d) move member out to parent grid / another drawer.
                int dv = DrawerCellAt(pt);
                Item src = cfg.items[srcDrawer].members[srcMem];
                if (dv >= 0) {
                    int dCfg = static_cast<int>(m_visible[dv]);
                    if (dCfg == srcDrawer) {
                        // 拖回自己抽屉的格子上：数据本就未被移除，保持原状即可。
                        // （旧代码在这里 insert 了一份副本却不删原件 → 抽屉里出现两个相同图标）
                    } else {
                        // 去重：目标抽屉已有同一图标（同路径+同类型）则不重复加入，源保持不动。
                        bool dup = false;
                        for (const auto& m : cfg.items[dCfg].members)
                            if (m.path == src.path && m.type == src.type) { dup = true; break; }
                        if (!dup) {
                            cfg.items[srcDrawer].members.erase(
                                cfg.items[srcDrawer].members.begin() + srcMem);
                            cfg.items[dCfg].members.push_back(src);
                            changed = true;
                        }
                    }
                } else {
                    cfg.items[srcDrawer].members.erase(
                        cfg.items[srcDrawer].members.begin() + srcMem);
                    src.group = cfg.currentGroup;  // show in the current group
                    InsertItemToGrid(src, InsertIndexAt(pt));
                    changed = true;
                }
            }
        }
    }

    if (changed) cfg.Save();
    m_dragging = false;
    m_dragFrom = -1;
    m_dragSrcDrawer = -1;
    m_dragOverTab = -1;
    m_dragOverDrawer = -1;
    ReleaseCapture();
    m_downValid = false;
    m_downIndex = -1;
    m_selected = -1;
    RebuildVisible();
    InvalidateGrid();
}

int MainWindow::InsertIndexAt(POINT pt) const {
    int hit = HitTestIcon(pt);
    if (hit >= 0) {
        // Drop on the right half of a cell -> after it; left half -> before.
        RECT r = CellRect(hit);
        int cx = (r.left + r.right) / 2;
        return (pt.x < cx) ? hit : hit + 1;
    }
    // Not directly over an icon: map the drop point to the nearest grid cell
    // so the item lands where the cursor is, instead of always appending at
    // the end (Bug 2: dragging into another group must respect the drop point).
    // relY is in CONTENT coordinates (+ scroll), matching CellRect().
    if (m_cols <= 0) return 0;
    int relX = pt.x - m_rcGrid.left;
    int relY = pt.y - m_rcGrid.top + m_scrollY;
    if (relX < 0) relX = 0;
    if (relY < 0) relY = 0;
    int col = relX / m_cellW;
    int row = relY / m_cellH;
    if (col >= m_cols) col = m_cols - 1;
    int idx = row * m_cols + col;
    if (idx < 0) idx = 0;
    if (idx > static_cast<int>(m_visible.size()))
        idx = static_cast<int>(m_visible.size());
    return idx;
}

void MainWindow::ShowIconMenu(int vi, POINT pt) {
    m_selected = vi;
    m_selectedMember = -1;
    InvalidateGrid();

    HMENU h = CreatePopupMenu();
    g_menuText.clear();
    g_menuDpi = m_dpi;
    // 快捷键提示一律不带 Alt 前缀：带菜单打开时 WndProc 里按裸字母就能触发，
    // 和主面板「按字母启动图标」不冲突（菜单打开期间字母被菜单钩子截走）。
    AddMenuText(h, ID_CTX_RENAME, L"重命名", L"F2");
    if (!m_app->GetConfig().items[m_visible[vi]].isDrawer)
        AddMenuText(h, ID_CTX_OPENLOC, L"打开目录", L"G");
    AddMenuText(h, ID_CTX_DELETE, L"删除图标", L"D");

    // 用户要求的固定顺序（中间不再有分隔线）：
    //   新建抽屉 → 移动分组 → 更换图标 → 设置快捷键
    AddMenuText(h, ID_CTX_NEWDRAWER, L"新建抽屉", L"N");

    // 「移动分组」：真正的系统子菜单（MF_POPUP），鼠标悬停就由系统弹出分组列表，
    // 右侧小三角也交给系统画 —— 自己画会和系统画的重叠成「两个箭头」。
    {
        const Config& cfg = m_app->GetConfig();
        if (cfg.groups.size() > 1) {
            HMENU hSub = CreatePopupMenu();
            for (int gi = 0; gi < static_cast<int>(cfg.groups.size()); ++gi) {
                if (cfg.groups[gi] == cfg.items[m_visible[vi]].group) continue;
                AddMenuText(hSub, ID_CTX_MOVETO_BASE + gi, cfg.groups[gi]);
            }
            if (GetMenuItemCount(hSub) > 0)
                AddMenuText(h, reinterpret_cast<UINT_PTR>(hSub), L"移动分组",
                            std::wstring(), true, 0, /*sysPopup=*/true);
            else
                DestroyMenu(hSub);
        }
    }

    // 「更换图标」：选一张图片（也可选 exe 提取图标）替换当前图标
    if (!m_app->GetConfig().items[m_visible[vi]].isDrawer)
        AddMenuText(h, ID_CTX_CHANGEICON, L"更换图标");

    // 「设置快捷键」：点开一个输入框，只收一个 A-Z 字母（不再罗列 26 个字母）
    AddMenuText(h, ID_CTX_SHORTCUT_SET, L"设置快捷键");

    SetForegroundWindow(m_hwnd);
    ClientToScreen(m_hwnd, &pt);
    g_menuKind = 1;   // icon menu shortcuts (F2 / G / D / N)
    HHOOK hook = SetWindowsHookExW(WH_KEYBOARD, KeyboardHookProc, nullptr,
                                    GetCurrentThreadId());
    HHOOK hook2 = SetWindowsHookExW(WH_GETMESSAGE, MenuGetMsgProc, nullptr,
                                     GetCurrentThreadId());
    TrackPopupMenu(h, TPM_RIGHTBUTTON | TPM_LEFTALIGN,
                   pt.x, pt.y, 0, m_hwnd, nullptr);
    if (hook) UnhookWindowsHookEx(hook);
    if (hook2) UnhookWindowsHookEx(hook2);
    g_menuKind = 0;
    DestroyMenu(h);
}

// 第6条需求：右键「更换图标」。选一张图片（或一个 exe 的内置图标），
// 统一转成 PNG 落到 data\icons 下，文件名带 "_custom" 后缀好认，
// 然后把 config.json 里该图标的 icon 字段换掉。
void MainWindow::ChangeItemIcon(int vi) {
    Config& cfg = m_app->GetConfig();

    Item* target = nullptr;
    if (m_openDrawer >= 0 && m_selectedMember >= 0 &&
        m_openDrawer < static_cast<int>(cfg.items.size())) {
        Item& d = cfg.items[m_openDrawer];
        if (m_selectedMember < static_cast<int>(d.members.size()))
            target = &d.members[m_selectedMember];
    } else if (vi >= 0 && vi < static_cast<int>(m_visible.size())) {
        const size_t idx = m_visible[vi];
        if (idx < cfg.items.size() && !cfg.items[idx].isDrawer)
            target = &cfg.items[idx];
    }
    if (!target) return;

    wchar_t file[MAX_PATH * 2] = {};
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = m_hwnd;
    ofn.lpstrFilter =
        L"图片 (*.ico;*.png;*.jpg;*.jpeg;*.bmp;*.gif)\0*.ico;*.png;*.jpg;*.jpeg;*.bmp;*.gif\0"
        L"程序 / 快捷方式（提取它的图标）\0*.exe;*.dll;*.lnk\0"
        L"所有文件 (*.*)\0*.*\0\0";
    ofn.lpstrFile  = file;
    ofn.nMaxFile   = MAX_PATH * 2;
    ofn.lpstrTitle = L"选择新图标（也可以选一个 exe，用它的图标）";
    ofn.lpstrDefExt = L"png";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER |
                OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return;   // 用户取消

    // .lnk 先解析成真实目标，否则提取不到图标
    std::wstring src = file;
    if (EndsWith(ToLower(src), L".lnk")) {
        std::wstring t;
        if (IconManager::ResolveLink(src, t)) src = t;
    }

    IconManager& im = m_app->GetIconManager();
    const std::wstring name = im.ImportCustomIcon(src, target->path);
    if (name.empty()) {
        MessageBoxW(m_hwnd, L"这个文件里没能读出图标，换一张图片试试。",
                    L"更换图标", MB_ICONWARNING | MB_OK);
        return;
    }
    target->icon = name;
    cfg.Save();
    RebuildVisible();
    InvalidateGrid();
}

void MainWindow::ShowMemberMenu(int mi, POINT pt) {    m_selectedMember = mi;
    m_selected = -1;
    InvalidateGrid();

    HMENU h = CreatePopupMenu();
    g_menuText.clear();
    g_menuDpi = m_dpi;
    // 与抽屉外图标菜单保持一致，仅少「新建抽屉」（抽屉内不能再建抽屉）。
    AddMenuText(h, ID_CTX_RENAME, L"重命名", L"F2");
    AddMenuText(h, ID_CTX_OPENLOC, L"打开目录", L"G");
    AddMenuText(h, ID_CTX_DELETE, L"删除图标", L"D");
    // 「移动分组」：把该成员移出抽屉，成为指定分组下的顶层图标
    {
        const Config& cfg = m_app->GetConfig();
        if (!cfg.groups.empty()) {
            HMENU hSub = CreatePopupMenu();
            for (int gi = 0; gi < static_cast<int>(cfg.groups.size()); ++gi)
                AddMenuText(hSub, ID_CTX_MOVETO_BASE + gi, cfg.groups[gi]);
            if (GetMenuItemCount(hSub) > 0)
                AddMenuText(h, reinterpret_cast<UINT_PTR>(hSub), L"移动分组",
                            std::wstring(), true, 0, /*sysPopup=*/true);
            else
                DestroyMenu(hSub);
        }
    }
    AddMenuText(h, ID_CTX_CHANGEICON, L"更换图标");
    AddMenuText(h, ID_CTX_SHORTCUT_SET, L"设置快捷键");
    SetForegroundWindow(m_hwnd);
    ClientToScreen(m_hwnd, &pt);
    g_menuKind = 3;   // member menu shortcuts (F2 / D / G)
    HHOOK hook = SetWindowsHookExW(WH_KEYBOARD, KeyboardHookProc, nullptr,
                                    GetCurrentThreadId());
    HHOOK hook2 = SetWindowsHookExW(WH_GETMESSAGE, MenuGetMsgProc, nullptr,
                                     GetCurrentThreadId());
    TrackPopupMenu(h, TPM_RIGHTBUTTON | TPM_LEFTALIGN,
                   pt.x, pt.y, 0, m_hwnd, nullptr);
    if (hook) UnhookWindowsHookEx(hook);
    if (hook2) UnhookWindowsHookEx(hook2);
    g_menuKind = 0;
    DestroyMenu(h);
}

void MainWindow::ShowTabMenu() {
    HMENU h = CreatePopupMenu();
    if (!h) return;
    g_menuText.clear();
    g_menuDpi = m_dpi;
    AddMenuText(h, ID_GROUP_NEW, L"新建分组", L"N");
    AddMenuText(h, ID_GROUP_REN, L"重命名分组", L"F2");
    AddMenuText(h, ID_GROUP_DEL, L"删除分组", L"D");
    SetForegroundWindow(m_hwnd);
    POINT pt = {0, 0};
    GetCursorPos(&pt);
    g_menuKind = 2;   // group-menu shortcuts (N / F2 / D) — NOT global
    HHOOK hook = SetWindowsHookExW(WH_KEYBOARD, KeyboardHookProc, nullptr,
                                    GetCurrentThreadId());
    HHOOK hook2 = SetWindowsHookExW(WH_GETMESSAGE, MenuGetMsgProc, nullptr,
                                     GetCurrentThreadId());
    TrackPopupMenu(h, TPM_RIGHTBUTTON | TPM_LEFTALIGN,
                   pt.x, pt.y, 0, m_hwnd, nullptr);
    if (hook) UnhookWindowsHookEx(hook);
    if (hook2) UnhookWindowsHookEx(hook2);
    g_menuKind = 0;
    DestroyMenu(h);
}

void MainWindow::SetCurrentGroup(int gi) {
    // Bug G: cancel any in-progress rename so the edit box never lingers
    // across a group switch (also triggered mid internal-drag, Bug H-e).
    ClearIconDblState();   // 切换分组，放弃未决的双击启动
    if (m_editRename) CancelRename();
    Config& cfg = m_app->GetConfig();
    if (gi < 0 || gi >= static_cast<int>(cfg.groups.size())) return;
    cfg.currentGroup = cfg.groups[gi];
    cfg.Save();
    m_selected = -1;
    m_selectedMember = -1;
    if (m_openDrawer >= 0) CloseDrawer();
    RebuildVisible();
    Relayout();   // refresh tab geometry for the (possibly new) group list
    UpdateTabScroll();   // keep the current group's tab fully visible
    InvalidateGrid();
}

// ---------------------------------------------------------------------------
// Group tab horizontal scroll + create/delete helpers (Requirement 5 & 6)
// ---------------------------------------------------------------------------

namespace {

// ============================================================
// 「快捷键...」弹窗：一个 EDIT（只收单个 A-Z）+ 清除 + 确定/取消
// 取代原来的 26 项子菜单 —— 用户要求「一个框写快捷键，后面加个清除按钮就好」。
// ============================================================
struct ScCtx {
    int          dpi = 96;
    HWND         hwnd = nullptr;
    HWND         edit = nullptr;
    HWND         btnOK = nullptr;
    HWND         btnCancel = nullptr;
    HWND         btnClear = nullptr;
    HINSTANCE    hInst = nullptr;
    std::wstring itemName;
    wchar_t      resultCh = 0;        // 确定后要写入的字母（0 = 无）
    bool         confirmed = false;
    bool         clearOnly = false;   // 点了「清除」→ 结果为「无快捷键」
    bool         done = false;
};

// EDIT 子类化：单字母输入 + 浅色占位提示
static WNDPROC s_scOldEditProc = nullptr;

// 第9条需求：输入框锁定「一个大写字母」。
// 无论字符是打字、粘贴、拖放还是输入法提交进来的，都统一走这里收口：
// 取第一个 A-Z（小写自动转大写，全角字母也认），其余全部丢掉。
static void NormalizeShortcutEdit(HWND hw) {
    wchar_t buf[64] = {};
    GetWindowTextW(hw, buf, 64);
    wchar_t keep = 0;
    for (int i = 0; buf[i]; ++i) {
        wchar_t c = buf[i];
        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
        // 全角 Ａ-Ｚ（U+FF21..U+FF3A）也换算成半角
        if (c >= 0xFF21 && c <= 0xFF3A) c = static_cast<wchar_t>(c - 0xFF21 + L'A');
        if (c >= L'A' && c <= L'Z') { keep = c; break; }
    }
    wchar_t want[2] = { keep, 0 };
    if (wcscmp(buf, want) != 0) {
        SetWindowTextW(hw, want);
        SendMessageW(hw, EM_SETSEL, keep ? 1 : 0, keep ? 1 : 0);
        InvalidateRect(hw, nullptr, TRUE);
    }
}

LRESULT CALLBACK ScEditProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CHAR) {
        const wchar_t ch = static_cast<wchar_t>(wp);
        if (ch == L'\b') {                       // 退格 => 清空（永远只有一个字符）
            SetWindowTextW(hw, L"");
            SendMessageW(hw, EM_SETSEL, 0, 0);
            return 0;
        }
        if (ch >= L'a' && ch <= L'z') {          // 小写自动转大写
            wchar_t b[2] = { static_cast<wchar_t>(ch - L'a' + L'A'), 0 };
            SetWindowTextW(hw, b);               // 直接替换，不累加
            SendMessageW(hw, EM_SETSEL, 1, 1);
            InvalidateRect(hw, nullptr, TRUE);
            return 0;
        }
        if (ch >= L'A' && ch <= L'Z') {
            wchar_t b[2] = { ch, 0 };
            SetWindowTextW(hw, b);
            SendMessageW(hw, EM_SETSEL, 1, 1);
            InvalidateRect(hw, nullptr, TRUE);
            return 0;
        }
        if (ch >= 32) { MessageBeep(MB_ICONASTERISK); return 0; }   // 其它可见字符一律拒收
        return 0;
    }
    if (msg == WM_KEYDOWN) {
        // Enter 确认 / Esc 取消：焦点在 EDIT 里，得转给父对话框。
        if (wp == VK_RETURN) { PostMessageW(GetParent(hw), WM_COMMAND, 1, 0); return 0; }
        if (wp == VK_ESCAPE) { PostMessageW(GetParent(hw), WM_COMMAND, 2, 0); return 0; }
    }
    LRESULT r = CallWindowProcW(s_scOldEditProc, hw, msg, wp, lp);
    // 粘贴 / 剪切 / 撤销 / 按键结束都再收口一次：EDIT 内部的 Ctrl+V 不走
    // WM_PASTE，拦不到，只能等按键结束再规范化，保证框里永远只剩一个大写字母。
    if (msg == WM_KEYUP || msg == WM_PASTE || msg == WM_CLEAR ||
        msg == WM_CUT   || msg == WM_UNDO)
        NormalizeShortcutEdit(hw);
    if (msg == WM_PAINT && GetWindowTextLengthW(hw) == 0) {
        // 浅色占位提示必须画在 EDIT 自己的 WM_PAINT 里 —— 在父窗口画会被
        // 子窗口整块盖掉（这个坑在搜索框占位提示上已经踩过一次）。
        HDC dc = GetDC(hw);
        if (dc) {
            RECT rc; GetClientRect(hw, &rc);
            HFONT f = reinterpret_cast<HFONT>(SendMessageW(hw, WM_GETFONT, 0, 0));
            HGDIOBJ of = f ? SelectObject(dc, f) : nullptr;
            int om = SetBkMode(dc, TRANSPARENT);
            COLORREF oc = SetTextColor(dc, RGB(0xA8, 0xAE, 0xB6));
            RECT tr = rc;
            tr.left += 6;
            DrawTextW(dc, L"A-Z", -1, &tr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SetTextColor(dc, oc);
            SetBkMode(dc, om);
            if (of) SelectObject(dc, of);
            ReleaseDC(hw, dc);
        }
    }
    return r;
}

LRESULT CALLBACK ShortcutDlgProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) {
        HDC dc = reinterpret_cast<HDC>(wp);
        RECT rc; GetClientRect(hw, &rc);
        FillRect(dc, &rc, reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        return 1;
    }
    if (msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLORBTN || msg == WM_CTLCOLOREDIT) {
        static HBRUSH s_white = CreateSolidBrush(RGB(255, 255, 255));
        HDC dc = reinterpret_cast<HDC>(wp);
        SetBkMode(dc, TRANSPARENT);
        SetBkColor(dc, RGB(255, 255, 255));
        // 控件 id 5 = 灰色说明行
        if (msg == WM_CTLCOLORSTATIC && GetDlgCtrlID(reinterpret_cast<HWND>(lp)) == 5)
            SetTextColor(dc, RGB(0x8A, 0x90, 0x99));
        return reinterpret_cast<LRESULT>(s_white);
    }
    if (msg == WM_CREATE) {
        CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lp);
        ScCtx* c = reinterpret_cast<ScCtx*>(cs->lpCreateParams);
        SetWindowLongPtrW(hw, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(c));
        c->hwnd = hw;
        double s = c->dpi / 96.0;
        RECT rc; GetClientRect(hw, &rc);

        static HFONT s_font = nullptr;
        static HFONT s_fontSmall = nullptr;
        if (!s_font) {
            // 字号必须跟布局用同一套 DPI（c->dpi），不能用 GetDeviceCaps：
            // 本程序窗口按 96 基准布局、由系统统一拉伸，若字体按屏幕真实
            // DPI（如 120）取字号，字会比布局大一圈，文字被挤到换行/裁切。
            const int dpiY = c->dpi;
            s_font = CreateFontW(-MulDiv(12, dpiY, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                                 FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
            s_fontSmall = CreateFontW(-MulDiv(10, dpiY, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                                      FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                      CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
        }
        const int pad = static_cast<int>(16 * s);   // 三处间距都用它：左/中/右

        // 第一行：设置快捷键（标题）
        HWND st = CreateWindowExW(0, L"STATIC", L"设置快捷键",
                                  WS_CHILD | WS_VISIBLE | SS_LEFT,
                                  pad, static_cast<int>(8 * s), rc.right - 2 * pad,
                                  static_cast<int>(22 * s), hw, nullptr, c->hInst, nullptr);
        SendMessageW(st, WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);

        // 第二行：灰色说明
        HWND note = CreateWindowExW(
            0, L"STATIC", L"仅支持单个字母 A-Z",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            pad, static_cast<int>(31 * s), rc.right - 2 * pad,
            static_cast<int>(16 * s), hw, reinterpret_cast<HMENU>(5), c->hInst, nullptr);
        SendMessageW(note, WM_SETFONT, reinterpret_cast<WPARAM>(s_fontSmall), TRUE);

        // 输入框 + 清除按钮
        const int ey = static_cast<int>(50 * s), eh = static_cast<int>(26 * s);
        const int clearW = static_cast<int>(54 * s);
        const int editW = rc.right - 2 * pad - clearW - static_cast<int>(8 * s);
        c->edit = CreateWindowExW(
            0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT | ES_AUTOHSCROLL | ES_UPPERCASE,
            pad, ey, editW, eh, hw,
            reinterpret_cast<HMENU>(3), c->hInst, nullptr);
        SendMessageW(c->edit, WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);
        s_scOldEditProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(c->edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ScEditProc)));
        // 第9条需求：摘掉这个输入框的输入法上下文。否则中文输入法会先把
        // 按键吃进组词状态，WM_CHAR 收不到，用户看到的可能是拼音/候选框，
        // 或者提交后进来的是全角字符 —— 表现就是「没能锁定大写 A-Z」。
        ImmAssociateContext(c->edit, nullptr);

        c->btnClear = CreateWindowExW(
            0, L"BUTTON", L"清除", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            pad + editW + static_cast<int>(8 * s), ey, clearW, eh, hw,
            reinterpret_cast<HMENU>(4), c->hInst, nullptr);
        SendMessageW(c->btnClear, WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);

        // 取消 / 确定：左边距 = 两钮间距 = 右边距 = pad
        const int bh = static_cast<int>(28 * s);
        const int by = rc.bottom - pad - bh;
        const int bw = (rc.right - 3 * pad) / 2;
        c->btnCancel = CreateWindowExW(
            0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            pad, by, bw, bh, hw, reinterpret_cast<HMENU>(2), c->hInst, nullptr);
        c->btnOK = CreateWindowExW(
            0, L"BUTTON", L"确定",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
            pad + bw + pad, by, bw, bh, hw, reinterpret_cast<HMENU>(1), c->hInst, nullptr);
        SendMessageW(c->btnOK, WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);
        SendMessageW(c->btnCancel, WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);

        SetFocus(c->edit);
        return 0;
    }
    ScCtx* c = reinterpret_cast<ScCtx*>(GetWindowLongPtrW(hw, GWLP_USERDATA));
    if (!c) return DefWindowProcW(hw, msg, wp, lp);
    switch (msg) {
        case WM_CLOSE:
            c->confirmed = false; c->done = true; DestroyWindow(hw);
            return 0;
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            if (id == 1) {                       // 确定
                // 必须在 DestroyWindow 之前把 EDIT 的文本取出来（控件销毁后句柄失效）
                wchar_t b[8] = {};
                GetWindowTextW(c->edit, b, 8);
                wchar_t c0 = b[0];
                if (c0 >= L'a' && c0 <= L'z') c0 = static_cast<wchar_t>(c0 - L'a' + L'A');
                c->resultCh = (c0 >= L'A' && c0 <= L'Z') ? c0 : 0;
                c->confirmed = true; c->done = true; DestroyWindow(hw);
            } else if (id == 2) {                // 取消
                c->confirmed = false; c->done = true; DestroyWindow(hw);
            } else if (id == 4) {                // 清除
                c->clearOnly = true; c->confirmed = true; c->done = true;
                DestroyWindow(hw);
            }
            return 0;
        }
        default:
            return DefWindowProcW(hw, msg, wp, lp);
    }
}


// Context for the custom delete-confirmation popup (Requirement 5).
struct ConfirmCtx {
    int dpi = 96;
    std::wstring message;
    bool result = false;   // true => confirmed delete
    bool done = false;     // loop termination flag
    HWND hwnd = nullptr;
    HWND btnOK = nullptr;
    HWND btnCancel = nullptr;
    HINSTANCE hInst = nullptr;
};

LRESULT CALLBACK ConfirmDlgProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {    // Pure-white body: paint the client background white and let the body text
    // draw transparently on top. Without this the class brush (COLOR_WINDOW+1)
    // leaves the dialog grey/dark (Requirement 3).
    if (msg == WM_ERASEBKGND) {
        HDC dc = reinterpret_cast<HDC>(wp);
        RECT rc; GetClientRect(hw, &rc);
        FillRect(dc, &rc, reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        return 1;
    }
    // Static body text: white background, transparent text (no grey box).
    if (msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLORBTN) {
        static HBRUSH s_white = CreateSolidBrush(RGB(255, 255, 255));
        HDC dc = reinterpret_cast<HDC>(wp);
        SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(s_white);
    }
    if (msg == WM_CREATE) {
        CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lp);
        ConfirmCtx* c = reinterpret_cast<ConfirmCtx*>(cs->lpCreateParams);
        SetWindowLongPtrW(hw, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(c));
        c->hwnd = hw;
        double s = c->dpi / 96.0;
        int pad = static_cast<int>(16 * s);
        RECT rc; GetClientRect(hw, &rc);
        // Microsoft YaHei, 12pt — created once and reused for the whole process.
        static HFONT s_font = nullptr;
        if (!s_font) {
            HDC dc = GetDC(hw);
            int dpiY = GetDeviceCaps(dc, LOGPIXELSY);
            ReleaseDC(hw, dc);
            int fh = -MulDiv(12, dpiY, 72);
            s_font = CreateFontW(fh, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
        }
        // Body text.
        HWND st = CreateWindowExW(0, L"STATIC", c->message.c_str(),
                        WS_CHILD | WS_VISIBLE | SS_LEFT,
                        pad, pad, rc.right - 2 * pad, static_cast<int>(40 * s),
                        hw, nullptr, c->hInst, nullptr);
        SendMessageW(st, WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);
        // Two buttons (确认 / 取消) centered symmetrically: total width =
        // 2*bw + gap, start centered in the client area. 确认 left, 取消 right.
        int bw = static_cast<int>(80 * s), bh = static_cast<int>(28 * s);
        int gap = static_cast<int>(8 * s);
        int total = 2 * bw + gap;
        int startX = (rc.right - total) / 2;
        int by = rc.bottom - bh - pad;
        c->btnOK = CreateWindowExW(
            0, L"BUTTON", L"确认",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
            startX, by, bw, bh, hw,
            reinterpret_cast<HMENU>(1), c->hInst, nullptr);
        c->btnCancel = CreateWindowExW(
            0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            startX + bw + gap, by, bw, bh, hw,
            reinterpret_cast<HMENU>(2), c->hInst, nullptr);
        SendMessageW(c->btnOK, WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);
        SendMessageW(c->btnCancel, WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);
        SetFocus(c->btnOK);
        return 0;
    }
    ConfirmCtx* c = reinterpret_cast<ConfirmCtx*>(
        GetWindowLongPtrW(hw, GWLP_USERDATA));
    switch (msg) {
        case WM_CLOSE:
            c->result = false; c->done = true; DestroyWindow(hw);
            return 0;
        case WM_COMMAND:
            if (HIWORD(wp) == BN_CLICKED) {
                int id = LOWORD(wp);
                if (id == 1) { c->result = true;  c->done = true; DestroyWindow(hw); }
                else if (id == 2) { c->result = false; c->done = true; DestroyWindow(hw); }
            }
            return 0;
        case WM_KEYDOWN:
            // Esc cancels (Enter / Space -> default button via IsDialogMessage).
            if (wp == VK_ESCAPE) { c->result = false; c->done = true; DestroyWindow(hw); }
            return 0;
        default:
            return DefWindowProcW(hw, msg, wp, lp);
    }
}

} // namespace

// 「快捷键...」弹窗：单个 A-Z 输入 + 清除。
void MainWindow::ShowShortcutDialog(int vi) {
    if (vi < 0 || vi >= static_cast<int>(m_visible.size())) return;
    Config& cfg = m_app->GetConfig();
    const size_t idx = m_visible[vi];
    if (idx >= cfg.items.size()) return;

    ScCtx ctx;
    ctx.dpi = m_dpi;
    ctx.hInst = m_hInst;
    ctx.itemName = cfg.items[idx].name;
    const wchar_t curCh = cfg.items[idx].shortcut;

    static bool s_reg = false;
    if (!s_reg) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = ShortcutDlgProc;
        wc.hInstance = m_hInst;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(IDC_ARROW));
        wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
        wc.lpszClassName = L"JisuQiShortcutDlg";
        wc.style = CS_HREDRAW | CS_VREDRAW;
        RegisterClassExW(&wc);
        s_reg = true;
    }

    double s = m_dpi / 96.0;
    // 目标客户区：240x126（原窗口 470x214 ≈ 长度一半、高度四分之三），
    // 再用 AdjustWindowRectEx 换算成含标题栏/边框的窗口尺寸，避免手算误差。
    const int cw = static_cast<int>(240 * s), ch = static_cast<int>(126 * s);
    RECT ar = {0, 0, cw, ch};
    AdjustWindowRectEx(&ar, WS_POPUP | WS_CAPTION | WS_BORDER, FALSE,
                       WS_EX_DLGMODALFRAME | WS_EX_TOPMOST);
    int w = ar.right - ar.left, h = ar.bottom - ar.top;
    RECT wr; GetWindowRect(m_hwnd, &wr);
    int x = wr.left + ((wr.right - wr.left) - w) / 2;
    int y = wr.top + ((wr.bottom - wr.top) - h) / 2;
    RECT wa = {0, 0, 0, 0};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
        if (x < wa.left) x = wa.left;
        if (y < wa.top) y = wa.top;
        if (x + w > wa.right) x = wa.right - w;
        if (y + h > wa.bottom) y = wa.bottom - h;
    }
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    HWND dlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        L"JisuQiShortcutDlg", L"快捷键",
        WS_POPUP | WS_CAPTION | WS_BORDER,
        x, y, w, h, m_hwnd, nullptr, m_hInst, &ctx);

    // 预填当前已绑定的字母
    if (curCh) {
        wchar_t b[2] = { curCh, 0 };
        SetWindowTextW(ctx.edit, b);
        SendMessageW(ctx.edit, EM_SETSEL, 1, 1);
    }

    EnableWindow(m_hwnd, FALSE);   // 真模态
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);
    SetFocus(ctx.edit);

    MSG m;
    BOOL got;
    while ((got = GetMessageW(&m, nullptr, 0, 0)) != 0) {
        if (ctx.done) break;
        HWND tgt = m.hwnd;
        if (tgt == dlg || IsChild(dlg, tgt)) {
            if (!IsDialogMessage(dlg, &m)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        } else {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    // 收到 WM_QUIT 时要把它投回队列，否则外层消息循环会永久阻塞（同 ConfirmDeleteGroup）
    if (got == 0) PostQuitMessage(static_cast<int>(m.wParam));
    EnableWindow(m_hwnd, TRUE);

    if (!ctx.confirmed) return;
    SetItemShortcut(vi, ctx.clearOnly ? 0 : ctx.resultCh);
}

void MainWindow::UpdateTabScroll() {
    if (!m_hwnd) return;
    RECT cr; GetClientRect(m_hwnd, &cr);
    double s = m_dpi / 96.0;
    int W = cr.right - cr.left;
    int tabStartX = static_cast<int>(12 * s);   // matches Relayout
    int gearLeft = W - static_cast<int>(64 * s);
    int avail = gearLeft - tabStartX;
    if (m_tabMaxScroll <= 0) { m_tabScroll = 0; return; }
    // Find the current group's on-screen tab rect and scroll it into view.
    const Config& cfg = m_app->GetConfig();
    int cur = 0;
    for (int gi = 0; gi < static_cast<int>(cfg.groups.size()); ++gi)
        if (cfg.groups[gi] == cfg.currentGroup) { cur = gi; break; }
    if (cur < 0 || cur >= static_cast<int>(m_tabRects.size())) return;
    RECT r = m_tabRects[cur];
    int delta = 0;
    if (r.right > tabStartX + avail)
        delta = r.right - (tabStartX + avail);   // scroll content right
    else if (r.left < tabStartX)
        delta = r.left - tabStartX;              // scroll content left (negative)
    int newScroll = m_tabScroll + delta;
    if (newScroll < 0) newScroll = 0;
    if (newScroll > m_tabMaxScroll) newScroll = m_tabMaxScroll;
    if (newScroll != m_tabScroll) {
        int shift = m_tabScroll - newScroll;     // move recorded rects by shift
        for (auto& tr : m_tabRects) { tr.left += shift; tr.right += shift; }
        m_tabScroll = newScroll;
        InvalidateGrid();
    }
}

void MainWindow::NewGroup() {
    Config& cfg = m_app->GetConfig();
    std::wstring name = L"新分组";
    int n = static_cast<int>(cfg.groups.size()) + 1;
    std::wstring cand = name + std::to_wstring(n);
    // Ensure uniqueness against existing group names.
    auto dup = [&](const std::wstring& s) {
        for (const auto& g : cfg.groups) if (g == s) return true;
        return false;
    };
    if (dup(cand)) {
        int m = n + 1;
        while (dup(name + std::to_wstring(m))) ++m;
        cand = name + std::to_wstring(m);
    }
    cfg.groups.push_back(cand);
    cfg.Save();
    int newIdx = static_cast<int>(cfg.groups.size()) - 1;
    SetCurrentGroup(newIdx);          // switches + Relayout (scrolls to new tab)
    StartTabRename(newIdx, true);     // enter rename for the fresh group
}

void MainWindow::ConfirmDeleteGroup(int idx) {
    Config& cfg = m_app->GetConfig();
    if (idx < 0 || idx >= static_cast<int>(cfg.groups.size())) return;
    if (cfg.groups.size() <= 1) { MessageBeep(MB_ICONWARNING); return; }
    if (idx == 0) {
        MessageBoxW(m_hwnd, L"默认分组不可删除。", L"极速启",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wstring group = cfg.groups[idx];
    // 重入守卫：下面的模态循环仍会派发“投递型”消息（EnableWindow(FALSE) 只挡
    // 键鼠输入，挡不住 PostMessage）。若此时又来一条 WM_APP_GROUP_DEL，第二次进入
    // 会在同一次删除流程里二次 erase 分组，造成越界 / 分组数据错乱。用 RAII 挡住。
    static bool s_deleting = false;
    if (s_deleting) return;
    s_deleting = true;
    struct ReentryGuard {
        bool* flag;
        ~ReentryGuard() { *flag = false; }
    } reentryGuard{&s_deleting};
    // Custom modal confirm dialog with "确认" / "取消" buttons (Requirement 5).
    ConfirmCtx ctx;
    ctx.dpi = m_dpi;
    ctx.hInst = m_hInst;
    ctx.message = L"确认删除分组「" + group + L"」？";
    static bool s_reg = false;
    if (!s_reg) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = ConfirmDlgProc;
        wc.hInstance = m_hInst;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(IDC_ARROW));
        wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
        wc.lpszClassName = L"JisuQiConfirmDlg";
        wc.style = CS_HREDRAW | CS_VREDRAW;
        RegisterClassExW(&wc);
        s_reg = true;
    }
    double s = m_dpi / 96.0;
    int w = static_cast<int>(320 * s), h = static_cast<int>(130 * s);
    // Center on the MAIN panel (screen rect of this window), not the client
    // rect (which is client-area coords, not screen coords — using it made the
    // dialog drift to the screen's top-left, Requirement 3).
    RECT wr; GetWindowRect(m_hwnd, &wr);
    int x = wr.left + ((wr.right - wr.left) - w) / 2;
    int y = wr.top  + ((wr.bottom - wr.top) - h) / 2;
    // Clamp inside the work area so it can never open off-screen.
    RECT wa = {0, 0, 0, 0};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
        if (x < wa.left) x = wa.left;
        if (y < wa.top)  y = wa.top;
        if (x + w > wa.right)  x = wa.right  - w;
        if (y + h > wa.bottom) y = wa.bottom - h;
    }
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    HWND dlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        L"JisuQiConfirmDlg", L"删除分组",
        WS_POPUP | WS_CAPTION | WS_BORDER,
        x, y, w, h, m_hwnd, nullptr, m_hInst, &ctx);
    if (!dlg) return;
    EnableWindow(m_hwnd, FALSE);   // make the confirm dialog truly modal
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);
    MSG m;
    BOOL got;
    while ((got = GetMessageW(&m, nullptr, 0, 0)) != 0) {
        if (ctx.done) break;
        HWND tgt = m.hwnd;
        if (tgt == dlg || IsChild(dlg, tgt)) {
            if (!IsDialogMessage(dlg, &m)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        } else {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    // 若循环是因收到 WM_QUIT 而结束（got == 0），必须把 WM_QUIT 重新投回队列。
    // 否则该消息被本嵌套循环吞掉，外层 App::Run() 的 GetMessageW 会永久阻塞，
    // 表现为：设置窗口开着时点托盘“退出”后进程退不掉（僵尸进程）。
    if (got == 0) PostQuitMessage(static_cast<int>(m.wParam));
    EnableWindow(m_hwnd, TRUE);
    if (!ctx.result) return;
    // Perform the deletion: reassign the group's items to the default group.
    if (m_openDrawer >= 0) CloseDrawer();   // avoid a dangling open-drawer ref
    std::wstring old = cfg.groups[idx];
    std::wstring remain = cfg.groups[0];   // default group is always index 0
    for (auto& it : cfg.items)
        if (it.group == old) it.group = remain;
    if (cfg.currentGroup == old) cfg.currentGroup = remain;
    cfg.groups.erase(cfg.groups.begin() + idx);
    cfg.Save();
    m_editGroup = -1;
    Relayout();
    RebuildVisible();
    InvalidateGrid();
}

int MainWindow::TabRectAt(POINT pt) const {
    for (int gi = 0; gi < static_cast<int>(m_tabRects.size()); ++gi) {
        if (PtInRect(&m_tabRects[gi], pt)) return gi;
    }
    return -1;
}

// ===== 托盘菜单：自绘弹窗 =====
// 为什么不用 HMENU：
//   1) 系统弹出菜单右侧固定预留一列（96dpi 约 55px、120dpi 约 72px），没有任何 API
//      能去掉它 —— 用户要的「右边和左边一样近」用系统菜单做不到；
//   2) 换成 owner-draw 更糟：行宽行高由我们上报，系统还会再补一段（实测 +46px），
//      行高也不采纳我们的值，于是出现「有的项缩进、末项被裁、右边更宽」。
// 所以自己开一个 WS_POPUP 窗口：左右内边距严格相等，行高自定，悬停高亮自绘，
// 按键由本窗口处理（弹出前 SetForegroundWindow，拿到焦点后 D/Esc/回车才有效）。
struct TrayMenuEntry {
    UINT id;                // 0 = 分隔线
    std::wstring text;      // 显示文字（不含 &）
    wchar_t mnemonic;       // 0 = 没有
};


static const wchar_t kTrayMenuClass[] = L"JisuQiTrayMenu";
static std::vector<TrayMenuEntry> g_trayItems;
static int   g_trayDpi   = 96;
static int   g_trayPadX  = 8;    // 左 = 右
static int   g_trayRowH  = 22;
static int   g_traySepH  = 8;
static int   g_trayTop   = 4;    // 上下内边距
static int   g_trayW     = 0;
static int   g_trayH     = 0;
static int   g_trayHot   = -1;   // 悬停/键盘选中项
static HWND  g_trayWnd   = nullptr;
static HWND  g_trayMain  = nullptr;   // 主窗口（命令投递目标）
static HHOOK g_trayKeyHook = nullptr;  // 菜单打开期间的键盘钩子
static bool  g_trayDone  = false;
static UINT  g_trayChosen = 0;
static DWORD g_trayOpenTick = 0;

static int TrayRowHeight(size_t i) {
    return g_trayItems[i].id == 0 ? g_traySepH : g_trayRowH;
}

// 返回命中的项下标；分隔线或空白返回 -1。
static int TrayHitTest(int y) {
    int y0 = g_trayTop;
    for (size_t i = 0; i < g_trayItems.size(); ++i) {
        const int h = TrayRowHeight(i);
        if (y >= y0 && y < y0 + h)
            return g_trayItems[i].id == 0 ? -1 : static_cast<int>(i);
        y0 += h;
    }
    return -1;
}

static void TrayMeasure() {
    int maxW = 0, textH = 0, trail = 0;
    wchar_t lastCh = 0;
    HDC dc = GetDC(nullptr);
    if (dc) {
        HGDIOBJ old = SelectObject(dc, MenuFont());
        for (const auto& it : g_trayItems) {
            if (it.id == 0) continue;
            SIZE sz;
            ZeroMemory(&sz, sizeof(sz));
            GetTextExtentPoint32W(dc, it.text.c_str(),
                                  static_cast<int>(it.text.size()), &sz);
            if (sz.cx > maxW) { maxW = sz.cx; lastCh = it.text.back(); }
            if (sz.cy > textH) textH = sz.cy;
        }
        // 全角括号「）」这类字符右侧有一块字形 bearing，只按步进宽度留白会显得
        // 右边比左边宽。把这部分扣掉，肉眼看到的左右留白才真的一样。
        if (lastCh) {
            ABCFLOAT abc;
            ZeroMemory(&abc, sizeof(abc));
            if (GetCharABCWidthsFloatW(dc, static_cast<UINT>(lastCh),
                                       static_cast<UINT>(lastCh), &abc)) {
                const int c = static_cast<int>(abc.abcfC + 0.5f);
                if (c > 0) trail = c;
            }
        }
        SelectObject(dc, old);
        ReleaseDC(nullptr, dc);
    }
    if (textH <= 0) textH = MulDiv(15, g_trayDpi, 96);
    g_trayPadX = MulDiv(7, g_trayDpi, 96);    // 左右各留同样多（用户要求两边一致）
    // 行高对齐系统菜单（SM_CYMENU），否则弹窗和系统菜单的行距会明显不一样。
    g_trayRowH = GetSystemMetrics(SM_CYMENU);
    if (g_trayRowH < textH + 2) g_trayRowH = textH + 2;
    g_traySepH = MulDiv(7, g_trayDpi, 96);
    g_trayTop  = MulDiv(3, g_trayDpi, 96);
    g_trayW = g_trayPadX * 2 + maxW - trail;
    if (g_trayW < g_trayPadX * 2 + 40) g_trayW = g_trayPadX * 2 + 40;
    g_trayH = g_trayTop * 2;
    for (size_t i = 0; i < g_trayItems.size(); ++i) g_trayH += TrayRowHeight(i);
}

static void TrayPaint(HWND hw, HDC dcIn) {
    PAINTSTRUCT ps;
    HDC dc = dcIn ? dcIn : BeginPaint(hw, &ps);
    RECT rc;
    GetClientRect(hw, &rc);

    HBRUSH bg = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    if (g_trayHot >= 0 && g_trayHot < static_cast<int>(g_trayItems.size())) {
        int y0 = g_trayTop;
        for (int i = 0; i < g_trayHot; ++i) y0 += TrayRowHeight(static_cast<size_t>(i));
        RECT hr = { 2, y0 + 1, rc.right - 2, y0 + TrayRowHeight(static_cast<size_t>(g_trayHot)) - 1 };
        HBRUSH hb = CreateSolidBrush(RGB(0xE5, 0xF4, 0xFD));
        FillRect(dc, &hr, hb);
        DeleteObject(hb);
    }

    const int obm = SetBkMode(dc, TRANSPARENT);
    const COLORREF ofg = SetTextColor(dc, RGB(0x2B, 0x2F, 0x36));
    HGDIOBJ oldFont = SelectObject(dc, MenuFont());

    int y = g_trayTop;
    for (const auto& it : g_trayItems) {
        const int h = it.id == 0 ? g_traySepH : g_trayRowH;
        if (it.id == 0) {
            // 分隔线：和文字左右对齐，不铺满整行（系统菜单也是这样缩进的）
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(0xE2, 0xE5, 0xEA));
            HGDIOBJ op = SelectObject(dc, pen);
            const int yl = y + h / 2;
            MoveToEx(dc, g_trayPadX, yl, nullptr);
            LineTo(dc, rc.right - g_trayPadX, yl);
            SelectObject(dc, op);
            DeleteObject(pen);
        } else {
            RECT r = { g_trayPadX, y, rc.right - g_trayPadX, y + h };
            DrawTextW(dc, it.text.c_str(), -1, &r,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        y += h;
    }

    SelectObject(dc, oldFont);
    SetTextColor(dc, ofg);
    SetBkMode(dc, obm);

    // 1px 边框（圆角由窗口 region 裁出来）
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(0xC6, 0xCB, 0xD2));
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, 0, 0, rc.right, rc.bottom);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);

    if (!dcIn) EndPaint(hw, &ps);
}

// 处理一次按键。菜单打开期间按键一律归菜单（和系统菜单一样吞掉所有键）。
static bool TrayHandleKey(WPARAM vk) {
    if (g_trayDone) return false;
    if (vk == VK_ESCAPE) {
        g_trayDone = true;
        return true;
    }
    if (vk == VK_RETURN || vk == VK_SPACE) {
        if (g_trayHot >= 0)
            g_trayChosen = g_trayItems[static_cast<size_t>(g_trayHot)].id;
        g_trayDone = true;
        return true;
    }
    if (vk == VK_DOWN || vk == VK_UP) {
        const int n = static_cast<int>(g_trayItems.size());
        int i = g_trayHot;
        for (int k = 0; k < n; ++k) {
            i = (vk == VK_DOWN) ? (i + 1) % n : (i <= 0 ? n - 1 : i - 1);
            if (g_trayItems[static_cast<size_t>(i)].id != 0) break;
        }
        if (i >= 0 && g_trayItems[static_cast<size_t>(i)].id != 0) {
            g_trayHot = i;
            if (g_trayWnd) InvalidateRect(g_trayWnd, nullptr, FALSE);
        }
        return true;
    }
    if (vk >= 'A' && vk <= 'Z') {          // 字母助记符（VK_A..VK_Z 与 'A'..'Z' 同值）
        for (size_t i = 0; i < g_trayItems.size(); ++i) {
            const wchar_t mn = g_trayItems[i].mnemonic;
            if (mn && (mn | 0x20) == (static_cast<int>(vk) | 0x20)) {
                g_trayChosen = g_trayItems[i].id;
                g_trayDone = true;
                return true;
            }
        }
    }
    return true;   // 其余键也吞掉，免得菜单开着时主窗口响应按键
}

// 收起弹窗。选中了某项就把命令投回主窗口。
// 故意不用嵌套消息循环：弹出即返回（和系统菜单一样是非模态的），
// 否则「退出」在嵌套循环里被处理时容易卡住主流程。
static void TrayClose() {
    HWND w = g_trayWnd;
    if (!w) return;
    g_trayWnd = nullptr;
    if (g_trayKeyHook) {
        UnhookWindowsHookEx(g_trayKeyHook);
        g_trayKeyHook = nullptr;
    }
    if (GetCapture() == w) ReleaseCapture();
    DestroyWindow(w);
    if (g_trayChosen && g_trayMain) {
        const UINT id = g_trayChosen;
        g_trayChosen = 0;
        PostMessageW(g_trayMain, WM_COMMAND, MAKEWPARAM(id, 0), 0);
    }
}

// 菜单打开期间的键盘处理放在低级键盘钩子里：托盘菜单弹出时本进程不一定持有
// 键盘焦点（焦点可能还留在主窗口或任务栏），只有 WH_KEYBOARD_LL 能稳定收到按键。
// 菜单开着时按键一律吞掉（和系统菜单一致）。
static LRESULT CALLBACK TrayLLKeyProc(int nCode, WPARAM wp, LPARAM lp) {
    if (nCode == HC_ACTION && g_trayWnd) {
        const KBDLLHOOKSTRUCT* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        if (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) {
            TrayHandleKey(k->vkCode);
            if (g_trayDone) TrayClose();
            return 1;
        }
        if (wp == WM_KEYUP || wp == WM_SYSKEYUP) return 1;
    }
    return CallNextHookEx(nullptr, nCode, wp, lp);
}

static LRESULT CALLBACK TrayMenuProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_MOUSEMOVE: {
        const int idx = TrayHitTest(static_cast<short>(HIWORD(lp)));
        if (idx != g_trayHot) {
            g_trayHot = idx;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        const int idx = TrayHitTest(static_cast<short>(HIWORD(lp)));
        if (idx >= 0) g_trayChosen = g_trayItems[static_cast<size_t>(idx)].id;
        g_trayDone = true;
        TrayClose();
        return 0;
    }
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_MOUSEWHEEL:
        g_trayDone = true;
        TrayClose();
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (TrayHandleKey(wp) && g_trayDone) TrayClose();
        return 0;
    // 失去焦点 / 输入被抢 / 前台被别的窗口拿走，都要收起。弹出瞬间（<120ms）
    // 忽略，避免激活过程中的瞬时通知把菜单自己关掉。
    case WM_KILLFOCUS:
    case WM_CAPTURECHANGED:
        if (GetTickCount() - g_trayOpenTick > 120) TrayClose();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && GetTickCount() - g_trayOpenTick > 250)
            TrayClose();
        return 0;
    case WM_DESTROY:
        // 正常收起走 TrayClose()（先卸钩子再销毁）；这里兜住"被别处连带销毁"的情况，
        // 免得钩子留在系统里（虽然判空后等于直通，但没必要长期挂）。
        g_trayWnd = nullptr;
        if (g_trayKeyHook) {
            UnhookWindowsHookEx(g_trayKeyHook);
            g_trayKeyHook = nullptr;
        }
        break;
    case WM_PAINT:
        TrayPaint(h, nullptr);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW)));
        return TRUE;
    }
    return DefWindowProcW(h, m, wp, lp);
}

void MainWindow::ShowTrayMenu() {
    if (g_trayWnd) return;                 // 已经开着就别重复弹
    g_trayItems.clear();
    g_trayItems.push_back({ ID_TRAY_SHOW, L"显示窗口", 0 });
    g_trayItems.push_back({ ID_TRAY_SETTINGS, L"设置", 0 });
    g_trayItems.push_back({ 0, L"", 0 });
    g_trayItems.push_back({ ID_TRAY_PAUSE_HOTKEY,
                            g_pauseHotkey ? L"暂停快捷键（开）" : L"暂停快捷键（关）", 0 });
    g_trayItems.push_back({ ID_TRAY_PAUSE_MOUSE,
                            g_pauseMouse ? L"暂停鼠标键（开）" : L"暂停鼠标键（关）", 0 });
    g_trayItems.push_back({ 0, L"", 0 });
    g_trayItems.push_back({ ID_TRAY_EXIT, L"退出(D)", L'D' });

    g_trayDpi = m_dpi > 0 ? m_dpi : 96;
    TrayMeasure();

    static bool sClassReg = false;
    if (!sClassReg) {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = TrayMenuProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        wc.lpszClassName = kTrayMenuClass;
        if (!RegisterClassExW(&wc)) return;
        sClassReg = true;
    }

    POINT pt = { 0, 0 };
    GetCursorPos(&pt);
    int x = pt.x;
    int y = pt.y - g_trayH;    // 底边对齐光标（原来是 TPM_BOTTOMALIGN 的效果）
    MONITORINFO moni;
    ZeroMemory(&moni, sizeof(moni));
    moni.cbSize = sizeof(moni);
    if (GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &moni)) {
        if (x + g_trayW > moni.rcWork.right) x = moni.rcWork.right - g_trayW;
        if (x < moni.rcWork.left) x = moni.rcWork.left;
        if (y + g_trayH > moni.rcWork.bottom) y = moni.rcWork.bottom - g_trayH;
        if (y < moni.rcWork.top) y = moni.rcWork.top;
    }

    SetForegroundWindow(m_hwnd);   // 托盘菜单的常规做法，保证弹窗能拿到输入
    HWND w = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kTrayMenuClass, L"",
                             WS_POPUP, x, y, g_trayW, g_trayH,
                             m_hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!w) return;
    HRGN rgn = CreateRoundRectRgn(0, 0, g_trayW + 1, g_trayH + 1,
                                  MulDiv(10, g_trayDpi, 96), MulDiv(10, g_trayDpi, 96));
    if (rgn) SetWindowRgn(w, rgn, TRUE);   // region 交给系统，不能自己删

    g_trayMain = m_hwnd;
    g_trayWnd = w;
    g_trayHot = -1;
    g_trayDone = false;
    g_trayChosen = 0;
    g_trayOpenTick = GetTickCount();
    ShowWindow(w, SW_SHOW);
    SetForegroundWindow(w);   // 拿到焦点，D / Esc / 回车才有效
    SetActiveWindow(w);       // 同一线程里还有主窗口，必须显式设为本线程的活动窗口
    SetFocus(w);
    SetCapture(w);            // 鼠标点到菜单外面也能收到，用来收起
    g_trayKeyHook = SetWindowsHookExW(WH_KEYBOARD_LL, TrayLLKeyProc,
                                      GetModuleHandleW(nullptr), 0);
}

// ---------------------------------------------------------------------------
// Edit control subclass procedures
// ---------------------------------------------------------------------------

LRESULT CALLBACK MainWindow::SearchEditProc(HWND hwnd, UINT msg,
                                           WPARAM wp, LPARAM lp) {
    MainWindow* self = reinterpret_cast<MainWindow*>(
        GetWindowLongPtrW(GetParent(hwnd), GWLP_USERDATA));
    if (msg == WM_KEYDOWN) {
        if (wp == VK_RETURN) {
            PostMessageW(GetParent(hwnd), WM_APP_SEARCH_ENTER, 0, 0);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            // Requirement 2: first ESC clears the search box; a second Esc
            // (when the box is already empty) hides the panel.
            if (GetWindowTextLengthW(hwnd) > 0) {
                SetWindowTextW(hwnd, L"");
                return 0;
            }
            PostMessageW(GetParent(hwnd), WM_APP_HIDE, 0, 0);
            return 0;
        }
        // Requirement 1: F2 is NOT a global shortcut; it only renames an icon
        // when the icon context menu is open.
    } else if (msg == WM_PAINT) {
        // 占位提示必须在 EDIT 自己的 WM_PAINT 里画：EDIT 是子窗口，永远显示在
        // 父窗口客户区之上，父窗口里画的提示会被它整块盖住。
        // 内容带当前「搜索框快捷键」，随设置实时变化（见 HotkeyText）。
        LRESULT r = CallWindowProcW(self ? self->GetOldSearchProc() : DefWindowProcW,
                                    hwnd, msg, wp, lp);
        if (GetWindowTextLengthW(hwnd) == 0 && GetFocus() != hwnd) {
            HDC dc = GetDC(hwnd);
            if (dc) {
                const ::Settings& st = GetSettings();
                const std::wstring key = HotkeyText(st.sboxMod, st.sboxVk);
                const std::wstring cue = key.empty()
                    ? std::wstring(L"搜索应用，回车启动")
                    : key + L" 搜索应用，回车启动";
                HFONT hf = reinterpret_cast<HFONT>(
                    SendMessageW(hwnd, WM_GETFONT, 0, 0));
                HGDIOBJ oldFont = SelectObject(dc, hf ? hf
                                                      : GetStockObject(DEFAULT_GUI_FONT));
                COLORREF oldColor = SetTextColor(dc, RGB(0x9A, 0xA0, 0xA6));
                int oldMode = SetBkMode(dc, TRANSPARENT);
                RECT rc; GetClientRect(hwnd, &rc);
                rc.left += 1;   // 和输入文字左边缘对齐
                DrawTextW(dc, cue.c_str(), -1, &rc,
                          DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER |
                          DT_END_ELLIPSIS);
                SetBkMode(dc, oldMode);
                SetTextColor(dc, oldColor);
                SelectObject(dc, oldFont);
                ReleaseDC(hwnd, dc);
            }
        }
        return r;
    } else if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) {
        // 焦点进出会改变「是否显示占位提示」：让 EDIT 自己重画一次。
        InvalidateRect(hwnd, nullptr, TRUE);
        // 搜索框要能输入中文：进框时装回 IME，离开（回主窗口，字母键用于快速
        // 启动）时再摘掉，避免中文输入法把字母截成拼音候选。
        SetImeEnabled(GetParent(hwnd), msg == WM_SETFOCUS);
    }
    return CallWindowProcW(self->GetOldSearchProc(), hwnd, msg, wp, lp);
}

LRESULT CALLBACK MainWindow::RenameEditProc(HWND hwnd, UINT msg,
                                           WPARAM wp, LPARAM lp) {
    MainWindow* self = reinterpret_cast<MainWindow*>(
        GetWindowLongPtrW(GetParent(hwnd), GWLP_USERDATA));
    // Guard: the edit may already have been torn down (e.g. an explicit commit
    // issued from a WM_LBUTTONDOWN handler). In that case just let the system
    // finish the focus change without re-entering the rename logic.
    if (!self || !self->m_editRename)
        return CallWindowProcW(self ? self->GetOldRenameProc()
                                    : DefWindowProcW, hwnd, msg, wp, lp);
    if (msg == WM_KEYDOWN) {
        if (wp == VK_RETURN) {
            wchar_t buf[512] = {0};
            GetWindowTextW(hwnd, buf, 512);
            std::wstring t(buf);
            size_t a = t.find_first_not_of(L" \t\r\n");
            size_t b = t.find_last_not_of(L" \t\r\n");
            if (a == std::wstring::npos) t.clear();
            else t = t.substr(a, b - a + 1);
            self->CommitRename(t);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            self->CancelRename();
            return 0;
        }
    } else if (msg == WM_KILLFOCUS) {
        // Requirement 4: losing focus confirms the edit. Trim surrounding
        // whitespace; an empty result keeps the original name (CancelRename).
        wchar_t buf[512] = {0};
        GetWindowTextW(hwnd, buf, 512);
        std::wstring t(buf);
        size_t a = t.find_first_not_of(L" \t\r\n");
        size_t b = t.find_last_not_of(L" \t\r\n");
        if (a == std::wstring::npos) t.clear();
        else t = t.substr(a, b - a + 1);
        if (!t.empty()) self->CommitRename(t);
        else self->CancelRename();
        return 0;
    }
    return CallWindowProcW(self->GetOldRenameProc(), hwnd, msg, wp, lp);
}

} // namespace JisuQi
