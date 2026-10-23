#ifndef BONNY_TRAYICON_H
#define BONNY_TRAYICON_H

#include <windows.h>
#include <string>
#include "resource.h"

namespace JisuQi {

// Thin wrapper over NOTIFYICONDATA. Shows the tray icon, handles the
// NIM_SETVERSION negotiation, and pops a simple context menu.
class TrayIcon {
public:
    TrayIcon() { ZeroMemory(&m_nid, sizeof(m_nid)); }

    // hWnd receives WM_TRAYICON callbacks; icon shown in the tray.
    bool Create(HWND hWnd, HICON icon, const std::wstring& tip);

    // Remove the icon (call before exit).
    void Delete();

private:
    NOTIFYICONDATAW m_nid;
    HWND m_hWnd = nullptr;
};

} // namespace JisuQi

#endif // BONNY_TRAYICON_H
