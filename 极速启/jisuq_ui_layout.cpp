#include "jisuq_ui_layout.h"

namespace JisuqUI {

// ---- 配色（与 MainWindow 中的常量保持一致，UI 调色在此一处） ----
static const Gdiplus::Color cHover(0xFF, 0xF0, 0xF0, 0xF2);   // 悬浮浅灰 #F0F0F2
static const Gdiplus::Color cSelFill(0xFF, 0xE5, 0xF4, 0xFD); // 选中浅蓝
static const Gdiplus::Color cAccent(0xFF, 0x29, 0xA8, 0xE0);  // logo 蓝
static const Gdiplus::Color cName(0xFF, 0x50, 0x54, 0x5C);    // 名字 #50545C

// 100% DPI 下的基准尺寸（括号内为常用缩放，运行时由 GetMetrics 按 dpi 算）。
//   窗口边距10 / 单元宽76 / 行高67 / 列距5 / 行距6 / 图标40 / mini15 / 间距3
//   / 名字距2 / 名字高14 / 高亮块宽68·内边距4 / 标题30 / 搜索框32 / 圆角10
// 说明：kimi 原表单元宽 66 偏窄（名字区只剩 ~62px，长名会被 ellipsis 切得更狠），
// 这里取 76 折中，图标保持 40。
Metrics GetMetrics(int dpi) {
    double s = dpi / 96.0;
    auto R = [s](double v) { return static_cast<int>(v * s + 0.5); };
    Metrics m{};
    m.scale   = s;
    m.margin  = R(10);
    m.cellW   = R(76);
    m.rowH    = R(67);
    m.colGap  = R(5);
    m.rowGap  = R(6);
    m.icon    = R(40);
    m.mini    = R(15);
    m.miniGap = R(3);
    m.nameGap = R(2);     // 名字距图标：1.3 曾收紧到 0，本轮加回 2px 看效果
    m.nameH   = R(14);
    m.tileW   = R(68);    // 高亮块比单元格(76)窄，居中，看起来更"收"
    m.tilePadY= R(4);     // 高亮块上下各留 4px（随间距一起放大一点）
    m.titleH  = R(30);
    m.searchH = R(32);
    m.corner  = static_cast<Gdiplus::REAL>(R(10));
    return m;
}

void CalcGrid(const Metrics& m, int gl, int /*gt*/, int gr, int /*gb*/, int& cols) {
    int strideX = m.cellW + m.colGap;
    int avail = (gr - gl) - strideX;             // 减去第一列占用的步距
    int c = 1 + (avail > 0 ? avail / strideX : 0);
    if (c < 1) c = 1;                            // 最少 1 列；宽度变窄时自动缩成 3/2/1 列
    cols = c;
}

Gdiplus::RectF ItemPos(const Metrics& m, int col, int row, int gl, int gt) {
    int strideX = m.cellW + m.colGap;
    int strideY = m.rowH + m.rowGap;
    return Gdiplus::RectF(static_cast<Gdiplus::REAL>(gl + col * strideX),
                          static_cast<Gdiplus::REAL>(gt + row * strideY),
                          static_cast<Gdiplus::REAL>(m.cellW),
                          static_cast<Gdiplus::REAL>(m.rowH));
}

void RoundRectPath(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r,
                   Gdiplus::REAL radius) {
    path.Reset();
    Gdiplus::REAL d = 2 * radius;
    if (d > r.Width)  d = r.Width;
    if (d > r.Height) d = r.Height;
    path.StartFigure();
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
}

void DrawTile(Gdiplus::Graphics& g, const Gdiplus::RectF& rect,
              Gdiplus::REAL corner, bool hover, bool selected) {
    if (!(hover || selected)) return;
    Gdiplus::GraphicsPath p;
    RoundRectPath(p, rect, corner);
    Gdiplus::Color fill = selected ? cSelFill : cHover;
    Gdiplus::SolidBrush tileFillBrush(fill);
    g.FillPath(&tileFillBrush, &p);
    Gdiplus::Color edge = selected ? cAccent : Gdiplus::Color(0xFF, 0xE3, 0xE3, 0xE6);
    Gdiplus::Pen tileEdgePen(edge, 1.0f);
    g.DrawPath(&tileEdgePen, &p);
}

// 计算「图标 + 名字」内容块的几何：内容在视觉行高 rowH 里垂直居中，
// 名字紧贴图标下方 nameGap 处。cell 传进来的宽高含列/行间距，这里只用
// Metrics 的 cellW / rowH（真实视觉尺寸），避免把间距算进居中和名字位置。
struct Block {
    int iconX, iconY;   // 图标左上角
    int contentH;       // 图标 + 间距 + 名字 的总高
    int nameTop;        // 名字顶部 y
    Gdiplus::RectF tile;// 高亮块矩形（贴合内容）
};

static Block LayoutBlock(const Metrics& m, const Gdiplus::RectF& cell, bool hasName) {
    Block b{};
    const int visualW = m.cellW;
    const int visualH = m.rowH;
    const int gap    = hasName ? m.nameGap : 0;
    b.contentH = m.icon + gap + (hasName ? m.nameH : 0);
    int topPad = (visualH - b.contentH) / 2;
    if (topPad < 0) topPad = 0;
    b.iconX = static_cast<int>(cell.X) + (visualW - m.icon) / 2;
    b.iconY = static_cast<int>(cell.Y) + topPad;
    b.nameTop = b.iconY + m.icon + m.nameGap;
    b.tile = Gdiplus::RectF(
        static_cast<Gdiplus::REAL>(cell.X) + (visualW - m.tileW) / 2.0f,
        static_cast<Gdiplus::REAL>(b.iconY - m.tilePadY),
        static_cast<Gdiplus::REAL>(m.tileW),
        static_cast<Gdiplus::REAL>(b.contentH + m.tilePadY * 2));
    return b;
}

Gdiplus::RectF ItemTileRect(const Metrics& m, const Gdiplus::RectF& cell, bool hasName) {
    return LayoutBlock(m, cell, hasName).tile;
}

// 名字统一走这里：YaHei UI 11px、居中、超宽省略号、颜色 #50545C。
static void DrawName(Gdiplus::Graphics& g, const Metrics& m,
                     const std::wstring& name, Gdiplus::REAL x, int topY) {
    if (name.empty()) return;
    Gdiplus::RectF nr(x, static_cast<Gdiplus::REAL>(topY),
                      static_cast<Gdiplus::REAL>(m.cellW),
                      static_cast<Gdiplus::REAL>(m.nameH));
    Gdiplus::Font f(L"Microsoft YaHei UI",
                    static_cast<Gdiplus::REAL>(11 * m.scale),
                    Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::StringFormat sf;
    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
    sf.SetLineAlignment(Gdiplus::StringAlignmentNear);
    sf.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    sf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    Gdiplus::SolidBrush nameBrush(cName);
    g.DrawString(name.c_str(), -1, &f, nr, &sf, &nameBrush);
}

// 第5条需求：给绑定了字母快捷键的图标画角标（图标右下角，粗体白字 + 黑描边）。
// 描边用「黑字八方向微偏移 + 白字压在上面」的经典做法，比 GraphicsPath 描边
// 便宜，而且在浅色/深色图标上都能读得清。
static void DrawShortcutBadge(Gdiplus::Graphics& g, const Metrics& m, wchar_t ch,
                              int iconX, int iconY) {
    if (ch < L'A' || ch > L'Z') return;
    wchar_t buf[2] = { ch, 0 };
    Gdiplus::Font f(L"Microsoft YaHei UI",
                    static_cast<Gdiplus::REAL>(11 * m.scale),
                    Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::StringFormat sf;
    sf.SetAlignment(Gdiplus::StringAlignmentNear);
    sf.SetLineAlignment(Gdiplus::StringAlignmentNear);
    sf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap |
                      Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
    Gdiplus::RectF bb(0, 0, 0, 0);
    g.MeasureString(buf, 1, &f, Gdiplus::PointF(0, 0), &sf, &bb);

    const Gdiplus::REAL sc = static_cast<Gdiplus::REAL>(m.scale);
    Gdiplus::REAL tx = static_cast<Gdiplus::REAL>(iconX + m.icon) - bb.Width - sc;
    Gdiplus::REAL ty = static_cast<Gdiplus::REAL>(iconY + m.icon) - bb.Height - sc * 0.5f;

    Gdiplus::SolidBrush edge(Gdiplus::Color(210, 0, 0, 0));
    const Gdiplus::REAL o = sc;   // 描边半径
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (!dx && !dy) continue;
            g.DrawString(buf, 1, &f, Gdiplus::PointF(tx + dx * o, ty + dy * o), &sf, &edge);
        }
    }
    Gdiplus::SolidBrush fill(Gdiplus::Color(255, 255, 255, 255));
    g.DrawString(buf, 1, &f, Gdiplus::PointF(tx, ty), &sf, &fill);
}

void DrawAppItem(Gdiplus::Graphics& g, const Metrics& m, Gdiplus::Bitmap* icon,
                 const std::wstring& name, const Gdiplus::RectF& cell,
                 bool selected, bool hover, bool drawName, wchar_t shortcut) {
    const bool hasName = !name.empty();
    Block b = LayoutBlock(m, cell, hasName);
    DrawTile(g, b.tile, m.corner, hover, selected);

    // 缩 PNG/ICO 前用高质量双三次插值，小图标不发糊。
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    if (icon) g.DrawImage(icon, b.iconX, b.iconY, m.icon, m.icon);
    g.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);

