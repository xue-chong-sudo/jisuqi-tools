#ifndef BONNY_APP_H
#define BONNY_APP_H

#include "Config.h"
#include "IconManager.h"
#include "MainWindow.h"
#include "TrayIcon.h"
#include "MouseHook.h"
#include <windows.h>

namespace JisuQi {

// 注意：设置窗口已由 settings_ui.cpp 的 OpenSettingsWindow 提供（模态自绘窗口），
// 不再使用旧的 SettingsWindow 类，因此此处不再保留其前向声明。

// Top-level controller: owns the data model (Config), the icon cache
// (IconManager), and all UI/OS glue (MainWindow, TrayIcon, MouseHook).
// Startup order: build window -> tray -> hook. Shutdown reverses it.
class App {
public:
    bool Init(HINSTANCE hInst);
    int Run();
    void Quit();

    void ToggleWindow();
    void ShowWindow();
    void HideWindow();

    // Add a dropped path at a specific position in Config::items (used by
    // drag-drop so an icon is inserted where it was dropped, not appended).
    // insertBeforeCfg >= items.size() appends at the end.
    void AddPathAt(const std::wstring& path, size_t insertBeforeCfg);

    // Add a dropped path as a member inside the drawer at drawerIdx.
    void AddToDrawer(size_t drawerIdx, const std::wstring& path);

    // Settings window wiring.
    void ShowSettings();
    void ApplySettings();
    void ApplyAutoHideOnly();   // 轻量：仅重置失焦隐藏计时（步进器/数字编辑确认用，避免卡顿）
    void AddRecycleBin();
    bool IsSettingsVisible() const;

    // 截图快捷键（设置页「截图」分组，默认 Alt+A）触发：
    // 确保截图识字插件宿主进程在运行，并向其投递触发消息。
    void TriggerScreenshot();

    // 搜索快捷键（设置页「搜索」分组，默认 Ctrl+F，仅主界面聚焦时生效）触发：
    // 启动/唤起 search\全盘搜索.exe，并按「开启预览」传入 --preview=0/1。
    void LaunchSearch();

    // 低层鼠标钩子看门狗：重装 WH_MOUSE_LL（系统可能因超时静默移除，导致鼠标呼出失效）。
    void ReinstallMouseHook();

    Config& GetConfig() { return m_config; }
    IconManager& GetIconManager() { return m_iconManager; }
    MainWindow& GetMainWindow() { return m_mainWindow; }
    TrayIcon& GetTray() { return m_tray; }
    DWORD GetInitError() const { return m_initError; }

private:
    // 第8条需求：把 items 里残留的哈希图标名换成「原始文件名.png」（递归抽屉成员）。
    void MigrateIconNames(std::vector<Item>& items);

    // Build an Item from a dropped path (resolve .lnk, classify, cache
    // icon, strip extension from the display name). Shared by AddPathAt
    // and AddToDrawer so drawer members look identical to top-level items.
    Item MakeItem(const std::wstring& path);

    HINSTANCE m_hInst = nullptr;
    Config m_config;
    IconManager m_iconManager;
    MainWindow m_mainWindow;
    TrayIcon m_tray;
    MouseHook m_hook;
    DWORD m_initError = 0;

    bool m_settingsUIOpen = false;    // 新设置窗口（模态）是否正处于打开状态
    bool m_hotkeyRegistered = false;
    bool m_shotHotkeyRegistered = false; // 截图快捷键是否注册成功
    bool m_autoStartKnown = false;     // 开机自启去抖：是否已成功应用过一次
    bool m_autoStartApplied = false;   // 上次应用的 autoStart 值（变化才重做 Shell/COM）

    // 确保 screenshot\截图识字.exe 以宿主模式（--host）在运行；没有则启动。
    void EnsureShotHostRunning();
};

} // namespace JisuQi

#endif // BONNY_APP_H
