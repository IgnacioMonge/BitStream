#!/usr/bin/env python3
"""End-to-end test: BitStream (divMMC build) in ZEsarUX + ZXESPEmu + local FTP.

Drives the real TAP through the emulated ZX-Uno UART and the ZXESPEmu ESP-AT
modem against a pyftpdlib server on loopback, typing commands through ZRCP
keyboard-matrix injection, and checks the files written to the emulated SD.

Not hardware validation: ZXESPEmu is a software ESP-AT model and ZEsarUX's
UART timing is not the divTIESUS FIFO. It exercises protocol logic, the
demultiplexer, esxDOS writes and the screen paths.

Usage (Windows, from the BitStream checkout, after `make`):
    python tools/e2e_zesarux.py [--model zxuno|next] [--zxespemu C:/dev/ZXESPEmu] [--keep]
Requires: pip install pyftpdlib
"""
import argparse
import hashlib
import os
import random
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Keyboard matrix rows in ZEsarUX set-ui-io-ports order
ROWS = [
    "\x01zxcv",     # FEFE  (\x01 = CAPS SHIFT)
    "asdfg",        # FDFE
    "qwert",        # FBFE
    "12345",        # F7FE
    "09876",        # EFFE
    "poiuy",        # DFFE
    "\nlkjh",       # BFFE
    " \x02mnb",     # 7FFE  (\x02 = SYMBOL SHIFT)
]
SYM = {
    '.': 'm', ':': 'z', '!': '1', '%': '5', '/': 'v', '-': 'j', '_': '0',
    '"': 'p', '>': 't', '<': 'r', '?': 'c', ',': 'n', '@': '2', '#': '3',
    '$': '4', '&': '6', "'": '7', '(': '8', ')': '9', '=': 'l', '+': 'k',
    '*': 'b', ';': 'o',
}


def key_pos(k):
    for r, row in enumerate(ROWS):
        c = row.find(k)
        if c >= 0:
            return r, c
    raise KeyError(repr(k))


class Zrcp:
    def __init__(self, port):
        deadline = time.monotonic() + 20
        while True:
            try:
                self.s = socket.create_connection(('127.0.0.1', port), timeout=5)
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.2)
        self.read_prompt()

    def read_prompt(self):
        buf = b''
        while not buf.endswith(b'> '):
            chunk = self.s.recv(4096)
            if not chunk:
                raise ConnectionError('ZRCP closed')
            buf += chunk
        cut = buf.rfind(b'\n')
        return buf[:cut] if cut >= 0 else b''

    def cmd(self, text):
        self.s.sendall(text.encode() + b'\n')
        return self.read_prompt().decode(errors='replace').strip()

    def peek(self, addr, n=1):
        out = self.cmd(f'read-memory {addr} {n}').split()
        return bytes.fromhex(out[-1][:n * 2]) if out else bytes(n)

    def keys(self, pressed):
        rows = [0xFF] * 8
        for k in pressed:
            r, c = key_pos(k)
            rows[r] &= ~(1 << c) & 0xFF
        self.cmd('set-ui-io-ports ' + ''.join(f'{v:02x}' for v in rows) + '00')

    def tap(self, *pressed, hold=0.08, gap=0.08):
        self.keys(pressed)
        time.sleep(hold)
        self.keys(())
        time.sleep(gap)

    def type(self, text):
        for ch in text:
            if ch in SYM:
                self.tap('\x02', SYM[ch])
            elif ch.isupper():
                self.tap('\x01', ch.lower())
            else:
                self.tap(ch)


def symbols(map_path):
    table = {}
    for line in map_path.read_text(errors='replace').splitlines():
        m = re.match(r'^(\S+)\s+=\s+\$([0-9A-Fa-f]+)', line)
        if m:
            table[m.group(1)] = int(m.group(2), 16)
    return table


def build_ftp_tree(base):
    if base.exists():
        shutil.rmtree(base)
    base.mkdir(parents=True)
    rnd = random.Random(1234)
    (base / 'small.txt').write_bytes(b'BitStream e2e small file\r\n' * 4)
    (base / 'empty.bin').write_bytes(b'')
    (base / 'big.bin').write_bytes(bytes(rnd.randrange(256) for _ in range(61_440 + 77)))
    (base / 'caf\u00e9.txt').write_bytes(b'utf8 name\n')
    sub = base / 'm\u00fasica'
    sub.mkdir()
    (sub / 'x.txt').write_bytes(b'inside a UTF-8 directory\n')
    for i in range(20):
        (base / f'f{i:02d}.dat').write_bytes(bytes([i]) * (100 + i))
    return base


