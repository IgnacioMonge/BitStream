#!/usr/bin/env python3
"""Run the actual divTiesus/Next UART opcodes with simulated port I/O.

Uses installed Python z80 and z88dk-z80asm; no physical timing/IRQs are modeled.
Also checks loss propagation in fresh build/BitStream_Next.{tap,map}.
"""
import subprocess
import tempfile
from pathlib import Path

import z80

from test_transfer_cpu import (CPU, FS_MOCKS, ROOT, UI_MOCKS, call_stack, cstr,
                               symbols, tool, transfer_patches, word)

HEAD, TAIL, FAILED, LIMIT = 0xB000, 0xB002, 0xB004, 0xB005
RING, RESULT, SOURCE = 0xA000, 0xB010, 0xC000


class UART:
    def __init__(self, variant, data=b'', busy=0, fault=0, fault_at=1, gap=False):
        self.variant, self.rx = variant, bytearray(data)
        self.busy, self.fault, self.fault_at, self.gap = busy, fault, fault_at, gap
        self.selected = self.samples = self.reads = 0
        self.sent = []

    def input(self, port):
        if self.variant == 'divTiesus':
            assert port == 0xFD3B
            status = self.selected == 0xC7
        else:
            assert port in (0x133B, 0x143B, 0x253B)
            if port == 0x253B:
                return 7  # HDMI video timing
            status = port == 0x133B
        if status:
            self.samples += 1
            ready = bool(self.rx)
            if self.gap and self.reads == 1:
                self.gap, ready = False, False
            busy = self.busy < 0 or self.samples <= self.busy
            if self.variant == 'divTiesus':
                return (0x80 if ready else 0) | (0x40 if busy else 0)
            return (int(ready) | (2 if busy else 0x10) |
                    (self.fault if self.samples == self.fault_at else 0))
        assert self.rx, 'read from an empty UART'
        self.reads += 1
        return self.rx.pop(0)

    def output(self, port, value):
        if self.variant == 'divTiesus':
            if port == 0xFC3B:
                self.selected = value
            else:
                assert port == 0xFD3B and self.selected == 0xC6
                self.sent.append(value)
        elif port == 0x133B:
            self.sent.append(value)
        else:
            assert port in (0x143B, 0x153B, 0x163B, 0x243B)


def run(variant, body, uart, head=0, tail=0, limit=0, enabled=False):
    defs = {'_rb_head': HEAD, '_rb_tail': TAIL, '_uart_tx_failed': FAILED,
            '_uart_drain_limit': LIMIT, '_ring_buffer': RING}
    definitions = ''.join(f'PUBLIC {k}\nDEFC {k} = {v}\n' for k, v in defs.items())
    harness = f'''SECTION code_user
ORG 0x8000
{definitions}
EXTERN _ay_uart_ready, _ay_uart_read, _ay_uart_send, _ay_uart_send_block
EXTERN _uart_send_string, _uart_drain_to_buffer, _ay_uart_init
entry:
    di
    ld sp, 0xFF00
    ld ix, 0xB16B
    ld iy, 0x5C3A
    {'ei' if enabled else 'di'}
{body}
    ld ({RESULT}), hl
    ld ({RESULT + 2}), sp
    push af
    pop hl
    ld ({RESULT + 4}), hl
    ld a, i
    ld a, 0
    jp po, iff_saved
    inc a
iff_saved:
    ld ({RESULT + 6}), a
    ld ({RESULT + 8}), ix
    ld ({RESULT + 10}), iy
    jp 0
'''
    filename = 'divtiesus_uart.asm' if variant == 'divTiesus' else 'next_uart.asm'
    with tempfile.TemporaryDirectory(prefix='bs-uart-') as t:
        tmp = Path(t)
        (tmp / 'h.asm').write_text(harness, encoding='ascii')
        # Copy to keep the assembler's object files outside the source tree.
        (tmp / 'driver.asm').write_bytes((ROOT / 'asm' / filename).read_bytes())
        out = tmp / 'out.bin'
        r = subprocess.run([tool('z88dk-z80asm'), '-b', '-m', '-o' + str(out),
                            str(tmp / 'h.asm'), str(tmp / 'driver.asm')],
                           capture_output=True, text=True)
        assert r.returncode == 0, r.stdout + r.stderr
        labels = symbols(tmp / 'out.map')
        memory = bytearray(65536)
        code = (tmp / 'out_code_user.bin').read_bytes()
        memory[0x8000:0x8000 + len(code)] = code
        memory[HEAD:HEAD + 2] = head.to_bytes(2, 'little')
        memory[TAIL:TAIL + 2] = tail.to_bytes(2, 'little')
        memory[LIMIT] = limit
        memory[RING - 1:RING + 2049] = b'\xA5' * 2050
        memory[SOURCE:SOURCE + 5] = b'ABC\0Z'
        machine = z80.Z80Machine()
        machine.memory[:] = memory
        machine.pc = labels['entry']
        machine.set_breakpoint(0)
        machine.set_input_callback(uart.input)
        machine.set_output_callback(uart.output)
        machine.ticks_to_stop = 20000000
        while machine.pc and machine.ticks_to_stop:
            machine.run()
        assert machine.pc == 0, 'UART did not return within its CPU budget'
        after = bytes(machine.memory)
    assert word(after, RESULT + 2) == 0xFF00, 'stack imbalance'
    assert after[RESULT + 6] == enabled, 'interrupt state changed'
    assert word(after, RESULT + 8) == 0xB16B and word(after, RESULT + 10) == 0x5C3A
    assert after[RING - 1] == after[RING + 2048] == 0xA5, 'ring guards'
    assert word(after, TAIL) == tail, 'producer changed the consumer tail'
    return after


