// search_app.cpp — 极速启 · 全盘文件搜索 (rebuilt, borderless flat UI)
// Build: see build.bat
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <cwchar>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <algorithm>
#include <unordered_set>
#include <deque>
#include <cstdio>
#include <io.h>
#include <malloc.h>
#include <chrono>
#include <timezoneapi.h>
#include <psapi.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "psapi.lib")

// ============================================================
// 0. basics
// ============================================================
static HINSTANCE g_inst = nullptr;
static HWND g_hwnd = nullptr;
static HWND g_edit = nullptr;
static UINT   g_wmActivate = 0;        // RegisterWindowMessage：单实例唤醒（叫到最前并聚焦搜索框）
static HANDLE g_singleMtx  = nullptr;  // 单实例互斥量（进程生命周期内保持打开）
static HWND g_pop = nullptr;      // scope popup
static int  g_dpi = 96;
static int  S(int v) { return MulDiv(v, g_dpi, 96); }

enum : UINT {
    WM_APP_SEARCHDONE = WM_APP + 1,
    WM_APP_INDEXSTAT  = WM_APP + 2,
    WM_APP_COMMITTED  = WM_APP + 3,
    WM_APP_EXIT        = WM_APP + 101,   // 主程序/右键菜单 → 显式退出
    WM_APP_RESCAN_DONE = WM_APP + 102,   // 调度线程重扫完成 → UI 重跑当前查询
};

// ---------------- colors ----------------
static const COLORREF C_TITLE    = RGB(46, 124, 214);   // same as buttons
static const COLORREF C_BTN      = RGB(46, 124, 214);
static const COLORREF C_BTN_HOV  = RGB(66, 142, 226);
static const COLORREF C_BG       = RGB(241, 244, 249);
static const COLORREF C_WHITE    = RGB(255, 255, 255);
static const COLORREF C_SEP      = RGB(227, 231, 238);
static const COLORREF C_TEXT     = RGB(50, 58, 70);
static const COLORREF C_SUB      = RGB(133, 143, 158);
static const COLORREF C_SEL      = RGB(232, 241, 252);
static const COLORREF C_HOVER    = RGB(244, 247, 251);
static const COLORREF C_BLUE_TXT = RGB(28, 108, 198);
static const COLORREF C_HEADERBG = RGB(250, 251, 253);
static const COLORREF C_THUMB    = RGB(186, 196, 212);
static const COLORREF C_BOXBRD   = RGB(225, 230, 238);

// ---------------- early globals / forward declarations ----------------
static uint64_t g_curEpoch = 0;
static int g_catDragY = 0;

static std::wstring U8(const std::string&);
static std::string W8(const std::wstring&);
static std::wstring LowerW(const std::wstring&);
static std::wstring ExtOf(const std::wstring&);
static std::wstring SizeStr(uint64_t);
static std::wstring TimeStr(uint64_t);
static std::wstring GetExeDir();
static std::wstring CacheDir();
struct Rec; struct Res; struct VolBuild; struct VolView;
static bool SaveVol(const std::wstring&, const VolBuild&, uint64_t);
static bool LoadVol(const std::wstring&, std::shared_ptr<VolView>&, uint64_t&);
static void IndexerThread(std::stop_token);
static void RequestSearch();
static void SearcherThread(std::stop_token);
static int VisibleRows();
static void ComputeLayout();
static void PaintAll();
static void FillRC(HDC, RECT, COLORREF);
static void FillRcRounded(HDC, RECT, int, COLORREF);
static void DrawTextRC(HDC, const std::wstring&, RECT, COLORREF, HFONT, UINT);
static void DrawExtBadge(HDC, int, int, const Res&, int, int);
static void DrawFolderGlyph(HDC, int, int, int, COLORREF);
static void DrawTitle(HDC);
static void DrawCatItem(HDC, int, int, bool, bool);
static void DrawList(HDC);
static void DrawPreview(HDC);
static int PtInRC(const RECT&, int, int);
static int ZoneAt(int, int);
static RECT PreviewOpenRect();
static RECT PreviewOpenFRect();
static int CatItemAt(int);
static int RowAt(int);
static int ColAt(int);
static bool NearColSep(int, int&);
static void ClampScroll();
static void SelectCat(int);
static void OpenItem(const Res&, bool);
static void CopyPath(const Res&);
static const Res* SelectedRes();
static void ShowScopePopup(int seg);
static void ShowContextMenu(POINT pt);
static void ShowTypeMenu();
static RECT FilterArrowRect();
static LRESULT CALLBACK EditProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK ScopePopupProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK CtxMenuProc(HWND, UINT, WPARAM, LPARAM);
static bool VScrollMetrics(int& trackH, int& thumbH, int& thumbTop, int& maxScroll);
static void SortResults();
static void RebuildView();
static void ApplySortAndView();
static void DoSearch();
static int CfgInt(const wchar_t*, int);
static void CfgSet(const wchar_t*, int);
static std::wstring ConfigPath();
static void LoadConfig();
static void SaveConfig();
static void MakeFonts();

// ============================================================
// 1. categories
// ============================================================
enum { CAT_ALL = 0, CAT_IMG = 1, CAT_DOC = 2, CAT_VIDEO = 3, CAT_AUDIO = 4,
       CAT_ARCH = 5, CAT_APP = 6, CAT_LNK = 7, CAT_DIR = 8, CAT_OTHER = 9,
       CAT_COUNT = 10 };
struct CatDef { int id; const wchar_t* name; COLORREF color; };
static const CatDef g_catDefs[CAT_COUNT] = {
    { CAT_ALL,   L"全部",     RGB(46,124,214)  },
    { CAT_IMG,   L"图片",     RGB(230,126,34)  },
    { CAT_DOC,   L"文档",     RGB(46,124,214)  },
    { CAT_VIDEO, L"视频",     RGB(142,90,216)  },
    { CAT_AUDIO, L"音频",     RGB(22,160,133)  },
    { CAT_ARCH,  L"压缩包",   RGB(231,76,60)   },
    { CAT_APP,   L"程序",     RGB(52,73,94)    },
    { CAT_LNK,   L"快捷方式", RGB(127,140,141) },
    { CAT_DIR,   L"文件夹",   RGB(224,168,0)   },
    { CAT_OTHER, L"其他",     RGB(127,140,141) },
};
static std::vector<int> g_catOrder = { 0,1,2,3,4,5,6,7,8,9 };

static bool ExtIn(const std::wstring& e, std::initializer_list<const wchar_t*> list) {
    for (auto* x : list) if (e == x) return true;
    return false;
}
static int ClassifyExt(const std::wstring& ext) {
    if (ext.empty()) return CAT_OTHER;
    if (ext == L"lnk" || ext == L"url") return CAT_LNK;
    if (ExtIn(ext, { L"jpg",L"jpeg",L"png",L"gif",L"bmp",L"webp",L"heic",L"heif",L"tif",L"tiff",L"svg",L"ico",L"psd",L"ai",L"cr2",L"nef",L"arw",L"dng",L"jfif",L"raw",L"emf",L"wmf" })) return CAT_IMG;
    if (ExtIn(ext, { L"docx",L"doc",L"wps",L"wpt",L"rtf",L"txt",L"xlsx",L"xls",L"et",L"ett",L"csv",L"pptx",L"ppt",L"dps",L"dpt",L"ppsx",L"pdf" })) return CAT_DOC;
    if (ExtIn(ext, { L"mp4",L"avi",L"mkv",L"mov",L"wmv",L"flv",L"m4v",L"rmvb",L"mpg",L"mpeg",L"webm",L"ts",L"3gp",L"vob",L"dat",L"m2ts" })) return CAT_VIDEO;
    if (ExtIn(ext, { L"mp3",L"wav",L"flac",L"aac",L"ogg",L"m4a",L"wma",L"opus",L"ape",L"mid",L"aiff" })) return CAT_AUDIO;
    if (ExtIn(ext, { L"zip",L"rar",L"7z",L"tar",L"gz",L"bz2",L"xz",L"zst",L"iso",L"cab",L"jar",L"tgz" })) return CAT_ARCH;
    if (ExtIn(ext, { L"exe",L"msi",L"bat",L"cmd",L"com",L"scr",L"appx",L"msix" })) return CAT_APP;
    return CAT_OTHER;
}

// ============================================================
// 2. index store
// ============================================================
#pragma pack(push, 1)
struct Rec {
    uint32_t nameOff, nameLen, lowerOff, lowerLen, parent;
    uint64_t size, mtime;
    uint32_t attrs;
    uint8_t  cat;
};
#pragma pack(pop)
static_assert(sizeof(Rec) == 41, "Rec must be 41B packed (disk == memory layout)");

// 构建期用的堆对象（只在重扫线程栈上，落盘后释放）
struct VolBuild {
    wchar_t letter = 0;
    uint32_t ver = 2;
    std::wstring parentPath;
    std::vector<Rec> recs;
    std::string nameBlob, lowerBlob;
    std::vector<std::wstring> dirs;        // DFS preorder；dirs[0]=L"C:"
    std::vector<uint32_t> subtreeEnd;
};
// mmap 映射的 RAII 句柄
struct MapHandle {
    HANDLE hFile = nullptr, hMap = nullptr;
    const uint8_t* base = nullptr;
    size_t size = 0;
    ~MapHandle() {
        if (base) UnmapViewOfFile(base);
        if (hMap) CloseHandle(hMap);
        if (hFile) CloseHandle(hFile);
    }
};
// 读端：只读文件映射视图（索引本体不再进堆，私有内存≈0）
struct VolView {
    wchar_t letter = 0;
    uint32_t ver = 2;
    std::shared_ptr<MapHandle> map;
    uint64_t nRecs = 0, nDirs = 0;
    const uint8_t* recs = nullptr;         // nRecs * 41
    const uint8_t* dirsBlob = nullptr;
    uint64_t dirsBlobLen = 0;
    const uint8_t* subtreeEnd = nullptr;   // nDirs * 4（v2）
    const uint8_t* nameBlob = nullptr;  uint64_t nameLen = 0;
    const uint8_t* lowerBlob = nullptr; uint64_t lowerLen = 0;
    std::vector<uint32_t> dirOff;          // 每个目录在 dirsBlob 内的偏移
    std::wstring shardRel;                 // P2：本片相对路径（小写，""=卷根片）
    bool isRootShard = false;               // P2：「仅显示顶层」只认卷根片里 parent==0 的记录
};
using Snapshot = std::vector<std::shared_ptr<VolView>>;
static std::atomic<std::shared_ptr<Snapshot>> g_snap{ std::make_shared<Snapshot>() };

// ---- Phase1：常驻 / 监听 / 调度 状态 ----
static std::atomic<bool> g_visible{ true };          // 窗口是否可见（隐藏期才施加最小重扫间隔）
static bool              g_wasHidden = false;       // 曾经被 WM_CLOSE 隐藏过（重显时需重载索引）
static std::mutex        g_listenMu;
static HANDLE            g_listenHandle[26] = {};    // 监听线程真实句柄（CancelSynchronousIo 用）
static HANDLE            g_listenThreadH[26] = {};   // CreateThread 句柄（join 用）
static std::mutex        g_rescanMu;                 // C6：全局串行化重扫（一次只重扫一个片）
static const size_t MAX_STORED = 50000;

// ---------------- string helpers ----------------
static std::wstring U8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
static std::string W8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
static std::wstring LowerW(const std::wstring& w) {
    std::wstring o; o.reserve(w.size());
    for (wchar_t c : w) o.push_back((wchar_t)towlower(c));
    return o;
}
// ---------------- mmap 视图读取辅助 ----------------
static inline uint32_t RdU32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint64_t RdU64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return v; }
static std::wstring VolDirAt(const VolView& v, size_t i) {
    if (i >= v.dirOff.size() || !v.dirsBlob) return {};
    size_t off = v.dirOff[i];
    if (off + 2 > v.dirsBlobLen) return {};
    uint16_t l = 0; memcpy(&l, v.dirsBlob + off, 2);
    if (off + 2 + (size_t)l > v.dirsBlobLen) return {};
    return U8(std::string((const char*)v.dirsBlob + off + 2, l));
}
static std::wstring ExtOf(const std::wstring& name) {
    size_t p = name.find_last_of(L'.');
    if (p == std::wstring::npos || p + 1 >= name.size()) return {};
    return LowerW(name.substr(p + 1));
}
static std::wstring SizeStr(uint64_t v) {
    wchar_t b[64];
    if (v < 1024) swprintf(b, 64, L"%llu B", (unsigned long long)v);
    else if (v < 1024ull * 1024) swprintf(b, 64, L"%.1f KB", v / 1024.0);
    else if (v < 1024ull * 1024 * 1024) swprintf(b, 64, L"%.1f MB", v / (1024.0 * 1024));
    else if (v < 1024ull * 1024 * 1024 * 1024) swprintf(b, 64, L"%.2f GB", v / (1024.0 * 1024 * 1024));
    else swprintf(b, 64, L"%.2f TB", v / (1024.0 * 1024 * 1024 * 1024));
    return b;
}
static std::wstring TimeStr(uint64_t ft) {
    FILETIME f{}; f.dwHighDateTime = (DWORD)(ft >> 32); f.dwLowDateTime = (DWORD)ft;
    SYSTEMTIME st{}, lst{};
    if (!FileTimeToSystemTime(&f, &st) || st.wYear < 1980) return L"-";
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &st, &lst)) lst = st;
    wchar_t b[40];
    swprintf(b, 40, L"%04u-%02u-%02u %02u:%02u", lst.wYear, lst.wMonth, lst.wDay, lst.wHour, lst.wMinute);
    return b;
}

// ---------------- idx persistence ----------------
#pragma pack(push, 1)
struct IdxHead {
    char     magic[8];      // "WBSVOL1\0"
    uint32_t ver;
    uint64_t buildTime;
    uint32_t letter;
    uint64_t nRecs, nDirs;
};
#pragma pack(pop)
static_assert(sizeof(IdxHead) == 40, "IdxHead layout");
static const char IDX_MAGIC[8] = { 'W','B','S','V','O','L','1','\0' };
static const uint64_t REFRESH_INTERVAL = 6ull * 3600 * 10000000; // 6h in FILETIME ticks(100ns)=6h*36e8? use filetime compare below
static const uint64_t SIX_HOURS_TICKS = 6ull * 3600 * 1000 * 10000; // 6h * 10^7
// 启动时只有缓存"非常旧"（>24h）才后台重扫；否则先直接用缓存秒出结果，避免每次打开都等 1~2 分钟。
static const uint64_t STARTUP_STALE_TICKS = 24ull * 60 * 60 * 1000 * 10000; // 24 小时

static bool SaveVol(const std::wstring& path, const VolBuild& v, uint64_t buildTime) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") || !f) return false;
    bool ok = true;
    do {
        IdxHead h{};
        memcpy(h.magic, IDX_MAGIC, 8);
        h.ver = 2; h.buildTime = buildTime; h.letter = v.letter;
        h.nRecs = v.recs.size(); h.nDirs = v.dirs.size();
        if (fwrite(&h, sizeof h, 1, f) != 1) { ok = false; break; }
        // v2：片的外部父路径（整卷片为空）
        {
            std::string pp = W8(v.parentPath);
            uint16_t l = (uint16_t)pp.size();
            if (fwrite(&l, 2, 1, f) != 1 || (l && fwrite(pp.data(), 1, l, f) != l)) { ok = false; break; }
        }
        for (auto& d : v.dirs) {
            std::string u = W8(d);
            uint16_t l = (uint16_t)u.size();
            if (fwrite(&l, 2, 1, f) != 1 || (l && fwrite(u.data(), 1, l, f) != l)) { ok = false; break; }
        }
        if (!ok) break;
        // v2：subtreeEnd（长度必须 = nDirs）
        if (h.nDirs) {
            if (v.subtreeEnd.size() == v.dirs.size()) {
                if (fwrite(v.subtreeEnd.data(), sizeof(uint32_t), v.subtreeEnd.size(), f) != v.subtreeEnd.size()) { ok = false; break; }
            } else {
                std::vector<uint32_t> zeros((size_t)h.nDirs, 0);
                if (fwrite(zeros.data(), sizeof(uint32_t), zeros.size(), f) != zeros.size()) { ok = false; break; }
            }
        }
        // v2：recs 直接写 41B 原始布局（磁盘 = 内存）
        if (h.nRecs) {
            if (fwrite(v.recs.data(), sizeof(Rec), v.recs.size(), f) != v.recs.size()) { ok = false; break; }
        }
        uint64_t nl = v.nameBlob.size(), ll = v.lowerBlob.size();
        if (fwrite(&nl, 8, 1, f) != 1 || fwrite(v.nameBlob.data(), 1, (size_t)nl, f) != nl ||
            fwrite(&ll, 8, 1, f) != 1 || fwrite(v.lowerBlob.data(), 1, (size_t)ll, f) != ll) ok = false;
    } while (0);
    // C4：落盘前把 CRT 缓冲刷到内核并 commit，确保随后 MoveFileEx 替换到的是完整文件，
    // 避免"半截 idx"被读端 LoadVol 读到。
    if (ok) { fflush(f); _commit(_fileno(f)); }
    fclose(f);
    return ok;
}
// P1b：把 idx 文件 mmap 成只读视图（索引本体不进堆）。失败=降级为无缓存，调用方重建。
static bool LoadVol(const std::wstring& path, std::shared_ptr<VolView>& out, uint64_t& buildTime) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li{};
    if (!GetFileSizeEx(hFile, &li) || li.QuadPart < (LONGLONG)sizeof(IdxHead)) { CloseHandle(hFile); return false; }
    size_t fsize = (size_t)li.QuadPart;
    HANDLE hMap = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!hMap) { CloseHandle(hFile); return false; }
    const uint8_t* base = (const uint8_t*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!base) { CloseHandle(hMap); CloseHandle(hFile); return false; }
    auto release = [&] { if (base) UnmapViewOfFile(base); if (hMap) CloseHandle(hMap); if (hFile) CloseHandle(hFile); };
    bool ok = false;
    do {
        IdxHead hd{}; memcpy(&hd, base, sizeof hd);
        if (memcmp(hd.magic, IDX_MAGIC, 8) != 0) break;
        if (hd.ver != 1 && hd.ver != 2) break;
        size_t cur = sizeof hd;
        auto take16 = [&](uint16_t& v) -> bool { if (cur + 2 > fsize) return false; memcpy(&v, base + cur, 2); cur += 2; return true; };
        auto view = std::make_shared<VolView>();
        view->letter = (wchar_t)hd.letter; view->ver = hd.ver;
        view->nRecs = hd.nRecs; view->nDirs = hd.nDirs;
        if (hd.ver >= 2) {
            uint16_t pl = 0; if (!take16(pl)) break;
            if (cur + (size_t)pl > fsize) break; cur += pl;
        }
        const uint8_t* dirsStart = base + cur;
        size_t dirsBegin = cur;
        bool dirsOk = true;
        for (uint64_t i = 0; i < hd.nDirs; i++) {
            // 偏移必须落在"长度前缀"上（VolDirAt 从该处先读 2 字节长度再取字符串）。
            // 曾经记成"字符串起点"，导致路径整体错位：路径列为空、末尾多出乱码、双击拼出的路径不存在。
            uint32_t off = (uint32_t)(cur - dirsBegin);
            uint16_t l = 0; if (!take16(l)) { dirsOk = false; break; }
            if (cur + (size_t)l > fsize) { dirsOk = false; break; }
            view->dirOff.push_back(off);
            cur += l;
        }
        if (!dirsOk || view->dirOff.size() != (size_t)hd.nDirs) break;
        view->dirsBlob = dirsStart;
        view->dirsBlobLen = cur - dirsBegin;
        if (hd.ver >= 2) {
            if (cur + (size_t)hd.nDirs * 4 > fsize) break;
            view->subtreeEnd = base + cur; cur += (size_t)hd.nDirs * 4;
        }
        if (cur + (size_t)hd.nRecs * sizeof(Rec) > fsize) break;
        view->recs = base + cur; cur += (size_t)hd.nRecs * sizeof(Rec);
        uint64_t nl = 0;
        if (cur + 8 > fsize) break; memcpy(&nl, base + cur, 8); cur += 8;
        if (cur + (size_t)nl > fsize) break;
        view->nameBlob = base + cur; view->nameLen = nl; cur += (size_t)nl;
        uint64_t ll = 0;
        if (cur + 8 > fsize) break; memcpy(&ll, base + cur, 8); cur += 8;
        if (cur + (size_t)ll > fsize) break;
        view->lowerBlob = base + cur; view->lowerLen = ll; cur += (size_t)ll;
        if (cur != fsize) break;   // 防线2：游标必须等于文件长度
        // 校验：每条记录偏移在界内、parent < nDirs（失败→降级重建）
        bool bad = false;
        for (uint64_t i = 0; i < hd.nRecs; i++) {
            const uint8_t* r = view->recs + (size_t)i * sizeof(Rec);
            uint32_t no = RdU32(r), nl2 = RdU32(r + 4), lo = RdU32(r + 8), ll2 = RdU32(r + 12), pa = RdU32(r + 16);
            if ((uint64_t)no + nl2 > nl || (uint64_t)lo + ll2 > ll || (hd.nDirs && pa >= hd.nDirs)) { bad = true; break; }
        }
        if (bad) break;
        auto mp = std::make_shared<MapHandle>();
        mp->hFile = hFile; mp->hMap = hMap; mp->base = base; mp->size = fsize;
        view->map = mp;
        out = view; buildTime = hd.buildTime; ok = true;
    } while (0);
    if (!ok) release();
    return ok;
}

// ---------------- indexer thread ----------------
static std::mutex g_statusMu;
static std::wstring g_indexStatus;
static void SetIndexStatus(std::wstring s) { std::lock_guard<std::mutex> lk(g_statusMu); g_indexStatus = std::move(s); }
static std::wstring GetIndexStatus() { std::lock_guard<std::mutex> lk(g_statusMu); return g_indexStatus; }
// 后台重扫期间临时占用状态栏，出作用域即复位。
// 修复：以前重扫只写不复位，跑完就永久停在"正在建立索引：C: (40000 个)"，看起来像一直在建索引。
struct StatusScope {
    explicit StatusScope(std::wstring s) { SetIndexStatus(std::move(s)); }
    ~StatusScope() { SetIndexStatus(L"索引已就绪"); }
};
static std::wstring GetExeDir() {
    wchar_t buf[MAX_PATH + 2] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p = buf;
    size_t s = p.find_last_of(L'\\');
    return s == std::wstring::npos ? L"." : p.substr(0, s);
}
// 数据根目录：默认 <exeDir>\data；可由主程序通过 --datadir= 指定为共享目录（极速启\data），
// 从而实现「主程序 + 搜索插件」共用同一个 data 文件夹，减少部署时的目录层级。
static std::wstring g_dataRoot;
static std::wstring DataRoot() {
    if (!g_dataRoot.empty()) return g_dataRoot;
    g_dataRoot = GetExeDir() + L"\\data";
    return g_dataRoot;
}
static std::wstring CacheDir() { return DataRoot() + L"\\cache"; }

