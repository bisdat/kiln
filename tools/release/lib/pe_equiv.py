#!/usr/bin/env python3
"""pe_equiv.py A B: are two managed PE files (a .dll built on two machines) the same program? Exit 0 when they are equal
after blanking what a deterministic C# build derives from the PDB and the build machine's compiler build: the COFF time
stamp and PE checksum (content hashes), every debug directory entry's stamp and data (CodeView PDB id + path, PDB
checksum), and the module MVID. Everything else (headers, IL, metadata, resources) must match byte for byte.
Used to compare the sample mods win1 builds (tools/release/lib/remote-win1.sh) with a local build of the same commit."""
import struct, sys

def normalize(data: bytes) -> bytes:
    b = bytearray(data)
    pe = struct.unpack_from("<I", b, 0x3C)[0]
    if b[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE file")
    nsec = struct.unpack_from("<H", b, pe + 6)[0]
    optsize = struct.unpack_from("<H", b, pe + 20)[0]
    struct.pack_into("<I", b, pe + 8, 0)                  # COFF TimeDateStamp
    opt = pe + 24
    magic = struct.unpack_from("<H", b, opt)[0]
    struct.pack_into("<I", b, opt + 64, 0)                # CheckSum
    dd = opt + (96 if magic == 0x10B else 112)
    secs = []
    for i in range(nsec):
        s = opt + optsize + 40 * i
        vsize, va, rsize, rptr = struct.unpack_from("<IIII", b, s + 8)
        secs.append((va, max(vsize, rsize), rptr))
    def off(rva):
        for va, size, rptr in secs:
            if va <= rva < va + size:
                return rva - va + rptr
        raise ValueError(f"RVA {rva:#x} in no section")
    dbg_rva, dbg_size = struct.unpack_from("<II", b, dd + 6 * 8)
    if dbg_rva:
        d = off(dbg_rva)
        for e in range(dbg_size // 28):
            ent = d + 28 * e
            struct.pack_into("<I", b, ent + 4, 0)          # TimeDateStamp
            size, _rva, ptr = struct.unpack_from("<III", b, ent + 16)
            b[ptr:ptr + size] = bytes(size)                # CodeView (RSDS id + path), PDB checksum, ...
    cli_rva, _ = struct.unpack_from("<II", b, dd + 14 * 8)
    if cli_rva:
        c = off(cli_rva)
        md = off(struct.unpack_from("<I", b, c + 8)[0])
        vlen = struct.unpack_from("<I", b, md + 12)[0]
        p = md + 16 + vlen
        nstreams = struct.unpack_from("<H", b, p + 2)[0]
        p += 4
        for _ in range(nstreams):
            so, ss = struct.unpack_from("<II", b, p)
            name_end = b.index(0, p + 8)
            name = bytes(b[p + 8:name_end])
            p = (name_end + 4) & ~3
            if name == b"#GUID" and ss >= 16:
                b[md + so:md + so + 16] = bytes(16)        # the Module row's MVID (GUID index 1)
    return bytes(b)

def mvid(data: bytes) -> str:
    """The module MVID of a managed PE file, as a lowercase GUID string."""
    import uuid
    b = data
    pe = struct.unpack_from("<I", b, 0x3C)[0]
    nsec = struct.unpack_from("<H", b, pe + 6)[0]
    optsize = struct.unpack_from("<H", b, pe + 20)[0]
    opt = pe + 24
    dd = opt + (96 if struct.unpack_from("<H", b, opt)[0] == 0x10B else 112)
    secs = [struct.unpack_from("<IIII", b, opt + optsize + 40 * i + 8) for i in range(nsec)]
    def off(rva):
        for vsize, va, rsize, rptr in secs:
            if va <= rva < va + max(vsize, rsize):
                return rva - va + rptr
        raise ValueError(f"RVA {rva:#x} in no section")
    md = off(struct.unpack_from("<I", b, off(struct.unpack_from("<I", b, dd + 14 * 8)[0]) + 8)[0])
    p = md + 16 + struct.unpack_from("<I", b, md + 12)[0]
    nstreams = struct.unpack_from("<H", b, p + 2)[0]
    p += 4
    for _ in range(nstreams):
        so, ss = struct.unpack_from("<II", b, p)
        name_end = b.index(0, p + 8)
        if bytes(b[p + 8:name_end]) == b"#GUID":
            return str(uuid.UUID(bytes_le=bytes(b[md + so:md + so + 16])))
        p = (name_end + 4) & ~3
    raise ValueError("no #GUID stream")

def main():
    a, b = (open(p, "rb").read() for p in sys.argv[1:3])
    if a == b:
        print("identical"); return 0
    if normalize(a) == normalize(b):
        print("equivalent (differ only in time stamp, checksum, debug directory, MVID)"); return 0
    print("DIFFERENT"); return 1

if __name__ == "__main__":
    sys.exit(main())
