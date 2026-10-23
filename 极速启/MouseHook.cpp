#include "MouseHook.h"
#include "resource.h"
#include "settings_ui.h"

namespace JisuQi {

// Single global instance so the static hook proc can reach the object.
static MouseHook* g_instance = nullptr;

// 当前设置选中的呼出鼠标键对应的虚拟键（长按超时后由主窗口用它确认按键仍被按住）。
// mouseBtn: 0=中键, 1=向后侧键(XBUTTON1), 2=向前侧键(XBUTTON2), 3=不使用鼠标呼出
static UINT MouseBtnVK() {
    switch (GetSettings().mouseBtn) {
        case 1:  return VK_XBUTTON1;
        case 2:  return VK_XBUTTON2;
        default: return VK_MBUTTON;
    }
}

// 该低层鼠标事件是否为"所选呼出键按下"（mouseBtn==3 表示不占用任何键）
static bool IsButtonDown(WPARAM msg, const MSLLHOOKSTRUCT* d) {
    int b = GetSettings().mouseBtn;
    if (b < 0 || b > 2) return false;
    if (b == 0) return msg == WM_MBUTTONDOWN;
    if (msg != WM_XBUTTONDOWN) return false;
    WORD x = HIWORD(d->mouseData);          // LL 钩子里侧键编号在 mouseData 高字
    return (b == 1) ? (x == XBUTTON1) : (x == XBUTTON2);
}

// 该低层鼠标事件是否为"所选呼出键抬起"
static bool IsButtonUp(WPARAM msg, const MSLLHOOKSTRUCT* d) {
    int b = GetSettings().mouseBtn;
    if (b < 0 || b > 2) return false;
    if (b == 0) return msg == WM_MBUTTONUP;
    if (msg != WM_XBUTTONUP) return false;
    WORD x = HIWORD(d->mouseData);
    return (b == 1) ? (x == XBUTTON1) : (x == XBUTTON2);
}

bool MouseHook::Install(HWND hWnd) {
    m_hWnd = hWnd;
    g_instance = this;
    m_hook = SetWindowsHookExW(WH_MOUSE_LL, &MouseHook::LowLevelMouseProc,
                               GetModuleHandleW(nullptr), 0);
    return m_hook != nullptr;
}

void MouseHook::Uninstall() {
    if (m_hook) {
        UnhookWindowsHookEx(m_hook);
        m_hook = nullptr;
    }
    if (g_instance == this) g_instance = nullptr;
}

// 长按计时用主窗口的共享定时器 ID（resource.h 的 TIMER_MOUSEHOLD = 4），
// 由 MainWindow.cpp 的 WM_TIMER 统一处理；此处不再重复定义裸字面量。

LRESULT CALLBACK MouseHook::LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && g_instance && g_instance->m_hWnd) {
        const MSLLHOOKSTRUCT* d = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        int hold = GetSettings().holdMs;    // 每次事件都读最新设置，保存后立即生效
        if (IsButtonDown(wParam, d)) {
            if (hold <= 0) {
                // 按下即触发（默认）。低层钩子是全局的，即使别的软件占用了
                // RegisterHotKey 呼出键，这里照样能呼出。
                PostMessageW(g_instance->m_hWnd, WM_APP_TOGGLE, 0, 0);
            } else {
                // 长按模式：按下启动计时；超时前收到抬起 → 取消；
                // 超时仍按住 → 主窗口 WM_TIMER(id=4) 里确认按键状态后触发。
                SetTimer(g_instance->m_hWnd, TIMER_MOUSEHOLD, (UINT)hold, nullptr);
            }
            // 占用的鼠标键被本程序独占：吞掉按下事件，其他应用收不到。
            return 1;
        }
        if (IsButtonUp(wParam, d)) {
            if (hold > 0) KillTimer(g_instance->m_hWnd, TIMER_MOUSEHOLD);
            // 抬起同样吞掉，避免其他应用收到"凭空"的抬起。
            return 1;
        }
    }
    // 其余事件（未占用的键 / 移动 / 滚轮）一律放行。
    return CallNextHookEx(g_instance ? g_instance->m_hook : nullptr,
                          nCode, wParam, lParam);
}

} // namespace JisuQi
