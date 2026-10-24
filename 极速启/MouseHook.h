#ifndef BONNY_MOUSEHOOK_H
#define BONNY_MOUSEHOOK_H

#include <windows.h>

namespace JisuQi {

// Installs a low-level mouse hook that watches for the configured summon
// button (middle / XBUTTON1 / XBUTTON2, or none) and posts WM_APP_TOGGLE to
// the target window (which toggles the launcher). The configured button's
// down/up events are swallowed so other applications never receive them;
// all other mouse events are passed through untouched.
class MouseHook {
public:
    MouseHook() : m_hook(nullptr), m_hWnd(nullptr) {}
    ~MouseHook() { Uninstall(); }

    // hWnd = the main window that should receive WM_APP_TOGGLE.
    bool Install(HWND hWnd);
    void Uninstall();

    // 最近一次收到的「呼出键」是否仍处于按下状态（由 down/up 事件自维护）。
    // 不能依赖 GetAsyncKeyState：它对 XBUTTON1/2 在部分鼠标/驱动上不可靠，
    // 会导致长按判定"没按住"→长按呼出失效。
    bool IsHeld() const { return m_held; }

private:
    static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);

    HHOOK m_hook;
    HWND m_hWnd;
    bool  m_held = false;
};

} // namespace JisuQi

#endif // BONNY_MOUSEHOOK_H
