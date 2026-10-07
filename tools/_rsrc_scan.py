#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
T0/T1 recon helper (temporary; delete when done).

Usage:
    python tools/_rsrc_scan.py <pe> [extra_ids] [--dialog <id>] [--iat <name>]

Sections:
  1) resource directory summary
  2) DIALOG templates (DLGTEMPLATEEX parsed) / DLGINIT combobox items
  3) user32/comctl32 dialog+window APIs in the import table
  4) `push imm32` hits for interesting control IDs (0x44C..0x4A3, 0x578/0x579, 100..104)
     with a short disassembly window
  5) `call dword ptr [<IAT>]` hits for a named import (e.g. EnableWindow) -- patch targets

Output labels are English on purpose (console codepage).
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe_re import PE  # noqa: E402

RT = {1: 'CURSOR', 2: 'BITMAP', 3: 'ICON', 4: 'MENU', 5: 'DIALOG', 6: 'STRING',
      7: 'FONTDIR', 8: 'FONT', 9: 'ACCELERATOR', 10: 'RCDATA', 11: 'MESSAGETABLE',
      12: 'GROUP_CURSOR', 14: 'GROUP_ICON', 16: 'VERSION', 17: 'DLGINCLUDE',
      19: 'PLUGPLAY', 20: 'VXD', 21: 'ANICURSOR', 22: 'ANIICON', 23: 'HTML',
      24: 'MANIFEST', 240: 'DLGINIT'}

# --- Trace page control IDs, from AGDI DIALOG 102 (COMPAT_ANALYSIS.md 17.10) ---
TRACE_CTRL_IDS = [0x44C, 0x44D, 0x44E, 0x450, 0x451, 0x456, 0x457, 0x458, 0x459,
                  0x45A, 0x45B, 0x460, 0x461, 0x46A, 0x46B, 0x46C, 0x46D,
                  0x474, 0x475, 0x476, 0x477, 0x478, 0x479, 0x47E, 0x47F,
                  0x578, 0x579, 0x5EC]


def rsrc_entries(pe):
    d = pe.d
    rsrc_rva = struct.unpack_from('<I', d, pe.ddoff + 2 * 8)[0]
    if not rsrc_rva:
        return []
    base = pe.r2o(rsrc_rva)
    if base is None:
        return []
    out = []

    def name_of(raw):
        if raw & 0x80000000:
            so = base + (raw & 0x7FFFFFFF)
            ln = struct.unpack_from('<H', d, so)[0]
            return d[so + 2:so + 2 + ln * 2].decode('utf-16-le', 'ignore')
        return raw

    def walk(off, acc):
        nnamed, nid = struct.unpack_from('<HH', d, off + 12)
        for i in range(nnamed + nid):
            eoff = off + 16 + i * 8
            raw_name, od = struct.unpack_from('<II', d, eoff)
            nm = name_of(raw_name)
            if od & 0x80000000:
                walk(base + (od & 0x7FFFFFFF), acc + [nm])
            else:
                rva, size, _cp, _res = struct.unpack_from('<IIII', d, base + od)
                t = acc[0] if len(acc) > 0 else None
                i2 = acc[1] if len(acc) > 1 else None
                out.append((t, i2, rva, size))

    walk(base, [])
    return out


def ansi_strs(data, minlen=1, limit=60):
    res = []
    for s in data.split(b'\0'):
        if len(s) >= minlen:
            try:
                res.append(s.decode('latin1'))
            except Exception:                     # noqa: BLE001
                pass
        if len(res) >= limit:
            break
    return res


def wide_strs(data, minlen=2, limit=40):
    res, cur = [], []
    for i in range(0, len(data) - 1, 2):
        ch = data[i] | (data[i + 1] << 8)
        if 0x20 <= ch < 0x7F:
            cur.append(chr(ch))
        else:
            if len(cur) >= minlen:
                res.append(''.join(cur))
                if len(res) >= limit:
                    return res
            cur = []
    if len(cur) >= minlen:
        res.append(''.join(cur))
    return res[:limit]


def import_funcs(pe):
    """-> [(dll, func, iat_va)]"""
    d = pe.d
    imp_rva = struct.unpack_from('<I', d, pe.ddoff + 8)[0]
    if not imp_rva:
        return []
    o = pe.r2o(imp_rva)
    out = []
    while True:
        oft, _ts, _fc, namerva, ft = struct.unpack_from('<IIIII', d, o)
        if namerva == 0:
            break
        no = pe.r2o(namerva)
        end = d.index(b'\0', no)
        dll = d[no:end].decode('latin1')
        thunk = oft or ft
        k = pe.r2o(thunk)
        slot = pe.r2o(ft)
        idx = 0
        while True:
            v = struct.unpack_from('<I', d, k)[0]
            if v == 0:
                break
            if not (v & 0x80000000):
                fo = pe.r2o(v)
                fe = d.index(b'\0', fo + 2)
                entry_va = pe.imagebase + _rva_of_raw(pe, slot) + idx * 4
                out.append((dll, d[fo + 2:fe].decode('latin1'), entry_va))
            k += 4
            idx += 1
        o += 20
    return out