// ================= P2：分片索引（manifest 为唯一提交点） =================
// 每个盘切成若干"片"，每片一个独立 idx 文件；一份 vol_X.manifest 记录当前各片的文件。
// 更新只重写变化的那一片 → 重扫峰值只和该片大小有关；热片可以高频重建（30 秒）。
// Windows 上不能覆盖/删除本进程正在 mmap 的文件 → 每次发布都写新的代次文件
// （vol_X.g<N>.idx），换文件只改 manifest（manifest 本身不被映射，可原子替换）。
static std::wstring IdxBase(wchar_t letter) { return CacheDir() + L"\\vol_" + std::wstring(1, letter); }
static std::wstring GenPath(wchar_t letter, uint32_t g) {
    return IdxBase(letter) + L".g" + std::to_wstring(g) + L".idx";
}
static std::wstring ManifestPath(wchar_t letter) { return IdxBase(letter) + L".manifest"; }
// "manifest 根本不存在" vs "文件在但读不出来"。前者是首次建立/被清缓存，可以从空开始写；
// 后者必须放弃本次写入并重试（否则会用旧副本覆盖别人的提交）。
static bool ManifestMissing(wchar_t letter) {
    return GetFileAttributesW(ManifestPath(letter).c_str()) == INVALID_FILE_ATTRIBUTES;
}
// 下一代编号：现存 g 文件的最大编号 +1（永不复用，避免撞上仍被映射的旧文件）
static uint32_t NextGen(wchar_t letter) {
    uint32_t mx = 0;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((IdxBase(letter) + L".g*.idx").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const wchar_t* p = wcsstr(fd.cFileName, L".g");
            if (p) { uint32_t n = (uint32_t)wcstoul(p + 2, nullptr, 10); if (n > mx) mx = n; }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return mx + 1;
}
// 把构建好的片发布为新代次文件，返回文件名（失败返回空）
// 发布时用来"往前推"代次号：清缓存后旧代次处于 delete-pending（目录里看不见、但名字还被占），
// 只靠 NextGen 扫目录会算出同一个名字，于是永远撞名、永远发布失败。这里用一个只增不减的提示值兜住。
static std::atomic<uint32_t> g_genHint[26];
static std::wstring PublishShard(wchar_t letter, VolBuild& b, uint64_t buildTime) {
    // 固定临时名（不以 .idx 结尾，不会被 manifest/GC 当作代次文件）
    std::wstring tmp = CacheDir() + L"\\vol_" + std::wstring(1, letter) + L".publish.tmp";
    if (!SaveVol(tmp, b, buildTime)) { DeleteFileW(tmp.c_str()); return {}; }
    for (int attempt = 0; attempt < 6; ++attempt) {
        uint32_t mx = NextGen(letter);
        uint32_t hint = ++g_genHint[letter - 'A'];
        uint32_t ng = mx > hint ? mx : hint;
        std::wstring target = GenPath(letter, ng);
        if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) continue;   // 名字已被占：换下一个
        // 刻意【不带】MOVEFILE_REPLACE_EXISTING：代次文件名本就不该已存在。
        // 万一撞名，宁可失败换号重试，也不要静默覆盖一个可能仍被在飞搜索结果映射着的旧代次。
        if (MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
            const wchar_t* name = wcsrchr(target.c_str(), L'\\');
            return name ? std::wstring(name + 1) : target;
        }
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS && err != ERROR_FILE_EXISTS &&
            err != ERROR_ACCESS_DENIED && err != ERROR_SHARING_VIOLATION) break;
        Sleep(40);
    }
    DeleteFileW(tmp.c_str());
    return {};
}

struct ShardSpec { std::wstring rel; std::wstring path; bool hot = false; };   // rel 小写相对路径，""=卷根片
struct ShardEntry { std::wstring rel, path, file; uint64_t nRecs = 0, nDirs = 0, buildTime = 0; };
// 片级状态：只在 g_shardMu 保护下读写（监听线程与调度线程共用）
struct ShardState {
    bool     dirty = false;
    uint32_t dirtySeq = 0;       // 每次标脏自增；重扫成功后只在 seq 未变时才清脏（避免吞掉重扫期间的新事件）
    uint64_t dirtyTimeMs = 0;    // 最近一次变化
    uint64_t firstDirtyMs = 0;   // 本轮首次变化（防止持续 churn 让防抖永远不满足）
    bool     busy = false;       // 正在重扫（防止同片重复排队）
    uint64_t lastRescanMs = 0;
};
static const uint64_t MAX_DEBOUNCE_WAIT_MS = 30ull * 1000;   // 持续变化也要在 30s 内扫一次
static std::mutex              g_shardMu;
static std::atomic<uint32_t>   g_cacheEpoch{ 0 };   // 手动刷新（F5）会 ++，索引线程据此放弃本轮构建
static std::vector<ShardSpec>  g_shards[26];
static std::vector<ShardEntry> g_manifest[26];
static std::vector<ShardState> g_shardState[26];
static const DWORD             DEBOUNCE_MS = 8000;
static const uint64_t          HOT_MIN_INTERVAL_MS  = 30ull * 1000;        // 热片：30 秒
static const uint64_t          COLD_MIN_INTERVAL_MS = 5ull * 60 * 1000;    // 冷片：5 分钟

// 决定一个盘的切片方案：卷根 + Windows 大头 + 每个用户账户的热区。
// 依据本机实测分布（C: 42 万条里 Windows\servicing 15.5 万、WinSxS 8.9 万、Users 10.6 万），
// 目标是"任一片重建峰值 ≤ 30MB"，同时把天天变的热区独立出来。
static void EnsureShards(wchar_t letter) {
    std::lock_guard<std::mutex> lk(g_shardMu);
    int i = letter - 'A';
    if (!g_shards[i].empty()) return;
    auto& v = g_shards[i];
    v.push_back({ L"", L"", false });
    std::wstring drv(1, letter); drv += L":";
    auto add = [&](const std::wstring& path, bool hot) {
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
        v.push_back({ LowerW(path.substr(3)), path, hot });
    };
    add(drv + L"\\Windows\\servicing", false);
    add(drv + L"\\Windows\\WinSxS", false);
    add(drv + L"\\Windows\\System32", false);
    add(drv + L"\\Windows", false);
    // 用户热区（多账户机器上按实际存在的账户展开，而不是写死 administrator）
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((drv + L"\\Users\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            std::wstring base = drv + L"\\Users\\" + fd.cFileName;
            add(base + L"\\AppData\\Roaming", true);
            add(base + L"\\AppData\\Local", true);
            add(base + L"\\AppData\\LocalLow", true);
            add(base + L"\\Desktop", true);
            add(base + L"\\Documents", true);
            add(base + L"\\Downloads", true);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(v.begin(), v.end(), [](const ShardSpec& a, const ShardSpec& b) { return a.rel < b.rel; });
    g_shardState[i].assign(v.size(), ShardState{});
}
// 一条路径（小写相对路径）属于哪一片：取最长的匹配前缀
static int ShardOfPath(wchar_t letter, const std::wstring& relLower) {
    int i = letter - 'A';
    int best = 0; size_t bestLen = 0;
    for (int k = 0; k < (int)g_shards[i].size(); k++) {
        const auto& s = g_shards[i][k];
        if (s.rel.empty()) continue;
        if (relLower.size() < s.rel.size()) continue;
        if (relLower.compare(0, s.rel.size(), s.rel) != 0) continue;
        if (relLower.size() != s.rel.size() && relLower[s.rel.size()] != L'\\') continue;
        if (s.rel.size() > bestLen) { bestLen = s.rel.size(); best = k; }
    }
    return best;
}
// 遍历某个盘时要"剪掉"的另一批片根（避免父子片重复收录）
static std::vector<std::wstring> PruneFor(wchar_t letter, int self) {
    std::vector<std::wstring> out;
    int i = letter - 'A';
    const auto& mine = g_shards[i][(size_t)self].rel;
    for (int k = 0; k < (int)g_shards[i].size(); k++) {
        if (k == self) continue;
        const auto& s = g_shards[i][k].rel;
        if (s.empty()) continue;
        if (s.size() <= mine.size()) continue;
        if (mine.empty() || (s.compare(0, mine.size(), mine) == 0 && s[mine.size()] == L'\\'))
            out.push_back(s);
    }
    return out;
}

// ---- manifest 读写（唯一提交点）----
static bool WriteManifest(wchar_t letter, const std::vector<ShardEntry>& es) {
    std::wstring p = ManifestPath(letter);
    std::wstring t = p + L".tmp";
    // 一律写 UTF-8 字节流（fwprintf 会把宽字符写成 UTF-16，读端按 UTF-8 解析就会失败）
    std::string out = "v1\n";
    for (auto& e : es) {
        out += "shard\t" + W8(e.rel) + "\t" + W8(e.path) + "\t" + W8(e.file) + "\t" +
               std::to_string((unsigned long long)e.nRecs) + "\t" +
               std::to_string((unsigned long long)e.nDirs) + "\t" +
               std::to_string((unsigned long long)e.buildTime) + "\n";
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, t.c_str(), L"wb") || !f) return false;
    bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
    if (ok) { fflush(f); _commit(_fileno(f)); }
    fclose(f);
    if (!ok) { DeleteFileW(t.c_str()); return false; }
    bool moved = false;
    for (int a = 0; a < 3 && !moved; ++a) {
        if (a) Sleep(200);
        moved = MoveFileExW(t.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    }
    if (!moved) DeleteFileW(t.c_str());
    return moved;
}
static bool LoadManifest(wchar_t letter, std::vector<ShardEntry>& out) {
    out.clear();
    std::wstring p = ManifestPath(letter);
    // 用全共享方式打开：否则本进程的读句柄会挡住 manifest 的原子替换（MoveFileEx 失败 → 更新发不出去）
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string all; char buf[4096]; DWORD got = 0;
    for (;;) {
        if (!ReadFile(h, buf, sizeof buf, &got, nullptr) || got == 0) break;
        all.append(buf, got);
    }
    CloseHandle(h);
    if (all.empty()) return false;
    std::wstring text = U8(all);
    size_t pos = 0; bool verOk = false;
    while (pos < text.size()) {
        size_t nl = text.find(L'\n', pos);
        if (nl == std::wstring::npos) nl = text.size();
        std::wstring line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty()) continue;
        if (line[0] == L'v' && line.size() <= 3) { verOk = true; continue; }
        // shard \t rel \t path \t file \t nRecs \t nDirs \t buildTime
        std::vector<std::wstring> f2;
        size_t s = 0;
        for (;;) {
            size_t t2 = line.find(L'\t', s);
            f2.push_back(line.substr(s, (t2 == std::wstring::npos) ? std::wstring::npos : t2 - s));
            if (t2 == std::wstring::npos) break;
            s = t2 + 1;
        }
        if (f2.size() < 7 || f2[0] != L"shard") continue;
        ShardEntry e;
        e.rel = f2[1]; e.path = f2[2]; e.file = f2[3];
        e.nRecs = wcstoull(f2[4].c_str(), nullptr, 10);
        e.nDirs = wcstoull(f2[5].c_str(), nullptr, 10);
        e.buildTime = wcstoull(f2[6].c_str(), nullptr, 10);
        out.push_back(std::move(e));
    }
    return verOk && !out.empty();
}
// 启动期 GC（此时没有任何视图存活）：删掉 manifest 未引用的代次文件、旧单文件、.cur、残留 tmp
// 注意：读不出 manifest 时【绝不删任何 idx】——否则会把刚发布的分片全清掉（曾经真踩过）。
static void GcGenerations() {
    for (int i = 0; i < 26; i++) {
        wchar_t letter = (wchar_t)('A' + i);
        std::vector<ShardEntry> es;
        bool haveMf = LoadManifest(letter, es);
        if (haveMf) {
            auto referenced = [&](const std::wstring& name) {
                for (auto& e : es) if (_wcsicmp(e.file.c_str(), name.c_str()) == 0) return true;
                return false;
            };
            WIN32_FIND_DATAW fd{};
            HANDLE h = FindFirstFileW((IdxBase(letter) + L".g*.idx").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (referenced(fd.cFileName)) continue;
                    DeleteFileW((CacheDir() + L"\\" + std::wstring(fd.cFileName)).c_str());
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            DeleteFileW((IdxBase(letter) + L".idx").c_str());   // 旧单文件索引退休
            DeleteFileW((IdxBase(letter) + L".cur").c_str());
        }
        WIN32_FIND_DATAW fd2{};
        HANDLE h2 = FindFirstFileW((IdxBase(letter) + L".*.tmp").c_str(), &fd2);
        if (h2 != INVALID_HANDLE_VALUE) {
            do { DeleteFileW((CacheDir() + L"\\" + std::wstring(fd2.cFileName)).c_str()); }
            while (FindNextFileW(h2, &fd2));
            FindClose(h2);
        }
    }
}
// 按所有盘的 manifest 重建内存快照（每片一个 mmap 视图；总代价约 1–2ms）
static void RebuildSnapshotFromManifests() {
    auto ns = std::make_shared<Snapshot>();
    for (int i = 0; i < 26; i++) {
        wchar_t letter = (wchar_t)('A' + i);
        std::vector<ShardEntry> es;
        if (!LoadManifest(letter, es)) continue;
        for (auto& e : es) {
            std::shared_ptr<VolView> v;
            uint64_t bt = 0;
            if (!LoadVol(CacheDir() + L"\\" + e.file, v, bt)) {
                // 片文件缺失/损坏：不能静默跳过（否则该子树永久搜不到），标记重建
                std::lock_guard<std::mutex> lk2(g_shardMu);
                auto& states = g_shardState[i];
                auto& specs = g_shards[i];
                for (int k = 0; k < (int)specs.size() && k < (int)states.size(); k++) {
                    if (specs[(size_t)k].rel == e.rel) {
                        uint64_t nowMs = GetTickCount64();
                        if (!states[(size_t)k].dirty) states[(size_t)k].firstDirtyMs = nowMs;
                        states[(size_t)k].dirty = true;
                        states[(size_t)k].dirtyTimeMs = nowMs;
                        break;
                    }
                }
                continue;
            }
            v->shardRel = e.rel;
            v->isRootShard = e.rel.empty();
            ns->push_back(v);
        }
    }
    std::sort(ns->begin(), ns->end(), [](auto& a, auto& b) {
        if (a->letter != b->letter) return a->letter < b->letter;
        return a->shardRel < b->shardRel;
    });
    g_snap.store(ns);
}


// ---- 构建后回收多余容量（P2 后每片重建体积小且频繁，压容量比预分配更重要）----
static void TrimVol(VolBuild& v) {
    if (v.recs.capacity() > v.recs.size() + v.recs.size() / 20) v.recs.shrink_to_fit();
    if (v.dirs.capacity() > v.dirs.size() + v.dirs.size() / 20) v.dirs.shrink_to_fit();
    v.nameBlob.shrink_to_fit();
    v.lowerBlob.shrink_to_fit();
    v.subtreeEnd.shrink_to_fit();
}

// 构建一个片：只遍历 rootPath 这一棵子树，跳过 prune 里的其它片根（避免父子片重复收录）。
// dirs[0] = 本片根目录的绝对路径；片根自身的目录记录归父片所有（父片负责显示/打开它）。
static void WalkShard(wchar_t letter, const std::wstring& rootPath, const std::vector<std::wstring>& pruneLower,
                      VolBuild& v, std::stop_token st) {
    v.letter = letter;
    v.ver = 2;
    v.parentPath.clear();
    std::wstring root = rootPath;
    if (root.empty() || root.back() != L'\\') root += L'\\';       // 扫描用（带尾斜杠）
    std::wstring rootNoSlash = root.substr(0, root.size() - 1);    // 显示用
    v.dirs.push_back(rootNoSlash);                                 // dirs[0] = 片根
    v.subtreeEnd.clear();
    std::vector<uint32_t> dirParent;     // 临时：目录父下标（用于算 subtreeEnd）
    dirParent.push_back(0);
    std::unordered_set<std::wstring> prune(pruneLower.begin(), pruneLower.end());
    struct Q { std::wstring path; uint32_t parent; bool isRoot; };
    std::vector<Q> stack;
    stack.push_back({ root, 0, true });
    uint64_t count = 0;
    while (!stack.empty() && !st.stop_requested()) {
        Q cur = std::move(stack.back()); stack.pop_back();
        uint32_t self;
        if (cur.isRoot) {
            self = 0;
        } else {
            self = (uint32_t)v.dirs.size();
            v.dirs.push_back(cur.path.substr(0, cur.path.size() - 1));  // 去掉尾部 '\'
            dirParent.push_back(cur.parent);
        }
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileExW((cur.path + L"*").c_str(), FindExInfoBasic, &fd,
                                    FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            const wchar_t* nm = fd.cFileName;
            if (nm[0] == L'.') { if (!wcscmp(nm, L".") || !wcscmp(nm, L"..")) continue; }
            if (!wcscmp(nm, L"System Volume Information") || !wcscmp(nm, L"$RECYCLE.BIN")) continue;
            std::wstring wname(nm);
            std::string u8 = W8(wname), u8l = W8(LowerW(wname));
            bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            Rec r;
            r.nameOff = (uint32_t)v.nameBlob.size(); r.nameLen = (uint32_t)u8.size();
            r.lowerOff = (uint32_t)v.lowerBlob.size(); r.lowerLen = (uint32_t)u8l.size();
            r.parent = self;
            r.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            r.mtime = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
            r.attrs = fd.dwFileAttributes;
            r.cat = isDir ? (uint8_t)CAT_DIR : (uint8_t)ClassifyExt(ExtOf(wname));
            v.nameBlob.append(u8); v.lowerBlob.append(u8l);
            v.recs.push_back(r);
            if (isDir) {
                std::wstring full = cur.path + nm;
                bool pruned = false;
                if (!prune.empty() && full.size() > 3) {
                    std::wstring rel = LowerW(full.substr(3));
                    pruned = prune.find(rel) != prune.end();
                }
                if (!pruned) stack.push_back({ full + L"\\", self, false });
            }
            if (++count % 20000 == 0) {
                wchar_t b[320];
                swprintf(b, 320, L"正在扫描 %ls (%llu 个)", rootNoSlash.c_str(), (unsigned long long)count);
                SetIndexStatus(b);
            }
        } while (FindNextFileW(h, &fd) && !st.stop_requested());
        FindClose(h);
    }
    // DFS preorder ⇒ 每个目录的子树是连续区间：subtreeEnd[i] = 子树右端点(exclusive)
    size_t n = v.dirs.size();
    v.subtreeEnd.assign(n, 0);
    for (size_t i = 0; i < n; i++) v.subtreeEnd[i] = (uint32_t)(i + 1);
    for (size_t i = n; i-- > 1; ) {
        uint32_t p = dirParent[i];
        if (v.subtreeEnd[i] > v.subtreeEnd[p]) v.subtreeEnd[p] = v.subtreeEnd[i];
    }
}

// ---------------- Phase1：监听 / 调度 / 重扫 ----------------

// 从磁盘上的 manifest 重载全部片（隐藏时快照已置空，窗口重新显示时调用）。
static void ReloadIndexFromCache() {
    RebuildSnapshotFromManifests();
}

// 重建一个片：只遍历该片子树 → 发布新代次文件 → 换 manifest 里对应条目（manifest 是提交点）。
// 返回 true 表示"发布 + manifest 提交"都成功；false 时调用方必须保持脏，稍后重试（不能静默丢更新）。
static bool RescanShard(wchar_t letter, int k, std::stop_token st) {
    std::lock_guard<std::mutex> lk(g_rescanMu);   // C6：同一时刻只允许一个片在重扫
    EnsureShards(letter);
    ShardSpec spec;
    {
        std::lock_guard<std::mutex> lk2(g_shardMu);
        if (k < 0 || k >= (int)g_shards[letter - 'A'].size()) return false;
        spec = g_shards[letter - 'A'][(size_t)k];
    }
    std::vector<std::wstring> prune = PruneFor(letter, k);
    std::wstring rootPath = spec.path.empty() ? (std::wstring(1, letter) + L":") : spec.path;
    StatusScope statusGuard(L"正在更新索引：" + rootPath);
    std::wstring file;
    uint64_t nRecs = 0, nDirs = 0, buildTime = 0;
    {
        // 构建对象独立作用域：出作用域先析构，之后 _heapmin 才能真把内存还给系统
        VolBuild b; b.letter = letter;
        WalkShard(letter, rootPath, prune, b, st);
        if (st.stop_requested()) return false;
        TrimVol(b);
        FILETIME f{}; GetSystemTimeAsFileTime(&f);
        buildTime = ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime;
        file = PublishShard(letter, b, buildTime);
        nRecs = b.recs.size(); nDirs = b.dirs.size();
    }
    _heapmin();
    if (file.empty()) return false;
    bool committed = false;
    uint64_t prevRecs = 0;
    bool hadEntry = false;
    {   // 换 manifest 条目（读-改-写，全部在 g_shardMu 下）
        std::lock_guard<std::mutex> lk2(g_shardMu);
        int i = letter - 'A';
        std::vector<ShardEntry> es;
        // manifest 不存在 = 首次建立（或刚被"刷新"清掉）：从空开始写，否则永远建不出 manifest
        // （曾经的 bug：清理后每片建好就因"读不到 manifest"被删，无限循环、界面一直显示正在建索引）
        if (!LoadManifest(letter, es)) {
            if (!ManifestMissing(letter)) { DeleteFileW((CacheDir() + L"\\" + file).c_str()); return false; }
            es.clear();
        }
        bool found = false;
        for (auto& e : es) {
            if (e.rel == spec.rel) {
                prevRecs = e.nRecs; hadEntry = true;
                e.path = spec.path; e.file = file;
                e.nRecs = nRecs; e.nDirs = nDirs; e.buildTime = buildTime;
                found = true; break;
            }
        }
        if (!found) es.push_back({ spec.rel, spec.path, file, nRecs, nDirs, buildTime });
        if (WriteManifest(letter, es)) {              // 只有 manifest 落盘成功才算提交
            g_manifest[i] = es;
            committed = true;
        } else {
            DeleteFileW((CacheDir() + L"\\" + file).c_str());   // 提交失败：删掉刚发布的文件，不留孤儿
        }
    }
    if (!committed) return false;
    GcGenerations();                              // 旧代次若仍被在飞结果映射，删不掉也无妨（下次启动再清）
    // 片「由非空变空」（片根被删/改名）：父片里那条指向该目录的记录会变成幽灵，标记父片重建一次。
    // 注意判据是"原来非空"，不是"本次为 0"——Documents/Downloads 这类本来就空的片不能每次变化都放大成父片重建。
    if (nRecs == 0 && hadEntry && prevRecs > 0 && !spec.rel.empty()) {
        size_t bs = spec.rel.rfind(L'\\');
        std::wstring parentRel = (bs == std::wstring::npos) ? std::wstring() : spec.rel.substr(0, bs);
        int pk = ShardOfPath(letter, parentRel);
        std::lock_guard<std::mutex> lk2(g_shardMu);
        auto& states = g_shardState[letter - 'A'];
        if (pk >= 0 && pk < (int)states.size()) {
            uint64_t nowMs = GetTickCount64();
            if (!states[(size_t)pk].dirty) states[(size_t)pk].firstDirtyMs = nowMs;
            states[(size_t)pk].dirty = true;
            states[(size_t)pk].dirtyTimeMs = nowMs;
            ++states[(size_t)pk].dirtySeq;
        }
    }
    if (g_visible.load()) {
        RebuildSnapshotFromManifests();           // 16 个 mmap ≈ 1–2ms
        if (g_hwnd) PostMessageW(g_hwnd, WM_APP_RESCAN_DONE, 0, 0);
    }
    return true;
}

// 每盘一个同步监听线程：只标脏，绝不在此重扫。
static DWORD WINAPI ListenerProc(LPVOID param) {
    int idx = (int)(INT_PTR)param;
    wchar_t letter = (wchar_t)('A' + idx);
    wchar_t root[4] = { letter, L':', L'\\', 0 };
    HANDLE self = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                         &self, THREAD_TERMINATE, FALSE, 0)) self = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_listenMu);
        g_listenHandle[idx] = self;
    }
    HANDLE hDir = CreateFileW(root, FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (hDir != INVALID_HANDLE_VALUE) {
        // 忽略本程序自身 data 目录（idx/config 写入）造成的变更，否则"写 idx → 触发重扫 → 再写 idx"会死循环。
        std::wstring ignore;
        {
            std::wstring data = DataRoot();
            if (data.size() >= 3 && towupper(data[0]) == towupper(letter) && data[1] == L':') {
                ignore = data.substr(3);
                for (auto& c : ignore) c = (wchar_t)towlower(c);
            }
        }
        std::vector<BYTE> buf(64 * 1024);
        for (;;) {
            DWORD bytes = 0;
            BOOL ok = ReadDirectoryChangesW(hDir, buf.data(), (DWORD)buf.size(), TRUE,
                // 只关心"增/删/改名"；不监听 SIZE/LAST_WRITE——浏览器等会持续写已有文件，
                // 若监听内容写入会导致"写文件→整卷重扫→再写→再扫"永远停不下来。
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME,
                &bytes, nullptr, nullptr);
            if (!ok) {
                DWORD err = GetLastError();
                if (err == ERROR_OPERATION_ABORTED) break;   // 被 CancelSynchronousIo 打断
                EnsureShards(letter);                         // 其他错误：整盘片标脏后退出，由调度兜底
                {
                    std::lock_guard<std::mutex> lk2(g_shardMu);
                    uint64_t nowMs = GetTickCount64();
                    for (auto& s2 : g_shardState[idx]) { s2.dirty = true; s2.dirtyTimeMs = nowMs; ++s2.dirtySeq; }
                }
                break;
            }
            bool any = false;
            std::vector<std::wstring> paths;   // 本次事件涉及的相对路径（小写）
            if (bytes == 0) {
                any = true;                                  // 溢出：无法判断路径，保守标脏
            } else {
                BYTE* p = buf.data();
                for (;;) {
                    auto* fni = (FILE_NOTIFY_INFORMATION*)p;
                    std::wstring path(fni->FileName, fni->FileNameLength / sizeof(WCHAR));
                    for (auto& c : path) c = (wchar_t)towlower(c);
                    // 忽略本程序自身 data 目录（idx/manifest 写入），但必须按路径边界比较，
                    // 否则 "...\data2" 这类同前缀目录会被误忽略。
                    bool self = false;
                    if (!ignore.empty() && path.size() >= ignore.size() &&
                        path.compare(0, ignore.size(), ignore) == 0 &&
                        (path.size() == ignore.size() || path[ignore.size()] == L'\\')) self = true;
                    if (!self) { any = true; paths.push_back(path); }
                    if (fni->NextEntryOffset == 0) break;
                    p += fni->NextEntryOffset;
                }
            }
            if (any) {
                // P2：按变化路径落到具体片，只给那一片标脏（热片 30 秒就能重扫完）
                EnsureShards(letter);
                uint64_t nowMs = GetTickCount64();
                std::lock_guard<std::mutex> lk2(g_shardMu);
                auto& states = g_shardState[idx];
                if (states.empty()) continue;
                if (paths.empty()) {                      // 缓冲区溢出：整盘片全部标脏
                    for (auto& s2 : states) {
                        if (!s2.dirty) s2.firstDirtyMs = nowMs;
                        s2.dirty = true; s2.dirtyTimeMs = nowMs; ++s2.dirtySeq;
                    }
                } else {
                    for (auto& rel : paths) {
                        // 用"变化项自身的路径"做最长前缀匹配：文件落在其所属片；
                        // 若变化的正是某个片根目录（被删/改名），也能命中该片，从而让该片重建（可能变成空片）。
                        int k2 = ShardOfPath(letter, rel);
                        if (k2 < 0 || k2 >= (int)states.size()) continue;
                        auto& st2 = states[(size_t)k2];
                        if (!st2.dirty) st2.firstDirtyMs = nowMs;   // 只在"从干净变脏"时记首变时间
                        st2.dirty = true;
                        st2.dirtyTimeMs = nowMs;
                        ++st2.dirtySeq;
                    }
                }
            }
        }
        CloseHandle(hDir);
    }
    {
        std::lock_guard<std::mutex> lk(g_listenMu);
        if (g_listenHandle[idx] == self) g_listenHandle[idx] = nullptr;
    }
    if (self) CloseHandle(self);
    return 0;
}

