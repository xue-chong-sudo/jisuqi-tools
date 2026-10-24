#define WIN32_LEAN_AND_MEAN
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>
#include <windows.h>
#include <psapi.h>
#pragma comment(lib,"psapi.lib")
static double ms(){ LARGE_INTEGER f,c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart*1000.0/f.QuadPart; }
static void Mem(const char* tag){
  PROCESS_MEMORY_COUNTERS_EX m{}; m.cb=sizeof m;
  GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&m,sizeof m);
  PROCESS_MEMORY_COUNTERS_EX2 m2{}; m2.cb=sizeof m2;
  GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&m2,sizeof m2);
  printf("  %-34s WS=%6.1fMB PrivateWS=%6.1fMB Commit=%6.1fMB\n",tag,
    m.WorkingSetSize/1048576.0, m2.PrivateWorkingSetSize/1048576.0, m.PrivateUsage/1048576.0);
}
// 模拟"冷"：先 EmptyWorkingSet 把页挤出 WS，再测首访
static double TouchAll(const uint8_t* b, size_t n){
  volatile uint64_t s=0; double t0=ms();
  for(size_t i=0;i<n;i+=64) s+=b[i];
  double t1=ms(); return t1-t0;
}
int main(){
  const wchar_t* f=L"volC.idx";
  HANDLE h=CreateFileW(f,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  HANDLE hm=CreateFileMappingW(h,nullptr,PAGE_READONLY,0,0,nullptr);
  LARGE_INTEGER li{}; GetFileSizeEx(h,&li); size_t n=(size_t)li.QuadPart;
  printf("=== PrefetchVirtualMemory 效果（file %.1f MB）===\n", n/1048576.0);
  printf("[A] 无 prefetch：EmptyWorkingSet 后首访\n");
  for(int r=0;r<3;r++){
    const uint8_t* b=(const uint8_t*)MapViewOfFile(hm,FILE_MAP_READ,0,0,0);
    TouchAll(b,n);                       // 预热到 standby
    UnmapViewOfFile(b); EmptyWorkingSet(GetCurrentProcess());
    b=(const uint8_t*)MapViewOfFile(hm,FILE_MAP_READ,0,0,0);
    double t=TouchAll(b,n);
    printf("   round %d 首访耗时 = %7.2f ms\n", r+1, t);
    UnmapViewOfFile(b);
  }
  printf("[B] 带 prefetch：PrefetchVirtualMemory 后首访\n");
  for(int r=0;r<3;r++){
    const uint8_t* b=(const uint8_t*)MapViewOfFile(hm,FILE_MAP_READ,0,0,0);
    TouchAll(b,n);
    UnmapViewOfFile(b); EmptyWorkingSet(GetCurrentProcess());
    Mem("    prefetch 前");
    b=(const uint8_t*)MapViewOfFile(hm,FILE_MAP_READ,0,0,0);
    WIN32_MEMORY_RANGE_ENTRY e{}; e.VirtualAddress=(PVOID)b; e.NumberOfBytes=n;
    double p0=ms();
    BOOL ok=PrefetchVirtualMemory(GetCurrentProcess(),1,&e,0);
    double p1=ms();
    Mem("    prefetch 后");
    printf("   round %d PrefetchVirtualMemory=%s 耗时 %6.2f ms\n", r+1, ok?"OK":"FAIL", p1-p0);
    double t=TouchAll(b,n);
    printf("   round %d 首访耗时 = %7.2f ms\n", r+1, t);
    UnmapViewOfFile(b);
  }
  CloseHandle(hm); CloseHandle(h);
  return 0;
}