def _rva_of_raw(pe, raw):
    for s in pe.secs:
        if s[3] <= raw < s[3] + s[4]:
            return s[1] + (raw - s[3])
    return 0


def text_bytes(pe):
    for s in pe.secs:
        if s[0] == '.text':
            return pe.d[s[3]:s[3] + s[4]], pe.imagebase + s[1]
    return b'', 0


def push_imm_hits(pe, idset):
    tb, base = text_bytes(pe)
    hits = []
    for m in re.finditer(rb'\x68(....)', tb, re.S):
        imm = struct.unpack_from('<I', m.group(1))[0]
        if imm in idset:
            hits.append((base + m.start(), imm))
    return hits


def iat_call_hits(pe, iat_vas):
    tb, base = text_bytes(pe)
    hits = []
    for va in iat_vas:
        pat = b'\xff\x15' + struct.pack('<I', va)
        i = 0
        while True:
            j = tb.find(pat, i)
            if j < 0:
                break
            hits.append((base + j, va))
            i = j + 1
    return hits


def align4(x):
    return (x + 3) & ~3


def varstr(blob, o):
    if o + 2 > len(blob):
        return None, o + 2
    w = struct.unpack_from('<H', blob, o)[0]
    if w == 0:
        return None, o + 2
    if w == 0xFFFF:
        return ('ord', struct.unpack_from('<H', blob, o + 2)[0]), o + 4
    e = o
    while e + 2 <= len(blob) and struct.unpack_from('<H', blob, e)[0] != 0:
        e += 2
    return blob[o:e].decode('utf-16-le', 'ignore'), e + 2


def parse_dialog(blob):
    o = 0
    ex = False
    style = 0
    if len(blob) >= 4 and struct.unpack_from('<HH', blob, 0) == (1, 0xFFFF):
        ex = True
        o = 8
        o += 4                                     # exStyle
        style = struct.unpack_from('<I', blob, o)[0]; o += 4
    else:
        style = struct.unpack_from('<I', blob, o)[0]; o += 4
        o += 4
    cdit = struct.unpack_from('<H', blob, o)[0]; o += 2
    o += 8
    _m, o = varstr(blob, o)
    _c, o = varstr(blob, o)
    _t, o = varstr(blob, o)
    if style & 0x40:
        o += 2
        _f, o = varstr(blob, o)
    items = []
    for _ in range(cdit):
        o = align4(o)
        if ex:
            o += 12
            o += 8
            cid = struct.unpack_from('<I', blob, o)[0]; o += 4
        else:
            o += 16
            cid = struct.unpack_from('<H', blob, o)[0]; o += 2
        cls, o = varstr(blob, o)
        txt, o = varstr(blob, o)
        cb = struct.unpack_from('<H', blob, o)[0] if o + 2 <= len(blob) else 0
        o += 2 + cb
        items.append((cid, cls, txt))
    return style, cdit, items


def parse_dlginit(blob):
    """DLGINIT = sequence of {WORD id; WORD msg; DWORD cb; BYTE data[cb]}, id==0 ends."""
    o = 0
    recs = []
    while o + 8 <= len(blob):
        cid, msg = struct.unpack_from('<HH', blob, o)
        cb = struct.unpack_from('<I', blob, o + 4)[0]
        o += 8
        if cid == 0 or cb > 0x1000 or o + cb > len(blob):
            break
        data = blob[o:o + cb]
        o += cb
        recs.append((cid, msg, data))
    return recs


def dis_window(pe, va, back=16, fwd=52):
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_OP_IMM
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    o = pe.va2o(va)
    if o is None:
        return
    blob = pe.d[max(0, o - back):o + fwd]
    for ins in md.disasm(blob, va - back):
        note = ''
        for op in ins.operands:
            if op.type == CS_OP_IMM:
                s = pe.cstr(op.imm & 0xFFFFFFFF)
                if s:
                    note = '   ; "%s"' % s
                break
        mark = ' <<<' if ins.address == va else ''
        print('      0x%08X  %-18s %s%s%s'
              % (ins.address, ins.bytes.hex(), ins.mnemonic + ' ' + ins.op_str,
                 note, mark))


