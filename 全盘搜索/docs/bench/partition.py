#!/usr/bin/env python3
"""churn-bounded 分区器：
   1) HOT 前缀先切出为独立片（不受 cap/floor）
   2) 余量按 cap/floor size 分区
   3) 事后统一计算 exclusions = 最近后代片前缀（保证「最长前缀匹配」唯一归属）
"""
import struct, collections, sys

SEP = "\\"; B = 250.0; E = 1048576
CAP  = 25*E/B      # 104,857 文件
FLOOR= 3*E/B       # 12,582 文件

def load(path):
    HDR = struct.Struct("<8sIQIQQ"); RECP = struct.Struct("<IIIIIQQIB")
    d = open(path,'rb').read()
    magic,ver,bt,letter,nRecs,nDirs = HDR.unpack_from(d,0)
    off = HDR.size; dirs=[]
    for _ in range(nDirs):
        l,=struct.unpack_from("<H",d,off); off+=2
        dirs.append(d[off:off+l].decode('utf-8','replace')); off+=l
    rs=off; recs=[RECP.unpack_from(d,rs+i*41) for i in range(nRecs)]
    p=rs+nRecs*41
    nl,=struct.unpack_from("<Q",d,p); p+=8; nb=d[p:p+nl]
    return chr(letter), dirs, recs, nb

def make_hot(L):
    U = L.lower()+":"+SEP+"users"+SEP+"administrator"
    return {U+SEP+"appdata"+SEP+"roaming", U+SEP+"appdata"+SEP+"local",
            U+SEP+"appdata"+SEP+"locallow", U+SEP+"desktop",
            U+SEP+"documents", U+SEP+"downloads"}

def partition(L, dirs, recs, nb, verbose=True):
    VOLROOT = L.lower()+":"
    pp = [dirs[r[4]].lower() if r[4] < len(dirs) else VOLROOT for r in recs]
    by_par = collections.defaultdict(list)
    for i,x in enumerate(pp): by_par[x].append(i)
    def under(path,pref): return path==pref or path.startswith(pref+SEP)
    def ncomp(p): return len([x for x in p.split(SEP) if x])

    HOT = make_hot(L)
    hot_present = sorted(h for h in HOT if any(under(x,h) for x in by_par))

    # 1) HOT 片
    shards = {}          # prefix -> list of rec idx
    for h in hot_present:
        inner = [i for x,idxs in by_par.items() if under(x,h) and any(under(x,o) for o in hot_present if o!=h and o.startswith(h+SEP)) for i in idxs]
        nested = [o for o in hot_present if o.startswith(h+SEP)]
        shards[h] = [i for x,idxs in by_par.items() if under(x,h) and not any(under(x,o) for o in nested) for i in idxs]

    # 2) 余量
    rest = [i for i,x in enumerate(pp) if not any(under(x,h) for h in hot_present)]
    restset = set(rest)
    rest_by_par = collections.defaultdict(list)
    for i in rest: rest_by_par[pp[i]].append(i)

    def build(pref):
        # 该前缀下、且未落入更近已建片 的余量记录
        own = []
        for x,idxs in rest_by_par.items():
            if not under(x,pref): continue
            own.extend(idxs)
        if len(own) <= CAP:
            shards[pref] = own; return
        depth = ncomp(pref)
        leaf = []                      # parentPath == pref 的记录（本层直系子项）
        groups = collections.defaultdict(list)
        for x,idxs in rest_by_par.items():
            if not under(x,pref): continue
            if x == pref: leaf.extend(idxs); continue
            comps=[y for y in x.split(SEP) if y]
            if depth >= len(comps): leaf.extend(idxs); continue
            groups[comps[depth]].extend(idxs)
        if len(groups) == 1:
            only = next(iter(groups))
            sub = groups[only]
            if len(sub) == len(own) - len(leaf):
                if leaf: shards[pref] = leaf
                build(pref+SEP+only); return
        if len(groups) <= 1:
            shards[pref] = own; return
        big   = {k:v for k,v in groups.items() if len(v) >= FLOOR}
        small = {k:v for k,v in groups.items() if len(v) <  FLOOR}
        # 本片：直系子项 + 所有小兄弟
        self_items = leaf + [i for v in small.values() for i in v]
        if self_items: shards[pref] = self_items
        for k in sorted(big): build(pref+SEP+k)
    build(VOLROOT)

    # 3) exclusions = 最近后代片
    excl = {}
    prefs = list(shards)
    for p in prefs:
        desc = [q for q in prefs if q!=p and q.startswith(p+SEP)]
        near = [q for q in desc if not any(o!=q and o.startswith(p+SEP) and q.startswith(o+SEP) for o in desc)]
        excl[p] = sorted(near)

    # 4) 校验唯一归属
    def owner(path):
        c=[p for p in prefs if under(path,p)]
        c.sort(key=len, reverse=True)
        for p in c:
            if not any(under(path,e) for e in excl[p]): return p
        return None
    bad=0
    for i,x in enumerate(pp):
        if owner(x) is None: bad+=1
    assigned = sum(len(v) for v in shards.values())
    return shards, excl, bad, assigned, HOT

