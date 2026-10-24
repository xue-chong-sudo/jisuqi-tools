#define WIN32_LEAN_AND_MEAN
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>
#include <chrono>
#include <windows.h>
static double ms(){ LARGE_INTEGER f,c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart*1000.0/f.QuadPart; }
#pragma pack(push,1)
struct Rec { uint32_t nameOff,nameLen,lowerOff,lowerLen,parent; uint64_t size,mtime; uint32_t attrs; uint8_t cat; };
struct IdxHead { char magic[8]; uint32_t ver; uint64_t bt; uint32_t letter; uint64_t nRecs,nDirs; };
#pragma pack(pop)
static_assert(sizeof(IdxHead)==40, "header must be 40 bytes packed");
static_assert(sizeof(Rec)==41 && alignof(Rec)==1, "");
template<class F> static double Best(F f){ f(); double b=1e18;
  for(int r=0;r<9;r++){ double t0=ms(); f(); double t1=ms(); if(t1-t0<b)b=t1-t0; } return b; }
int main(){
  setvbuf(stdout,NULL,_IONBF,0);
  const char* KW="e";
  HANDLE h=CreateFileW(L"snapC_dfs.idx",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  HANDLE hm=CreateFileMappingW(h,nullptr,PAGE_READONLY,0,0,nullptr);
  const uint8_t* b=(const uint8_t*)MapViewOfFile(hm,FILE_MAP_READ,0,0,0);
  IdxHead hd; memcpy(&hd,b,sizeof hd);
  printf("ver=%u nRecs=%llu nDirs=%llu\n",hd.ver,(unsigned long long)hd.nRecs,(unsigned long long)hd.nDirs);
  size_t off=sizeof hd;
  for(uint64_t i=0;i<hd.nDirs;i++){ uint16_t l; memcpy(&l,b+off,2); off+=2+l; }
  const uint8_t* recs=b+off; off += hd.nRecs*41;
  uint64_t nl; memcpy(&nl,b+off,8); off+=8; off+=nl;
  uint64_t ll; memcpy(&ll,b+off,8); off+=8; const char* lowerB=(const char*)(b+off);
  printf("nRecs=%llu  lowerBlob=%.1fMiB\n",(unsigned long long)hd.nRecs, ll/1048576.0);

  // A) memcpy 逐字段（C++20 合规，无 object-lifetime 依赖）
  auto byMemcpy=[&]{ uint64_t n=0;
    for(uint64_t i=0;i<hd.nRecs;i++){ const uint8_t* r=recs+i*41; uint32_t lo,l2,par;
      memcpy(&lo,r+8,4); memcpy(&l2,r+12,4); memcpy(&par,r+16,4);
      if(std::string_view(lowerB+lo,l2).find(KW)!=std::string_view::npos) n+=(par&1); } return n; };
  // B) 强转 Rec* 直接成员访问（依赖 object lifetime；MSVC 实际能跑，但不是合规保证）
  auto byTyped=[&]{ uint64_t n=0; const Rec* R=(const Rec*)recs;
    for(uint64_t i=0;i<hd.nRecs;i++){ const Rec& r=R[i];
      if(std::string_view(lowerB+r.lowerOff,r.lowerLen).find(KW)!=std::string_view::npos) n+=(r.parent&1); } return n; };
  double a=Best(byMemcpy), c=Best(byTyped);
  printf("A) memcpy 逐字段 (C++20 合规) : %6.2f ms\n", a);
  printf("B) Rec* 成员访问 (需 C++23 SLAA): %6.2f ms   -> 差 %+.2f ms (%+.1f%%)\n", c, c-a, (c-a)/a*100);
  printf("\n结论: 合规路径的代价 = %.1f%%（噪声级）\n", (a-c)/c*100);
  return 0;
}
