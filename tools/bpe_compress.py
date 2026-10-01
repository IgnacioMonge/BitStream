#!/usr/bin/env python3
"""
BPE (Byte Pair Encoding) string compressor for BitStreamZX.

Scans C source files for screen-only string literals, compresses them
using BPE with tokens 0x80-0xFF, and generates:
  1. Modified .c files with compressed string contents (build/bpe_final/)
  2. BPE dictionary as ASM defb lines (patched into bitstream_asm.asm copy)

Screen-only strings: passed to main_puts, main_print, fail, etc.
Excluded: uart_send_string, strcmp, safe_copy, print_str64, etc.

Based on SpectalkZX bpe_compress.py, adapted for BitStream's architecture.
"""

import re
import os
import sys
import shutil
from collections import Counter

# ============================================================================
# Configuration
# ============================================================================

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(SCRIPT_DIR)
BUILD_DIR = os.path.join(ROOT, 'build')
# Outside build/: `make clean` between targets must not delete the backup
BPE_ORIGINALS = os.path.join(ROOT, '.bpe_originals')

SRC_C_FILES = ['globals.c', 'ui.c', 'comms.c', 'net_esp.c', 'net_spectranext.c',
               'fs_esx.c', 'fs_spectranext.c', 'ftp.c', 'commands.c', 'main.c']
ASM_FILE = 'bitstream_asm.asm'

# Functions whose string arguments are screen-only (go through main_puts BPE decoder)
SCREEN_FUNCS = {
    'main_print', 'main_puts', 'main_puts2', 'fail',
    'status_result', 'print_smart_path', 'print_reply', 'report_reply',
}

# Functions whose string arguments must NOT be compressed
EXCLUDE_FUNCS = {
    'uart_send_string', 'esp_send_at',
    'print_str64', 'print_line64_fast', 'print_big_str', 'draw_big_char',
    'print_char64', 'put_char64_input_cached',
    'safe_copy', 'strcmp', 'strstr', 'strncmp', 'strncpy',
    'memcpy', 'strcpy', 'strcat', 'strncat',
    'str_append', 'char_append', 'u16_to_dec',
    'ftp_command', 'ftp_cmd_reply', 'ftp_transfer_begin', 'prompt_input_zone',
    'wait_for_string', 'wait_for_ftp_code_fast',
}

TOKEN_START = 0x80
TOKEN_END = 0xFF
MAX_TOKENS = TOKEN_END - TOKEN_START + 1

STRING_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')
HEX_CHARS = set('0123456789abcdefABCDEF')

# ============================================================================
# String extraction
# ============================================================================

def decode_c_string(s):
    """Decode C string escape sequences."""
    result = []
    i = 0
    while i < len(s):
        if s[i] == '\\' and i + 1 < len(s):
            c = s[i + 1]
            if c == 'n': result.append('\n')
            elif c == 'r': result.append('\r')
            elif c == 't': result.append('\t')
            elif c == '0': result.append('\0')
            elif c == '\\': result.append('\\')
            elif c == '"': result.append('"')
            elif c == 'x' and i + 3 < len(s):
                try:
                    result.append(chr(int(s[i+2:i+4], 16)))
                    i += 4
                    continue
                except ValueError:
                    result.append(s[i])
                    i += 1
                    continue
            else:
                result.append(s[i])
                result.append(c)
            i += 2
        else:
            result.append(s[i])
            i += 1
    return ''.join(result)


def extract_screen_strings(src_dir):
    """Extract all screen-only string literals from C source files."""
    strings = []

    for fname in SRC_C_FILES:
        fpath = os.path.join(src_dir, fname)
        if not os.path.exists(fpath):
            print(f"  WARNING: {fpath} not found, skipping")
            continue
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
                try:
                    decoded = decode_c_string(raw)
                except:
                    continue
                if len(decoded) < 3:
                    continue

                col = m.start()
                prefix = line[:col].rstrip()
                fm = re.search(r'(\w+)\s*\([^)]*$', prefix)
                ctx = fm.group(1) if fm else 'UNKNOWN'

                if ctx in SCREEN_FUNCS:
                    strings.append({
                        'file': fname,
                        'line': i + 1,
                        'raw': raw,
                        'decoded': decoded,
                        'context': ctx,
                    })

    return strings


# ============================================================================
# BPE Compression
# ============================================================================

def build_corpus(strings):
    corpus = []
    for s in strings:
        for ch in s['decoded']:
            b = ord(ch)
            if b < 0x80:
                corpus.append(b)
        corpus.append(0)
    return corpus


def count_pairs(data):
    pairs = Counter()
    for i in range(len(data) - 1):
        a, b = data[i], data[i + 1]
        if a == 0 or b == 0:
            continue
        pairs[(a, b)] += 1
    return pairs


