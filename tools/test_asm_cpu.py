#!/usr/bin/env python3
"""Execute BitStream's real ASM kernels under z88dk-ticks (no hardware I/O).

Assembles asm/bitstream_asm.asm with a generated harness (z88dk-z80asm),
runs it in z88dk-ticks over a flat 64K image and checks:
  - utf8_to_ascii: continuation validation, Latin-1 fallback, ASCII-only output
  - main_puts BPE expander: out-of-dictionary tokens and nesting beyond the
    return stack terminate cleanly (no wild writes, SP intact)
  - main_print_asm: an embedded LF takes the slow path (two newlines)
  - rb_read_block: wrap-around, bounds, tail update
  - print_line64_fast == 64 x print_str64_char (blank pairs, NUL padding,
    control bytes, >= 0x80, both nibbles), attributes included
  - scroll_main_zone: exact geometry, nothing outside the zone touched, IFF kept

Usage: python tools/test_asm_cpu.py   (z88dk-z80asm / z88dk-ticks on PATH)
Timings exclude contention and interrupts.
"""
import random
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ASM = (ROOT / 'asm/bitstream_asm.asm').read_text(encoding='utf-8')

# Fixed addresses for the C globals the kernels reference
GLOBALS = {
    '_current_attr': 0xB000, '_main_col': 0xB001, '_main_line': 0xB002,
    '_rb_head': 0xB004, '_rb_tail': 0xB006, '_rx_pos': 0xB008,
    '_rx_overflow': 0xB009, '_rx_line': 0xB100, '_ring_buffer': 0xA000,
}
DRAIN_CALLS = 0xB00A
RESULT = 0xB010            # harness scratch (SP, flags, counts)


def screen_addr(row, scan):
    return 0x4000 + ((row & 24) << 8) + ((row & 7) << 5) + scan * 256


def run(body, setup=None, asm_text=ASM, pad=0, want_code=False):
    with tempfile.TemporaryDirectory(prefix='bs-cpu-') as t:
        tmp = Path(t)
        defs = ''.join(f'PUBLIC {k}\nDEFC {k} = 0x{v:04X}\n' for k, v in GLOBALS.items())
        harness = f'''SECTION code_user
ORG 0x8000
{defs}
PUBLIC _uart_drain_to_buffer
EXTERN _utf8_to_ascii, _main_puts, _main_print_asm, _rb_read_block
EXTERN _print_line64_fast, _print_str64_char, _scroll_main_zone
EXTERN _g_ps64_y, _g_ps64_col, _g_ps64_attr, _cache_row_y
entry:
    di
    ld sp, 0xFF00
{body}
    jp 0
    defs {pad}
_uart_drain_to_buffer:
    push hl
    ld hl, 0x{DRAIN_CALLS:04X}
    inc (hl)
    pop hl
    ret
SECTION bss_user
'''
        (tmp / 'h.asm').write_text(harness, encoding='utf-8')
        (tmp / 'k.asm').write_text(asm_text, encoding='utf-8')
        r = subprocess.run(['z88dk-z80asm', '-b', '-o' + str(tmp / 'out.bin'),
                            str(tmp / 'h.asm'), str(tmp / 'k.asm')],
                           capture_output=True, text=True)
        assert r.returncode == 0, r.stdout + r.stderr
        code = (tmp / 'out_code_user.bin').read_bytes()   # bss_user is not emitted
        mem = bytearray(65536)
        rnd = random.Random(7)
        mem[0x4000:0x5B00] = bytes(rnd.randrange(256) for _ in range(0x1B00))
        mem[0x8000:0x8000 + len(code)] = code
        if setup:
            setup(mem)
        before = bytes(mem)
        (tmp / 'in.bin').write_bytes(mem)
        r = subprocess.run(['z88dk-ticks', '-pc', '8000', '-start', '8000', '-end', '0000',
                            '-counter', '40000000', '-output', str(tmp / 'o.bin'), str(tmp / 'in.bin')],
                           capture_output=True, text=True, timeout=60)
        assert r.returncode == 0, r.stdout + r.stderr
        after = (tmp / 'o.bin').read_bytes()[:65536]
        cycles = int(re.findall(r'\d+', r.stdout)[-1]) if re.findall(r'\d+', r.stdout) else -1
        if want_code:
            return before, after, cycles, code
        return before, after, cycles


