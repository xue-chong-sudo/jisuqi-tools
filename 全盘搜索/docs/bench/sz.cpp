#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <windows.h>
#include <psapi.h>
#pragma comment(lib,"psapi.lib")
struct Rec {
    uint32_t nameOff = 0, nameLen = 0, lowerOff = 0, lowerLen = 0, parent = 0;
    uint64_t size = 0, mtime = 0;
    uint32_t attrs = 0;
    uint8_t  cat = 0;
};
static size_t Priv(){ PROCESS_MEMORY_COUNTERS_EX m{}; m.cb=sizeof m;
  GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&m,sizeof m); return m.PrivateUsage; }
int main(){
    printf("sizeof(Rec)=%zu  alignof(Rec)=%zu\n", sizeof(Rec), alignof(Rec));
    printf("sizeof(std::wstring)=%zu  sizeof(std::string)=%zu\n", sizeof(std::wstring), sizeof(std::string));
    // MSVC vector growth factor
    { std::vector<int> v; size_t prev=0; double r=0;
      for(int i=0;i<3000000;i++){ v.push_back(i); if(v.capacity()!=prev){ prev=v.capacity(); if(v.size()>500000) r=(double)v.capacity()/v.size(); } }
      printf("vector<int> final cap/size ratio=%.4f\n", r); }
    // realistic: 419273 records C: -- measure actual private delta for recs+blobs+dirs
    size_t n = 419273;
    size_t p0 = Priv();
    auto* recs = new std::vector<Rec>(); 
    for(size_t i=0;i<n;i++){ Rec r; r.nameOff=(uint32_t)(i*40); r.nameLen=39; recs->push_back(r); }
    size_t p1 = Priv();
    printf("[recs] size=%zu cap=%zu bytes=%zu (%.2fMB) priv_delta=%.2fMB  per-rec-capacity=%.1fB\n",
        recs->size(), recs->capacity(), recs->size()*sizeof(Rec), recs->size()*sizeof(Rec)/1048576.0,
        (p1-p0)/1048576.0, (double)recs->capacity()*sizeof(Rec)/recs->size());
    // blobs: 16.17MB each, appended incrementally (worst case geometric growth)
    size_t p2 = Priv();
    std::string nameBlob, lowerBlob;
    char buf[64]; memset(buf,'a',sizeof buf); buf[39]=0;
    for(size_t i=0;i<n;i++){ nameBlob.append(buf,39); lowerBlob.append(buf,39); }
    size_t p3 = Priv();
    printf("[blobs] name=%zu(%.2fMB cap) lower=%zu(%.2fMB cap) priv_delta=%.2fMB\n",
        nameBlob.capacity(), nameBlob.capacity()/1048576.0, lowerBlob.capacity(), lowerBlob.capacity()/1048576.0,(p3-p2)/1048576.0);
    printf("TOTAL sim priv delta = %.2f MB\n", (Priv()-p0)/1048576.0);
    return 0;
}
