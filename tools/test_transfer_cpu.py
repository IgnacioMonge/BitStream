#!/usr/bin/env python3
"""Run transfer regressions against the linked Z80 application, without hardware.

Requires fresh build/BitStream_{divTiesus,Next,AY,Spectranext}.{tap,map}.
Only firmware, transport, filesystem and UI seams named below are mocked;
the FTP decisions, filename selection and storage close implementations run
as compiled. No boot code or ROM is executed. Interrupts stay disabled.
"""
import argparse
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VARIANTS = ('divTiesus', 'Next', 'AY', 'Spectranext')
ENTRY, RESULT, CREATED, REMOVED, EXISTS = 0x5B00, 0x5000, 0x5100, 0x5120, 0x5200


def symbols(path):
    return {name: int(value, 16) for name, value in re.findall(
        r'^([^\s=]+)\s*=\s*\$([0-9A-Fa-f]+)', path.read_text(), re.M)}


def load_tap(path):
    mem = bytearray(65536)
    data, pos, header, loaded = path.read_bytes(), 0, None, 0
    while pos < len(data):
        assert pos + 2 <= len(data), 'truncated TAP length'
        length = int.from_bytes(data[pos:pos + 2], 'little')
        block = data[pos + 2:pos + 2 + length]
        assert len(block) == length and length >= 2, 'truncated TAP block'
        checksum = 0
        for b in block:
            checksum ^= b
        assert checksum == 0, 'bad TAP checksum'
        if block[0] == 0:
            assert length == 19, 'invalid TAP header'
            header = block
        elif block[0] == 255 and header is not None:
            if header[1] == 3:
                size = int.from_bytes(header[12:14], 'little')
                address = int.from_bytes(header[14:16], 'little')
                assert len(block) == size + 2 and address + size <= 65536
                mem[address:address + size] = block[1:-1]
                loaded += 1
            header = None
        pos += length + 2
    assert loaded == 1, f'expected one CODE block, found {loaded}'
    return mem


def tool(name):
    found = shutil.which(name)
    fallback = Path('C:/z88dk/bin') / (name + '.exe')
    assert found or fallback.is_file(), f'{name} missing (install z88dk or set PATH)'
    return found or str(fallback)


class CPU:
    def __init__(self, variant):
        base = ROOT / 'build' / ('BitStream_' + variant)
        self.variant = variant
        self.sym = symbols(base.with_suffix('.map'))
        self.memory = load_tap(base.with_suffix('.tap'))
        self.sp = self.sym['__register_sp']

    def run(self, body, mocks='', patches=None, setup=None):
        defs = '\n'.join(f'DEFC {k} = {v}' for k, v in self.sym.items())
        asm = f'''SECTION code_user
ORG {ENTRY}
{defs}
entry:
    di
    ld sp, {self.sp}
{body}
    ld ({RESULT + 2}), sp
    ld a, 0xA7
    ld ({RESULT + 4}), a
    jp 0
{mocks}
'''
        with tempfile.TemporaryDirectory(prefix='bs-transfer-') as t:
            tmp = Path(t)
            (tmp / 'h.asm').write_text(asm, encoding='ascii')
            out = tmp / 'out.bin'
            r = subprocess.run([tool('z88dk-z80asm'), '-b', '-m', '-o' + str(out),
                                str(tmp / 'h.asm')], capture_output=True, text=True)
            assert r.returncode == 0, r.stdout + r.stderr
            code = (tmp / 'out_code_user.bin').read_bytes()
            assert ENTRY + len(code) <= 24000, 'harness overlaps application'
            labels = symbols(tmp / 'out.map')
            mem = self.memory.copy()
            mem[ENTRY:ENTRY + len(code)] = code
            for target, replacement in (patches or {}).items():
                at = self.sym[target]
                dest = labels[replacement]
                mem[at:at + 3] = bytes((0xC3, dest & 255, dest >> 8))
            if setup:
                setup(mem)
            guards = (self.sp - 528, self.sp)
            for at in guards:
                mem[at:at + 16] = bytes([0xD3]) * 16
            (tmp / 'in.bin').write_bytes(mem)
            r = subprocess.run([tool('z88dk-ticks'), '-pc', f'{ENTRY:04X}',
                                '-start', f'{ENTRY:04X}', '-end', '0000',
                                '-counter', '4000000', '-output', str(tmp / 'after.bin'),
                                str(tmp / 'in.bin')], capture_output=True, text=True, timeout=30)
            assert r.returncode == 0, r.stdout + r.stderr
            after = (tmp / 'after.bin').read_bytes()[:65536]
            assert after[RESULT + 4] == 0xA7, 'did not return to harness before cycle limit'
            assert word(after, RESULT + 2) == self.sp, 'unbalanced caller stack'
            for at in guards:
                assert after[at:at + 16] == bytes([0xD3]) * 16, f'stack guard {at:04X}'
            return after


