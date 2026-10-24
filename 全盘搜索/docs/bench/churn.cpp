#define WIN32_LEAN_AND_MEAN
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <deque>
#include <vector>
#include <io.h>
#include <chrono>
#include <windows.h>
static double ms(){ LARGE_INTEGER f,c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart*1000.0/f.QuadPart; }
struct Cnt{ uint64_t files=0,dirs=0,utf8=0; };
static Cnt Walk(const std::wstring& root){
  Cnt c; std::deque<std::wstring> q; q.push_back(root.back()==L'\\'?root:root+L"\\"); char buf[2048];
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
      if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY){ c.dirs++; q.push_back(cur+nm+L"\\"); }
    }while(FindNextFileW(h,&fd));
    FindClose(h);
  }
  return c;
}
int main(){
  setvbuf(stdout,NULL,_IONBF,0);
  struct T{ const wchar_t* p; const char* tag; } ts[]={
    {L"C://Users//Administrator//AppData//Roaming","AppData\\Roaming"},
    {L"C://Users//Administrator//AppData//Local//Temp","AppData\\Local\\Temp"},
    {L"C://Users//Administrator//Desktop","Desktop"},
    {L"C://Users//Administrator//Documents","Documents"},
    {L"C://Users//Administrator//Downloads","Downloads"},
    {L"C://Users//Administrator//AppData//Local","AppData\\Local"},
    {L"C://Users//Administrator//AppData","AppData(whole)"},
  };
  printf("%-24s %9s %8s %9s %10s %12s %12s\n","路径","files","dirs","单遍(ms)","files/s","流式构建MB","两遍(ms)");
  for(auto& t:ts){
    double best=1e18; Cnt c;
    for(int r=0;r<3;r++){ double a=ms(); c=Walk(t.p); double b=ms(); if(b-a<best)best=b-a; }
    double rate=c.files/(best/1000.0);
    double mb=c.files*250.0/1048576.0;           // 堆内构建口径（仅作参照）
    printf("%-24s %9llu %8llu %9.0f %10.0f %12.1f %12.0f\n",t.tag,
      (unsigned long long)c.files,(unsigned long long)c.dirs,best,rate,mb,best*2);
  }
  return 0;
}
