import struct, collections
exec(open('partition.py',encoding='utf-8').read().split('def report')[0])
SEP="\\"
def analyze(fn):
    L,dirs,recs,nb=load(fn)
    VOLROOT=L.lower()+":"
    pp=[dirs[r[4]].lower() if r[4]<len(dirs) else VOLROOT for r in recs]
    def under(p,q): return p==q or p.startswith(q+SEP)
    shards,excl,bad,assigned,HOT = partition(L,dirs,recs,nb,verbose=False)
    # 目录记录 full->idx
    full2idx={}
    for i,r in enumerate(recs):
        if r[8]!=8: continue
        par=dirs[r[4]] if r[4]<len(dirs) else VOLROOT
        nm=nb[r[0]:r[0]+r[1]].decode('utf-8','replace')
        full2idx[(par+SEP+nm).lower()]=i
    def owner(path):
        c=[p for p in shards if under(path,p)]; c.sort(key=len,reverse=True)
        for p in c:
            if not any(under(path,e) for e in excl[p]): return p
        return None
    # 新模型：片根记录归子片。根记录的 parentPath = 前缀的父路径
    delta={p:0 for p in shards}; move=[]
    for p in shards:
        if p==VOLROOT: continue
        if p not in full2idx: continue
        parp = p[:p.rfind(SEP)]
        cur  = owner(parp)
        if cur is not None and cur!=p:
            delta[p]+=1; delta[cur]-=1
            move.append((p,cur))
    return dict(L=L,n=len(recs),shards=shards,excl=excl,bad=bad,HOT=HOT,delta=delta,move=move,VOLROOT=VOLROOT)

res=[analyze(f) for f in ("snapC.idx","snapD.idx")]
B=250.0; E=1048576
mb=lambda k:k*B/E
print("="*100)
print("【1】churn-bounded 分区结果（cap 25MB / floor 3MB；HOT 前缀不受 cap/floor）")
print(f"{'盘':<4}{'片数':>6}{'pure':>6}{'misc':>6}{'最大片':>10}{'misc容量占比':>14}{'excl最大':>9}{'无主记录':>9}")
tot_sh=0
for r in res:
    pure=[p for p in r['shards'] if not r['excl'][p]]; misc=[p for p in r['shards'] if r['excl'][p]]
    mx=max(mb(len(r['shards'][p])) for p in r['shards'])
    share=sum(len(r['shards'][p]) for p in misc)/r['n']*100
    emax=max((len(r['excl'][p]) for p in misc),default=0)
    tot_sh+=len(r['shards'])
    print(f"{r['L']:<4}{len(r['shards']):>6}{len(pure):>6}{len(misc):>6}{mx:>9.1f}MB{share:>13.1f}%{emax:>9}{r['bad']:>9}")
print(f"两盘合计片数 = {tot_sh}")
print()
print("【2】C: 分片明细 + 片根换归属后条数")
r=res[0]
for p in sorted(r['shards'],key=lambda q:-len(r['shards'][q])):
    k=len(r['shards'][p]); d=r['delta'][p]; kind="misc" if r['excl'][p] else "pure"
    tag=" [HOT]" if p in r['HOT'] else ""
    print(f"  {mb(k):6.1f}MB {k:7,d} -> {k+d:7,d} ({d:+d})  {kind:4s} excl={len(r['excl'][p])}{tag}  {p[2:] if p!=r['VOLROOT'] else '(整卷)'}")
print(f"  Σ = {sum(len(r['shards'][p])+r['delta'][p] for p in r['shards']):,}  (应 {r['n']:,})")
print(f"  片根记录搬迁: {len(r['move'])} 条")
print()
print("【3】那个怪片的去向（原 0.0MB/1条/misc/LCU/excl=1）")
for p in r['shards']:
    if p.endswith(SEP+"lcu"):
        print(f"  现在: {p[2:]}  条数={len(r['shards'][p])}  换归属后={len(r['shards'][p])+r['delta'][p]}  excl={len(r['excl'][p])}")
        for e in r['excl'][p]: print(f"       └ {e[2:][:80]}")
