// view.cpp —— 结果视图：无边框原图 + 拖选文字灰底白字高亮 + 右键菜单复制
// 交互（用户确认）：
//   - 截图后直接显示一张无边框原图（默认 1:1，居中；图大于屏幕时可滚动）
//   - Ctrl+滚轮缩放；滚轮/滚动条滚动；图比窗口小则居中显示
//   - 平时是干净的原图（没有任何识别框）
//   - 左键拖选：命中的文字呈“灰底白字”高亮
//   - 右键：弹出菜单【复制文字 / 关闭】；左键点“复制文字”即复制
//   - 复制成功：底部提示“已复制到剪贴板”，随后自动关闭；ESC 也可随时关闭
#include "ocraddon_internal.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <utility>   // std::move
#include <mutex>
#include <atomic>
#include <functional>
#include "text_join.h"   // 智能换行：textjoin::JoinLines
#include <shlobj.h>      // 保存到桌面（CSIDL_DESKTOPDIRECTORY）
#include <gdiplus.h>     // 标注与工具栏的抗锯齿绘制（wWinMain 已 GdiplusStartup）

// GET_X_LPARAM / GET_Y_LPARAM are normally in <windowsx.h>, but that header
// conflicts with other definitions. Define them locally to stay self-contained.
#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((LONG)(short)LOWORD(lp))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lp) ((LONG)(short)HIWORD(lp))
#endif

namespace {

namespace gp = Gdiplus;   // GDI+ 简写别名（本文件只用，不污染外部）
using namespace Gdiplus;  // 本 TU 内直接使用 GDI+ 类型名（Point/Color/...）

constexpr double kZoomMin = 0.2;
constexpr double kZoomMax = 4.0;
// 缩放时窗口跟随内容的上限：宽或高任一达到屏幕工作区 80% 后窗口不再扩大，
// 之后继续放大只作用于图片内容（可用滚动/平移查看）。
constexpr double kMaxScreenFrac = 0.80;
constexpr COLORREF kHiBg = RGB(208, 213, 221);   // 高亮背景：#d0d5dd（#ebeef2 太淡，用户要求加深）
constexpr COLORREF kBg = RGB(32, 32, 32);        // 图外空白背景

// ---- 标注工具（微信风格）----
// 工具：1=矩形 2=椭圆 3=箭头 4=笔；AT_TEXT(5)=「抓取文字」=普通框选文字模式（默认）。
enum { AT_NONE = 0, AT_RECT, AT_ELLIPSE, AT_ARROW, AT_PEN, AT_TEXT };
constexpr int kAnnoColors[6][3] = {
    { 230,  60,  60},   // 红（默认）
    { 245, 200,  40},   // 黄
    {  50, 130, 245},   // 蓝
    {  40, 180,  90},   // 绿
    {  35,  35,  35},   // 黑
    { 255, 255, 255},   // 白
};
constexpr int kAnnoWidths[4] = { 2, 4, 6, 9 };   // 线宽（图像像素，随缩放呈现）

struct Anno {
    int tool = AT_NONE;
    int color = 0;                   // kAnnoColors 下标
    int widthIdx = 0;                // kAnnoWidths 下标
    POINT a{ 0, 0 }, b{ 0, 0 };      // 图像像素坐标（矩形/椭圆/箭头两端）
    std::vector<POINT> pts;          // 笔：折线点集（图像像素坐标）
};

struct VState {
    CaptureImage img;
    std::vector<OcrWord> words;      // 按值持有，避免悬垂指针
    double s = 1.0;                  // 缩放比例
    int scrollX = 0, scrollY = 0;    // 内容超出窗口时的滚动偏移
    int clientW = 0, clientH = 0;
    bool resizing = false;           // 边缘拖拽调整窗口大小进行中
    int  rsHT = 0;                   // 命中的边/角（HTLEFT..HTBOTTOMRIGHT）
    POINT rsStartPt = {0, 0};        // 拖拽起点（屏幕坐标）
    RECT  rsStartRect = {0,0,0,0};   // 拖拽起点的窗口矩形
    bool userResized = false;        // 用户手动拖过窗口大小：此后缩放不再改变窗口尺寸
    bool drag = false;
    POINT dragStart = {0, 0};        // 客户区坐标
    RECT selImg = {0, 0, 0, 0};      // 选区（图像像素坐标）
    bool hasSel = false;
    bool toast = false;              // 复制成功提示
    std::wstring toastMsg = L"已复制到剪贴板";   // 提示内容（复制文字 / 标注图 / 撤销）
    bool ocrPending = true;          // 后台 OCR 是否尚未完成（完成前无高亮）
    HWND hwnd = nullptr;
    HBITMAP hSrc = nullptr;          // 原图 24bpp DIB（只建一次，与缩放无关）
    // ---- 新增交互 ----
    bool panning = false;            // Ctrl+左键拖动平移图片
    POINT panStart = {0, 0};         // 平移起点（客户区坐标）
    int scrollStartX = 0, scrollStartY = 0; // 平移起点滚动偏移
    // ---- 标注 ----
    int  tool = AT_TEXT;             // 当前模式：默认「抓取文字」=普通框选文字
    int  annColor = 0;               // 当前颜色下标（默认红）
    int  annWidth = 1;               // 当前粗细档（默认第 2 档）
    std::vector<Anno> annos;         // 已完成的标注（图像像素坐标，缩放无关）
    bool drawing = false;            // 正在绘制当前笔画
    Anno  cur;                       // 进行中的标注
};
VState g;
std::mutex g_wordsMutex; // 保护 g.words：窗口线程读，OCR 工作线程写
static std::atomic<int> g_resultGen{0}; // 结果代次：新截图递增，旧 OCR 回调据此丢弃
static int g_exitMode = 0;           // 退出方式：0=复制后直接退出（默认），1=复制后 ESC 退出

// 把 RGB(top-down) 图像构建成 24bpp bottom-up BGR 的 DIB section（只做一次）。
HBITMAP BuildSourceDIB(int w, int h, const std::vector<unsigned char>& rgb) {
    if (w <= 0 || h <= 0 || rgb.empty()) return nullptr;
    int rowBytes = ((w * 3 + 3) / 4) * 4;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = h; // positive => bottom-up
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 24;
    bmi.bmiHeader.biCompression = BI_RGB;
    bmi.bmiHeader.biSizeImage = (DWORD)rowBytes * h;
    HDC hScr = GetDC(NULL);
    void* bits = nullptr;
    HBITMAP hb = CreateDIBSection(hScr, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, hScr);
    if (hb && bits) {
        for (int y = 0; y < h; y++) {
            int srcY = h - 1 - y;
            const unsigned char* srcRow = &rgb[(size_t)srcY * w * 3];
            BYTE* dstRow = (BYTE*)bits + (size_t)y * rowBytes;
            for (int x = 0; x < w; x++) {
                dstRow[x * 3 + 0] = srcRow[x * 3 + 2]; // B
                dstRow[x * 3 + 1] = srcRow[x * 3 + 1]; // G
                dstRow[x * 3 + 2] = srcRow[x * 3 + 0]; // R
            }
        }
    }
    return hb;
}

int ContentW() { return (int)(g.img.width * g.s + 0.5); }
int ContentH() { return (int)(g.img.height * g.s + 0.5); }

// 内容在客户区中的绘制原点：小图居中，大图随滚动偏移
void ComputeOrigin(int& ox, int& oy) {
    ox = (ContentW() <= g.clientW) ? (g.clientW - ContentW()) / 2 : -g.scrollX;
    oy = (ContentH() <= g.clientH) ? (g.clientH - ContentH()) / 2 : -g.scrollY;
}

void ClampScroll() {
    g.scrollX = (ContentW() <= g.clientW) ? 0
              : std::max(0, std::min(g.scrollX, ContentW() - g.clientW));
    g.scrollY = (ContentH() <= g.clientH) ? 0
              : std::max(0, std::min(g.scrollY, ContentH() - g.clientH));
}

void ApplyScrollbars(HWND h) {
    SCROLLINFO si{ sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS };
    si.nMax = ContentW(); si.nPage = (UINT)g.clientW; si.nPos = g.scrollX;
    SetScrollInfo(h, SB_HORZ, &si, TRUE);
    si.nMax = ContentH(); si.nPage = (UINT)g.clientH; si.nPos = g.scrollY;
    SetScrollInfo(h, SB_VERT, &si, TRUE);
}

void InvalidateAll(HWND h) { InvalidateRect(h, NULL, FALSE); }

// 客户区坐标 -> 图像像素坐标
POINT ClientToImg(POINT c) {
    int ox, oy; ComputeOrigin(ox, oy);
    POINT p;
    p.x = (int)((c.x - ox) / g.s);
    p.y = (int)((c.y - oy) / g.s);
    return p;
}

// 图像像素坐标 -> 客户区坐标
RECT ImgToClient(const RECT& r) {
    int ox, oy; ComputeOrigin(ox, oy);
    RECT c;
    c.left   = ox + (int)(r.left * g.s);
    c.top    = oy + (int)(r.top * g.s);
    c.right  = ox + (int)(r.right * g.s);
    c.bottom = oy + (int)(r.bottom * g.s);
    return c;
}

bool IsAlnumAscii(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

// 把一组词（行内）按原顺序拼接；仅当边界两字符都是 ASCII 字母/数字时补一个空格
// （中文直接连写）。三种换行模式共用。
static std::string JoinWordsWithSpace(const std::vector<OcrWord>& words) {
    std::string out;
    for (const OcrWord& wd : words) {
        if (out.empty()) { out = wd.text; continue; }
        bool engJoin = IsAlnumAscii((unsigned char)out.back()) &&
                       !wd.text.empty() &&
                       IsAlnumAscii((unsigned char)wd.text.front());
        if (engJoin) out += ' ';
        out += wd.text;
    }
    return out;
}

// 把词按“行”分组（垂直重叠超过较小高度一半视为同一行），行内再按 x 从左到右。
static std::vector<std::vector<OcrWord>> GroupByLine(const std::vector<OcrWord>& words) {
    std::vector<OcrWord> sorted = words;
    std::sort(sorted.begin(), sorted.end(), [](const OcrWord& a, const OcrWord& b) {
        int ca = a.y + a.h / 2, cb = b.y + b.h / 2;
        if (std::abs(ca - cb) > 8) return ca < cb; // 不同行按 y
        return a.x < b.x;                            // 同行按左到右
    });
    std::vector<std::vector<OcrWord>> lines;
    for (const OcrWord& wd : sorted) {
        bool merged = false;
        for (auto& ln : lines) {
            int y0 = 1 << 30, y1 = 0;
            for (const auto& p : ln) { y0 = std::min(y0, p.y); y1 = std::max(y1, p.y + p.h); }
            int ovTop = std::max(y0, wd.y), ovBot = std::min(y1, wd.y + wd.h);
            int minH  = std::min(y1 - y0, wd.h);
            if (ovBot - ovTop > minH / 2) { ln.push_back(wd); merged = true; break; }
        }
        if (!merged) lines.push_back({ wd });
    }
    for (auto& ln : lines)
        std::sort(ln.begin(), ln.end(), [](const OcrWord& a, const OcrWord& b) { return a.x < b.x; });
    return lines;
}

// 收集选区命中的词（阅读顺序），按全局换行模式格式化。
// 换行模式由宿主/默认值决定：Smart=智能换行（JoinLines），Keep=保留原始换行。
// 注意：g_lineMode 留在匿名命名空间（内部链接）；SetLineMode 的定义在文件末尾
// 匿名命名空间之外（供 ocraddon.cpp 跨编译单元调用，否则 LNK2019）。
static NewlineMode g_lineMode = NewlineMode::Smart;

std::string CollectSelectedText() {
    std::vector<OcrWord> picked;
    {
        std::lock_guard<std::mutex> lk(g_wordsMutex);
        for (const OcrWord& wd : g.words) {
            if (wd.x + wd.w < g.selImg.left || wd.x > g.selImg.right ||
                wd.y + wd.h < g.selImg.top || wd.y > g.selImg.bottom) continue;
            picked.push_back(wd);
        }
    }
    return FormatSelection(picked, g_lineMode);
}

bool CopyTextToClipboard(HWND h, const std::string& utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (n <= 0) return false;
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, (size_t)n * sizeof(wchar_t));
    if (!hg) return false;
    wchar_t* p = (wchar_t*)GlobalLock(hg);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, p, n);
    GlobalUnlock(hg);
    if (!OpenClipboard(h)) { GlobalFree(hg); return false; }
    EmptyClipboard();
    SetClipboardData(CF_UNICODETEXT, hg); // 成功后剪贴板拥有该内存
    CloseClipboard();
    return true;
}

