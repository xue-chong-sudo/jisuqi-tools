#include <cstdio>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>
#include <windows.h>
#include <psapi.h>
#pragma comment(lib,"psapi.lib")
static void M(const char* tag){
  PROCESS_MEMORY_COUNTERS_EX m{}; m.cb=sizeof m;
  GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&m,sizeof m);
  printf("%-44s WS=%7.1fMB  Private=%7.1fMB\n", tag, m.WorkingSetSize/1048576.0, m.PrivateUsage/1048576.0);
}
#pragma pack(push,1)
struct RecP { uint32_t nameOff,nameLen,lowerOff,lowerLen,parent; uint64_t size,mtime; uint32_t attrs; uint8_t cat; };
struct IdxHead { char magic[8]; uint32_t ver; uint64_t buildTime; uint32_t letter; uint64_t nRecs,nDirs; };
#pragma pack(pop)
struct Mapped { HANDLE h=nullptr,hmap=nullptr; const uint8_t* p=nullptr; size_t n=0; };
static Mapped Map(const wchar_t* path){
  Mapped r;
  r.h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if(r.h==INVALID_HANDLE_VALUE){ r.h=nullptr; return r; }
  LARGE_INTEGER li{}; GetFileSizeEx(r.h,&li); r.n=(size_t)li.QuadPart;
  r.hmap = CreateFileMappingW(r.h,nullptr,PAGE_READONLY,0,0,nullptr);
  if(!r.hmap){ return r; }
  r.p = (const uint8_t*)MapViewOfFile(r.hmap, FILE_MAP_READ, 0,0,0);
  return r;
}
int main(){
  printf("=== mmap of the REAL index files (read-only, PAGE_READONLY) ===\n");
  M("baseline");
  const wchar_t* files[]={L"volC.idx",L"volD.idx"};
  std::vector<Mapped> maps;
  size_t total=0;
  for(auto* f: files){ Mapped m=Map(f); if(m.p){ maps.push_back(m); total+=m.n; } }
  M("after mapping all 4 (no touch)");
  printf("total mapped bytes = %.2f MB\n", total/1048576.0);

  // warm the pages: linear scan matching SearcherThread's access pattern
  auto t0=std::chrono::steady_clock::now();
  uint64_t hits=0;
  for(auto& m: maps){
    const uint8_t* q=m.p;
    IdxHead hd; memcpy(&hd,q,sizeof hd);
    size_t off=sizeof(IdxHead);
    // skip dirs
    for(uint64_t i=0;i<hd.nDirs;i++){ uint16_t l; memcpy(&l,q+off,2); off+=2+l; }
    const RecP* rp=(const RecP*)(q+off);
    for(uint64_t i=0;i<hd.nRecs;i++){
      // touch every record + one byte of the name blob (like a substring match)
      const char* nm=(const char*)(q+ off + hd.nRecs*sizeof(RecP) + 16 + rp[i].nameOff);
      if(rp[i].nameLen && (nm[0]=='a')) hits++;
    }
  }
  auto t1=std::chrono::steady_clock::now();
  double ms=std::chrono::duration<double,std::milli>(t1-t0).count();
  printf("cold-ish full scan of %.1fMB: %.1f ms (hits=%llu)\n", total/1048576.0, ms,(unsigned long long)hits);
  M("after full scan (pages faulted in)");

  // time it again (warm)
  auto t2=std::chrono::steady_clock::now();
  for(auto& m: maps){ volatile uint8_t x=m.p[0]; volatile uint8_t y=m.p[m.n-1]; (void)x;(void)y; }
  auto t3=std::chrono::steady_clock::now();
  printf("warm re-touch: %.2f ms\n", std::chrono::duration<double,std::milli>(t3-t2).count());
  for(auto& m: maps) if(m.p) UnmapViewOfFile(m.p);
  EmptyWorkingSet(GetCurrentProcess());
  M("after unmap + EmptyWorkingSet");
  return 0;
}
