#!/usr/bin/env python3
"""Pack the approved BridgeZX-style pixel wordmark into its ULA header.

assets/banner_logo.txt is the editable artwork: one '#' or '.' per pixel.
No system font, rasterizer or image library is needed. Use --check to verify
that the checked-in header matches the artwork.
"""
import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'assets/banner_logo.txt'
OUTPUT = ROOT / 'src/banner_logo.h'
WIDTH, HEIGHT, CYAN_X = 179, 14, 144


def render():
    rows = SOURCE.read_text(encoding='ascii').splitlines()
    if len(rows) != HEIGHT or any(len(row) != WIDTH or set(row) - set('#.') for row in rows):
        raise ValueError(f'{SOURCE}: expected {HEIGHT} rows of {WIDTH} #/. pixels')
    stride = (WIDTH + 7) // 8
    packed = [bytes(int(row.ljust(stride * 8, '.')[i:i + 8].replace('#', '1').replace('.', '0'), 2)
                    for i in range(0, stride * 8, 8)) for row in rows]
    header = ('/* Generated from assets/banner_logo.txt; BridgeZX-style bitstreamzx. */\n'
              '#define BANNER_LOGO_Y 1\n'
              f'#define BANNER_LOGO_ROWS {HEIGHT}\n'
              f'#define BANNER_LOGO_BYTES {stride}\n'
              f'#define BANNER_LOGO_ZX_COL {CYAN_X // 8}\n'
              f'#define BANNER_LOGO_ZX_CELLS {stride - CYAN_X // 8}\n'
              'static const uint8_t banner_logo[BANNER_LOGO_ROWS][BANNER_LOGO_BYTES] = {\n')
    header += ''.join('    {' + ', '.join(f'0x{b:02X}' for b in row) + '},\n' for row in packed)
    return header + '};\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    header = render()
    if args.check:
        if OUTPUT.read_text() != header:
            raise SystemExit('Stale banner_logo.h: run tools/banner_logo.py')
    else:
        OUTPUT.write_text(header)
    print(f'BridgeZX-style bitstreamzx: {WIDTH}x{HEIGHT} pixels; cyan starts at {CYAN_X}')


if __name__ == '__main__':
    main()