def cstr(mem, addr):
    end = mem.index(0, addr)
    return bytes(mem[addr:end])


def test_utf8():
    cases = [
        (b'caf\xe9.txt', b'cafe.txt'),          # Latin-1 byte, extension kept
        (b'm\xc3\xbasica', b'musica'),           # UTF-8 U+00FA
        (b'\xc3\x91and\xc3\xba', b'Nandu'),
        (b'\xe2\x80\x99x', b'?x'),               # 3-byte sequence
        (b'\xf0\x9f\x98\x80y', b'?y'),           # 4-byte sequence
        (b'a\x80b', b'a?b'),                     # stray continuation
        (b'\xc3', b'A'),                         # truncated lead -> Latin-1
        (b'\xc2\xa1!', b'!!'),
        (b'\xd0\x96z', b'?z'),                   # U+0416
        (b'\xe9\xe8', b'ee'),                    # two Latin-1 leads
        (b'plain', b'plain'),
    ]

    def setup(mem):
        for i, (src, _) in enumerate(cases):
            a = 0xC000 + i * 32
            mem[a:a + len(src) + 1] = src + b'\0'
    body = ''.join(f'    ld hl, 0x{0xC000 + i * 32:04X}\n    call _utf8_to_ascii\n' for i in range(len(cases)))
    _, after, cyc = run(body, setup)
    for i, (src, want) in enumerate(cases):
        got = cstr(after, 0xC000 + i * 32)
        assert got == want, f'utf8 {src!r}: {got!r} != {want!r}'
        assert all(b < 0x80 for b in got)
    print(f'PASS utf8_to_ascii: {len(cases)} cases ({cyc} T)')


def bpe_with_dict(entries):
    lines = '\n'.join(f'    defb 0x{a:02X}, 0x{b:02X}, 0x00' for a, b in entries)
    start = '; --- BPE DICT START (replaced by bpe_compress.py) ---'
    i = ASM.index(start) + len(start)
    j = ASM.index('; --- BPE DICT END ---')
    return ASM[:i] + '\n' + lines + '\n' + ASM[j:]


def test_bpe_guard():
    # Dictionary: 0x80 = "ab"; 0x81..0x93 = chain, each = (prev, 'x'): depth 20
    entries = [(ord('a'), ord('b'))] + [(0x80 + k, ord('x')) for k in range(19)]
    asm_text = bpe_with_dict(entries)
    text = b'\x80\xfe\x93Z\0'          # valid, out-of-dict, too deep, ASCII

    def setup(mem):
        mem[0xC800:0xC800 + len(text)] = text
        mem[GLOBALS['_main_line']] = 5
        mem[GLOBALS['_main_col']] = 0
        mem[GLOBALS['_current_attr']] = 0x47
    body = f'''    ld a, 0xFF
    ld (_cache_row_y), a
    ld hl, 0xC800
    call _main_puts
    ld (0x{RESULT:04X}), sp
'''
    _, after, cyc = run(body, setup, asm_text)
    sp = after[RESULT] | after[RESULT + 1] << 8
    assert sp == 0xFF00, hex(sp)
    col = after[GLOBALS['_main_col']]
    line = after[GLOBALS['_main_line']]
    # "ab" + "?" + (depth-capped expansion: >= 2 chars) + "Z", wrapping allowed
    assert line in (5, 6, 7, 8) and (col > 0 or line > 5), (line, col)
    print(f'PASS BPE guard: out-of-dict token and depth-20 chain terminate (line {line} col {col}, {cyc} T)')


def test_main_print_lf():
    def setup(mem):
        mem[0xC900:0xC906] = b'AB\nCD\0'
        mem[GLOBALS['_main_line']] = 4
        mem[GLOBALS['_main_col']] = 0
        mem[GLOBALS['_current_attr']] = 0x47
    body = '''    ld a, 0xFF
    ld (_cache_row_y), a
    ld hl, 0xC900
    call _main_print_asm
'''
    _, after, _ = run(body, setup)
    assert after[GLOBALS['_main_line']] == 6 and after[GLOBALS['_main_col']] == 0, \
        (after[GLOBALS['_main_line']], after[GLOBALS['_main_col']])
    print('PASS main_print: embedded LF takes the newline path')


