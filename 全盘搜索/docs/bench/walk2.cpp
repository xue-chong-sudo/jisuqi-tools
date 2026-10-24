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
struct Cnt { uint64_t files=0, dirs=0, utf8=0, pathW=0; };
static Cnt Pass1(const std::wstring& root){
  Cnt c; std::deque<std::wstring> q; q.push_back(root); char buf[2048];
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
      c.utf8 += (uint64_t)WideCharToMultiByte(CP_UTF8,0,nm,-1,buf,sizeof buf,nullptr,nullptr)-1;
      if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY){ c.dirs++; c.pathW += cur.size()+wcslen(nm)+1; q.push_back(cur+nm+L"\\"); }
    }while(FindNextFileW(h,&fd));
    FindClose(h);
  }
  return c;
}
// 精确 reserve 版本的 pass2（构建器只写堆，模拟"两遍法"的峰值）
static void Pass2(const std::wstring& root, const Cnt& c, bool verbose){
  struct R { uint32_t a,b,cc,dd,e; uint64_t f,g; uint32_t h; uint8_t i; uint8_t pad[3]; };
  static_assert(sizeof(R)==48,"");
  size_t p0=Priv();
  std::vector<R> recs; recs.reserve((size_t)c.files);
  std::string nb, lb; nb.reserve((size_t)c.utf8); lb.reserve((size_t)c.utf8);
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
        uint32_t id=(uint32_t)dirs.size(); dirs.push_back(cur.first+nm); q.push_back({cur.first+nm+L"\\",id});
      }
    }while(FindNextFileW(h,&fd));
    FindClose(h);
  }
  size_t p1=Priv();
  if(verbose){
    size_t dirsHeap=0;
    printf("      recs %zu×48=%.1fMB  blobs %.1f+%.1f=%.1fMB  dirs %zu 条\n",
      recs.size(), recs.capacity()*48/1048576.0, nb.size()/1048576.0, lb.size()/1048576.0,
      (nb.size()+lb.size())/1048576.0, dirs.size());
    printf("      ** pass2 实测 private 峰值增量 = %.1f MB (%.0f B/file) **\n", (p1-p0)/1048576.0, (double)(p1-p0)/recs.size());
  }
}
int main(){
  const wchar_t* ts[]={L"C://Windows//servicing//",L"C://Windows//WinSxS//",L"C://Windows//System32//",L"C://Users//Administrator//AppData//"};
  for(auto* t:ts){
    printf("=== %ls ===\n", t);
    double a=ms(); Cnt c=Pass1(t); double b=ms();
    printf("  pass1 只计数(含UTF-8测量): %6.0f ms  files=%llu dirs=%llu utf8=%.1fMB\n", b-a,
      (unsigned long long)c.files,(unsigned long long)c.dirs,c.utf8/1048576.0);
    double c0=ms(); Pass2(t,c,true); double c1=ms();
    printf("  pass2 物化          : %6.0f ms\n  两遍合计 = %.1f s  (单遍 %.1f s)\n",
      c1-c0, ((b-a)+(c1-c0))/1000.0, (b-a)/1000.0);
  }
  return 0;
}
