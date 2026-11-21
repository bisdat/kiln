#!/usr/bin/env python3
"""compare_samples.py A B: are two releases' sample mod zips (A/samples/*.zip, B/samples/*.zip; or two folders of zips)
the same mods? For checking a KILN_BUILD_REMOTE=win1 build against a local one of the same commit. Per zip: the same
member names; each member byte-identical, or a DLL equal but for what lib/pe_equiv.py blanks (PDB id and path, time
stamp, checksum, MVID), or kiln.gametypes.json equal but for its "mvid", which must be its own zip's DLL's MVID. PDBs
are listed apart: they carry the compiler's build string (a source-built SDK here, Microsoft's build on win1)."""
import io, json, os, sys, zipfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pe_equiv

def zips(root):
    d = os.path.join(root, "samples") if os.path.isdir(os.path.join(root, "samples")) else root
    return {n: os.path.join(d, n) for n in sorted(os.listdir(d)) if n.endswith(".zip")}

def main():
    a, b = zips(sys.argv[1]), zips(sys.argv[2])
    problems, stats = [], {"identical": 0, "dll-equivalent": 0, "gametypes-equivalent": 0, "pdb-differs": 0}
    if set(a) != set(b):
        problems.append(f"zip lists differ: only in A {sorted(set(a) - set(b))}, only in B {sorted(set(b) - set(a))}")
    for name in sorted(set(a) & set(b)):
        za, zb = zipfile.ZipFile(a[name]), zipfile.ZipFile(b[name])
        na, nb = za.namelist(), zb.namelist()
        if sorted(na) != sorted(nb):
            problems.append(f"{name}: members differ: only in A {sorted(set(na) - set(nb))}, only in B {sorted(set(nb) - set(na))}")
            continue
        for m in sorted(na):
            x, y = za.read(m), zb.read(m)
            if x == y:
                stats["identical"] += 1
            elif m.lower().endswith((".dll", ".exe")) and pe_equiv.normalize(x) == pe_equiv.normalize(y):
                stats["dll-equivalent"] += 1
            elif m.lower().endswith(".pdb"):
                stats["pdb-differs"] += 1
            elif m.endswith("kiln.gametypes.json"):
                jx, jy = json.loads(x), json.loads(y)
                ok = True
                for j, z in ((jx, za), (jy, zb)):
                    dll = m.rsplit("/", 1)[0] + "/" + j["assembly"]
                    ok &= j.pop("mvid") == pe_equiv.mvid(z.read(dll))
                if ok and jx == jy:
                    stats["gametypes-equivalent"] += 1
                else:
                    problems.append(f"{name}: {m} differs beyond its own DLL's MVID")
            else:
                problems.append(f"{name}: {m} differs")
    print(f"{len(set(a) & set(b))} zips compared: " + ", ".join(f"{k} {v}" for k, v in stats.items()))
    for p in problems:
        print("DIFFERENT: " + p)
    return 1 if problems else 0

if __name__ == "__main__":
    sys.exit(main())
