#define _WIN32_IE 0x0600
#include "IconManager.h"
#include <shlobj.h>
#include <shellapi.h>
#include <objidl.h>
#include <commctrl.h>
#include <commoncontrols.h>
#include <vector>
#include <cstdio>
#include <fstream>

namespace JisuQi {

std::wstring IconManager::HashPath(const std::wstring& path) {
    // FNV-1a 64-bit over the lower-cased path. Stable across runs and
    // machines, so the same target reuses one cached PNG.
    unsigned long long h = 1469598103934665603ULL;
    for (wchar_t c : path) {
        c = static_cast<wchar_t>(towlower(c));
        h ^= static_cast<unsigned long long>(c);
        h *= 1099511628211ULL;
    }
    wchar_t buf[32];
    swprintf(buf, 31, L"%016llx", h);
    return std::wstring(buf);
}

HICON IconManager::FallbackIcon() {
    SHSTOCKICONINFO sii = {sizeof(sii)};
    if (SUCCEEDED(SHGetStockIconInfo(SIID_DOCNOASSOC,
                                     SHGSI_ICON | SHGSI_SMALLICON, &sii))) {
        return sii.hIcon;                 // 调用方持有，可 DestroyIcon
    }
    // 退回到系统默认图标：LoadIconW 返回的是“共享”图标，绝不能 DestroyIcon。
    // 复制一份私有副本，维持“本函数返回的 HICON 一律可 DestroyIcon”的约定。
    HICON shared = LoadIconW(nullptr, MAKEINTRESOURCEW(IDI_APPLICATION));
    return shared ? CopyIcon(shared) : nullptr;
}

Gdiplus::Bitmap* IconManager::ToBitmap(HICON hicon) {
    if (!hicon) return nullptr;
    ICONINFO ii = {sizeof(ii)};
    if (!GetIconInfo(hicon, &ii)) return Gdiplus::Bitmap::FromHICON(hicon);
    Gdiplus::Bitmap* result = nullptr;
    do {
        if (!ii.hbmColor || !ii.hbmMask) break;
        BITMAP cm = {}, mm = {};
        if (!GetObject(ii.hbmColor, sizeof(cm), &cm) ||
            !GetObject(ii.hbmMask, sizeof(mm), &mm)) break;
        int w = cm.bmWidth;
        int h = cm.bmHeight < 0 ? -cm.bmHeight : cm.bmHeight;
        if (w <= 0 || h <= 0) break;
        // Cap icon size to avoid runaway allocations on corrupt HICONs.
        if (w > 512 || h > 512) break;
        std::vector<BYTE> color(static_cast<size_t>(w) * h * 4);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;   // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        HDC dc = GetDC(nullptr);
        bool okC = GetDIBits(dc, ii.hbmColor, 0, h, color.data(), &bi,
                             DIB_RGB_COLORS) == h;
        ReleaseDC(nullptr, dc);
        if (!okC) break;
        // Does the color data carry a real alpha channel?
        bool hasAlpha = false;
        for (size_t i = 3; i < color.size(); i += 4)
            if (color[i]) { hasAlpha = true; break; }
        if (!hasAlpha) {
            // Legacy masked icon: derive alpha from the AND mask (bottom
            // half of the icon's hbmMask). hbmMask is conceptually a 1bpp
            // bitmap that is twice the icon height: top half is the XOR
            // mask, bottom half is the AND (opacity) mask. We read the
            // bottom half into a top-down DIB and use the AND bit to set
            // per-pixel alpha (bit set -> transparent).
            const int maskRow = (w + 31) / 32 * 4;
            std::vector<BYTE> mask(static_cast<size_t>(maskRow) * h);
            BITMAPINFO bmi2 = bi;
            bmi2.bmiHeader.biBitCount = 1;
            HDC dc2 = GetDC(nullptr);
            // startScan = h, cScan = h reads the bottom half (AND mask).
            bool okM = GetDIBits(dc2, ii.hbmMask, h, h, mask.data(), &bmi2,
                                 DIB_RGB_COLORS) == h;
            ReleaseDC(nullptr, dc2);
            if (!okM) break; // fallback to FromHICON
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    BYTE* px = &color[(static_cast<size_t>(y) * w + x) * 4];
                    int bit = mask[static_cast<size_t>(y) * maskRow + x / 8]
                              & (0x80 >> (x % 8));
                    px[3] = bit ? 0 : 255;
                }
            }
        }
        // Create the resulting bitmap and copy the (now alpha-correct)
        // pixel data into it via LockBits. This avoids the external-scan0
        // constructor and its ownership subtleties.
        result = new Gdiplus::Bitmap(w, h, PixelFormat32bppARGB);
        if (!result || result->GetLastStatus() != Gdiplus::Ok) {
            delete result;
            result = nullptr;
            break;
        }
        Gdiplus::BitmapData bd = {};
        Gdiplus::Rect rect(0, 0, w, h);
        if (result->LockBits(&rect, Gdiplus::ImageLockModeWrite,
                             PixelFormat32bppARGB, &bd) == Gdiplus::Ok) {
            const int stride = w * 4;
            for (int y = 0; y < h; ++y) {
                BYTE* dst = static_cast<BYTE*>(bd.Scan0) + y * bd.Stride;
                const BYTE* src = color.data() + y * stride;
                memcpy(dst, src, stride);
            }
            result->UnlockBits(&bd);
        } else {
            delete result;
            result = nullptr;
        }
    } while (false);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (!result) result = Gdiplus::Bitmap::FromHICON(hicon); // legacy fallback
    if (result && result->GetLastStatus() != Gdiplus::Ok) {
        delete result;
        result = nullptr;
    }
    return result;
}

