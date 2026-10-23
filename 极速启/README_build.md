# 极速启 Windows 快速启动工具 — 编译与运行说明

极速启 是一个**单文件、免安装**的 Windows 快速启动工具。整份程序只有一个 x64 的
`JisuQi.exe`，运行时不依赖任何额外运行库（使用 `/MT` 静态链接 CRT），也不写注册表、
不联网、不收集数据。把整个 `极速启/` 文件夹拷贝到 U 盘或另一台电脑即可直接使用。

---

## 1. 目录结构

```
极速启v2-src/
├── main.cpp / App.cpp / MainWindow.cpp / TrayIcon.cpp
├── MouseHook.cpp / DragDrop.cpp / Config.cpp / IconManager.cpp
├── Pinyin.cpp / AutoStart.cpp / settings_ui.cpp / jisuq_ui_layout.cpp
├── *.h                      # 对应的头文件
├── JisuQi.rc / resource.h    # 资源（托盘图标）与消息/菜单 ID
├── app.ico                 # 托盘/窗口图标（已生成）
├── include/nlohmann/json.hpp# 单头 JSON 库（MIT，已下载 v3.11.3）
├── build_local.bat         # 标准 VS2022 开发构建（用 vcvars64，输出 JisuQi.exe）
├── build_v2.bat            # ASCII 路径构建（不依赖 vcvars64，见下方说明；输出 JisuQi.exe）
└── README_build.md
```

> ⚠️ 旧版 `build.bat`（仅含 v1.5 的源文件、漏编 `jisuq_ui_layout.cpp`）**对当前版本已失效**，请勿使用；请用 `build_local.bat` 或 `build_v2.bat`。
> 两份脚本都编译 `jisuq_ui_layout.cpp`，输出 `JisuQi.exe`，发布时重命名为 **极速启.exe**。

- 运行期自动在 **exe 同目录** 生成 `data/config.json` 与 `data/icons/*.png`（首次运行自动创建，无需预先打包）。

---

## 2. 使用 Visual Studio 2022 编译（推荐，目标交付方式）

1. 安装 **Visual Studio 2022 生成工具**（或完整 VS），勾选工作负载
   **“使用 C++ 的桌面开发”**（含 MSVC v14x、Windows 10/11 SDK）。
2. **把整个源码目录复制到一个全 ASCII 路径**（例如 `D:\jisuqi_build\v2`），
   因为 MSVC 在含中文/非 ASCII 的路径下对 `/utf-8` 源文件处理不稳，容易乱码或失败。
3. 在 ASCII 副本里双击 `build_local.bat`（标准开发机构建）：
   - 通过 `vswhere` 自动定位 VS 并加载 `vcvars64.bat`（x64 环境）；
   - 用 `cl.exe` 以 `/std:c++17 /O2 /MT /EHsc` 编译全部 `.cpp`（含 `jisuq_ui_layout.cpp`）；
   - 用 `rc.exe` 编译资源；
   - 用 `link.exe`（`/SUBSYSTEM:WINDOWS`，静态链接）生成 `JisuQi.exe`。
   - 若环境无法调用 `vcvars64.bat`（例如某些沙箱禁用了 reg 调用），改用
     **`build_v2.bat`**：它不依赖 `vcvars64`，而是直接写死 MSVC/SDK 的环境变量
     （与 `build_static.ps1` 同一布局），参数一致。
4. 编译成功后在目录内得到 `极速启.exe`，可直接发布。
   若提示找不到 VS，请确认上面勾选的工作负载已安装。

> 关键编译宏（缺一不可）：`/std:c++17 /MT /DJISUQI_EMBEDDED /D_WIN32_WINNT=0x0A00 /D_WIN32_IE=0x0600`。

---

## 3. 关于 MinGW

本项目**已不再提供** MinGW 构建（原 `Makefile.mingw` 未同步新增的 `Pinyin.cpp /
AutoStart.cpp / settings_ui.cpp`，且缺少 `-DJISUQI_EMBEDDED`，会与 `settings_ui.cpp`
内的独立 `wWinMain` 冲突，已删除）。请统一使用第 2 节的 MSVC `build.bat`。

> 关键编译宏（缺一不可）：`/std:c++17 /MT /DJISUQI_EMBEDDED /D_WIN32_WINNT=0x0A00 /D_WIN32_IE=0x0600`。

---

## 4. 运行与基本操作

- 双击 `极速启.exe`：仅在**系统托盘**出现图标，窗口默认隐藏。
- **鼠标中键**任意位置：呼出 / 隐藏窗口（toggle）。
- 窗口内：拖入 `.lnk` / `.exe` / 文件夹 / 文件即可添加；单击图标启动并自动隐藏；
  顶部搜索框实时过滤；`F2` 或图标右键菜单可重命名；右键菜单含
  “重命名 / 删除 / 打开所在位置”；`Esc` 隐藏；空白处右键隐藏；关闭按钮（×）仅隐藏到托盘。
- 托盘图标右键菜单：**显示窗口 / 退出**（只有“退出”才会真正结束进程）。
- 窗口位置会被记忆到 `data/config.json`，下次启动还原（首次运行屏幕居中）。

---

## 5. 依赖说明

- **JSON**：使用 nlohmann/json 单头文件（`include/nlohmann/json.hpp`，v3.11.3，MIT）。
  若需重新获取，可从此处下载并放到该路径：
  `https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp`
  （即 release 中的单头 `json.hpp`）。
- **图标**：系统外壳图标通过 `SHGetImageList` 提取（与资源管理器一致），
  `.lnk` 用 `IShellLink` 解析真实目标；缓存为 `data/icons/{hash}.png`（GDI+ 编码）。
- **API**：仅使用 Windows 自带（`user32 / gdi32 / shell32 / ole32 / comctl32 / gdiplus`），
  目标为单 x64 exe、静态链接、可整体拷贝。

---

## 6. 截图识字插件构建（screenshot-ocr）

极速启新增了「截图识字」插件，位于独立的 `screenshot-ocr/` 目录，与主程序
通过**进程间消息 + settings.ini** 协作（详见《源码导读》）。构建方式：

1. 同样把 `screenshot-ocr/src/` 复制到一个**全 ASCII 路径**（如 `D:\jisuqi_build\v2shot`），
   并把 Tesseract 的 `deps/tesseract/{include,lib,lib_static}` 放到该 ASCII 目录下。
2. 运行 `screenshot-ocr/build_host.bat`，得到 `dist/screenshot_host.exe`，
   重命名为 **截图识字.exe**，连同 `tessdata/`（语言包）与必要的运行库 DLL 一起
   放入主程序的 `screenshot/` 子目录即可。
3. 主程序只负责注册截图快捷键、触发消息；OCR 引擎、截图界面、结果窗口都在这个
   插件进程里（`ocraddon_init / capture_and_copy / capture_direct` 等 C 接口）。

> 该插件也可**独立运行**：`截图识字.exe`（不带参数）即旧版独立工具，自带
> `Ctrl+Shift+S` 热键；带 `--host <tessdata> <settings.ini>` 则为极速启宿主常驻模式。

## 7. 已知约束 / 备注

- 支持多分组（新建 / 重命名 / 删除 / 跨分组拖拽）与抽屉（二级收纳）视图。
- DPI：启动即启用每显示器 DPI 感知（PMv2），并响应 `WM_DPICHANGED` 重排。
- 本说明面向**开发者**；最终验收在用户的 VS2022 机器上完成。
