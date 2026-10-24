#include "App.h"
#include "resource.h"
#include "settings_ui.h"   // 新设置界面接口（OpenSettingsWindow / GetSettings 等）
#include "AutoStart.h"
#include <shellapi.h>

namespace JisuQi {

// settings_ui.cpp 通过回调把“保存设置 / 添加回收站”桥接回主程序。
// 这里用一个全局实例指针 + 跳板函数连接二者（本进程只有一个 App 实例）。
namespace {
JisuQi::App* g_appInstance = nullptr;
void AppApplySettingsTrampoline() {
    if (g_appInstance) g_appInstance->ApplySettings();
}
void AppApplyAutoHideTrampoline() {
    if (g_appInstance) g_appInstance->ApplyAutoHideOnly();
}
void AppAddRecycleBinTrampoline() {
    if (g_appInstance) g_appInstance->AddRecycleBin();
}
// 截图识字插件宿主（screenshot\截图识字.exe --host）的隐藏窗口类名。
// 与插件 main.cpp 中的 kHostClass 保持一致；改一处必须同步另一处。
static const wchar_t kShotHostClass[] = L"JisuQiShotHost";
// 触发消息名（RegisterWindowMessage，系统级原子，两进程按名字取到同一编号）。
static const wchar_t kShotTriggerName[] = L"JisuQiShotTrigger";

// 旧版图标文件名 = 路径 FNV-1a 哈希的 16 位十六进制 + ".png"。
static bool LooksLikeHashIcon(const std::wstring& n) {
    if (n.size() != 16 + 4) return false;
    if (n.compare(16, 4, L".png") != 0) return false;
    for (size_t i = 0; i < 16; ++i) {
        const wchar_t c = n[i];
        const bool hex = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
        if (!hex) return false;
    }
    return true;
}

// 第8条需求：把 config.json 里残留的哈希文件名换成「原始文件名.png」。
// 只在文件名是旧哈希、或者对应 PNG 已经不存在时才重算 —— 用户自选的
// *_custom.png 只要文件还在就原样保留。返回是否有改动（需要回写 json）。
static bool MigrateIconNamesRec(IconManager& im, const std::wstring& iconsDir,
                                std::vector<Item>& items) {
    bool changed = false;
    for (auto& it : items) {
        if (it.isDrawer) {
            if (MigrateIconNamesRec(im, iconsDir, it.members)) changed = true;
            continue;
        }
        const std::wstring full = iconsDir + L"\\" + it.icon;
        const bool missing = it.icon.empty() ||
                             GetFileAttributesW(full.c_str()) == INVALID_FILE_ATTRIBUTES;
        const bool hashed = LooksLikeHashIcon(ToLower(it.icon));
        if (!missing && !hashed) continue;
        if (hashed && !missing) {
            // 旧哈希文件还在：删掉，让它按新规则重新生成（避免目录里留垃圾）
            DeleteFileW(full.c_str());
        }
        const std::wstring nn = im.GetIconFileName(it.path, it.type);
        if (!nn.empty() && nn != it.icon) { it.icon = nn; changed = true; }
    }
    return changed;
}

} // namespace

bool App::Init(HINSTANCE hInst) {
    m_hInst = hInst;

    m_config.exeDir = Config::GetExeDir();
    m_config.dataDir = m_config.exeDir + L"\\data";
    m_config.iconsDir = m_config.dataDir + L"\\icons";
    m_config.EnsureDirs();
    if (!m_config.Load()) {
        // 首次运行：config.json 不存在，上面已回退到默认分组（常用）。
        // 立即落盘，保证下次启动以及外部观察都有 config.json，行为一致。
        m_config.Save();
    }

    // 接入新设置界面（settings_ui.cpp）：注册回调并保存实例指针，
    // 使“保存设置 / 添加回收站”能回到主程序逻辑。
    g_appInstance = this;
    SetSettingsApplyCallback(AppApplySettingsTrampoline);
    SetSettingsApplyAutoHideCallback(AppApplyAutoHideTrampoline);
    SetSettingsAddRecycleBinCallback(AppAddRecycleBinTrampoline);

    // 首次迁移：若新配置文件 data\settings.ini 尚不存在，则从旧 config.json
    // 里对应的偏好项“种”出一份，保留用户此前的设置，避免行为突变；
    // 之后 settings.ini 即为这些设置的唯一权威来源。
    if (!SettingsUIExists()) {
        auto& s = GetSettings();
        const Settings& c = m_config.settings;
        s.middleButton    = c.middleClickEnabled;
        s.rememberSize    = c.rememberWindowSize;
        s.hideSec         = c.autoHideSeconds;
        s.launchMode      = c.trayLaunchMode;   // 注：字段名仍叫 trayLaunchMode，现语义为「面板图标启动方式」
        s.autoStart       = c.autoStart;        // 开机自启：从 config.json 迁移到设置界面（写入 settings.ini 为权威）
        if (c.hotkeyEnabled && c.hotkeyVk) {
            s.hkMod = c.hotkeyMods;
            s.hkVk  = c.hotkeyVk;
        }
        SaveSettingsUI();
    }
    GetSettings().autoStart = m_config.settings.autoStart;  // 注入 config 的 autoStart，作为 settings.ini 缺失 autostart 键时的回落值
    LoadSettingsUI();   // 把 settings.ini 载入到实时设置对象 g_s

    m_iconManager.SetIconsDir(m_config.iconsDir);
    m_iconManager.InvalidateOldCache();   // purge black-bg cache once
    // 第8条需求迁移：图标文件从「路径哈希」改为「原始文件名」。缓存被清掉
    // 以后 config.json 里还留着旧的哈希文件名，这里按 path 重新算一遍；
    // 用户自己换过的图标（*_custom.png）文件还在，不会被覆盖。
    MigrateIconNames(m_config.items);
    m_iconManager.PreloadAll(m_config.items);

    if (!m_mainWindow.Create(hInst, this)) {
        m_initError = m_mainWindow.GetCreateError();
        return false;
    }

    // Load the small tray icon at the system's actual small-icon size so
    // it stays crisp instead of downscaling the 32px version.
    HICON icon = reinterpret_cast<HICON>(LoadImageW(
        m_hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR));
    if (!icon)
        icon = LoadIconW(m_hInst, MAKEINTRESOURCEW(IDI_APP));
    if (!icon) icon = LoadIconW(nullptr, MAKEINTRESOURCEW(IDI_APPLICATION));
    m_tray.Create(m_mainWindow.GetHwnd(), icon, L"极速启");

    m_hook.Install(m_mainWindow.GetHwnd());
    // 低层鼠标钩子看门狗：WH_MOUSE_LL 可能被系统因回调超时静默移除（导致鼠标呼出"突然失效"），
    // 定期重装以自愈（每 30 秒一次，重装开销极小）。
    SetTimer(m_mainWindow.GetHwnd(), TIMER_HOOKCHECK, 30000, nullptr);
    ApplySettings();       // (re)apply hotkey / auto-start before showing
    // 预热截图识字宿主：在后台提前加载 OCR 引擎，避免「每次首次触发截图」时
    // 因 OCR 初始化（约 1~3 秒）而卡住。宿主为常驻隐藏进程，预热后每次触发即秒开。
    EnsureShotHostRunning();
    m_mainWindow.Show();   // open the launch panel on startup
    return true;
}

int App::Run() {
    MSG msg;
    int r;
    do {
        r = static_cast<int>(GetMessageW(&msg, nullptr, 0, 0));
        if (r == 0 || r == -1) break;   // 0=WM_QUIT，-1=错误（旧写法会死循环）
        // 搜索快捷键（默认 Ctrl+F，仅主界面可见时生效）：在 Translate/Dispatch 之前拦截，
        // 这样即使焦点在面板的搜索框子控件上也能触发；同时 Ctrl+F 不会生成 'f' 字符。
        if (msg.message == WM_KEYDOWN && m_mainWindow.IsVisible() &&
            !m_settingsUIOpen && msg.wParam != 0) {
            const ::Settings& s = GetSettings();
            if (s.searchEnabled && s.searchHkVk != 0 &&
                static_cast<UINT>(msg.wParam) == s.searchHkVk) {
                bool ok = true;
                if (s.searchHkMod & MOD_CONTROL)
                    ok = ok && (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                if (s.searchHkMod & MOD_SHIFT)
                    ok = ok && (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                if (s.searchHkMod & MOD_ALT)
                    ok = ok && (GetKeyState(VK_MENU) & 0x8000) != 0;
                if (s.searchHkMod & MOD_WIN)
                    ok = ok && ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) != 0;
                if (ok) {
                    // 唤起全盘搜索时先收起主面板：两个窗口叠在一起时，主面板会
                    // 挡住搜索结果且抢走焦点，视觉上也很乱。
                    m_mainWindow.Hide();
                    LaunchSearch();
                    continue;
                }
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    } while (true);
    return (r == 0) ? static_cast<int>(msg.wParam) : 1;
}

void App::Quit() {
    m_mainWindow.Hide();            // persists window rect
    m_tray.Delete();
    m_hook.Uninstall();
    if (m_hotkeyRegistered) {
        UnregisterHotKey(m_mainWindow.GetHwnd(), ID_HOTKEY_TOGGLE);
        m_hotkeyRegistered = false;
    }
    if (m_shotHotkeyRegistered) {
        UnregisterHotKey(m_mainWindow.GetHwnd(), ID_HOTKEY_SHOT);
        m_shotHotkeyRegistered = false;
    }
    // 通知截图识字宿主退出（插件常驻进程随主程序一起收尾；找不到则无操作）
    if (HWND host = FindWindowW(kShotHostClass, nullptr))
        PostMessageW(host, WM_CLOSE, 0, 0);
    if (m_mainWindow.GetHwnd())
        DestroyWindow(m_mainWindow.GetHwnd());
    PostQuitMessage(0);
}

void App::ToggleWindow() {
    if (m_mainWindow.IsVisible()) m_mainWindow.Hide();
    else m_mainWindow.Show();
}

void App::ShowWindow() { m_mainWindow.Show(); }
void App::HideWindow() { m_mainWindow.Hide(); }

void App::MigrateIconNames(std::vector<Item>& items) {
    if (MigrateIconNamesRec(m_iconManager, m_config.iconsDir, items))
        m_config.Save();
}

Item App::MakeItem(const std::wstring& path) {    std::wstring real = path;
    bool wasLink = EndsWith(ToLower(path), L".lnk");
    if (wasLink) {
        std::wstring t;
        if (IconManager::ResolveLink(path, t)) real = t;
    }

    Item it;
    it.type = IconManager::GuessType(real, wasLink);
    it.path = real;
    it.name = IconManager::BaseName(path, wasLink);
    // Drop the trailing extension from the displayed name so items read
    // more cleanly ("report.docx" -> "report", "notepad.exe" -> "notepad",
    // "run.bat" -> "run"). This only affects the DISPLAY name stored in
    // config.json; launching always uses the full path in it.path, so
    // opening the item is completely unaffected.
    {
        size_t dot = it.name.find_last_of(L'.');
        // Guard: keep the name untouched when there is no dot, when the
        // dot is the first character (dotfiles such as ".gitignore"), or
        // when stripping would leave an empty name.
        if (dot != std::wstring::npos && dot > 0 &&
            dot + 1 < it.name.size())
            it.name.resize(dot);
    }
    it.group = m_config.currentGroup;
    it.icon = m_iconManager.GetIconFileName(real, it.type);
    return it;
}

void App::AddPathAt(const std::wstring& path, size_t insertBeforeCfg) {
    Item it = MakeItem(path);
    if (insertBeforeCfg >= m_config.items.size())
        m_config.items.push_back(it);
    else
        m_config.items.insert(m_config.items.begin() + insertBeforeCfg, it);
    m_config.Save();
    m_mainWindow.OnItemsChanged();
}

void App::AddToDrawer(size_t drawerIdx, const std::wstring& path) {
    if (drawerIdx >= m_config.items.size()) return;
    Item& d = m_config.items[drawerIdx];
    if (!d.isDrawer) return;
    d.members.push_back(MakeItem(path));
    m_config.Save();
    m_mainWindow.OnItemsChanged();
}

// ---------------------------------------------------------------------------
// Settings window wiring
// ---------------------------------------------------------------------------

void App::ShowSettings() {
    // Safety check: never attempt to create the settings window while the
    // main window handle is invalid (it is used as the owner and as the
    // centering reference). Surface the problem instead of failing silently.
    HWND hwndMain = m_mainWindow.GetHwnd();
    if (!hwndMain || !IsWindow(hwndMain)) {
        MessageBoxW(nullptr, L"主窗口句柄无效，无法打开设置。",
                    L"极速启", MB_OK | MB_ICONERROR);
        return;
    }
    // OpenSettingsWindow 自带模态消息循环：会一直阻塞到设置窗口被关闭，
    // 因此用标志位标记“打开中”，供主窗口在失焦时跳过自动隐藏计时。
    // 注意：设置窗口开着时，其嵌套循环仍会派发托盘的“设置”命令，导致这里被
    // 重入；此时只把已有窗口置前，绝不能把标志位提前清成 false（否则面板会在
    // 设置窗仍开着时自动隐藏）。
    const bool wasOpen = m_settingsUIOpen;
    m_settingsUIOpen = true;
    OpenSettingsWindow(m_hInst, hwndMain);
    if (!wasOpen) m_settingsUIOpen = false;
}

bool App::IsSettingsVisible() const {
    return m_settingsUIOpen;
}

void App::ApplySettings() {
    // 设置来源统一为 data\settings.ini（由 settings_ui.cpp 管理）。
    // 这里把新设置映射到原有运行时，并据此重新注册热键、重置计时等。
    auto& s = GetSettings();
    Settings& cs = m_config.settings;
    cs.middleClickEnabled = s.middleButton;   // 鼠标中键呼出
    cs.rememberWindowSize = s.rememberSize;    // 记住窗口尺寸
    cs.autoHideSeconds    = s.hideSec;         // 失焦自动隐藏秒数
    cs.trayLaunchMode     = s.launchMode;   // 面板图标启动方式：0=单击启动, 1=双击启动（托盘固定单击呼出）
    cs.autoStart          = s.autoStart;    // 开机自启（设置界面现已可切换）

    // 按 g_s.hkMod / g_s.hkVk 重新注册全局呼出快捷键。
    if (m_hotkeyRegistered) {
        UnregisterHotKey(m_mainWindow.GetHwnd(), ID_HOTKEY_TOGGLE);
        m_hotkeyRegistered = false;
    }
    if (s.hkVk != 0) {
        if (RegisterHotKey(m_mainWindow.GetHwnd(), ID_HOTKEY_TOGGLE,
                           s.hkMod, s.hkVk))
            m_hotkeyRegistered = true;
    }

    // 截图快捷键（设置页「截图」分组，默认 Alt+A；capHkVk=0 表示不启用）。
    // 与呼出快捷键共用 RegisterHotKey，ID 必须不同；组合相同则后者注册失败
    // （系统不允许同组合双热键），此时截图快捷键静默不生效。
    if (m_shotHotkeyRegistered) {
        UnregisterHotKey(m_mainWindow.GetHwnd(), ID_HOTKEY_SHOT);
        m_shotHotkeyRegistered = false;
    }
    if (s.capHkVk != 0) {
        if (RegisterHotKey(m_mainWindow.GetHwnd(), ID_HOTKEY_SHOT,
                           s.capHkMod, s.capHkVk))
            m_shotHotkeyRegistered = true;
    }

    // 开机自启：仅在“值真正变化”时才调用 Shell/COM（写/删启动文件夹 .lnk），
    // 避免每次设置提交都重做昂贵的 SHGetKnownFolderPath + CoCreateInstance + IPersistFile::Save，
    // 这正是此前“确认数字后界面卡 1 秒以上”的主因。
    if (!m_autoStartKnown || m_autoStartApplied != cs.autoStart) {
        SetAutoStart(cs.autoStart);
        m_autoStartApplied = cs.autoStart;
        m_autoStartKnown   = true;
    }

    // 通知主窗口：刷新失焦隐藏计时 / 重新布局。
    PostMessageW(m_mainWindow.GetHwnd(), WM_APP_SETTINGS_CHANGED, 0, 0);
}

void App::ApplyAutoHideOnly() {
    // 仅“失焦自动隐藏秒数”变化时由设置界面轻量路径调用：更新内存值并通过
    // WM_APP_SETTINGS_CHANGED(wParam=1) 只重置隐藏计时，不重绘主面板、不重注册热键、
    // 不碰开机自启，从而消除步进器 / 数字编辑确认时的卡顿。
    auto& s = GetSettings();
    m_config.settings.autoHideSeconds = s.hideSec;
    PostMessageW(m_mainWindow.GetHwnd(), WM_APP_SETTINGS_CHANGED, 1, 0);
}

void App::AddRecycleBin() {
    Config& cfg = m_config;
    // De-duplicate: do nothing if a recycle bin item already exists.
    for (const auto& it : cfg.items)
        if (it.path == L"shell:RecycleBinFolder") return;
    Item it;
    it.name = L"回收站";
    it.path = L"shell:RecycleBinFolder";
    it.type = ItemType::File;
    it.group = cfg.currentGroup;
    it.icon = m_iconManager.GetRecycleBinIconFile();
    cfg.items.push_back(it);
    cfg.Save();
    m_mainWindow.OnItemsChanged();
}

void App::ReinstallMouseHook() {
    // Install 内部会先卸掉旧钩子再重装，等价于安全重装；钩子健在时重装也无副作用。
    m_hook.Install(m_mainWindow.GetHwnd());
}

bool App::IsMouseHeld() {
    return m_hook.IsHeld();
}

// ---------------------------------------------------------------------------
// 截图识字插件
// ---------------------------------------------------------------------------

void App::EnsureShotHostRunning() {
    // 插件随极速启分发在 exe 同级 screenshot\ 目录（截图识字.exe + tessdata + DLL）。
    const std::wstring dir  = m_config.exeDir + L"\\screenshot";
    const std::wstring exe  = dir + L"\\截图识字.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return; // 未随附则静默
    // 宿主参数：tessdata 目录 + 本程序 settings.ini 路径。
    // 插件每次触发时自行读 [screenshot] 段，设置改动即时生效、无需重启插件。
    std::wstring cmd = L"\"" + exe + L"\" --host \"" + dir + L"\\tessdata\" \""
                     + m_config.dataDir + L"\\settings.ini\"";
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

void App::TriggerScreenshot() {
    UINT msg = RegisterWindowMessageW(kShotTriggerName);
    if (!msg) return;
    // 宿主已运行 -> 直接投递；否则启动后轮询等待其初始化完成（首次约零点几秒，
    // 之后常驻触发零延迟）。
    HWND host = FindWindowW(kShotHostClass, nullptr);
    if (!host) {
        EnsureShotHostRunning();
        for (int i = 0; i < 80 && !host; i++) {   // 最多等 4 秒（OCR 引擎初始化）
            Sleep(50);
            host = FindWindowW(kShotHostClass, nullptr);
        }
    }
    if (host) PostMessageW(host, msg, 0, 0);
    // 找不到宿主（插件缺失/初始化失败）时静默返回，不影响主程序。
}

// ---------------------------------------------------------------------------
// 全盘搜索插件（search\全盘搜索.exe）
// ---------------------------------------------------------------------------

void App::LaunchSearch() {
    const ::Settings& s = GetSettings();
    if (!s.searchEnabled) return;

    // 优先与主程序同目录的 全盘搜索.exe；找不到再回退到 search\ 子目录（兼容旧布局）。
    std::wstring exe = m_config.exeDir + L"\\全盘搜索.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wstring alt = m_config.exeDir + L"\\search\\全盘搜索.exe";
        if (GetFileAttributesW(alt.c_str()) != INVALID_FILE_ATTRIBUTES) exe = alt;
    }
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(m_mainWindow.GetHwnd(),
                    L"未找到 全盘搜索.exe。\n请确认它随程序一起分发（与主程序同目录或 search 子目录）。",
                    L"极速启", MB_OK | MB_ICONWARNING);
        return;
    }

    // 「开启预览」映射为搜索程序的启动参数：--preview=1 显示右侧预览栏，0 隐藏。
    // --datadir 将搜索插件的数据（config.ini / cache 索引）指向主程序的 data 目录，
    // 实现主程序与搜索插件共用同一个 data 文件夹，减少部署目录层级。
    // 搜索程序自身是单实例：已在运行时再次启动只会把已有窗口叫到最前（参数不改变已开窗口）。
    std::wstring cmd = L"\"" + exe + L"\" --preview=" + (s.searchPreview ? L"1" : L"0")
                     + L" --datadir=\"" + m_config.dataDir + L"\"";
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                       0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

} // namespace JisuQi