def test_driver(variant):
    for enabled in (False, True):
        uart = UART(variant, b'\x00\xFF')
        a = run(variant, '    call _ay_uart_ready\n    call _ay_uart_ready', uart, enabled=enabled)
        assert a[RESULT] == 1 and uart.reads == 0, 'readiness consumed RX'
        for data in (b'', b'\x00', b'\xFF'):
            uart = UART(variant, data)
            a = run(variant, '    call _ay_uart_read', uart, enabled=enabled)
            assert a[RESULT] == (data[0] if data else 0) and uart.reads == len(data)

    for head, tail, limit in ((0, 0, 0), (2040, 2040, 16), (2047, 1, 16),
                              (100, 102, 255), (2047, 0, 16), (0, 0, 1)):
        data = bytes(range(256))
        uart = UART(variant, data, gap=True)
        a = run(variant, '    call _uart_drain_to_buffer', uart, head, tail, limit)
        count = min((tail - head - 1) & 2047, limit or 255)
        assert uart.reads == count and word(a, HEAD) == (head + count) & 2047
        expected = bytearray(b'\xA5' * 2048)
        for i in range(count):
            expected[(head + i) & 2047] = data[i]
        assert a[RING:RING + 2048] == expected, 'ring order/wrap/free-space limit'

    for full in (False, True):
        uart = UART(variant, b'\x00\xFF', busy=2)
        a = run(variant, '    ld l, 90\n    call _ay_uart_send', uart, 2047 if full else 0)
        assert uart.sent == [90] and uart.reads == (0 if full else 2)
        assert a[FAILED] == 0 and a[RESULT] == 0 and not a[RESULT + 4] & 1
        assert word(a, HEAD) == (2047 if full else 2)

    uart = UART(variant, busy=-1)
    a = run(variant, '    ld l, 90\n    call _ay_uart_send\n    call _ay_uart_send', uart)
    assert uart.samples == (16384 if variant == 'divTiesus' else 65536)
    assert not uart.sent and a[FAILED] == 1 and a[RESULT + 4] & 1, 'bounded fail-stop'
    for body, expected in ((f'    ld hl, {SOURCE}\n    call _uart_send_string', b'ABC'),
                           (f'    ld hl, {SOURCE}\n    push hl\n    ld hl, 5\n    push hl\n'
                            '    call _ay_uart_send_block', b'ABC\0Z')):
        uart = UART(variant)
        run(variant, body, uart)
        assert bytes(uart.sent) == expected, 'string/block contents or callee ABI'
    print(f'PASS {variant}: readiness, binary RX/TX, ring limits/wrap, TX busy/full/timeout, ABI/IFF')