bool IconManager::GetEncoderClsid(const wchar_t* mime, CLSID& clsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (num == 0 || size == 0) return false;
    std::vector<BYTE> buf(size);
    Gdiplus::ImageCodecInfo* p = reinterpret_cast<Gdiplus::ImageCodecInfo*>(&buf[0]);
    if (Gdiplus::GetImageEncoders(num, size, p) != Gdiplus::Ok) return false;
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(p[i].MimeType, mime) == 0) {
            clsid = p[i].Clsid;
            return true;
        }
    }
    return false;
}

bool IconManager::SaveBitmapPng(Gdiplus::Bitmap* bmp, const std::wstring& path) {
    CLSID clsid;
    if (!GetEncoderClsid(L"image/png", clsid)) return false;
    return bmp->Save(path.c_str(), &clsid, nullptr) == Gdiplus::Ok;
}

bool IconManager::ResolveLink(const std::wstring& lnkPath, std::wstring& outTarget) {
    IShellLinkW* psl = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                  reinterpret_cast<void**>(&psl));
    if (FAILED(hr) || !psl) return false;

    IPersistFile* ppf = nullptr;
    hr = psl->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&ppf));
    bool ok = false;
    if (SUCCEEDED(hr) && ppf) {
        hr = ppf->Load(lnkPath.c_str(), STGM_READ);
        if (SUCCEEDED(hr)) {
            wchar_t buf[MAX_PATH * 2] = {0};
            WIN32_FIND_DATAW fd = {0};
            if (SUCCEEDED(psl->GetPath(buf, MAX_PATH * 2, &fd, SLGP_UNCPRIORITY)) && buf[0]) {
                outTarget = buf;
                ok = true;
            }
        }
        ppf->Release();
    }
    psl->Release();
    return ok;
}

ItemType IconManager::GuessType(const std::wstring& path, bool wasLink) {
    if (wasLink) return ItemType::Link;
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES &&
        (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        return ItemType::Folder;
    }
    std::wstring lower = ToLower(path);
    if (EndsWith(lower, L".exe") || EndsWith(lower, L".msi") ||
        EndsWith(lower, L".bat") || EndsWith(lower, L".cmd") ||
        EndsWith(lower, L".com")) {
        return ItemType::Exe;
    }
    return ItemType::File;
}

std::wstring IconManager::BaseName(const std::wstring& path, bool wasLink) {
    std::wstring s = path;
    size_t sl = s.find_last_of(L"\\/");
    if (sl != std::wstring::npos) s = s.substr(sl + 1);
    if (wasLink) {
        std::wstring lower = ToLower(s);
        if (EndsWith(lower, L".lnk")) s = s.substr(0, s.size() - 4);
    }
    return s;
}

std::wstring IconManager::SanitizeName(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        // Windows 文件名非法字符 + 控制字符一律换成下划线
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) out.push_back(L'_');
        else out.push_back(c);
    }
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.')) out.pop_back();
    if (out.size() > 96) out.resize(96);
    return out;
}

std::wstring IconManager::IndexPath() const {
    return m_iconsDir + L"\\_index.ini";
}

// 索引文件用 UTF-16 存（先写 BOM），否则含中文的路径会被按 ANSI 写坏。
static void EnsureIniW(const std::wstring& path) {
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    std::ofstream f(path.c_str(), std::ios::binary);
    if (!f.good()) return;
    const unsigned char bom[2] = {0xFF, 0xFE};
    f.write(reinterpret_cast<const char*>(bom), 2);
    f.close();
}

std::wstring IconManager::LookupIndex(const std::wstring& path) const {
    if (m_iconsDir.empty() || path.empty()) return std::wstring();
    std::wstring ini = IndexPath();
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES)
        return std::wstring();
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetPrivateProfileStringW(L"icons", path.c_str(), L"", buf,
                                       MAX_PATH * 2, ini.c_str());
    return n ? std::wstring(buf, n) : std::wstring();
}

