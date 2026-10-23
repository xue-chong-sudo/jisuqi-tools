#include "AutoStart.h"
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <string>

namespace JisuQi {

namespace {

// Full path of the Startup-folder shortcut we manage.
std::wstring ShortcutPath() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Startup, 0, nullptr, &raw)))
        return std::wstring();
    std::wstring dir(raw);
    CoTaskMemFree(raw);
    return dir + L"\\极速启.lnk";
}

} // namespace

bool SetAutoStart(bool enable) {
    std::wstring p = ShortcutPath();
    if (p.empty()) return false;
    if (!enable) {
        DeleteFileW(p.c_str());
        return true;
    }

    // 幂等短路：自启快捷方式已存在即视为已生效，直接返回，避免每次提交都重做昂贵的
    // COM 创建（SHGetKnownFolderPath + CoCreateInstance + IPersistFile::Save）。
    // 极速启为单文件便携程序，.lnk 目标即当前 exe，路径稳定，无需重复生成。
    DWORD attr = GetFileAttributesW(p.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
        return true;

    wchar_t exe[MAX_PATH * 2] = {0};
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);

    IShellLinkW* sl = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl));
    if (FAILED(hr)) return false;
    sl->SetPath(exe);
    sl->SetDescription(L"极速启 快速启动");

    IPersistFile* pf = nullptr;
    hr = sl->QueryInterface(IID_PPV_ARGS(&pf));
    if (SUCCEEDED(hr)) {
        hr = pf->Save(p.c_str(), TRUE);
        pf->Release();
    }
    sl->Release();
    return SUCCEEDED(hr);
}

} // namespace JisuQi
