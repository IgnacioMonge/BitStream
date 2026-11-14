#!/usr/bin/env python3
"""Check the real linked banner pixels, ULA colours and write boundaries."""
import argparse
import hashlib
import re
from pathlib import Path

from test_transfer_cpu import CPU, ROOT, VARIANTS, RESULT

APPROVED_LOGO_SHA = '50b221e46674738b0842fbd477fbe4f28aea63bbf13b521f81d55edc4e1e0507'


def screen_addr(row, scan):
    return 0x4000 + ((row & 24) << 8) + ((row & 7) << 5) + (scan << 8)


def test_banner(cpu):
    header = (ROOT / 'src/banner_logo.h').read_text()
    defines = {name: int(value) for name, value in re.findall(r'^#define (BANNER_\w+) (\d+)$', header, re.M)}
    data = bytes(int(x, 16) for x in re.findall(r'0x([0-9A-F]{2})', header))
    y0, rows, width, zx_col, zx_cells = (defines['BANNER_LOGO_' + key] for key in ('Y', 'ROWS', 'BYTES', 'ZX_COL', 'ZX_CELLS'))
    assert len(data) == rows * width and width <= 27 and y0 + rows <= 15
    assert 'BridgeZX-style bitstreamzx.' in header
    assert hashlib.sha256(data).hexdigest() == APPROVED_LOGO_SHA, 'not the approved BridgeZX-style bitmap'
    assert (y0, rows, zx_col) == (1, 14, 18), 'logo geometry or cyan boundary changed'
    def setup(mem):
        mem[0x4000:0x5B00] = b'\x96' * 0x1B00
    after = cpu.run('    call _draw_banner\n', setup=setup)
    for y in range(16):
        at = screen_addr(y // 8, y % 8)
        expected = data[(y-y0)*width:(y-y0+1)*width] if y0 <= y < y0+rows else b''
        expected += bytes(27-len(expected))
        assert after[at:at+27] == expected, f'{cpu.variant}: logo scanline {y}'
    for row, badge in enumerate((bytes((0x40,0x42,0x56,0x74,0x61)), bytes((0x42,0x56,0x74,0x61,0x48)))):
        expected = bytearray([0x47] * 27)
        expected[zx_col:zx_col+zx_cells] = bytes([0x45]) * zx_cells
        at = 0x5800 + row*32
        assert after[at:at+32] == expected + badge, f'{cpu.variant}: white/cyan/rainbow attributes'
    assert after[screen_addr(2,0):screen_addr(2,0)+32] == b'\xFF'*32
    assert after[0x5840:0x5860] == b'\x07'*32
    harness = {RESULT+2,RESULT+3,RESULT+4}
    for row in range(2,24):
        for scan in range(8):
            if row==2 and scan==0: continue
            at=screen_addr(row,scan)
            for offset in range(32):
                address=at+offset
                if address not in harness: assert after[address]==0x96, f'banner overwrote {address:04X}'
        if row>2: assert after[0x5800+row*32:0x5820+row*32] == b'\x96'*32
    print(f'PASS {cpu.variant}: approved BridgeZX-style bitstreamzx, cyan zx, black gap, separator, body guards; {cpu.cycles} T')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir',type=Path,default=ROOT/'build')
    parser.add_argument('--variant',choices=VARIANTS,action='append')
    args=parser.parse_args()
    for variant in args.variant or VARIANTS: test_banner(CPU(variant,args.build_dir))
    print('ALL BANNER CPU TESTS PASSED (physical display untested)')


if __name__=='__main__': main()
