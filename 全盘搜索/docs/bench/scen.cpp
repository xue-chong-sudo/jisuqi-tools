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
static void M(const char* tag, size_t extra=0){
  PROCESS_MEMORY_COUNTERS_EX m{}; m.cb=sizeof m;
  GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&m,sizeof m);
  printf("%-46s WS=%6.1fMB  Private=%6.1fMB\n", tag, m.WorkingSetSize/1048576.0, m.PrivateUsage/1048576.0);
}
// faithful: avg name 38.6B, avg dirpath 141 wchar, dirs/file ratio from real idx
static void Build(Vol& v, size_t N, size_t Nd, size_t avgName, size_t avgDirW, bool reserve){
  if(reserve){ v.recs.reserve(N); v.nameBlob.reserve(N*avgName); v.lowerBlob.reserve(N*avgName); v.dirs.reserve(Nd); }
  std::string pad(avgName,'x');
  wchar_t dp[512]; for(size_t i=0;i<avgDirW && i<511;i++) dp[i]=(i%7==6)?L'\\':L'p'; dp[avgDirW<511?avgDirW:511]=0;
  v.dirs.push_back(L"C:");
  for(size_t i=0;i<N;i++){
    Rec r; r.nameOff=(uint32_t)v.nameBlob.size(); r.nameLen=(uint32_t)avgName;
    r.lowerOff=(uint32_t)v.lowerBlob.size(); r.lowerLen=(uint32_t)avgName;
    r.parent=(uint32_t)(i%Nd);
    v.nameBlob.append(pad); v.lowerBlob.append(pad); v.recs.push_back(r);
    if(i%(N/Nd+1)==0 && v.dirs.size()<Nd){
      std::wstring p(dp); p += std::to_wstring(i);  // heap alloc like real
      v.dirs.push_back(p);
    }
  }
}
int main(int argc,char**argv){
  const size_t N=419273, Nd=102970, avgName=39, avgDirW=141;
  printf("=== A. hidden state baseline ===\n"); M("start");
  printf("=== B. current: full C: rebuild (visible, base resident) ===\n");
  auto* base = new Vol(); Build(*base,N,Nd,avgName,avgDirW,false);
  M("base index resident (no reserve)");
  auto* v1 = new Vol(); Build(*v1,N,Nd,avgName,avgDirW,false);
  M("+ full C: rebuild in flight  [PEAK]");
  delete v1; v1=nullptr; M("after freeing rebuild buffer");
  delete base; base=nullptr; EmptyWorkingSet(GetCurrentProcess()); M("hidden again");
  printf("\n=== C. same full rebuild WITH reserve() ===\n");
  base = new Vol(); Build(*base,N,Nd,avgName,avgDirW,true);
  M("base index resident (reserve)");
  v1 = new Vol(); Build(*v1,N,Nd,avgName,avgDirW,true);
  M("+ full C: rebuild (reserve) [PEAK]");
  delete v1; delete base; base=nullptr; EmptyWorkingSet(GetCurrentProcess()); M("hidden again");

  printf("\n=== D. incremental: base resident + ONE subtree ===\n");
  struct S{const char* n; size_t files, ratio; size_t dirPerRec;};
  // subtree dir ratio: dirs/files from real data
  S sc[] = {
    {"C://Windows//servicing  (155,412 files)",155412,155412,L""[0]},
    {"C://Windows            (286,183 files)",286183,286183,0},
    {"C://Users//Administrator(104,192 files)",104192,104192,0},
    {"C://Windows//WinSxS      (88,558 files)",88558,88558,0},
    {"C://Program Files        (20,335 files)",20335,20335,0},
    {"typical AppData vendor    (2,000 files)",2000,2000,0},
  };
  for(auto& s : sc){
    size_t nd = (size_t)(s.files*0.2456);
    auto* b2 = new Vol(); Build(*b2,N,Nd,avgName,avgDirW,true);   // base resident
    size_t before=0; { PROCESS_MEMORY_COUNTERS_EX m{}; m.cb=sizeof m; GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&m,sizeof m); before=m.PrivateUsage; }
    auto* sub = new Vol(); Build(*sub,s.files,nd,avgName,avgDirW,false);
    PROCESS_MEMORY_COUNTERS_EX m2{}; m2.cb=sizeof m2; GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&m2,sizeof m2);
    printf("%-42s subtree Vol=%5.1fMB  PEAK private=%6.1fMB\n", s.n,
           (m2.PrivateUsage-before)/1048576.0, m2.PrivateUsage/1048576.0);
    delete sub; delete b2;}
  return 0;
}
