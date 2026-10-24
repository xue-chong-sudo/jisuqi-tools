#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>
#include <psapi.h>
#pragma comment(lib,"psapi.lib")
struct Res { std::wstring name, dir, sizeStr, timeStr; uint64_t size=0,mtime=0; uint8_t cat=0; };
static size_t P(){ PROCESS_MEMORY_COUNTERS_EX m{}; m.cb=sizeof m; GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&m,sizeof m); return m.PrivateUsage; }
int main(){
  printf("sizeof(Res)=%zu\n", sizeof(Res));
  size_t p0=P();
  auto* v=new std::vector<Res>(); v->reserve(100000);
  wchar_t d[400]; for(int i=0;i<390;i++) d[i]=(i%9==8)?L'\\':L'a'; d[390]=0;
  for(int i=0;i<100000;i++){
    Res r;
    r.name = L"some_file_name_number_" + std::to_wstring(i) + L".docx";
    r.dir  = d;                              // full path copy, exactly like it.dir = dd
    r.sizeStr = L"12.3 MB"; r.timeStr = L"2026-10-24 23:07";
    r.cat=2; v->push_back(std::move(r));
  }
  size_t p1=P();
  printf("100,000 results: Private +%.1fMB (%.0f B/result)\n",(p1-p0)/1048576.0,(double)(p1-p0)/100000);
  delete v; return 0;
}