def test_rb_read_block():
    ring = GLOBALS['_ring_buffer']

    def setup(mem):
        for i in range(2048):
            mem[ring + i] = i & 0xFF ^ 0x5A
        mem[GLOBALS['_rb_tail']:GLOBALS['_rb_tail'] + 2] = (2040).to_bytes(2, 'little')
        mem[GLOBALS['_rb_head']:GLOBALS['_rb_head'] + 2] = (10).to_bytes(2, 'little')
    body = f'''    ld hl, 0xD000
    push hl
    ld hl, 100
    push hl
    call _rb_read_block
    ld (0x{RESULT:04X}), hl
    ld hl, 0xD100
    push hl
    ld hl, 4
    push hl
    call _rb_read_block
    ld (0x{RESULT + 2:04X}), hl
    ld hl, 0xD200
    push hl
    ld hl, 100
    push hl
    call _rb_read_block
    ld (0x{RESULT + 4:04X}), hl
    ld hl, 0xD300
    push hl
    ld hl, 100
    push hl
    call _rb_read_block
    ld (0x{RESULT + 6:04X}), hl
    ld (0x{RESULT + 8:04X}), sp
'''
    _, a, _ = run(body, setup)
    w = lambda o: a[RESULT + o] | a[RESULT + o + 1] << 8
    exp = lambda i: (i & 0xFF) ^ 0x5A
    assert (w(0), w(2), w(4), w(6)) == (8, 4, 6, 0), (w(0), w(2), w(4), w(6))
    assert w(8) == 0xFF00
    assert bytes(a[0xD000:0xD008]) == bytes(exp(i) for i in range(2040, 2048))
    assert bytes(a[0xD100:0xD104]) == bytes(exp(i) for i in range(0, 4))
    assert bytes(a[0xD200:0xD206]) == bytes(exp(i) for i in range(4, 10))
    tail = a[GLOBALS['_rb_tail']] | a[GLOBALS['_rb_tail'] + 1] << 8
    assert tail == 10
    print('PASS rb_read_block: wrap, bounds, empty, callee stack')


def test_plf_equivalence():
    rnd = random.Random(3)
    rows = []
    s1 = b'Hello  World   ' + bytes(range(32, 80))
    rows.append(s1[:64])
    s2 = bytes(rnd.choice(b'  ab \x01\x7f\x80\xff Z') for _ in range(40))   # NUL-padded after 40
    rows.append(s2)
    rows.append(b'')
    rows.append(b' x' * 32)
    targets = [7, 8, 15, 18]

    def cells(s):
        out = []
        for i in range(64):
            c = s[i] if i < len(s) else 32
            out.append(32 if (c < 32 or c >= 128) else c)
        return out

    def setup(mem):
        for k, s in enumerate(rows):
            a = 0xC000 + k * 80
            mem[a:a + len(s) + 1] = s + b'\0'
            for i, c in enumerate(cells(s)):
                mem[0xC400 + k * 64 + i] = c
    # pass 1: fast renderer on rows; pass 2: per-char renderer on rows+? (separate run)
    body_fast = ''.join(f'''    ld hl, {y}
    push hl
    ld hl, 0x{0xC000 + k * 80:04X}
    push hl
    ld hl, 0x45
    push hl
    call _print_line64_fast
    pop hl
    pop hl
    pop hl
''' for k, y in enumerate(targets))
    body_slow = ''
    for k, y in enumerate(targets):
        body_slow += f'''    ld a, {y}
    ld (_g_ps64_y), a
    ld a, 0x45
    ld (_g_ps64_attr), a
    xor a
    ld (_g_ps64_col), a
plf_slow_{k}:
    ld a, (_g_ps64_col)
    ld e, a
    ld d, 0
    ld hl, 0x{0xC400 + k * 64:04X}
    add hl, de
    ld l, (hl)
    call _print_str64_char
    ld hl, _g_ps64_col
    inc (hl)
    ld a, (hl)
    cp 64
    jr nz, plf_slow_{k}
'''
    body_slow = '    ld a, 0xFF\n    ld (_cache_row_y), a\n' + body_slow
    _, fast, cf = run(body_fast, setup)
    _, slow, cs = run(body_slow, setup)
    for y in targets:
        for scan in range(8):
            a = screen_addr(y, scan)
            assert fast[a:a + 32] == slow[a:a + 32], f'row {y} scan {scan}'
        at = 0x5800 + y * 32
        assert fast[at:at + 32] == slow[at:at + 32] == bytes([0x45]) * 32, f'attr row {y}'
    # nothing else changed by the fast renderer
    rows_set = set(targets)
    for addr in range(0x4000, 0x5B00):
        if addr < 0x5800:
            row = ((addr >> 8) & 0x18) | ((addr >> 5) & 7)
        else:
            row = (addr - 0x5800) >> 5
        if row in rows_set:
            continue
        assert fast[addr] == slow[addr], hex(addr)
    print(f'PASS print_line64_fast == 64 x print_str64_char on 4 rows ({cf} T vs {cs} T)')


