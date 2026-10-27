## 下载（免安装，解压即用）

到 **[Releases](../../releases)** 下载最新的 `jisuqi-v1.0-<日期>.zip`，解压到任意可写目录，双击 `极速启.exe` 即可。

- 只有一个前提：Windows 10 / 11 **64 位**。不写注册表、不联网、不收集数据。
- 想开机自启：面板里「设置 → 启动」打开开关。
- 想推到干净状态：双击 `重置数据.bat`（会清空配置与索引缓存）。

**详细用法见 → [极速启-使用说明.md](极速启-使用说明.md)**（面板、截图识字、全盘搜索三件套的完整说明）

---

# 极速启工具集

**一句话**：三个独立的 Windows 桌面小工具合集 —— 程序快速启动面板（极速启）+ 截图 OCR 识字（截图识字）+ 全盘文件搜索（全盘搜索）。

三个项目**互相独立**，各自能单独编译、单独运行；其中「截图识字」和「全盘搜索」也作为「极速启」的插件被调用（插件方式是：把编译出的 `全盘搜索.exe` 放到极速启 exe **同目录**、`截图识字.exe` 放到同级 `screenshot\` 目录）。主程序启动搜索插件时会传 `--datadir`，让两者的配置与索引共用同一个 `data\`，整包只保留一个 `data` 文件夹。

> 改哪个就提交哪个，不要顺手改别的项目。

---

## 仓库结构

```
极速启工具集/
├── README.md          ← 本文件（总说明）
├── .gitignore         ← 总忽略规则（编译产物 / 依赖 / 用户数据一律不入库）
├── 极速启/            ← 启动面板
├── 截图识字/          ← 截图 OCR
└── 全盘搜索/          ← 全盘文件搜索
```

---

## 三个子项目

| 子目录 | 是什么 | 技术栈 | 规模 |
|---|---|---|---|
| `极速启/` | Windows 桌面快速启动面板：无边框自绘窗口、图标网格、拼音搜索、分组/抽屉、字母快捷键一键启动、拖拽添加、自定义图标、托盘 | C++ + 原生 Win32 / GDI / GDI+，**无第三方 UI 框架**；随源码带单头 JSON 库 `include/nlohmann/json.hpp` | 12 个 `.cpp` + 11 个 `.h` |
| `截图识字/` | 屏幕截图 + OCR 取字：框选截图、tesseract 识别、文本拼接与编辑、剪贴板复制 | C++ + 原生 Win32 / GDI，OCR 用 **Tesseract 5.5 + Leptonica** | `src\` 下 10 个源文件，共 **3039 行** |
| `全盘搜索/` | 按文件名快速搜索本机所有固定硬盘（不做内容搜索）。自建索引 + 内存快照 | C++ + 原生 Win32 / GDI，单文件实现 | `search_app.cpp` 单文件，约 **2650 行** |

---

## 怎么编译

每个子目录里都有 `build.ps1`，一键编译：

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

产物分别在各自目录的 `dist\` 下：

| 子目录 | 产物 | 运行时库 |
|---|---|---|
| `极速启\` | `dist\极速启.exe` | `/MT` 静态链接 CRT，单文件即可跑 |
| `截图识字\` | `dist\截图识字.exe` | `/MD` 动态 CRT，需要额外 DLL（见下） |
| `全盘搜索\` | `dist\全盘搜索.exe` | `/MT` 静态链接 CRT，单文件即可跑 |

工具链写在每个 `build.ps1` 顶部，换机器时只改这几行：

```powershell
$msvc = 'D:\Visual Studio 2022\VC\Tools\MSVC\14.51.36231'
$sdk  = 'D:\Windows Kits\10'
$sdkv = '10.0.26100.0'
```

即 **Visual Studio 2022（MSVC 14.51）+ Windows SDK 10.0.26100**。

### 语言标准档位（实测，三个项目不一致）

| 子目录 | `build.ps1` 实际参数 |
|---|---|
| `极速启\` | `/std:c++17` |
| `全盘搜索\` | `/std:c++20`（另带 `/utf-8`） |
| `截图识字\` | **未显式指定 `/std:`**，即 MSVC 默认档（C++14） |

> ⚠️ 这是现状记录，不是建议值。三个项目各自能编译通过即可；若要统一到 C++20，需逐个改 `build.ps1` 并重新验证。

---

## 依赖说明

### 极速启、全盘搜索 —— 只依赖 Windows SDK

不需要任何第三方库。`cl.exe` + `rc.exe` 直接编译，链接的系统库就是 `user32 / gdi32 / shell32 / comctl32 / advapi32` 那一批。

换台全新机器：装好 VS 2022（含 C++ 桌面开发工作负载）和 Windows 10/11 SDK，改 `build.ps1` 顶部三行路径，就能编出来。

### 截图识字 —— 需要 Tesseract / Leptonica

**编译时**需要 tesseract 5.5 的头文件和静态库（约 46 MB，**没有**随源码进仓库，已在 `.gitignore` 里排除 `deps/`）：

```
deps\tesseract\
    include\                        ← 头文件
    lib\tesseract55.lib
    lib\leptonica-1.87.0.lib
    lib_static\tesseract55.lib      ← 43 MB 静态库
