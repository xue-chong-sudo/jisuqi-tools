#ifndef BONNY_MAINWINDOW_H
#define BONNY_MAINWINDOW_H

#include <windows.h>
#include <string>
#include <vector>
#include "Config.h"
#include "DragDrop.h"
#include "jisuq_ui_layout.h"   // 主面板布局单一出口（GetMetrics / CalcGrid / 绘制原语）

// Gdiplus is only referenced by signature (DrawItem takes a Graphics&),
// so a forward declaration keeps the header light.
namespace Gdiplus { class Graphics; }

namespace JisuQi {

class App;

// The borderless, topmost, always-on-top launcher window. Draws the
// icon grid, search box, tab bar and handles input (mouse, keyboard,
// drag-drop, context menus). All business logic is delegated to App /
// Config / IconManager; this class owns only presentation + input.
class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    bool Create(HINSTANCE hInst, App* app);
    HWND GetHwnd() const { return m_hwnd; }
    DWORD GetCreateError() const { return m_createError; }

    void Show();
    void Hide();
    bool IsVisible() const;

    // Called after items are added/removed/renamed elsewhere.
    void OnItemsChanged();

    WNDPROC GetOldSearchProc() const { return m_oldSearchProc; }
    WNDPROC GetOldRenameProc() const { return m_oldRenameProc; }

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);

    void Relayout();
    void RebuildVisible();
    void InvalidateGrid();
    void UpdateScrollInfo();
    void SetScroll(int y);
    int GridContentHeight() const;
    RECT ScrollbarRect() const;
    RECT ScrollbarThumbRect() const;
    RECT CellRect(int visibleIndex) const;
    int HitTestIcon(POINT pt) const;          // cell-wide (drag-drop insertion)
    int HitTestIconBody(POINT pt) const;      // image+text only ( Requirement 7 )
    RECT IconBodyRect(int visibleIndex) const;
    int HitTestMemberBody(POINT pt) const;    // image+text only
    RECT MemberBodyRect(int memberIndex) const;
    bool InCloseButton(POINT pt) const;
    void Paint(HDC hdc);
    void SavePosition();

    // Draws a single icon cell (icon + name, plus the 2x2 preview for
    // drawers). Used by both the closed grid and the drawer split view.
    void DrawItem(Gdiplus::Graphics& g, const Item& it, RECT cell,
                  bool selected, bool skipName, double s, bool hover = false);

    void LaunchItem(int visibleIndex);
    void StartRename(int visibleIndex);
    void CommitRename(const std::wstring& name);
    void CancelRename();
    void DeleteItem(int visibleIndex);
    void OpenLocation(int visibleIndex);
    void ShowIconMenu(int visibleIndex, POINT pt);
    // 「更换图标」：选图片 → 统一转 PNG 存进 data/icons → 写回 config。
    void ChangeItemIcon(int visibleIndex);
    void ShowTrayMenu();
    std::wstring GetSearchText() const;

    // ---- 图标字母快捷键（A-Z 快速启动）----
    // 主面板可见时按下字母即启动绑定了该字母的图标（并隐藏面板）。
    // 返回 true 表示该字母已绑定并被处理；false 表示未绑定（调用方应让键落到搜索框）。
    bool TryLaunchByShortcut(wchar_t ch);
    // 绑定/清除某个图标（m_visible 下标）的快捷键；ch=0 表示清除。
    // 同一字母全局唯一：绑定新图标会自动清掉别处的旧绑定。
    void SetItemShortcut(int visibleIndex, wchar_t ch);
    // 「快捷键...」弹窗：输入单个 A-Z 字母，框内浅色提示，右侧带「清除」。
    void ShowShortcutDialog(int visibleIndex);
    // 刷新搜索框占位提示（把当前「搜索框快捷键」写进 cue banner）。
    void UpdateSearchCueBanner();
    // 判断当前按键是否匹配「搜索框快捷键」（默认 Alt+F）。
    bool MatchSearchBoxHotkey(WPARAM vk) const;
    // 返回绑定了该字母的图标名（含当前图标自身）；没人绑定时返回空串。
    std::wstring FindShortcutOwnerName(wchar_t ch) const;

    // ---- Drawer support (v3 split view) ----
    // A drawer is a normal Item with isDrawer=true whose children live in
    // Item::members. The open drawer is shown as a split view: the parent
    // group is dimmed in the upper half, the drawer members in the lower.
    void CreateDrawer();                       // new empty drawer (auto-named 抽屉N)
    void OpenDrawer(int cfgIndex);             // navigate into a drawer
    void CloseDrawer();                        // navigate back to the top level
    void LaunchDrawerAll(int visibleIndex);    // Ctrl+click: launch every member
    void LaunchDrawerMember(int drawerCfgIndex, int memberIndex);
    bool IsDrawerOpen() const { return m_openDrawer >= 0; }
    int OpenDrawerCfgIndex() const { return m_openDrawer; }
    int DrawerCellAt(POINT pt) const;          // visible index of a closed drawer under pt, or -1
    int HitTestMember(POINT pt) const;         // member index when a drawer is open, or -1
    size_t VisibleToCfg(int vi) const { return m_visible[vi]; }

    // Returns the index (into m_visible) before which a drop at client
    // point pt should be inserted: dropping on the right half of a cell
    // inserts after it, the left half before it; empty/over-grid -> end.
    int InsertIndexAt(POINT pt) const;

    // ---- Internal drag-reorder helpers (Bug H) ----
    // The drag SOURCE is recorded by cfg index (not m_visible index) so that
    // a mid-drag group switch - which rebuilds m_visible - never invalidates
    // the source. m_dragSrcDrawer selects the source kind:
    //   < 0  => a top-level item (m_dragSrcItem = index into cfg.items)
    //   >= 0 => a drawer member (m_dragSrcItem unused; m_dragSrcMember =
    //           member index inside cfg.items[m_dragSrcDrawer].members)
    size_t GroupAppendPos() const;            // cfg index after the last item of the current group
    bool MoveTopItemToInsert(size_t srcCfg, int insertVi);  // reorder within items
    void InsertItemToGrid(const Item& src, int insertVi);   // add an item at a grid drop point
    int MemberInsertIndexAt(POINT pt) const;  // like InsertIndexAt but over drawer members
    void DropInternalDrag(POINT pt);          // commit a finished internal drag

    int DrawerSplitY() const;                  // y of the split between the two halves
    int MemberOffset() const;                  // pixel shift applied to member cells
    RECT MemberCellRect(int k) const;          // member grid cell (lower half)

    void StartDrawerNameRename();              // m_editMode = 2
    void ShowMemberMenu(int memberIndex, POINT pt);
    void StartMemberRename();                  // m_editMode = 3
    void DeleteMember(int memberIndex);
    void OpenLocationMember(int memberIndex);

    // ---- Multi-group tab bar ----
    void SetCurrentGroup(int gi);
    int TabRectAt(POINT pt) const;
    void ShowTabMenu();
    void StartTabRename(int gi, bool isNew);   // m_editMode = 1
    void NewGroup();                           // 新建分组（N 键 / 菜单共用）
    void ConfirmDeleteGroup(int gi);           // 删除分组（带自定义确认框，D 键 / 菜单共用）
    void UpdateTabScroll();                    // 标题栏标签横向滚动跟随当前组

    // Creates the rename EDIT control at the given rect, subclassing it
    // and pre-filling it. Shared by every rename entry point.
    HWND CreateRenameEdit(RECT nr, const std::wstring& initial);

    static LRESULT CALLBACK SearchEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK RenameEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    friend class DropTarget;   // drag-drop queries drawer geometry/members
    App* m_app = nullptr;
    HINSTANCE m_hInst = nullptr;
    HWND m_hwnd = nullptr;
    DWORD m_createError = 0;
    HWND m_editSearch = nullptr;
    HWND m_editRename = nullptr;
    WNDPROC m_oldSearchProc = nullptr;
    WNDPROC m_oldRenameProc = nullptr;
    DropTarget* m_drop = nullptr;
    HFONT m_font = nullptr;
    HFONT m_font11 = nullptr;   // 11px font for the rename edit box
    bool  m_fontOwned = false;    // m_font 是否为自建字体（库存对象不可 DeleteObject）
    bool  m_font11Owned = false;  // m_font11 是否为自建字体（可能与 m_font 别名）

    int m_openDrawer = -1;     // index into cfg.items of the open drawer, -1 = none
    bool m_drawerDragging = false; // 抽屉分隔线拖动中
    RECT m_rcSearchBg = {0, 0, 0, 0};  // white background card behind the search box
    RECT m_rcDrawerName = {0, 0, 0, 0}; // drawer name hit/edit rect in the upper half

    // Rename state. m_editMode: 0 = icon name, 1 = group tab, 2 = drawer
    // name, 3 = drawer member name.
    int m_editMode = 0;
    int m_editGroup = -1;      // group being renamed / right-clicked
    int m_editMember = -1;     // drawer member being renamed

    int m_dpi = 96;
    JisuqUI::Metrics m_ui{};   // 当前 DPI 下的全部布局尺寸（来自 jisuq_ui_layout::GetMetrics）
    int m_hoverIndex = -1;     // 当前鼠标悬浮的 item（在 m_visible 中的下标），-1 表示无
    RECT m_rcClose = {0, 0, 0, 0};
    RECT m_rcSearch = {0, 0, 0, 0};
    RECT m_rcSearchEdit = {0, 0, 0, 0};  // 搜索 EDIT 的实际矩形（放大镜与文字对齐用）
    RECT m_rcTabAdd = {0, 0, 0, 0};      // 标题栏「+ 新建分组」按钮
    RECT m_rcGrid = {0, 0, 0, 0};
    int m_titleH = 30;
    int m_cellW = 80, m_cellH = 86, m_iconSize = 40, m_gridMargin = 14;
    int m_cols = 1;

    // Icon-grid scrolling (custom scrollbar; the window has no standard
    // scrollbar so we draw + handle one ourselves).
    int m_scrollY = 0;        // current scroll offset in pixels
    int m_maxScroll = 0;      // max scroll offset in pixels
    int m_scrollBarW = 8;     // scrollbar track width (DPI-scaled)
    bool m_dragScroll = false;
    int m_scrollDragOffset = 0;

    std::vector<size_t> m_visible;  // indices into Config::items
    int m_selected = -1;            // index into m_visible
    int m_renamingIndex = -1;       // index into m_visible, or -1
    int m_selectedMember = -1;      // selected drawer member, or -1
    int m_renamingMember = -1;      // drawer member whose name is hidden while renaming
    int m_downIndex = -1;
    POINT m_downPt = {0, 0};
    bool m_downValid = false;

    // Tab bar (multi-group).
    std::vector<RECT> m_tabRects;
    int m_tabScroll = 0;       // 标题栏标签横向滚动偏移（像素）
    int m_tabMaxScroll = 0;    // 标签内容超出可视区的最大可滚量
    RECT m_rcGear = {0, 0, 0, 0};

    // Drag-to-reorder / cross-group move.
    bool m_dragging = false;
    int m_dragFrom = -1;       // visible index of the dragged source (for dimming); -1
    POINT m_dragPt = {0, 0};
    int m_dragOverTab = -1;    // group tab hovered during drag, -1
    int m_dragSrcDrawer = -1;  // -1 => top-level item; >=0 => cfg index of source drawer
    size_t m_dragSrcItem = 0;  // top-level drag source: index into cfg.items
    size_t m_dragSrcMember = 0;// drawer-member drag source: member index in the drawer
    int m_dragOverDrawer = -1; // m_visible index of a hovered drawer cell, -1

    // 托盘左键事件去重（V4 下单击可能同时上报 NIN_SELECT 与 WM_LBUTTONUP，
    // 间隔极小；30ms 内重复事件去重，避免把一次单击误判成双击）。
    DWORD m_trayLastEventTick = 0;

    // ---- 面板图标 单击/双击启动门控（由设置项 launchMode 决定：0=单击启动, 1=双击启动）----
    bool m_iconDblPending = false; // 第一击已到、等第二击（双击模式）
    int  m_iconDblIndex   = -1;    // 待启动顶层 m_visible 索引
    int  m_iconDblMember  = -1;    // 待启动抽屉成员索引（-1=顶层）
    bool m_ignoreNextDbl  = false; // 刚单击开了抽屉，吞掉紧随的 WM_LBUTTONDBLCLK（避免误改名/误关）
    void ClearIconDblState();      // 复位上述状态并 KillTimer(TIMER_ICONDBL)
};

} // namespace JisuQi

#endif // BONNY_MAINWINDOW_H
