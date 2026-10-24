#define WIN32_LEAN_AND_MEAN
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <deque>
#include <io.h>
#include <chrono>
#include <windows.h>
#include <psapi.h>
#pragma comment(lib,"psapi.lib")
static double ms(){ LARGE_INTEGER f,c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart*1000.0/f.QuadPart; }
static size_t Priv(){ PROCESS_MEMORY_COUNTERS_EX m{}; m.cb=sizeof m; GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&m,sizeof m); return m.PrivateUsage; }
struct Cnt { uint64_t files=0, dirs=0, nameBytes=0, pathChars=0; };
// pass1：只计数，不分配
static Cnt Pass1(const std::wstring& root){
  Cnt c; std::deque<std::wstring> q; q.push_back(root);
  while(!q.empty()){
    std::wstring cur=q.front(); q.pop_front();
    WIN32_FIND_DATAW fd{};
    HANDLE h=FindFirstFileExW((cur+L"*").c_str(),FindExInfoBasic,&fd,FindExSearchNameMatch,nullptr,FIND_FIRST_EX_LARGE_FETCH);
    if(h==INVALID_HANDLE_VALUE) continue;
    do{
      if(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
      const wchar_t* nm=fd.cFileName;
      if(nm[0]==L'.'){ if(!wcscmp(nm,L".")||!wcscmp(nm,L"..")) continue; }
      c.files++;
      size_t L=wcslen(nm);
      c.nameBytes += L*2;          // 粗略：UTF-8 长度 ≈ wchar 数（ASCII 主导）
      if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY){
        c.dirs++; c.pathChars += cur.size()+L+1;
        q.push_back(cur+nm+L"\\");
      }
    }while(FindNextFileW(h,&fd));
    FindClose(h);
  }
  return c;
}
// pass2：按精确容量一次性 reserve 后 materialize（记录 + 两 blob + dirs）
static size_t Pass2(const std::wstring& root, const Cnt& c){
  struct R { uint32_t a,b,cc,dd,e; uint64_t f,g; uint32_t h; uint8_t i; };
  std::vector<R> recs; recs.reserve((size_t)c.files);
  std::string nb, lb, nbuf; nb.reserve((size_t)c.nameBytes); lb.reserve((size_t)c.nameBytes);
  std::vector<std::wstring> dirs; dirs.reserve((size_t)c.dirs+1);
  dirs.push_back(root);
  std::deque<std::pair<std::wstring,uint32_t>> q; q.push_back({root,0});
  char buf[2048];
  while(!q.empty()){
    auto cur=q.front(); q.pop_front();
    WIN32_FIND_DATAW fd{};
    HANDLE h=FindFirstFileExW((cur.first+L"*").c_str(),FindExInfoBasic,&fd,FindExSearchNameMatch,nullptr,FIND_FIRST_EX_LARGE_FETCH);
    if(h==INVALID_HANDLE_VALUE) continue;
    do{
      if(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
      const wchar_t* nm=fd.cFileName;
      if(nm[0]==L'.'){ if(!wcscmp(nm,L".")||!wcscmp(nm,L"..")) continue; }
      int n=WideCharToMultiByte(CP_UTF8,0,nm,-1,buf,sizeof buf,nullptr,nullptr);
      if(n>0){ n--; R r{}; r.a=(uint32_t)nb.size(); r.b=(uint32_t)n; r.cc=(uint32_t)lb.size(); r.dd=(uint32_t)n; r.e=cur.second;
               nb.append(buf,(size_t)n); lb.append(buf,(size_t)n); recs.push_back(r); }
      if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY){
        uint32_t id=(uint32_t)dirs.size();
        dirs.push_back(cur.first+nm);
        q.push_back({cur.first+nm+L"\\", id});
      }
    }while(FindNextFileW(h,&fd));
    FindClose(h);
  }
  size_t total = recs.capacity()*sizeof(R) + nb.capacity()+lb.capacity();
  for(auto&d:dirs) total += 32 + d.size()*2;
  return total;
}
int main(){
  struct T { const wchar_t* p; } ts[] = {
    {L"C://Windows//servicing//"}, {L"C://Windows//WinSxS//"}, {L"C://Windows//System32//"}, {L"C://Users//Administrator//AppData//"}
  };
  for(auto& t:ts){
    printf("=== %ls ===\n", t.p);
    size_t p0=Priv();
    double a=ms(); Cnt c=Pass1(t.p); double b=ms();
    printf("  pass1 只计数 : %7.0f ms   files=%llu dirs=%llu\n", b-a,(unsigned long long)c.files,(unsigned long long)c.dirs);
    double c0=ms(); size_t heap=Pass2(t.p,c); double c1=ms();
    printf("  pass2 物化   : %7.0f ms   堆占用 %.1f MB (reserve 精确容量)\n", c1-c0, heap/1048576.0);
    printf("  两遍合计 : %.0f ms\n", (b-a)+(c1-c0));
  }
  // mmap 写吞吐
  printf("\n=== mmap 顺序写吞吐（24MB）===\n");
  const size_t SZ=24u<<20;
  HANDLE h=CreateFileW(L"out.tmp",GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
  HANDLE hm=CreateFileMappingW(h,nullptr,PAGE_READWRITE,0,(DWORD)SZ,nullptr);
  char* v=(char*)MapViewOfFile(hm,FILE_MAP_WRITE,0,0,SZ);
  double t0=ms(); memset(v,0xAB,SZ); double t1=ms();
  printf("  memset 到 mmap : %6.1f ms  => %.0f MB/s\n", t1-t0, SZ/1048576.0/((t1-t0)/1000.0));
  UnmapViewOfFile(v); CloseHandle(hm); CloseHandle(h); DeleteFileW(L"out.tmp");
  t0=ms();
  FILE* f=nullptr; _wfopen_s(&f,L"out2.tmp",L"wb");
  std::vector<char> tmp(SZ,0xCD); fwrite(tmp.data(),1,SZ,f); fflush(f); _commit(_fileno(f)); fclose(f);
  t1=ms();
  printf("  fwrite+commit : %6.1f ms  => %.0f MB/s\n", t1-t0, SZ/1048576.0/((t1-t0)/1000.0));
  DeleteFileW(L"out2.tmp");
  return 0;
}
