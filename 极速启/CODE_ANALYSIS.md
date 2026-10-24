# 极速启 —— 代码解析（供 AI / 维护者快速上手）

> 本文件用于让 AI 或新接手者无需通读全部源码即可定位修改点。最新版以本仓库 + `桌面\极速启\` 部署文件夹为准。

## 1. 项目概述
Windows 桌面快速启动工具。无边框 GDI+ 自绘窗口，支持：图标分组（抽屉/drawer）、右键菜单、拖拽重排、托盘、开机自启、拼音/名称搜索、设置界面。纯 Win32 原生 C++，无 MFC/Qt。

## 2. 技术栈
- 纯 Win32 API + C++（无框架）
- MSVC（VS2022，14.51），`/std:c++20` `/O2` `/MT`（**静态运行库**）
- GDI+ 自绘无边框窗口，DPI 感知（`main.cpp:EnableDpiAwareness`）
- 配置用 nlohmann/json 单头库（`include/nlohmann/json.hpp`）
- 配置文件：`dist/data/config.json`、`dist/data/settings.ini`；图标缓存 `dist/data/icons/.cache_v4`

## 3. 构建
```powershell
# 在 极速启/ 目录下
powershell -ExecutionPolicy Bypass -File build.ps1
# 产物：dist/极速启.exe
```
- 依赖 MSVC 14.51 + Windows SDK 10.0.26100.0，路径在 `build.ps1` 硬编码，换机需改。
- `build_local.bat` / `build_v2.bat` 为备用脚本。

## 4. 文件结构与职责
| 文件 | 职责 |
|------|------|
| `main.cpp` | 入口 `wWinMain`；`EnableDpiAwareness()` |
| `App.cpp/.h` | 应用核心：分组(drawer)数据模型，`AddPathAt` / `AddToDrawer` |
| `MainWindow.cpp/.h` | **主文件（约 3700 行）**：窗口过程、绘制、所有右键菜单、拖拽分组 |
| `jisuq_ui_layout.cpp/.h` | 主界面布局计算 |
| `settings_ui.cpp/.h` | 设置界面（含 EditSubProc/StepSubProc/SliderSubProc 子类化） |
| `Config.cpp/.h` | 配置读写 |
| `DragDrop.cpp/.h` | 拖放 |
| `IconManager.cpp/.h` | 图标提取与缓存 |
| `TrayIcon.cpp/.h` | 系统托盘 |
| `MouseHook.cpp/.h` | 全局低级鼠标钩子 |
| `Pinyin.cpp/.h` | 拼音/搜索匹配 |
| `AutoStart.cpp/.h` | 开机自启 |
| `resource.h` / `JisuQi.rc` / `app.ico` / `Bonny.exe.manifest` | 资源 |

## 5. 关键函数（行号基于当前仓库）
- `MainWindow::WndProc` (`MainWindow.cpp:469`)：主窗口过程；`WndProcThunk` (454) 转发到成员。
- `MainWindow::Paint(HDC)` (1781)：整窗自绘。
- `ShowIconMenu` (2911) / `ShowMemberMenu` (经 1072) / `ShowTabMenu` (1067/1109) / `ShowTrayMenu` (1278)：四类右键菜单。
- `MoveTopItemToInsert` (2734)：拖拽改分组/排序的核心。
- `MenuGetMsgProc` (355) + `SetWindowsHookExW(WH_GETMESSAGE, ...)` (2960)：**菜单消息钩子**，仅在 `TrackPopupMenu` 期间装载。

## 6. 已知坑与修复（避免重踩）
1. **右键菜单简化（已完成）**：已移除分隔线、Alt+ 前缀（含 Alt+B 快捷键映射）；菜单项间距由 `MenuPadX()`(141)/`MenuGapX()`(160) 控制，约 3 字符。`MainWindow.cpp:2919` 注释确认"菜单打开时按裸字母即可触发，无需 Alt"。
2. **D 键删除 vs 快速启动 无冲突**：菜单键盘钩子 `MenuGetMsgProc` 仅在 TrackPopupMenu 期间装载并吞键（return 1），主面板收不到，故菜单打开时按 D 删除不会触发快速启动。
3. **C++20 严格化**：临时对象取地址 → C2102（物化为具名局部变量）；GDI+ 重载歧义 → C2668（用 `static_cast<int>` 锁定 INT 重载）。
4. **标题栏双击最大化/切换时发白**：GDI+ 自绘重绘时序问题，绘制收尾见 `Paint`。

## 7. 部署/运行
- `dist/极速启.exe` 直接运行；数据在 `dist/data/`。
- 入口批处理：`启动极速启.bat`、`重置数据.bat`。
- 搜索插件 `全盘搜索.exe` 与截图插件 `截图识字.exe` 由极速启在 `search/`、`screenshot/` 子目录调用（见部署文件夹 `桌面\极速启\`）。

## 8. 依赖
仅 MSVC + Windows SDK；无第三方二进制依赖（json 为头文件）。