def start_ftp(root, port, masquerade):
    from pyftpdlib.authorizers import DummyAuthorizer
    from pyftpdlib.handlers import FTPHandler
    from pyftpdlib.servers import FTPServer

    auth = DummyAuthorizer()
    auth.add_anonymous(str(root))
    handler = FTPHandler
    handler.authorizer = auth
    handler.banner = 'BitStream e2e test server'
    handler.passive_ports = range(30000, 30100)
    handler.use_encoding = 'utf8'
    if masquerade:
        handler.masquerade_address = masquerade
    server = FTPServer(('127.0.0.1', port), handler)
    t = threading.Thread(target=server.serve_forever, kwargs={'timeout': 0.5}, daemon=True)
    t.start()
    return server


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--zxespemu', type=Path, default=ROOT.parent / 'ZXESPEmu')
    ap.add_argument('--model', choices=('zxuno', 'next'), default='zxuno',
                    help='zxuno: divTiesus build on ZX-Uno; next: Next build on TBBlue')
    ap.add_argument('--tap', type=Path, help='default: the build for --model')
    ap.add_argument('--masquerade', default='10.9.8.7',
                    help='PASV address advertised by the server (NAT case); "" disables')
    ap.add_argument('--keep', action='store_true', help='leave the emulator running')
    ap.add_argument('--window', action='store_true', help='show the emulator window')
    args = ap.parse_args()

    sys.path.insert(0, str(args.zxespemu))
    import run as zx   # ZXESPEmu launcher helpers

    work = ROOT / 'build/e2e'
    ftp_root = build_ftp_tree(work / 'ftp')
    sd = work / 'sd'
    if sd.exists():
        shutil.rmtree(sd)
    sd.mkdir(parents=True)
    shots = work / 'shots'
    shots.mkdir(exist_ok=True)

    if args.tap is None:
        args.tap = ROOT / ('build/BitStream_Next.tap' if args.model == 'next'
                           else 'build/BitStream_divTiesus.tap')
    sym = symbols(args.tap.with_suffix('.map'))
    ftp_port = 2121
    server = start_ftp(ftp_root, ftp_port, args.masquerade)

    uart = work / 'uart'
    if uart.exists():
        uart.unlink()
    modem_log = (work / 'zxesp.log').open('wb')
    modem = subprocess.Popen([sys.executable, str(args.zxespemu / 'zxesp.py'), '--link', str(uart),
                              '--bind', '127.0.0.1', '--reported-ip', '127.0.0.1',
                              '--wifi-mode', 'simulated', '--verbose'],
                             stdout=modem_log, stderr=subprocess.STDOUT)
    zx.wait_for_uart(modem, uart)
    command, _ = zx.artifact_emulator_command(args.tap, args.model, uart, not args.window, sd, True)
    zx.apply_uart_endpoint(command, uart)
    rport = zx.free_tcp_port()
    command[-1:-1] = ['--enable-remoteprotocol', '--remoteprotocol-port', str(rport)]
    if args.model == 'next':
        # No NextZXOS SD image here: boot straight into the 48K ROM with the
        # Next hardware enabled; ZEsarUX's esxDOS handler serves RST 8.
        command[-1:-1] = ['--tbblue-fast-boot-mode']
    emu_log = (work / 'zesarux.log').open('wb')
    emu = subprocess.Popen(command, cwd=zx.RESOURCES, stdout=emu_log, stderr=subprocess.STDOUT)

    results = []

    def check(name, ok, detail=''):
        results.append((name, ok, detail))
        print(('PASS ' if ok else 'FAIL ') + name + (f'  [{detail}]' if detail else ''), flush=True)

    try:
        z = Zrcp(rport)

        def mem(name, n=1):
            return z.peek(sym[name], n)

        def wait(pred, timeout, what):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                if pred():
                    return True
                time.sleep(0.2)
            print('timeout waiting for', what, flush=True)
            return False

        def shot(tag):
            z.cmd(f'save-screen {(shots / (tag + ".bmp")).as_posix()}')

        # Boot: smart_init ends with connection_state = WIFI_OK (1)
        check('boot to WiFi OK', wait(lambda: mem('_connection_state')[0] == 1, 60, 'boot'))
        time.sleep(1.0)
        shot('01_boot')

        # Line editing (SpecTalkZX chords, SYMBOL+CAPS + digit)
        def chord(d):
            z.tap('\x02', '\x01', d)
        z.type('ab cd ef')
        chord('0')              # delete word      -> "ab cd "
        chord('5')              # word left        -> cursor before "cd"
        z.type('X')
        chord('7')              # line start
        z.type('Y')
        chord('6')              # line end
        z.type('Z')
        time.sleep(0.3)
        n = mem('_line_len')[0]
        line = mem('_line_buffer', 16)[:n]
        check('line editing chords', line == b'Yab Xcd Z', repr(line))
        shot('01b_edit')
        for _ in range(n):
            z.tap('\x01', '0')     # DELETE
        time.sleep(0.3)
        check('DELETE clears the line', mem('_line_len')[0] == 0)

        z.type(f'open 127.0.0.1:{ftp_port}\n')
        check('banner 220 -> FTP connected', wait(lambda: mem('_connection_state')[0] >= 2, 20, 'connect'))
        time.sleep(1.0)
        z.type('\n')            # user: anonymous
        time.sleep(0.6)
        z.type('\n')            # password: default
        check('login', wait(lambda: mem('_connection_state')[0] == 3, 20, 'login'))
        time.sleep(2.0)
        shot('02_login')

        # LIST: 25 entries, pauses at "More?" after 15 lines
        z.type('ls\n')
        time.sleep(6)
        shot('03_list_more')
        z.tap('a')
        wait(lambda: False, 6, 'list')
        matches = int.from_bytes(mem('_list_matches', 2), 'little')
        check('LIST through pager', matches == 25, f'{matches} items')
        shot('04_list_done')

        # GET: normal, empty, big (multi-frame, size verified), Latin-1 name escaped
        z.type('get small.txt empty.bin big.bin\n')
        wait(lambda: (sd / 'BIG.BIN').exists() and (sd / 'BIG.BIN').stat().st_size ==
             (ftp_root / 'big.bin').stat().st_size, 120, 'big.bin')
        time.sleep(3)
        shot('05_get')
        for local, remote in (('SMALL.TXT', 'small.txt'), ('EMPTY.BIN', 'empty.bin'), ('BIG.BIN', 'big.bin')):
            p = sd / local
            check(f'GET {remote}', p.exists() and sha(p) == sha(ftp_root / remote),
                  f'{p.stat().st_size if p.exists() else "missing"} bytes')

        # Missing file: 550 must be reported, no file left behind
        z.type('get nothere.bin\n')
        time.sleep(4)
        check('GET missing -> no local file', not (sd / 'NOTHERE.BIN').exists())
        shot('06_get_missing')

        # UTF-8 directory via %-escapes, then PWD (path bytes >= 0x80 from server)
        z.type('cd m%C3%BAsica\n')
        time.sleep(4)
        path = mem('_ftp_path', 48).split(b'\0')[0]
        check('CD into UTF-8 dir, PWD folded to ASCII', path == b'/musica', repr(path))
        shot('07_cd_utf8')
        z.type('get x.txt\n')
        wait(lambda: (sd / 'X.TXT').exists(), 30, 'x.txt')
        time.sleep(2)
        check('GET inside UTF-8 dir', (sd / 'X.TXT').exists() and
              sha(sd / 'X.TXT') == sha(ftp_root / 'm\u00fasica' / 'x.txt'))
        z.type('cd ..\n')
        time.sleep(3)
        z.type('ls caf\n')
        time.sleep(5)
        matches = int.from_bytes(mem('_list_matches', 2), 'little')
        check('search with UTF-8 name', matches == 1, f'{matches} matches')
        shot('08_search')

        z.type('quit\n')
        time.sleep(0.8)
        z.type('y')
        check('QUIT', wait(lambda: mem('_connection_state')[0] == 1, 15, 'quit'))
        shot('09_quit')

        failed = [r for r in results if not r[1]]
        print(f'\n{len(results) - len(failed)}/{len(results)} checks passed', flush=True)
        if args.keep:
            input('Emulator left running; press Enter to stop.')
        return 1 if failed else 0
    finally:
        emu.terminate()
        modem.terminate()
        server.close_all()


if __name__ == '__main__':
    sys.exit(main())
