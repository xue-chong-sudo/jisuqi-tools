#ifndef BONNY_CONFIG_H
#define BONNY_CONFIG_H

#include <windows.h>
#include <string>
#include <vector>

namespace JisuQi {

// Kind of a launch entry. Serialized as a lower-case string in JSON.
enum class ItemType { Link, Exe, Folder, File };

// A single launchable entry shown as an icon in the grid.
struct Item {
    std::wstring name;       // display name (file/folder name)
    std::wstring path;       // real target path (resolved for .lnk)
    ItemType    type = ItemType::File;
    std::wstring icon;       // cached icon file name, e.g. "a1b2c3.png"
    std::wstring group = L"常用"; // owning group
    bool        isDrawer = false;       // true => this cell is a drawer
    std::vector<Item> members;          // drawer contents (only when isDrawer)
    // 主界面快速启动键：0 = 未绑定，'A'..'Z' = 主面板可见时按下即启动该图标。
    // 大小写不敏感（内部统一存大写）。
    wchar_t     shortcut = 0;
};

// User preferences, persisted under "settings" in config.json.
struct Settings {
    bool middleClickEnabled = true;      // 鼠标中键呼出
    bool hotkeyEnabled = false;          // 快捷键呼出
    UINT hotkeyMods = MOD_CONTROL;       // MOD_CONTROL / MOD_ALT / MOD_SHIFT / MOD_WIN
    UINT hotkeyVk = VK_SPACE;            // hotkey virtual key (0 = none)
    bool autoStart = false;              // 开机自启（shell:startup 快捷方式）
    bool rememberWindowSize = true;      // 记住窗口尺寸
    int  autoHideSeconds = 5;            // 失焦自动隐藏秒数 0-60，0 = 关闭
    double drawerDivider = 0.5;          // 抽屉分隔线比例(0.15-0.85 有效，0.5=中点)
    int  trayLaunchMode = 0;            // 托盘左键启动方式：0=仅单击, 1=仅双击
};

// Small Unicode helpers used across modules.
std::wstring ToLower(const std::wstring& s);
bool EndsWith(const std::wstring& s, const std::wstring& suffix);
ItemType ItemTypeFromString(const std::wstring& s);
std::wstring ItemTypeToString(ItemType t);

// Configuration: persisted to <exedir>/data/config.json.
class Config {
public:
    std::wstring exeDir;      // directory of the running exe
    std::wstring dataDir;     // <exeDir>/data
    std::wstring iconsDir;    // <exeDir>/data/icons

    int winX = 0, winY = 0, winW = 0, winH = 0; // window rect (physical px)
    std::wstring currentGroup = L"常用";
    std::vector<Item> items;
    std::vector<std::wstring> groups;   // group tab list (at least one)
    Settings settings;

    // Directory that contains the current executable (no trailing slash).
    static std::wstring GetExeDir();

    // Create data/ and data/icons/ if missing.
    void EnsureDirs();

    // Load from config.json. On any failure, falls back to defaults
    // (empty items, zero rect -> caller centers) without throwing.
    bool Load();

    // Persist current state to config.json (UTF-8).
    bool Save();
};

} // namespace JisuQi

#endif // BONNY_CONFIG_H