// 复制选中文字并按“退出方式”处理后续的辅助函数（定义见下方，先前向声明）。
static void DoCopy(HWND h);

// 复制选中的文字到剪贴板，并按“退出方式”处理后续：
//   exit=0 复制后约 1 秒自动关闭；exit=1 复制后保持打开，等待 ESC 关闭。
// 同时供右键菜单与 Ctrl+C 调用，避免逻辑重复。
static void DoCopy(HWND h) {
    if (!g.hasSel) return;
    std::string sel = CollectSelectedText();
    if (sel.empty()) return;
    if (!CopyTextToClipboard(h, sel)) return;
    g.toast = true;
    g.toastMsg = (g_exitMode == 1) ? L"已复制到剪贴板（ESC 关闭）"
                                   : L"已复制到剪贴板";
    if (g_exitMode == 0)
        SetTimer(h, 1, 1000, NULL);   // 默认：提示 1 秒后自动关闭
    InvalidateAll(h);
}

// 右键菜单：复制文字（左键点确认）/ 关闭
void ShowContextMenu(HWND h, POINT clientPt) {
    HMENU menu = CreatePopupMenu();
    std::string sel = g.hasSel ? CollectSelectedText() : std::string();
    AppendMenuW(menu, (sel.empty() ? MF_GRAYED : MF_STRING), 1, L"复制文字");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, 2, L"关闭");
    POINT sp = clientPt;
    ClientToScreen(h, &sp);
    SetForegroundWindow(h);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                             sp.x, sp.y, 0, h, NULL);
    DestroyMenu(menu);
    if (cmd == 1) {
        DoCopy(h);            // 复制并依据“退出方式”处理后续
    } else if (cmd == 2) {
        DestroyWindow(h);
    }
}

// 缩放时的窗口跟随逻辑（用户定案）：
//   ① 用户手动拖过窗口大小后，窗口尺寸永久固定，缩放不再改变（userResized）；
//   ② 默认 = 原图 + 20px 余量（滚动条位，见 ShowResultAndCopy）；
//   ③ 放大时窗口跟随变大，上限为屏幕 80%（宽或高任一到达即停）；
//      缩小时窗口【不】跟随缩小——窗口只会因用户手动拖动而变小。
void FitWindowToContent(HWND h) {
    if (!h || g.img.width <= 0) return;
    if (g.userResized) return;          // ① 手动调整过：完全固定
    HMONITOR hm = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfo(hm, &mi))
        mi.rcWork = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    int maxW = (int)((mi.rcWork.right - mi.rcWork.left) * kMaxScreenFrac);
    int maxH = (int)((mi.rcWork.bottom - mi.rcWork.top) * kMaxScreenFrac);
    int wantW = std::min(ContentW(), maxW);
    int wantH = std::min(ContentH(), maxH);
    wantW = std::max(wantW, 320);
    wantH = std::max(wantH, 240);
    RECT rc; GetClientRect(h, &rc);
    int dx = wantW - (rc.right - rc.left);
    int dy = wantH - (rc.bottom - rc.top);
    if (dx < 0) dx = 0;                 // ③ 缩小不缩窗（grow-only）
    if (dy < 0) dy = 0;
    if (dx == 0 && dy == 0) return;
    RECT wr; GetWindowRect(h, &wr);
    int w = (wr.right - wr.left) + dx;
    int hh = (wr.bottom - wr.top) + dy;
    // 保持窗口中心不动（若越界则钳回工作区；RECT 成员是 LONG，统一转 int 参与
    // std::min/max 模板推导，否则 LONG/int 混用会推导失败）
    int x = wr.left - dx / 2, y = wr.top - dy / 2;
    x = std::max((int)mi.rcWork.left, std::min(x, (int)mi.rcWork.right - w));
    y = std::max((int)mi.rcWork.top,  std::min(y, (int)mi.rcWork.bottom - hh));
    // SWP_NOCOPYBITS：禁止系统把旧窗口内容位块搬进新尺寸（拖拽残影来源之一）
    SetWindowPos(h, NULL, x, y, w, hh,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
    // 立即连同非客户区（滚动条）整体重绘，不留中间态
    RedrawWindow(h, NULL, NULL, RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW);
}

// 以鼠标位置为锚点缩放（Ctrl+滚轮）：缩放前后鼠标所指的图像点保持在原地
void ZoomAt(HWND h, POINT clientPt, double factor) {
    double ns = g.s * factor;
    if (ns < kZoomMin) ns = kZoomMin;
    if (ns > kZoomMax) ns = kZoomMax;
    if (ns == g.s) return;
    POINT ip = ClientToImg(clientPt);
    g.s = ns;
    // 先让窗口跟随内容扩/缩（受屏幕 80% 上限、弹窗大小下限约束），再按新客户区做锚点对位
    FitWindowToContent(h);
    ClampScroll();
    // 让 ip 继续位于鼠标下方：scroll = ip*s - clientPt（仅当内容大于窗口时有效）
    if (ContentW() > g.clientW)
        g.scrollX = (int)(ip.x * g.s) - clientPt.x;
    if (ContentH() > g.clientH)
        g.scrollY = (int)(ip.y * g.s) - clientPt.y;
    ClampScroll();
    ApplyScrollbars(h);
    InvalidateAll(h);
}

void DoScroll(HWND h, int bar, int cmd) {
    SCROLLINFO si{ sizeof(si), SIF_ALL };
    GetScrollInfo(h, bar, &si);
    int pos = si.nPos;
    switch (cmd) {
        case SB_LINEUP:      pos -= 60; break;
        case SB_LINEDOWN:    pos += 60; break;
        case SB_PAGEUP:      pos -= (int)si.nPage; break;
        case SB_PAGEDOWN:    pos += (int)si.nPage; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        default: return;
    }
    if (bar == SB_HORZ) g.scrollX = pos; else g.scrollY = pos;
    ClampScroll();
    ApplyScrollbars(h);
    InvalidateAll(h);
}

// 把选区命中的词按“行”合并成条带：同一行（垂直重叠超过较小高度的一半）的词
// 取并集。否则 Tesseract 的紧贴词框高矮胖瘦各异、词间还有缝，视觉上很碎。
struct LineBand { int x0, y0, x1, y1; };

std::vector<LineBand> CollectLineBands(const std::vector<OcrWord>& words) {
    std::vector<const OcrWord*> picked;
    for (const OcrWord& wd : words) {
        if (wd.x + wd.w < g.selImg.left || wd.x > g.selImg.right ||
            wd.y + wd.h < g.selImg.top || wd.y > g.selImg.bottom) continue;
        picked.push_back(&wd);
    }
    std::sort(picked.begin(), picked.end(),
              [](const OcrWord* a, const OcrWord* b) { return a->y < b->y; });
    std::vector<LineBand> bands;
    for (const OcrWord* wd : picked) {
        bool merged = false;
        for (LineBand& b : bands) {
            int ovTop = std::max(b.y0, wd->y);
            int ovBot = std::min(b.y1, wd->y + wd->h);
            int minH  = std::min(b.y1 - b.y0, wd->h);
            if (ovBot - ovTop > minH / 2) {  // 同一行：并入并集
                b.x0 = std::min(b.x0, wd->x);
                b.y0 = std::min(b.y0, wd->y);
                b.x1 = std::max(b.x1, wd->x + wd->w);
                b.y1 = std::max(b.y1, wd->y + wd->h);
                merged = true;
                break;
            }
        }
        if (!merged)
            bands.push_back({ wd->x, wd->y, wd->x + wd->w, wd->y + wd->h });
    }
    return bands;
}

