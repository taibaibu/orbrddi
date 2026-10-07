#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""patch_agdi_etb.py — 放开 `CMSIS_AGDI.dll` 的 Trace Port 下拉（含 "Embedded Trace Buffer"）

依据 / 结论见 `COMPAT_ANALYSIS.md` §17.9 (7)。
AGDI 在两个函数里各硬编了一处 `EnableWindow(0x456 /*Trace Port 组合框*/, FALSE)`：

    函数 A  0x1003B2A0（Trace 页可控态刷新；本页所有控件处理器的 tail-jmp 目标）
        0x1003B39A  6A 00  ->  6A 01   （文件偏移 0x03A79A）
    函数 B  0x1003B610（把设置灌进对话框；OnInitDialog 尾段调用）
        0x1003BA8D  6A 00  ->  6A 01   （文件偏移 0x03AE8D）

两处**必须同时**改：只改一处，另一次刷新会把下拉重新灰掉。
改完 `push 0` -> `push 1`，即 `EnableWindow(combo, TRUE)`，6 个 Trace Port 选项
（index 5 = "Embedded Trace Buffer"）即可点选。

⚠️ 只放开 UI，不建数据通路：RDDI 层（本 ORBMDK）没有 ETM/ETB 采集接口（§17.9 (4)），
   选中 ETB 后能否真正取到数据是另一回事。

用法（默认**只检查**，不改盘）：
    python tools/patch_agdi_etb.py                    # 检查当前状态
    python tools/patch_agdi_etb.py --apply            # 备份 .bak + 打补丁
    python tools/patch_agdi_etb.py --revert           # 从 .bak 还原
    python tools/patch_agdi_etb.py --dll <CMSIS_AGDI.dll 路径>
"""

import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pe_re  # noqa: E402

DEFAULT_DLL = os.environ.get('CMSIS_AGDI_DLL',
                             r'D:\Keil_v5\ARM\BIN\CMSIS_AGDI.dll')

# (VA, 文件偏移, 原字节, 新字节)；两处后面都紧跟 `68 56 04 00 00`(push 0x456) 以自校验
PATCHES = [
    (0x1003B39A, 0x03A79A, b'\x6A\x00', b'\x6A\x01'),
    (0x1003BA8D, 0x03AE8D, b'\x6A\x00', b'\x6A\x01'),
]
FOLLOW = b'\x68\x56\x04\x00\x00'  # push 0x456


def _status(data):
    """返回 [(va, off, 'orig'|'patched'|'unknown'), ...]"""
    out = []
    for va, off, old, new in PATCHES:
        cur = data[off:off + len(old)]
        tail = data[off + len(old):off + len(old) + len(FOLLOW)]
        tail_ok = (tail == FOLLOW)
        if cur == new and tail_ok:
            st = 'patched'
        elif cur == old and tail_ok:
            st = 'orig'
        else:
            st = 'unknown'
        out.append((va, off, st))
    return out


def check(dll):
    if not os.path.isfile(dll):
        print('找不到 DLL: %s' % dll)
        return 1, None
    with open(dll, 'rb') as f:
        data = f.read()
    # VA->offset 一致性校验（防换版本）
    try:
        pe = pe_re.PE(dll)
    except Exception as e:  # noqa: BLE001
        print('PE 解析失败: %s' % e)
        return 1, None
    bad = []
    for va, off, _old, _new in PATCHES:
        calc = pe.va2o(va)
        if calc != off:
            bad.append((va, off, calc))
    if bad:
        print('⚠️ 版本不匹配（VA->偏移 与预期不符），**不要**打补丁：')
        for va, off, calc in bad:
            print('    VA 0x%08X 期望 file 0x%06X，实际 0x%06X' % (va, off, calc or -1))
        return 1, None
    st = _status(data)
    print('目标: %s' % dll)
    ok = True
    for va, off, s in st:
        label = {'orig': '未打补丁 (6A 00)',
                 'patched': '已打补丁 (6A 01)',
                 'unknown': '★ 字节不符（既非 6A 00 也非 6A 01）'}[s]
        print('  VA 0x%08X  file 0x%06X  ->  %s' % (va, off, label))
        if s == 'unknown':
            ok = False
    bak = dll + '.bak'
    print('  备份 %s: %s' % (bak, '存在' if os.path.isfile(bak) else '不存在'))
    return (0 if ok else 1), data


def apply_patch(dll):
    rc, data = check(dll)
    if data is None:
        return 1
    st = _status(data)
    if all(s == 'patched' for _v, _o, s in st):
        print('已是补丁后状态，无需改动。')
        return 0
    if any(s == 'unknown' for _v, _o, s in st):
        print('存在未知字节，拒绝打补丁。')
        return 1
    bak = dll + '.bak'
    if not os.path.isfile(bak):
        shutil.copy2(dll, bak)
        print('已备份 -> %s' % bak)
    else:
        print('备份已存在（保留原备份）: %s' % bak)
    buf = bytearray(data)
    for _va, off, old, new in PATCHES:
        if buf[off:off + len(old)] == new:
            continue
        assert buf[off:off + len(old)] == old
        buf[off:off + len(new)] = new
    tmp = dll + '.new'
    with open(tmp, 'wb') as f:
        f.write(buf)
    shutil.move(tmp, dll)
    print('已打补丁（2 处 6A 00 -> 6A 01）。回退：--revert')
    return 0


def revert(dll):
    bak = dll + '.bak'
    if not os.path.isfile(bak):
        print('没有备份文件可还原: %s' % bak)
        return 1
    shutil.copy2(bak, dll)
    print('已从 %s 还原。' % bak)
    check(dll)
    return 0


def main(argv):
    dll = DEFAULT_DLL
    action = 'check'
    if '--dll' in argv:
        i = argv.index('--dll')
        if i + 1 < len(argv):
            dll = argv[i + 1]
    if '--apply' in argv:
        action = 'apply'
    elif '--revert' in argv:
        action = 'revert'
    if action == 'apply':
        return apply_patch(dll)
    if action == 'revert':
        return revert(dll)
    rc, _ = check(dll)
    return rc


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
