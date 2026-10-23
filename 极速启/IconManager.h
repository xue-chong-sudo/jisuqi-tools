#ifndef BONNY_ICONMANAGER_H
#define BONNY_ICONMANAGER_H

#include <windows.h>
#include <gdiplus.h>
#include <string>
#include <map>
#include <vector>
#include "Config.h"

// Windows headers define ExtractIcon as a macro (-> ExtractIconW/A) which
// would clobber our member helper of the same name. Undefine it so the
// member function is used as intended.
#ifdef ExtractIcon
#undef ExtractIcon
#endif

namespace JisuQi {

// Extracts system shell icons (matching Explorer), caches them as PNG
// files under <exedir>/data/icons, and provides GDI+ bitmaps for drawing.
class IconManager {
public:
    void SetIconsDir(const std::wstring& dir) { m_iconsDir = dir; }

    // Returns the cached icon file name for a real path, creating the
    // PNG on first use. The file is named after the target's own file name
    // ("QQ.exe" -> "QQ.exe.png"); a second target with the same base name
    // gets "QQ.exe (2).png". A path -> file index (data/icons/_index.ini)
    // keeps one path bound to one file across restarts.
    std::wstring GetIconFileName(const std::wstring& realPath, ItemType type);

    // 用户自己挑的图标：把任意图片（ico/png/jpg/bmp/gif）或某个 exe 的图标
    // 转成 PNG 存进 icons 目录，返回文件名（失败返回空串）。realPath 只用来
    // 生成好认的文件名（"QQ.exe_custom.png"）。
    std::wstring ImportCustomIcon(const std::wstring& srcPath,
                                 const std::wstring& realPath);

    // 丢掉某个图标文件名的内存位图（换图标后必须调用，否则仍画旧图）。
    void EvictBitmap(const std::wstring& iconFileName);

    // Returns a (cached) GDI+ bitmap for drawing; falls back to the
    // system "unknown file" icon if the PNG is missing/corrupt.
    Gdiplus::Bitmap* GetBitmap(const std::wstring& iconFileName);

    // Warm the cache for all items.
    void PreloadAll(const std::vector<Item>& items);

    // Delete any legacy (pre-v2) cached icon PNGs exactly once, then stamp
    // a version marker so this never re-runs. Call after SetIconsDir().
    void InvalidateOldCache();

    // Free all cached bitmaps (called on shutdown).
    void ClearCache();

    // Returns the cached file name (e.g. "recyclebin.png") for the system
    // Recycle Bin stock icon, generating the PNG on first use.
    std::wstring GetRecycleBinIconFile();

    // Resolve a .lnk to its real target path. Returns false on failure.
    static bool ResolveLink(const std::wstring& lnkPath, std::wstring& outTarget);

    // Decide item type from path (and whether it arrived as a .lnk).
    static ItemType GuessType(const std::wstring& path, bool wasLink);

    // Display name derived from a dropped path.
    static std::wstring BaseName(const std::wstring& path, bool wasLink);

private:
    std::wstring m_iconsDir;
    std::map<std::wstring, Gdiplus::Bitmap*> m_cache;

    static std::wstring HashPath(const std::wstring& path);
    static HICON FallbackIcon();
    static Gdiplus::Bitmap* ToBitmap(HICON hicon);
    static bool SaveBitmapPng(Gdiplus::Bitmap* bmp, const std::wstring& path);
    static bool GetEncoderClsid(const wchar_t* mime, CLSID& clsid);

    // ---- 图标文件命名（第8条需求：用原始文件名，重名自动加序号）----
    static std::wstring SanitizeName(const std::wstring& s);
    std::wstring IndexPath() const;                       // <icons>/_index.ini
    std::wstring LookupIndex(const std::wstring& path) const;
    void         StoreIndex(const std::wstring& path, const std::wstring& fname);
    std::wstring AllocIconName(const std::wstring& base) const;  // 避开已占用的名字
    // 把一个图片文件/exe 读成位图；失败返回 nullptr（调用方 delete）
    static Gdiplus::Bitmap* LoadImageAny(const std::wstring& srcPath);

    // Extract the system shell icon for a path, Explorer-style. Defined
    // out-of-line in IconManager.cpp because it needs commctrl internals.
    static HICON ExtractIcon(const std::wstring& path);
};

} // namespace JisuQi

#endif // BONNY_ICONMANAGER_H
