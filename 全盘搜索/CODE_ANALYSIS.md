# 全盘搜索 —— 代码解析（供 AI / 维护者快速上手）

> 极速启的"全盘搜索"插件：无边框搜索框，输入即时检索全盘文件/应用，结果可拖拽、可最大化/最小化、支持刷新。
> 仓库内另有 `docs/代码指引.md`、`docs/改版迭代与需求说明.md` 可供交叉参考。

## 1. 技术栈
- 纯 Win32 + C++，`/std:c++20` `/O2` `/MT`（静态运行库，与极速启一致）
- GDI+ 自绘；`dwmapi` 做圆角
- **单文件主体** `search_app.cpp`（约 2700 行）

## 2. 构建
```powershell
# 在 全盘搜索/ 目录下
powershell -ExecutionPolicy Bypass -File build.ps1
# 产物：dist/全盘搜索.exe（链接时先出 newsearch.exe 再改名）
```
- 图标由 `gen_ico.py` + `make_ico.cpp` 生成 `app.ico`。
- 依赖 MSVC 14.51 + Windows SDK 10.0.26100.0。

## 3. 文件结构与职责
| 文件 | 职责 |
|------|------|
| `search_app.cpp` | 全部逻辑：窗口过程、绘制、排序、圆角、刷新 |
| `app.rc` / `app.ico` / `app.manifest` | 资源与 DPI 清单 |
| `make_ico.cpp` | 图标导出小工具 |
| `gen_ico.py` | 由 PNG 生成 ico 的脚本 |
| `docs/*.md` | 既有设计/迭代文档 |

## 4. 关键函数（行号基于当前仓库）
- `WndProc` (120 / 1769 / 1903 / 2054)：多处分发的窗口过程。
- `SortResults` (124 / 872 / 936) + `CmpResults` (789)：排序。⚠️ 见第 6 节坑 1。
- `ApplyRoundRegion` (1354)：圆角区域，每次 `WM_SIZE` 重设。
- `PaintAll` (92 / 1363)：整窗绘制，收尾用 `GetDC(g_hwnd)` 整窗 `BitBlt`（绕过 `BeginPaint` 的无效区∩区域裁剪）。
- `DoRefresh` (463 / 2155) + `Z_REFRESH` (657 / 1510)：刷新按钮逻辑。
- `CompareStringEx(..., SORT_DIGITSASNUMBERS)` (792 / 807 / 2073)：早期排序实现，已弃用。

## 5. 部署
`dist/全盘搜索.exe` 由极速启在 `search/` 子目录调用（部署见 `桌面\极速启\search\全盘搜索.exe`）。

## 6. 已知坑与修复（重要，避免重踩）
1. **排序错乱**：原 `CmpResults` 返回三态 int(-1/0/1) 被当 `std::stable_sort` 的 bool 谓词（隐式转 true 致非法比较）；`CompareStringEx(SORT_DIGITSASNUMBERS)` 不满足严格弱序的传递性。修复：预计算排序键 + `memcmp` 比较（`search_app.cpp` 290 / 808 / 867 一带）。
2. **刷新按钮消失**：窗口过程未处理刷新命令，补 `Z_REFRESH` / `DoRefresh`。
3. **最大化/最小化点不动（弹按钮）**：`WM_NCHITTEST` 一律返回 `HTCLIENT`，最大化改在 `WM_LBUTTONDOWN` 自处理。
4. **标题切换发白**：重绘时序。
5. **拉伸死区（最隐蔽）**：Win10 不支持 `DWMWA_WINDOW_CORNER_PREFERENCE`，回退 `SetWindowRgn` 仅启动时设一次；区域坐标相对窗口左上角，尺寸变化时不扩张 → 右/下新增区画不上、旧像素永不重绘。修复：每次 `WM_SIZE` 调 `ApplyRoundRegion()`；`PaintAll` 收尾改用 `GetDC(g_hwnd)` 整窗 `BitBlt`；`WM_NCCALCSIZE` 用 `MONITORINFO.rcWork` 统一口径。

## 7. 依赖
仅 MSVC + Windows SDK + GDI+/dwmapi 系统库；无第三方二进制依赖。