def word(mem, addr):
    return int.from_bytes(mem[addr:addr + 2], 'little')


def cstr(mem, addr):
    return bytes(mem[addr:mem.index(0, addr)])


def call_stack(name, args):
    return ''.join(f'    ld hl, {a}\n    push hl\n' for a in args) + \
        f'    call {name}\n' + '    pop bc\n' * len(args)


UI_MOCKS = '''mock_ret:
    ret
mock_fail:
    ld a, (0x5010)
    inc a
    ld (0x5010), a
    ret
'''
UI_PATCHES = {'_fail': 'mock_fail', '_print_reply': 'mock_ret',
              '_connection_lost': 'mock_ret'}


def test_final_reply(cpu):
    codes = (0, 1, 2, 150, 200, 225, 226, 227, 250, 251, 300, 350, 400, 421, 426, 500, 550)
    for code in codes:
        a = cpu.run(f'''    ld hl, {code}
    call _ftp_transfer_failed
    ld ({RESULT}), hl
''', UI_MOCKS, UI_PATCHES)
        assert a[RESULT] == (0 if code in (226, 250) else 1), f'final reply {code}'
        assert a[cpu.sym['_g_user_cancel']] == (code == 1), f'cancel state for {code}'
    print(f'PASS {cpu.variant}: only 226/250 complete; cancellation recorded')


def test_close(cpu):
    if cpu.variant == 'Spectranext':
        for error in (0, 1):
            mocks = f'''mock_close:
    ld a, h
    cp 0x3E
    jr nz, unexpected_close
    ld a, l
    cp 0xD2
    jr nz, unexpected_close
    ld a, (0x5011)
    inc a
    ld (0x5011), a
    ld a, (_spxn_regs)
    ld (0x5012), a
    ld hl, {error}
    ret
unexpected_close:
    ld hl, 0xEE
    ret
'''
            a = cpu.run(f'    ld hl, 7\n    call _fs_close\n    ld ({RESULT}), hl\n',
                        mocks, {'_spxn_rom_hlcall': 'mock_close'})
            assert a[RESULT] == 1 - error, f'XFS VCLOSE error={error}'
            assert a[0x5011:0x5013] == bytes((1, 7)), 'VCLOSE must close original fd once'
    else:
        for sync_error, close_error in ((0, 0), (1, 0), (0, 1), (1, 1)):
            # RST 8 consumes the inline esxDOS selector, then emulates firmware C.
            mocks = f'''mock_rst:
    pop hl
    ld c, (hl)
    inc hl
    push hl
    ld (0x5013), a
    ld a, c
    cp 0x9C
    jr z, mock_sync
    cp 0x9B
    jr nz, bad_selector
    ld a, (0x5012)
    inc a
    ld (0x5012), a
    ld a, {close_error}
    jr mock_flag
mock_sync:
    ld a, (0x5011)
    inc a
    ld (0x5011), a
    ld a, {sync_error}
mock_flag:
    rrca
    ret
bad_selector:
    ld a, 0xEE
    ld (0x5012), a
    scf
    ret
'''
            # _fs_close is untouched; RST 8 itself is the firmware boundary.
            old = cpu.sym.get('firmware_rst')
            cpu.sym['firmware_rst'] = 8
            try:
                a = cpu.run(f'    ld hl, 7\n    call _fs_close\n    ld ({RESULT}), hl\n',
                            mocks, {'firmware_rst': 'mock_rst'})
            finally:
                if old is None:
                    del cpu.sym['firmware_rst']
                else:
                    cpu.sym['firmware_rst'] = old
            assert a[RESULT] == int(not (sync_error or close_error)), \
                f'esxDOS sync={sync_error}, close={close_error}'
            assert a[0x5011:0x5014] == bytes((1, 1, 7)), 'must SYNC then CLOSE original handle'
    print(f'PASS {cpu.variant}: close status and firmware error propagation')