def main(argv):
    path = argv[1]
    VALUED = ('--dialog', '--iat', '--imm-hits')
    opts, extra = {}, []
    i = 2
    while i < len(argv):
        a = argv[i]
        if a in VALUED:
            vals = []
            i += 1
            while i < len(argv) and not argv[i].startswith('--'):
                vals.append(argv[i])
                i += 1
            opts[a] = vals
            continue
        if a.startswith('--'):
            i += 1
            continue
        extra += [int(x, 0) for x in a.split(',') if x]
        i += 1
    def _ids(vals):
        s = set()
        for v in vals:
            s |= set(int(x, 0) for x in v.split(',') if x)
        return s

    want = _ids(opts.get('--dialog', [])) or None
    iat_pat = (opts.get('--iat') or [None])[0]
    imm_ids = _ids(opts.get('--imm-hits', []))

    pe = PE(path)
    quiet = '--quiet' in argv
    print('=== %s ===' % path)
    print('imagebase=0x%08X' % pe.imagebase)

    ent = rsrc_entries(pe)
    if quiet:
        ent = []
    print('\n--- 1) resources: %d ---' % len(ent))
    byt = {}
    for t, i, rva, size in ent:
        byt.setdefault(t, []).append((i, rva, size))

    def tkey(x):
        return (isinstance(x, str), x if isinstance(x, int) else 0, str(x))

    res_ids = set()
    for t in sorted(byt, key=tkey):
        ids = [x[0] for x in byt[t]]
        print('  type %-4s %-14s n=%-3d ids=%s'
              % (t, RT.get(t, '?') if isinstance(t, int) else str(t), len(ids),
                 ids[:24]))
        for x in ids:
            if isinstance(x, int):
                res_ids.add(x)

    print('\n--- 2) DIALOG / DLGINIT ---')
    for t, i, rva, size in ent:
        if t not in (5, 240):
            continue
        if want is not None and not (isinstance(i, int) and i in want):
            continue
        o = pe.r2o(rva)
        if o is None:
            continue
        blob = pe.d[o:o + min(size, 0x10000)]
        print('  [%s] id=%s size=0x%X rva=0x%X va=0x%08X'
              % (RT.get(t), i, size, rva, pe.imagebase + rva))
        if t == 5:
            try:
                style, cdit, items = parse_dialog(blob)
                print('      style=0x%08X cdit=%d' % (style, cdit))
                for cid, cls, txt in items:
                    cl = ('ord:%d' % cls[1]) if isinstance(cls, tuple) else (cls or '')
                    tx = txt if isinstance(txt, str) else (
                        '(ord:%d)' % txt[1] if isinstance(txt, tuple) else '')
                    print('        id=%s  %-12s "%s"'
                          % (('%d(0x%X)' % (cid, cid)) if isinstance(cid, int) else '?',
                             cl, tx))
            except Exception as e:                # noqa: BLE001
                print('      parse failed: %s' % e)
        else:
            for cid, msg, data in parse_dlginit(blob):
                print('      ctrl=0x%X(%d) msg=0x%X cb=%d'
                      % (cid, cid, msg, len(data)))
                a = ansi_strs(data, 1, 60)
                if a:
                    print('        ascii: %s' % a)
                w = wide_strs(data, 3, 20)
                if w:
                    print('        wide : %s' % w)

    print('\n--- 3) import table: dialog/window APIs ---')
    rx = re.compile(r'dialog|propertysheet|window|resource|menu|messagebox|combo', re.I)
    ux = re.compile(r'user32|comctl32|comdlg32|ole32|shell32|mfc|afx', re.I)
    funcs = import_funcs(pe)
    for dll, fn, iat in funcs:
        if ux.search(dll) and rx.search(fn):
            print('  %-14s %-28s iat=0x%08X' % (dll, fn, iat))

    focus = set(TRACE_CTRL_IDS) | {100, 101, 102, 103, 104} | set(extra)
    idset = set(focus)
    if len(res_ids) <= 200 or want is not None:
        idset |= res_ids
    print('\n--- 4) push imm32 hits (focus=%d ids) ---' % len(focus))
    major = [(va, imm) for va, imm in push_imm_hits(pe, idset) if imm in focus]
    print('  hits: %d' % len(major))
    for va, imm in major:
        print('  ---- 0x%08X  push 0x%X (%d) ----' % (va, imm, imm))
        dis_window(pe, va)

    if iat_pat:
        target = [(dll, fn, iat) for dll, fn, iat in funcs
                  if iat_pat.lower() in fn.lower()]
        print('\n--- 5) call [IAT] hits for "%s" (%d import(s)) ---'
              % (iat_pat, len(target)))
        for dll, fn, iat in target:
            print('  import %s!%s  iat=0x%08X' % (dll, fn, iat))
        hits = iat_call_hits(pe, [x[2] for x in target])
        print('  call sites: %d' % len(hits))
        for va, iat in hits[:60]:
            print('  ---- 0x%08X  call [0x%08X] ----' % (va, iat))
            dis_window(pe, va, 20, 24)

    if imm_ids:
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_OP_IMM
        tb, base = text_bytes(pe)
        md = Cs(CS_ARCH_X86, CS_MODE_32)
        md.detail = True
        print('\n--- 6) linear scan: immediate == %s ---'
              % ['0x%X' % x for x in sorted(imm_ids)])
        n = 0
        for ins in md.disasm(tb, base):
            if not ins.operands:
                continue
            for op in ins.operands:
                if op.type == CS_OP_IMM and op.imm in imm_ids:
                    print('  0x%08X  %-18s %s'
                          % (ins.address, ins.bytes.hex(),
                             ins.mnemonic + ' ' + ins.op_str))
                    n += 1
                    break
            if n >= 150:
                print('  ... (truncated)')
                break
        print('  total shown: %d' % n)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