// 把一块矩形区域（图像像素坐标，整条行带）内的“背景类”像素替换为高亮灰，
// 文字像素保留原图——不重绘字形，因此文字不会变形。判定：亮度 >= 160 视为背景。
// 按行带整块替换后，词间空隙也被统一覆盖，色带连续、等高。
void BlitWordHighlight(HDC dst, const CaptureImage& img,
                       int bx, int by, int bw, int bh, const RECT& cr) {
    int x0 = std::max(0, bx), y0 = std::max(0, by);
    int x1 = std::min(img.width,  bx + bw);
    int y1 = std::min(img.height, by + bh);
    int w = x1 - x0, hh = y1 - y0;
    if (w <= 0 || hh <= 0) return;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -hh; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    HDC scr = GetDC(NULL);
    void* bits = nullptr;
    HBITMAP hb = CreateDIBSection(scr, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, scr);
    if (!hb || !bits) { if (hb) DeleteObject(hb); return; }
    DWORD* px = (DWORD*)bits;
    const unsigned hiR = 208, hiG = 213, hiB = 221; // #d0d5dd
    for (int y = 0; y < hh; y++) {
        const unsigned char* row = &img.rgb[((size_t)(y0 + y) * img.width + x0) * 3];
        for (int x = 0; x < w; x++) {
            unsigned r = row[x * 3 + 0], gc = row[x * 3 + 1], bc = row[x * 3 + 2];
            unsigned lum = (r * 299 + gc * 587 + bc * 114) / 1000;
            DWORD v = (lum >= 160)
                ? (0xFF000000u | (hiR << 16) | (hiG << 8) | hiB)   // 背景 -> 高亮灰
                : (0xFF000000u | (r << 16) | (gc << 8) | bc);      // 文字 -> 原样保留
            px[(size_t)y * w + x] = v;
        }
    }
    SetStretchBltMode(dst, COLORONCOLOR);
    StretchDIBits(dst, cr.left, cr.top, cr.right - cr.left, cr.bottom - cr.top,
                  0, 0, w, hh, bits, &bmi, DIB_RGB_COLORS, SRCCOPY);
    DeleteObject(hb);
}

// ---- 标注渲染（GDI+ 抗锯齿，圆头线条）----
// sc=缩放，ox/oy=图像原点在绘制表面的位置：
//   屏幕呈现：sc=g.s、ox/oy=客户区原点；写剪贴板：sc=1、ox/oy=0（图像像素坐标）。
// （CopyImageToClipboard 已由 ocraddon_internal.h 在全局作用域声明，勿在本匿名
//  命名空间内重复前向声明——两个作用域的同签名声明会造成 C2668 二义性。）
void DrawAnnoAt(gp::Graphics& gr, const Anno& a, double sc, float ox, float oy) {
    if (a.tool == AT_NONE || a.tool == AT_TEXT) return;
    Color col(255, kAnnoColors[a.color][0], kAnnoColors[a.color][1],
              kAnnoColors[a.color][2]);
    float pw = (float)std::max(1.0, kAnnoWidths[a.widthIdx] * sc);
    gp::Pen pen(col, pw);
    pen.SetStartCap(gp::LineCapRound);      // 圆头：线条端点/拐点圆润光滑
    pen.SetEndCap(gp::LineCapRound);
    pen.SetLineJoin(gp::LineJoinRound);
    auto M = [&](const POINT& p) {
        return gp::PointF(ox + (float)(p.x * sc), oy + (float)(p.y * sc));
    };
    if (a.tool == AT_RECT) {
        PointF p1 = M({ std::min(a.a.x, a.b.x), std::min(a.a.y, a.b.y) });
        PointF p2 = M({ std::max(a.a.x, a.b.x), std::max(a.a.y, a.b.y) });
        gr.DrawRectangle(&pen, gp::RectF(p1.X, p1.Y, p2.X - p1.X, p2.Y - p1.Y));
    } else if (a.tool == AT_ELLIPSE) {
        PointF p1 = M({ std::min(a.a.x, a.b.x), std::min(a.a.y, a.b.y) });
        PointF p2 = M({ std::max(a.a.x, a.b.x), std::max(a.a.y, a.b.y) });
        gr.DrawEllipse(&pen, gp::RectF(p1.X, p1.Y, p2.X - p1.X, p2.Y - p1.Y));
    } else if (a.tool == AT_ARROW) {
        PointF A = M(a.a), B = M(a.b);
        gr.DrawLine(&pen, A, B);
        // 箭头头部：从 B 反向转约 ±26° 的两条圆头短线
        double ang = atan2((double)(B.Y - A.Y), (double)(B.X - A.X));
        float L = std::max(10.0f, pw * 4);
        for (int k = -1; k <= 1; k += 2) {
            double a2 = ang + 3.14159265358979 + k * 0.45;
            gr.DrawLine(&pen, B, PointF(B.X + (REAL)(L * cos(a2)),
                                        B.Y + (REAL)(L * sin(a2))));
        }
    } else if (a.tool == AT_PEN && a.pts.size() >= 2) {
        std::vector<gp::PointF> pts;
        pts.reserve(a.pts.size());
        for (const POINT& p : a.pts) pts.push_back(M(p));
        gr.DrawLines(&pen, pts.data(), (INT)pts.size());
    }
}

// 画全部标注（含进行中的一笔），屏幕客户区坐标
void DrawAnnotations(gp::Graphics& gr) {
    int ox, oy; ComputeOrigin(ox, oy);
    for (const Anno& a : g.annos) DrawAnnoAt(gr, a, g.s, (float)ox, (float)oy);
    if (g.drawing) DrawAnnoAt(gr, g.cur, g.s, (float)ox, (float)oy);
}

// ---- 工具栏：独立小工具窗（微信风格），不再画在结果窗口客户区里。
// 默认位置：结果窗口右下角 ↔ 工具栏右上角对齐，紧贴窗口正下方（缝 8px）；
// 屏幕下方放不下（如满屏截图）则叠在窗口内右下角——只有这种情况下
// 工具栏才压在图片上。手动拖过后位置自由（始终钳制在屏幕工作区内）。
// 一行分三组：[3 形状 + 笔 + 文字] ┃ [6 颜色] ┃ [4 粗细]；
// 组间画淡灰分界线。交互：短按选功能；长按 0.3s 拖动；
// 按住空白处立即拖动。
constexpr int kTbCell = 44;   // 每项格子边长
constexpr int kTbPad  = 14;   // 条内左右边距
constexpr int kTbGap  = 24;   // 分组间隔（形状|颜色|粗细 之间）
constexpr int kTbH    = 72;   // 条高
constexpr int kTbItems = 16;  // 语义项数：0-3 粗细，4-9 颜色，10-13 工具，14 抓取文字，15 下载

// 视觉顺序 -> 语义索引。
// 用户要求的新排列：形状(矩形/椭圆/箭头) → 笔 → 文字 → 下载 丨 颜色 丨 粗细。
// 「下载」紧跟在「文字」后面、两者之间不画分界线（用户明确要求）。
// 好处是语义索引（0-3 粗细 / 4-9 颜色 / 10-15 工具与动作）保持不变，
// 选中、命中、绘制逻辑都不用改，只在这一层做映射。
static const int kTbOrder[kTbItems] = {
    10, 11, 12, 13, 14, 15,   // 形状 3 个 + 笔 + 文字 + 下载
     4,  5,  6,  7,  8, 9,    // 颜色 6 个
     0,  1,  2,  3            // 粗细 4 个
};

int TbWidth() { return kTbPad * 2 + kTbItems * kTbCell + 2 * kTbGap; }

// 独立工具窗状态（不放进 VState：VState 每次截图重置，窗口句柄不能丢）
HWND   g_tb = nullptr;         // 工具栏窗口（owner = 结果窗口）
bool   g_tbUserMoved = false;  // 用户手动拖过 → 不再自动跟随主窗口
bool   g_tbDrag = false;       // 正在拖动工具栏
bool   g_tbArmed = false;      // 长按 0.3s 定时器已触发（转入拖动）
bool   g_tbPress = false;      // 已按下等待判定（短按选择 or 长按拖动）
int    g_tbPressIdx = -1;      // 按下的项（-1 = 空白处）
int    g_tbHover = -1;         // 悬停项（-1 无）
bool   g_tbHoverTrack = false; // 已开启 WM_MOUSELEAVE 跟踪
POINT  g_tbDragPt{ 0, 0 };     // 拖动起点（屏幕坐标）
POINT  g_tbDragPos{ 0, 0 };    // 拖动起点时的窗口位置