def report(L, dirs, recs, nb):
    shards, excl, bad, assigned, HOT = partition(L, dirs, recs, nb)
    n=len(recs)
    mb=lambda k: k*B/E
    pure=[p for p in shards if not excl[p]]; misc=[p for p in shards if excl[p]]
    print("="*92)
    print(f"盘 {L}:  nRecs={n:,}   片数={len(shards)}  pure={len(pure)}  misc={len(misc)}")
    print(f"  归属校验: 无主记录={bad}   已分配={assigned:,}/{n:,} {'OK' if assigned==n else '差 %d'%(n-assigned)}")
    sp=sorted((mb(len(shards[p])) for p in pure),reverse=True)
    sm=sorted((mb(len(shards[p])) for p in misc),reverse=True)
    if sp: print(f"  pure 片: 最大 {sp[0]:.1f}MB  中位 {sp[len(sp)//2]:.1f}MB")
    if sm: print(f"  misc 片: 最大 {sm[0]:.1f}MB  中位 {sm[len(sm)//2]:.1f}MB  超cap={sum(1 for x in sm if x>25)}  容量占比={sum(len(shards[p]) for p in misc)/n*100:.1f}%")
    print(f"  exclusions: 分布={dict(sorted(collections.Counter(len(excl[p]) for p in misc).items()))}  最大={max((len(excl[p]) for p in misc),default=0)}")
    print(f"  HOT 片: {len([p for p in shards if p in HOT])} 个")
    print("\n  明细（按大小降序）:")
    for p in sorted(shards, key=lambda q:-len(shards[q])):
        k=len(shards[p]); kind="misc" if excl[p] else "pure"
        tag=" [HOT]" if p in HOT else ""
        print(f"   {mb(k):6.1f}MB {k:7,d}  {kind:4s} excl={len(excl[p])}{tag}  {p[2:] if p!=L.lower()+':' else '(整卷)'}")
        for e in excl[p][:4]: print(f"           └ 排除 {e[2:]}")
        if len(excl[p])>4: print(f"           └ ... 另 {len(excl[p])-4} 条")
    delta, missing = root_record_shift(L, dirs, recs, nb, shards, excl)
    print(f"  片根记录换归属影响: 涉及 {len([p for p in delta if delta[p]])} 片；找不到根记录的片={len(missing)}")
    for p in sorted(delta, key=lambda q:-abs(delta[q])):
        if delta[p]:
            print(f"    {p[2:] if p!=L.lower()+':' else '(整卷)'}: {len(shards[p]):,} -> {len(shards[p])+delta[p]:,} ({delta[p]:+d})")
    print(f"  换归属后 Σ片 = {sum(len(shards[p])+delta[p] for p in shards):,}  (应={n:,})")
    return shards, excl, HOT

def root_record_shift(L, dirs, recs, nb, shards, excl):
    """新模型：片根自身的目录记录归子片所有。返回 (每片调整量, 汇总)"""
    VOLROOT = L.lower()+":"
    # 建立 目录记录 full path -> rec idx
    full2idx = {}
    for i,r in enumerate(recs):
        if r[8] != 8: continue                       # cat==8 == CAT_DIR
        par = dirs[r[4]] if r[4] < len(dirs) else VOLROOT
        nm  = nb[r[0]:r[0]+r[1]].decode('utf-8','replace')
        full = (par + SEP + nm) if not par.endswith(':') else (par + SEP + nm)
        full2idx[full.lower()] = i
    def under(p,q): return p==q or p.startswith(q+SEP)
    def owner(path):
        c=[p for p in shards if under(path,p)]; c.sort(key=len,reverse=True)
        for p in c:
            if not any(under(path,e) for e in excl[p]): return p
        return None
    delta = {p:0 for p in shards}
    missing=[]
    for p in shards:
        if p == VOLROOT: continue
        ri = full2idx.get(p)
        if ri is None: missing.append(p); continue
        cur = owner(p)                     # 该目录记录当前属于哪个片（按父目录归属）
        if cur != p:
            delta[p]  += 1
            if cur is not None: delta[cur] -= 1
    return delta, missing

if __name__ == "__main__":
    for f in (sys.argv[1:] or ["snapC.idx","snapD.idx"]):
        L,dirs,recs,nb = load(f)
        report(L,dirs,recs,nb)
