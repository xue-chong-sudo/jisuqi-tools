#define WIN32_LEAN_AND_MEAN
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <windows.h>
#pragma pack(push,1)
struct IdxHead { char magic[8]; uint32_t ver; uint64_t buildTime; uint32_t letter; uint64_t nRecs,nDirs; };
struct Rec     { uint32_t nameOff,nameLen,lowerOff,lowerLen,parent; uint64_t size,mtime; uint32_t attrs; uint8_t cat; };
#pragma pack(pop)
// 防线1：编译期
static_assert(sizeof(IdxHead)==40, "IdxHead must be 40B");
static_assert(sizeof(Rec)==41 && alignof(Rec)==1, "Rec must be 41B packed");

// 有界游标校验器（防线2）
struct Cur { const uint8_t* b; size_t n, off; bool ok=true; const char* why=""; };
static bool Take(Cur& c, size_t k, const void** out){
  if(!c.ok) return false;
  if(c.off + k > c.n){ c.ok=false; c.why="越界"; return false; }
  if(out) *out = c.b + c.off;
  c.off += k; return true;
}
static bool Validate(const wchar_t* path, uint32_t recSizeIfV2){
  HANDLE h=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(h==INVALID_HANDLE_VALUE){ printf("  open fail\n"); return false; }
  LARGE_INTEGER li{}; GetFileSizeEx(h,&li); size_t fsz=(size_t)li.QuadPart;
  HANDLE hm=CreateFileMappingW(h,nullptr,PAGE_READONLY,0,0,nullptr);
  const uint8_t* b=(const uint8_t*)MapViewOfFile(hm,FILE_MAP_READ,0,0,0);
  Cur c{b,fsz,0};
  IdxHead hd{}; const void* p=nullptr;
  if(!Take(c,sizeof hd,&p)){ printf("  头越界\n"); return false; }
  memcpy(&hd,p,sizeof hd);
  if(memcmp(hd.magic,"WBSVOL1\0",8)!=0){ printf("  magic 错\n"); return false; }
  uint32_t rs = (hd.ver==1)?41:recSizeIfV2;
  // 先做长度下界检查，再信任 nRecs/nDirs
  size_t minNeed = sizeof hd + (size_t)hd.nDirs*2 + (size_t)hd.nRecs*rs;
  if(minNeed > fsz){ printf("  下界检查失败: 需要>=%zu, 文件=%zu  -> REJECT\n",minNeed,fsz); return false; }
  for(uint64_t i=0;i<hd.nDirs;i++){ const void* q; if(!Take(c,2,&q)){ printf("  dirs 越界@%llu\n",(unsigned long long)i); return false; }
    uint16_t l; memcpy(&l,q,2); if(!Take(c,l,nullptr)){ printf("  dir 串越界\n"); return false; } }
  if(!Take(c,(size_t)hd.nRecs*rs,nullptr)){ printf("  recs 越界 -> REJECT\n"); return false; }
  uint64_t nl; if(!Take(c,8,&p)){ printf("  nl 越界\n"); return false; } memcpy(&nl,p,8);
  if(!Take(c,(size_t)nl,nullptr)){ printf("  nameBlob 越界 -> REJECT\n"); return false; }
  uint64_t ll; if(!Take(c,8,&p)){ printf("  ll 越界\n"); return false; } memcpy(&ll,p,8);
  if(!Take(c,(size_t)ll,nullptr)){ printf("  lowerBlob 越界 -> REJECT\n"); return false; }
  // ★ 防线2：游标必须精确等于文件长度
  printf("  ver=%u recSize=%u nRecs=%llu nDirs=%llu | 游标=%zu 文件=%zu  %s\n",
     hd.ver,rs,(unsigned long long)hd.nRecs,(unsigned long long)hd.nDirs, c.off, fsz,
     c.off==fsz ? "OK" : "!! 游标 != 文件长度 -> REJECT");
  return c.off==fsz;
}
int main(){
  setvbuf(stdout,NULL,_IONBF,0);
  printf("防线1 (static_assert) 已通过编译，但它只检查【声明】，检查不到【文件内容】\n\n");
  struct T{const wchar_t*p;uint32_t v2;const char*t;}ts[]={
    {L"snapC.idx",     48,"好的 ver=1 文件"},
    {L"snapC_dfs.idx", 48,"好的 ver=1(DFS) 文件"},
    {L"snapC48.idx",   48,"**坏的 ver=2 文件**（我生成时写成 44B stride，头却声明 ver=2=48B）"},
  };
  for(auto&t:ts){
    printf("[%ls] %s\n", t.p, t.t);
    bool ok=Validate(t.p,t.v2);
    printf("  => %s\n\n", ok?"接受":"拒绝（防线2 捕获）");
  }
  return 0;
}