def bpe_compress(corpus, max_tokens=MAX_TOKENS):
    data = list(corpus)
    dictionary = []
    token = TOKEN_START

    for _ in range(max_tokens):
        pairs = count_pairs(data)
        if not pairs:
            break

        best_pair, best_count = pairs.most_common(1)[0]
        savings = best_count - 1 - 3  # 3 bytes dict entry overhead
        if savings <= 0:
            break

        new_data = []
        i = 0
        while i < len(data):
            if (i < len(data) - 1 and
                data[i] == best_pair[0] and data[i + 1] == best_pair[1] and
                data[i] != 0 and data[i + 1] != 0):
                new_data.append(token)
                i += 2
            else:
                new_data.append(data[i])
                i += 1

        dictionary.append(best_pair)
        data = new_data
        token += 1
        if token > TOKEN_END:
            break

    return data, dictionary


def compress_string_bytes(decoded, dictionary):
    data = [ord(c) for c in decoded if ord(c) < 0x80]
    for idx, (a, b) in enumerate(dictionary):
        token = TOKEN_START + idx
        new_data = []
        i = 0
        while i < len(data):
            if i < len(data) - 1 and data[i] == a and data[i + 1] == b:
                new_data.append(token)
                i += 2
            else:
                new_data.append(data[i])
                i += 1
        data = new_data
    return bytes(data)


def encode_bytes_as_c_string(raw_bytes):
    """Convert compressed bytes to C string literal content.
    CRITICAL: hex escape ambiguity fix — insert string break after \\xNN
    if next char is a hex digit."""
    parts = []
    prev_was_hex = False
    for b in raw_bytes:
        if prev_was_hex and b < 0x80:
            c = chr(b) if 32 <= b < 127 else ''
            if c in HEX_CHARS:
                parts.append('" "')  # string concatenation break

        prev_was_hex = False
        if b >= 0x80:
            parts.append(f'\\x{b:02x}')
            prev_was_hex = True
        elif b == ord('\\'):
            parts.append('\\\\')
        elif b == ord('"'):
            parts.append('\\"')
        elif b == ord('\n'):
            parts.append('\\n')
        elif b == ord('\r'):
            parts.append('\\r')
        elif b == ord('\t'):
            parts.append('\\t')
        elif 32 <= b < 127:
            parts.append(chr(b))
        else:
            parts.append(f'\\x{b:02x}')
            prev_was_hex = True
    return ''.join(parts)


def expand_token(idx, dictionary, depth=0):
    if depth > 10:
        return '?'
    a, b = dictionary[idx]
    result = ''
    for byte in (a, b):
        if byte >= TOKEN_START and (byte - TOKEN_START) < len(dictionary):
            result += expand_token(byte - TOKEN_START, dictionary, depth + 1)
        elif 32 <= byte < 127:
            result += chr(byte)
        else:
            result += f'[{byte:02X}]'
    return result


# ============================================================================
# Output generation
# ============================================================================

# Runtime limit: bpe_rstack in asm/bitstream_asm.asm holds 16 continuations.
BPE_RSTACK_LEVELS = 16


def token_depth(idx, dictionary, memo):
    """Nesting depth reached while expanding token idx (1 = no nested token)."""
    if idx in memo:
        return memo[idx]
    depth = 1
    for byte in dictionary[idx]:
        if byte >= TOKEN_START and (byte - TOKEN_START) < len(dictionary):
            depth = max(depth, 1 + token_depth(byte - TOKEN_START, dictionary, memo))
    memo[idx] = depth
    return depth


def generate_dict_asm(dictionary):
    """Generate BPE dictionary as ASM defb lines."""
    lines = []
    for i, (a, b) in enumerate(dictionary):
        token = TOKEN_START + i
        expanded = expand_token(i, dictionary)
        lines.append(f'    defb 0x{a:02X}, 0x{b:02X}, 0x00  ; 0x{token:02X} = "{expanded}"')
    return '\n'.join(lines)


def generate_compressed_sources(src_dir, out_dir, strings, dictionary):
    """Generate modified .c files with BPE-compressed string literals."""
    replacements = {}
    for s in strings:
        compressed_bytes = compress_string_bytes(s['decoded'], dictionary)
        compressed_raw = encode_bytes_as_c_string(compressed_bytes)
        if compressed_raw != s['raw']:
            key = (s['file'], s['line'])
            if key not in replacements:
                replacements[key] = []
            replacements[key].append((s['raw'], compressed_raw))

    os.makedirs(out_dir, exist_ok=True)

    for fname in SRC_C_FILES:
        fpath = os.path.join(src_dir, fname)
        with open(fpath, 'r', encoding='utf-8', errors='replace') as f:
            lines = f.readlines()

        modified = False
        new_lines = []
        for i, line in enumerate(lines):
            key = (fname, i + 1)
            if key in replacements:
                for orig_raw, comp_raw in replacements[key]:
                    old = f'"{orig_raw}"'
                    new = f'"{comp_raw}"'
                    if old in line:
                        line = line.replace(old, new, 1)
                        modified = True
            new_lines.append(line)

        out_path = os.path.join(out_dir, fname)
        with open(out_path, 'w', encoding='utf-8') as f:
            f.writelines(new_lines)

        count = sum(1 for k in replacements if k[0] == fname)
        if count:
            print(f"  {fname}: {count} strings compressed")