RECT TbItemRect(const RECT& tb, int i) {
    // 分组：[0..5] 形状/笔/文字/下载  [6..11] 颜色  [12..15] 粗细
    int gapsBefore = (i >= 12) ? 2 : (i >= 6) ? 1 : 0;
    int x = tb.left + kTbPad + i * kTbCell + gapsBefore * kTbGap;
    int y = tb.top + (kTbH - kTbCell) / 2;
    return { x, y, x + kTbCell, y + kTbCell };
}
int TbHitTest(const RECT& tb, POINT pt) {
    // 返回语义索引（调用方按语义判断粗细/颜色/工具），因此这里要反查映射表。
    for (int i = 0; i < kTbItems; i++) {
        RECT r = TbItemRect(tb, i);
        if (pt.x >= r.left - 2 && pt.x < r.right + 2 &&
            pt.y >= r.top - 2 && pt.y < r.bottom + 2) return kTbOrder[i];
    }
    return -1;
}
// ---- 工具栏「下载」：把当前图（含已画标注）另存为文件 ----
static int TbEncoderClsid(const wchar_t* mime, CLSID* clsid) {
    UINT num = 0, bytes = 0;
    if (GetImageEncodersSize(&num, &bytes) != Ok || bytes == 0) return -1;
    std::vector<BYTE> buf(bytes);
    auto* info = reinterpret_cast<ImageCodecInfo*>(buf.data());
    if (GetImageEncoders(num, bytes, info) != Ok) return -1;
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(info[i].MimeType, mime) == 0) { *clsid = info[i].Clsid; return (int)i; }
    }
    return -1;
}
// 「上次保存到哪个目录」记在 data\lastsave.txt（与 settings.ini 同目录）。
// 这样下次保存默认就在上次的位置，不用每次重新找目录。
static std::wstring TbLastSaveFile() {
    std::wstring base;
    std::wstring ini = ShotIniPath();
    if (!ini.empty()) {
        size_t p = ini.find_last_of(L'\\');
        base = (p == std::wstring::npos) ? std::wstring() : ini.substr(0, p);
    }
    if (base.empty()) {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring s = exe;
        size_t p = s.find_last_of(L'\\');
        base = (p == std::wstring::npos) ? std::wstring(L".") : s.substr(0, p);
    }
    return base + L"\\lastsave.txt";
}
static std::wstring TbLastSaveDir() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, TbLastSaveFile().c_str(), L"rb") != 0 || !f) return {};
    wchar_t buf[512] = {};
    size_t n = fread(buf, sizeof(wchar_t), 511, f);
    fclose(f);
    if (!n) return {};
    std::wstring s(buf, n);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
    if (s.empty()) return {};
    DWORD at = GetFileAttributesW(s.c_str());
    if (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY)) return {};
    return s;
}
static void TbStoreLastDir(const std::wstring& filePath) {
    size_t p = filePath.find_last_of(L'\\');
    if (p == std::wstring::npos || p == 0) return;
    std::wstring dir = filePath.substr(0, p);
    FILE* f = nullptr;
    if (_wfopen_s(&f, TbLastSaveFile().c_str(), L"wb") != 0 || !f) return;
    fwrite(dir.c_str(), sizeof(wchar_t), dir.size(), f);
    fclose(f);
}
static std::wstring TbDefaultSavePath() {
    std::wstring dir = TbLastSaveDir();
    if (dir.empty()) {
        wchar_t desk[MAX_PATH] = {};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desk)) ||
            !desk[0])
            GetCurrentDirectoryW(MAX_PATH, desk);
        dir = desk;
    }
    SYSTEMTIME st{}; GetLocalTime(&st);
    wchar_t name[64];
    swprintf(name, 64, L"截图_%04d%02d%02d_%02d%02d%02d.png",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return dir + L"\\" + name;
}
void TbSaveToFile(HWND owner) {
    const int W = g.img.width, H = g.img.height;
    if (W <= 0 || H <= 0 || g.img.rgb.empty()) return;
    std::wstring def = TbDefaultSavePath();
    std::vector<wchar_t> buf(MAX_PATH, 0);
    size_t cp = def.size() < MAX_PATH - 1 ? def.size() : MAX_PATH - 1;
    memcpy(buf.data(), def.c_str(), cp * sizeof(wchar_t));

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.hwnd ? g.hwnd : owner;
    ofn.lpstrFilter = L"PNG 图片 (*.png)\0*.png\0JPEG 图片 (*.jpg)\0*.jpg\0所有文件 (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"png";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return;      // 用户取消

    std::wstring path(buf.data());
    std::wstring ext;
    size_t dot = path.find_last_of(L'.');
    if (dot != std::wstring::npos) ext = path.substr(dot + 1);
    for (auto& c : ext) c = (wchar_t)towlower(c);
    const bool jpg = (ext == L"jpg" || ext == L"jpeg");
    CLSID clsid{};
    if (TbEncoderClsid(jpg ? L"image/jpeg" : L"image/png", &clsid) < 0) {
        MessageBoxW(ofn.hwndOwner, L"系统缺少对应的图片编码器，无法保存。", L"下载",
                    MB_ICONWARNING);
        return;
    }
    // 复制到自有位图后再画标注：绝不能在 g.img.rgb 上画，否则标注会“烙”进原图
    gp::Bitmap bmp(W, H, PixelFormat24bppRGB);
    if (bmp.GetLastStatus() != Ok) return;
    {
        gp::Rect rc(0, 0, W, H);
        gp::BitmapData bd{};
        if (bmp.LockBits(&rc, gp::ImageLockModeWrite, PixelFormat24bppRGB, &bd) != Ok) return;
        for (int y = 0; y < H; ++y)
            memcpy((BYTE*)bd.Scan0 + (size_t)y * bd.Stride,
                   &g.img.rgb[(size_t)y * W * 3], (size_t)W * 3);
        bmp.UnlockBits(&bd);
        if (!g.annos.empty()) {
            gp::Graphics gr(&bmp);
            gr.SetSmoothingMode(gp::SmoothingModeAntiAlias);
            for (const Anno& a : g.annos) DrawAnnoAt(gr, a, 1.0, 0.f, 0.f);
        }
    }
    if (bmp.Save(path.c_str(), &clsid, nullptr) != Ok) {
        MessageBoxW(ofn.hwndOwner, L"保存失败（路径只读、磁盘空间不足或被占用）。",
                    L"下载", MB_ICONWARNING);
        return;
    }
    // 成功不弹提示（用户要求：点保存就直接确认）；只记住这次选的目录，下次默认还在这里。
    TbStoreLastDir(path);
}

