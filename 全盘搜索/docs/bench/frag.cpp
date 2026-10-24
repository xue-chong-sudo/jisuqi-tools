#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <windows.h>
#include <psapi.h>
#pragma comment(lib,"psapi.lib")
struct Rec { uint32_t nameOff=0,nameLen=0,lowerOff=0,lowerLen=0,parent=0; uint64_t size=0,mtime=0; uint32_t attrs=0; uint8_t cat=0; };
struct Vol { wchar_t letter=0; std::vector<Rec> recs; std::string nameBlob, lowerBlob; std::vector<std::wstring> dirs; };
static void Mem(const char* tag){
  PROCESS_MEMORY_COUNTERS_EX m{}; m.cb=sizeof m;
  GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&m,sizeof m);
  printf("%-34s WS=%6.1fMB Private=%6.1fMB\n", tag, m.WorkingSetSize/1048576.0, m.PrivateUsage/1048576.0);
}
// simulate WalkVolume for N files, D dirs -- exactly the allocation pattern of search_app.cpp
static void Build(Vol& v, size_t N, size_t Nd, bool reserve){
  if(reserve){ v.recs.reserve(N); v.nameBlob.reserve(N*39); v.lowerBlob.reserve(N*39); v.dirs.reserve(Nd); }
  v.dirs.push_back(L"C:");
  wchar_t nm[80];
  for(size_t i=0;i<N;i++){
    // mimic: std::wstring wname(nm); W8(wname); W8(LowerW(wname)); -> 3 temp allocs/iteration
    swprintf(nm,80,L"file_%08zu_name_with_some_padding.dat", i);
    std::wstring wname(nm);
    std::string u8(80,'a'), u8l(80,'b');
    Rec r; r.nameOff=(uint32_t)v.nameBlob.size(); r.nameLen=(uint32_t)u8.size();
    r.lowerOff=(uint32_t)v.lowerBlob.size(); r.lowerLen=(uint32_t)u8l.size();
    r.parent = (uint32_t)(i % Nd);
    v.nameBlob.append(u8); v.lowerBlob.append(u8l); v.recs.push_back(r);
    if(i % (N/Nd+1) == 0 && v.dirs.size() < Nd){
      // full absolute path, like the real code: cur.path + nm
      std::wstring p = L"C://Users//Administrator//AppData//Local//SomeVendor//SomeProduct//Deep//Nested//" + wname + L"\\sub";
      v.dirs.push_back(p);
    }
  }
}
int main(){
  Mem("baseline (start)");
  const size_t N=419273, Nd=102970;
  for(int round=1; round<=5; round++){
    auto* v = new Vol();
    Build(*v, N, Nd, false);           // current code: no reserve
    char b[64]; sprintf(b,"round %d: built (no reserve)", round); Mem(b);
    delete v;
    sprintf(b,"round %d: after delete", round); Mem(b);
    EmptyWorkingSet(GetCurrentProcess());
    sprintf(b,"round %d: after EmptyWorkingSet", round); Mem(b);
  }
  printf("---- now with reserve() ----\n");
  for(int round=1; round<=3; round++){
    auto* v = new Vol();
    Build(*v, N, Nd, true);
    char b[64]; sprintf(b,"round %d: built (reserve)", round); Mem(b);
    delete v; EmptyWorkingSet(GetCurrentProcess());
    sprintf(b,"round %d: after delete+EWS", round); Mem(b);
  }
  return 0;
}
