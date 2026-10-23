#include "Config.h"
#include <fstream>
#include <sstream>
#include <cwctype>
#include <algorithm>
#include "nlohmann/json.hpp"

namespace JisuQi {

namespace {

// ---- UTF-16 <-> UTF-8 conversion (CP_UTF8) ----
std::wstring Utf8ToWStringImpl(const std::string& s) {
    if (s.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(len) - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], len);
    return out;
}

std::string WStringToUtf8Impl(const std::wstring& s) {
    if (s.empty()) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0,
                                  nullptr, nullptr);
    if (len <= 0) return std::string();
    std::string out(static_cast<size_t>(len) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, &out[0], len,
                        nullptr, nullptr);
    return out;
}

} // namespace

std::wstring ToLower(const std::wstring& s) {
    std::wstring out = s;
    for (wchar_t& c : out) c = static_cast<wchar_t>(towlower(c));
    return out;
}

bool EndsWith(const std::wstring& s, const std::wstring& suffix) {
    if (suffix.size() > s.size()) return false;
    return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

ItemType ItemTypeFromString(const std::wstring& s) {
    std::wstring t = ToLower(s);
    if (t == L"link")   return ItemType::Link;
    if (t == L"exe")    return ItemType::Exe;
    if (t == L"folder") return ItemType::Folder;
    return ItemType::File;
}

std::wstring ItemTypeToString(ItemType t) {
    switch (t) {
        case ItemType::Link:   return L"link";
        case ItemType::Exe:    return L"exe";
        case ItemType::Folder: return L"folder";
        default:               return L"file";
    }
}

std::wstring Config::GetExeDir() {
    wchar_t buf[MAX_PATH * 2] = {0};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    std::wstring s(buf, n > 0 ? n : 0);
    size_t sl = s.find_last_of(L"\\/");
    if (sl != std::wstring::npos) s = s.substr(0, sl);
    return s;
}

void Config::EnsureDirs() {
    CreateDirectoryW(dataDir.c_str(), nullptr);
    CreateDirectoryW(iconsDir.c_str(), nullptr);
}

bool Config::Load() {
    std::wstring path = dataDir + L"\\config.json";
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) {
        // No config yet -> defaults (empty items, zero rect).
        // 但至少要有一个默认分组，否则首次启动面板会没有“常用”标签。
        if (currentGroup.empty()) currentGroup = L"常用";
        if (groups.empty()) groups.push_back(L"常用");
        return false;
    }
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    try {
        nlohmann::json j = nlohmann::json::parse(content);
        if (j.contains("window") && j["window"].is_object()) {
            const auto& w = j["window"];
            winX = w.value("x", 0);
            winY = w.value("y", 0);
            winW = w.value("w", 0);
            winH = w.value("h", 0);
        }
        if (j.contains("currentGroup"))
            currentGroup = Utf8ToWStringImpl(j["currentGroup"].get<std::string>());
        if (currentGroup.empty()) currentGroup = L"常用";
        if (j.contains("groups") && j["groups"].is_array()) {
            groups.clear();
            for (const auto& g : j["groups"]) {
                std::wstring name = Utf8ToWStringImpl(g.get<std::string>());
                if (!name.empty()) groups.push_back(name);
            }
        }
        if (groups.empty()) groups.push_back(L"常用");
        if (std::find(groups.begin(), groups.end(), currentGroup) == groups.end())
            groups.push_back(currentGroup);
        if (j.contains("settings") && j["settings"].is_object()) {
            const auto& st = j["settings"];
            settings.middleClickEnabled  = st.value("middleClick", true);
            settings.hotkeyEnabled       = st.value("hotkeyEnabled", false);
            settings.hotkeyMods          = st.value("hotkeyMods", static_cast<UINT>(MOD_CONTROL));
            settings.hotkeyVk            = st.value("hotkeyVk", static_cast<UINT>(VK_SPACE));
            settings.autoStart           = st.value("autoStart", false);
            settings.rememberWindowSize  = st.value("rememberSize", true);
            settings.autoHideSeconds     = st.value("autoHideSeconds", 5);
            settings.drawerDivider        = st.value("drawerDivider", 0.5);
            settings.trayLaunchMode       = st.value("trayLaunchMode", 0);
            if (settings.autoHideSeconds < 0) settings.autoHideSeconds = 0;
            if (settings.autoHideSeconds > 60) settings.autoHideSeconds = 60;
            // 分隔线比例必须落在 (0,1) 内：越界/NaN 会让 double->int 转换成为 UB，
            // 手改 config.json 写入 1e308 或 "nan" 即可触发。!(x>=lo) 同时挡掉 NaN。
            if (!(settings.drawerDivider >= 0.15)) settings.drawerDivider = 0.15;
            if (settings.drawerDivider > 0.85)     settings.drawerDivider = 0.85;
        }
        if (j.contains("items") && j["items"].is_array()) {
            for (const auto& e : j["items"]) {
                Item it;
                it.name  = Utf8ToWStringImpl(e.value("name",  std::string()));
                it.path  = Utf8ToWStringImpl(e.value("path",  std::string()));
                it.type  = ItemTypeFromString(
                               Utf8ToWStringImpl(e.value("type", std::string("file"))));
                it.icon  = Utf8ToWStringImpl(e.value("icon",  std::string()));
                it.group = Utf8ToWStringImpl(e.value("group", std::string("常用")));
                if (it.group.empty()) it.group = L"常用";
                it.isDrawer = e.value("isDrawer", false);
                // 快捷键：接受 "A"/"a" 字符串或单字符；其它一律视为未绑定。
                {
                    std::string sc = e.value("shortcut", std::string());
                    if (!sc.empty()) {
                        wchar_t c = Utf8ToWStringImpl(sc)[0];
                        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
                        it.shortcut = (c >= L'A' && c <= L'Z') ? c : 0;
                    }
                }
                if (it.isDrawer && e.contains("members") &&
                    e["members"].is_array()) {
                    for (const auto& m : e["members"]) {
                        Item mem;
                        mem.name = Utf8ToWStringImpl(m.value("name", std::string()));
                        mem.path = Utf8ToWStringImpl(m.value("path", std::string()));
                        mem.type = ItemTypeFromString(
                            Utf8ToWStringImpl(m.value("type", std::string("file"))));
                        mem.icon = Utf8ToWStringImpl(m.value("icon", std::string()));
                        mem.group = Utf8ToWStringImpl(m.value("group", std::string("常用")));
                        if (mem.group.empty()) mem.group = L"常用";
                        mem.isDrawer = false;
                        {
                            std::string sc = m.value("shortcut", std::string());
                            if (!sc.empty()) {
                                wchar_t c = Utf8ToWStringImpl(sc)[0];
                                if (c >= L'a' && c <= L'z')
                                    c = static_cast<wchar_t>(c - L'a' + L'A');
                                mem.shortcut = (c >= L'A' && c <= L'Z') ? c : 0;
                            }
                        }
                        it.members.push_back(mem);
                    }
                }
                items.push_back(it);
            }
        }
        // 把 items 引用到、但 groups 里缺失的分组补进来（否则该分组的图标会
        // 因为找不到对应标签而“消失”）。注意：这一步必须在 items 解析之后执行
        // ——原实现放在解析之前，此时 items 还是空的，整段是死逻辑。
        for (const auto& it : items)
            if (std::find(groups.begin(), groups.end(), it.group) == groups.end())
                groups.push_back(it.group);
        return true;
    } catch (...) {
        // Corrupt config -> fall back to defaults, do not crash.
        items.clear();
        winX = winY = winW = winH = 0;
        return false;
    }
}

