#!/usr/bin/env python3
"""Analyze BPE compression potential for BitStreamZX."""
import re
import os

SRC_DIR = os.path.join(os.path.dirname(__file__), '..')
SRC_FILES = [
    os.path.join(SRC_DIR, 'src', f) for f in
    ['globals.c', 'ui.c', 'comms.c', 'ftp.c', 'commands.c', 'main.c']
]

# Functions whose string args go through main_puts (compressible)
SCREEN_FUNCS = {
    'main_print', 'main_puts', 'main_puts2', 'fail',
    'status_result', 'print_smart_path',
}

STRING_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')

def analyze():
    screen_strings = []

    for fpath in SRC_FILES:
        fname = os.path.basename(fpath)
        with open(fpath, 'r', encoding='utf-8', errors='replace') as f:
            lines = f.readlines()

        for i, line in enumerate(lines):
            stripped = line.strip()
            if stripped.startswith('#') or stripped.startswith('//'):
                continue
            # Skip const char definitions (shared with UART/compare)
            if re.match(r'\s*(?:static\s+)?(?:const\s+)?char\s+\w+\s*\[\s*\]\s*=', line):
                continue

            for m in STRING_RE.finditer(line):
                raw = m.group(1)
                if len(raw) < 3:
                    continue

                col = m.start()
                prefix = line[:col].rstrip()
                fm = re.search(r'(\w+)\s*\([^)]*$', prefix)
                ctx = fm.group(1) if fm else 'UNKNOWN'

                if ctx in SCREEN_FUNCS:
                    screen_strings.append({
                        'file': fname,
                        'line': i + 1,
                        'context': ctx,
                        'raw': raw,
                        'bytes': len(raw) + 1,
                    })

    total_bytes = sum(s['bytes'] for s in screen_strings)

    print(f"Screen-only strings: {len(screen_strings)}")
    print(f"Screen-only bytes:   {total_bytes}")
    print(f"Est. BPE savings:    {int(total_bytes * 0.25)}-{int(total_bytes * 0.35)} bytes (25-35%)")
    print()

    for s in screen_strings:
        display = s['raw'][:55]
        print(f"  {s['file']:12s}:{s['line']:4d} {s['context']:15s} \"{display}\"")

    # Show byte pair frequency
    corpus = []
    for s in screen_strings:
        for ch in s['raw']:
            b = ord(ch)
            if b < 0x80:
                corpus.append(b)
        corpus.append(0)

    from collections import Counter
    pairs = Counter()
    for i in range(len(corpus) - 1):
        a, b = corpus[i], corpus[i+1]
        if a == 0 or b == 0:
            continue
        pairs[(a,b)] += 1

    print(f"\nTop 15 byte pairs:")
    for (a,b), count in pairs.most_common(15):
        ca = chr(a) if 32 <= a < 127 else f'\\x{a:02x}'
        cb = chr(b) if 32 <= b < 127 else f'\\x{b:02x}'
        savings = count - 1 - 3
        print(f"  '{ca}{cb}' : {count:3d} occurrences (net {savings:+d} bytes)")

if __name__ == '__main__':
    analyze()