FS_MOCKS = f'''mock_exists:
    push hl
    ld de, 6
    add hl, de
    ld a, (hl)
    cp '~'
    ld e, 0
    jr nz, exists_index
    inc hl
    ld a, (hl)
    sub '0'
    ld e, a
exists_index:
    ld d, 0
    ld hl, {EXISTS}
    add hl, de
    ld a, (hl)
    pop hl
    ld l, a
    ld h, 0
    ret
mock_create:
    ld de, {CREATED}
    ld bc, 13
    ldir
    ld a, (0x5011)
    inc a
    ld (0x5011), a
    ld hl, 7
    ret
mock_remove:
    ld de, {REMOVED}
    ld bc, 13
    ldir
    ld a, (0x5012)
    inc a
    ld (0x5012), a
    ret
mock_yes:
    ld hl, 1
    ret
mock_eof:
    ld hl, 65535
    ret
mock_zero32:
    ld hl, 0
    ld de, 0
    ret
mock_commit:
    ld a, (0x5013)
    inc a
    ld (0x5013), a
    ld hl, 1
    ret
mock_close_result:
    ld hl, (0x5014)
    ret
'''


def transfer_patches(cpu):
    patches = dict(UI_PATCHES, _fs_exists='mock_exists', _fs_create='mock_create',
                   _fs_remove='mock_remove', _fs_close='mock_close_result',
                   _ftp_transfer_begin='mock_yes', _ftp_xfer_read='mock_eof',
                   _net_data_close='mock_ret')
    for name in ('_drain_mode_fast', '_drain_mode_normal', '_main_print', '_draw_progress_bar'):
        if name in cpu.sym:
            patches[name] = 'mock_ret'
    return patches


def test_spool(cpu):
    patches = transfer_patches(cpu)
    for occupied in (0, 1, 2, 9, 10):
        for close_ok in (0, 1):
            def setup(mem):
                mem[cpu.sym['_fs_available']] = 1
                mem[EXISTS:EXISTS + occupied] = bytes([1]) * occupied
                mem[0x5014] = close_ok
            a = cpu.run(f'    call _list_spool\n    ld ({RESULT}), hl\n',
                        UI_MOCKS + FS_MOCKS, patches, setup)
            if occupied == 10:
                assert a[RESULT] == 0 and a[0x5011] == a[0x5012] == 0, 'exhausted names must refuse safely'
            else:
                name = b'BSLIST.TMP' if occupied == 0 else f'BSLIST~{occupied}.TMP'.encode()
                assert cstr(a, CREATED) == name, f'collision {occupied}: {cstr(a, CREATED)!r}'
                assert a[RESULT] == close_ok and a[0x5011] == 1, 'LIST must reject close failure'
                assert a[0x5012] == 1 - close_ok
                if not close_ok:
                    assert cstr(a, REMOVED) == name, 'remove only the newly created spool'
    # A previous suffixed name must not become the starting name next call.
    body = f'''    call _list_spool
    xor a
    ld ({EXISTS}), a
    call _list_spool
    ld ({RESULT}), hl
'''
    def setup(mem):
        mem[cpu.sym['_fs_available']] = mem[EXISTS] = mem[0x5014] = 1
    a = cpu.run(body, UI_MOCKS + FS_MOCKS, patches, setup)
    assert cstr(a, CREATED) == b'BSLIST.TMP' and a[0x5011] == 2, 'reset temporary name each call'
    patches['_ftp_transfer_begin'] = 'mock_zero32'
    a = cpu.run(f'    call _list_spool\n    ld ({RESULT}), hl\n',
                UI_MOCKS + FS_MOCKS, patches, setup)
    assert a[RESULT] == 0 and a[0x5011] == a[0x5012] == 1
    assert cstr(a, CREATED) == cstr(a, REMOVED) == b'BSLIST~1.TMP', 'failed PASV removes only owned spool'
    assert a[EXISTS] == 1, 'pre-existing BSLIST.TMP remains'
    print(f'PASS {cpu.variant}: spool collision/exhaustion/reset and close failure')


