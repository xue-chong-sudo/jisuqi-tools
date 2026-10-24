#define WIN32_LEAN_AND_MEAN
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <chrono>
#include <windows.h>
static double ms(){ LARGE_INTEGER f,c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart*1000.0/f.QuadPart; }
#pragma pack(push,1)
struct IdxHead { char magic[8]; uint32_t ver; uint64_t buildTime; uint32_t letter; uint64_t nRecs,nDirs; };
#pragma pack(pop)
template<class F> static double Best(F f,int reps=9){ f(); double b=1e18;
  for(int r=0;r<reps;r++){ double t0=ms(); f(); double t1=ms(); if(t1-t0<b)b=t1-t0; } return b; }
static const char* KW="e";

int main(){
  setvbuf(stdout,NULL,_IONBF,0);
  HANDLE h=CreateFileW(L"snapC_dfs.idx",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  HANDLE hm=CreateFileMappingW(h,nullptr,PAGE_READONLY,0,0,nullptr);
  const uint8_t* b=(const uint8_t*)MapViewOfFile(hm,FILE_MAP_READ,0,0,0);
  IdxHead hd; memcpy(&hd,b,sizeof hd);
  size_t off=sizeof(IdxHead);
  std::vector<uint16_t> dirLen; std::vector<const char*> dirPtr;
  for(uint64_t i=0;i<hd.nDirs;i++){ uint16_t l; memcpy(&l,b+off,2); dirLen.push_back(l); dirPtr.push_back((const char*)(b+off+2)); off+=2+l; }
  const uint8_t* r41=b+off; off += hd.nRecs*41;
  uint64_t nl; memcpy(&nl,b+off,8); off+=8; const char* nameB=(const char*)(b+off); off+=nl;
  uint64_t ll; memcpy(&ll,b+off,8); off+=8; const char* lowerB=(const char*)(b+off);
  printf("41B: nRecs=%llu nDirs=%llu  lowerBlob=%.1fMB\n",(unsigned long long)hd.nRecs,(unsigned long long)hd.nDirs,ll/1048576.0);

  // 堆上 41B / 48B 对照（同一份数据，仅记录步长不同）
  std::vector<uint8_t> h41((size_t)hd.nRecs*41), h48((size_t)hd.nRecs*48);
  memcpy(h41.data(), r41, h41.size());
  for(uint64_t i=0;i<hd.nRecs;i++){ memcpy(&h48[i*48], &r41[i*41], 41); memset(&h48[i*48+41],0,3); }

  auto hot=[&](const uint8_t* base,size_t stride)->uint64_t{
    uint64_t n=0;
    for(uint64_t i=0;i<hd.nRecs;i++){
      const uint8_t* r=base+i*stride; uint32_t lo,llen,par;
      memcpy(&lo,r+8,4); memcpy(&llen,r+12,4); memcpy(&par,r+16,4);
      if(std::string_view(lowerB+lo,llen).find(KW)!=std::string_view::npos) n+=(par&1)+1;
    } return n; };
  auto all=[&](const uint8_t* base,size_t stride)->uint64_t{
    uint64_t n=0;
    for(uint64_t i=0;i<hd.nRecs;i++){
      const uint8_t* r=base+i*stride; uint32_t lo,llen,par;
      memcpy(&lo,r+8,4); memcpy(&llen,r+12,4); memcpy(&par,r+16,4);
      uint64_t sz2,mt; memcpy(&sz2,r+20,8); memcpy(&mt,r+28,8);   // 41B 布局下的偏移
      if(std::string_view(lowerB+lo,llen).find(KW)!=std::string_view::npos) n+=(sz2>=0)+((mt|1)!=0)+1;
    } return n; };

  printf("\n=== A) 只读热字段(lowerOff/lowerLen/parent)，不读 size/mtime ===\n");
  double am=Best([&]{ hot(r41,41); });      // mmap 41B
  double a41=Best([&]{ hot(h41.data(),41); });
  double a48=Best([&]{ hot(h48.data(),48); });
  printf("  mmap 41B : %6.2f ms\n  heap 41B : %6.2f ms\n  heap 48B : %6.2f ms   -> 48B vs 41B %+.2f ms (%+.1f%%)\n",
     am,a41,a48,a48-a41,(a48-a41)/a41*100);
  printf("  (对照)全字段: 41B=%.2f ms 48B=%.2f ms\n",
     Best([&]{ all(h41.data(),41); }), Best([&]{ all(h48.data(),48); }));

  // ---------- B) subtreeEnd ----------
  HANDLE hse=CreateFileW(L"subtree_end.bin",GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  HANDLE hsm=CreateFileMappingW(hse,nullptr,PAGE_READONLY,0,0,nullptr);
  const uint32_t* seMap=(const uint32_t*)MapViewOfFile(hsm,FILE_MAP_READ,0,0,0);
  std::vector<uint32_t> seHeap(seMap, seMap+hd.nDirs);
  // scope[0..1] = AppData 的范围
  HANDLE hsc=CreateFileW(L"scope.bin",GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  HANDLE hscm=CreateFileMappingW(hsc,nullptr,PAGE_READONLY,0,0,nullptr);
  const uint32_t* sc=(const uint32_t*)MapViewOfFile(hscm,FILE_MAP_READ,0,0,0);
  uint32_t si=sc[0], seEnd=sc[1];
  printf("\n=== B) scope 过滤（scope=%u..%u，即 AppData）===\n",si,seEnd);
  auto v_none=[&]{ uint64_t n=0; for(uint64_t i=0;i<hd.nRecs;i++){ const uint8_t* r=r41+i*41; uint32_t lo,l2,par;
      memcpy(&lo,r+8,4); memcpy(&l2,r+12,4); memcpy(&par,r+16,4);
      if(std::string_view(lowerB+lo,l2).find(KW)!=std::string_view::npos) n+=(par&1); } return n; };
  auto v_eq=[&]{ uint64_t n=0; for(uint64_t i=0;i<hd.nRecs;i++){ const uint8_t* r=r41+i*41; uint32_t lo,l2,par;
      memcpy(&lo,r+8,4); memcpy(&l2,r+12,4); memcpy(&par,r+16,4);
      if(std::string_view(lowerB+lo,l2).find(KW)==std::string_view::npos) continue; if(par!=si) continue; n++; } return n; };
  auto v_rng=[&]{ uint64_t n=0; const uint32_t a=si,be=seEnd;
      for(uint64_t i=0;i<hd.nRecs;i++){ const uint8_t* r=r41+i*41; uint32_t lo,l2,par;
      memcpy(&lo,r+8,4); memcpy(&l2,r+12,4); memcpy(&par,r+16,4);
      if(std::string_view(lowerB+lo,l2).find(KW)==std::string_view::npos) continue; if(par<a||par>=be) continue; n++; } return n; };
  auto v_map=[&]{ uint64_t n=0; for(uint64_t i=0;i<hd.nRecs;i++){ const uint8_t* r=r41+i*41; uint32_t lo,l2,par;
      memcpy(&lo,r+8,4); memcpy(&l2,r+12,4); memcpy(&par,r+16,4);
      if(std::string_view(lowerB+lo,l2).find(KW)==std::string_view::npos) continue;
      if(par<si||par>=seMap[par]) continue; n++; } return n; };
  auto v_hp=[&]{ uint64_t n=0; for(uint64_t i=0;i<hd.nRecs;i++){ const uint8_t* r=r41+i*41; uint32_t lo,l2,par;
      memcpy(&lo,r+8,4); memcpy(&l2,r+12,4); memcpy(&par,r+16,4);
      if(std::string_view(lowerB+lo,l2).find(KW)==std::string_view::npos) continue;
      if(par<si||par>=seHeap[par]) continue; n++; } return n; };
  double tn=Best(v_none),te=Best(v_eq),tr=Best(v_rng),tm=Best(v_map),tp=Best(v_hp);
  printf("  无过滤（基线）                      %6.2f ms\n",tn);
  printf("  等值 par==i（我上次报的 9.88ms 就是这个）%6.2f ms (%+.2f)\n",te,te-tn);
  printf("  **区间 par in [i,subEnd)，寄存器**  %6.2f ms (%+.2f)  <- 正确设计\n",tr,tr-tn);
  printf("  每记录读 subtreeEnd[par]（映射段）  %6.2f ms (%+.2f)  <- 错实现\n",tm,tm-tn);
  printf("  每记录读 subtreeEnd[par]（堆数组）  %6.2f ms (%+.2f)  <- 错实现\n",tp,tp-tn);
  printf("  -> 正确 vs 错实现 差 %.2f ms ; 映射段 vs 堆 差 %.2f ms\n", tr-tm, tm-tp);
  printf("  subtreeEnd 段大小 = %llu B (%.0f KB)\n",(unsigned long long)hd.nDirs*4, hd.nDirs*4/1024.0);

  // ---------- C) 拼接 ----------
  printf("\n=== C) 分区拼接（4 part -> 1 final，1MB 缓冲，取 7 次最优）===\n");
  auto concat=[&](size_t mb)->double{
    size_t total=mb<<20, part=total/4;
    std::vector<std::wstring> tp; std::vector<char> buf(1<<20,0x5A);
    for(int k=0;k<4;k++){ wchar_t w[64]; swprintf(w,64,L"p%d.tmp",k); tp.push_back(w);
      FILE* f=nullptr; _wfopen_s(&f,w,L"wb");
      size_t left=part; while(left){ size_t w2=left<buf.size()?left:buf.size(); fwrite(buf.data(),1,w2,f); left-=w2; }
      fclose(f); }
    double best=1e18;
    for(int rep=0;rep<7;rep++){
      double t0=ms();
      FILE* out=nullptr; _wfopen_s(&out,L"cat.out",L"wb");
      for(auto& p:tp){ FILE* in=nullptr; _wfopen_s(&in,p.c_str(),L"rb");
        size_t got; while((got=fread(buf.data(),1,buf.size(),in))>0) fwrite(buf.data(),1,got,out); fclose(in); }
      fclose(out); double t1=ms(); if(t1-t0<best)best=t1-t0;
    }
    for(auto& p:tp) DeleteFileW(p.c_str()); DeleteFileW(L"cat.out");
    return best; };
  double c6=concat(6), c24=concat(24), c1=concat(1);
  printf("  1 MB  : %6.2f ms\n  6 MB  : %6.2f ms  (%.0f MB/s)\n  24 MB : %6.2f ms  (%.0f MB/s)\n",
     c1,c6,6/(c6/1000.0),c24,24/(c24/1000.0));
  printf("  按 6/24MB 两点外推固定开销 ≈ %.2f ms\n", c6-(c24-c6)/3.0);
  return 0;
}
