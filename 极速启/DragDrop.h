#ifndef BONNY_DRAGDROP_H
#define BONNY_DRAGDROP_H

#include <windows.h>
#include <objidl.h>

namespace JisuQi {

class App;

// IDropTarget implementation: accepts files/folders/.lnk dropped onto
// the main window, forwards each path to App::AddPathAt().
class DropTarget : public IDropTarget {
public:
    explicit DropTarget(App* app) : m_app(app), m_ref(1) {}
    virtual ~DropTarget() {}

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IDropTarget
    STDMETHODIMP DragEnter(IDataObject* pdo, DWORD grfKeyState,
                           POINTL pt, DWORD* pdwEffect) override;
    STDMETHODIMP DragOver(DWORD grfKeyState, POINTL pt,
                          DWORD* pdwEffect) override;
    STDMETHODIMP DragLeave() override;
    STDMETHODIMP Drop(IDataObject* pdo, DWORD grfKeyState, POINTL pt,
                      DWORD* pdwEffect) override;

private:
    static FORMATETC FileFormat();

    App* m_app;
    ULONG m_ref;
    bool m_hasFiles = false;
};

} // namespace JisuQi

#endif // BONNY_DRAGDROP_H
