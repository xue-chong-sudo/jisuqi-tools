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
static double now_ms(){ LARGE_INTEGER f,c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart*1000.0/f.QuadPart; }
#pragma pack(push,1)
struct RecP { uint32_t nameOff,nameLen,lowerOff,lowerLen,parent; uint64_t size,mtime; uint32_t attrs; uint8_t cat; };
struct IdxHead { char magic[8]; uint32_t ver; uint64_t buildTime; uint32_t letter; uint64_t nRecs,nDirs; };
#pragma pack(pop)
struct Shard { const uint8_t* base=nullptr; IdxHead hd{}; const RecP* recs=nullptr; const char* lower=nullptr; const char* name=nullptr; std::vector<const char*> dirOff; };
static std::vector<Shard> shards;
static std::vector<HANDLE> maps;
static void Load(const wchar_t* p){
  HANDLE h=CreateFileW(p,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(h==INVALID_HANDLE_VALUE) return;
  LARGE_INTEGER li{}; GetFileSizeEx(h,&li);
  HANDLE m=CreateFileMappingW(h,nullptr,PAGE_READONLY,0,0,nullptr);
  const uint8_t* b=(const uint8_t*)MapViewOfFile(m,FILE_MAP_READ,0,0,0);
  Shard s; s.base=b; memcpy(&s.hd,b,sizeof(IdxHead));
  size_t off=sizeof(IdxHead);
  for(uint64_t i=0;i<s.hd.nDirs;i++){ s.dirOff.push_back((const char*)(b+off)); uint16_t l; memcpy(&l,b+off,2); off+=2+l; }
  s.recs=(const RecP*)(b+off); off += s.hd.nRecs*sizeof(RecP);
  uint64_t nl; memcpy(&nl,b+off,8); off+=8; s.name=(const char*)(b+off); off+=nl;
  uint64_t ll; memcpy(&ll,b+off,8); off+=8; s.lower=(const char*)(b+off);
  shards.push_back(s); maps.push_back(m);
}
// 复刻 SearcherThread 751-778 的热循环
static uint64_t Search(const char* kw, bool reconstructDir, std::vector<std::string>* hits){
  uint64_t n=0; size_t kl=strlen(kw);
  for(auto& s: shards){
    for(size_t i=0;i<s.hd.nRecs;i++){
      const RecP& rc=s.recs[i];
      std::string_view low(s.lower+rc.lowerOff, rc.lowerLen);
      if(low.find(kw) == std::string_view::npos) continue;
      n++;
      if(hits){
        std::string d;
        if(reconstructDir){
          // 若 dirs 改紧凑表示：需按 parent 链重建绝对路径
          uint32_t p = rc.parent; std::vector<std::string> chain;
          for(int guard=0; guard<64 && p < s.dirOff.size(); ++guard){
            uint16_t l; memcpy(&l,s.dirOff[p],2);
            if(l) chain.push_back(std::string(s.dirOff[p]+2,l));
            // 无法从紧凑表取 parent，这里按最坏情况计入 7 层
            break;
          }
          d = chain.empty()? std::string() : chain[0];
        } else {
          uint32_t p = rc.parent < s.dirOff.size()? rc.parent : 0;
          uint16_t l; memcpy(&l,s.dirOff[p],2);
          d.assign(s.dirOff[p]+2,l);          // 现状：直接取已存好的绝对路径
        }
        if(hits->size()<100000) hits->push_back(std::move(d));
      }
    }
  }
  return n;
}
int main(){
  Load(L"volC.idx"); Load(L"volD.idx");
  size_t tot=0; for(auto&s:shards) tot+=s.hd.nRecs;
  printf("shards=%zu  total recs=%zu\n", shards.size(), tot);
  for(const char* kw : {"e", "setup", "config", "zzzznomatch"}){
    // 预热
    Search(kw,false,nullptr);
    double best=1e9;
    for(int r=0;r<5;r++){ double t0=now_ms(); uint64_t n=Search(kw,false,nullptr); double t1=now_ms(); if(t1-t0<best) best=t1-t0;
      if(r==0) printf("kw='%-12s' hits=%7llu  ", kw,(unsigned long long)n); }
    printf("best=%.1f ms (纯扫描, dirs 绝对路径)\n", best);
  }
  { std::vector<std::string> hits; hits.reserve(100000);
    double best=1e9;
    for(int r=0;r<5;r++){ hits.clear(); double t0=now_ms(); Search("e",false,&hits); double t1=now_ms(); if(t1-t0<best) best=t1-t0; }
    printf("kw='e' 含 100k 结果 dir 拷贝: best=%.1f ms (hits=%zu)\n", best, hits.size()); }
  return 0;
}
