# 截图识字 —— 代码解析（供 AI / 维护者快速上手）

> 截图 OCR 工具：截图后调用 tesseract 识别文字并复制到剪贴板，支持智能换行、退出模式等。由极速启在 `screenshot/` 子目录调用，也可独立运行（host 模式）。

## 1. 技术栈
- 纯 Win32 + C++，`/std:c++20` `/O2` **`/MD`（动态运行库，与极速启/全盘搜索的 /MT 不同！混链会报运行时冲突）**
- GDI / GDI+ 截图；`tesseract 5.5` + `leptonica 1.87` **静态库**
- 依赖在 `deps/tesseract/`（已纳入仓库，约 46MB；核心：`lib_static/tesseract55.lib` + `lib/leptonica-1.87.0.lib`）

## 2. 构建
```powershell
# 在 截图识字/ 目录下
powershell -ExecutionPolicy Bypass -File build.ps1 [-Deps <tesseract目录>]
# 默认自动探测 $root\deps\tesseract（仓库内已带，可开箱编译）
# 产物：dist/截图识字.exe（链接时名为 screenshot_host.exe）
```
- 链接：`lib_static/tesseract55.lib` + `lib/leptonica-1.87.0.lib`，及 comctl/gdiplus/gdi32 等。
- `build_host.bat` 为备用。

## 3. 文件结构与职责
| 文件 | 职责 |
|------|------|
| `main.cpp` | 入口 `wWinMain`；`ReadShotSettings` 读配置；`RunHost` 独立运行模式 |
| `capture.cpp` | 截图与屏幕绘制：`EnsureDpi`/`EnsureDwm`/`Snap`/`SetPixelBGRA`/`FillRectBGRA`/`Redraw` |
| `ocr_tesseract.cpp` | OCR 封装：`OcrInit`/`OcrShutdown`/`WaitAsyncOcr`/`W2U8`/`IsAllAscii` |
| `ocraddon.cpp/.h` | **插件导出接口**（供极速启调用）：`ocraddon_init`/`ocraddon_capture_and_copy`/`ocraddon_capture_direct`/`ocraddon_set_exit_mode`/`ocraddon_shutdown` |
| `view.cpp` | 结果视图与滚动条：`ContentW/H`/`ComputeOrigin`/`ClampScroll`/`ApplyScrollbars` |
| `text_join.h` | 文本智能拼接（换行逻辑） |
| `test_main.cpp` | 独立测试：`SaveBmp`/`CopyToClipboard` |
| `text_join_test.cpp` | 拼接单测 |
| `res/app.rc` / `app.ico` | 资源 |

## 4. 部署
`dist/截图识字.exe` 由极速启在 `screenshot/` 子目录调用。注意部署文件夹 `桌面\极速启\screenshot\` 内含全部**运行时 DLL**（leptonica-1.87.0.dll、tesseract 相关 DLL、msvcp140/vcruntime140 等）与 `tessdata/` 语言包——这是动态链接版，与仓库内 `deps` 的**静态库**是两回事。

## 5. 已知坑
1. **运行库不一致**：本项目 `/MD`，极速启/全盘搜索 `/MT`。若把静态库换成动态、或混用运行库，会在链接或运行时报 CRT 冲突。
2. **tessdata 不在仓库**：`.gitignore` 排除 `tessdata/`，运行时需自备语言包（部署文件夹已带）。
3. **deps 已入库**：`截图识字/deps/` 含 tesseract/leptonica 静态库（46MB），`build.ps1` 默认探测该路径，已可开箱编译。两份 `.gitignore` 的 `deps/` 忽略规则都用 `!deps/` + `!deps/**` 例外放行。

## 6. 依赖
- 编译期：MSVC 14.51 + Windows SDK + 仓库内 `deps/tesseract` 静态库。
- 运行期（部署）：`screenshot/` 目录内的 DLL + `tessdata/`。
