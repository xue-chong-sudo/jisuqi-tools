# 截图识字 —— 源码

屏幕截图 + OCR 取字的 Windows 小工具，作为「极速启」的插件运行（也可独立启动）。
界面自绘无边框窗口 / 框选截图 / tesseract 识别 / 文本拼接与编辑 / 剪贴板复制。

## 目录

| 文件 | 行数 | 作用 |
|---|---|---|
| `src/view.cpp` | ~1575 | 界面与交互（**最大的一块**：窗口、框选、结果列表、工具栏） |
| `src/capture.cpp` | ~476 | 屏幕截图、多显示器、DPI、白名单/黑名单合成 |
| `src/text_join.h` | ~213 | 识别文本的拼接与换行规则（有独立测试） |
| `src/ocr_tesseract.cpp` | ~188 | tesseract 封装：初始化、识别、语言包 |
| `src/main.cpp` | ~158 | 入口、插件宿主通信 |
| `src/ocraddon.cpp/.h` | ~37 | 插件导出接口 |
| `src/ocraddon_internal.h` | ~80 | 内部结构与常量 |
| `src/test_main.cpp` / `text_join_test.cpp` | ~277 | 自测入口（不参与主程序链接） |
| `res/app.ico` `res/app.rc` | — | 图标资源 |

源码共 **3039 行**。

## 编译

```powershell
# 依赖会自动在几个已知位置找；也可以显式指定
powershell -ExecutionPolicy Bypass -File .\build.ps1
powershell -ExecutionPolicy Bypass -File .\build.ps1 -Deps D:\bt\tess
```
产物：`dist\截图识字.exe`

## 依赖（没随源码复制，约 46MB）

编译需要 tesseract 5.5 的头文件和静态库，目录结构：

```
deps\tesseract\
    include\            ← 头文件（785KB）
    lib\tesseract55.lib, leptonica-1.87.0.lib
    lib_static\tesseract55.lib   ← 43MB 静态库
```

现存位置（任选其一，用 `-Deps` 指过去即可）：
- `D:\jisuqi_build\v2shot\deps\tesseract`
- `D:\jisuqi_build_v2\deps\tesseract`
- `D:\bt\tess`

**运行**还需要 exe 旁边有：`leptonica-1.87.0.dll`、`jpeg8.dll`、`libpng16.dll`、`liblzma.dll`、
`deflate.dll`、`gif-7.dll`、`Lerc.dll` 等运行时 DLL，以及 `tessdata\`（语言包）。
这些都在部署目录 `桌面\极速启\screenshot\` 里。

## 注意

- 编译产物是 2.4MB 左右；如果大小明显不对，通常是链接到了动态库而不是 `lib_static` 里的静态库。
- `tessdata\` 里的 `chi_sim.traineddata` 决定能不能识别中文，缺了就只能识别英文。
- 这个项目**没有第三方 UI 框架**，全部是 Win32 + GDI；界面在 `view.cpp` 里手绘。
