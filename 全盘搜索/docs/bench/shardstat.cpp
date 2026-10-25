// shardstat: 解析 v2 索引文件，输出各目录子树的记录数（用于设计分片策略）
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

#pragma pack(push,1)
struct Rec { uint32_t nameOff,nameLen,lowerOff,lowerLen,parent; uint64_t size,mtime; uint32_t attrs; uint8_t cat; };
struct IdxHead { char magic[8]; uint32_t ver; uint64_t buildTime; uint32_t letter; uint64_t nRecs,nDirs; };
#pragma pack(pop)

static std::wstring U8(const std::string& s){ if(s.empty())return{}; int n=MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0); std::wstring w(n,0); MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),w.data(),n); return w; }

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { printf("usage: shardstat <idx>\n"); return 1; }
    FILE* f = nullptr;
    if (_wfopen_s(&f, argv[1], L"rb") || !f) { printf("open fail\n"); return 2; }
    IdxHead h{}; if (fread(&h,sizeof h,1,f)!=1) return 3;
    if (memcmp(h.magic,"WBSVOL1",7)!=0) { printf("bad magic\n"); return 4; }
    printf("ver=%u letter=%c nRecs=%llu nDirs=%llu\n", h.ver, (char)h.letter,
           (unsigned long long)h.nRecs, (unsigned long long)h.nDirs);
    auto rd16=[&](uint16_t&v){ return fread(&v,2,1,f)==1; };
    if (h.ver>=2){ uint16_t pl=0; rd16(pl); std::vector<char> b(pl); if(pl) fread(b.data(),1,pl,f); }
    std::vector<std::wstring> dirs; dirs.reserve((size_t)h.nDirs);
    for (uint64_t i=0;i<h.nDirs;i++){ uint16_t l=0; if(!rd16(l)){printf("dirs trunc\n");return 5;} std::string s(l,0); if(l) fread(s.data(),1,l,f); dirs.push_back(U8(s)); }
    std::vector<uint32_t> sub((size_t)h.nDirs,0);
    if (h.ver>=2 && h.nDirs) fread(sub.data(),4,(size_t)h.nDirs,f);
    std::vector<uint32_t> own((size_t)h.nDirs,0);
    for (uint64_t i=0;i<h.nRecs;i++){ Rec r{}; if(fread(&r,sizeof r,1,f)!=1){printf("recs trunc\n");return 6;} if(r.parent<own.size()) own[r.parent]++; }
    // 按一级目录聚合
    std::map<std::wstring,std::pair<unsigned long long,unsigned long long>> lvl1; // name -> (dirs, recs)
    for (uint64_t i=0;i<h.nDirs;i++){
        const std::wstring& p = dirs[(size_t)i];
        // p like "C:" or "C:/Users" or "C:/Users/x"
        if (p.size()<=2) { lvl1[L"<root>"].first++; lvl1[L"<root>"].second += own[(size_t)i]; continue; }
        std::wstring rest = p.substr(3);
        size_t bs = rest.find(L'\\');
        std::wstring k = (bs==std::wstring::npos) ? rest : rest.substr(0,bs);
        lvl1[k].first++; lvl1[k].second += own[(size_t)i];
    }
    std::vector<std::pair<std::wstring,std::pair<unsigned long long,unsigned long long>>> v(lvl1.begin(),lvl1.end());
    std::sort(v.begin(),v.end(),[](auto&a,auto&b){return a.second.second>b.second.second;});
    printf("\n== 一级目录（自身记录数）==\n");
    for (auto& e : v) printf("%8llu recs %7llu dirs  %ls\n", e.second.second, e.second.first, e.first.c_str());

    // 对前 6 大做二级细分
    printf("\n== 二级细分（前 6 大）==\n");
    int shown=0;
    for (auto& e : v) {
        if (shown>=6) break;
        if (e.first==L"<root>") continue;
        std::map<std::wstring,unsigned long long> lvl2;
        for (uint64_t i=0;i<h.nDirs;i++){
            const std::wstring& p=dirs[(size_t)i];
            if (p.size()<=3) continue;
            std::wstring rest=p.substr(3);
            size_t bs=rest.find(L'\\');
            if (bs==std::wstring::npos) continue;
            std::wstring k1=rest.substr(0,bs);
            if (k1!=e.first) continue;
            std::wstring rest2=rest.substr(bs+1);
            size_t bs2=rest2.find(L'\\');
            std::wstring k2=(bs2==std::wstring::npos)?rest2:rest2.substr(0,bs2);
            if (k2.empty()) k2=L"<self>";
            lvl2[k2]+=own[(size_t)i];
        }
        std::vector<std::pair<std::wstring,unsigned long long>> w(lvl2.begin(),lvl2.end());
        std::sort(w.begin(),w.end(),[](auto&a,auto&b){return a.second>b.second;});
        printf("\n[%ls] 共 %llu\n", e.first.c_str(), e.second.second);
        int n=0; for(auto& q:w){ if(n++>=8) break; printf("   %8llu  %ls\n", q.second, q.first.c_str()); }
        shown++;
    }
    fclose(f);
    return 0;
}
