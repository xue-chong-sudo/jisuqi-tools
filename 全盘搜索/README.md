# 全盘搜索 —— 源码

按文件名快速搜索本机所有固定硬盘的文件（不是内容搜索）。作为「极速启」的插件运行，
也可以单独启动。自建索引 + 内存快照，纯 Win32 + GDI，单文件实现。

## 目录

| 文件 | 作用 |
|---|---|
| `search_app.cpp` | **全部源码**（约 2650 行）：索引、搜索线程、列表/预览/分类、自绘窗口、各类弹出层 |
| `app.rc` / `app.ico` / `app.manifest` | 资源与 DPI 声明 |
| `build.ps1` | 一键编译（→ `dist\全盘搜索.exe`） |
| `build.bat` | 原版批处理构建（产物叫 `newsearch.exe`） |
| `gen_ico.py` / `make_ico.cpp` | 生成 `app.ico`（`app.ico` 已在，一般用不到） |
| `docs\代码指引.md` | 架构说明：目录结构、线程模型、布局/命中体系、无边框窗口关键消息、config 键表 |
| `docs\改版迭代与需求说明.md` | 各轮需求与实现要点记录 |

## 编译

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```
产物：`dist\全盘搜索.exe`。工具链 MSVC 14.51 + SDK 10.0.26100，路径在脚本顶部。

## 运行时数据

exe 旁边：

```
data\cache\vol_X.idx   ← 每个盘的索引缓存（超过 6 小时自动重建；状态栏「刷新」或 F5 可强制重建）
data\config.ini        ← 窗口位置、列宽、排序状态、分类顺序
```

## 注意（这块代码里踩过的坑，改之前务必先看）

1. **排序比较器**：`CmpResults` 返回三态 `int`（-1/0/1），**不能**直接当 `std::sort` 的谓词 ——
   `-1` 和 `+1` 都会被转成 `true`，比较器非法，结果是“看起来完全没排序”。必须包 `[](){ return CmpResults(a,b) < 0; }`。
2. **名称列排序键**：`CompareStringEx(..., SORT_DIGITSASNUMBERS)` **不满足传递性**，同样不能直接当谓词。
   现在用「`NameBucket` 分桶 + 数字补零定宽 + `LCMAP_SORTKEY` 生成键 + memcmp」。
   排序优先级是用户指定的：**字母 → 数字 → 中文拼音 → 符号**（和资源管理器不同）。
3. **Win10 圆角**：`DWMWA_WINDOW_CORNER_PREFERENCE` 只有 Win11 支持，Win10 回退 `SetWindowRgn`。
   而窗口区域**不随尺寸扩张**，MSDN 明确警告设过区域的窗口改大小会留下“永不重绘”的死区。
   所以每次 `WM_SIZE` 都要 `ApplyRoundRegion()` 重设，拖动期间（`WM_ENTERSIZEMOVE`）先撤掉。
4. **贴图**：`PaintAll` 末尾必须用 `GetDC` 而不是 `BeginPaint` 的 DC —— 后者的裁剪区是
   「无效区 ∩ 窗口区域」，新增的边缘永远贴不上去。
5. **标题栏按钮**：`WM_NCHITTEST` 不能返回 `HTMINBUTTON/HTMAXBUTTON/HTCLOSE`，
   否则 DWM 会再画一套原生按钮盖在自绘按钮上，点击也会被系统接管。现在统一返回 `HTCLIENT`，
   在 `WM_LBUTTONDOWN` 里自己 `ShowWindow`。
6. **`WM_NCCALCSIZE`**：`wp=TRUE`（client==window）和 `wp=FALSE`（交回 `DefWindowProc`）两个分支
   必须同一口径，否则客户区和窗口会差出一圈，中间夹一条谁都不画的竖条。