// 全局唯一调度线程：按片独立计时——热片 30 秒、冷片 5 分钟（防高频变更把重扫打成死循环）。
static void SchedulerThread(std::stop_token st) {
    while (!st.stop_requested()) {
        uint64_t now = GetTickCount64();
        DWORD drives = GetLogicalDrives();
        for (int i = 0; i < 26; i++) {
            if (st.stop_requested()) return;
            if (!(drives & (1u << i))) continue;
            wchar_t letter = (wchar_t)('A' + i);
            wchar_t root[4] = { letter, L':', L'\\', 0 };
            if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
            EnsureShards(letter);
            int done = -1;
            uint32_t doneSeq = 0;
            {
                std::lock_guard<std::mutex> lk2(g_shardMu);
                auto& states = g_shardState[i];
                const auto& specs = g_shards[i];
                for (int k = 0; k < (int)states.size(); k++) {
                    auto& vs = states[(size_t)k];
                    if (!vs.dirty || vs.busy) continue;
                    // 防抖：静了 8s 才扫；但持续 churn 时不能无限等，首变超过 30s 也强扫一次
                    bool quiet = (now - vs.dirtyTimeMs >= DEBOUNCE_MS);
                    bool overdue = (vs.firstDirtyMs != 0 && now - vs.firstDirtyMs >= MAX_DEBOUNCE_WAIT_MS);
                    if (!quiet && !overdue) continue;
                    uint64_t floorMs = specs[(size_t)k].hot ? HOT_MIN_INTERVAL_MS : COLD_MIN_INTERVAL_MS;
                    if (now - vs.lastRescanMs < floorMs) continue;
                    vs.busy = true;
                    vs.lastRescanMs = now;
                    done = k;
                    doneSeq = vs.dirtySeq;       // 记下开始重扫时的变化序号
                    break;                       // 一次只重扫一片（串行，C6）
                }
            }
            if (done >= 0) {
                bool okr = RescanShard(letter, done, st);
                std::lock_guard<std::mutex> lk2(g_shardMu);
                auto& vs = g_shardState[i][(size_t)done];
                vs.busy = false;
                // 只有"重扫期间没有新事件"且成功时才清脏；否则保留脏标记，让下一轮再补一次
                if (okr && vs.dirtySeq == doneSeq) {
                    vs.dirty = false;
                } else {
                    vs.dirtyTimeMs = GetTickCount64();
                    vs.firstDirtyMs = vs.dirtyTimeMs;
                    ++vs.dirtySeq;               // 重置序号，便于下一轮判定
                }
            }
        }
        for (int k = 0; k < 5 && !st.stop_requested(); k++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// 显式退出：停监听（CancelSynchronousIo）→ join → DestroyWindow（→WM_DESTROY→PostQuitMessage）
static void BeginExit(HWND h) {
    {
        std::lock_guard<std::mutex> lk(g_listenMu);
        for (int i = 0; i < 26; i++)
            if (g_listenHandle[i]) CancelSynchronousIo(g_listenHandle[i]);
    }
    for (int i = 0; i < 26; i++) {
        if (g_listenThreadH[i]) {
            WaitForSingleObject(g_listenThreadH[i], 3000);
            CloseHandle(g_listenThreadH[i]);
            g_listenThreadH[i] = nullptr;
        }
    }
    SaveConfig();
    DestroyWindow(h);
}

// 手动刷新（状态栏「刷新」按钮 / F5）：删掉所有卷的 idx 缓存，再让索引线程
// 重新扫一遍。索引线程跑完一轮会挂在这里等，刷新信号一来就重来。
static std::mutex              g_refreshMu;
static std::condition_variable g_refreshCv;
static int                     g_refreshSeq = 0;

static void ClearIndexCache() {
    // 代次文件 vol_X.gN.idx、manifest、旧单文件、残留 tmp 一并清掉。
    // 注意：被 mmap 的旧代次这里删不掉（设计如此），启动期 GcGenerations 会兜底清理。
    for (const wchar_t* pat : { L"\\vol_*.idx", L"\\vol_*.manifest", L"\\vol_*.cur", L"\\vol_*.tmp" }) {
        WIN32_FIND_DATAW fd{};
        std::wstring p = CacheDir() + pat;
        HANDLE hFind = FindFirstFileW(p.c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) continue;
        do { DeleteFileW((CacheDir() + L"\\" + std::wstring(fd.cFileName)).c_str()); }
        while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
    std::lock_guard<std::mutex> lk2(g_shardMu);
    for (int i = 0; i < 26; i++) g_manifest[i].clear();
    ++g_cacheEpoch;   // 让索引线程放弃正在进行的构建，回到外层重新开始
}

static void IndexerThread(std::stop_token st) {
    // ensure dirs
    CreateDirectoryW(DataRoot().c_str(), nullptr);
    CreateDirectoryW(CacheDir().c_str(), nullptr);
    GcGenerations();   // 此刻没有视图存活：清旧代次 / 旧单文件 / 残留 tmp
    for (;;) {
        int mySeq;
        {
            std::lock_guard<std::mutex> lk(g_refreshMu);
            mySeq = g_refreshSeq;
        }
        DWORD drives = GetLogicalDrives();
        // 升序处理盘符：C 盘（系统盘）最先就绪，搜索可尽快出结果
        for (int i = 0; i < 26; i++) {
            if (!(drives & (1u << i))) continue;
            if (st.stop_requested()) return;
            wchar_t letter = (wchar_t)('A' + i);
            wchar_t root[4] = { letter, L':', L'\\', 0 };
            if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
            EnsureShards(letter);
            // 先按现有 manifest 出结果（分片已存在就直接用，不再全量扫）
            std::vector<ShardSpec> specs;
            {
                std::lock_guard<std::mutex> lk2(g_shardMu);
                specs = g_shards[i];
            }
            std::vector<ShardEntry> es;
            bool haveMf = LoadManifest(letter, es);
            {
                std::lock_guard<std::mutex> lk2(g_shardMu);
                g_manifest[i] = es;
            }
            FILETIME ft{}; GetSystemTimeAsFileTime(&ft);
            uint64_t nowTicks = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
            // 逐片判定"可复用"：文件在 + buildTime 在 24h 内 + manifest 里有条目。
            // 缺任何一条（例如上次建索引中途退出、漏了一片）都要重建——否则该子树会永久搜不到。
            auto needBuild = [&](const ShardSpec& s) -> bool {
                if (!haveMf) return true;
                for (auto& e : es) {
                    if (e.rel != s.rel) continue;
                    if (GetFileAttributesW((CacheDir() + L"\\" + e.file).c_str()) == INVALID_FILE_ATTRIBUTES) return true;
                    if (!(e.buildTime > 0 && nowTicks - e.buildTime < STARTUP_STALE_TICKS)) return true;
                    return false;
                }
                return true;
            };
            bool anyNeed = false;
            for (auto& s : specs) if (needBuild(s)) { anyNeed = true; break; }
            if (!anyNeed) { RebuildSnapshotFromManifests(); continue; }
            // 逐片构建，每片落盘即发布，边建边可用
            std::lock_guard<std::mutex> rl(g_rescanMu);   // C6：与调度线程串行化
            const uint32_t myEpoch = g_cacheEpoch.load();
            for (int k = 0; k < (int)specs.size(); k++) {
                if (st.stop_requested()) return;
                if (g_cacheEpoch.load() != myEpoch) break;   // 用户按了刷新：放弃本轮，回外层重来
                const ShardSpec spec = specs[(size_t)k];
                if (!needBuild(spec)) continue;
                std::vector<std::wstring> prune = PruneFor(letter, k);
                std::wstring rootPath = spec.path.empty() ? (std::wstring(1, letter) + L":") : spec.path;
                std::wstring file;
                uint64_t nRecs = 0, nDirs = 0, buildTime = 0;
                {
                    VolBuild b; b.letter = letter;
                    WalkShard(letter, rootPath, prune, b, st);
                    if (st.stop_requested()) return;
                    TrimVol(b);
                    FILETIME f{}; GetSystemTimeAsFileTime(&f);
                    buildTime = ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime;
                    file = PublishShard(letter, b, buildTime);
                    nRecs = b.recs.size(); nDirs = b.dirs.size();
                }
                _heapmin();   // 每片建完就压实：峰值只跟单片大小有关
                if (file.empty()) continue;
                {   // 以磁盘 manifest 为准做读-改-写（不在内存里长期持有引用，避免刷新时被清空）
                    std::lock_guard<std::mutex> lk2(g_shardMu);
                    std::vector<ShardEntry> cur;
                    if (!LoadManifest(letter, cur)) {
                        // 文件在但读不出 → 放弃本次提交（避免覆盖别人的提交）；文件不存在 → 从空开始建
                        if (!ManifestMissing(letter)) { DeleteFileW((CacheDir() + L"\\" + file).c_str()); continue; }
                        cur.clear();
                    }
                    bool found = false;
                    for (auto& e : cur) {
                        if (e.rel == spec.rel) {
                            e.path = spec.path; e.file = file;
                            e.nRecs = nRecs; e.nDirs = nDirs; e.buildTime = buildTime;
                            found = true; break;
                        }
                    }
                    if (!found) cur.push_back({ spec.rel, spec.path, file, nRecs, nDirs, buildTime });
                    if (WriteManifest(letter, cur)) g_manifest[i] = cur;
                    else DeleteFileW((CacheDir() + L"\\" + file).c_str());   // 提交失败：不留孤儿
                }
                RebuildSnapshotFromManifests();   // 这一片立即可搜
                wchar_t sb[96];
                swprintf(sb, 96, L"正在建立分片索引：%c: %d/%d",
                         (wchar_t)letter, k + 1, (int)specs.size());
                SetIndexStatus(sb);
            }
            GcGenerations();
            SetIndexStatus(L"索引已更新：" + std::wstring(1, letter) + L":");
            if (g_curEpoch > 0) RequestSearch();   // re-run with fresh data
        }
        SetIndexStatus(L"索引已就绪");

        // 等待下一次「手动刷新」或退出；若长时间无操作，30 分钟自动后台刷新一次
        //（长开程序也能自动更新，无需手动点刷新）。
        std::unique_lock<std::mutex> lk(g_refreshMu);
        // 周期重扫已交给 SchedulerThread；此处只等「手动刷新」或退出。
        g_refreshCv.wait(lk, [&] { return g_refreshSeq != mySeq || st.stop_requested(); });
        if (st.stop_requested()) return;
    }
}

// UI 线程调用：清缓存 + 通知索引线程重扫（按钮和 F5 都走这里）
static void DoRefresh() {
    SetIndexStatus(L"正在刷新索引…");
    ClearIndexCache();
    {
        std::lock_guard<std::mutex> lk(g_refreshMu);
        ++g_refreshSeq;
    }
    g_refreshCv.notify_all();
}
static void RequestSearch() {
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP + 9, 0, 0);   // triggers DoSearch on UI thread
}

// ---------------- search thread ----------------
struct Res {
    std::wstring name;
    std::shared_ptr<const VolView> owner;   // 结果持有 mmap 视图，路径按需解析
    uint32_t dirId = 0;
    uint64_t size = 0, mtime = 0;
    uint8_t cat = CAT_OTHER;
};
static std::wstring ResDir(const Res& r) {
    if (!r.owner) return {};
    return VolDirAt(*r.owner, r.dirId);
}
struct DoneMsg {
    uint64_t epoch;
    std::vector<Res>* items;
    uint64_t total;
    uint32_t cat[CAT_COUNT];
};
struct Req {
    uint64_t epoch = 0;
    std::wstring kw;
    wchar_t scope = 0;   // 0 = 此电脑
    std::wstring scopeDir;   // 非空 = 仅搜索该目录（含子目录视 sub 而定）
    bool sub = true;
    bool pending = false;
};
static std::mutex g_reqMtx;
static std::condition_variable_any g_reqCv;
static Req g_req;
static std::atomic<uint64_t> g_epoch{ 0 };

static void SearcherThread(std::stop_token st) {
    for (;;) {
        Req r;
        {
            std::unique_lock<std::mutex> lk(g_reqMtx);
            g_reqCv.wait(lk, st, [&] { return g_req.pending || st.stop_requested(); });
            if (st.stop_requested() || !g_req.pending) break;
            r = g_req; g_req.pending = false;
        }
        auto snap = g_snap.load();
        std::wstring kw = r.kw;
        // split keywords
        std::vector<std::wstring> parts;
        {
            std::wstring cur;
            for (wchar_t c : kw) {
                if (c == L' ' || c == L'\t') { if (!cur.empty()) { parts.push_back(LowerW(cur)); cur.clear(); } }
                else cur.push_back(c);
            }
            if (!cur.empty()) parts.push_back(LowerW(cur));
        }
        std::vector<std::string> kw8;
        for (auto& p : parts) kw8.push_back(W8(p));
        auto* done = new DoneMsg{};
        done->epoch = r.epoch;
        done->items = new std::vector<Res>();
        done->total = 0;
        memset(done->cat, 0, sizeof done->cat);
        if (!kw8.empty() && snap) {
            for (auto& vol : *snap) {
                if (r.scope && vol->letter != r.scope) continue;
                const uint8_t* nb = vol->nameBlob;   // mmap 裸字节
                const uint8_t* lb = vol->lowerBlob;
                // 目录范围过滤：一次性预算每个目录是否在范围内（避免逐记录解码路径）
                std::vector<uint8_t> allowed, eqScope;
                if (!r.scopeDir.empty()) {
                    allowed.assign((size_t)vol->nDirs, 0);
                    eqScope.assign((size_t)vol->nDirs, 0);
                    for (uint64_t d = 0; d < vol->nDirs; d++) {
                        std::wstring dd = VolDirAt(*vol, (size_t)d);
                        bool inside = dd == r.scopeDir ||
                            (dd.size() > r.scopeDir.size() &&
                             wcsncmp(dd.c_str(), r.scopeDir.c_str(), r.scopeDir.size()) == 0 &&
                             dd[r.scopeDir.size()] == L'\\');
                        allowed[(size_t)d] = inside ? 1 : 0;
                        eqScope[(size_t)d] = (dd == r.scopeDir) ? 1 : 0;
                    }
                }
                for (uint64_t i = 0; i < vol->nRecs; i++) {
                    const uint8_t* rp = vol->recs + (size_t)i * sizeof(Rec);
                    uint32_t nameOff = RdU32(rp), nameLen = RdU32(rp + 4);
                    uint32_t lowerOff = RdU32(rp + 8), lowerLen = RdU32(rp + 12);
                    uint32_t parent = RdU32(rp + 16);
                    uint64_t sz = RdU64(rp + 20), mt = RdU64(rp + 28);
                    uint8_t cat = rp[40];
                    if (!r.scopeDir.empty()) {
                        if (parent >= vol->nDirs || !allowed[parent]) continue;
                        if (!r.sub && !eqScope[parent]) continue;
                    } else if (!r.sub) {
                        // P2：「仅显示顶层」= 卷根片里 parent==0 的记录（各盘一级目录）。
                        // 热区片/Windows 片的根目录本身仍由卷根片的记录显示，不重复。
                        if (!vol->isRootShard || parent != 0) continue;
                    }
                    std::string_view low((const char*)lb + lowerOff, lowerLen);
                    bool ok = true;
                    for (auto& k : kw8)
                        if (low.find(k) == std::string_view::npos) { ok = false; break; }
                    if (!ok) continue;
                    done->total++;
                    done->cat[cat]++;
                    if (done->items->size() >= MAX_STORED) continue;
                    Res it;
                    it.name = U8(std::string((const char*)nb + nameOff, nameLen));
                    it.owner = vol;
                    it.dirId = parent;
                    it.size = sz; it.mtime = mt;
                    it.cat = cat;
                    done->items->push_back(std::move(it));
                }
            }
        }
        done->cat[CAT_ALL] = (uint32_t)done->total;   // 需求3：全部 = 各分类之和
        PostMessageW(g_hwnd, WM_APP_SEARCHDONE, 0, (LPARAM)done);
    }
}

// ============================================================
// 3. UI state
// ============================================================
struct Layout {
    RECT title{}, edit{}, btn{}, scope{}, chk{}, chkBox{}, main{}, cat{}, list{}, header{},
        prev{}, status{}, split1{}, split2{}, vscroll{}, refresh{};
    RECT bcSeg[3]{};           // 面包屑段：0=此电脑 1=盘 2=子目录（需求1）
    int  bcSegN = 0;
    int nameX = 0, pathX = 0, sizeX = 0, timeX = 0;
    int nameW = 0, pathW = 0, sizeW = 0, timeW = 0;
    int headerH = 0, rowH = 0, catItemH = 0, listBodyY = 0;
};
static Layout gL;

// config-persisted
static int  g_catW = 190, g_prevW = 310, g_nameW = 260, g_pathW = 300;
static int  g_sizeW = 100;                 // 大小列宽（可拖动，需求6）
static int  g_sortCol = 0; static bool g_sortAsc = true;
static bool g_subfolders = true;
static wchar_t g_scopeLetter = 0;          // 0 = 此电脑
static std::wstring g_scopeDir;            // 子目录范围（面包屑第三段），空 = 整盘
static bool g_maximized = false;
static WINDOWPLACEMENT g_wp{ sizeof(WINDOWPLACEMENT) };

// runtime
static std::vector<Res> g_results;
static std::vector<int> g_view;
static uint32_t g_catCount[CAT_COUNT] = {};
static uint64_t g_totalFound = 0;
static int  g_curCat = CAT_ALL;
// 格式筛选（第四轮需求4/5）：按"具体扩展名"筛选。空 = 不过滤（等效全选）。
// 勾选状态跨"重开下拉/切换分类/重新搜索"记忆；切换分类后下拉只列该分类下实际存在的格式
static std::vector<std::wstring> g_typeSel;
static std::vector<std::wstring> g_resultKeys;   // 与 g_results 平行的扩展名 key 缓存（筛选勾满卡顿优化）
// 结果条目的格式 key：文件夹 → "文件夹"；无扩展名 → "(无扩展名)"；其余 → 大写扩展名
static std::wstring ExtKeyOf(const Res& r) {
    if (r.cat == CAT_DIR) return L"文件夹";
    size_t p = r.name.find_last_of(L'.');
    if (p == std::wstring::npos || p + 1 >= r.name.size()) return L"(无扩展名)";
    std::wstring e = r.name.substr(p + 1);
    for (auto& c : e) c = (wchar_t)toupper(c);
    return e;
}
static int  g_sel = -1;                    // index into g_view
static int  g_dblRow = -1;                 // 手动双击检测：上一次左键按下命中行号
static DWORD g_dblTime = 0;                // 手动双击检测：上一次左键按下的消息时间
static int  g_scroll = 0;                  // first visible row
static std::wstring g_statusL, g_statusR;
static bool g_searching = false;

// fonts
static HFONT g_fTitle = nullptr, g_fUI = nullptr, g_fList = nullptr, g_fBig = nullptr,
             g_fBold = nullptr, g_fSmall = nullptr, g_fBadge = nullptr;
static void MakeFonts() {
    auto mk = [&](int pt, int weight) {
        LOGFONTW lf{};
        lf.lfHeight = -MulDiv(pt, g_dpi, 72);
        lf.lfWeight = weight;
        lf.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(lf.lfFaceName, L"Microsoft YaHei UI");
        return CreateFontIndirectW(&lf);
    };
    // 列表/分类/列头字号放大两个号（需求12：原10pt→12pt），并保证垂直居中(DT_VCENTER)
    // 搜索框字号再放大一号（需求8：13→14pt）
    g_fTitle = mk(13, 600);
    g_fUI    = mk(11, 400);
    g_fList  = mk(12, 400);
    g_fBig   = mk(14, 400);
    g_fBold  = mk(12, 600);
    g_fSmall = mk(10, 400);
    g_fBadge = mk(14, 700);
}

// hover / drag state
static int g_hoverZone = 0;
static int g_hoverIdx = -1;         // cat item / row
static int g_hoverTitle = 0;        // 1 min 2 max 3 close
static bool g_btnDown = false;
static int  g_downZone = 0, g_downIdx = -1;
// drags
static int g_drag = 0;              // 1 splitter1 2 splitter2 3 cat-reorder 4 col 5 vscroll
static int g_dragOff = 0;
static int g_catDragFrom = -1, g_catDragTo = -1;
static bool g_catDragActive = false;
static int g_colDrag = -1;
static double g_vscrollAnchor = 0;

// ---------------- zones ----------------
enum Zone {
    Z_NONE = 0, Z_TITLE, Z_MIN, Z_MAX, Z_CLOSE, Z_EDIT, Z_BTN, Z_SCOPE, Z_CHK,
    Z_CAT, Z_SPLIT1, Z_LIST, Z_HEADER, Z_SPLIT2, Z_PREVIEW, Z_OPEN, Z_OPENF, Z_STATUS,
    Z_VSCROLL, Z_REFRESH
};
static int PtInRC(const RECT& r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}
static int ZoneAt(int x, int y) {
    if (PtInRC(gL.title, x, y)) {
        if (x > gL.title.right - S(46) * 3 - 8) {
            if (x > gL.title.right - S(46)) return Z_CLOSE;
            if (x > gL.title.right - S(46) * 2) return Z_MAX;
            return Z_MIN;
        }
        return Z_TITLE;
    }
    if (PtInRC(gL.btn, x, y)) return Z_BTN;
    if (PtInRC(gL.edit, x, y)) return Z_EDIT;
    if (PtInRC(gL.scope, x, y)) return Z_SCOPE;
    if (PtInRC(gL.chk, x, y)) return Z_CHK;
    if (PtInRC(gL.split1, x, y)) return Z_SPLIT1;
    if (PtInRC(gL.split2, x, y)) return Z_SPLIT2;
    if (PtInRC(gL.header, x, y)) return Z_HEADER;
    if (gL.vscroll.right > gL.vscroll.left && PtInRC(gL.vscroll, x, y)) return Z_VSCROLL;
    if (PtInRC(gL.cat, x, y)) return Z_CAT;
    if (PtInRC(gL.list, x, y)) return Z_LIST;
    if (PtInRC(gL.prev, x, y)) {
        // open buttons
        RECT bo = PreviewOpenRect(), bf = PreviewOpenFRect();
        if (PtInRC(bo, x, y)) return Z_OPEN;
        if (PtInRC(bf, x, y)) return Z_OPENF;
        return Z_PREVIEW;
    }
    if (gL.refresh.right > gL.refresh.left && PtInRC(gL.refresh, x, y)) return Z_REFRESH;
    if (PtInRC(gL.status, x, y)) return Z_STATUS;
    return Z_NONE;
}
// 需求6：按钮宽度按文字实测自适应（统一左右内边距），避免"框大小不适配字数"
static int TextBtnW(const wchar_t* txt) {
    HDC dc = GetDC(g_hwnd);
    SIZE sz{};
    HFONT of = (HFONT)SelectObject(dc, g_fList);
    GetTextExtentPoint32W(dc, txt, (int)wcslen(txt), &sz);
    SelectObject(dc, of);
    ReleaseDC(g_hwnd, dc);
    return sz.cx + S(44);
}
// 需求3（第四轮）：底部两按钮"左边距 = 按钮间距 = 右边距"，随面板宽度自适应始终相等
static void PreviewBtnLayout(RECT& bf, RECT& bo) {
    int wF = TextBtnW(L"打开目录"), wO = TextBtnW(L"打开"), h = S(34);
    int total = gL.prev.right - gL.prev.left;
    int gap = (total - wF - wO) / 3;
    if (gap < S(8)) gap = S(8);
    int y = gL.prev.bottom - S(16) - h;
    int xF = gL.prev.left + gap;
    bf = { xF, y, xF + wF, y + h };
    int xO = bf.right + gap;
    bo = { xO, y, xO + wO, y + h };
}
static RECT PreviewOpenRect() {
    RECT bf, bo; PreviewBtnLayout(bf, bo); return bo;
}
static RECT PreviewOpenFRect() {
    RECT bf, bo; PreviewBtnLayout(bf, bo); return bf;
}

// ============================================================
// 4. config
// ============================================================
static std::wstring ConfigPath() { return DataRoot() + L"\\config.ini"; }
static int CfgInt(const wchar_t* k, int def) {
    return GetPrivateProfileIntW(L"ui", k, def, ConfigPath().c_str());
}
static void CfgSet(const wchar_t* k, int v) {
    wchar_t b[32]; swprintf(b, 32, L"%d", v);
    WritePrivateProfileStringW(L"ui", k, b, ConfigPath().c_str());
}
static void LoadConfig() {
    g_catW = CfgInt(L"catw", 190);
    g_prevW = CfgInt(L"prevw", 310);
    g_nameW = CfgInt(L"namew", 260);
    g_pathW = CfgInt(L"pathw", 300);
    if (g_pathW < 120) g_pathW = 120; if (g_pathW > 1200) g_pathW = 1200;
    g_sizeW = CfgInt(L"sizew", 100);
    if (g_sizeW < 56) g_sizeW = 56; if (g_sizeW > 300) g_sizeW = 300;
    g_sortCol = CfgInt(L"sortcol", 0);
    g_sortAsc = CfgInt(L"sortasc", 1) != 0;
    g_subfolders = CfgInt(L"subfolders", 1) != 0;
    g_scopeLetter = (wchar_t)CfgInt(L"scope", 0);
    wchar_t sd[MAX_PATH + 4] = {};
    GetPrivateProfileStringW(L"ui", L"scopedir", L"", sd, MAX_PATH + 4, ConfigPath().c_str());
    g_scopeDir = sd;
    if (!g_scopeDir.empty() && g_scopeDir.size() >= 2)
        g_scopeLetter = g_scopeDir[0];
    int x = CfgInt(L"x", -1), y = CfgInt(L"y", -1), w = CfgInt(L"w", 0), h = CfgInt(L"h", 0);
    g_maximized = CfgInt(L"max", 0) != 0;
    g_wp.showCmd = g_maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    if (w > 400 && h > 300) {
        g_wp.rcNormalPosition = { x, y, x + w, y + h };
    }
    wchar_t buf[128] = {};
    GetPrivateProfileStringW(L"ui", L"catorder", L"0,1,2,3,4,5,6,7,8,9", buf, 128, ConfigPath().c_str());
    g_catOrder.clear();
    wchar_t* ctx = nullptr;
    wchar_t* tok = wcstok_s(buf, L",", &ctx);
    while (tok) { g_catOrder.push_back(_wtoi(tok)); tok = wcstok_s(nullptr, L",", &ctx); }
    if (g_catOrder.size() != CAT_COUNT) g_catOrder = { 0,1,2,3,4,5,6,7,8,9 };
}
static void SaveConfig() {
    if (!g_hwnd) return;
    WINDOWPLACEMENT wp{ sizeof(wp) };
    GetWindowPlacement(g_hwnd, &wp);
    RECT r = wp.rcNormalPosition;
    CfgSet(L"x", r.left); CfgSet(L"y", r.top);
    CfgSet(L"w", r.right - r.left); CfgSet(L"h", r.bottom - r.top);
    CfgSet(L"max", wp.showCmd == SW_SHOWMAXIMIZED ? 1 : 0);
    CfgSet(L"catw", g_catW); CfgSet(L"prevw", g_prevW); CfgSet(L"namew", g_nameW);
    CfgSet(L"pathw", g_pathW);
    CfgSet(L"sizew", g_sizeW);
    CfgSet(L"sortcol", g_sortCol); CfgSet(L"sortasc", g_sortAsc ? 1 : 0);
    CfgSet(L"subfolders", g_subfolders ? 1 : 0);
    CfgSet(L"scope", (int)g_scopeLetter);
    WritePrivateProfileStringW(L"ui", L"scopedir", g_scopeDir.c_str(), ConfigPath().c_str());
    std::wstring o;
    for (size_t i = 0; i < g_catOrder.size(); i++) {
        if (i) o += L",";
        o += std::to_wstring(g_catOrder[i]);
    }
    WritePrivateProfileStringW(L"ui", L"catorder", o.c_str(), ConfigPath().c_str());
}

// ============================================================
// 5. results handling
// ============================================================
static int CmpResults(const Res& a, const Res& b) {
    int c = 0;
    switch (g_sortCol) {
    case 0: c = CompareStringEx(LOCALE_NAME_INVARIANT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS,
                                a.name.c_str(), (int)a.name.size(), b.name.c_str(), (int)b.name.size(), nullptr, nullptr, 0) - 2; break;
    case 1: {
        std::wstring ad = ResDir(a), bd = ResDir(b);
        c = CompareStringEx(LOCALE_NAME_INVARIANT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS,
                            ad.c_str(), (int)ad.size(), bd.c_str(), (int)bd.size(), nullptr, nullptr, 0) - 2;
    } break;
    case 2: c = a.size < b.size ? -1 : a.size > b.size ? 1 : 0; break;
    case 3: c = a.mtime < b.mtime ? -1 : a.mtime > b.mtime ? 1 : 0; break;
    }
    if (!g_sortAsc) c = -c;
    return c;
}

// ---- 名称排序（用户指定优先级：字母 → 数字 → 中文拼音 → 符号）----
// ★ 两个坑，别再踩：
//   1) CmpResults 返回三态 int，**不能**直接当 std::sort 的 bool 谓词 —— -1 和
//      +1 都会被转成 true，比较器非法，结果彻底乱序。必须包 `< 0`。
//   2) CompareStringEx(..., SORT_DIGITSASNUMBERS) **不满足传递性**，同样不能
//      直接当排序谓词。这里改成「预生成排序键 + 逐字节 memcmp」。
static int NameBucket(wchar_t c) {
    if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z')) return 0;   // 英文字母
    if (c >= L'0' && c <= L'9') return 1;                                // 数字
    if ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) ||
        (c >= 0xF900 && c <= 0xFAFF)) return 2;                          // 汉字（按拼音）
    return 3;                                                            // 符号等
}
// 数字串补零成 10 位定宽，让字典序等价于数值序（2 排在 12 前面）
static std::wstring PadNumberRuns(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + 8);
    size_t i = 0;
    while (i < s.size()) {
        const wchar_t c = s[i];
        if (c >= L'0' && c <= L'9') {
            size_t j = i;
            while (j < s.size() && s[j] >= L'0' && s[j] <= L'9') ++j;
            std::wstring digits = s.substr(i, j - i);
            size_t k = 0;
            while (k + 1 < digits.size() && digits[k] == L'0') ++k;
            std::wstring num = digits.substr(k);
            if (num.size() > 10) num.resize(10);
            out.append(10 - num.size(), L'0');
            out += num;
            i = j;
        } else { out.push_back(c); ++i; }
    }
    return out;
}
struct NameKey {
    unsigned char bucket = 3;
    std::vector<BYTE> bytes;
};
static void BuildNameKey(const std::wstring& raw, NameKey& k) {
    k.bucket = raw.empty() ? 3 : (unsigned char)NameBucket(raw[0]);
    const std::wstring s = PadNumberRuns(raw);
    k.bytes.clear();
    static const wchar_t* kLocales[] = { L"zh-CN", LOCALE_NAME_USER_DEFAULT };
    for (const wchar_t* loc : kLocales) {
        int need = LCMapStringEx(loc, LCMAP_SORTKEY | LINGUISTIC_IGNORECASE,
                                 s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr, 0);
        if (need <= 0) continue;
        k.bytes.resize((size_t)need);
        int got = LCMapStringEx(loc, LCMAP_SORTKEY | LINGUISTIC_IGNORECASE,
                                s.c_str(), (int)s.size(),
                                (LPWSTR)k.bytes.data(), need, nullptr, nullptr, 0);
        if (got > 0) { k.bytes.resize((size_t)got); return; }
        k.bytes.clear();
    }
    for (wchar_t c : s) {   // 兜底：小写后按 UTF-16 小端比，仍然可传递
        const wchar_t lo = (wchar_t)towlower(c);
        k.bytes.push_back((BYTE)(lo & 0xFF));
        k.bytes.push_back((BYTE)((lo >> 8) & 0xFF));
    }
}
static int CmpNameKey(const NameKey& a, const NameKey& b) {
    if (a.bucket != b.bucket) return a.bucket < b.bucket ? -1 : 1;
    const size_t n = a.bytes.size() < b.bytes.size() ? a.bytes.size() : b.bytes.size();
    if (n) { int r = memcmp(a.bytes.data(), b.bytes.data(), n); if (r) return r < 0 ? -1 : 1; }
    if (a.bytes.size() != b.bytes.size()) return a.bytes.size() < b.bytes.size() ? -1 : 1;
    return 0;
}

