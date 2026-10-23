#include "DragDrop.h"
#include "App.h"

namespace JisuQi {

STDMETHODIMP DropTarget::QueryInterface(REFIID riid, void** ppv) {
    if (riid == IID_IUnknown || riid == IID_IDropTarget) {
        *ppv = static_cast<IDropTarget*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) DropTarget::AddRef() {
    return ++m_ref;
}

STDMETHODIMP_(ULONG) DropTarget::Release() {
    ULONG r = --m_ref;
    if (r == 0) delete this;
    return r;
}

// CF_HDROP payload is carried in an HGLOBAL STGMEDIUM (tymed = TYMED_HGLOBAL).
// NOTE: there is no "TYMED_HDROP" in the TYMED enum; using a made-up value
// made GetData fail silently, which is why drops previously added nothing.
FORMATETC DropTarget::FileFormat() {
    FORMATETC fe = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    return fe;
}

STDMETHODIMP DropTarget::DragEnter(IDataObject* pdo, DWORD /*ks*/,
                                   POINTL /*pt*/, DWORD* pdwEffect) {
    // Show the copy cursor only when the payload actually contains files.
    m_hasFiles = false;
    if (pdo) {
        FORMATETC fe = FileFormat();
        m_hasFiles = (pdo->QueryGetData(&fe) == S_OK);
    }
    *pdwEffect = m_hasFiles ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return S_OK;
}

STDMETHODIMP DropTarget::DragOver(DWORD /*ks*/, POINTL /*pt*/,
                                  DWORD* pdwEffect) {
    // DragOver receives no IDataObject; reuse the flag from DragEnter.
    *pdwEffect = m_hasFiles ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return S_OK;
}

STDMETHODIMP DropTarget::DragLeave() {
    m_hasFiles = false;
    return S_OK;
}

STDMETHODIMP DropTarget::Drop(IDataObject* pdo, DWORD /*ks*/, POINTL pt,
                              DWORD* pdwEffect) {
    *pdwEffect = m_hasFiles ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    m_hasFiles = false;
    if (!pdo) return S_OK;

    FORMATETC fe = FileFormat();
    STGMEDIUM med = {0};
    if (SUCCEEDED(pdo->GetData(&fe, &med)) && med.tymed == TYMED_HGLOBAL &&
        med.hGlobal) {
        HDROP hdrop = reinterpret_cast<HDROP>(med.hGlobal);
        UINT count = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
        const UINT kBuf = MAX_PATH * 2;
        // Decide the target: an open drawer, a closed drawer under the
        // cursor, or the top-level grid.
        POINT cp = {static_cast<LONG>(pt.x), static_cast<LONG>(pt.y)};
        ScreenToClient(m_app->GetMainWindow().GetHwnd(), &cp);
        MainWindow& mw = m_app->GetMainWindow();
        int drawerIdx = -1;
        if (mw.IsDrawerOpen())
            drawerIdx = mw.OpenDrawerCfgIndex();
        else {
            int di = mw.DrawerCellAt(cp);
            if (di >= 0) drawerIdx = static_cast<int>(mw.VisibleToCfg(di));
        }
        for (UINT i = 0; i < count; ++i) {
            wchar_t buf[kBuf] = {0};
            if (DragQueryFileW(hdrop, i, buf, kBuf) > 0 && buf[0]) {
                if (drawerIdx >= 0) {
                    // Drop onto a (closed or open) drawer -> into the drawer.
                    m_app->AddToDrawer(static_cast<size_t>(drawerIdx), buf);
                } else {
                    // Top-level grid: insert at the dropped location instead
                    // of always appending (Bug E). Map the visible index to a
                    // Config::items index and insert before it.
                    int ins = mw.InsertIndexAt(cp);
                    size_t cfgIns = m_app->GetConfig().items.size();
                    if (ins >= 0 && ins < static_cast<int>(mw.m_visible.size()))
                        cfgIns = mw.VisibleToCfg(ins);
                    m_app->AddPathAt(buf, cfgIns);
                }
            }
        }
        ReleaseStgMedium(&med);
    }
    return S_OK;
}

} // namespace JisuQi
