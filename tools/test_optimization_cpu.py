#!/usr/bin/env python3
"""Linked-TAP regressions and deterministic measurements for format_size / CRT.

No ROM or physical device is emulated. CRT runs from __Start to a mocked _main;
the incoming BASIC state is SP, IY=$5C3A, IM1, and enabled interrupts. The real
frame-clock initializer and ISR run, but the ISR is explicitly called: external
interrupt delivery, contention, firmware and full application boot are untested.
Format batches and the 128-call timing fixture use identical harness overhead
for every snapshot. Reported T-states include this fixed harness overhead.
"""
import argparse
from pathlib import Path

from test_transfer_cpu import CPU, ROOT, VARIANTS, word

TABLE, OUTPUT, SLOT, BATCH = 0x4800, 0x5100, 12, 128
STATE = 0x5010


def expected_size(value):
    if value >= 1048576:
        return f'{value >> 20}.{(((value & 0xFFFFF) >> 17) * 10) >> 3}MB'.encode()
    if value >= 1024:
        return f'{value >> 10}KB'.encode()
    return f'{value}B'.encode()


def format_batch(cpu, values):
    # sccz80 caller-cleaned: push long high, long low, then the buffer pointer.
    body = f'''    ld hl, {TABLE}
    ld ({STATE}), hl
    ld hl, {OUTPUT + 1}
    ld ({STATE + 2}), hl
    ld a, {len(values)}
    ld ({STATE + 4}), a
format_loop:
    ld hl, ({STATE})
    ld e, (hl)
    inc hl
    ld d, (hl)
    inc hl
    push de
    ld e, (hl)
    inc hl
    ld d, (hl)
    inc hl
    push de
    ld ({STATE}), hl
    ld hl, ({STATE + 2})
    push hl
    call _format_size
    pop bc
    pop bc
    pop bc
    ld hl, ({STATE + 2})
    ld de, {SLOT}
    add hl, de
    ld ({STATE + 2}), hl
    ld a, ({STATE + 4})
    dec a
    ld ({STATE + 4}), a
    jp nz, format_loop
'''
    def setup(mem):
        encoded = b''.join((v >> 16).to_bytes(2, 'little') +
                           (v & 65535).to_bytes(2, 'little') for v in values)
        mem[TABLE:TABLE + len(encoded)] = encoded
        mem[OUTPUT:OUTPUT + len(values) * SLOT] = b'\xA5' * (len(values) * SLOT)
    after = cpu.run(body, setup=setup)
    for index, value in enumerate(values):
        at = OUTPUT + index * SLOT
        expected = expected_size(value) + b'\0'
        assert after[at] == 0xA5, f'{value}: leading output guard'
        assert after[at + 1:at + 1 + len(expected)] == expected, \
            f'{value}: expected {expected!r}, got {bytes(after[at + 1:at + SLOT])!r}'
        assert after[at + 1 + len(expected):at + SLOT] == \
            b'\xA5' * (SLOT - 1 - len(expected)), f'{value}: trailing output guard'
    return cpu.cycles


def test_format(cpu):
    # Exhaust every integer which can reach a decimal conversion in each unit.
    values = set(range(1024))
    values.update(v << 10 for v in range(1, 1024))
    values.update(v << 20 for v in range(1, 4096))
    for boundary in (1024, 1048576, 0x100000000):
        values.update(v for v in (boundary - 1, boundary, boundary + 1)
                      if 0 <= v <= 0xFFFFFFFF)
    # Preserve the existing eighth-MB approximation, including its jump points.
    for whole in (1, 9, 10, 99, 100, 999, 1000, 4095):
        for eighth in range(9):
            boundary = (whole << 20) + (eighth << 17)
            values.update(v for v in (boundary - 1, boundary, boundary + 1)
                          if 1048576 <= v <= 0xFFFFFFFF)
    values = sorted(values)
    for start in range(0, len(values), BATCH):
        format_batch(cpu, values[start:start + BATCH])
    fixture = (0, 1, 9, 10, 99, 100, 1023, 1024, 1025, 10239, 10240,
               1048575, 1048576, 1179647, 1179648, 0xFFFFFFFF) * 8
    cycles = format_batch(cpu, fixture)
    print(f'PASS {cpu.variant}: format {len(values)} values; '
          f'128-call fixture={cycles} T (fixed harness included)')


def snapshot(prefix, offset):
    return f'''    ld ({STATE + offset}), sp
    ld ({STATE + offset + 2}), iy
    ld a, i
    ld ({STATE + offset + 4}), a
    push af
    pop hl
    ld ({STATE + offset + 5}), hl
'''