def test_download_close(cpu):
    patches = transfer_patches(cpu)
    patches.update(_download_request_size='mock_zero32', _fs_commit='mock_commit',
                   _ftp_transfer_end='mock_final')
    for close_ok, final_code in ((0, 226), (1, 0), (1, 1), (1, 200), (1, 226), (1, 250)):
        def setup(mem):
            mem[0x5300:0x5306] = b'X.TXT\0'
            mem[0x5014] = close_ok
        body = call_stack('_download_file_core', (0x5300, 0x5300, 1, 1, 0x5340))
        mocks = UI_MOCKS + FS_MOCKS + f'mock_final:\n    ld hl, {final_code}\n    ret\n'
        a = cpu.run(body + f'    ld ({RESULT}), hl\n', mocks, patches, setup)
        success = int(close_ok and final_code in (226, 250))
        assert a[RESULT] == success, f'GET close={close_ok}, final={final_code}'
        assert a[0x5013] == success, 'commit only after verified close and final reply'
        assert a[0x5012] == 1 - success, f'remove failed GET: close={close_ok}, final={final_code}'
        assert a[cpu.sym['_g_user_cancel']] == (final_code == 1)
        assert word(a, 0x5340) == 0 and word(a, 0x5342) == 0
    print(f'PASS {cpu.variant}: GET close/final-reply failure removes partial without commit')


def test_recv(cpu):
    # Raw Z80 F: Z=0x40, N=0x02. C reports errors; idle Z ignores stale BC events.
    cases = ((0x40, 4, 0, 0), (0x41, 4, 65535, 0), (0x02, 4, 3, 1),
             (0, 6, 3, 1), (1, 4, 65535, 0), (0, 2, 65535, 0), (0, 0, 0, 0))
    for raw_flags, events, expected, recvs in cases:
        mocks = f'''mock_rom:
    ld a, l
    cp 0x24
    jr nz, mock_recv
    ld hl, {events}
    ld (_spxn_regs + 1), hl
    ld hl, {raw_flags}
    ret
mock_recv:
    ld a, (0x5011)
    inc a
    ld (0x5011), a
    ld hl, (_spxn_regs + 3)
    ld (hl), 0x61
    inc hl
    ld (hl), 0x62
    inc hl
    ld (hl), 0x63
    ld hl, 3
    ld (_spxn_regs + 1), hl
    ld hl, 0
    ret
'''
        def setup(mem):
            mem[cpu.sym['_nx_fd']] = 7
            mem[cpu.sym['_nx_hup']] = 0
            mem[0x5300:0x5305] = b'\xCC' * 5
        body = call_stack('_nx_recv', (0, 0x5301, 3)) + f'    ld ({RESULT}), hl\n'
        a = cpu.run(body, mocks, {'_spxn_rom_hlcall': 'mock_rom'}, setup)
        assert word(a, RESULT) == expected and a[0x5011] == recvs, \
            f'POLLFD raw F={raw_flags:02X}, events={events:02X}'
        assert a[cpu.sym['_nx_hup']] == (expected == 65535)
        assert a[0x5300] == a[0x5304] == 0xCC, 'receive buffer guards'
        assert a[0x5301:0x5304] == (b'abc' if recvs else b'\xCC' * 3)
    print('PASS Spectranext: raw Z/N/C flags, POLLIN|HUP drain and buffer guards')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--variant', choices=VARIANTS, action='append', help='default: all four builds')
    args = parser.parse_args()
    for variant in args.variant or VARIANTS:
        cpu = CPU(variant)
        test_final_reply(cpu)
        test_close(cpu)
        test_download_close(cpu)
        if variant in ('divTiesus', 'Next'):
            test_spool(cpu)
        if variant == 'Spectranext':
            test_recv(cpu)
    print('ALL TRANSFER CPU TESTS PASSED (mocked hardware; not hardware timing validation)')


if __name__ == '__main__':
    main()