// 按下工具栏某项：短按（松开）=选择；长按 0.3 秒=转为拖拽整条工具栏
void TbApplySelection(int idx) {
    if (idx < 0) return;
    if (idx < 4)       g.annWidth = idx;
    else if (idx < 10) g.annColor = idx - 4;
    else if (idx < 14) g.tool = idx - 9;    // 矩形/椭圆/箭头/笔
    else if (idx == 14) g.tool = AT_TEXT;   // 抓取文字
    else                TbSaveToFile(g_tb); // 15 = 下载（动作按钮，不改变当前工具）
}
RECT TbWorkArea(HWND ref) {
    HMONITOR hm = MonitorFromWindow(ref, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    if (GetMonitorInfo(hm, &mi)) return mi.rcWork;
    return { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
}

// 圆角矩形路径（条底 / 选中底共用）
void TbRoundPath(gp::GraphicsPath& p, const gp::RectF& r, float rad) {
    float d = rad * 2;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}

// 画工具栏整条（GDI+ 抗锯齿；白底圆角条 + 圆头线形图标，选中/悬停浅灰圆角底）。
// 画在独立工具窗的客户区内：条底铺满整个窗口（圆角由窗口 Rgn 裁出）。
void TbPaintBar(gp::Graphics& gr, int cw, int ch) {
    RECT tb{ 0, 0, cw, ch };
    gp::RectF bar(0.0f, 0.0f, (float)cw, (float)ch);
    // 条底：白底圆角 + 浅灰描边
    {
        gp::GraphicsPath path;
        TbRoundPath(path, bar, 16);
        gp::SolidBrush white(Color(255, 255, 255, 255));
        gr.FillPath(&white, &path);
        gp::Pen border(Color(255, 224, 227, 231), 1.5f);
        gr.DrawPath(&border, &path);
    }
    // 分组分界线（淡灰竖线，微信风格）：形状/笔/文字/下载 ┃ 颜色 ┃ 粗细
    // 「文字」与「下载」之间不画线（用户要求），所以线在位置 5（下载）与 11（最后一个颜色）之后各一条。
    {
        gp::Pen dv(Color(255, 226, 229, 233), 1.5f);
        for (int gi : {5, 11}) {
            RECT a = TbItemRect(tb, gi), b = TbItemRect(tb, gi + 1);
            float gx = (a.right + b.left) / 2.0f;
            gr.DrawLine(&dv, PointF(gx, tb.top + 16), PointF(gx, tb.bottom - 16));
        }
    }
    for (int i = 0; i < kTbItems; i++) {
        const int sem = kTbOrder[i];      // 语义索引（决定画什么、是否选中）
        RECT r = TbItemRect(tb, i);
        gp::RectF ir((float)r.left, (float)r.top,
                     (float)(r.right - r.left), (float)(r.bottom - r.top));
        bool sel = (sem < 4)  ? (g.annWidth == sem)
                 : (sem < 10) ? (g.annColor == sem - 4)
                 : (sem < 14) ? (g.tool == sem - 9)
                 : (sem == 14) ? (g.tool == AT_TEXT)   // 「抓取文字」=框选模式
                 :               false;                // 15「下载」是动作按钮，没有选中态
        bool hov = (g_tbHover == sem);
        if (sel || hov) {   // 选中/悬停：浅灰圆角方块底
            // 底色比格子内缩 3px：相邻两项（如「文字」和「下载」）不会粘成一整块长方形，
            // 每个都是一眼可辨的独立方块（用户反馈的"下载框怎么是长方形"就是这个粘连）。
            gp::RectF hb(ir.X + 3, ir.Y + 3, ir.Width - 6, ir.Height - 6);
            gp::GraphicsPath path;
            TbRoundPath(path, hb, 9);
            gp::SolidBrush b(sel ? Color(255, 229, 232, 236)
                                 : Color(255, 243, 244, 246));
            gr.FillPath(&b, &path);
        }
        float cx = ir.X + ir.Width / 2, cy = ir.Y + ir.Height / 2;
        Color ink(255, 62, 66, 72);
        if (sem < 4) {                       // 粗细：由小到大的圆点
            float d = 8 + sem * 4;
            gp::SolidBrush b(ink);
            gr.FillEllipse(&b, cx - d / 2, cy - d / 2, d, d);
        } else if (sem < 10) {               // 颜色：色点（白点加浅灰描边才可见）
            int ci = sem - 4;
            gp::SolidBrush b(Color(255, kAnnoColors[ci][0], kAnnoColors[ci][1],
                                   kAnnoColors[ci][2]));
            gr.FillEllipse(&b, cx - 10.0f, cy - 10.0f, 20.0f, 20.0f);
            if (ci == 5) {
                gp::Pen po(Color(255, 205, 208, 212), 1.5f);
                gr.DrawEllipse(&po, cx - 10.0f, cy - 10.0f, 20.0f, 20.0f);
            }
        } else {                           // 工具：圆头线形图标（3px 深灰）
            gp::Pen pen(ink, 3.0f);
            pen.SetStartCap(gp::LineCapRound);
            pen.SetEndCap(gp::LineCapRound);
            pen.SetLineJoin(gp::LineJoinRound);
            int t = (sem < 14) ? (sem - 9) : AT_TEXT;
            if (sem == 15) {
                // 「下载」：向下的箭头 + 下方托盘。托盘是个方括号形状，
                // 两个下角用圆弧过渡（用户明确要求），笔帽圆头所以两端也圆。
                PointF tip(cx, ir.Y + 26);
                gr.DrawLine(&pen, PointF(cx, ir.Y + 9), tip);
                gr.DrawLine(&pen, PointF(cx - 6.5f, ir.Y + 19.5f), tip);
                gr.DrawLine(&pen, PointF(cx + 6.5f, ir.Y + 19.5f), tip);
                const float bx0 = cx - 9.5f, bx1 = cx + 9.5f;
                const float bTop = ir.Y + 28.0f, bBot = ir.Y + 34.5f, rr = 4.0f;
                gp::GraphicsPath tray;
                tray.AddLine(bx0, bTop, bx0, bBot - rr);
                tray.AddArc(bx0, bBot - 2 * rr, 2 * rr, 2 * rr, 180, -90);
                tray.AddLine(bx0 + rr, bBot, bx1 - rr, bBot);
                tray.AddArc(bx1 - 2 * rr, bBot - 2 * rr, 2 * rr, 2 * rr, 90, -90);
                tray.AddLine(bx1, bBot - rr, bx1, bTop);
                gr.DrawPath(&pen, &tray);
            } else switch (t) {
                case AT_RECT: {
                    // 方形图标：四角倒小圆弧（用户要求）
                    gp::GraphicsPath rp;
                    TbRoundPath(rp, gp::RectF(ir.X + 11, ir.Y + 14, 22, 16), 4);
                    gr.DrawPath(&pen, &rp);
                    break;
                }
                case AT_ELLIPSE:
                    gr.DrawEllipse(&pen, gp::RectF(ir.X + 11, ir.Y + 14, 22, 16));
                    break;
                case AT_ARROW: {
                    PointF A(ir.X + 11, ir.Y + ir.Height - 11);
                    PointF B(ir.X + ir.Width - 11, ir.Y + 11);
                    gr.DrawLine(&pen, A, B);
                    double ang = atan2((double)(B.Y - A.Y), (double)(B.X - A.X));
                    float L = 9;
                    for (int k = -1; k <= 1; k += 2) {
                        double a2 = ang + 3.14159265358979 + k * 0.5;
                        gr.DrawLine(&pen, B, PointF(B.X + (REAL)(L * cos(a2)),
                                                    B.Y + (REAL)(L * sin(a2))));
                    }
                    break;
                }
                case AT_PEN: {
                    // 微信铅笔：细长笔杆（斜 45°）+ 圆弧尾端 + 实心笔尖
                    PointF T(ir.X + 12, ir.Y + 33);          // 笔尖顶点（左下）
                    PointF E(ir.X + 33, ir.Y + 11);          // 尾端中心（右上）
                    float dx = E.X - T.X, dy = E.Y - T.Y;
                    float len = sqrtf(dx * dx + dy * dy);
                    float ux = dx / len, uy = dy / len;      // 轴向单位向量
                    float px = -uy, py = ux;                 // 垂直向单位向量
                    float w = 4, tip = 9;                    // 笔杆更细、整体更长
                    PointF B1(T.X + ux * tip + px * w, T.Y + uy * tip + py * w);
                    PointF B2(T.X + ux * tip - px * w, T.Y + uy * tip - py * w);
                    PointF A1(E.X + px * w, E.Y + py * w);
                    PointF A2(E.X - px * w, E.Y - py * w);
                    // 笔杆轮廓：A1 --(绕尾端半圆弧，倒圆角)-- A2 -- B2 -- B1
                    // （B1-B2 一侧开口，由实心三角笔尖盖住）
                    gp::GraphicsPath body;
                    float a0 = atan2(py, px) * 180.0f / 3.14159265f;
                    body.AddArc(gp::RectF(E.X - w, E.Y - w, 2 * w, 2 * w),
                                a0, -180.0f);
                    body.AddLine(A2.X, A2.Y, B2.X, B2.Y);
                    body.AddLine(B2.X, B2.Y, B1.X, B1.Y);
                    body.CloseFigure();
                    gr.DrawPath(&pen, &body);
                    PointF tri[3] = { T, B1, B2 };
                    gp::SolidBrush tb(ink);
                    gr.FillPolygon(&tb, tri, 3);
                    // 尾部（橡皮端）分隔线
                    gr.DrawLine(&pen,
                                PointF(E.X - ux * 7 + px * w, E.Y - uy * 7 + py * w),
                                PointF(E.X - ux * 7 - px * w, E.Y - uy * 7 - py * w));
                    break;
                }
                case AT_TEXT: {
                    // 「抓取文字」：就是一个「文」字（外框已按用户要求去掉），
                    // 字号与其他工具图标一致（约 22px），MeasureString 精确居中
                    // /std:c++20 隐含 /permissive：不能再对临时对象取地址（C2102）。
                    // FontFamily 必须先于 Font 构造、晚于 Font 析构，故一并提为 static。
                    static gp::FontFamily ffText(L"Microsoft YaHei");
                    static gp::Font fText(&ffText,
                                          22, gp::FontStyleRegular, gp::UnitPixel);
                    gp::SolidBrush tb(ink);
                    gp::StringFormat sf;
                    sf.SetAlignment(gp::StringAlignmentCenter);
                    sf.SetLineAlignment(gp::StringAlignmentCenter);
                    gp::RectF meas;
                    gr.MeasureString(L"文", 1, &fText, gp::PointF(0.f, 0.f), &sf, &meas);
                    float fx = cx - meas.Width / 2 - meas.X;
                    float fy = cy - meas.Height / 2 - meas.Y;
                    gr.DrawString(L"文", -1, &fText, gp::PointF(fx, fy), &sf, &tb);
                    break;
                }
            }
        }
    }
}

// ---- 独立工具窗：定位 / 拖动 / 窗口过程 / 创建 ----

// 未被用户手动拖过时跟随主窗口：工具栏右上角 ↔ 主窗口右下角对齐，
// 紧贴窗口正下方（缝 8px）；屏幕下方放不下（如满屏截图）则叠在窗口
// 内右下角——只有这种情况下工具栏才压在图片上。始终不伸出屏幕。
void TbReposition() {
    if (!g_tb || !g.hwnd) return;
    RECT wr; GetWindowRect(g.hwnd, &wr);
    RECT wa = TbWorkArea(g.hwnd);
    int tw = TbWidth();
    int x = wr.right - tw;   // 右对齐：工具栏右上角 = 窗口右下角
    int y = wr.bottom + 8;   // 紧贴窗口正下方
    if (x < wa.left + 8) x = wa.left + 8;
    if (x + tw > wa.right - 8) x = wa.right - 8 - tw;
    if (y + kTbH > wa.bottom - 8) y = wr.bottom - 8 - kTbH;  // 下方放不下→窗口内右下
    if (y < wa.top + 8) y = wa.top + 8;
    SetWindowPos(g_tb, NULL, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}
// 拖动中：把工具栏窗口钳在屏幕工作区内（边距 8px），绝不伸出屏幕
void TbClampToScreen() {
    if (!g_tb) return;
    RECT wa = TbWorkArea(g_tb);
    RECT wr; GetWindowRect(g_tb, &wr);
    int w = wr.right - wr.left, hh = wr.bottom - wr.top;
    int x = std::max(wa.left + 8, std::min(wr.left, wa.right - 8 - w));
    int y = std::max(wa.top + 8, std::min(wr.top, wa.bottom - 8 - hh));
    if (x != wr.left || y != wr.top)
        SetWindowPos(g_tb, NULL, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}
void TbDragFromCurrent() {   // 以当前位置进入拖动
    GetCursorPos(&g_tbDragPt);
    RECT wr; GetWindowRect(g_tb, &wr);
    g_tbDragPos = { wr.left, wr.top };
    SetCursor(LoadCursor(NULL, IDC_SIZEALL));
}

LRESULT CALLBACK TbProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc; GetClientRect(h, &rc);
            // 双缓冲：先画到内存位图再贴出（避免圆角边缘闪烁）
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP mb = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HGDIOBJ ob = SelectObject(mem, mb);
            {
                gp::Graphics gr(mem);
                gr.SetSmoothingMode(gp::SmoothingModeAntiAlias);
                TbPaintBar(gr, rc.right, rc.bottom);
            }
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, ob);
            DeleteObject(mb);
            DeleteDC(mem);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            RECT tb; GetClientRect(h, &tb);
            POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            int idx = TbHitTest(tb, pt);
            g_tbPress = true;
            g_tbPressIdx = idx;
            g_tbArmed = false;
            SetCapture(h);
            SetTimer(h, 2, 300, NULL);   // 长按 0.3s 判定
            if (idx < 0) {               // 空白处：立即拖动
                g_tbArmed = true;
                g_tbDrag = true;
                TbDragFromCurrent();
            }
            return 0;
        }
        case WM_TIMER:
            if (w == 2) {
                KillTimer(h, 2);
                if (g_tbPress && !g_tbArmed) {   // 长按超 0.3s：转入拖动
                    g_tbArmed = true;
                    g_tbDrag = true;
                    TbDragFromCurrent();
                }
            }
            return 0;
        case WM_MOUSEMOVE: {
            if (g_tbDrag) {
                POINT sp; GetCursorPos(&sp);
                SetWindowPos(h, NULL,
                             g_tbDragPos.x + sp.x - g_tbDragPt.x,
                             g_tbDragPos.y + sp.y - g_tbDragPt.y,
                             0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                TbClampToScreen();
                return 0;
            }
            RECT tb; GetClientRect(h, &tb);
            POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            int hov = TbHitTest(tb, pt);
            if (hov != g_tbHover) {
                g_tbHover = hov;
                InvalidateRect(h, NULL, FALSE);
            }
            if (!g_tbHoverTrack) {
                TRACKMOUSEEVENT te{ sizeof(te), TME_LEAVE, h, 0 };
                TrackMouseEvent(&te);
                g_tbHoverTrack = true;
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g_tbHoverTrack = false;
            if (g_tbHover != -1) {
                g_tbHover = -1;
                InvalidateRect(h, NULL, FALSE);
            }
            return 0;
        case WM_LBUTTONUP: {
            // 先保存状态：ReleaseCapture() 会同步发送 WM_CAPTURECHANGED，
            // 如果先 ReleaseCapture，g_tbPress/g_tbDrag 会被重置为 false，
            // 导致短按/拖动的后续逻辑无法识别（表现为“能拖动但点击没反应”）。
            bool wasPress = g_tbPress && !g_tbArmed;
            bool wasDrag = g_tbDrag;
            int pressIdx = g_tbPressIdx;
            KillTimer(h, 2);
            ReleaseCapture();
            g_tbPress = false;
            g_tbArmed = false;
            g_tbDrag = false;
            if (wasDrag) {
                g_tbUserMoved = true;   // 手动拖过：不再自动跟随主窗口
            } else if (wasPress) {      // 短按：此刻才选择工具
                TbApplySelection(pressIdx);
                InvalidateRect(h, NULL, FALSE);
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            g_tbPress = false;
            g_tbArmed = false;
            g_tbDrag = false;
            KillTimer(h, 2);
            return 0;
        case WM_MOUSEACTIVATE:
            // 点击工具栏不抢主窗口焦点（ESC/Ctrl+Z 始终有效），但仍正常
            // 接收鼠标消息——这是“可点击但不激活”的标准做法，比
            // WS_EX_NOACTIVATE 在「附属窗 + 前台 owner」组合下可靠得多。
            return MA_NOACTIVATE;
        case WM_DESTROY:
            g_tb = nullptr;
            return 0;
    }
    return DefWindowProc(h, m, w, l);
}

// 创建独立工具窗（owner = 结果窗口，结果窗口销毁时随之销毁）。
// 不设 WS_EX_TOPMOST（见下方 CreateWindowEx 注释）：避免“顶层+被拥有+不激活”
// 组合吞掉点击；工具栏靠“被拥有的弹出窗”天然浮在结果窗之上即可。
// 不抢主窗口焦点（靠 WM_MOUSEACTIVATE 返回 MA_NOACTIVATE 实现）。
void TbCreate() {
    if (g_tb) { DestroyWindow(g_tb); g_tb = nullptr; }
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = TbProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;
        wc.lpszClassName = L"OCRTb";
        RegisterClassW(&wc);
        reg = true;
    }
    int tw = TbWidth();
    // 关键修复（第十一轮）：去掉 WS_EX_TOPMOST。
    // 原因：WS_EX_TOPMOST + 被前台 owner 拥有的弹出窗 + “不激活”机制三件套，
    // 会让 Windows 在点击时把消息的“归属/激活”判定搞混，从而把这次点击吞掉
    // （工具栏看得见却整排点不动）——这正是前两轮（R8/R9）的顽疾。
    // 正确且稳定的做法（微信风格）：
    //   ① owner = 结果窗口 ⇒ 工具栏永远浮在结果窗之上、且随结果窗销毁；
    //   ② WS_EX_TOOLWINDOW ⇒ 不在任务栏占位；
    //   ③ WM_MOUSEACTIVATE 返回 MA_NOACTIVATE ⇒ 点击不抢焦点（ESC/Ctrl+Z 始终有效），
    //      但点击消息仍正常投递到 TbProc（这是“可点但不激活”的标准做法，
    //      比 WS_EX_NOACTIVATE *样式* 在“附属窗 + 前台 owner”组合下可靠得多）。
    // 不设 WS_EX_TOPMOST：工具栏只需浮在“自己的”结果窗口之上即可，不必盖住所有程序，
    // 这与用户“像微信一样、别伸出屏幕”的诉求一致。
    g_tb = CreateWindowExW(WS_EX_TOOLWINDOW, L"OCRTb", L"",
                           WS_POPUP, 0, 0, tw, kTbH,
                           g.hwnd, NULL, GetModuleHandle(NULL), NULL);
    if (!g_tb) return;
    // 圆角窗口区域：条底圆角半径 16 的外轮廓，四角透明
    HRGN rgn = CreateRoundRectRgn(0, 0, tw + 1, kTbH + 1, 32, 32);
    SetWindowRgn(g_tb, rgn, FALSE);
    g_tbUserMoved = false;
    g_tbHover = -1;
    g_tbHoverTrack = false;
    g_tbDrag = false;
    g_tbArmed = false;
    g_tbPress = false;
    TbReposition();
    ShowWindow(g_tb, SW_SHOWNOACTIVATE);
}

// 把「原图 + 已完成标注」合成为最终图并写入剪贴板（CF_DIB）。
// 复制时机（用户定案）：窗口退出时统一调用——有标注复制最终标注图，
// 无标注复制原图；复制文字产生的文字内容会被本复制覆盖（Win+V 可找回）。
void CopyAnnotatedToClipboard(HWND h) {
    if (g.annos.empty()) { CopyImageToClipboard(g.img); return; }
    int W = g.img.width, H = g.img.height;
    int rowBytes = ((W * 3 + 3) / 4) * 4;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = W;
    bmi.bmiHeader.biHeight = H;   // bottom-up
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 24;
    bmi.bmiHeader.biCompression = BI_RGB;
    bmi.bmiHeader.biSizeImage = (DWORD)rowBytes * H;
    HDC scr = GetDC(NULL);
    void* bits = nullptr;
    HBITMAP hb = CreateDIBSection(scr, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, scr);
    if (!hb || !bits) { if (hb) DeleteObject(hb); CopyImageToClipboard(g.img); return; }
    // 拷入原图（RGB top-down -> BGR bottom-up），再按图像像素坐标画标注
    for (int y = 0; y < H; y++) {
        BYTE* dstRow = (BYTE*)bits + (size_t)y * rowBytes;
        const unsigned char* srcRow = &g.img.rgb[(size_t)(H - 1 - y) * W * 3];
        for (int x = 0; x < W; x++) {
            dstRow[x * 3 + 0] = srcRow[x * 3 + 2];
            dstRow[x * 3 + 1] = srcRow[x * 3 + 1];
            dstRow[x * 3 + 2] = srcRow[x * 3 + 0];
        }
    }
    HDC mem = CreateCompatibleDC(NULL);
    HGDIOBJ old = SelectObject(mem, hb);
    {
        gp::Graphics gr(mem);
        gr.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        for (const Anno& a : g.annos) DrawAnnoAt(gr, a, 1.0, 0.f, 0.f);
    }
    SelectObject(mem, old);
    DeleteDC(mem);
    // 读回 top-down RGB
    CaptureImage out;
    out.width = W; out.height = H;
    out.rgb.resize((size_t)W * H * 3);
    for (int y = 0; y < H; y++) {
        const BYTE* srcRow = (const BYTE*)bits + (size_t)(H - 1 - y) * rowBytes;
        unsigned char* dstRow = &out.rgb[(size_t)y * W * 3];
        for (int x = 0; x < W; x++) {
            dstRow[x * 3 + 0] = srcRow[x * 3 + 2];
            dstRow[x * 3 + 1] = srcRow[x * 3 + 1];
            dstRow[x * 3 + 2] = srcRow[x * 3 + 0];
        }
    }
    DeleteObject(hb);
    CopyImageToClipboard(out);
}

LRESULT CALLBACK VWnd(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_ERASEBKGND:
            return 1; // 背景由 WM_PAINT 双缓冲统一绘制，消除闪烁

        case WM_NCHITTEST: {
            // 边缘 6px 按可调大小边框处理：像面板一样拖拽外框改变窗口大小。
            // WS_POPUP 无系统边框，这里手动给出命中结果，鼠标形状也随之变化。
            POINT sp = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };   // 屏幕坐标
            RECT wr; GetWindowRect(h, &wr);
            const int B = 6;
            bool atL = sp.x < wr.left + B, atR = sp.x >= wr.right - B;
            bool atT = sp.y < wr.top + B,  atB = sp.y >= wr.bottom - B;
            if (atL && atT) return HTTOPLEFT;
            if (atR && atT) return HTTOPRIGHT;
            if (atL && atB) return HTBOTTOMLEFT;
            if (atR && atB) return HTBOTTOMRIGHT;
            if (atL) return HTLEFT;
            if (atR) return HTRIGHT;
            if (atT) return HTTOP;
            if (atB) return HTBOTTOM;
            break;   // 其余交给 DefWindowProc（HTCLIENT）
        }

        case WM_NCLBUTTONDOWN: {
            // 点击"边框"区：进入自绘调整大小流程。不用 SC_SIZE 模态循环——
            // 它对无边框 WS_POPUP 不可靠（HTTOPLEFT=13..HTBOTTOMRIGHT=17 超出
            // SC_SIZE 低 4 位编码，角部会失效甚至被当成移动）。改为 SetCapture
            // 手动跟踪鼠标，窗口矩形实时跟随；光标样式已由 WM_NCHITTEST 提供。
            if (w >= HTLEFT && w <= HTBOTTOMRIGHT) {
                g.resizing = true;
                g.rsHT = (int)w;
                g.userResized = true;   // 手动调整过：此后缩放不再改变窗口尺寸
                GetCursorPos(&g.rsStartPt);
                GetWindowRect(h, &g.rsStartRect);
                SetCapture(h);
                return 0;
            }
            break;
        }

        case WM_GETMINMAXINFO: {
            // 拖拽缩放的最小尺寸，防止把窗口拖没了
            MINMAXINFO* mmi = (MINMAXINFO*)l;
            mmi->ptMinTrackSize = { 200, 120 };
            return 0;
        }

        case WM_MOVE:               // 主窗口移动：工具栏（未手动拖过）跟随
            if (g_tb && !g_tbUserMoved) TbReposition();
            break;

        case WM_SIZE: {
            g.clientW = LOWORD(l); g.clientH = HIWORD(l);
            ClampScroll();
            ApplyScrollbars(h);
            InvalidateAll(h);
            if (g_tb && !g_tbUserMoved) TbReposition();
            break;
        }

        case WM_LBUTTONDOWN: {
            POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            if (GetKeyState(VK_MENU) & 0x8000) {
                // Alt + 左键：像拖动标题栏一样移动整个结果窗口
                // （窗口无标题栏，平时没法挪动；HTCAPTION 让 Windows 按
                //  鼠标拖动来处理窗口移动，与边框窗口的标题栏拖动等价）
                PostMessage(h, WM_NCLBUTTONDOWN, HTCAPTION, l);
                break;
            }
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                // Ctrl + 左键：平移图片（仅当图片比窗口大才有效）
                g.panning = true;
                g.panStart = pt;
                g.scrollStartX = g.scrollX;
                g.scrollStartY = g.scrollY;
            } else if (g.tool != AT_TEXT) {
                // 标注工具激活：左键拖动绘制（不进入文字拖选）
                g.drawing = true;
                g.cur = Anno{};
                g.cur.tool = g.tool;
                g.cur.color = g.annColor;
                g.cur.widthIdx = g.annWidth;
                g.cur.a = g.cur.b = ClientToImg(pt);
                g.cur.pts.clear();
                g.cur.pts.push_back(g.cur.a);
                g.selImg = {0, 0, 0, 0};
                g.hasSel = false;
                g.toast = false;
            } else {
                g.drag = true;
                g.dragStart = pt;
                g.selImg = {0, 0, 0, 0};
                g.hasSel = false;
                g.toast = false;
            }
            SetCapture(h);
            InvalidateAll(h);
            break;
        }

        case WM_MOUSEMOVE: {
            // ---- 边缘拖拽调整窗口大小 ----
            if (g.resizing) {
                if (GetCapture() != h) { g.resizing = false; break; }
                POINT sp; GetCursorPos(&sp);
                int dx = sp.x - g.rsStartPt.x, dy = sp.y - g.rsStartPt.y;
                RECT r = g.rsStartRect;
                const int minW = 200, minH = 120;   // 与 WM_GETMINMAXINFO 一致
                switch (g.rsHT) {
                    case HTLEFT: case HTTOPLEFT: case HTBOTTOMLEFT:
                        r.left += dx;
                        if (r.right - r.left < minW) r.left = r.right - minW;
                        break;
                    default: break;
                }
                switch (g.rsHT) {
                    case HTRIGHT: case HTTOPRIGHT: case HTBOTTOMRIGHT:
                        r.right += dx;
                        if (r.right - r.left < minW) r.right = r.left + minW;
                        break;
                    default: break;
                }
                switch (g.rsHT) {
                    case HTTOP: case HTTOPLEFT: case HTTOPRIGHT:
                        r.top += dy;
                        if (r.bottom - r.top < minH) r.top = r.bottom - minH;
                        break;
                    default: break;
                }
                switch (g.rsHT) {
                    case HTBOTTOM: case HTBOTTOMLEFT: case HTBOTTOMRIGHT:
                        r.bottom += dy;
                        if (r.bottom - r.top < minH) r.bottom = r.top + minH;
                        break;
                    default: break;
                }
                SetWindowPos(h, NULL, r.left, r.top,
                             r.right - r.left, r.bottom - r.top,
                             SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
                // 强制连非客户区（滚动条）一起重绘：拖拽残影来源之二
                RedrawWindow(h, NULL, NULL,
                             RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW);
                break;   // WM_SIZE 会同步 clientW/H、滚动条与重绘
            }
            // ---- 工具栏拖动 ----（工具栏已是独立窗口，在其自身过程中处理）
            if (g.panning) {
                int dx = GET_X_LPARAM(l) - g.panStart.x;
                int dy = GET_Y_LPARAM(l) - g.panStart.y;
                if (ContentW() > g.clientW) g.scrollX = g.scrollStartX - dx;
                if (ContentH() > g.clientH) g.scrollY = g.scrollStartY - dy;
                ClampScroll();
                ApplyScrollbars(h);
                InvalidateAll(h);
                break;
            }
            if (g.drawing) {
                // 正在绘制标注：更新进行中的一笔（图像像素坐标，缩放无关）
                POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
                POINT ip = ClientToImg(pt);
                g.cur.b = ip;
                if (g.cur.tool == AT_PEN) {
                    // 折线：距上一点超过 1 图像像素才追加，避免点爆炸
                    if (!g.cur.pts.empty()) {
                        POINT& last = g.cur.pts.back();
                        if (std::abs(ip.x - last.x) + std::abs(ip.y - last.y) > 1)
                            g.cur.pts.push_back(ip);
                    }
                }
                InvalidateAll(h);
                break;
            }
            if (!g.drag) break;   // 工具栏悬停已在独立工具窗内处理
            POINT cur = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            POINT a = ClientToImg(g.dragStart);
            POINT b = ClientToImg(cur);
            g.selImg.left   = std::min(a.x, b.x);
            g.selImg.top    = std::min(a.y, b.y);
            g.selImg.right  = std::max(a.x, b.x);
            g.selImg.bottom = std::max(a.y, b.y);
            g.hasSel = (g.selImg.right - g.selImg.left > 2 &&
                        g.selImg.bottom - g.selImg.top > 2);
            InvalidateAll(h);
            break;
        }

        case WM_LBUTTONUP: {
            if (g.resizing) {           // 边缘拖拽结束
                g.resizing = false;
                ReleaseCapture();
                // 全屏失效一次（带擦除）：清掉快速拖拽时可能残留在屏幕上的拖影
                InvalidateRect(h, NULL, FALSE);
                InvalidateRect(NULL, NULL, TRUE);
                break;
            }
            if (g.panning) {
                g.panning = false;
                ReleaseCapture();
                InvalidateAll(h);
                break;
            }
            if (g.drawing) {
                // 完成一笔：有效则入列。不在此时复制——最终图在窗口退出时
                // 统一复制（用户定案），标注过程安静无提示。
                g.drawing = false;
                ReleaseCapture();
                bool ok = false;
                if (g.cur.tool == AT_PEN) {
                    ok = g.cur.pts.size() >= 2;
                } else {
                    int dx = std::abs(g.cur.b.x - g.cur.a.x);
                    int dy = std::abs(g.cur.b.y - g.cur.a.y);
                    ok = (dx > 2 || dy > 2);
                }
                if (ok) g.annos.push_back(g.cur);
                InvalidateAll(h);
                break;
            }
            if (!g.drag) break;
            g.drag = false;
            ReleaseCapture();
            InvalidateAll(h);
            break;
        }

        case WM_RBUTTONUP: {
            POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            ShowContextMenu(h, pt);
            break;
        }

        case WM_NCLBUTTONUP:        // 非客户区松开也要结束拖拽（保险）
            if (g.resizing) { g.resizing = false; ReleaseCapture(); }
            break;

        case WM_CAPTURECHANGED:     // 捕获被系统夺走（如弹出了菜单）：终止拖拽
            g.resizing = false;
            g.drawing = false;
            break;

        case WM_MOUSEWHEEL: {
            POINT sp = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            ScreenToClient(h, &sp);
            int delta = GET_WHEEL_DELTA_WPARAM(w);
            if (GetKeyState(VK_CONTROL) < 0) {
                ZoomAt(h, sp, delta > 0 ? 1.25 : 0.8);
            } else {
                bool shift = GetKeyState(VK_SHIFT) < 0;
                if (shift) g.scrollX -= delta; else g.scrollY -= delta;
                ClampScroll();
                ApplyScrollbars(h);
                InvalidateAll(h);
            }
            break;
        }

        case WM_VSCROLL: DoScroll(h, SB_VERT, LOWORD(w)); break;
        case WM_HSCROLL: DoScroll(h, SB_HORZ, LOWORD(w)); break;

        case WM_APP + 1: // 后台 OCR 完成，词表已回填
            InvalidateAll(h);
            break;

        case WM_KEYDOWN:
            if (w == VK_ESCAPE) { DestroyWindow(h); break; }
            // Ctrl 组合键：Ctrl+A 全选、Ctrl+C 复制（与 Ctrl+滚轮缩放不冲突）
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                if (w == 'A' || w == 'a') {       // 全选：选区扩到整张图
                    if (g.img.width > 0 && g.img.height > 0) {
                        g.hasSel = true;
                        g.selImg = { 0, 0, g.img.width, g.img.height };
                        g.toast = false;
                        InvalidateAll(h);
                    }
                    break;
                }
                if (w == 'C' || w == 'c') {       // 复制选中文字
                    DoCopy(h);
                    break;
                }
                if (w == 'Z' || w == 'z') {       // 撤销最后一笔标注
                    if (!g.annos.empty()) {
                        g.annos.pop_back();
                        InvalidateAll(h);
                    }
                    break;
                }
            }
            break;

        case WM_TIMER:
            if (w == 1) {
                KillTimer(h, 1);
                DestroyWindow(h); // 复制成功提示后自动关闭
            }
            break;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc; GetClientRect(h, &rc);
            g.clientW = rc.right; g.clientH = rc.bottom;

            // 词表快照：OCR 工作线程可能正在回填，先在锁内取一份
            std::vector<OcrWord> words;
            bool ocrPending;
            {
                std::lock_guard<std::mutex> lk(g_wordsMutex);
                words = g.words;
                ocrPending = g.ocrPending;
            }

            // ---- 双缓冲：全部画到内存位图，再一次贴到屏幕（无闪烁）----
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP mb = CreateCompatibleBitmap(dc, g.clientW, g.clientH);
            HGDIOBJ oldBmp = SelectObject(mem, mb);

            HBRUSH bg = CreateSolidBrush(kBg);
            FillRect(mem, &rc, bg);
            DeleteObject(bg);

            if (g.hSrc && g.img.width > 0 && g.img.height > 0) {
                int ox, oy; ComputeOrigin(ox, oy);
                int cw = ContentW(), ch = ContentH();

                // 原图（1:1 时不设任何拉伸模式，HALFTONE 只在真放大时启用，
                // 避免等尺寸位块传输出现条纹伪影）
                HDC imgDC = CreateCompatibleDC(mem);
                HGDIOBJ oldSrc = SelectObject(imgDC, g.hSrc);
                if (g.s < 1.0) {
                    SetStretchBltMode(mem, COLORONCOLOR);
                } else if (g.s > 1.0) {
                    SetStretchBltMode(mem, HALFTONE);
                    SetBrushOrgEx(mem, ox % 8, oy % 8, NULL);
                }
                StretchBlt(mem, ox, oy, cw, ch,
                           imgDC, 0, 0, g.img.width, g.img.height, SRCCOPY);
                SelectObject(imgDC, oldSrc);
                DeleteDC(imgDC);

                // 选中的文字：按行合并成连续等高色带，背景替换为高亮灰，
                // 文字保持原图（平时完全无框）
                if (g.hasSel) {
                    for (const LineBand& b : CollectLineBands(words)) {
                        RECT wr{ b.x0, b.y0, b.x1, b.y1 };
                        RECT cr = ImgToClient(wr);
                        RECT clip;
                        if (IntersectRect(&clip, &cr, &rc)) {
                            HRGN rgn = CreateRectRgnIndirect(&clip);
                            SelectClipRgn(mem, rgn);
                            BlitWordHighlight(mem, g.img, b.x0, b.y0,
                                              b.x1 - b.x0, b.y1 - b.y0, cr);
                            SelectClipRgn(mem, NULL);
                            DeleteObject(rgn);
                        }
                    }
                    // 拖选虚线框（不填充，避免盖住文字）；整图全选时不画框
                    bool full = (g.selImg.left == 0 && g.selImg.top == 0 &&
                                 g.selImg.right == g.img.width &&
                                 g.selImg.bottom == g.img.height);
                    if (!full) {
                        RECT sr = ImgToClient(g.selImg);
                        HPEN pen = CreatePen(PS_DASH, 1, RGB(0, 120, 215));
                        HGDIOBJ oldPen = SelectObject(mem, pen);
                        HGDIOBJ oldBrush = SelectObject(mem, GetStockObject(NULL_BRUSH));
                        Rectangle(mem, sr.left, sr.top, sr.right, sr.bottom);
                        SelectObject(mem, oldBrush);
                        SelectObject(mem, oldPen);
                        DeleteObject(pen);
                    }
                }

                // 标注：画在图像与选区高亮之上（涂改只影响图片显示与复制，
                // 不影响文字识别/拾取——词表来自原图）
                {
                    gp::Graphics gr(mem);
                    gr.SetSmoothingMode(gp::SmoothingModeAntiAlias);
                    DrawAnnotations(gr);
                }

                // 复制成功提示（底部居中小条）
                if (g.toast) {
                    const wchar_t* tip = g.toastMsg.c_str();
                    HFONT f = CreateFontW(-18, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                          CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                          DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
                    HGDIOBJ oldf = SelectObject(mem, f);
                    SIZE ts; GetTextExtentPoint32W(mem, tip, (int)wcslen(tip), &ts);
                    int bw = ts.cx + 32, bh = ts.cy + 14;
                    RECT bar{ (g.clientW - bw) / 2, g.clientH - bh - 24,
                              (g.clientW + bw) / 2, g.clientH - 24 };
                    HBRUSH bb = CreateSolidBrush(RGB(50, 50, 50));
                    FillRect(mem, &bar, bb);
                    DeleteObject(bb);
                    SetBkMode(mem, TRANSPARENT);
                    SetTextColor(mem, RGB(255, 255, 255));
                    DrawTextW(mem, tip, -1, &bar,
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    SelectObject(mem, oldf);
                    DeleteObject(f);
                }

                // OCR 未完成提示（完成前拖选没有高亮属正常现象）
                if (ocrPending) {
                    const wchar_t* tip = L"正在识别文字…";
                    HFONT f = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                          CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                          DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
                    HGDIOBJ oldf = SelectObject(mem, f);
                    SetBkMode(mem, TRANSPARENT);
                    SetTextColor(mem, RGB(170, 170, 170));
                    RECT tr{12, g.clientH - 26, 320, g.clientH - 4};
                    DrawTextW(mem, tip, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                    SelectObject(mem, oldf);
                    DeleteObject(f);
                }
            } else {
                SetBkMode(mem, TRANSPARENT);
                SetTextColor(mem, RGB(200, 200, 200));
                DrawTextW(mem, L"（无图像）", -1, &rc,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }

            // 标注工具栏：客户区右下角（最上层，最后画）
            // （工具栏已改为独立工具窗，不再画在结果窗口客户区里）

            BitBlt(dc, 0, 0, g.clientW, g.clientH, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldBmp);
            DeleteObject(mb);
            DeleteDC(mem);
            EndPaint(h, &ps);
            break;
        }

        case WM_DESTROY:
            // 退出时统一复制最终版图片（用户定案）：
            //   ①不标注不识字直接退出 → 复制原图；
            //   ②标注后退出 → 复制最终标注图；
            //   ③标注+识字：复制文字时剪贴板是文字，退出时再覆盖为最终标注图
    //     （文字可凭 Windows+V 剪切板历史找回）。
            // 覆盖所有退出路径：ESC / 右键关闭 / 复制文字后自动关闭。
            if (g.img.width > 0 && g.img.height > 0)
                CopyAnnotatedToClipboard(h);
            if (g.hSrc) { DeleteObject(g.hSrc); g.hSrc = nullptr; }
            PostQuitMessage(0);
            break;
    }
    return DefWindowProc(h, m, w, l);
}

} // namespace

// 换行模式全局开关（定义在匿名命名空间之外：ocraddon.cpp 跨编译单元调用，
// 必须是外部链接；g_lineMode 本体留在匿名命名空间，仅本文件可见）。
void SetLineMode(NewlineMode mode) { g_lineMode = mode; }

// 退出方式全局开关（同样放匿名命名空间之外，供 ocraddon.cpp 设置）。
void SetExitMode(int m) { g_exitMode = (m == 1) ? 1 : 0; }

// 复制选中的文字到剪贴板，并按“退出方式”处理后续：
//   exit=0 复制后约 1 秒自动关闭；exit=1 复制后保持打开，等待 ESC 关闭。
// 同时供右键菜单与 Ctrl+C 调用，避免逻辑重复。
// 定义放在文件末尾的匿名命名空间之外会触发 C2129（前向声明在 anon-ns 内），
// 故本实现仅在本文件使用，直接置于 anon-ns 内、ShowContextMenu 之前。


// 把截到的图片以 CF_DIB 写入剪贴板（“直接截图”模式用，静默无窗口）。
// 输入 RGB top-down，剪贴板要求 bottom-up BGR，这里做一次行序与通道转换。
bool CopyImageToClipboard(const CaptureImage& img) {
    if (img.width <= 0 || img.height <= 0 || img.rgb.empty()) return false;
    int rowBytes = ((img.width * 3 + 3) / 4) * 4;
    size_t size = sizeof(BITMAPINFOHEADER) + (size_t)rowBytes * img.height;
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!hg) return false;
    void* p = GlobalLock(hg);
    if (!p) { GlobalFree(hg); return false; }
    BITMAPINFOHEADER* bi = (BITMAPINFOHEADER*)p;
    bi->biSize          = sizeof(BITMAPINFOHEADER);
    bi->biWidth         = img.width;
    bi->biHeight        = img.height;      // 正值 = bottom-up（剪贴板标准方向）
    bi->biPlanes        = 1;
    bi->biBitCount      = 24;
    bi->biCompression   = BI_RGB;
    bi->biSizeImage     = (DWORD)rowBytes * img.height;
    bi->biXPelsPerMeter = 0;
    bi->biYPelsPerMeter = 0;
    bi->biClrUsed       = 0;
    bi->biClrImportant  = 0;
    BYTE* base = (BYTE*)p + sizeof(BITMAPINFOHEADER);
    for (int y = 0; y < img.height; y++) {
        BYTE* dstRow = base + (size_t)y * rowBytes;
        const unsigned char* srcRow = &img.rgb[(size_t)(img.height - 1 - y) * img.width * 3];
        for (int x = 0; x < img.width; x++) {
            dstRow[x * 3 + 0] = srcRow[x * 3 + 2]; // B
            dstRow[x * 3 + 1] = srcRow[x * 3 + 1]; // G
            dstRow[x * 3 + 2] = srcRow[x * 3 + 0]; // R
        }
    }
    GlobalUnlock(hg);
    // OpenClipboard 可能被其他进程长时间占用，阻塞会导致 UI 无响应。
    // 这里做有限次重试，每次间隔 50ms，最多约 1 秒；仍失败则放弃本次复制。
    bool opened = false;
    for (int i = 0; i < 20; ++i) {
        if (OpenClipboard(nullptr)) { opened = true; break; }
        Sleep(50);
    }
    if (!opened) { GlobalFree(hg); return false; }
    EmptyClipboard();
    if (!SetClipboardData(CF_DIB, hg)) GlobalFree(hg); // 成功后剪贴板接管内存
    CloseClipboard();
    return true;
}

// 换行设置统一入口（接口在 ocraddon_internal.h 预留）。三种模式：
//   Delete 删除所有换行（= 旧行为）；Keep 保持识别出的行结构；
//   Smart  智能换行：先按行分组，再交给 textjoin::JoinLines 按续行信号合并/保留。
std::string FormatSelection(const std::vector<OcrWord>& words, NewlineMode mode) {
    if (words.empty()) return {};
    if (mode == NewlineMode::Delete) return JoinWordsWithSpace(words); // 删除所有换行
    auto lines = GroupByLine(words);
    std::vector<std::string> lineTexts;
    for (auto& ln : lines) lineTexts.push_back(JoinWordsWithSpace(ln));
    if (mode == NewlineMode::Keep) {                                  // 保持换行
        std::string s;
        for (size_t i = 0; i < lineTexts.size(); i++) {
            s += lineTexts[i];
            if (i + 1 < lineTexts.size()) s += '\n';
        }
        return s;
    }
    return textjoin::JoinLines(lineTexts);                            // 智能换行
}

void ShowResultAndCopy(CaptureImage img) {
    // 不再等待上一轮 OCR（旧实现会卡到上一轮识别结束，连续截图要等 ~10 秒）。
    // 先递增代次：旧 OCR 回调即使抢到锁也会因代次不符被丢弃，绝不污染新一轮状态。
    const int myGen = ++g_resultGen;
    {
        std::lock_guard<std::mutex> lk(g_wordsMutex);   // 与 OCR 回调互斥，安全重置
        if (g.hSrc) { DeleteObject(g.hSrc); g.hSrc = nullptr; }
        g = VState{};
        g.img = std::move(img);
        g.ocrPending = true;
        g.s = 1.0; // 默认 1:1
        g.hSrc = BuildSourceDIB(g.img.width, g.img.height, g.img.rgb);
    }
    // 弹出图片的同时把原图写入剪贴板（CF_DIB）：截图即所得，随后复制文字时
    // 会覆盖为文字。与“直接截图”模式共用同一个写剪贴板实现。
    CopyImageToClipboard(g.img);

    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = VWnd;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_CROSS);
        wc.hbrBackground = NULL;
        wc.hIcon = LoadIconW(GetModuleHandle(NULL), MAKEINTRESOURCEW(1));
        wc.lpszClassName = L"OCRView";
        RegisterClassW(&wc);
        reg = true;
    }

    // 窗口默认大小（用户定案②，第十轮修订）：= 原图 + 21px 余量。
    // 标准滚动条一旦显示就吃掉约 17px 客户区；开窗瞬间客户区（窗口宽-17）
    // 仍 ≥ 图片，滚动条判定“无需滚动”自动隐藏，最终客户区 = 原图 + 21px。
    // 满/超屏截图仍钳到工作区大小（此时滚动条是真实需要，属正常）。
    POINT cp; GetCursorPos(&cp);
    HMONITOR hm = MonitorFromPoint(cp, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfo(hm, &mi)) {
        mi.rcWork = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    }
    int mw = mi.rcWork.right - mi.rcWork.left;
    int mh = mi.rcWork.bottom - mi.rcWork.top;
    int ww = std::max(320, std::min(g.img.width + 21,  mw));
    int wh = std::max(240, std::min(g.img.height + 21, mh));
    int x = mi.rcWork.left + (mw - ww) / 2;
    int y = mi.rcWork.top + (mh - wh) / 2;

    g.hwnd = CreateWindowExW(WS_EX_APPWINDOW, L"OCRView", L"OCRView",
                             WS_POPUP | WS_VSCROLL | WS_HSCROLL | WS_VISIBLE,
                             x, y, ww, wh,
                             NULL, NULL, GetModuleHandle(NULL), NULL);
    if (!g.hwnd) {
        printf("[view] CreateWindowEx failed, err=%lu\n", GetLastError());
        return;
    }
    // 窗口尺寸逻辑见 FitWindowToContent 顶部注释（默认=原图；放大跟随、
    // 缩小不跟随；用户手动拖动后完全固定）。手动调整最小 200x120 仍在
    // WM_GETMINMAXINFO / 拖拽逻辑中保证。
    // 无边框窗口需要主动拿到前台焦点，否则键盘（ESC）不响应
    SetForegroundWindow(g.hwnd);
    SetFocus(g.hwnd);
    // 标注工具栏：独立小工具窗（微信风格），外挂在结果窗口右下角外
    TbCreate();
    // 先显示图片；OCR 放到后台线程，完成后回填词表并刷新（高亮那时才可用）
    OcrImageAsync(g.img, [myGen](std::vector<OcrWord> words) {
        std::lock_guard<std::mutex> lk(g_wordsMutex);
        if (myGen != g_resultGen.load()) return;   // 旧一代结果，丢弃
        g.words = std::move(words);
        g.ocrPending = false;
        if (g.hwnd) PostMessageW(g.hwnd, WM_APP + 1, 0, 0);
    });
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
}