```

`build.ps1` 会依次在下列位置自动找，也可以显式指定：

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1 -Deps D:\bt\tess
```

自动搜索路径（按序）：
1. `<项目目录>\deps\tesseract`
2. `D:\jisuqi_build\v2shot\deps\tesseract`
3. `D:\jisuqi_build_v2\deps\tesseract`
4. `D:\bt\tess`

本机另有一份备份在 `D:\bs`，需要时用 `-Deps` 指过去即可。

**运行时**还需要把下面这些东西放到 exe 同目录：

- 运行时 DLL：`leptonica-1.87.0.dll`、`jpeg8.dll`、`libpng16.dll`、`liblzma.dll`、`deflate.dll`、`gif-7.dll`、`Lerc.dll` 等
- 语言包目录：`tessdata\`（**缺 `chi_sim.traineddata` 就只能识别英文**）

---

## 换台电脑怎么恢复

```powershell
# 优先从 Gitee 克隆（国内更快更稳），也可改从 GitHub 克隆，二者内容一致
git clone https://gitee.com/xue-chong-sudo/jisuqi-tools.git
cd jisuqi-tools
# 然后进各自子目录编译
powershell -ExecutionPolicy Bypass -File .\极速启\build.ps1
```

仓库里**只有源码、构建脚本、文档和图标资源**。以下这些不入库，随时能重新编译 / 重新生成：

- 编译产物：`dist\`、`*.obj`、`*.res`、`*.exe`、`*.pdb`、`*.exp`、`*.lib`
- 第三方依赖：`deps\`
- 用户数据与运行时配置：`data\`、`*.idx`、`config.json`、`settings.ini`、`icons\`、`tessdata\`
- 临时文件：`*.tmp`、`*.log`

---

## 备份与同步（GitHub + Gitee 双私有仓库）

代码同时保存在两个**私有**仓库，互为备份：

| 平台 | 仓库地址 | 可见性 |
|---|---|---|
| GitHub | https://github.com/xue-chong-sudo/jisuqi-tools | 私有 |
| Gitee  | https://gitee.com/xue-chong-sudo/jisuqi-tools   | 私有 |

两个仓库内容完全一致（同一份 `main` 分支）。任一个平台临时不可达，另一个仍能正常备份与恢复。

### 怎么一键同步

仓库根目录提供两个文件：

- `同步到云端.bat` —— 双击入口（在资源管理器里双击即可）
- `sync.ps1` —— 实际同步逻辑（由 `.bat` 调用）

双击 `同步到云端.bat` 会：

1. 检查是否有未提交的改动；有就提示并**停止**，绝不强制推送；
2. 依次 `git push gitee main` 和 `git push origin main`；
3. 把每个远端的结果（成功 / 失败 + 原因）用中文清楚打印；
4. 失败时给出可执行的处理建议（例如「GitHub 超时，但 Gitee 已成功，代码已安全」）。

> 认证说明：脚本不含任何令牌明文。推送用的令牌存在 Windows 凭据管理器，由 `credential.helper=manager` 自动取用。换机器或令牌过期后，重新生成私有令牌并用 `git credential approve` 存回即可。

### 换台电脑怎么恢复

从任一平台克隆都能拿到完整源码：

```powershell
# 方式一：从 Gitee（国内通常更快更稳）
git clone https://gitee.com/xue-chong-sudo/jisuqi-tools.git
# 方式二：从 GitHub
git clone https://github.com/xue-chong-sudo/jisuqi-tools.git
cd jisuqi-tools
# 然后进各自子目录编译
powershell -ExecutionPolicy Bypass -File .\极速启\build.ps1
```

---

## 提醒

三个项目互相独立，改哪个就提交哪个。提交前建议先 `git status` 看一眼，确认没有把 `dist\` 或 `data\` 这类东西带进去。