def test_scroll():
    def setup(mem):
        mem[GLOBALS['_current_attr']] = 0x38
    body = '''    ei
    call _scroll_main_zone
    ld a, i
    ld a, 0
    jp po, scroll_di
    inc a
scroll_di:
    ld (0xB012), a
    di
'''
    before, after, cyc = run(body, setup)
    for row in range(24):
        for scan in range(8):
            a = screen_addr(row, scan)
            if 3 <= row <= 17:
                want = before[screen_addr(row + 1, scan):screen_addr(row + 1, scan) + 32]
            elif row == 18:
                want = bytes(32)
            else:
                want = before[a:a + 32]
            assert after[a:a + 32] == want, f'bitmap row {row} scan {scan}'
        at = 0x5800 + row * 32
        if 3 <= row <= 17:
            want = before[at + 32:at + 64]
        elif row == 18:
            want = bytes([0x38]) * 32
        else:
            want = before[at:at + 32]
        assert after[at:at + 32] == want, f'attr row {row}'
    assert after[0xB012] == 1, 'scroll must keep interrupts enabled'
    print(f'PASS scroll_main_zone: geometry exact, IFF kept ({cyc} T, uncontended)')


LUT = bytes([0x00, 0x22, 0x44, 0x55, 0x66, 0x88, 0xAA, 0xCC, 0xEE, 0xFF])


def font_rows(code, ch):
    base = code.index(LUT) + len(LUT)            # font64_packed follows font_lut
    b = code[base + (ch - 32) * 3: base + (ch - 32) * 3 + 3]
    return [LUT[n] for x in b for n in (x >> 4, x & 15)]


def test_glyphs_any_lut_address():
    chars = list(range(32, 128))
    body = '    ld a, 0xFF\n    ld (_cache_row_y), a\n    ld a, 0x47\n    ld (_g_ps64_attr), a\n'
    for i, ch in enumerate(chars):
        body += f'''    ld a, {10 + i // 64}
    ld (_g_ps64_y), a
    ld a, {i % 64}
    ld (_g_ps64_col), a
    ld l, {ch}
    call _print_str64_char
'''
    *_, code = run(body, want_code=True)
    lut_at = 0x8000 + code.index(LUT)
    tested = []
    for low in (0xF8, 0xFE, (lut_at & 0xFF)):
        pad = (low - (lut_at & 0xFF)) % 256
        _, after, _, code2 = run(body, pad=pad, want_code=True)
        at = 0x8000 + code2.index(LUT)
        assert at & 0xFF == low, hex(at)
        for i, ch in enumerate(chars):
            row, col = 10 + i // 64, i % 64
            rows = font_rows(code2, ch)
            mask = 0xF0 if col % 2 == 0 else 0x0F
            got = [after[screen_addr(row, sc) + col // 2] & mask for sc in range(8)]
            want = [0] + [r & mask for r in rows] + [0]
            assert got == want, f'char {ch} with font_lut at {at:04X}: {got} != {want}'
        tested.append(f'{at:04X}')
    print('PASS glyph decode for all 96 chars with font_lut at ' + ', '.join(tested))


if __name__ == '__main__':
    test_glyphs_any_lut_address()
    test_utf8()
    test_bpe_guard()
    test_main_print_lf()
    test_rb_read_block()
    test_plf_equivalence()
    test_scroll()
    print('ALL ASM CPU TESTS PASSED')