static void SortResults() {
    // 名称列：排序键 + 下标排序；其余列是纯数值比较，包一层 < 0 直接排。
    if (g_sortCol != 0) {
        std::stable_sort(g_results.begin(), g_results.end(),
                         [](const Res& a, const Res& b) { return CmpResults(a, b) < 0; });
        return;
    }
    const size_t n = g_results.size();
    if (n < 2) return;
    std::vector<NameKey> keys(n);
    for (size_t i = 0; i < n; ++i) BuildNameKey(g_results[i].name, keys[i]);
    std::vector<int> order(n);
    for (size_t i = 0; i < n; ++i) order[i] = (int)i;
    const bool asc = g_sortAsc;
    std::stable_sort(order.begin(), order.end(), [&keys, asc](int a, int b) {
        const int r = CmpNameKey(keys[(size_t)a], keys[(size_t)b]);
        return asc ? (r < 0) : (r > 0);
    });
    std::vector<Res> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) out.push_back(std::move(g_results[(size_t)order[i]]));
    g_results.swap(out);
    g_resultKeys.clear();   // 结果集已变，扩展名缓存失效
}
static void RebuildView() {
    g_view.clear();
    g_view.reserve(g_results.size());
    // 扩展名 key 缓存：结果集变化后惰性重建一次，避免每次勾选筛选都全量重算扩展名
    //（勾满格式时卡顿的主因）。g_resultKeys 与 g_results 同步清空。
    if (g_resultKeys.size() != g_results.size()) {
        g_resultKeys.resize(g_results.size());
        for (size_t i = 0; i < g_results.size(); ++i) g_resultKeys[i] = ExtKeyOf(g_results[i]);
    }
    // 选中格式用集合做 O(1) 命中，去掉每结果 O(|sel|) 的线性扫描。
    std::unordered_set<std::wstring> selSet(g_typeSel.begin(), g_typeSel.end());
    const bool hasSel = !selSet.empty();
    for (size_t i = 0; i < g_results.size(); i++) {
        // 左侧分类 + 文件名列头的格式筛选（第四轮需求4/5：按具体扩展名）；空 = 不过滤
        bool typeOk = !hasSel || (selSet.find(g_resultKeys[i]) != selSet.end());
        if ((g_curCat == CAT_ALL || g_results[i].cat == g_curCat) && typeOk)
            g_view.push_back((int)i);
    }
    int maxScroll = (int)g_view.size() - VisibleRows();
    if (maxScroll < 0) maxScroll = 0;
    if (g_scroll > maxScroll) g_scroll = maxScroll;
    if (g_sel >= (int)g_view.size()) g_sel = (int)g_view.size() - 1;
}
static int VisibleRows() {
    int h = gL.list.bottom - gL.list.top - gL.headerH;
    int rh = gL.rowH ? gL.rowH : S(32);
    return h > 0 ? h / rh : 1;
}
static void ComputeLayout();
// 统一的状态栏文案：数字与列表/角标严格一致（需求12）；超过显示上限时注明
static void UpdateStatusText() {
    const wchar_t* names[4] = { L"名称", L"路径", L"大小", L"修改时间" };
    wchar_t b[220];
    unsigned long long shown = (unsigned long long)g_view.size();
    if (g_curCat == CAT_ALL && g_typeSel.empty() &&
        g_totalFound > shown && shown >= MAX_STORED) {
        swprintf(b, 220, L"共 %llu 个结果，仅显示前 %d 条（已按%s·%s）",
                 (unsigned long long)g_totalFound, (int)shown,
                 names[g_sortCol], g_sortAsc ? L"升序" : L"降序");
    } else {
        swprintf(b, 220, L"共 %llu 个结果（已按%s排列·%s）",
                 shown, names[g_sortCol], g_sortAsc ? L"升序" : L"降序");
    }
    g_statusL = b;
}
static void ApplySortAndView() {
    SortResults(); RebuildView();
    UpdateStatusText();
}
static void DoSearch() {
    wchar_t buf[1024] = {};
    GetWindowTextW(g_edit, buf, 1024);
    g_curEpoch = ++g_epoch;
    {
        std::lock_guard<std::mutex> lk(g_reqMtx);
        g_req.epoch = g_curEpoch; g_req.kw = buf; g_req.scope = g_scopeLetter;
        g_req.scopeDir = g_scopeDir;
        g_req.sub = g_subfolders; g_req.pending = true;
    }
    g_reqCv.notify_one();
    g_searching = true;
    g_statusL = L"正在搜索…";
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ============================================================
// 6. painting
// ============================================================
static void FillRC(HDC dc, RECT r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}
static void FillRcRounded(HDC dc, RECT r, int rad, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    HBRUSH ob = (HBRUSH)SelectObject(dc, b);
    HPEN pen = CreatePen(PS_NULL, 0, 0);
    HPEN op = (HPEN)SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad * 2, rad * 2);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(pen); DeleteObject(b);
}
static void DrawTextRC(HDC dc, const std::wstring& s, RECT r, COLORREF c, HFONT f, UINT fmt = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    HFONT of = (HFONT)SelectObject(dc, f);
    DrawTextW(dc, s.c_str(), -1, &r, fmt);
    SelectObject(dc, of);
}
static void DrawExtBadge(HDC dc, int x, int cy, const Res& r, int badgeW, int badgeH) {
    // small colored ext label
    std::wstring label;
    if (r.cat == CAT_DIR) label = L"DIR";
    else {
        size_t p = r.name.find_last_of(L'.');
        if (p != std::wstring::npos && p + 1 < r.name.size()) {
            label = r.name.substr(p + 1);
            for (auto& c : label) c = (wchar_t)toupper(c);
            if (label.size() > 5) label = label.substr(0, 4) + L"…";
        } else label = L"文件";
    }
    COLORREF col = g_catDefs[r.cat].color;
    RECT rc{ x, cy - badgeH / 2, x + badgeW, cy + badgeH / 2 };
    DrawTextRC(dc, label, rc, col, g_fSmall, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS);
}
static void DrawFolderGlyph(HDC dc, int x, int y, int s, COLORREF col) {
    // simple folder shape
    HBRUSH b = CreateSolidBrush(col);
    HBRUSH ob = (HBRUSH)SelectObject(dc, b);
    HPEN pen = CreatePen(PS_NULL, 0, 0); HPEN op = (HPEN)SelectObject(dc, pen);
    // body
    RECT body{ x, y + s / 4, x + s, y + s };
    RoundRect(dc, body.left, body.top, body.right, body.bottom + 1, 4, 4);
    // tab
    RECT tab{ x, y, x + s * 45 / 100, y + s / 3 };
    RoundRect(dc, tab.left, tab.top, tab.right, tab.bottom + 2, 3, 3);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(pen); DeleteObject(b);
}

static void DrawTitle(HDC dc) {
    FillRC(dc, gL.title, C_TITLE);
    // icon
    HICON ic = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(101), IMAGE_ICON, S(20), S(20), LR_DEFAULTCOLOR);
    if (ic) {
        DrawIconEx(dc, S(12), (gL.title.bottom - S(20)) / 2, ic, S(20), S(20), 0, nullptr, DI_NORMAL);
        DestroyIcon(ic);
    }
    RECT tr = gL.title; tr.left = S(40); tr.left += 0;
    DrawTextRC(dc, L"极速启 · 全盘文件搜索", tr, C_WHITE, g_fTitle, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
    // window buttons
    int bw = S(46);
    struct { int idx; RECT r; } btns[3];
    btns[0] = { 1, { gL.title.right - bw * 3, 0, gL.title.right - bw * 2, gL.title.bottom } };
    btns[1] = { 2, { gL.title.right - bw * 2, 0, gL.title.right - bw, gL.title.bottom } };
    btns[2] = { 3, { gL.title.right - bw, 0, gL.title.right, gL.title.bottom } };
    for (auto& b : btns) {
        bool hov = (g_hoverTitle == b.idx);
        if (b.idx == 3 && hov) FillRC(dc, b.r, RGB(232, 17, 35));
        else if (hov) FillRC(dc, b.r, RGB(86, 144, 222));
        int cx = (b.r.left + b.r.right) / 2, cy = (b.r.top + b.r.bottom) / 2;
        HPEN pen = CreatePen(PS_SOLID, 1, C_WHITE);
        HPEN op = (HPEN)SelectObject(dc, pen);
        HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
        if (b.idx == 1) { // minimize
            MoveToEx(dc, cx - 5, cy, nullptr); LineTo(dc, cx + 5, cy);
        } else if (b.idx == 2) { // maximize / restore
            if (IsZoomed(g_hwnd)) {
                RECT r1{ cx - 5, cy - 3, cx + 3, cy + 5 };
                RECT r2{ cx - 3, cy - 5, cx + 5, cy + 3 };
                Rectangle(dc, r2.left, r2.top, r2.right, r2.bottom);
                // cover overlap
                FillRC(dc, { r1.left + 1, r1.top + 1, r1.right - 1, r1.bottom - 1 }, C_TITLE);
                Rectangle(dc, r1.left, r1.top, r1.right, r1.bottom);
            } else {
                Rectangle(dc, cx - 5, cy - 5, cx + 5, cy + 5);
            }
        } else { // close
            MoveToEx(dc, cx - 5, cy - 5, nullptr); LineTo(dc, cx + 5, cy + 5);
            MoveToEx(dc, cx + 5, cy - 5, nullptr); LineTo(dc, cx - 5, cy + 5);
        }
        SelectObject(dc, ob); SelectObject(dc, op);
        DeleteObject(pen);
    }
}

