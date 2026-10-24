#!/usr/bin/env python3
"""解析 极速启/全盘搜索 的 vol_*.idx 索引文件，输出结构统计与内存估算。

支持多版本布局（供 v1/v2 回归对比）：
  ver=1  记录 41B packed（当前线上格式）
  ver=2  记录 48B 自然对齐（若采纳「磁盘布局=内存布局」）

用法:
  python parsec.py <cache_dir> [vol_C.idx vol_D.idx ...]
  python parsec.py            # 默认读 极速启/data/cache，全部卷
"""
import struct, os, sys, collections

MAGIC = b"WBSVOL1\x00"
HDR = struct.Struct("<8sIQIQQ")          # magic, ver, buildTime, letter, nRecs, nDirs

# 按 ver 区分的记录布局：41B packed 与 48B 自然对齐（尾部 3B padding）
REC_LAYOUT = {
    1: (struct.Struct("<IIIIIQQIB"), 41),   # packed
    2: (struct.Struct("<IIIIIQQIB3x"), 48), # 显式跳过 3B padding => 48B
}


def parse(path):
    data = open(path, "rb").read()
    magic, ver, buildtime, letter, nRecs, nDirs = HDR.unpack_from(data, 0)
    if magic != MAGIC:
        raise ValueError(f"{path}: bad magic {magic!r}")
    if ver not in REC_LAYOUT:
        raise ValueError(f"{path}: unsupported ver={ver} (known: {sorted(REC_LAYOUT)})")
    recfmt, recsize = REC_LAYOUT[ver]

    off = HDR.size
    dirs = []
    for _ in range(nDirs):
        (l,) = struct.unpack_from("<H", data, off); off += 2
        dirs.append(data[off:off + l].decode("utf-8", "replace")); off += l

    recs = []
    for _ in range(nRecs):
        r = recfmt.unpack_from(data, off); off += recsize
        recs.append(r)          # (nameOff,nameLen,lowerOff,lowerLen,parent,size,mtime,attrs,cat)

    (nl,) = struct.unpack_from("<Q", data, off); off += 8
    nameBlob = data[off:off + nl]; off += nl
    (ll,) = struct.unpack_from("<Q", data, off); off += 8
    lowerBlob = data[off:off + ll]; off += ll

    return dict(path=path, ver=ver, buildtime=buildtime, letter=chr(letter),
                nRecs=nRecs, nDirs=nDirs, dirs=dirs, recs=recs,
                nameBlob=nameBlob, lowerBlob=lowerBlob,
                filesize=len(data), trailing=len(data) - off, recsize=recsize)


def estimate(v):
    """估算该卷在堆内的常驻/峰值内存（Res 结构见 search_app.cpp 的 struct Rec）。"""
    n, d = v["nRecs"], v["nDirs"]
    sizeof_rec = 48                                   # 内存 Rec：uint64 对齐后 48B
    cap_ratio = 1.11                                  # 实测 MSVC vector push_back 后 cap/size
    recs = n * sizeof_rec * cap_ratio
    blobs = (len(v["nameBlob"]) + len(v["lowerBlob"])) * 1.5   # 实测 string 增长 1.5x
    wchar = sum(len(x) for x in v["dirs"])            # 近似：UTF-8 字节数≈字符数（ASCII 主导）
    dirs_heap = d * 32 + d * 16 + wchar * 2           # wstring 对象 + CRT 头 + 字符
    return dict(recs=recs, blobs=blobs, dirs=dirs_heap, total=recs + blobs + dirs_heap)


def report(v):
    print("=" * 74)
    print(f"{os.path.basename(v['path'])}  ver={v['ver']}  recsize={v['recsize']}B  "
          f"letter={v['letter']}  file={v['filesize']/1048576:.2f}MB  trailing={v['trailing']}")
    print(f"  nRecs={v['nRecs']:,}  nDirs={v['nDirs']:,}")
    print(f"  nameBlob={len(v['nameBlob'])/1048576:.2f}MB  lowerBlob={len(v['lowerBlob'])/1048576:.2f}MB")
    if v["nRecs"]:
        avg = sum(r[1] for r in v["recs"]) / v["nRecs"]
        print(f"  avg nameLen={avg:.1f}B   on-disk B/条={v['filesize']/v['nRecs']:.1f}")
    e = estimate(v)
    print(f"  堆估算: recs={e['recs']/1048576:.1f}MB  blobs={e['blobs']/1048576:.1f}MB  "
          f"dirs={e['dirs']/1048576:.1f}MB  => 合计 {e['total']/1048576:.1f}MB")
    if v["nDirs"]:
        print(f"  dirs 平均 wchar={(sum(len(x) for x in v['dirs'])/v['nDirs']):.1f}")
    # 顶层子树分布
    root = v["letter"] + ":" + "\\"
    b = collections.Counter()
    for r in v["recs"]:
        p = r[4]
        dd = v["dirs"][p] if p < len(v["dirs"]) else root
        rel = dd[len(root):] if dd.upper().startswith(root) else dd
        parts = [x for x in rel.split("\\") if x]
        b[parts[0] if parts else "(root)"] += 1
    top = b.most_common(5)
    print("  顶层子树 Top5: " + "  ".join(f"{k}={c:,}({c/v['nRecs']*100:.1f}%)" for k, c in top))


def main():
    if len(sys.argv) > 1:
        base = sys.argv[1]
        names = sys.argv[2:] or sorted(
            f for f in os.listdir(base) if f.startswith("vol_") and f.endswith(".idx"))
    else:
        base = r"C:/Users/Administrator/Desktop/极速启/data/cache"
        names = sorted(f for f in os.listdir(base) if f.startswith("vol_") and f.endswith(".idx"))

    total = 0
    for n in names:
        p = n if os.path.isabs(n) else os.path.join(base, n)
        if not os.path.exists(p):
            print(f"skip (not found): {p}"); continue
        v = parse(p)
        report(v)
        total += estimate(v)["total"]
    print("=" * 74)
    print(f"全部卷堆内存估算合计 = {total/1048576:.1f} MB")


if __name__ == "__main__":
    main()
