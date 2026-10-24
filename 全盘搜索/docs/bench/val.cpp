#define WIN32_LEAN_AND_MEAN
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <windows.h>
#pragma pack(push,1)
struct IdxHead { char magic[8]; uint32_t ver; uint64_t buildTime; uint32_t letter; uint64_t nRecs,nDirs; };
#pragma pack(pop)
int main(int argc,char**argv){
  setvbuf(stdout,NULL,_IONBF,0);
  const wchar_t* p = (argc>1)? L"snapC48.idx" : L"snapC_dfs.idx";
  HANDLE h=CreateFileW(p,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(h==INVALID_HANDLE_VALUE){ printf("open fail\n"); return 1; }
  LARGE_INTEGER li{}; GetFileSizeEx(h,&li); size_t fsz=(size_t)li.QuadPart;
  HANDLE hm=CreateFileMappingW(h,nullptr,PAGE_READONLY,0,0,nullptr);
  const uint8_t* b=(const uint8_t*)MapViewOfFile(hm,FILE_MAP_READ,0,0,0);
  printf("file=%ls size=%zu\n",p,fsz);
  IdxHead hd; memcpy(&hd,b,sizeof hd);
  uint32_t rs = (hd.ver==1)?41:48;
  printf("ver=%u recSize=%u nRecs=%llu nDirs=%llu\n",hd.ver,rs,
    (unsigned long long)hd.nRecs,(unsigned long long)hd.nDirs);
  size_t off=sizeof(IdxHead);
  for(uint64_t i=0;i<hd.nDirs;i++){ uint16_t l; memcpy(&l,b+off,2); off+=2+l; }
  size_t dEnd=off;
  const uint8_t* recs=b+off; off += hd.nRecs*rs;
  uint64_t nl; memcpy(&nl,b+off,8); off+=8; const char* nm=(const char*)(b+off); off+=nl;
  uint64_t ll; memcpy(&ll,b+off,8); off+=8; const char* lb=(const char*)(b+off); off+=ll;
  printf("dirsEnd=%zu recsEnd=%zu nl=%llu ll=%llu 计算末端=%zu 文件=%zu %s\n",
    dEnd,(size_t)(dEnd+hd.nRecs*rs),(unsigned long long)nl,(unsigned long long)ll,off,fsz, off==fsz?"OK":"MISMATCH");
  size_t bad=0, maxEnd=0;
  for(uint64_t i=0;i<hd.nRecs;i++){
    const uint8_t* r=recs+i*rs;
    uint32_t lo,llen,par;
    memcpy(&lo,r+8,4); memcpy(&llen,r+12,4); memcpy(&par,r+16,4);
    size_t e=(size_t)lo+llen;
    if(e>maxEnd) maxEnd=e;
    if(e>ll || par>=hd.nDirs){ if(bad<5) printf("  BAD i=%llu lo=%u len=%u par=%u (ll=%llu nDirs=%llu)\n",
      (unsigned long long)i,lo,llen,par,(unsigned long long)ll,(unsigned long long)hd.nDirs); bad++; }
    if(i%100000==0) printf("  ...%llu\n",(unsigned long long)i);
  }
  printf("越界记录=%zu  max(lo+len)=%zu (blob=%llu)\n",bad,maxEnd,(unsigned long long)ll);
  return 0;
}
