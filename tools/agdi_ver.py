#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
agdi_ver.py — 定位 CMSIS_AGDI.dll 里**读取适配器条目字段**的代码

背景：AGDI 的适配器条目（步长 0x148）布局（由 dis 逆出）：
    +0x07C  名字        （写在 0x102F90C0 起，0x104 字节）
    +0x17D  序列号      （写在 0x102F91C1 起，0x20 字节）
    +0x19E  固件版本    （写在 0x102F91E2 起，0x20 字节）  ← Identify(idNo=4)
    +0x1C0  ifNo        （写在 0x102F9204 起）

思路：这些字段的"基址偏移"会以 disp32 形式出现在指令里（例如
    lea eax, [edi + 0x102F91E2]  →  e2 91 2f 10
）。按字节模式在整个 .text 里搜，就能找出**所有**读/写这些字段的点，
比逐个函数反汇编快且不漏。

用法：
    python tools/agdi_ver.py <dll> [--ctx 0x40]
"""

import struct
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_32

PATTERNS = {
    "固件版本 +0x102F91E2": bytes.fromhex("e2912f10"),
    "序列号   +0x102F91C1": bytes.fromhex("c1912f10"),
    "名字     +0x102F90C0": bytes.fromhex("c0902f10"),
    "ifNo     +0x102F9204": bytes.fromhex("04922f10"),
}


def parse_sections(d):
    e = struct.unpack_from("<I", d, 0x3C)[0]
    assert d[e:e + 4] == b"PE\0\0", "not a PE"
    coff = e + 4
    nsec = struct.unpack_from("<H", d, coff + 2)[0]
    optsize = struct.unpack_from("<H", d, coff + 16)[0]
    opt = coff + 20
    imagebase = struct.unpack_from("<I", d, opt + 28)[0]
    sec = opt + optsize
    out = []
    for i in range(nsec):
        o = sec + i * 40
        name = d[o:o + 8].rstrip(b"\0").decode("latin1")
        vsize, va, rsize, raw = struct.unpack_from("<IIII", d, o + 8)
        out.append((name, imagebase + va, vsize, raw, rsize))
    return imagebase, out


def va_to_off(secs, va):
    for name, sva, vsize, raw, rsize in secs:
        if sva <= va < sva + max(vsize, rsize):
            return raw + (va - sva)
    return None


def show_str(d, secs, va, n=64):
    off = va_to_off(secs, va)
    if off is None:
        print(f"  (va 0x{va:X} 不在任何节里)")
        return
    chunk = d[off:off + n]
    end = chunk.find(b"\0")
    if end >= 0:
        chunk = chunk[:end]
    print(f"  va 0x{va:X}: ascii={chunk!r}")


def show_callers(d, secs, text, target, ctx=0x40):
    tname, tva, tvsize, traw, trsize = text
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    hits = []
    for i in range(traw, traw + trsize - 5):
        if d[i] not in (0xE8, 0xE9):
            continue
        rel = struct.unpack_from("<i", d, i + 1)[0]
        va = tva + (i - traw)
        if va + 5 + rel == target:
            hits.append((va, d[i]))
    print(f"=== 指向 0x{target:X} 的 call/jmp：{len(hits)} 处 ===")
    for va, op in hits:
        base = va - ctx
        code = d[va - ctx - (tva - traw) if False else (tva - traw) + (va - ctx - tva):(tva - traw) + (va - ctx - tva) + ctx * 2]
        print(f"  -- {('call' if op == 0xE8 else 'jmp')} @0x{va:X}")
        for ins in md.disasm(code, base):
            mark = " <<<" if ins.address == va else ""
            print(f"     0x{ins.address:08X}  {ins.mnemonic:<7} {ins.op_str}{mark}")
    print()


def main():
    dll = sys.argv[1]
    ctx = 0x40
    if "--ctx" in sys.argv:
        ctx = int(sys.argv[sys.argv.index("--ctx") + 1], 0)

    d = open(dll, "rb").read()
    imagebase, secs = parse_sections(d)
    text = [s for s in secs if s[0] == ".text"][0]
    tname, tva, tvsize, traw, trsize = text

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    print(f"{dll}\n  imagebase=0x{imagebase:X}  .text va=0x{tva:X} raw=0x{traw:X} size=0x{trsize:X}\n")

    if "--str" in sys.argv:
        for a in sys.argv[sys.argv.index("--str") + 1:]:
            show_str(d, secs, int(a, 0))
        print()
    if "--callers" in sys.argv:
        for a in sys.argv[sys.argv.index("--callers") + 1:]:
            show_callers(d, secs, text, int(a, 0), ctx)
    if "--str" in sys.argv or "--callers" in sys.argv:
        return

    for label, pat in PATTERNS.items():
        hits = []
        idx = traw
        end = traw + trsize
        while True:
            i = d.find(pat, idx, end)
            if i < 0:
                break
            idx = i + 1
            hits.append(i)
        print(f"=== {label}  →  .text 内 {len(hits)} 处 ===")
        for i in hits:
            va = tva + (i - traw)
            base = va - ctx
            code = d[i - ctx:i + ctx]
            if base < tva:
                continue
            print(f"  -- va 0x{va:X} (file 0x{i:X})")
            for ins in md.disasm(code, base):
                mark = " <<<" if ins.address == va else ""
                print(f"     0x{ins.address:08X}  {ins.mnemonic:<7} {ins.op_str}{mark}")
        print()


if __name__ == "__main__":
    main()
