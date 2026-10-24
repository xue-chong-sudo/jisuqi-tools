import struct, os, collections
CACHE = r"C:/Users/Administrator/Desktop/极速启/data/cache"
HDR = struct.Struct("<8sIQIQQ"); RECP = struct.Struct("<IIIIIQQIB")
for name in ["vol_C.idx","vol_D.idx"]:
    p=os.path.join(CACHE,name); data=open(p,'rb').read()
    magic,ver,bt,letter,nRecs,nDirs=HDR.unpack_from(data,0)
    off=HDR.size; dirs=[]
    for i in range(nDirs):
        l,=struct.unpack_from("<H",data,off); off+=2
        dirs.append(data[off:off+l].decode('utf-8','replace')); off+=l
    recs=[]
    for i in range(nRecs):
        r=RECP.unpack_from(data,off); off+=RECP.size; recs.append(r)
    nl,=struct.unpack_from("<Q",data,off); off+=8+nl
    ll,=struct.unpack_from("<Q",data,off)
    # bucket each record by first path component of its dir
    root=f"{chr(letter)}:\\"
    buckets=collections.Counter()
    bytes_b=collections.Counter()
    for r in recs:
        parent = r[4]
        d = dirs[parent] if parent < len(dirs) else root
        rel = d[len(root):] if d.upper().startswith(root) else d
        top = rel.split("\\")[0] if rel else "(root)"
        buckets[top]+=1
        bytes_b[top]+= r[1]+r[3]   # name+lower utf8 bytes
    print("="*78)
    print(f"{name}  total recs={nRecs:,}  (avg 2 blobs {2*(nl)/nRecs:.1f}B/rec)")
    tot=sum(buckets.values())
    print(f"{'subtree':38s} {'recs':>9s} {'%':>6s}  {'1x bytes(MB)':>12s}  {'resident MB(est)':>16s}")
    print("-"*78)
    for k,v in buckets.most_common(18):
        pct=v/tot*100
        one = (v*48) + (bytes_b[k]*1.5)   # Rec actual + blobs at 1.5x cap
        one /= 1048576
        print(f"{k[:38]:38s} {v:9,d} {pct:5.1f}%  {bytes_b[k]/1048576:12.2f}  {one:16.1f}")
    # cumulative
    print("-"*78)
    cum=0
    for i,(k,v) in enumerate(buckets.most_common()):
        cum+=v
        if cum/tot>0.5: print(f"top {i+1} subtrees cover 50% of records"); break
    cum=0
    for i,(k,v) in enumerate(buckets.most_common()):
        cum+=v
        if cum/tot>0.9: print(f"top {i+1} subtrees cover 90% of records"); break
    print(f"n top-level entries={len(buckets)}")
