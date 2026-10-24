#define WIN32_LEAN_AND_MEAN
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <chrono>
#include <windows.h>
#include <psapi.h>
#pragma comment(lib,"psapi.lib")
static double ms(){ LARGE_INTEGER f,c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart*1000.0/f.QuadPart; }
static void Mem(const char* tag){
  PROCESS_MEMORY_COUNTERS_EX m{}; m.cb=sizeof m;
  GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&m,sizeof m);
  // PrivateWorkingSetSize 需要 PROCESS_MEMORY_COUNTERS_EX2 (Win8.1+)
  PROCESS_MEMORY_COUNTERS_EX2 m2{}; m2.cb=sizeof m2;
  BOOL ok2 = GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&m2,sizeof m2);
  printf("%-40s WS=%7.1fMB  PrivateWS=%7.1fMB  Commit=%7.1fMB\n",
    tag,
    m.WorkingSetSize/1048576.0,
    ok2? m2.PrivateWorkingSetSize/1048576.0 : -1.0,
    m.PrivateUsage/1048576.0);
}
int main(){
  printf("=== 口径对照：TotalWS / PrivateWorkingSet / Commit ===\n");
  Mem("baseline");
  HANDLE h=CreateFileW(L"volC.idx",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  HANDLE hm=CreateFileMappingW(h,nullptr,PAGE_READONLY,0,0,nullptr);
  const uint8_t* b=(const uint8_t*)MapViewOfFile(hm,FILE_MAP_READ,0,0,0);
  LARGE_INTEGER li{}; GetFileSizeEx(h,&li);
  Mem("after MapView (untouched)");
  // 全量触碰
  volatile uint64_t sum=0;
  for(size_t i=0;i<(size_t)li.QuadPart;i+=64) sum += b[i];
  Mem("after full touch (64.3MB mapped)");
  printf("  ^ mapped file pages 计入 TotalWS(%s)，PrivateWS 几乎不涨\n", "可裁剪");
  // PrefetchVirtualMemory
  UnmapViewOfFile(b); CloseHandle(hm); CloseHandle(h);
  EmptyWorkingSet(GetCurrentProcess());
  Mem("after unmap + EmptyWorkingSet");
  return 0;
}