void IconManager::StoreIndex(const std::wstring& path, const std::wstring& fname) {
    if (m_iconsDir.empty() || path.empty() || fname.empty()) return;
    std::wstring ini = IndexPath();
    EnsureIniW(ini);
    WritePrivateProfileStringW(L"icons", path.c_str(), fname.c_str(), ini.c_str());
}

// 给 base 找一个还没被占用的图标文件名（同名自动加 (2)(3)…）
std::wstring IconManager::AllocIconName(const std::wstring& base) const {
    std::wstring stem = base.empty() ? std::wstring(L"icon") : base;
    std::wstring cand = stem + L".png";
    auto exists = [this](const std::wstring& n) {
        std::wstring full = m_iconsDir + L"\\" + n;
        return GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES;
    };
    if (!exists(cand)) return cand;
    for (int i = 2; i < 1000; ++i) {
        cand = stem + L" (" + std::to_wstring(i) + L").png";
        if (!exists(cand)) return cand;
    }
    return stem + L"_" + HashPath(stem) + L".png";
}

Gdiplus::Bitmap* IconManager::LoadImageAny(const std::wstring& srcPath) {
    std::wstring low = ToLower(srcPath);
    if (EndsWith(low, L".exe") || EndsWith(low, L".dll") || EndsWith(low, L".ico")) {
        HICON h = ExtractIcon(srcPath);
        if (h) {
            Gdiplus::Bitmap* b = ToBitmap(h);
            DestroyIcon(h);
            if (b) return b;
        }
    }
    Gdiplus::Bitmap* b = Gdiplus::Bitmap::FromFile(srcPath.c_str());
    if (b && b->GetLastStatus() != Gdiplus::Ok) { delete b; b = nullptr; }
    return b;
}

std::wstring IconManager::ImportCustomIcon(const std::wstring& srcPath,
                                           const std::wstring& realPath) {
    if (m_iconsDir.empty()) return std::wstring();
    Gdiplus::Bitmap* src = LoadImageAny(srcPath);
    if (!src) return std::wstring();

    std::wstring base = SanitizeName(BaseName(realPath, false));
    if (base.empty()) base = L"icon";
    std::wstring name = base + L"_custom.png";
    std::wstring full = m_iconsDir + L"\\" + name;

    // 旧位图（以及 GDI+ 对该 PNG 的文件锁）必须先释放，否则写不进去
    EvictBitmap(name);

    // 统一缩到 256x256 以内：既能保住清晰度，也不会让超大图拖慢每次重绘
    const int kMax = 256;
    int w = static_cast<int>(src->GetWidth());
    int h = static_cast<int>(src->GetHeight());
    if (w <= 0 || h <= 0) { delete src; return std::wstring(); }
    double k = 1.0;
    if (w > kMax || h > kMax) k = (w > h) ? static_cast<double>(kMax) / w
                                          : static_cast<double>(kMax) / h;
    int nw = static_cast<int>(w * k + 0.5), nh = static_cast<int>(h * k + 0.5);
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;

    Gdiplus::Bitmap out(nw, nh, PixelFormat32bppARGB);
    bool ok = false;
    if (out.GetLastStatus() == Gdiplus::Ok) {
        Gdiplus::Graphics g(&out);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        ok = g.DrawImage(src, 0, 0, nw, nh) == Gdiplus::Ok;
    }
    delete src;
    if (ok) ok = SaveBitmapPng(&out, full);
    return ok ? name : std::wstring();
}

void IconManager::EvictBitmap(const std::wstring& iconFileName) {
    auto it = m_cache.find(iconFileName);
    if (it == m_cache.end()) return;
    delete it->second;      // 释放 GDI+ 对象，同时解开它对 PNG 的文件占用
    m_cache.erase(it);
}

std::wstring IconManager::GetIconFileName(const std::wstring& realPath, ItemType /*type*/) {
    if (m_iconsDir.empty()) return std::wstring();

    // 第8条需求：图标文件名 = 原始文件名（"QQ.exe" -> "QQ.exe.png"），
    // 同名冲突自动加 (2)(3)。_index.ini 保证同一个路径永远对应同一个文件。
    const std::wstring key = ToLower(realPath);
    std::wstring fname = LookupIndex(key);
    if (!fname.empty()) {
        std::wstring full = m_iconsDir + L"\\" + fname;
        if (GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES)
            return fname;               // 已缓存，直接复用
        // 文件被清掉了（重置数据/清缓存）→ 沿用同名重新生成，别改名字
    } else {
        fname = AllocIconName(SanitizeName(BaseName(realPath, false)));
        StoreIndex(key, fname);
    }

    std::wstring full = m_iconsDir + L"\\" + fname;
    HICON h = ExtractIcon(realPath);
    if (!h) h = FallbackIcon();
    Gdiplus::Bitmap* b = ToBitmap(h);
    if (b) {
        SaveBitmapPng(b, full);
        delete b;
    }
    if (h) DestroyIcon(h);
    return fname;
}