def patch_asm_dict(src_dir, out_dir, dict_asm):
    """Copy ASM file with BPE dict inserted between markers."""
    asm_src = os.path.join(src_dir, '..', 'asm', ASM_FILE)
    asm_dst = os.path.join(out_dir, ASM_FILE)
    os.makedirs(out_dir, exist_ok=True)

    with open(asm_src, 'r', encoding='utf-8') as f:
        content = f.read()

    marker_start = '; --- BPE DICT START (replaced by bpe_compress.py) ---'
    marker_end = '; --- BPE DICT END ---'
    idx_start = content.find(marker_start)
    idx_end = content.find(marker_end)

    if idx_start == -1 or idx_end == -1:
        print("ERROR: BPE dict markers not found in ASM file!")
        sys.exit(1)

    new_content = (content[:idx_start + len(marker_start)] + '\n' +
                   dict_asm + '\n' +
                   content[idx_end:])

    with open(asm_dst, 'w', encoding='utf-8') as f:
        f.write(new_content)
    print(f"  {ASM_FILE}: {len(dict_asm.splitlines())} dict entries inserted")


# ============================================================================
# Main
# ============================================================================

def main():
    src_dir = os.path.join(ROOT, 'src')
    bpe_final = os.path.join(BUILD_DIR, 'bpe_final')
    bpe_originals = BPE_ORIGINALS

    # An interrupted build (or a concurrent one) can leave compressed sources
    # installed: put the originals back before taking a new backup, or the
    # compressed text would become the "original".
    restore()

    # 1. Extract screen-only strings
    strings = extract_screen_strings(src_dir)
    corpus = build_corpus(strings)
    content_bytes = sum(1 for b in corpus if b != 0)

    # 2. Compress
    compressed, dictionary = bpe_compress(corpus)
    compressed_bytes = sum(1 for b in compressed if b != 0)
    dict_size = len(dictionary) * 3
    saved = content_bytes - compressed_bytes

    print(f"  BPE: {len(strings)} strings, {len(dictionary)} tokens, "
          f"{content_bytes}B -> {compressed_bytes}B (-{saved}B, dict {dict_size}B)")

    if len(dictionary) == 0:
        print("  No compression possible, skipping")
        return

    memo = {}
    max_depth = max(token_depth(i, dictionary, memo) for i in range(len(dictionary)))
    if max_depth > BPE_RSTACK_LEVELS:
        print(f"ERROR: BPE nesting depth {max_depth} exceeds bpe_rstack "
              f"({BPE_RSTACK_LEVELS} levels)")
        sys.exit(1)

    # 3. Backup originals
    os.makedirs(bpe_originals, exist_ok=True)
    for fname in SRC_C_FILES:
        src = os.path.join(src_dir, fname)
        dst = os.path.join(bpe_originals, fname)
        shutil.copy2(src, dst)
    shutil.copy2(os.path.join(ROOT, 'asm', ASM_FILE),
                 os.path.join(bpe_originals, ASM_FILE))

    # 4. Generate compressed .c files
    generate_compressed_sources(src_dir, bpe_final, strings, dictionary)

    # 5. Generate ASM with dict
    dict_asm = generate_dict_asm(dictionary)
    patch_asm_dict(src_dir, bpe_final, dict_asm)

    # 6. Install compressed files for compilation
    for fname in SRC_C_FILES:
        shutil.copy2(os.path.join(bpe_final, fname), os.path.join(src_dir, fname))
    shutil.copy2(os.path.join(bpe_final, ASM_FILE),
                 os.path.join(ROOT, 'asm', ASM_FILE))


def restore():
    """Restore original source files from backup, then drop the backup."""
    bpe_originals = BPE_ORIGINALS
    if not os.path.exists(bpe_originals):
        return
    src_dir = os.path.join(ROOT, 'src')
    for fname in SRC_C_FILES:
        orig = os.path.join(bpe_originals, fname)
        if os.path.exists(orig):
            shutil.copy2(orig, os.path.join(src_dir, fname))
    orig_asm = os.path.join(bpe_originals, ASM_FILE)
    if os.path.exists(orig_asm):
        shutil.copy2(orig_asm, os.path.join(ROOT, 'asm', ASM_FILE))
    shutil.rmtree(bpe_originals, ignore_errors=True)


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--restore':
        restore()
        print("  BPE: originals restored")
    else:
        main()
