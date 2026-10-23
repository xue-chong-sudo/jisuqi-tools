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

private:
    static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);

    HHOOK m_hook;
    HWND m_hWnd;
};

} // namespace JisuQi

#endif // BONNY_MOUSEHOOK_H