Gdiplus::Bitmap* IconManager::GetBitmap(const std::wstring& iconFileName) {
    auto it = m_cache.find(iconFileName);
    if (it != m_cache.end()) return it->second;

    Gdiplus::Bitmap* b = nullptr;
    std::wstring full = m_iconsDir + L"\\" + iconFileName;
    if (GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES) {
        b = Gdiplus::Bitmap::FromFile(full.c_str());
        if (b && b->GetLastStatus() != Gdiplus::Ok) {
            delete b;
            b = nullptr;
        }
    }
    if (!b) {
        HICON hf = FallbackIcon();
        b = ToBitmap(hf);
        if (hf) DestroyIcon(hf);
    }
    m_cache[iconFileName] = b; // may be nullptr (extreme fallback)
    return b;
}

void IconManager::PreloadAll(const std::vector<Item>& items) {
    for (const auto& it : items) GetBitmap(it.icon);
}

void IconManager::ClearCache() {
    for (auto& kv : m_cache) {
        if (kv.second) delete kv.second;
    }
    m_cache.clear();
}

// Purge legacy or stale cached icon PNGs exactly once. The previous v2
// cache could be produced by a buggy ToBitmap (wrong AND-mask half /
// external-scan0 ownership) that yielded corrupted icons, so bump the
// marker to v3 and regenerate everything on the next run.
// v4：图标文件从「路径哈希」改成「原始文件名」（第8条需求），旧哈希文件
// 全部清掉重新按新命名生成；用户自己换过的 *_custom.png 必须保留。
void IconManager::InvalidateOldCache() {
    if (m_iconsDir.empty()) return;
    std::wstring marker = m_iconsDir + L"\\.cache_v4";
    if (GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES)
        return; // already migrated

    std::wstring pattern = m_iconsDir + L"\\*.png";
    WIN32_FIND_DATAW fd = {0};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            if (EndsWith(ToLower(name), L"_custom.png")) continue;  // 用户自选图标
            std::wstring full = m_iconsDir + L"\\" + name;
            DeleteFileW(full.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    // 路径 → 文件名索引一起重置，让所有图标按新规则重新命名
    DeleteFileW(IndexPath().c_str());

    std::ofstream f(marker.c_str(), std::ios::binary);
    if (f.good()) f << "v4\n";
    f.close();
    // Remove any older marker so the cache dir stays tidy.
    DeleteFileW((m_iconsDir + L"\\.cache_v3").c_str());
    DeleteFileW((m_iconsDir + L"\\.cache_v2").c_str());
}

// Extract the system icon for a path, Explorer-style (large/extra-large).
// Defined out-of-line because it needs shlobj internals.
HICON IconManager::ExtractIcon(const std::wstring& path) {
    SHFILEINFOW sfi = {0};
    if (!SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX)) {
        return nullptr;
    }
    IImageList* piml = nullptr;
    HICON hicon = nullptr;
    if (SUCCEEDED(SHGetImageList(SHIL_EXTRALARGE, IID_IImageList,
                                 reinterpret_cast<void**>(&piml))) && piml) {
        if (FAILED(piml->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &hicon)) || !hicon) {
            hicon = nullptr;
        }
        piml->Release();
    }
    return hicon;
}

std::wstring IconManager::GetRecycleBinIconFile() {
    std::wstring fname = L"recyclebin.png";
    std::wstring full = m_iconsDir + L"\\" + fname;
    if (GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES)
        return fname;

    HICON h = nullptr;
    SHSTOCKICONINFO sii = {sizeof(sii)};
    if (SUCCEEDED(SHGetStockIconInfo(SIID_RECYCLER, SHGSI_ICON, &sii)))
        h = sii.hIcon;
    if (!h) {
        // Fallback: a generic folder icon for the local drive root.
        SHFILEINFOW fi = {0};
        if (SHGetFileInfoW(L"", FILE_ATTRIBUTE_DIRECTORY, &fi, sizeof(fi),
                           SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES))
            h = fi.hIcon;
    }
    if (h) {
        Gdiplus::Bitmap* b = ToBitmap(h);
        if (b) {
            SaveBitmapPng(b, full);
            delete b;
        }
        DestroyIcon(h);
    }
    return fname;
}

} // namespace JisuQi
