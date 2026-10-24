#define WIN32_LEAN_AND_MEAN
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <deque>
#include <chrono>
#include <windows.h>
static double ms(){ LARGE_INTEGER f,c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart*1000.0/f.QuadPart; }
struct Cnt{ uint64_t files=0,dirs=0; };
static Cnt Walk(const std::wstring& root){
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
      if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY){ c.dirs++; q.push_back(cur+nm+L"\\"); }
    }while(FindNextFileW(h,&fd));
    FindClose(h);
  }
  return c;
}
int main(){
  setvbuf(stdout,NULL,_IONBF,0);
  const wchar_t* roots[]={L"C://Windows//servicing//", L"C://Users//Administrator//AppData//Roaming//"};
  const char* tags[]={"Windows\\servicing (155k)","AppData\\Roaming (44k)"};
  for(int k=0;k<2;k++){
    printf("=== %s  连续 6 次遍历 ===\n", tags[k]);
    Cnt c{};
    double prev=0;
    for(int i=1;i<=6;i++){
      double a=ms(); c=Walk(roots[k]); double b=ms();
      double t=b-a;
      printf("  run%d: %7.0f ms   (%.0f files/s)%s%s\n", i, t, c.files/(t/1000.0),
             (i>1 && prev>0) ? "   较上次 " : "", (i>1&&prev>0)? "" : "");
      if(i>1 && prev>0) printf("         ^ 与 run1 比 %+.1f%%\n", (t/prev-1)*100.0);
      if(i==1) prev=t;
    }
    printf("  文件数=%llu dirs=%llu\n\n",(unsigned long long)c.files,(unsigned long long)c.dirs);
  }
  return 0;
}
