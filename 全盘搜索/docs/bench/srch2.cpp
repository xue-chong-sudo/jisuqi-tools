#define WIN32_LEAN_AND_MEAN
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <chrono>
#include <windows.h>
#include <psapi.h>
#pragma comment(lib,"psapi.lib")
static double ms(){ LARGE_INTEGER f,c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart*1000.0/f.QuadPart; }
#pragma pack(push,1)
struct RecP { uint32_t nameOff,nameLen,lowerOff,lowerLen,parent; uint64_t size,mtime; uint32_t attrs; uint8_t cat; };
struct IdxHead { char magic[8]; uint32_t ver; uint64_t buildTime; uint32_t letter; uint64_t nRecs,nDirs; };
#pragma pack(pop)
struct Rec48 { uint32_t nameOff,nameLen,lowerOff,lowerLen,parent; uint64_t size,mtime; uint32_t attrs; uint8_t cat; uint8_t pad[7]; };
struct Shard { const uint8_t* b; IdxHead hd; const RecP* recs; const char* name; const char* lower; std::vector<uint16_t> dirLen; std::vector<const char*> dirPtr; };
static std::vector<Shard> sh;
static void Load(const wchar_t* p){
  HANDLE h=CreateFileW(p,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(h==INVALID_HANDLE_VALUE){ printf("open fail\n"); return; }
  HANDLE hm=CreateFileMappingW(h,nullptr,PAGE_READONLY,0,0,nullptr);
  const uint8_t* b=(const uint8_t*)MapViewOfFile(hm,FILE_MAP_READ,0,0,0);
  Shard s{}; s.b=b; memcpy(&s.hd,b,sizeof(IdxHead));
  size_t off=sizeof(IdxHead);
  for(uint64_t i=0;i<s.hd.nDirs;i++){ uint16_t l; memcpy(&l,b+off,2); s.dirLen.push_back(l); s.dirPtr.push_back((const char*)(b+off+2)); off+=2+l; }
  s.recs=(const RecP*)(b+off); off+=s.hd.nRecs*sizeof(RecP);
  uint64_t nl; memcpy(&nl,b+off,8); off+=8; s.name=(const char*)(b+off); off+=nl;
  uint64_t ll; memcpy(&ll,b+off,8); off+=8; s.lower=(const char*)(b+off);
  sh.push_back(s);
}
int main(){
  Load(L"volC.idx"); Load(L"volD.idx");
  size_t tot=0; for(auto&s:sh) tot+=s.hd.nRecs;
  printf("shards=%zu recs=%zu\n",sh.size(),tot);
  const char* kw="e";
  // ---------- 0) 基线：无 scope 过滤 ----------
  auto scan0=[&](int variant, uint32_t scopeId)->uint64_t{
    uint64_t n=0;
    for(auto&s:sh){
      const char* lb=s.lower;
      for(size_t i=0;i<s.hd.nRecs;i++){
        const RecP& rc=s.recs[i];
        std::string_view low(lb+rc.lowerOff, rc.lowerLen);
        if(low.find(kw)==std::string_view::npos) continue;
        if(variant==1){ if(!(rc.parent==scopeId)) continue; }              // 整数比较（DFS+subtreeEnd 代理）
        else if(variant==2){                                                // 朴素 mmap：每条记录 UTF-8→wstring 再比较
          std::wstring d(s.dirLen[rc.parent],0);
          if(rc.parent<s.dirLen.size() && s.dirLen[rc.parent])
            MultiByteToWideChar(CP_UTF8,0,s.dirPtr[rc.parent],s.dirLen[rc.parent],d.data(),(int)d.size());
          if(d!=L"c://users//administrator//appdata") continue;
        }
        n++;
      }
    }
    return n;
  };
  // 预转 wstring dirs（一次性 103k 次转换）后逐条比较
  std::vector<std::vector<std::wstring>> wdirs(sh.size());
  for(size_t k=0;k<sh.size();k++){
    wdirs[k].resize(sh[k].hd.nDirs);
    for(uint32_t i=0;i<sh[k].hd.nDirs;i++){
      if(!sh[k].dirLen[i]) continue;
      wdirs[k][i].resize(sh[k].dirLen[i]);
      MultiByteToWideChar(CP_UTF8,0,sh[k].dirPtr[i],sh[k].dirLen[i],wdirs[k][i].data(),(int)wdirs[k][i].size());
    }
  }
  auto scan2=[&]()->uint64_t{
    uint64_t n=0; std::wstring scope=L"c://users//administrator//appdata";
    for(size_t k=0;k<sh.size();k++){
      for(size_t i=0;i<sh[k].hd.nRecs;i++){
        const RecP& rc=sh[k].recs[i];
        std::string_view low(sh[k].lower+rc.lowerOff, rc.lowerLen);
        if(low.find(kw)==std::string_view::npos) continue;
        if(wdirs[k][rc.parent]!=scope) continue;
        n++;
      }
    }
    return n;
  };
  auto bench=[&](const char* nm, auto fn)->double{
    fn(); double best=1e9;
    for(int r=0;r<7;r++){ double t0=ms(); fn(); double t1=ms(); if(t1-t0<best)best=t1-t0; }
    printf("  %-46s %6.2f ms\n", nm, best); return best;
  };
  printf("\n=== scope 过滤开销（关键字 'e'）===\n");
  double b0=bench("无 scope 过滤（基线）", [&]{ return scan0(0,0); });
  double b1=bench("整数比较（DFS preorder + subtreeEnd）", [&]{ return scan0(1,0); });
  double b2=bench("朴素 mmap：每条记录 UTF-8→wstring", [&]{ return scan0(2,0); });
  double b3=bench("预转 103k wstring dirs + 逐条比较", [&]{ return scan2(); });
  printf("  -> 整数比较增量 %.2f ms | 朴素mmap增量 %.2f ms | 预转增量 %.2f ms\n", b1-b0, b2-b0, b3-b0);

  // ---------- 41B packed vs 48B aligned ----------
  printf("\n=== 41B packed vs 48B 自然对齐（同样 553k 条，遍历全部字段）===\n");
  size_t nR=tot;
  std::vector<RecP> p41(nR); std::vector<Rec48> p48(nR);
  { size_t k=0; for(auto&s:sh) for(size_t i=0;i<s.hd.nRecs;i++,k++){ p41[k]=s.recs[i]; p48[k]=Rec48{}; memcpy(&p48[k],&s.recs[i],41); } }
  volatile uint64_t acc=0;
  auto run41=[&]{ uint64_t a=0; for(size_t i=0;i<nR;i++){ a+=p41[i].lowerOff+p41[i].lowerLen+p41[i].parent+p41[i].cat; } acc=a; };
  auto run48=[&]{ uint64_t a=0; for(size_t i=0;i<nR;i++){ a+=p48[i].lowerOff+p48[i].lowerLen+p48[i].parent+p48[i].cat; } acc=a; };
  double t41=1e9,t48=1e9;
  for(int r=0;r<9;r++){ double a=ms(); run41(); double b=ms(); if(b-a<t41)t41=b-a;
                        double c=ms(); run48(); double d=ms(); if(d-c<t48)t48=d-c; }
  printf("  41B packed 全字段遍历: %6.2f ms\n  48B aligned 全字段遍历: %6.2f ms   (差 %.2f ms, %+.1f%%)\n",
         t41,t48,t48-t41,(t48-t41)/t41*100);
  return 0;
}
