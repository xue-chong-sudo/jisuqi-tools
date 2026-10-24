#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>
#include <chrono>
#include <windows.h>
static double now_ms(){ LARGE_INTEGER f,c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart*1000.0/f.QuadPart; }
int main(){
  const int N=200;
  std::vector<std::wstring> paths;
  for(int i=0;i<N;i++){ wchar_t b[64]; swprintf(b,64,L"shards/s%03d.idx",i); paths.push_back(b); }
  // A: CreateFile + ReadFile 全量进堆（现状 LoadVol 的做法）
  {
    double t0=now_ms(); size_t total=0;
    for(int round=0;round<3;round++)
    for(auto& p:paths){
      HANDLE h=CreateFileW(p.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                           nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
      if(h==INVALID_HANDLE_VALUE) continue;
      LARGE_INTEGER li{}; GetFileSizeEx(h,&li);
      std::string buf; buf.resize((size_t)li.QuadPart);
      DWORD got=0; ReadFile(h,buf.data(),(DWORD)buf.size(),&got,nullptr); total+=got;
      CloseHandle(h);
    }
    double t1=now_ms();
    printf("A. CreateFile+ReadFile  %d 片 x3 轮: %7.1f ms  (单轮 %6.1f ms)  read %.1fMB\n",
           N,t1-t0,(t1-t0)/3,total/3/1048576.0);
  }
  // B: CreateFile + CreateFileMapping + MapViewOfFile（mmap，不读内容）
  {
    double t0=now_ms(); size_t total=0;
    for(int round=0;round<3;round++)
    for(auto& p:paths){
      HANDLE h=CreateFileW(p.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                           nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
      if(h==INVALID_HANDLE_VALUE) continue;
      HANDLE m=CreateFileMappingW(h,nullptr,PAGE_READONLY,0,0,nullptr);
      if(m){ void* v=MapViewOfFile(m,FILE_MAP_READ,0,0,0);
             if(v){ total+=(size_t)1; VOLATILE_READ:; volatile char c=((char*)v)[0]; (void)c; UnmapViewOfFile(v);} 
             CloseHandle(m); }
      CloseHandle(h);
    }
    double t1=now_ms();
    printf("B. CreateFile+CreateFileMapping+MapView  %d 片 x3: %7.1f ms  (单轮 %6.1f ms)\n",N,t1-t0,(t1-t0)/3);
  }
  // C: 只打开不读
  {
    double t0=now_ms();
    for(int round=0;round<3;round++)
    for(auto& p:paths){
      HANDLE h=CreateFileW(p.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                           nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
      if(h!=INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    double t1=now_ms();
    printf("C. CreateFile+Close only              %d 片 x3: %7.1f ms  (单轮 %6.1f ms)\n",N,t1-t0,(t1-t0)/3);
  }
  return 0;
}
