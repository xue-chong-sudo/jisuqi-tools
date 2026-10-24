# 极速启 · 启动面板 —— 源码

一个 Windows 桌面快速启动面板：无边框自绘窗口、图标网格、拼音搜索、分组/抽屉、
字母快捷键一键启动、拖拽添加、自定义图标。纯 Win32 + GDI/GDI+，无第三方框架。

## 目录

| 文件 | 作用 |
|---|---|
| `main.cpp` | 入口、单实例、DPI、消息循环 |
| `App.cpp/.h` | 应用装配：配置、图标管理、托盘、热键、面板 |
| `MainWindow.cpp/.h` | 主面板窗口（**最大的一块**）：图标网格、右键菜单、抽屉、搜索、快捷键 |
| `jisuq_ui_layout.cpp/.h` | 绘制辅助（圆角、文字、快捷键角标等） |
| `IconManager.cpp/.h` | 图标提取与缓存（PNG 落盘、文件名映射表） |
| `Config.cpp/.h` | `config.json` 读写 |
| `settings_ui.cpp/.h` | 设置界面 |
| `Pinyin.cpp/.h` | 拼音首字母匹配 |
| `TrayIcon.cpp/.h` | 托盘图标与菜单 |
| `MouseHook.cpp/.h` | 全局鼠标钩子（中键唤出面板） |
| `DragDrop.cpp/.h` | OLE 拖放 |
| `AutoStart.cpp/.h` | 开机自启 |
| `JisuQi.rc` / `app.ico` | 资源（图标） |
| `include/nlohmann/` | JSON 头文件（单头库，随源码带上） |

## 编译

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```
产物：`dist\极速启.exe`（单文件，静态链接 CRT）。

工具链：Visual Studio 2022（MSVC 14.51）+ Windows SDK 10.0.26100，路径写在 `build.ps1` 顶部，
换机器时改那两行即可。

## 换机/部署

程序是绿色版，可执行文件旁边就是数据目录：

```
极速启.exe
data\config.json      ← 图标、分组、快捷键
data\icons\           ← 图标缓存 PNG（文件名 = 原程序文件名）
data\settings.ini     ← 设置（热键、自动隐藏秒数等）
全盘搜索.exe          ← 内置的“全盘搜索”插件（另一个项目，与主程序同目录）
data\config.ini       ← 搜索窗口 UI 状态（由搜索插件写入，共用同一 data）
data\cache\           ← 搜索索引缓存（由搜索插件写入，共用同一 data）
screenshot\截图识字.exe ← 内置的“截图识字”插件（另一个项目）
```

## 注意（踩过的坑）

- `MainWindow.cpp` 里右键菜单是**整菜单自绘**（`MF_OWNERDRAW`）。系统菜单会在左侧固定预留一块
  check 区，没有 API 能改，只能自绘。
- 「移动分组」用的是**真子菜单**（`MF_POPUP`）。`MF_POPUP + MF_OWNERDRAW` 下系统会自己画子菜单三角，
  再自绘一枚就会变成“两个箭头”，且会给菜单留一条右侧空白。
- 菜单里的字母快捷键（G/D/N/F2）靠**只在 `TrackPopupMenu` 前后安装的线程键盘钩子**实现；
  菜单一关钩子就卸载，所以和主面板“按 A-Z 启动图标”不冲突。
- 图标文件名用「原始文件名 + .png」，映射表在 `data\icons\_index.ini`（UTF-16 + BOM，
  否则中文路径会被按 ANSI 写坏）。
