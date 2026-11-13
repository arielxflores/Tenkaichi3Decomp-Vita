"""The shader bank (port/vita/shbank.bin, packed into the VPK as app0:shbank.bin; port/vita/vita_vgl.c sh_cached):
the programs the game uses, as the Vita's own shader compiler made them, so that the game starts without compiling
and without libshacccg.suprx for them (the idea of Grarak's MH3 port's offline shader bank).

Made from the files a run with bt3.txt shbank=1 writes on the Vita (ux0:data/bt3/shbank.bin, and BT4's program's
shbank-bt4.bin), fetched to the PC:

    python3 port/vita/tools/shbank.py port/vita/shbank.bin shbank.bin [shbank-bt4.bin ...] [port/vita/shbank.bin]

Every program of every file once (by the hash of its source, which the file keeps), the first file's first. Giving
the bank itself as one of the inputs keeps what it had. `--list FILE` prints what a file holds.
File: "BT3SHBK1", then per program: the hash (8 bytes), the size (4), the binary, padded to 4 bytes.
"""
import struct
import sys

MAGIC = b'BT3SHBK1'


def read(path):
    d = open(path, 'rb').read()
    if d[:8] != MAGIC:
        raise SystemExit(f'{path}: not a shader bank file')
    out, off = [], 8
    while off + 12 <= len(d):
        h, n = struct.unpack_from('<QI', d, off)
        if n == 0 or off + 12 + n > len(d):
            print(f'{path}: cut short at {off} (a run stopped while writing): the rest left out')
            break
        out.append((h, d[off + 12:off + 12 + n]))
        off += 12 + ((n + 3) & ~3)
    return out


def main():
    if len(sys.argv) == 3 and sys.argv[1] == '--list':
        for h, b in read(sys.argv[2]):
            print(f'{h:016x} {len(b):6d} bytes')
        return
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    seen, prog = set(), []
    for path in sys.argv[2:]:
        for h, b in read(path):
            if h not in seen:
                seen.add(h)
                prog.append((h, b))
    with open(sys.argv[1], 'wb') as f:
        f.write(MAGIC)
        for h, b in prog:
            f.write(struct.pack('<QI', h, len(b)))
            f.write(b)
            f.write(b'\0' * (-len(b) & 3))
    print(f'{sys.argv[1]}: {len(prog)} programs, {sum(len(b) for _, b in prog) // 1024} KB')


if __name__ == '__main__':
    main()
