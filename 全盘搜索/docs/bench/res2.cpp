#define WIN32_LEAN_AND_MEAN
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>
#include <psapi.h>
#pragma comment(lib,"psapi.lib")
// 完全复刻 search_app.cpp 的 Res 与结果集构造
struct Res { std::wstring name, dir, sizeStr, timeStr; uint64_t size=0,mtime=0; uint8_t cat=0; };
static size_t Priv(){ PROCESS_MEMORY_COUNTERS_EX m{}; m.cb=sizeof m; GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&m,sizeof m); return m.PrivateUsage; }
static void Run(const char* tag, size_t dirW, size_t nameChars, int N){
  size_t p0=Priv();
  auto* v=new std::vector<Res>(); v->reserve(N);
  std::wstring d(dirW,L'a'); for(size_t i=0;i<dirW;i+=9) d[i]=L'\\';
  for(int i=0;i<N;i++){
    Res r;
    r.name.assign(nameChars,L'n'); r.name += std::to_wstring(i%1000);      // name
    r.dir = d;                                                              // it.dir = dd 全路径拷贝
    r.sizeStr = L"12.3 MB";                                                 // SSO
    r.timeStr = L"2026-10-24 23:07";                                        // 16 chars -> heap
    v->push_back(std::move(r));
  }
  size_t p1=Priv();
  printf("  %-46s %6.1f MB  (%.0f B/条)\n", tag,(p1-p0)/1048576.0,(double)(p1-p0)/N);
  delete v;
}
int main(){
  printf("sizeof(Res)=%zu\n", sizeof(Res));
  const int N=100000;
  Run("现状：dir 拷完整绝对路径(141 wchar), name 30 字符",141,30,N);
  Run("若 dir 改 dirId 不拷字符串（模拟：dir 为空）",0,30,N);
  Run("若 MAX_STORED 降到 20000（按 650B 折算）",141,30,20000);
  return 0;
}
