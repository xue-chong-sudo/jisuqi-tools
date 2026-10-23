#pragma once
// ---------------------------------------------------------------------------
// jisuq_ui_layout.h —— 极速启主面板「单一尺寸出口 + 绘制原语」
//
// 设计目标（来自 kimi 优化方案，采纳 80%）：
//   1. 所有尺寸只从一个函数 GetMetrics(dpi) 取，以后想调间距只改这一处；
//   2. CalcGrid 决定列数（最小 4 列，宽度够一列自动 +1）；
//   3. ItemPos 由列/行推导单元格矩形；
//   4. DrawAppItem / DrawDrawer / DrawTile 负责把"图标 + 名字"画成
//      浅灰悬浮底（#F0F0F2，圆角 10），彻底告别系统蓝框。
//
// 本模块不依赖 MainWindow / Config，所有数据通过参数显式传入，便于单测与复用。
// ---------------------------------------------------------------------------
#include <windows.h>
#include <gdiplus.h>
#include <string>

namespace JisuqUI {

struct Metrics {
    double scale;        // dpi / 96.0
    int margin;          // 窗口边距
    int cellW;           // 单元宽（不含列距）
    int rowH;            // 行高（图标 + 名字）
    int colGap;          // 列距
    int rowGap;          // 行距
    int icon;            // 图标尺寸
    int mini;            // 抽屉 2x2 缩略图尺寸
    int miniGap;         // 缩略图间距
    int nameGap;         // 名字距图标（0 = 紧贴）
    int nameH;           // 名字行高
    int tileW;           // 悬浮/选中高亮块宽度（比单元格窄，居中，视觉更收敛）
    int tilePadY;        // 高亮块上下内边距
    int titleH;          // 标题栏高
    int searchH;         // 搜索框高
    Gdiplus::REAL corner;// 圆角半径
};

// 单一出口：所有尺寸都从这里取（已按 dpi 缩放并取整）。
// 想调间距 → 只改这个函数的基准值（100% DPI 下的像素）。
Metrics GetMetrics(int dpi);

// CalcGrid：根据网格可用矩形计算列数。最小 4 列，宽度够一列自动 +1。
// （窗口是固定尺寸，网格左对齐铺满即可；若以后想靠右，这里加居中偏移。）
void CalcGrid(const Metrics& m, int gridLeft, int gridTop,
              int gridRight, int gridBottom, int& cols);

// ItemPos：第 (col,row) 个格子的矩形（由网格左上角推导）。
Gdiplus::RectF ItemPos(const Metrics& m, int col, int row,
                       int gridLeft, int gridTop);

// ItemTileRect：某个格子里「图标 + 名字」高亮块的实际矩形（与绘制端同一几何）。
// MainWindow 的点击命中区用它，保证「点空白不选中、点蓝框才选中」。
Gdiplus::RectF ItemTileRect(const Metrics& m, const Gdiplus::RectF& cell, bool hasName);

// 圆角矩形路径（供 Fill/Draw 复用）。
void RoundRectPath(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r,
                   Gdiplus::REAL radius);

// 高亮块：按传入矩形精确填充（hover 浅灰 #F0F0F2 / 选中浅蓝 kSelFill），
// 圆角 corner。矩形由调用方按「图标 + 名字」内容算好后传入，所以内容变矮时
// 块也会跟着变矮（不再固定铺满整格）。
void DrawTile(Gdiplus::Graphics& g, const Gdiplus::RectF& rect,
              Gdiplus::REAL corner, bool hover, bool selected);

// 普通应用项：图标（HighQualityBicubic 缩放）+ 名字（YaHei UI 11px、居中、
// #50545C、距图标 nameGap）。drawName=false 时只画图标（重命名态由编辑框覆盖名字）。
// shortcut != 0 时在图标右下角叠一枚字母角标（粗体白字 + 黑描边）。
void DrawAppItem(Gdiplus::Graphics& g, const Metrics& m, Gdiplus::Bitmap* icon,
                 const std::wstring& name, const Gdiplus::RectF& cell,
                 bool selected, bool hover, bool drawName,
                 wchar_t shortcut = 0);

// 抽屉：虚线圆角容器 + 2x2 迷你图标（吃 IconManager 的 Bitmap* 缓存）+ 名字。
// members 至多 4 个，不足补 nullptr。
void DrawDrawer(Gdiplus::Graphics& g, const Metrics& m, Gdiplus::Bitmap* members[4],
                const std::wstring& name, const Gdiplus::RectF& cell,
                bool selected, bool hover, wchar_t shortcut = 0);

} // namespace JisuqUI
