#ifndef BONNY_RESOURCE_H
#define BONNY_RESOURCE_H

// Resource & message identifiers shared across the project.
// Custom window messages use values >= WM_APP (0x8000) to avoid
// collisions with system messages. They are literal numbers so this
// header is also safe to include from the .rc file.

#define IDI_APP                 101

// Tray popup menu
#define ID_TRAY_SHOW            1001
#define ID_TRAY_EXIT            1002
#define ID_TRAY_SETTINGS        1003
#define ID_TRAY_PAUSE_HOTKEY    1004   // 暂停使用快捷键（切换）
#define ID_TRAY_PAUSE_MOUSE     1005   // 暂停使用鼠标（切换）

// Icon context menu
#define ID_CTX_RENAME           2001
#define ID_CTX_DELETE           2002
#define ID_CTX_OPENLOC          2003
#define ID_CTX_NEWDRAWER        2004
#define ID_CTX_CHANGEICON       2005   // 「更换图标」：选一张图片替换当前图标
#define ID_CTX_MOVEGROUP        2006   // 「移动分组」：点击后再弹出分组列表
#define ID_CTX_MOVETO_BASE      2100   // +group index -> move item to group
#define ID_CTX_SHORTCUT_BASE    2200   // +0..25 -> 给该图标绑定 A..Z 快捷键（旧版子菜单，已改为弹窗）
#define ID_CTX_SHORTCUT_CLEAR   2230   // 清除该图标的快捷键
#define ID_CTX_SHORTCUT_SET     2240   // 「快捷键...」：弹出输入框设置单个字母

// Group (tab) context menu
#define ID_GROUP_NEW            3001
#define ID_GROUP_REN            3002
#define ID_GROUP_DEL            3003

// Custom window messages
#define WM_TRAYICON             (WM_APP + 1)   // 0x8001
#define WM_APP_TOGGLE           (WM_APP + 2)   // 0x8002
#define WM_APP_SEARCH_ENTER     (WM_APP + 3)   // 0x8003
#define WM_APP_HIDE             (WM_APP + 4)   // 0x8004
#define WM_APP_F2               (WM_APP + 5)   // 0x8005
#define WM_APP_SHOW             (WM_APP + 6)   // 0x8006: wake/foreground
#define WM_APP_DELETE           (WM_APP + 7)   // 0x8007: context-menu Delete key
#define WM_APP_OPENLOC          (WM_APP + 8)   // 0x8008: context-menu OpenLoc key
#define WM_APP_NEWDRAWER        (WM_APP + 9)   // 0x8009: icon-menu New Drawer key
#define WM_APP_SETTINGS_CHANGED (WM_APP + 10)  // 0x800A: settings saved, re-apply
#define WM_APP_SHORTCUT_SET     (WM_APP + 11)  // 0x800B: icon-menu Set Shortcut key (B)

// Group-menu (tab context menu) keyboard shortcuts. Only forwarded while the
// group menu is open, so they are NOT global hotkeys (Requirement 1).
#define WM_APP_GROUP_NEW (WM_APP + 12)  // 0x800C: group-menu N key
#define WM_APP_GROUP_REN (WM_APP + 13)  // 0x800D: group-menu F2 key
#define WM_APP_GROUP_DEL (WM_APP + 14)  // 0x800E: group-menu D key
#define WM_APP_MOVEGROUP (WM_APP + 15)  // 0x800F: 图标菜单「移动分组」→ 弹出分组列表
#define WM_APP_CHANGEICON (WM_APP + 16) // 0x8010: 图标菜单「更换图标」→ 文件选择框

// RegisterHotKey id used for the user-configurable summon hotkey.
#define ID_HOTKEY_TOGGLE        1
// RegisterHotKey id for the screenshot hotkey (default Alt+A, settings page「截图」).
// 注意：与 SetTimer 的 ID 各自独立命名空间；取 11 避开 1xxx 菜单 ID 与定时器 2-6。
#define ID_HOTKEY_SHOT          11

// SetTimer ids。主窗口与鼠标钩子共用同一条定时器消息，必须全局唯一，
// 否则 SetTimer 传入相同 id 会静默重置前一个计时（长按触发即失效）。
#define TIMER_AUTOHIDE          2   // 失焦自动隐藏
#define TIMER_MOUSEHOLD         4   // 鼠标长按计时（MouseHook 启动、WM_TIMER 处理）
#define TIMER_TOPMOST           5   // 置顶看门狗
#define TIMER_ICONDBL           6   // 面板图标单击/双击启动门控（仅兜底清理用，非主判据）
#define TIMER_HOOKCHECK         7   // 低层鼠标钩子看门狗：定期重装，防止被系统超时静默移除
#define TIMER_TRIM              8   // 隐藏后延迟回收工作集（内存占用回落，见 MainWindow::Hide）

#endif // BONNY_RESOURCE_H
