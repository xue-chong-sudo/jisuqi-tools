#include <windows.h>
#include <dwmapi.h>
#include <cstdio>
#pragma comment(lib,"user32.lib")
#pragma comment(lib,"dwmapi.lib")
int main(){
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int vx=GetSystemMetrics(SM_XVIRTUALSCREEN), vy=GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw=GetSystemMetrics(SM_CXVIRTUALSCREEN), vh=GetSystemMetrics(SM_CYVIRTUALSCREEN);
    printf("virtual screen = (%d,%d)-(%d,%d) %dx%d\n", vx,vy,vx+vw,vy+vh,vw,vh);
    printf("--- Z 序（GetTopWindow -> GW_HWNDNEXT）---\n");
    int i=0;
    for(HWND h=GetTopWindow(NULL); h && i<12; h=GetWindow(h,GW_HWNDNEXT),++i){
        if(!IsWindowVisible(h)) { printf("  %2d <invisible>\n", i); continue; }
        int cl=0; DwmGetWindowAttribute(h,DWMWA_CLOAKED,&cl,sizeof(cl));
        char cls[128]={0},ttl[160]={0};
        GetClassNameA(h,cls,128); GetWindowTextA(h,ttl,160);
        RECT r{}; GetWindowRect(h,&r);
        DWORD pid=0; GetWindowThreadProcessId(h,&pid);
        long long a=(long long)(r.right-r.left)*(r.bottom-r.top);
        double cov = 100.0*a/((long long)vw*vh);
        printf("  %2d z=%-2d cls=%-24s cloaked=%d pid=%-6lu rect=(%5d,%5d)-(%5d,%5d)  覆盖虚拟屏 %5.1f%%\n",
               i,i,cls,cl,pid,r.left,r.top,r.right,r.bottom,cov);
    }
    // 关键问题：光标在屏幕中央时，Z 序首命中的是谁？
    POINT p{vx+vw/2, vy+vh/2};
    printf("\n--- 虚拟屏中心点 (%d,%d) 的命中链 ---\n", p.x,p.y);
    int hit=0;
    for(HWND h=GetTopWindow(NULL); h; h=GetWindow(h,GW_HWNDNEXT)){
        if(!IsWindowVisible(h)) continue;
        int cl=0; DwmGetWindowAttribute(h,DWMWA_CLOAKED,&cl,sizeof(cl));
        if(cl) continue;
        RECT r{}; if(!GetWindowRect(h,&r)) continue;
        if(PtInRect(&r,p)){
            char cls[128]={0}; GetClassNameA(h,cls,128);
            DWORD pid=0; GetWindowThreadProcessId(h,&pid);
            printf("  命中#%d (Z序首个) cls=%-24s pid=%lu rect=(%d,%d)-(%d,%d)\n",
                   ++hit,cls,pid,r.left,r.top,r.right,r.bottom);
            if(hit>=3) break;
        }
    }
    return 0;
}