def test_next_faults():
    for fault in (0x04, 0x20, 0x40, 0x80):
        for call in ('_ay_uart_ready', '_ay_uart_read', '_uart_drain_to_buffer', '_ay_uart_send'):
            uart = UART('Next', b'XY', fault=fault)
            a = run('Next', f'    ld l, 90\n    call {call}\n    call _ay_uart_ready', uart)
            assert a[FAILED] == 1 and uart.samples == 1
            assert not uart.reads and not uart.sent and word(a, HEAD) == 0
        uart = UART('Next', b'XY', fault=fault, fault_at=2)
        a = run('Next', '    call _uart_drain_to_buffer', uart)
        assert uart.reads == 1 and a[RING] == ord('X') and word(a, HEAD) == 1
        assert a[FAILED] == 1, 'mid-drain loss was hidden'
        uart = UART('Next', fault=fault)
        a = run('Next', '    call _ay_uart_ready\n    call _ay_uart_init\n'
                '    ld l, 90\n    call _ay_uart_send', uart)
        assert not a[FAILED] and uart.sent == [90], 'reinitialization failed to clear latch'
    print('PASS Next: every RX error bit, all status readers, mid-drain fault, reinitialization')


def test_linked_loss():
    cpu = CPU('Next')
    def setup(mem):
        for name, value in (('_uart_tx_failed', 1), ('_dm_state', 2), ('_dm_left', 3),
                            ('_ctrl_ready', 1), ('_ev_flags', 0x80), ('_rb_head', 3)):
            mem[cpu.sym[name]] = value
        mem[cpu.sym['_xfer_code']:cpu.sym['_xfer_code'] + 2] = (226).to_bytes(2, 'little')
        mem[cpu.sym['_ring_buffer']:cpu.sym['_ring_buffer'] + 3] = b'abc'
        mem[0x5300:0x5304] = b'\xA5' * 4
    body = call_stack('_ftp_xfer_read', (0x5300, 3)) + '    ld (0x5000), hl\n'
    a = cpu.run(body, 'mock_ret:\n    ret\n', {'_uart_drain_to_buffer': 'mock_ret'}, setup)
    assert word(a, 0x5000) == 65534 and word(a, cpu.sym['_xfer_code']) == 2
    assert not a[cpu.sym['_ctrl_ready']] and a[cpu.sym['_ev_flags']] == 2
    assert word(a, cpu.sym['_rb_tail']) == 0 and a[0x5300:0x5304] == b'\xA5' * 4
    print('PASS linked Next: loss rejects buffered binary data and stale 226/CLOSED')

    patches = transfer_patches(cpu)
    del patches['_ftp_xfer_read']  # run the real FTP reader and ESP demultiplexer
    patches.update(_download_request_size='mock_zero32', _fs_commit='mock_commit',
                   _uart_drain_to_buffer='mock_ret', _ftp_transfer_abort='mock_ret')
    def download_setup(mem):
        setup(mem)
        mem[0x5300:0x5306] = b'X.TXT\0'
        mem[0x5014] = 1
    body = call_stack('_download_file_core', (0x5300, 0x5300, 1, 1, 0x5340))
    a = cpu.run(body + '    ld (0x5000), hl\n', UI_MOCKS + FS_MOCKS,
                patches, download_setup)
    assert not a[0x5000] and not a[0x5013] and a[0x5012] == 1, 'failed GET committed a file'
    assert cstr(a, 0x5120) == b'X.TXT', 'failed GET removed the wrong file'
    print('PASS linked Next: RX loss removes the partial GET without SIZE, despite cached 226')


if __name__ == '__main__':
    for variant in ('divTiesus', 'Next'):
        test_driver(variant)
    test_next_faults()
    test_linked_loss()
    print('ALL UART CPU TESTS PASSED (physical UART timing untested)')
