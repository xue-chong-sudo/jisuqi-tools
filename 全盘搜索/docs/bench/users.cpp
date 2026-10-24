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
  struct T{const wchar_t*p;const char*t;}ts[]={
    {L"C://Users//Administrator//","Users\\Administrator (现状整片)"},
    {L"C://Windows//","Windows (剪枝前的全走)"},
    {L"C://","C: 整卷"},
  };
  for(auto&t:ts){
    double best=1e18; Cnt c;
    for(int r=0;r<2;r++){ double a=ms(); c=Walk(t.p); double b=ms(); if(b-a<best)best=b-a; }
    printf("%-34s files=%8llu dirs=%7llu  单遍=%7.0f ms  (%.0f files/s)\n",t.t,
      (unsigned long long)c.files,(unsigned long long)c.dirs,best,c.files/(best/1000.0));
  }
  return 0;
}
