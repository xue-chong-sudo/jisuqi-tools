#define _WIN32_IE 0x0600
#include "TrayIcon.h"

namespace JisuQi {

bool TrayIcon::Create(HWND hWnd, HICON icon, const std::wstring& tip) {
    m_hWnd = hWnd;
    m_nid.cbSize = sizeof(m_nid);
    m_nid.hWnd = hWnd;
    m_nid.uID = 1;
    m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    m_nid.uCallbackMessage = WM_TRAYICON;
    m_nid.hIcon = icon;
    // _TRUNCATE: 超长提示自动截断并保证以 '\0' 结尾，永不触发
    // wcscpy_s 的非法参数处理器（旧写法的 size 守卫会把恰好 128 字符的
    // 提示整条丢弃，属于潜在 bug）。
    wcsncpy_s(m_nid.szTip, ARRAYSIZE(m_nid.szTip), tip.c_str(), _TRUNCATE);

    // 使用标准 NOTIFYICON_VERSION_4（现代 Windows 的默认/推荐版本）。
    // 该版本下左键单击以 NIN_SELECT 通知上报；MainWindow.cpp 的 WM_TRAYICON
    // 已统一处理 WM_LBUTTONUP / NIN_SELECT / NIN_KEYSELECT，并用一个定时器
    // 做双击判定，不依赖 WM_LBUTTONDBLCLK 是否送达。cbSize 用完整结构大小。
    m_nid.uVersion = NOTIFYICON_VERSION_4;
    if (!Shell_NotifyIconW(NIM_ADD, &m_nid)) return false;
    Shell_NotifyIconW(NIM_SETVERSION, &m_nid);
    return true;
}

void TrayIcon::Delete() {
    Shell_NotifyIconW(NIM_DELETE, &m_nid);
}

} // namespace JisuQi