    DrawShortcutBadge(g, m, shortcut, b.iconX, b.iconY);

    // 重命名态（drawName=false）仍保留名字占位，图标位置不跳动，编辑框盖在名字处。
    if (drawName) DrawName(g, m, name, cell.X, b.nameTop);
}

void DrawDrawer(Gdiplus::Graphics& g, const Metrics& m, Gdiplus::Bitmap* members[4],
                const std::wstring& name, const Gdiplus::RectF& cell,
                bool selected, bool hover, wchar_t shortcut) {
    const bool hasName = !name.empty();
    Block b = LayoutBlock(m, cell, hasName);
    DrawTile(g, b.tile, m.corner, hover, selected);

    // 去掉原来外面的虚线示意框，直接在图标区域居中放 2x2 迷你图标。
    const int grid = m.mini * 2 + m.miniGap;
    const int gx = b.iconX + (m.icon - grid) / 2;
    const int gy = b.iconY + (m.icon - grid) / 2;
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    for (int i = 0; i < 4; ++i) {
        if (!members[i]) continue;
        int col = i % 2, row = i / 2;
        g.DrawImage(members[i],
                    gx + col * (m.mini + m.miniGap),
                    gy + row * (m.mini + m.miniGap),
                    m.mini, m.mini);
    }
    g.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);

    DrawShortcutBadge(g, m, shortcut, b.iconX, b.iconY);

    DrawName(g, m, name, cell.X, b.nameTop);
}

} // namespace JisuqUI