bool Config::Save() {
    EnsureDirs();
    nlohmann::json j;
    j["window"]["x"] = winX;
    j["window"]["y"] = winY;
    j["window"]["w"] = winW;
    j["window"]["h"] = winH;
    j["currentGroup"] = WStringToUtf8Impl(currentGroup);

    nlohmann::json garr = nlohmann::json::array();
    for (const auto& g : groups) garr.push_back(WStringToUtf8Impl(g));
    j["groups"] = garr;

    j["settings"]["middleClick"]     = settings.middleClickEnabled;
    j["settings"]["hotkeyEnabled"]   = settings.hotkeyEnabled;
    j["settings"]["hotkeyMods"]      = settings.hotkeyMods;
    j["settings"]["hotkeyVk"]        = settings.hotkeyVk;
    j["settings"]["autoStart"]       = settings.autoStart;
    j["settings"]["rememberSize"]    = settings.rememberWindowSize;
    j["settings"]["autoHideSeconds"] = settings.autoHideSeconds;
    j["settings"]["drawerDivider"]    = settings.drawerDivider;
    j["settings"]["trayLaunchMode"]   = settings.trayLaunchMode;

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& it : items) {
        nlohmann::json o;
        o["name"]  = WStringToUtf8Impl(it.name);
        o["path"]  = WStringToUtf8Impl(it.path);
        o["type"]  = WStringToUtf8Impl(ItemTypeToString(it.type));
        o["icon"]  = WStringToUtf8Impl(it.icon);
        o["group"] = WStringToUtf8Impl(it.group);
        o["isDrawer"] = it.isDrawer;
        if (it.shortcut) {
            // 只写 A-Z 的单字符字符串，未绑定则不写（保持 json 精简）。
            std::string one(1, static_cast<char>(it.shortcut));
            o["shortcut"] = one;
        }
        if (it.isDrawer) {
            nlohmann::json marr = nlohmann::json::array();
            for (const auto& mem : it.members) {
                nlohmann::json mo;
                mo["name"]  = WStringToUtf8Impl(mem.name);
                mo["path"]  = WStringToUtf8Impl(mem.path);
                mo["type"]  = WStringToUtf8Impl(ItemTypeToString(mem.type));
                mo["icon"]  = WStringToUtf8Impl(mem.icon);
                mo["group"] = WStringToUtf8Impl(mem.group);
                if (mem.shortcut) {
                    std::string one(1, static_cast<char>(mem.shortcut));
                    mo["shortcut"] = one;
                }
                marr.push_back(mo);
            }
            o["members"] = marr;
        }
        arr.push_back(o);
    }
    j["items"] = arr;

    std::string s = j.dump(2);
    std::wstring path = dataDir + L"\\config.json";
    std::ofstream f(path, std::ios::binary);
    if (!f.good()) return false;
    f << s;
    return true;
}

} // namespace JisuQi