static void DrawCatItem(HDC dc, int pos, int y, bool selected, bool hovered) {
    int id = g_catOrder[pos];
    RECT r{ 0, y, gL.cat.right, y + gL.catItemH };
    if (selected) {
        FillRC(dc, r, C_SEL);
        RECT bar{ 0, y + 4, S(3), y + gL.catItemH - 4 };
        FillRC(dc, bar, C_TITLE);
    } else if (hovered) FillRC(dc, r, C_HOVER);
    const CatDef& cd = g_catDefs[id];
    RECT tr = r; tr.left = S(20); tr.right = gL.cat.right - S(58);
    DrawTextRC(dc, cd.name, tr, selected ? C_BLUE_TXT : C_TEXT, selected ? g_fBold : g_fList);
    wchar_t cb[24];
    swprintf(cb, 24, L"%u", g_catCount[id]);
    // 计数区：小一号字体 + 足够宽度，多位数（10 万级）也能完整显示
    RECT cr = r; cr.left = gL.cat.right - S(56); cr.right = gL.cat.right - S(10);
    DrawTextRC(dc, cb, cr, C_SUB, g_fSmall, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
}

static void DrawList(HDC dc) {
    // header
    FillRC(dc, gL.header, C_HEADERBG);
    RECT hl = gL.header; hl.top = hl.bottom - 1;
    FillRC(dc, hl, C_SEP);
    const wchar_t* cols[4] = { L"文件名", L"所在路径", L"大小", L"修改时间" };
    int xs[4] = { gL.nameX, gL.pathX, gL.sizeX, gL.timeX };
    int ws[4] = { gL.nameW, gL.pathW, gL.sizeW, gL.timeW };
    for (int i = 0; i < 4; i++) {
        // 需求7-11：列头悬停时整格高亮（Windows 风格）
        if (g_hoverZone == Z_HEADER && g_hoverIdx == i)
            FillRC(dc, { xs[i], gL.header.top, xs[i] + ws[i], gL.header.bottom - 1 }, RGB(226, 239, 253));
        RECT cr{ xs[i] + S(2), gL.header.top, xs[i] + ws[i] - S(8), gL.header.bottom - 1 };
        COLORREF tc = (g_sortCol == i) ? C_BLUE_TXT : C_TEXT;
        HFONT f = (g_sortCol == i) ? g_fBold : g_fList;
        UINT fmt = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_LEFT;   // 需求5：列头文字紧靠左侧
        if (i == 0) cr.right -= S(24);   // 给筛选箭头留位
        DrawTextRC(dc, cols[i], cr, tc, f, fmt);
        if (i == 0) {
            // 文件名列头的格式筛选按钮 ▼（需求7-11）：有筛选时呈蓝色
            int ax = xs[0] + ws[0] - S(18), ay = (gL.header.top + gL.header.bottom - 1) / 2;
            COLORREF ac = g_typeSel.empty() ? RGB(150, 160, 176) : C_BLUE_TXT;
            POINT tri[3] = { { ax, ay - 2 }, { ax + S(10), ay - 2 }, { ax + S(5), ay + 4 } };
            HBRUSH b = CreateSolidBrush(ac);
            HBRUSH ob = (HBRUSH)SelectObject(dc, b);
            HPEN pn = CreatePen(PS_NULL, 0, 0); HPEN opn = (HPEN)SelectObject(dc, pn);
            Polygon(dc, tri, 3);
            SelectObject(dc, ob); SelectObject(dc, opn);
            DeleteObject(b); DeleteObject(pn);
        }
        if (g_sortCol == i) {
            // arrow
            int ax = xs[i] + (i == 2 ? ws[i] - S(8) : 0);
            // draw at right of text: estimate text width
            SIZE sz; HFONT of = (HFONT)SelectObject(dc, f);
            GetTextExtentPoint32W(dc, cols[i], (int)wcslen(cols[i]), &sz);
            SelectObject(dc, of);
            ax = xs[i] + sz.cx + S(6);
            int ay = (gL.header.top + gL.header.bottom - 1) / 2;
            POINT tri[3];
            if (g_sortAsc) {
                tri[0] = { ax, ay - 2 }; tri[1] = { ax + S(8), ay - 2 }; tri[2] = { ax + S(4), ay + 3 };
            } else {
                tri[0] = { ax, ay + 2 }; tri[1] = { ax + S(8), ay + 2 }; tri[2] = { ax + S(4), ay - 3 };
            }
            HBRUSH b = CreateSolidBrush(C_BLUE_TXT);
            HBRUSH ob = (HBRUSH)SelectObject(dc, b);
            HPEN pn = CreatePen(PS_NULL, 0, 0); HPEN opn = (HPEN)SelectObject(dc, pn);
            Polygon(dc, tri, 3);
            SelectObject(dc, ob); SelectObject(dc, opn);
            DeleteObject(b); DeleteObject(pn);
        }
    }
    // column separators（需求5：加深加高，清晰可见）
    for (int i = 1; i < 4; i++) {
        RECT sr{ xs[i] - 1, gL.header.top + S(5), xs[i], gL.header.bottom - S(6) };
        FillRC(dc, sr, RGB(198, 206, 220));
    }
    // rows
    RECT body{ gL.list.left, gL.listBodyY, gL.list.right, gL.list.bottom };
    SaveDC(dc);
    IntersectClipRect(dc, body.left, body.top, body.right, body.bottom);
    int rh = gL.rowH;
    int rows = VisibleRows();
    int n = (int)g_view.size();
    for (int vi = g_scroll; vi < n && vi < g_scroll + rows; vi++) {
        const Res& r = g_results[g_view[vi]];
        int y = body.top + (vi - g_scroll) * rh;
        RECT rr{ gL.list.left, y, gL.list.right, y + rh };
        bool sel = (vi == g_sel);
        if (sel) {
            FillRC(dc, rr, C_SEL);
            FillRC(dc, { gL.list.left, y, gL.list.left + S(3), y + rh }, C_TITLE);
        } else if (vi == g_hoverIdx && g_hoverZone == Z_LIST) FillRC(dc, rr, C_HOVER);
        // badge
        DrawExtBadge(dc, gL.nameX, y + rh / 2, r, S(40), rh);
        RECT nr{ gL.nameX + S(46), y, gL.nameX + gL.nameW, y + rh };
        DrawTextRC(dc, r.name, nr, sel ? C_BLUE_TXT : C_TEXT, sel ? g_fBold : g_fList);
        RECT pr{ gL.pathX, y, gL.pathX + gL.pathW, y + rh };
        DrawTextRC(dc, ResDir(r), pr, C_SUB, g_fList);
        RECT sr{ gL.sizeX + S(2), y, gL.sizeX + gL.sizeW, y + rh };
        DrawTextRC(dc, SizeStr(r.size), sr, C_TEXT, g_fList, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);   // 需求1：大小靠左
        RECT tr{ gL.timeX, y, gL.timeX + gL.timeW, y + rh };
        DrawTextRC(dc, TimeStr(r.mtime), tr, C_SUB, g_fList);
    }
    if (n == 0 && !g_searching) {
        std::wstring hint = g_curEpoch ? L"无匹配结果" : L"输入文件名关键词后回车或点击“搜索”";
        DrawTextRC(dc, hint, body, C_SUB, g_fUI, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }
    RestoreDC(dc, -1);
    // vscroll（需求10：加宽且可拖动；度量统一由 VScrollMetrics 提供）
    int trackH, thumbH, thumbTop, maxScroll;
    if (VScrollMetrics(trackH, thumbH, thumbTop, maxScroll)) {
        FillRC(dc, gL.vscroll, C_BG);
        FillRcRounded(dc, { gL.vscroll.left + S(2), thumbTop, gL.vscroll.right - S(2), thumbTop + thumbH },
                      S(5), (g_hoverZone == Z_VSCROLL || g_drag == 5) ? RGB(150, 163, 184) : C_THUMB);
    }
}

// 滚动条度量（trackH/thumbH/thumbTop/maxScroll），无滚动条时返回 false
static bool VScrollMetrics(int& trackH, int& thumbH, int& thumbTop, int& maxScroll) {
    if (gL.vscroll.right <= gL.vscroll.left) return false;
    int n = (int)g_view.size(), rows = VisibleRows();
    if (n <= rows || n <= 0) return false;
    trackH = gL.vscroll.bottom - gL.vscroll.top;
    thumbH = (int)(__int64(trackH) * rows / n);
    if (thumbH < S(36)) thumbH = S(36);   // 需求10：滑块最小高度加大，便于拖动
    if (thumbH > trackH) thumbH = trackH;
    maxScroll = n - rows; if (maxScroll < 1) maxScroll = 1;
    thumbTop = gL.vscroll.top + (int)((__int64)(trackH - thumbH) * g_scroll / maxScroll);
    return true;
}

static void DrawPreview(HDC dc) {
    if (g_sel < 0 || g_sel >= (int)g_view.size()) {
        RECT r = gL.prev;
        DrawTextRC(dc, L"选中文件后在此预览", r, C_SUB, g_fUI, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        return;
    }
    const Res& r = g_results[g_view[g_sel]];
    int cx = (gL.prev.left + gL.prev.right) / 2;
    int bw = S(96), bh = S(96);
    int bx = cx - bw / 2, by = gL.prev.top + S(36);
    bool isDir = r.cat == CAT_DIR;
    FillRcRounded(dc, { bx, by, bx + bw, by + bh }, S(14), isDir ? RGB(255, 233, 168) : RGB(238, 241, 246));
    if (isDir) {
        DrawFolderGlyph(dc, cx - S(28), by + S(24), S(56), RGB(240, 190, 60));
    } else {
        std::wstring label;
        size_t p = r.name.find_last_of(L'.');
        if (p != std::wstring::npos && p + 1 < r.name.size()) {
            label = r.name.substr(p + 1);
            for (auto& c : label) c = (wchar_t)toupper(c);
            if (label.size() > 6) label = label.substr(0, 5) + L"…";
        } else label = L"文件";
        RECT br{ bx, by, bx + bw, by + bh };
        DrawTextRC(dc, label, br, g_catDefs[r.cat].color, g_fBadge, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }
    // name
    RECT nr{ gL.prev.left + S(16), by + bh + S(14), gL.prev.right - S(16), by + bh + S(14) + S(40) };
    SetTextColor(dc, C_TEXT);
    HFONT of = (HFONT)SelectObject(dc, g_fBold);
    DrawTextW(dc, r.name.c_str(), -1, &nr, DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, of);
    // path
    RECT pr{ gL.prev.left + S(16), nr.bottom + S(4), gL.prev.right - S(16), nr.bottom + S(4) + S(36) };
    SetTextColor(dc, C_SUB);
    of = (HFONT)SelectObject(dc, g_fSmall);
    DrawTextW(dc, ResDir(r).c_str(), -1, &pr, DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, of);
    // divider
    int dy = pr.bottom + S(12);
    FillRC(dc, { gL.prev.left + S(16), dy, gL.prev.right - S(16), dy + 1 }, C_SEP);
    // info rows
    std::wstring szStr = SizeStr(r.size), tmStr = TimeStr(r.mtime);
    struct { const wchar_t* k; const wchar_t* v; } rows[2] = {
        { L"大小", szStr.c_str() }, { L"修改时间", tmStr.c_str() }
    };
    int iy = dy + S(14);
    for (auto& row : rows) {
        RECT kr{ gL.prev.left + S(16), iy, gL.prev.left + S(88), iy + S(22) };
        DrawTextRC(dc, row.k, kr, C_SUB, g_fSmall);
        RECT vr{ gL.prev.left + S(92), iy, gL.prev.right - S(16), iy + S(22) };
        DrawTextRC(dc, row.v, vr, C_TEXT, g_fSmall);
        iy += S(26);
    }
    // buttons
    RECT bo = PreviewOpenRect(), bf = PreviewOpenFRect();
    bool hovO = g_hoverZone == Z_OPEN, hovF = g_hoverZone == Z_OPENF;
    FillRcRounded(dc, bo, S(6), hovO ? C_BTN_HOV : C_BTN);
    DrawTextRC(dc, L"打开", bo, C_WHITE, g_fList, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    FillRcRounded(dc, bf, S(6), hovF ? RGB(228, 233, 241) : RGB(237, 241, 247));
    DrawTextRC(dc, L"打开目录", bf, C_TEXT, g_fList, DT_SINGLELINE | DT_VCENTER | DT_CENTER);   // 需求7：改名
}

static void ComputeLayout() {
    RECT rc; GetClientRect(g_hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    int titleH = S(38), searchRow = S(64), filterRow = S(46), statusH = S(30);
    gL.title = { 0, 0, w, titleH };
    int editX = S(18), editY = titleH + S(13), editH = S(38);
    int btnW = S(96), btnH = S(38), btnX = w - editX - btnW;
    gL.edit = { editX, editY, btnX - S(14), editY + editH };
    gL.btn = { btnX, editY, btnX + btnW, editY + btnH };
    int fy = titleH + searchRow;
    // ---- 面包屑：搜索范围: [此电脑] > [D:\] > [子目录] ----
    int by = fy + S(8), bh = S(30);
    int bx = editX;
    bx += S(80);                                  // "搜索范围:" 标签宽度
    gL.bcSegN = 1;
    gL.bcSeg[0] = { bx, by, bx + S(76), by + bh }; bx += S(76);
    if (g_scopeLetter) {
        bx += S(22);                              // "›" 分隔
        gL.bcSeg[1] = { bx, by, bx + S(58), by + bh }; bx += S(58);
        gL.bcSegN = 2;
        if (!g_scopeDir.empty()) {
            bx += S(22);
            gL.bcSeg[2] = { bx, by, bx + S(200), by + bh }; bx += S(200);
            gL.bcSegN = 3;
        }
    }
    gL.scope = { editX, by, bx, by + bh };
    gL.chk = { w - S(190), fy + S(10), w - S(18), fy + S(10) + S(24) };
    gL.chkBox = { gL.chk.left, gL.chk.top + S(2), gL.chk.left + S(20), gL.chk.top + S(22) };
    int mainY = titleH + searchRow + filterRow;
    int mainH = h - mainY - statusH;
    int split = S(8);
    int catR = g_catW;
    int split2X = w - g_prevW - split;
    int prevX = split2X + split;
    gL.main = { 0, mainY, w, mainY + mainH };
    gL.cat = { 0, mainY, catR, mainY + mainH };
    gL.split1 = { catR, mainY, catR + split, mainY + mainH };
    gL.list = { catR + split, mainY, split2X, mainY + mainH };
    gL.split2 = { split2X, mainY, prevX, mainY + mainH };
    gL.prev = { prevX, mainY, w, mainY + mainH };
    gL.headerH = S(40); gL.rowH = S(36); gL.catItemH = S(42);
    gL.header = { gL.list.left, mainY, gL.list.right, mainY + gL.headerH };
    gL.listBodyY = mainY + gL.headerH;
    int listW = split2X - gL.list.left;
    // 需求6：文件名/所在路径/大小 三列各自持久化宽度，修改时间列弹性吸收总宽变化，
    // 使三条分隔线拖动时都严格跟随鼠标（而不是联动到别的边界）
    int innerW = listW - S(20);
    int nameW = g_nameW, pathW = g_pathW, sizeW = g_sizeW;
    int minName = S(150), minPath = S(120), minSize = S(64), minTime = S(130);
    if (nameW < minName) nameW = minName;
    if (pathW < minPath) pathW = minPath;
    if (sizeW < minSize) sizeW = minSize;
    int over = nameW + pathW + sizeW + minTime - innerW;
    if (over > 0) {
        int d = std::min(over, pathW - minPath); pathW -= d; over -= d;
        if (over > 0) { d = std::min(over, nameW - minName); nameW -= d; over -= d; }
        if (over > 0) { d = std::min(over, sizeW - minSize); sizeW -= d; over -= d; }
    }
    int timeW = innerW - nameW - pathW - sizeW;
    if (timeW < minTime) timeW = minTime;
    gL.nameW = nameW; gL.pathW = pathW; gL.sizeW = sizeW; gL.timeW = timeW;
    gL.nameX = gL.list.left + S(10);
    gL.pathX = gL.nameX + nameW;
    gL.sizeX = gL.pathX + pathW;
    gL.timeX = gL.sizeX + gL.sizeW;
    int rows = VisibleRows();
    int n = (int)g_view.size();
    if (n > rows) {
        int vw = S(20);                           // 需求10：滚动条放宽一倍
        gL.vscroll = { gL.list.right - vw, gL.listBodyY, gL.list.right, gL.list.bottom };
    } else gL.vscroll = {};
    gL.status = { 0, mainY + mainH, w, mainY + mainH + statusH };
    // 状态栏右下角的「刷新」按钮（用户要求的那个）：固定 56×22，距右边 12
    {
        int bh2 = S(22), bw2 = S(56);
        int by = gL.status.top + ((gL.status.bottom - gL.status.top) - bh2) / 2;
        gL.refresh = { gL.status.right - S(12) - bw2, by,
                       gL.status.right - S(12), by + bh2 };
    }
    // move edit child（内缩：左侧留放大镜、右侧留 Ctrl+F 提示，上下内缩保证文字垂直居中）
    // 需求11：仅在矩形真正变化时才移动，且不擦除背景，避免拖动/滚动时搜索框闪烁
    if (g_edit) {
        RECT want{ gL.edit.left + S(46), gL.edit.top + S(4),
                   gL.edit.right - S(96), gL.edit.bottom - S(4) };
        RECT cur{}; GetWindowRect(g_edit, &cur);
        POINT tl{ cur.left, cur.top }, br{ cur.right, cur.bottom };
        ScreenToClient(g_hwnd, &tl); ScreenToClient(g_hwnd, &br);
        RECT now{ tl.x, tl.y, br.x, br.y };
        if (want.left != now.left || want.top != now.top ||
            want.right != now.right || want.bottom != now.bottom) {
            MoveWindow(g_edit, want.left, want.top,
                       want.right - want.left, want.bottom - want.top, FALSE);
        }
    }
}

static HBITMAP g_memBmp = nullptr, g_memOldBmp = nullptr;
static HDC g_memDc = nullptr; static SIZE g_memSize{};
static bool g_roundByRgn = false;   // Win10 走 GDI 圆角回退（区域必须随尺寸重设）

// ★ Win10 不支持 DWMWA_WINDOW_CORNER_PREFERENCE，只能用 SetWindowRgn 做圆角。
//   但窗口区域坐标是「相对窗口左上角的定值」，**不随尺寸扩张**；MSDN 对
//   SetWindowRgn 有明确警告：设过区域的窗口一旦移动/改大小，屏幕上旧区域会
//   「永远不重绘、永远不消失」。所以每次尺寸变化后都必须重设区域，
//   拖动期间先取消（见 WM_ENTERSIZEMOVE），松手后再设回来。
static void ApplyRoundRegion() {
    if (!g_roundByRgn || !g_hwnd) return;
    RECT rc; GetClientRect(g_hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    int rad = GetDpiForWindow(g_hwnd) * 12 / 96;
    HRGN rgn = CreateRoundRectRgn(0, 0, rc.right + 1, rc.bottom + 1, rad * 2, rad * 2);
    SetWindowRgn(g_hwnd, rgn, TRUE);   // 成功后 rgn 归系统所有，不要再 DeleteObject
}

static void PaintAll() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(g_hwnd, &ps);
    RECT rc; GetClientRect(g_hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) { EndPaint(g_hwnd, &ps); return; }
    if (!g_memDc || g_memSize.cx != w || g_memSize.cy != h) {
        if (g_memDc) {                       // 先把位图从 DC 里摘出来，否则位图删不掉（GDI 泄漏）
            if (g_memOldBmp) SelectObject(g_memDc, g_memOldBmp);
            DeleteDC(g_memDc); g_memDc = nullptr; g_memOldBmp = nullptr;
        }
        if (g_memBmp) { DeleteObject(g_memBmp); g_memBmp = nullptr; }
        HDC ndc = CreateCompatibleDC(dc);
        HBITMAP nbmp = CreateCompatibleBitmap(dc, w, h);
        if (!ndc || !nbmp) {                 // 句柄耗尽的兜底，别让整帧消失
            if (ndc) DeleteDC(ndc);
            if (nbmp) DeleteObject(nbmp);
            EndPaint(g_hwnd, &ps);
            return;
        }
        g_memDc = ndc;
        g_memBmp = nbmp;
        g_memOldBmp = (HBITMAP)SelectObject(g_memDc, g_memBmp);
        g_memSize = { w, h };
    }
    HDC m = g_memDc;
    // base bg
    FillRC(m, rc, C_BG);
    DrawTitle(m);
    // search row: no borders; white edit field drawn by child itself
    RECT btnR = gL.btn;
    bool hovBtn = g_hoverZone == Z_BTN && g_downZone != Z_BTN;
    bool downBtn = g_downZone == Z_BTN;
    FillRcRounded(m, btnR, S(8), downBtn ? RGB(36, 104, 184) : hovBtn ? C_BTN_HOV : C_BTN);
    DrawTextRC(m, L"搜索", btnR, C_WHITE, g_fBold, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    // scope：面包屑样式（需求1：搜索范围: 此电脑 › D:\ › 子文件夹）
    DrawTextRC(m, L"搜索范围:", { gL.scope.left, gL.scope.top, gL.scope.left + S(76), gL.scope.bottom },
               C_SUB, g_fUI);
    for (int si = 0; si < gL.bcSegN; si++) {
        const RECT& r = gL.bcSeg[si];
        bool leaf = (si == gL.bcSegN - 1);
        bool hovSeg = (g_hoverZone == Z_SCOPE && g_hoverIdx == si);
        // 底色：末段浅蓝高亮，其余白色；悬停浅灰
        COLORREF bg = leaf ? C_SEL : C_WHITE;
        if (hovSeg) bg = leaf ? RGB(214, 232, 252) : C_HOVER;
        FillRcRounded(m, r, S(6), bg);
        if (!leaf) {
            HPEN pen = CreatePen(PS_SOLID, 1, C_BOXBRD); HPEN op = (HPEN)SelectObject(m, pen);
            HBRUSH ob = (HBRUSH)SelectObject(m, GetStockObject(NULL_BRUSH));
            RoundRect(m, r.left, r.top, r.right, r.bottom, S(12), S(12));
            SelectObject(m, ob); SelectObject(m, op); DeleteObject(pen);
        }
        std::wstring txt;
        if (si == 0) txt = L"此电脑";
        else if (si == 1) txt = std::wstring(1, g_scopeLetter) + L":\\";
        else {
            txt = g_scopeDir;
            size_t p = txt.find_last_of(L'\\');
            if (p != std::wstring::npos && p + 1 < txt.size()) txt = txt.substr(p + 1);
        }
        RECT tr = r; tr.left += S(10); tr.right -= S(10);
        DrawTextRC(m, txt, tr, leaf ? C_BLUE_TXT : C_TEXT, g_fList);
        // 分隔箭头 ›
        if (si < gL.bcSegN - 1) {
            int ax = r.right + S(8), ay = (r.top + r.bottom) / 2;
            HPEN pen = CreatePen(PS_SOLID, 1, C_SUB); HPEN op = (HPEN)SelectObject(m, pen);
            MoveToEx(m, ax, ay - S(4), nullptr); LineTo(m, ax + S(5), ay); LineTo(m, ax, ay + S(4));
            SelectObject(m, op); DeleteObject(pen);
        }
    }
    // 搜索框内：放大镜图标（第四轮需求7：圆+柄整体在框内垂直居中——手柄在右下，
    // 故圆心上移 S(2) 做光学居中；配色对照参考图：浅蓝填充+蓝描边）
    {
        int cx = gL.edit.left + S(20);
        int cy = (gL.edit.top + gL.edit.bottom) / 2 - S(2);
        int rad = S(8);
        HBRUSH fb = CreateSolidBrush(RGB(223, 236, 252));
        HBRUSH ofb = (HBRUSH)SelectObject(m, fb);
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(86, 136, 212)); HPEN op = (HPEN)SelectObject(m, pen);
        Ellipse(m, cx - rad, cy - rad, cx + rad, cy + rad);
        SelectObject(m, GetStockObject(NULL_BRUSH));
        MoveToEx(m, cx + rad * 6 / 10, cy + rad * 6 / 10, nullptr);
        LineTo(m, cx + rad + S(4), cy + rad + S(4));
        SelectObject(m, ofb); SelectObject(m, op); DeleteObject(pen); DeleteObject(fb);
    }
    DrawTextRC(m, L"Ctrl+F 聚焦", { gL.edit.right - S(92), gL.edit.top, gL.edit.right - S(12), gL.edit.bottom },
               RGB(170, 178, 192), g_fSmall, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
    // checkbox
    bool hovChk = g_hoverZone == Z_CHK;
    RECT cb = gL.chkBox;
    HBRUSH b2 = CreateSolidBrush(g_subfolders ? C_BTN : C_WHITE);
    HBRUSH ob2 = (HBRUSH)SelectObject(m, b2);
    HPEN pen2 = CreatePen(PS_SOLID, 1, C_BOXBRD); HPEN op2 = (HPEN)SelectObject(m, pen2);
    RoundRect(m, cb.left, cb.top, cb.right, cb.bottom, S(4), S(4));
    SelectObject(m, ob2); SelectObject(m, op2);
    DeleteObject(pen2); DeleteObject(b2);
    if (g_subfolders) {
        HPEN pen = CreatePen(PS_SOLID, 2, C_WHITE); HPEN op = (HPEN)SelectObject(m, pen);
        MoveToEx(m, cb.left + S(3), cb.top + S(8), nullptr);
        LineTo(m, cb.left + S(7), cb.bottom - S(4));
        LineTo(m, cb.right - S(3), cb.top + S(3));
        SelectObject(m, op); DeleteObject(pen);
    }
    RECT ct = gL.chk; ct.left = cb.right + S(8);
    DrawTextRC(m, L"包含子文件夹", ct, hovChk ? C_TEXT : C_SUB, g_fList);   // 需求9：再大一号(12pt)
    // category pane
    FillRC(m, gL.cat, C_WHITE);
    int y = gL.cat.top + S(8);
    for (int pos = 0; pos < (int)g_catOrder.size(); pos++) {
        int id = g_catOrder[pos];
        bool sel = (g_curCat == id && !g_catDragActive);
        bool hov = (g_hoverZone == Z_CAT && g_hoverIdx == pos);
        DrawCatItem(m, pos, y, sel, hov);
        y += gL.catItemH;
    }
    if (g_catDragActive && g_catDragFrom >= 0) {
        // insertion line
        int iy = gL.cat.top + S(8) + g_catDragTo * gL.catItemH;
        FillRC(m, { 0, iy - 2, gL.cat.right, iy + 2 }, C_TITLE);
        // dragged item at cursor
        int dy = iy - gL.catItemH / 2;
        // draw dragged cat item following mouse — use g_catDragY stored on move
        DrawCatItem(m, g_catDragFrom, g_catDragY, false, false);
    }
    // splitters: 需求5/9 —— 分隔线加清晰（1px 中灰）
    {
        int x1 = (gL.split1.left + gL.split1.right) / 2;
        int x2 = (gL.split2.left + gL.split2.right) / 2;
        FillRC(m, { x1, gL.split1.top, x1 + 1, gL.split1.bottom }, RGB(214, 221, 232));
        FillRC(m, { x2, gL.split2.top, x2 + 1, gL.split2.bottom }, RGB(214, 221, 232));
    }
    // list pane
    FillRC(m, gL.list, C_WHITE);
    DrawList(m);
    // preview pane
    FillRC(m, gL.prev, C_WHITE);
    DrawPreview(m);
    // status
    FillRC(m, gL.status, C_BG);
    RECT sl = gL.status; sl.bottom = sl.top + 1;
    FillRC(m, sl, C_SEP);
    RECT lt = gL.status; lt.left = S(14); lt.right = gL.status.right - S(220);
    DrawTextRC(m, g_statusL, lt, C_SUB, g_fSmall);
    // 右侧：索引状态 + 「刷新」按钮（按钮在最右，状态文字让位给它）
    RECT rt = gL.status; rt.right = gL.refresh.left - S(10); rt.left = gL.status.right - S(400);
    DrawTextRC(m, GetIndexStatus(), rt, C_SUB, g_fSmall, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
    if (gL.refresh.right > gL.refresh.left) {
        const bool hov = (g_hoverZone == Z_REFRESH);
        HBRUSH bg = CreateSolidBrush(hov ? RGB(0xE5, 0xF4, 0xFD) : RGB(0xFF, 0xFF, 0xFF));
        HBRUSH bd = CreateSolidBrush(hov ? RGB(0x29, 0xA8, 0xE0) : RGB(0xD6, 0xDD, 0xE8));
        FillRect(m, &gL.refresh, bg);
        FrameRect(m, &gL.refresh, bd);
        DeleteObject(bg); DeleteObject(bd);
        DrawTextRC(m, L"刷新", gL.refresh,
                   hov ? RGB(0x1A, 0x7F, 0xB0) : RGB(0x50, 0x54, 0x5C), g_fSmall,
                   DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }
    // ★ 不要用 BeginPaint 拿到的 dc 来贴：它的裁剪区是「无效区 ∩ 窗口区域 ∩ 可见区」，
    //   窗口被拉大后右/下新增的那一条永远不在无效区里，旧像素也就永远贴不上去
    //   （用户看到的"残影碎块 / 新竖条发白"就是这么来的）。
    //   本函数每次都是整窗全量绘制，所以这里用未按无效区裁剪的整窗 DC 一次贴满。
    HDC wdc = GetDC(g_hwnd);
    if (wdc) {
        BitBlt(wdc, 0, 0, w, h, m, 0, 0, SRCCOPY);
        ReleaseDC(g_hwnd, wdc);
    }
    EndPaint(g_hwnd, &ps);
}

// ============================================================
// 7. interactions
// ============================================================
static int CatItemAt(int y) {
    int yy = gL.cat.top + S(8);
    for (int pos = 0; pos < (int)g_catOrder.size(); pos++) {
        if (y >= yy && y < yy + gL.catItemH) return pos;
        yy += gL.catItemH;
    }
    return -1;
}
static int SegmentAt(int x, int y) {
    if (!PtInRC(gL.scope, x, y)) return -1;
    for (int i = 0; i < gL.bcSegN; i++)
        if (PtInRC(gL.bcSeg[i], x, y)) return i;
    return -1;
}
static int RowAt(int y) {
    if (y < gL.listBodyY || y >= gL.list.bottom) return -1;
    int idx = g_scroll + (y - gL.listBodyY) / gL.rowH;
    if (idx >= 0 && idx < (int)g_view.size()) return idx;
    return -1;
}
static int ColAt(int x) {
    // 0 name,1 path,2 size,3 time
    if (x >= gL.nameX && x < gL.pathX) return 0;
    if (x >= gL.pathX && x < gL.sizeX) return 1;
    if (x >= gL.sizeX && x < gL.timeX) return 2;
    if (x >= gL.timeX && x < gL.list.right) return 3;
    return -1;
}
static bool NearColSep(int x, int& col) {
    // col = 拖动该边界时调整其左侧列宽：0=文件名 1=所在路径 2=大小（需求6）
    if (abs(x - gL.pathX) <= S(4)) { col = 0; return true; }
    if (abs(x - gL.sizeX) <= S(4)) { col = 1; return true; }
    if (abs(x - gL.timeX) <= S(4)) { col = 2; return true; }
    return false;
}
static void ClampScroll() {
    int maxScroll = (int)g_view.size() - VisibleRows();
    if (maxScroll < 0) maxScroll = 0;
    if (g_scroll < 0) g_scroll = 0;
    if (g_scroll > maxScroll) g_scroll = maxScroll;
}
static void SelectCat(int pos) {
    int id = g_catOrder[pos];
    g_curCat = id;
    g_typeSel.clear();          // 切换分类清空格式筛选记忆（回到默认「全不勾=不过滤」）
    RebuildView();
    UpdateStatusText();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}
static void OpenItem(const Res& r, bool explore) {
    std::wstring dir = ResDir(r);
    std::wstring full = dir + L"\\" + r.name;
    if (explore) {
        if (r.cat == CAT_DIR) {
            // parent of dir = dir itself is dir path; open its parent
            std::wstring parent = dir;
            size_t p = parent.find_last_of(L'\\');
            if (p != std::wstring::npos && p > 2) parent = parent.substr(0, p);
            else parent = dir;
            ShellExecuteW(g_hwnd, nullptr, parent.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else {
            std::wstring cmd = L"/select,\"" + full + L"\"";
            ShellExecuteW(g_hwnd, nullptr, L"explorer.exe", cmd.c_str(), nullptr, SW_SHOWNORMAL);
        }
    } else {
        if (r.cat == CAT_DIR) ShellExecuteW(g_hwnd, nullptr, full.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        // 修复："open" 是动词，必须放在 lpOperation 位；原来放成 lpFile 导致打开无反应
        else ShellExecuteW(g_hwnd, L"open", full.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}
static void CopyPath(const Res& r) {
    std::wstring full = ResDir(r) + L"\\" + r.name;
    if (OpenClipboard(g_hwnd)) {
        EmptyClipboard();
        size_t bytes = (full.size() + 1) * sizeof(wchar_t);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (h) {
            memcpy(GlobalLock(h), full.c_str(), bytes);
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
        }
        CloseClipboard();
    }
}
static const Res* SelectedRes() {
    if (g_sel >= 0 && g_sel < (int)g_view.size()) return &g_results[g_view[g_sel]];
    return nullptr;
}

// ---------- 通用列表弹窗（搜索范围面包屑用，需求1） ----------
struct PopItem { std::wstring text; int type; wchar_t letter; std::wstring path; };
// type: 0=此电脑 1=整个磁盘 2=进入子目录 3=返回上一级
static std::vector<PopItem> g_popList;
static int g_popScroll = 0, g_popHov = -1;
static bool g_popEntered = false;
static POINT g_popOrigin{ 0, 0 };

static void PopApply(const PopItem& it) {
    switch (it.type) {
    case 0: g_scopeLetter = 0; g_scopeDir.clear(); break;
    case 1: g_scopeLetter = it.letter; g_scopeDir.clear(); break;
    case 2: g_scopeDir = it.path; if (g_scopeDir.size() >= 2) g_scopeLetter = g_scopeDir[0]; break;
    case 3: g_scopeDir = it.path; break;   // 可为空 = 整盘
    }
    SaveConfig();
    DoSearch();
}

static void ListSubDirs(const std::wstring& base, std::vector<PopItem>& out, int cap) {
    WIN32_FIND_DATAW fd{};
    HANDLE hF = FindFirstFileW((base + L"\\*").c_str(), &fd);
    if (hF == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        const wchar_t* nm = fd.cFileName;
        if (!wcscmp(nm, L".") || !wcscmp(nm, L"..")) continue;
        PopItem it; it.text = nm; it.type = 2; it.path = base + L"\\" + nm;
        out.push_back(std::move(it));
        if ((int)out.size() >= cap) break;
    } while (FindNextFileW(hF, &fd));
    FindClose(hF);
    std::sort(out.begin(), out.end(), [](const PopItem& a, const PopItem& b) {
        return _wcsicmp(a.text.c_str(), b.text.c_str()) < 0; });
}

static LRESULT CALLBACK ScopePopupProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    const int ih = S(34);
    RECT rc; GetClientRect(h, &rc);
    int vis = (rc.bottom - S(4)) / ih; if (vis < 1) vis = 1;
    switch (msg) {
    case WM_CREATE: break;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        FillRC(dc, rc, C_WHITE);
        HPEN pen = CreatePen(PS_SOLID, 1, C_BOXBRD); HPEN op = (HPEN)SelectObject(dc, pen);
        HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, 0, 0, rc.right, rc.bottom);
        SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(pen);
        for (int vi = 0; vi < vis; vi++) {
            int idx = g_popScroll + vi;
            if (idx < 0 || idx >= (int)g_popList.size()) break;
            const PopItem& it = g_popList[idx];
            int y = S(4) + vi * ih;
            RECT r{ S(4), y, rc.right - S(4), y + ih };
            bool cur = false;
            if (it.type == 0) cur = (g_scopeLetter == 0 && g_scopeDir.empty());
            else if (it.type == 1) cur = (g_scopeLetter == it.letter && g_scopeDir.empty());
            else if (it.type == 2) cur = (g_scopeDir == it.path);
            if (idx == g_popHov) FillRcRounded(dc, r, S(6), C_SEL);
            RECT tr = r; tr.left += S(12);
            DrawTextRC(dc, it.text, tr, (idx == g_popHov || cur) ? C_BLUE_TXT : C_TEXT, g_fList);
            if (cur) {   // 当前范围打勾
                int cx2 = r.right - S(16), cy2 = (r.top + r.bottom) / 2;
                HPEN pn2 = CreatePen(PS_SOLID, 2, C_BLUE_TXT); HPEN op2 = (HPEN)SelectObject(dc, pn2);
                MoveToEx(dc, cx2 - S(4), cy2, nullptr); LineTo(dc, cx2 - S(1), cy2 + S(3));
                LineTo(dc, cx2 + S(4), cy2 - S(3));
                SelectObject(dc, op2); DeleteObject(pn2);
            }
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        // 只做悬停高亮；不因鼠标移出而关闭（避免真实光标/抖动导致弹窗被误销毁）
        int i = g_popScroll + (GET_Y_LPARAM(lp) - S(4)) / ih;
        int nh = (i >= 0 && i < (int)g_popList.size()) ? i : -1;
        if (nh != g_popHov) { g_popHov = nh; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int maxs = (int)g_popList.size() - vis; if (maxs < 0) maxs = 0;
        g_popScroll -= (delta / WHEEL_DELTA) * 2;
        if (g_popScroll < 0) g_popScroll = 0;
        if (g_popScroll > maxs) g_popScroll = maxs;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT sp{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ClientToScreen(h, &sp);
        RECT wr; GetWindowRect(h, &wr);
        if (!PtInRect(&wr, sp)) { DestroyWindow(h); return 0; }   // 点外部关闭
        int i = g_popScroll + (GET_Y_LPARAM(lp) - S(4)) / ih;
        if (i >= 0 && i < (int)g_popList.size()) PopApply(g_popList[i]);
        DestroyWindow(h);
        SetForegroundWindow(g_hwnd);
        return 0;
    }
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        DestroyWindow(h);
        return 0;
    case WM_KEYDOWN:
        DestroyWindow(h);    // 需求3：按任意键即关闭
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;    // 鼠标点击不再重复激活
    case WM_KILLFOCUS:
        DestroyWindow(h);    // 需求2：焦点移到任何窗口（含其他应用）即关闭
        return 0;
    case WM_NCDESTROY:
        if (GetCapture() == h) ReleaseCapture();
        if (g_pop == h) g_pop = nullptr;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void ShowScopePopup(int seg) {
    if (g_pop) { DestroyWindow(g_pop); g_pop = nullptr; }
    g_popList.clear(); g_popScroll = 0; g_popHov = -1; g_popEntered = false;
    if (seg <= 0) {
        g_popList.push_back({ L"此电脑", 0, 0, L"" });
        DWORD drives = GetLogicalDrives();
        for (int i = 0; i < 26; i++) {
            if (!(drives & (1u << i))) continue;
            wchar_t L_ = (wchar_t)('A' + i);
            wchar_t root[4] = { L_, L':', L'\\', 0 };
            if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
            g_popList.push_back({ std::wstring(1, L_) + L":\\", 1, L_, L"" });
        }
    } else if (seg == 1 && g_scopeLetter) {
        std::wstring root = std::wstring(1, g_scopeLetter) + L":";
        g_popList.push_back({ L"整个 " + root + L"\\", 1, g_scopeLetter, L"" });
        ListSubDirs(root, g_popList, 400);
    } else if (seg == 2 && !g_scopeDir.empty()) {
        std::wstring parent = g_scopeDir;
        size_t p = parent.find_last_of(L'\\');
        parent = (p == std::wstring::npos || p < 3) ? std::wstring() : parent.substr(0, p);
        g_popList.push_back({ parent.empty() ? L"整个磁盘" : L"返回上一级", 3, 0, parent });
        ListSubDirs(g_scopeDir, g_popList, 400);
    }
    if (g_popList.empty()) return;
    WNDCLASSW wc{};
    wc.lpfnWndProc = ScopePopupProc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"WbScopePopup";
    wc.style = CS_DROPSHADOW;
    RegisterClassW(&wc);
    const int ih = S(34), MAXVIS = 12;
    int wd = S(250);
    int vis = (int)g_popList.size() < MAXVIS ? (int)g_popList.size() : MAXVIS;
    int ht = vis * ih + S(8);
    // 锚定到所点分段下方
    const RECT& sr = (seg >= 1 && seg < gL.bcSegN) ? gL.bcSeg[seg] : gL.bcSeg[0];
    POINT pt{ sr.left, sr.bottom + 2 };
    ClientToScreen(g_hwnd, &pt);
    g_popOrigin = pt;
    g_pop = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"WbScopePopup", nullptr,
                            WS_POPUP | WS_VISIBLE, pt.x, pt.y, wd, ht,
                            g_hwnd, nullptr, g_inst, nullptr);
    SetWindowPos(g_pop, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    // 需求2/3：临时接管前台 → 点击任何地方或按任意键即关闭（WM_KILLFOCUS/WM_KEYDOWN）
    SetForegroundWindow(g_pop);
    UpdateWindow(g_pop);
    g_popEntered = true;
}

// ---------- 自绘右键菜单（需求2 展示图样式；需求7 打开目录） ----------
struct CtxItem { const wchar_t* label; const wchar_t* shortcut; int id; };
static const CtxItem g_ctxItems[] = {
    { L"打开",     L"Enter",        100 },
    { L"打开目录", L"Ctrl+E",       101 },
    { L"复制路径", L"Ctrl+Shift+C", 102 },
};
static const int g_ctxN = 3;
static HWND g_ctx = nullptr;
static int g_ctxHov = -1;
static bool g_ctxEntered = false;
static POINT g_ctxOrigin{ 0, 0 };

static void CtxRun(int id) {
    const Res* r = SelectedRes();
    if (!r) return;
    if (id == 100) OpenItem(*r, false);
    else if (id == 101) OpenItem(*r, true);
    else if (id == 102) CopyPath(*r);
}

static void CtxDrawIcon(HDC dc, int id, int ix, int cy, bool hov) {
    COLORREF col = hov ? C_BLUE_TXT : C_SUB;
    HPEN pen = CreatePen(PS_SOLID, 2, col); HPEN op = (HPEN)SelectObject(dc, pen);
    HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    if (id == 100) {          // → 箭头
        MoveToEx(dc, ix - S(6), cy, nullptr); LineTo(dc, ix + S(5), cy);
        MoveToEx(dc, ix + S(1), cy - S(4), nullptr); LineTo(dc, ix + S(5), cy); LineTo(dc, ix + S(1), cy + S(4));
    } else if (id == 101) {   // 文件夹
        SelectObject(dc, op); DeleteObject(pen);
        DrawFolderGlyph(dc, ix - S(7), cy - S(7), S(14), hov ? RGB(240, 190, 60) : RGB(172, 181, 195));
        pen = CreatePen(PS_SOLID, 2, col); op = (HPEN)SelectObject(dc, pen);
        SelectObject(dc, GetStockObject(NULL_BRUSH));
    } else {                  // 复制：双层矩形
        RoundRect(dc, ix - S(3), cy - S(7), ix + S(7), cy + S(3), S(3), S(3));
        RoundRect(dc, ix - S(7), cy - S(3), ix + S(3), cy + S(7), S(3), S(3));
    }
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(pen);
}

static LRESULT CALLBACK CtxMenuProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    const int ih = S(38), pad = S(6);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        FillRC(dc, rc, C_WHITE);
        HPEN pen = CreatePen(PS_SOLID, 1, C_BOXBRD); HPEN op = (HPEN)SelectObject(dc, pen);
        HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, 0, 0, rc.right, rc.bottom, S(16), S(16));
        SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(pen);
        for (int i = 0; i < g_ctxN; i++) {
            int y = pad + i * ih;
            RECT r{ pad, y, rc.right - pad, y + ih };
            bool hov = (i == g_ctxHov);
            if (hov) FillRcRounded(dc, r, S(6), C_SEL);
            int cy = (r.top + r.bottom) / 2;
            CtxDrawIcon(dc, g_ctxItems[i].id, pad + S(20), cy, hov);
            RECT tr = r; tr.left = pad + S(38);
            DrawTextRC(dc, g_ctxItems[i].label, tr, hov ? C_BLUE_TXT : C_TEXT, g_fList);
            RECT sr = r; sr.right -= S(14);
            DrawTextRC(dc, g_ctxItems[i].shortcut, sr, hov ? RGB(120, 150, 190) : C_SUB, g_fSmall,
                       DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        // 需求2：只做悬停高亮；不因移出而关闭，避免误销毁
        int i = (GET_Y_LPARAM(lp) - pad) / ih;
        int nh = (i >= 0 && i < g_ctxN) ? i : -1;
        if (nh != g_ctxHov) { g_ctxHov = nh; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT sp{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ClientToScreen(h, &sp);
        RECT wr; GetWindowRect(h, &wr);
        if (!PtInRect(&wr, sp)) { DestroyWindow(h); return 0; }
        int i = (GET_Y_LPARAM(lp) - pad) / ih;
        if (i >= 0 && i < g_ctxN) CtxRun(g_ctxItems[i].id);
        DestroyWindow(h);
        SetForegroundWindow(g_hwnd);   // 执行完动作后把前台还给主窗口
        return 0;
    }
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        DestroyWindow(h);
        return 0;
    case WM_KEYDOWN:
        DestroyWindow(h);    // 需求3：按任意键（含快捷键）即关闭
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_KILLFOCUS:
        DestroyWindow(h);    // 需求2：焦点移到任何窗口（含其他应用）即关闭
        return 0;
    case WM_NCDESTROY:
        if (GetCapture() == h) ReleaseCapture();
        if (g_ctx == h) g_ctx = nullptr;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void ShowContextMenu(POINT pt) {
    if (g_ctx) { DestroyWindow(g_ctx); g_ctx = nullptr; }
    WNDCLASSW wc{};
    wc.lpfnWndProc = CtxMenuProc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"WbCtxMenu";
    wc.style = CS_DROPSHADOW;
    RegisterClassW(&wc);
    const int ih = S(38), pad = S(6);
    int wd = S(232), ht = pad * 2 + g_ctxN * ih;
    // 屏幕边缘翻转
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof mi };
    if (GetMonitorInfoW(mon, &mi)) {
        if (pt.y + ht > mi.rcWork.bottom) pt.y = mi.rcWork.bottom - ht - S(2);
        if (pt.x + wd > mi.rcWork.right) pt.x = mi.rcWork.right - wd - S(2);
    }
    g_ctxHov = -1; g_ctxEntered = false; g_ctxOrigin = pt;
    g_ctx = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"WbCtxMenu", nullptr,
                            WS_POPUP | WS_VISIBLE, pt.x, pt.y, wd, ht,
                            g_hwnd, nullptr, g_inst, nullptr);
    // 圆角裁剪（需求2）
    HRGN rgn = CreateRoundRectRgn(0, 0, wd, ht, S(16), S(16));
    SetWindowRgn(g_ctx, rgn, FALSE);
    SetWindowPos(g_ctx, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    // 需求2/3：菜单临时接管前台 → 点击任何地方（含其他应用）或按任意键都会触发
    // WM_KILLFOCUS/WM_KEYDOWN 而关闭，不再残留、不再叠加
    SetForegroundWindow(g_ctx);
    UpdateWindow(g_ctx);
    g_ctxEntered = true;
}

// ---------- 文件名列头的格式筛选下拉（需求7-11，Windows 类型筛选风格） ----------
static RECT FilterArrowRect() {
    int ax = gL.nameX + gL.nameW - S(26);
    return { ax, gL.header.top, gL.nameX + gL.nameW - S(4), gL.header.bottom };
}
static HWND g_typeMenu = nullptr;
static int  g_tmHov = -1;

// 下拉数据：当前分类（"全部"=所有结果）下实际存在的具体格式 + 数量，按数量降序
struct TmItem { std::wstring key; int count; };
static std::vector<TmItem> g_tmList;
static int  g_tmScroll = 0;
static const int TM_MAXVIS = 14;

static void TypeMenuToggle(const std::wstring& key) {
    auto it = std::find(g_typeSel.begin(), g_typeSel.end(), key);
    if (it != g_typeSel.end()) g_typeSel.erase(it);
    else g_typeSel.push_back(key);
    // Windows 式筛选语义：所有格式都被勾选 == 不过滤（等效全选）→ 归一为「全不勾」，
    // 避免出现「逐项全勾」与「全不勾」两种等价但显示不同的冗余状态。
    if (!g_tmList.empty() && g_typeSel.size() >= g_tmList.size()) g_typeSel.clear();
    RebuildView();
    UpdateStatusText();
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

static LRESULT CALLBACK TypeMenuProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    const int ih = S(34), pad = S(6);
    int n = (int)g_tmList.size();
    int vis = n < TM_MAXVIS ? n : TM_MAXVIS;
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        FillRC(dc, rc, C_WHITE);
        HPEN pen = CreatePen(PS_SOLID, 1, C_BOXBRD); HPEN op = (HPEN)SelectObject(dc, pen);
        HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, 0, 0, rc.right, rc.bottom, S(14), S(14));
        SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(pen);
        for (int row = 0; row < vis; row++) {
            int li = row + g_tmScroll;
            if (li < 0 || li >= n) break;
            int y = pad + row * ih;
            RECT r{ pad, y, rc.right - pad, y + ih };
            bool hov = (row == g_tmHov);
            if (hov) FillRcRounded(dc, r, S(6), C_SEL);
            // 复选框（默认全不勾 = 不过滤；勾选 = 只显示该格式）
            bool chk = false;
            for (const auto& s : g_typeSel) if (s == g_tmList[li].key) { chk = true; break; }
            RECT cb{ r.left + S(12), (r.top + r.bottom) / 2 - S(8), r.left + S(28), (r.top + r.bottom) / 2 + S(8) };
            HBRUSH bb = CreateSolidBrush(chk ? C_BTN : C_WHITE);
            HBRUSH obb = (HBRUSH)SelectObject(dc, bb);
            HPEN pb = CreatePen(PS_SOLID, 1, chk ? C_BTN : C_BOXBRD); HPEN opb = (HPEN)SelectObject(dc, pb);
            RoundRect(dc, cb.left, cb.top, cb.right, cb.bottom, S(4), S(4));
            SelectObject(dc, obb); SelectObject(dc, opb);
            DeleteObject(pb); DeleteObject(bb);
            if (chk) {
                HPEN pc = CreatePen(PS_SOLID, 2, C_WHITE); HPEN opc = (HPEN)SelectObject(dc, pc);
                MoveToEx(dc, cb.left + S(3), cb.top + S(8), nullptr);
                LineTo(dc, cb.left + S(7), cb.bottom - S(4));
                LineTo(dc, cb.right - S(3), cb.top + S(3));
                SelectObject(dc, opc); DeleteObject(pc);
            }
            // 格式名（具体扩展名）
            RECT tr = r; tr.left = cb.right + S(10); tr.right -= S(56);
            DrawTextRC(dc, g_tmList[li].key, tr, hov ? C_BLUE_TXT : C_TEXT, g_fList);
            // 数量（灰字右对齐）
            wchar_t cn[24]; swprintf(cn, 24, L"%d", g_tmList[li].count);
            RECT cr = r; cr.left = r.right - S(56);
            DrawTextRC(dc, cn, cr, C_SUB, g_fSmall, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
        }
        // 细滚动条（超过可见行数时）
        if (n > vis && vis > 0) {
            int trackH = rc.bottom - pad * 2;
            int thumbH = trackH * vis / n; if (thumbH < S(24)) thumbH = S(24);
            int maxScroll = n - vis; if (maxScroll < 1) maxScroll = 1;
            int tt = pad + (trackH - thumbH) * g_tmScroll / maxScroll;
            FillRcRounded(dc, { rc.right - S(6), tt, rc.right - S(2), tt + thumbH }, S(2), RGB(190, 198, 212));
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int row = (GET_Y_LPARAM(lp) - pad) / ih;
        int nh = (row >= 0 && row < vis) ? row : -1;
        if (nh != g_tmHov) { g_tmHov = nh; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int d = GET_WHEEL_DELTA_WPARAM(wp);
        int maxScroll = n - vis; if (maxScroll < 0) maxScroll = 0;
        int ns = g_tmScroll - (d / WHEEL_DELTA) * 3;
        if (ns < 0) ns = 0; if (ns > maxScroll) ns = maxScroll;
        if (ns != g_tmScroll) { g_tmScroll = ns; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int row = (GET_Y_LPARAM(lp) - pad) / ih;
        int li = row + g_tmScroll;
        if (row >= 0 && li >= 0 && li < n) TypeMenuToggle(g_tmList[li].key);   // 勾选后菜单保持打开，列表实时刷新
        return 0;
    }
    case WM_RBUTTONDOWN:
        DestroyWindow(h);
        return 0;
    case WM_KEYDOWN:
        DestroyWindow(h);
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_KILLFOCUS:
        DestroyWindow(h);
        return 0;
    case WM_NCDESTROY:
        if (g_typeMenu == h) g_typeMenu = nullptr;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void ShowTypeMenu() {
    if (g_typeMenu) { DestroyWindow(g_typeMenu); g_typeMenu = nullptr; }
    WNDCLASSW wc{};
    wc.lpfnWndProc = TypeMenuProc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"WbTypeMenu";
    wc.style = CS_DROPSHADOW;
    RegisterClassW(&wc);
    // 统计当前分类下实际存在的具体格式（没有的不出现；第四轮需求4）
    {
        std::vector<TmItem> tmp;
        for (const auto& r : g_results) {
            if (g_curCat != CAT_ALL && r.cat != g_curCat) continue;
            std::wstring k = ExtKeyOf(r);
            bool found = false;
            for (auto& t : tmp) if (t.key == k) { t.count++; found = true; break; }
            if (!found) tmp.push_back({ k, 1 });
        }
        std::stable_sort(tmp.begin(), tmp.end(), [](const TmItem& a, const TmItem& b) {
            if (a.count != b.count) return a.count > b.count;
            return CompareStringEx(LOCALE_NAME_INVARIANT, LINGUISTIC_IGNORECASE,
                                   a.key.c_str(), (int)a.key.size(),
                                   b.key.c_str(), (int)b.key.size(), nullptr, nullptr, 0) - 2 < 0;
        });
        g_tmList = std::move(tmp);
    }
    const int ih = S(34), pad = S(6);
    int n = (int)g_tmList.size();
    int vis = n < TM_MAXVIS ? n : TM_MAXVIS;
    int wd = S(210), ht = pad * 2 + vis * ih;
    int maxScroll = n - vis; if (maxScroll < 0) maxScroll = 0;
    if (g_tmScroll > maxScroll) g_tmScroll = maxScroll;
    RECT fa = FilterArrowRect();
    POINT pt{ fa.left - S(6), gL.header.bottom + 2 };
    ClientToScreen(g_hwnd, &pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof mi };
    if (GetMonitorInfoW(mon, &mi)) {
        if (pt.y + ht > mi.rcWork.bottom) pt.y = mi.rcWork.bottom - ht - S(2);
        if (pt.x + wd > mi.rcWork.right) pt.x = mi.rcWork.right - wd - S(2);
    }
    g_tmHov = -1;
    g_typeMenu = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"WbTypeMenu", nullptr,
                                 WS_POPUP | WS_VISIBLE, pt.x, pt.y, wd, ht,
                                 g_hwnd, nullptr, g_inst, nullptr);
    HRGN rgn = CreateRoundRectRgn(0, 0, wd, ht, S(14), S(14));
    SetWindowRgn(g_typeMenu, rgn, FALSE);
    SetWindowPos(g_typeMenu, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetForegroundWindow(g_typeMenu);
    UpdateWindow(g_typeMenu);
}

// edit subclass
static bool TryOpenPathFromEdit() {
    if (!g_edit) return false;
    wchar_t buf[1024] = {};
    GetWindowTextW(g_edit, buf, 1024);
    std::wstring p = buf;
    auto trim = [](std::wstring& s) {
        while (!s.empty() && (s.front() == L' ' || s.front() == L'\t')) s.erase(s.begin());
        while (!s.empty() && (s.back() == L' ' || s.back() == L'\t')) s.pop_back();
    };
    trim(p);
    if (p.size() >= 2 && p.front() == L'"' && p.back() == L'"')
        p = p.substr(1, p.size() - 2);
    trim(p);
    if (p.size() < 3) return false;

    wchar_t expanded[MAX_PATH * 2] = {};
    if (ExpandEnvironmentStringsW(p.c_str(), expanded, MAX_PATH * 2) > 0)
        p = expanded;

    const bool driveAbs = p.size() >= 2 && p[1] == L':' &&
                          ((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z'));
    const bool hasSep = p.find(L'\\') != std::wstring::npos ||
                        p.find(L'/') != std::wstring::npos;
    if (!driveAbs && !hasSep) return false;   // 普通关键词，不当路径

    const DWORD attr = GetFileAttributesW(p.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return false;

    std::wstring workDir;
    if (attr & FILE_ATTRIBUTE_DIRECTORY) workDir = p;
    else {
        size_t sl = p.find_last_of(L"\\/");
        if (sl != std::wstring::npos) workDir = p.substr(0, sl);
    }
    HINSTANCE r = ShellExecuteW(g_hwnd, L"open", p.c_str(), nullptr,
                                workDir.empty() ? nullptr : workDir.c_str(),
                                SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(r) > 32;
}

static LRESULT CALLBACK EditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_CHAR:
        SetTimer(g_hwnd, 1, 300, nullptr);
        break;
// 第7条需求：搜索框里直接输一个已存在的路径，回车就打开它（文件夹用资源
// 管理器打开，文件用默认程序打开）。不是路径就返回 false，照常搜索。
    case WM_KEYDOWN:
        // 需求1（第四轮）：回车 = 打开选中项；没有选中结果时才触发搜索
        if (wp == VK_RETURN) {
            KillTimer(g_hwnd, 1);
            if (TryOpenPathFromEdit()) return 0;   // 第7条：输的是路径就直接打开
            if (g_sel >= 0 && g_sel < (int)g_view.size()) {
                OpenItem(g_results[g_view[g_sel]], false);
            } else {
                DoSearch();
            }
            return 0;
        }
        break;
    case WM_PAINT: {
        LRESULT r = DefSubclassProc(h, msg, wp, lp);
        if (GetWindowTextLengthW(h) == 0 && GetFocus() != h) {
            HDC dc = GetDC(h);
            RECT rc; GetClientRect(h, &rc);
            rc.left += S(2);
            SetBkMode(dc, TRANSPARENT);
            DrawTextRC(dc, L"输入文件名关键词，多个词用空格分开（回车打开）", rc, C_SUB, g_fBig);
            ReleaseDC(h, dc);
        }
        return r;
    }
    case WM_SETFOCUS: case WM_KILLFOCUS:
        InvalidateRect(h, nullptr, FALSE);
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// main wndproc
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    // 单实例唤醒：另一进程发现已有实例后 PostMessage 本消息 → 把窗口叫到最前并聚焦搜索框。
    if (g_wmActivate && msg == g_wmActivate) {
        if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
        ShowWindow(h, SW_SHOW);
        UpdateWindow(h);                       // 先重画再交互，避免隐藏→显示时的透明闪烁
        SetForegroundWindow(h);
        g_visible = true;
        if (g_snap.load()->empty()) ReloadIndexFromCache();   // 隐藏时已释放，重新载入
        if (g_edit) SetFocus(g_edit);
        return 0;
    }
    switch (msg) {
    case WM_SHOWWINDOW:
        // 兜底：任何方式重新显示窗口（不只单实例唤醒）时，若隐藏期已释放索引则重载，
        // 否则会「窗口回来了但搜什么都是 0 条」。
        if (wp && g_wasHidden) {
            g_wasHidden = false;
            g_visible = true;
            ReloadIndexFromCache();
        }
        return 0;
    case WM_CREATE: {
        g_hwnd = h;
        g_dpi = GetDpiForWindow(h);
        MakeFonts();
        ComputeLayout();
        // search edit
        g_edit = CreateWindowExW(0, L"EDIT", nullptr,
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                 gL.edit.left, gL.edit.top, gL.edit.right - gL.edit.left, gL.edit.bottom - gL.edit.top,
                                 h, (HMENU)1, g_inst, nullptr);
        SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_fBig, TRUE);
        SetWindowSubclass(g_edit, EditProc, 1, 0);
        // 彻底去除原生边框（需求1：去掉黑色细框），靠白色底色+浅灰圆角框区分
        LONG ex = GetWindowLongPtrW(g_edit, GWL_EXSTYLE);
        SetWindowLongPtrW(g_edit, GWL_EXSTYLE, ex & ~WS_EX_CLIENTEDGE | WS_EX_COMPOSITED);   // 需求11：防闪烁
        SetWindowLongPtrW(g_edit, GWL_STYLE, GetWindowLongPtrW(g_edit, GWL_STYLE) & ~WS_BORDER);
        MARGINS mg{ 0, 0, 0, 1 };
        DwmExtendFrameIntoClientArea(h, &mg);
        return 0;
    }
    case WM_SIZE:
        if (g_pop) { DestroyWindow(g_pop); g_pop = nullptr; }
        if (g_ctx) { DestroyWindow(g_ctx); g_ctx = nullptr; }
        if (g_typeMenu) { DestroyWindow(g_typeMenu); g_typeMenu = nullptr; }
        ComputeLayout();
        ApplyRoundRegion();                 // 区域必须跟着新尺寸重设，否则右/下会留死区
        // 只标脏 + 立刻出一帧：WM_ERASEBKGND 已经 return 1，不需要 RDW_ERASE；
        // 也不再 RDW_ALLCHILDREN 强制子控件同步重画（那是搜索框闪烁的来源）。
        InvalidateRect(h, nullptr, FALSE);
        UpdateWindow(h);
        return 0;
    case WM_ENTERSIZEMOVE:
        // 拖动期间先撤掉窗口区域：区域+改尺寸是 MSDN 点名的「lost regions」组合
        if (g_roundByRgn) SetWindowRgn(h, nullptr, TRUE);
        return 0;
    case WM_EXITSIZEMOVE:
        ApplyRoundRegion();
        InvalidateRect(h, nullptr, FALSE);
        UpdateWindow(h);
        SaveConfig();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        PaintAll();
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize = { S(900), S(600) };
        return 0;
    }
    case WM_NCCALCSIZE: {
        // 无边框窗口：客户区 == 窗口矩形（只剩自绘标题栏）。
        // ★ 两个分支必须是同一个口径：wp=FALSE 时直接 break 交回 DefWindowProc，
        //   否则系统会按标准 WS_OVERLAPPEDWINDOW 再减一圈 frame+caption，
        //   客户区和窗口差一圈 → 中间夹出一条谁都不画的竖条。
        if (!wp) break;
        NCCALCSIZE_PARAMS* np = (NCCALCSIZE_PARAMS*)lp;
        if (IsZoomed(h) || IsIconic(h)) {
            // 最大化/最小化：贴监视器工作区（不压任务栏，也没有 SM_* 估算误差）
            MONITORINFO mi{ sizeof mi };
            if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi))
                np->rgrc[0] = mi.rcWork;
        }
        return 0;
    }
    case WM_NCHITTEST: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT wr; GetWindowRect(h, &wr);
        if (!IsZoomed(h)) {
            int e = S(6);
            bool l = pt.x < wr.left + e, r = pt.x >= wr.right - e;
            bool t = pt.y < wr.top + e, b = pt.y >= wr.bottom - e;
            if (l && t) return HTTOPLEFT;
            if (l && b) return HTBOTTOMLEFT;
            if (r && t) return HTTOPRIGHT;
            if (r && b) return HTBOTTOMRIGHT;
            if (l) return HTLEFT;
            if (r) return HTRIGHT;
            if (t) return HTTOP;
            if (b) return HTBOTTOM;
        }
        // client-space zones
        POINT cp = pt; ScreenToClient(h, &cp);
        int z = ZoneAt(cp.x, cp.y);
        // ★ 标题栏三个按钮**故意不返回 HTMINBUTTON/HTMAXBUTTON/HTCLOSE**：
        //   一旦返回，DWM 会认为这是系统原生的标题按钮，于是它自己又画一套
        //   「— □ ✕」盖在我们自绘的按钮上（用户看到的就是“图标贴在按钮上”），
        //   同时系统还会接管点击（最大化点不动、偶尔弹出一个小方块）。
        //   这里统一返回 HTCLIENT，由 WM_LBUTTONDOWN 自己处理三颗按钮。
        if (z == Z_TITLE) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_NCPAINT:
        // 无边框窗口不需要系统画非客户区；返回 0 可以避免最大化/还原瞬间
        // 那一下「标题框发白」（默认过程会先用窗口背景刷一遍 NC 区域）。
        return 0;
    case WM_NCACTIVATE:
        // 同理：别让默认过程重绘 NC 边框，直接告诉系统「已经处理好」
        return TRUE;
    case WM_NCMOUSEMOVE: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(h, &pt);
        int z = ZoneAt(pt.x, pt.y);
        int ht = 0;
        if (z == Z_CLOSE) ht = 3; else if (z == Z_MAX) ht = 2; else if (z == Z_MIN) ht = 1;
        if (ht != g_hoverTitle) {
            g_hoverTitle = ht;
            InvalidateRect(h, &gL.title, FALSE);
        }
        TRACKMOUSEEVENT tme{ sizeof tme, TME_LEAVE | TME_NONCLIENT, h, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_NCMOUSELEAVE:
        if (g_hoverTitle) { g_hoverTitle = 0; InvalidateRect(h, &gL.title, FALSE); }
        return 0;
    case WM_MOUSEMOVE: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int x = pt.x, y = pt.y;
        // 标题栏三颗按钮的悬停高亮：改成 HTCLIENT 后系统不再发 WM_NCMOUSEMOVE，
        // 只好在客户区的鼠标移动里自己维护（否则按钮永远不高亮）。
        if (!g_drag) {
            int tz = ZoneAt(x, y);
            int ht = (tz == Z_CLOSE) ? 3 : (tz == Z_MAX) ? 2 : (tz == Z_MIN) ? 1 : 0;
            if (ht != g_hoverTitle) {
                g_hoverTitle = ht;
                InvalidateRect(h, &gL.title, FALSE);
            }
        }
        if (g_drag == 1) { g_catW = x - g_dragOff; if (g_catW < S(120)) g_catW = S(120); if (g_catW > S(400)) g_catW = S(400); ComputeLayout(); InvalidateRect(h, nullptr, FALSE); return 0; }
        if (g_drag == 2) { g_prevW = gL.list.right - x - g_dragOff; if (g_prevW < S(180)) g_prevW = S(180); if (g_prevW > S(520)) g_prevW = S(520); ComputeLayout(); InvalidateRect(h, nullptr, FALSE); return 0; }
        if (g_drag == 3) {
            int pos = CatItemAt(y);
            int ty = pos < 0 ? (int)g_catOrder.size() : pos;
            if (!g_catDragActive && abs(y - g_catDragY) > S(5)) g_catDragActive = true;
            if (g_catDragActive) {
                g_catDragTo = ty;
                g_catDragY = y - gL.catItemH / 2;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        if (g_drag == 4) {
            // 需求6：三条列边界各自可拖，被抓的线严格跟随鼠标；
            // 拖动时按可用宽度限制，保证"修改时间"列仍有最小宽度，避免线不跟手
            int innerW = (gL.list.right - gL.list.left) - S(20);
            int minT = S(130);
            if (g_colDrag == 0) {
                int nw = x - gL.nameX;
                int maxN = innerW - gL.pathW - g_sizeW - minT;
                if (nw < S(150)) nw = S(150);
                if (nw > maxN) nw = maxN;
                if (nw < S(150)) nw = S(150);
                g_nameW = nw;
            } else if (g_colDrag == 1) {
                int pw = x - gL.pathX;
                int maxP = innerW - gL.nameW - g_sizeW - minT;
                if (pw < S(120)) pw = S(120);
                if (pw > maxP) pw = maxP;
                if (pw < S(120)) pw = S(120);
                g_pathW = pw;
            } else {
                int sw = x - gL.sizeX;
                int maxS = innerW - gL.nameW - gL.pathW - minT;
                if (sw < S(64)) sw = S(64);
                if (sw > maxS) sw = maxS;
                if (sw < S(64)) sw = S(64);
                g_sizeW = sw;
            }
            ComputeLayout(); InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (g_drag == 5) {
            int n = (int)g_view.size(), rows = VisibleRows();
            if (n > rows) {
                int trackH, thumbH, tt, ms;
                if (VScrollMetrics(trackH, thumbH, tt, ms)) {
                    int rel = y - gL.vscroll.top - (int)(g_vscrollAnchor * trackH);
                    int sc = (int)((__int64)rel * ms / (trackH - thumbH));
                    if (sc < 0) sc = 0; if (sc > ms) sc = ms;
                    if (sc != g_scroll) { g_scroll = sc; InvalidateRect(h, nullptr, FALSE); }
                }
            }
            return 0;
        }
        // hover
        int z = ZoneAt(x, y);
        int hovIdx = -1;
        if (z == Z_CAT) hovIdx = CatItemAt(y);
        if (z == Z_LIST) hovIdx = RowAt(y);
        if (z == Z_SCOPE) hovIdx = SegmentAt(x, y);
        if (z == Z_HEADER) hovIdx = ColAt(x);   // 需求7-11：列头格悬停高亮
        if (z != g_hoverZone || hovIdx != g_hoverIdx) {
            g_hoverZone = z; g_hoverIdx = hovIdx;
            InvalidateRect(h, nullptr, FALSE);
        }
        // cursor
        static HCURSOR curWE = LoadCursor(nullptr, IDC_SIZEWE);
        static HCURSOR curNS = LoadCursor(nullptr, IDC_SIZENS);
        static HCURSOR curArr = LoadCursor(nullptr, IDC_ARROW);
        HCURSOR cur = curArr;
        if (z == Z_SPLIT1 || z == Z_SPLIT2) cur = curWE;
        if (z == Z_HEADER) { int c; if (NearColSep(x, c)) cur = curWE; }
        SetCursor(cur);
        TRACKMOUSEEVENT tme{ sizeof tme, TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_hoverZone || g_hoverIdx >= 0) { g_hoverZone = 0; g_hoverIdx = -1; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONDOWN: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int x = pt.x, y = pt.y;
        // 点击主窗口任意位置：关闭已打开的范围弹窗 / 右键菜单 / 格式筛选
        if (g_pop) {
            RECT pr{}; GetWindowRect(g_pop, &pr); POINT sp = pt; ClientToScreen(h, &sp);
            if (!PtInRect(&pr, sp)) { DestroyWindow(g_pop); g_pop = nullptr; }
        }
        if (g_ctx) { DestroyWindow(g_ctx); g_ctx = nullptr; }
        if (g_typeMenu && !PtInRC(gL.header, x, y)) { DestroyWindow(g_typeMenu); g_typeMenu = nullptr; }
        int z = ZoneAt(x, y);
        // 标题栏三颗按钮 + 状态栏「刷新」：全部在客户区自己处理（见 WM_NCHITTEST）
        if (z == Z_MIN) { ShowWindow(h, SW_MINIMIZE); return 0; }
        if (z == Z_MAX) {
            if (IsZoomed(h)) ShowWindow(h, SW_RESTORE);
            else             ShowWindow(h, SW_MAXIMIZE);
            return 0;
        }
        if (z == Z_CLOSE) { PostMessageW(h, WM_CLOSE, 0, 0); return 0; }
        if (z == Z_REFRESH) { DoRefresh(); return 0; }
        g_downZone = z; g_downIdx = -1; g_btnDown = true;
        SetCapture(h);
        if (z == Z_SCOPE) { g_downIdx = SegmentAt(x, y); return 0; }
        if (z == Z_VSCROLL) {
            // 需求10：滚动条可拖动；点在滑块上跟随，点在轨道上直接跳转
            int trackH, thumbH, tt, ms;
            if (VScrollMetrics(trackH, thumbH, tt, ms)) {
                if (y >= tt && y < tt + thumbH) {
                    g_drag = 5;
                    g_vscrollAnchor = (double)(y - tt) / (double)trackH;   // 需求10：用 double 保存抓取锚点
                } else {
                    int sc = (int)((__int64)(y - gL.vscroll.top - thumbH / 2) * ms / (trackH - thumbH));
                    if (sc < 0) sc = 0; if (sc > ms) sc = ms;
                    g_scroll = sc;
                    g_drag = 5;
                    g_vscrollAnchor = (double)thumbH / 2 / (double)trackH;
                    InvalidateRect(h, nullptr, FALSE);
                }
            }
            return 0;
        }
        if (z == Z_SPLIT1) { g_drag = 1; g_dragOff = x - g_catW; return 0; }
        if (z == Z_SPLIT2) { g_drag = 2; g_dragOff = gL.list.right - x - g_prevW; return 0; }
        if (z == Z_HEADER) {
            int c;
            if (NearColSep(x, c)) { g_drag = 4; g_colDrag = c; return 0; }
            RECT fa = FilterArrowRect();
            if (ColAt(x) == 0 && PtInRC(fa, x, y)) { ShowTypeMenu(); return 0; }   // 需求7-11
            int col = ColAt(x);
            if (col >= 0) {
                if (g_sortCol == col) g_sortAsc = !g_sortAsc; else { g_sortCol = col; g_sortAsc = true; }
                ApplySortAndView();
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        if (z == Z_CAT) {
            int pos = CatItemAt(y);
            if (pos >= 0) {
                g_catDragFrom = pos; g_catDragTo = pos; g_catDragY = y;
                g_drag = 3;
            }
            return 0;
        }
        if (z == Z_LIST) {
            int idx = RowAt(y);
            if (idx >= 0 && idx != g_sel) { g_sel = idx; InvalidateRect(h, nullptr, FALSE); }
            // 手动双击检测：不依赖 WM_LBUTTONDBLCLK / CS_DBLCLKS（某些环境下该消息不生成，
            // 导致双击被拆成两次普通点击、只选中不打开）。改为在第二次 WM_LBUTTONDOWN 时
            // 判断「与上一次同属一行且间隔 < 系统双击时限」即视为双击。
            if (idx >= 0) {
                DWORD now = GetMessageTime();
                DWORD dct = GetDoubleClickTime(); if (dct == 0) dct = 500;
                if (g_dblRow == idx && (DWORD)(now - g_dblTime) < dct) {
                    if (const Res* r = SelectedRes()) OpenItem(*r, ColAt(x) == 1);
                    g_dblRow = -1; g_dblTime = 0;   // 清状态，防连锁/重复触发
                } else {
                    g_dblRow = idx; g_dblTime = now;
                }
            }
            return 0;
        }
        if (z == Z_CHK) { g_subfolders = !g_subfolders; SaveConfig(); InvalidateRect(h, nullptr, FALSE); return 0; }
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        // 双击打开逻辑已迁移到 WM_LBUTTONDOWN(Z_LIST) 的手动双击检测（不依赖 CS_DBLCLKS）。
        // 移除 CS_DBLCLKS 后系统不再生成此消息，保留空 case 无害。
        return 0;
    }
    case WM_LBUTTONUP: {
        if (GetCapture() == h) ReleaseCapture();
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int x = pt.x, y = pt.y;
        int z = ZoneAt(x, y);
        if (g_drag == 3) {
            if (g_catDragActive && g_catDragFrom >= 0 && g_catDragTo != g_catDragFrom) {
                int id = g_catOrder[g_catDragFrom];
                g_catOrder.erase(g_catOrder.begin() + g_catDragFrom);
                int to = g_catDragTo; if (to > g_catDragFrom) to--;
                if (to < 0) to = 0; if (to > (int)g_catOrder.size()) to = (int)g_catOrder.size();
                g_catOrder.insert(g_catOrder.begin() + to, id);
                SaveConfig();
            } else if (!g_catDragActive && g_catDragFrom >= 0) {
                // 需求4：未发生拖动 → 视为单击，切换选中分类
                int pos = g_catDragFrom;
                g_drag = 0; g_catDragFrom = g_catDragTo = -1; g_catDragActive = false;
                SelectCat(pos);
                return 0;
            }
            g_drag = 0; g_catDragFrom = g_catDragTo = -1; g_catDragActive = false;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (g_drag == 1 || g_drag == 2) { g_drag = 0; SaveConfig(); InvalidateRect(h, nullptr, FALSE); return 0; }
        if (g_drag == 4) { g_drag = 0; g_colDrag = -1; SaveConfig(); InvalidateRect(h, nullptr, FALSE); return 0; }
        if (g_drag == 5) { g_drag = 0; return 0; }
        g_btnDown = false;
        if (z == Z_BTN && g_downZone == Z_BTN) { DoSearch(); }
        else if (z == Z_SCOPE && g_downZone == Z_SCOPE && g_downIdx >= 0) {
            int seg = SegmentAt(x, y);
            if (seg == g_downIdx) ShowScopePopup(seg);   // 需求1：按段弹出
        }
        else if (z == Z_OPEN && g_downZone == Z_OPEN) { const Res* r = SelectedRes(); if (r) OpenItem(*r, false); }
        else if (z == Z_OPENF && g_downZone == Z_OPENF) { const Res* r = SelectedRes(); if (r) OpenItem(*r, true); }
        g_downZone = 0; g_downIdx = -1;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        if (g_pop) { SendMessageW(g_pop, msg, wp, lp); return 0; }   // 弹窗打开时滚轮转发
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(h, &pt);
        if (PtInRC(gL.list, pt.x, pt.y) || PtInRC(gL.header, pt.x, pt.y)) {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            int rows = VisibleRows();
            g_scroll -= (delta / WHEEL_DELTA) * 3;
            ClampScroll();
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_RBUTTONDOWN: {
        if (g_pop) { DestroyWindow(g_pop); g_pop = nullptr; }
        if (g_typeMenu) { DestroyWindow(g_typeMenu); g_typeMenu = nullptr; }
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int z = ZoneAt(pt.x, pt.y);
        if (z == Z_LIST) {
            int idx = RowAt(pt.y);
            if (idx >= 0) {
                if (idx != g_sel) { g_sel = idx; InvalidateRect(h, nullptr, FALSE); }
            }
        }
        return 0;
    }
    case WM_CONTEXTMENU: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (pt.x == -1 && pt.y == -1) return 0;
        POINT cp = pt; ScreenToClient(h, &cp);
        int z = ZoneAt(cp.x, cp.y);
        if (z == Z_CLOSE) {                       // D4：关闭按钮右键 → 退出菜单
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, 1, L"退出");
            int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, nullptr);
            DestroyMenu(m);
            if (cmd == 1) PostMessageW(h, WM_APP_EXIT, 0, 0);
            return 0;
        }
        if (z != Z_LIST) return 0;
        int idx = RowAt(cp.y);
        if (idx < 0) return 0;
        g_sel = idx;
        InvalidateRect(h, nullptr, FALSE);
        ShowContextMenu(pt);   // 需求2：自绘菜单
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        // 搜索框底色纯白、文字色统一（配合防闪烁）
        HDC dc = (HDC)wp;
        SetBkColor(dc, C_WHITE);
        SetTextColor(dc, C_TEXT);
        static HBRUSH wb = CreateSolidBrush(C_WHITE);
        return (LRESULT)wb;
    }
    case WM_TIMER:
        if (wp == 1) { KillTimer(h, 1); DoSearch(); }
        return 0;
    case WM_COMMAND:
        // (context menu uses TPM_RETURNCMD; nothing here)
        return 0;
    case WM_APP + 9:
        DoSearch();
        return 0;
    case WM_APP_SEARCHDONE: {
        DoneMsg* m = (DoneMsg*)lp;
        if (m->epoch == g_curEpoch) {
            g_results = std::move(*m->items);
            g_resultKeys.clear();   // 结果集已变，扩展名缓存失效
            g_totalFound = m->total;
            // 需求12：角标与列表严格同源——从"实际保存的结果"重新统计，
            // 超过 MAX_STORED 时全量数字只出现在状态栏说明里，避免对不上
            memset(g_catCount, 0, sizeof g_catCount);
            for (const auto& r : g_results) g_catCount[r.cat]++;
            g_catCount[CAT_ALL] = (uint32_t)g_results.size();
            g_searching = false;
            g_sel = -1; g_scroll = 0;
            ApplySortAndView();
            if (!g_view.empty()) g_sel = 0;
            ComputeLayout();
        }
        delete m->items; delete m;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_APP_RESCAN_DONE:
        if (g_curEpoch) DoSearch();   // 可见期重扫完成 → 用新快照重跑当前查询
        return 0;
    case WM_APP_EXIT:
        BeginExit(h);
        return 0;
    case WM_APP_INDEXSTAT:
        InvalidateRect(h, &gL.status, FALSE);
        return 0;
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);
        MakeFonts();
        RECT* r = (RECT*)lp;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_CLOSE: {
        // 关窗=隐藏，不退出；但必须真释放索引/结果（否则 ~130MB 不归还）。
        g_visible = false;
        ++g_epoch; g_curEpoch = 0;                   // 丢弃在飞结果并复位到“未搜索”
        if (g_edit) SetWindowTextW(g_edit, L"");     // 清空搜索框，下次打开不残留上次关键词
        g_results.clear();     g_results.shrink_to_fit();
        g_view.clear();        g_view.shrink_to_fit();
        g_resultKeys.clear();  g_resultKeys.shrink_to_fit();
        g_totalFound = 0;
        memset(g_catCount, 0, sizeof g_catCount);
        g_sel = -1; g_scroll = 0;
        g_snap.store(std::make_shared<Snapshot>());  // 释放整份索引（搜索线程若在飞，结束后自然释放）
        // 隐藏时用不到绘制后备位图（约 3MB 私有内存）：一并释放，下次绘制按尺寸重建。
        if (g_memDc) {
            if (g_memOldBmp) SelectObject(g_memDc, g_memOldBmp);
            DeleteDC(g_memDc); g_memDc = nullptr; g_memOldBmp = nullptr;
        }
        if (g_memBmp) { DeleteObject(g_memBmp); g_memBmp = nullptr; }
        g_memSize = SIZE{};
        SaveConfig();
        // 结果集里每条结果名是独立小分配（5 万条约 4–9MB），clear/shrink 只还大块，
        // 小分配块仍留在 CRT 堆段里占着 Private。压实两次把空段还给系统。
        _heapmin();
        HeapCompact(GetProcessHeap(), 0);
        EmptyWorkingSet(GetCurrentProcess());        // 降工作集；大 vector 已析构降 Private
        ShowWindow(h, SW_HIDE);
        g_wasHidden = true;
        return 0;
    }
    case WM_DESTROY:
        if (g_memDc) {
            if (g_memOldBmp) SelectObject(g_memDc, g_memOldBmp);
            DeleteDC(g_memDc); g_memDc = nullptr; g_memOldBmp = nullptr;
        }
        if (g_memBmp) { DeleteObject(g_memBmp); g_memBmp = nullptr; }
        SaveConfig();
        g_visible = false;
        g_hwnd = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// periodic index status refresh via worker → post to UI
static void IndexStatusPump(std::stop_token st) {
    while (!st.stop_requested()) {
        if (g_hwnd) PostMessageW(g_hwnd, WM_APP_INDEXSTAT, 0, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR, int nCmdShow) {
    g_inst = hi;
    // DPI awareness (manifest covers, but ensure)
    if (FAILED(SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))) {
        SetProcessDPIAware();
    }
    INITCOMMONCONTROLSEX icc{ sizeof icc, ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    // 单实例：已在运行时把已有窗口叫到最前并聚焦搜索框，本次直接退出，避免重复开窗。
    g_wmActivate = RegisterWindowMessageW(L"JisuqiSearchActivate");
    g_singleMtx = CreateMutexW(nullptr, TRUE, L"JisuqiSearch_SingleInstance");
    if (g_singleMtx && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"JisuqiFullDiskSearch", nullptr);
        if (existing) {
            if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
            ShowWindow(existing, SW_SHOW);
            SetForegroundWindow(existing);
            if (g_wmActivate) PostMessageW(existing, g_wmActivate, 0, 0);
        }
        CloseHandle(g_singleMtx);
        return 0;
    }

    // 解析主程序传入的数据目录：--datadir="<path>"
    // 未传（独立运行）时退回默认 <exeDir>\data，保持向后兼容。
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            for (int i = 0; i < argc; i++) {
                std::wstring a = argv[i];
                if (a.rfind(L"--datadir=", 0) == 0) {
                    g_dataRoot = a.substr(10);
                    if (!g_dataRoot.empty() && g_dataRoot.front() == L'"')
                        g_dataRoot = g_dataRoot.substr(1);
                    if (!g_dataRoot.empty() && g_dataRoot.back() == L'"')
                        g_dataRoot = g_dataRoot.substr(0, g_dataRoot.size() - 1);
                    break;
                }
            }
            LocalFree(argv);
        }
    }

    LoadConfig();

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"JisuqiFullDiskSearch";
    wc.hIcon = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(101), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    wc.style = CS_HREDRAW | CS_VREDRAW;   // 不含 CS_DBLCLKS：双击改由 WM_LBUTTONDOWN 手动检测，避免该消息在某些环境下不生成导致双击失效
    RegisterClassW(&wc);

    // default window size: large, centered, remember if config present
    int x = g_wp.rcNormalPosition.left, y = g_wp.rcNormalPosition.top,
        w = g_wp.rcNormalPosition.right - g_wp.rcNormalPosition.left,
        hh = g_wp.rcNormalPosition.bottom - g_wp.rcNormalPosition.top;
    if (w <= 400 || hh <= 300) {
        RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        w = 1280; hh = 800;
        if (w > wa.right - wa.left - 40) w = wa.right - wa.left - 40;
        if (hh > wa.bottom - wa.top - 40) hh = wa.bottom - wa.top - 40;
        x = wa.left + ((wa.right - wa.left) - w) / 2;
        y = wa.top + ((wa.bottom - wa.top) - hh) / 2;
    }
    HWND hwnd = CreateWindowExW(0, L"JisuqiFullDiskSearch", L"极速启 · 全盘文件搜索",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,   // 先不 WS_VISIBLE：首帧画完再显示，避免“搜索框透明/穿透”闪烁
                                x, y, w, hh, nullptr, nullptr, hi, nullptr);
    if (!hwnd) return 1;
    // force non-client recalc so the custom (borderless) frame applies immediately
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    // 第四轮需求6：窗口外圈八角倒角。Win11 走 DWM 圆角（抗锯齿）；
    // Win10 必然不支持该属性 → 记下标记，改用 GDI 区域（由 ApplyRoundRegion 随尺寸重设）
    {
        DWM_WINDOW_CORNER_PREFERENCE pref = DWMWCP_ROUND;
        if (FAILED(DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref)))) {
            g_roundByRgn = true;
            ApplyRoundRegion();
        }
    }
    // 先完成布局与首帧绘制，再显示窗口，并从任务栏恢复时同样先重画，消除初始透明。
    ShowWindow(hwnd, g_wp.showCmd ? g_wp.showCmd : SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(g_edit);

    // threads
    std::jthread indexer([](std::stop_token st) { IndexerThread(st); });
    std::jthread searcher([](std::stop_token st) { SearcherThread(st); });
    std::jthread pump([](std::stop_token st) { IndexStatusPump(st); });
    std::jthread scheduler([](std::stop_token st) { SchedulerThread(st); });
    // 每个固定盘一个监听线程（同步 RDCW，只标脏）
    {
        DWORD drives = GetLogicalDrives();
        for (int i = 0; i < 26; i++) {
            if (!(drives & (1u << i))) continue;
            wchar_t letter = (wchar_t)('A' + i);
            wchar_t root[4] = { letter, L':', L'\\', 0 };
            if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
            HANDLE th = CreateThread(nullptr, 0, ListenerProc, (LPVOID)(INT_PTR)i, 0, nullptr);
            if (th) g_listenThreadH[i] = th;
        }
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        // 全局快捷键（需求4/5）：Ctrl+F 聚焦搜索框、Enter 打开、Ctrl+E 打开目录、Ctrl+Shift+C 复制路径
        if (msg.message == WM_KEYDOWN && (msg.hwnd == g_hwnd || msg.hwnd == g_edit)) {
            bool ctrl  = GetKeyState(VK_CONTROL) < 0;
            bool shift = GetKeyState(VK_SHIFT) < 0;
            WPARAM vk  = msg.wParam;
            if (!ctrl && !shift && vk == VK_ESCAPE) {
                PostMessageW(g_hwnd, WM_CLOSE, 0, 0);   // ESC = 关闭（隐藏）窗口
            } else if (ctrl && !shift && vk == 'F') {
                SetFocus(g_edit);
            } else if (ctrl && !shift && vk == 'E') {
                if (const Res* r = SelectedRes()) OpenItem(*r, true);
            } else if (ctrl && shift && vk == 'C') {
                if (const Res* r = SelectedRes()) CopyPath(*r);
            } else if (!ctrl && !shift && vk == VK_RETURN && msg.hwnd == g_hwnd) {
                if (const Res* r = SelectedRes()) OpenItem(*r, false);
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    // 索引线程停在 g_refreshCv.wait_for(30min) 里，必须显式 notify 唤醒它，
    // 否则主线程 join 会一直阻塞到 30 分钟超时，进程关不掉、内存不释放。
    indexer.request_stop(); g_refreshCv.notify_all();
    searcher.request_stop(); g_reqCv.notify_all();
    pump.request_stop();
    scheduler.request_stop();
    // 兜底：确保监听线程退出并回收句柄（正常路径已由 BeginExit 处理）
    {
        std::lock_guard<std::mutex> lk(g_listenMu);
        for (int i = 0; i < 26; i++)
            if (g_listenHandle[i]) CancelSynchronousIo(g_listenHandle[i]);
    }
    for (int i = 0; i < 26; i++) {
        if (g_listenThreadH[i]) {
            WaitForSingleObject(g_listenThreadH[i], 3000);
            CloseHandle(g_listenThreadH[i]);
            g_listenThreadH[i] = nullptr;
        }
    }
    // drain pending app messages (free payloads)
    MSG m2;
    while (PeekMessageW(&m2, nullptr, WM_APP, WM_APP + 20, PM_REMOVE)) {
        if (m2.message == WM_APP_SEARCHDONE && m2.lParam) {
            DoneMsg* m = (DoneMsg*)m2.lParam;
            delete m->items; delete m;
        }
    }
    return (int)msg.wParam;
}