def test_crt(cpu):
    sym = cpu.sym
    assert sym['__crt_model'] == 0, 'test expects the flat RAM CRT model'
    assert sym['__crt_enable_commandline'] == 0, 'unexpected main arguments'
    assert sym['__crt_interrupt_mode'] == 0xFFFFFFFF, 'expected inherited IM1'
    assert sym['__crt_org_vector_table'] == 0, 'unexpected CRT vector table'
    assert sym['__crt_enable_eidi'] == 1, 'expected CRT DI at main entry'
    assert sym['__crt_initialize_bss'] == 1, 'BSS initialization disabled'
    start, end = sym['__BSS_head'], sym['__BSS_UNINITIALIZED_head']
    assert start < end < cpu.sp - 528, 'BSS must not overlap guarded stack'
    data_start, data_end = sym['__data_compiler_head'], sym['__data_compiler_tail']
    initial_data = cpu.memory[data_start:data_end]
    for name, expected in (('_last_conn_state', 255), ('_force_status_redraw', 1)):
        assert cpu.memory[sym[name]] == expected, f'bad initialized fixture: {name}'
    incoming_sp = cpu.sp - 32
    body = f'''    ld sp, {incoming_sp}
    ld iy, 0x5C3A
    ld a, 0x55
    ld i, a
    im 1
    ei
    jp __Start
startup_done:
    di
    ld sp, {cpu.sp}
'''
    mocks = 'mock_main:\n' + snapshot('main', 0)
    has_clock = cpu.variant != 'Spectranext'
    if has_clock:
        # Read IFF2 using LD A,I immediately after the actual initializer.
        mocks += '    call _frame_clock_init\n' + snapshot('clock', 8)
        mocks += f'''    ld hl, (0xFCFF)
    ld (invoke_isr + 1), hl
    ld hl, 0xBEEF
invoke_isr:
    call 0
    ld ({STATE + 16}), hl
    ld ({STATE + 18}), iy
'''
    mocks += '    jp startup_done\n'

    def setup(mem):
        mem[start:end] = b'\xA5' * (end - start)
        mem[0xFCFE:0xFD02] = b'\xD7' * 4
        mem[0x5C78:0x5C7B] = bytes((255, 255, 0xA3))

    after = cpu.run(body, mocks, {'_main': 'mock_main'}, setup)
    assert word(after, STATE) == cpu.sp - 2, 'CRT main entry SP'
    assert word(after, STATE + 2) == 0x5C3A, 'CRT must preserve BASIC IY'
    assert after[STATE + 4] == 0x55, 'CRT must preserve inherited I'
    assert not after[STATE + 5] & 4, 'CRT main entry interrupts must be disabled'
    assert word(after, sym['__sp_or_ret']) == incoming_sp - 4, 'saved BASIC SP'
    for section in ('compiler', 'user'):
        lo, hi = sym[f'__bss_{section}_head'], sym[f'__bss_{section}_tail']
        assert after[lo:hi] == bytes(hi - lo), f'application BSS {section} not zero'
    assert after[data_start:data_end] == initial_data, 'initialized application data changed'
    if has_clock:
        assert word(after, STATE + 8) == cpu.sp - 2, 'frame init stack'
        assert word(after, STATE + 10) == 0x5C3A, 'frame init IY'
        assert after[STATE + 12] == 0xFC, 'frame init I register'
        assert after[STATE + 13] & 4, 'frame init must enable interrupts'
        assert word(after, 0xFCFF) == sym['frame_clock_isr'], 'IM2 vector'
        assert after[0xFCFE] == after[0xFD01] == 0xD7, 'vector guards'
        assert word(after, STATE + 16) == 0xBEEF, 'ISR must preserve HL'
        assert word(after, STATE + 18) == 0x5C3A, 'ISR must preserve IY'
        assert after[0x5C78:0x5C7B] == bytes((0, 0, 0xA3)), '16-bit FRAMES wrap'
        # CPU cannot read IM: assert the executed initializer contains IM 2.
        clock = sym['_frame_clock_init']
        isr = sym['frame_clock_isr']
        assert b'\xED\x5E' in cpu.memory[clock:isr], 'frame init IM 2 instruction'
    else:
        assert after[0xFCFE:0xFD02] == b'\xD7' * 4, 'Spectranext leaves IM2 vectors alone'
        assert after[0x5C78:0x5C7B] == bytes((255, 255, 0xA3)), 'CRT FRAMES unchanged'
    print(f'PASS {cpu.variant}: linked CRT to mocked main, poisoned BSS, data, '
          f'SP/IY/IFF2' + (', real frame init + called ISR' if has_clock else ', inherited ROM IM1') +
          f'; fixture={cpu.cycles} T (fixed harness included)')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build')
    parser.add_argument('--variant', choices=VARIANTS, action='append')
    parser.add_argument('--only', choices=('format', 'crt', 'all'), default='all')
    args = parser.parse_args()
    for variant in args.variant or VARIANTS:
        cpu = CPU(variant, args.build_dir)
        if args.only in ('format', 'all'):
            test_format(cpu)
        if args.only in ('crt', 'all'):
            test_crt(cpu)
    print('ALL OPTIMIZATION CPU TESTS PASSED (mocked main; no full boot/hardware proof)')


if __name__ == '__main__':
    main()
