#!/usr/bin/env python3
"""Verify the linked bow font in text, paired lines, input and status labels."""
import argparse
import hashlib
from pathlib import Path

from test_asm_cpu import font_rows, screen_addr
from test_transfer_cpu import CPU, ROOT, VARIANTS, call_stack

FONT_SHA = 'b297ba3af5a5ef1602b6a1527beb5e63bafd32e1c334112879562eb5441ef2f6'
TEXT = 0x5300


def check_char(memory, row, col, char, double=False):
    glyph = font_rows(ord(char))
    rows = ([0, 0] + [bits for bits in glyph for _ in range(2)] + [0, 0]
            if double else [0] + glyph + [0])
    mask = 0x0F if col & 1 else 0xF0
    for scan, bits in enumerate(rows):
        address = screen_addr(row + scan // 8, scan % 8) + col // 2
        assert memory[address] & mask == bits & mask, f'{char}: row={row} col={col} scan={scan}'


def test_font(cpu):
    at = cpu.sym['font64_packed']
    assert hashlib.sha256(cpu.memory[at:at + 288]).hexdigest() == FONT_SHA, 'linked font is not bow'
    assert cpu.memory[at + 285:at + 288] == b'\xFF' * 3, 'progress glyph must stay solid'
    body = '''    ld a, 255
    ld (_cache_row_y), a
    ld a, 3
    ld (_g_ps64_y), a
    xor a
    ld (_g_ps64_col), a
    ld a, 0x47
    ld (_g_ps64_attr), a
    ld l, 98
    call _print_str64_char
'''
    body += call_stack('_print_line64_fast', (5, TEXT, 0x47))
    # This input renderer consumes its four stack arguments (callee ABI).
    body += ''.join(f'    ld hl, {arg}\n    push hl\n' for arg in (22, 5, ord('g'), 0x47))
    body += '    call _put_char64_input_cached\n'
    body += '''    ld a, 20
    ld (_g_ps64_y), a
    ld a, 2
    ld (_g_ps64_col), a
    ld l, 65
    call _draw_big_char
'''

    def setup(memory):
        memory[0x4000:0x5B00] = b'\x96' * 0x1B00
        memory[TEXT:TEXT + 3] = b'Ab\0'
        memory[cpu.sym['_input_cache_char'] + 5] = 0xFF

    after = cpu.run(body, setup=setup)
    for row, col, char, double in ((3, 0, 'b', False), (5, 0, 'A', False),
                                   (5, 1, 'b', False), (22, 5, 'g', False),
                                   (20, 2, 'A', True)):
        check_char(after, row, col, char, double)
    assert after[cpu.sym['_input_cache_char'] + 5] == ord('g')
    print(f'PASS {cpu.variant}: linked bow glyphs in text, paired lines, input and double-height status; solid progress block')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build')
    parser.add_argument('--variant', choices=VARIANTS, action='append')
    args = parser.parse_args()
    for variant in args.variant or VARIANTS:
        test_font(CPU(variant, args.build_dir))
    print('ALL BASE FONT CPU TESTS PASSED (physical display untested)')


if __name__ == '__main__':
    main()
