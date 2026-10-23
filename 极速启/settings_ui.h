//==============================================================
// 极速启 · 设置界面  共享头文件 (settings_ui.h)
//
// 该头文件把 settings_ui.cpp 的接口（数据结构 + 导出函数）暴露给主工程，
// 仅供主程序调用，不改动 settings_ui.cpp 内部的界面绘制逻辑。
//
// 设计要点：
//  - 配置以 data\settings.ini 为唯一权威来源（不写注册表，U 盘便携）；
//  - 旧的 data\config.json 仍负责图标/分组/窗口矩形/开机自启，二者职责不重叠；
//  - 主程序通过回调（SetSettingsApplyCallback 等）把“保存设置”挂接到自己的
//    热键注册 / 中键监听 / 失焦隐藏计时 / 单击双击逻辑上，settings_ui 不反向依赖 App。
//==============================================================
#pragma once
#include <windows.h>

// 设置数据结构（与 settings_ui.cpp 中的 static Settings g_s 保持一致）。
// 默认值即“开箱即用”的合理值。
struct Settings {
    bool    rememberSize = true;   // 记住窗口尺寸
    bool    middleButton = true;   // （已废弃，恒为 true）旧“鼠标中键呼出”开关，由 mouseBtn/holdMs 取代
    int     mouseBtn = 0;          // 呼出鼠标键：0=中键, 1=向后侧键(XBUTTON1), 2=向前侧键(XBUTTON2), 3=不使用鼠标呼出
    int     holdMs = 0;            // 长按毫秒：0=按下即触发；125/250/375/500=长按该时长才触发
    UINT    hkMod  = 0;            // 呼出快捷键修饰键（0=未设置）
    UINT    hkVk   = 0;            // 呼出快捷键虚拟键码（0=未设置 => 不注册全局快捷键）
    // 主界面「搜索框」快捷键（默认 Alt+F）：主面板打开时把焦点移到搜索框。
    // 与下方 searchHkMod/searchHkVk（Ctrl+F 唤起全盘搜索窗口）是两个不同的功能。
    UINT    sboxMod = MOD_ALT;     // 搜索框快捷键修饰键
    UINT    sboxVk  = 0x46;        // 搜索框快捷键主键，默认 'F'；0=不启用
    int     launchMode = 0;       // 面板图标启动方式：0=单击启动应用, 1=双击启动应用（托盘固定单击呼出）
    int     hideSec = 5;           // 失焦自动隐藏秒数
    bool    autoStart = false;     // 开机自启（写入 data\settings.ini，并同步 config.json / 启动文件夹 .lnk）

    // ---- 截图分组（settings.ini [screenshot] 段；配合 screenshot\截图识字.exe）----
    UINT    capHkMod = 1;          // 截图快捷键修饰键，默认 MOD_ALT（1）
    UINT    capHkVk  = 0x41;       // 截图快捷键主键，默认 'A'；0=不启用截图快捷键
    int     shotMode = 0;          // 识图方式：0=启动识图（截图后弹出图片识图）, 1=直接截图（截图后直接复制图片）
    bool    shotWrapSmart = true;  // 换行模式：true=智能换行（合并 OCR 碎行为段落）, false=保留原始换行
    int     shotExitMode = 0;      // 退出方式：0=复制文字后直接退出（默认）, 1=复制完文字后 ESC 退出

    // ---- 搜索分组（settings.ini [search] 段；配合 search\全盘搜索.exe）----
    // 「开启搜索」关闭时，Ctrl+F 不再唤起全盘搜索；「开启预览」控制搜索窗口右侧预览栏。
    bool    searchEnabled = true;  // 开启搜索（默认开）
    bool    searchPreview = true;  // 开启预览（默认开）
    UINT    searchHkMod = 2;       // 搜索快捷键修饰键，默认 MOD_CONTROL(2)
    UINT    searchHkVk  = 0x46;    // 搜索快捷键主键，默认 'F'；0=不启用
};

// 设置保存后由 settings_ui 回调主程序（重新注册热键 / 刷新中键 / 重置计时等）。
typedef void (*SettingsApplyCallback)();

// 注册“保存设置”回调（主程序在初始化时调用一次）。
void SetSettingsApplyCallback(SettingsApplyCallback cb);

// 注册“添加回收站到面板”回调（对应设置界面里的 ID_ADD 按钮）。
void SetSettingsAddRecycleBinCallback(void (*cb)());

// 注册“仅重置失焦隐藏计时”的轻量回调（步进器 / 数字编辑确认用，避免整链同步卡顿）。
void SetSettingsApplyAutoHideCallback(void (*cb)());

// 打开设置窗口。hInst 传主程序模块句柄，hOwner 传主窗口句柄（实现模态体验）。
void OpenSettingsWindow(HINSTANCE hInst, HWND hOwner);

// 返回实时设置对象（持久化到 data\settings.ini）。
Settings& GetSettings();

// 应用设置：内部调用主程序注册的回调。
void ApplySettings();

// 强制从 data\settings.ini 重新载入（供主程序启动时同步）。
void LoadSettingsUI();

// 强制把当前设置写入 data\settings.ini（供首次迁移/保存时调用）。
void SaveSettingsUI();

// 判断 data\settings.ini 是否已存在（用于首次迁移判断）。
bool SettingsUIExists();
