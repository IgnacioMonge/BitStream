#!/usr/bin/env python3
"""Exercise linked classic ESP close paths with deterministic UART seams.

Real close, demultiplexer, ring operations and FTP final-reply decisions run.
UART RX is preloaded or released one byte per drain. wait_poll records scheduled
frames and polls without HALT; FTP/AT wait HALTs are NOPed for interrupt-free CPU
execution. Cycles exclude physical UART and interrupt/frame timing.
"""
import argparse
from pathlib import Path

from test_transfer_cpu import CPU, RESULT, call_stack, cstr, word

VARIANTS = ('divTiesus', 'AY', 'Next')
WAIT, TX, DRAINS, KEYS, FAILS = 0x5010, 0x5012, 0x5014, 0x5016, 0x5018
LEFT, PTR, DELAY, FEED, OUTPUT = 0x5020, 0x5022, 0x5024, 0x5400, 0x5300

MOCKS = f'''mock_zero:
    ld hl, 0
    ret
mock_tx:
    ld a, ({TX})
    inc a
    ld ({TX}), a
    ret
mock_key:
    ld hl, ({KEYS})
    inc hl
    ld ({KEYS}), hl
    ld hl, 0
    ret
mock_fail:
    ld a, ({FAILS})
    inc a
    ld ({FAILS}), a
    ret
mock_wait:
    ld bc, ({WAIT})
    add hl, bc
    ld ({WAIT}), hl
    or a
    sbc hl, bc
wait_loop:
    ld a, h
    or l
    ret z
    push hl
    call _net_poll
    pop hl
    dec hl
    jr wait_loop
mock_drain:
    push af
    push bc
    push de
    push hl
    ld hl, ({DRAINS})
    inc hl
    ld ({DRAINS}), hl
    ld a, ({DELAY})
    or a
    jr z, drain_feed
    dec a
    ld ({DELAY}), a
    jr drain_done
drain_feed:
    ld hl, ({LEFT})
    ld a, h
    or l
    jr z, drain_done
    dec hl
    ld ({LEFT}), hl
    ld hl, ({PTR})
    ld a, (hl)
    inc hl
    ld ({PTR}), hl
    ld hl, (_rb_head)
    ld de, _ring_buffer
    add hl, de
    ld (hl), a
    ld hl, (_rb_head)
    inc hl
    ld a, h
    and 7
    ld h, a
    ld (_rb_head), hl
drain_done:
    pop hl
    pop de
    pop bc
    pop af
    ret
'''
PATCHES = {'_uart_drain_to_buffer': 'mock_drain', '_ay_uart_ready': 'mock_zero',
           '_uart_send_string': 'mock_tx', '_key_break_down': 'mock_key',
           '_wait_poll': 'mock_wait', '_fail': 'mock_fail',
           '_print_reply': 'mock_zero'}


def ipd(link, payload):
    return f'+IPD,{link},{len(payload)}:'.encode() + payload


def put_word(mem, addr, value):
    mem[addr:addr + 2] = value.to_bytes(2, 'little')


def run(cpu, body, ring=b'', feed=b'', delay=0, extra_mocks='', extra_patches=None):
    def setup(mem):
        # Keep the parser/wait loops intact, replacing their hardware timing seam.
        for first, last in (('_ftp_wait_reply', '_ftp_command'),
                            ('_esp_wait_ev', '_esp_wait_prompt'),
                            ('_ftp_xfer_read', '_ftp_transfer_end')):
            start, end = cpu.sym[first], cpu.sym[last]
            assert start < end, f'unexpected function order: {first}/{last}'
            count = 0
            for pos in range(start, end - 1):
                if mem[pos:pos + 2] == b'\xfb\x76':
                    mem[pos:pos + 2] = b'\x00\x00'
                    count += 1
            assert count == 1, f'expected one EI/HALT in {first}, got {count}'
        assert len(ring) + len(feed) < 2048 and len(feed) <= 512
        mem[cpu.sym['_ring_buffer']:cpu.sym['_ring_buffer'] + len(ring)] = ring
        put_word(mem, cpu.sym['_rb_head'], len(ring))
        put_word(mem, cpu.sym['_rb_tail'], 0)
        mem[FEED:FEED + len(feed)] = feed
        put_word(mem, PTR, FEED)
        put_word(mem, LEFT, 0)  # The harness enables RX after its initial poll.
        mem[DELAY] = delay
        mem[cpu.sp - 512:cpu.sp] = bytes([0xD3]) * 512
    return cpu.run(body, MOCKS + extra_mocks, {**PATCHES, **(extra_patches or {})}, setup)


def enable_feed(data):
    return f'    ld hl, {len(data)}\n    ld ({LEFT}), hl\n'


def assert_drained(cpu, mem):
    assert word(mem, cpu.sym['_rb_head']) == word(mem, cpu.sym['_rb_tail']), 'unread ring bytes'
    assert mem[cpu.sym['_dm_state']] == 0 and word(mem, cpu.sym['_dm_left']) == 0, 'unfinished IPD frame'
    assert mem[cpu.sym['_dm_discard']] == 1, 'closed data must remain in discard mode'


def report(cpu, case, mem):
    used = next((cpu.sp - pos for pos in range(cpu.sp - 512, cpu.sp)
                 if mem[pos] != 0xD3), 0)
    print(f'PASS {cpu.variant} {case}: stack_bytes={used} scheduled_wait_frames={word(mem, WAIT)} '
          f'cpu_cycles={cpu.cycles} uart_drains={word(mem, DRAINS)}')


def test_closed(cpu, expected_wait):
    # CLOSED is discovered by the real parser, not by writing its event flag.
    mem = run(cpu, '    call _net_poll\n    call _net_data_close\n', b'1,CLOSED\r\n')
    assert mem[cpu.sym['_ev_flags']] & 0x80
    assert word(mem, WAIT) == expected_wait and mem[TX] == 0, 'already-closed path'
    assert_drained(cpu, mem)
    report(cpu, 'already-CLOSED', mem)


def test_open_and_failed_close(cpu):
    for at_reply in (b'OK\r\n', b'ERROR\r\n'):
        final = b'226 complete\r\n'
        stream = ipd(1, b'UNREAD') + at_reply + ipd(0, final)
        mem = run(cpu, f'    call _ftp_transfer_end\n    ld ({RESULT}), hl\n', stream)
        assert word(mem, RESULT) == 226, 'close must retain final FTP reply'
        assert word(mem, WAIT) == 5 and mem[TX] == 2, 'open/error close must retain five-frame drain'
        assert cstr(mem, cpu.sym['_at_buffer']) == b'AT+CIPCLOSE=1'
        assert mem[cpu.sym['_ev_flags']] & 0x80
        assert_drained(cpu, mem)
        report(cpu, 'close-' + at_reply.strip().decode(), mem)


def test_fragmented_final(cpu, expected_wait):
    for code in (226, 250):
        final = f'{code} complete\r\n'.encode()
        feed = ipd(0, final)
        # Initial poll stops before UNREAD. Closing must enable its discard;
        # a final reply arriving after several empty polls must still complete.
        ring = b'1,CLOSED\r\n' + ipd(1, b'UNREAD')
        body = '    call _net_poll\n' + enable_feed(feed) + f'''    call _ftp_transfer_end
    ld ({RESULT}), hl
'''
        mem = run(cpu, body, ring, feed, delay=8)
        assert word(mem, RESULT) == code, 'fragmented/delayed final reply'
        assert word(mem, WAIT) == expected_wait and mem[TX] == 0
        assert word(mem, LEFT) == 0 and word(mem, KEYS) > 0, 'final must arrive after idle polls'
        assert cstr(mem, cpu.sym['_rx_line']) == final.rstrip(b'\r\n')
        assert_drained(cpu, mem)
        report(cpu, f'delayed-bytewise-{code}', mem)


def test_consumed_final(cpu, expected_finished_wait):
    for code in (226, 250):
        final = f'{code} complete\r\n'.encode()
        # Real caller lifecycle: consume data, then stop at the FIRST EOF.
        # The fragmented final precedes CLOSED; idle reader HALTs use the seam.
        ring = ipd(1, b'DATA') + ipd(0, final)[:-5]
        feed = final[-5:] + b'1,CLOSED\r\n'
        body = '    call _net_poll\n' + enable_feed(feed) + 'read_final:\n'
        body += call_stack('_ftp_xfer_read', (OUTPUT, 4))
        body += f'''    ld ({RESULT + 6}), hl
    bit 7, h
    jr nz, first_eof
    ld a, h
    or l
    jr z, read_final
    ld de, ({RESULT + 10})
    add hl, de
    ld ({RESULT + 10}), hl
    jr read_final
first_eof:
    ld hl, (_xfer_code)
    ld ({RESULT + 8}), hl
    call _ftp_transfer_end
    ld ({RESULT}), hl
'''
        mem = run(cpu, body, ring, feed, delay=8)
        assert word(mem, RESULT + 6) == 65535, 'caller must stop at first NET_EOF'
        assert word(mem, RESULT + 8) == code, 'final already cached at first EOF'
        assert word(mem, RESULT + 10) == 4 and mem[OUTPUT:OUTPUT + 4] == b'DATA'
        assert word(mem, RESULT) == code and word(mem, cpu.sym['_xfer_code']) == code
        assert word(mem, WAIT) == expected_finished_wait and mem[TX] == 0
        assert word(mem, LEFT) == 0 and word(mem, KEYS) > 0
        assert_drained(cpu, mem)
        report(cpu, f'consumed-bytewise-{code}', mem)


def test_finish_code_gate(cpu, expected_wait, expected_finished_wait):
    # Include pseudo-codes and non-transfer successes: only 226/250 qualify.
    for code in (0, 1, 200, 226, 250, 426):
        for closed in (False, True):
            ring = (b'1,CLOSED\r\n' if closed else b'') + b'OK\r\n'
            body = f'''    call _net_poll
    ld hl, {code}
    ld (_xfer_code), hl
    call _ftp_transfer_end
    ld ({RESULT}), hl
'''
            mem = run(cpu, body, ring)
            assert word(mem, RESULT) == code
            fast = closed and code in (226, 250)
            assert word(mem, WAIT) == (expected_finished_wait if fast else expected_wait)
            assert mem[TX] == (0 if closed else 2), 'open socket must actively close'
            assert_drained(cpu, mem)
            report(cpu, f'cached-{code}-' + ('CLOSED' if closed else 'open'), mem)


def inject(stream, label):
    # Run-time fixture delivery between calls; actual framing remains untouched.
    body = f'''    ld hl, {label}
    ld de, _ring_buffer
    ld bc, {len(stream)}
    ldir
    ld hl, {len(stream)}
    ld (_rb_head), hl
    ld hl, 0
    ld (_rb_tail), hl
'''
    data = label + ':\n    defb ' + ','.join(str(b) for b in stream) + '\n'
    return body, data


def test_cancel_reopen(cpu, expected_wait, expected_finished_wait):
    connect, connect_data = inject(b'1,CONNECT\r\nOK\r\n', 'connect_stream')
    next_stream = ipd(1, b'NEW!') + b'1,CLOSED\r\n' + ipd(0, b'250 complete\r\n')
    receive, receive_data = inject(next_stream, 'receive_stream')
    body = '    call _net_poll\n' + call_stack('_ftp_transfer_abort', (65533,))
    body += '''    call _net_ctrl_discard
''' + connect
    body += call_stack('_net_data_open', (0x5350, 1234))
    body += f'''    ld ({RESULT + 6}), hl
''' + receive
    body += call_stack('_net_data_read', (OUTPUT, 4))
    body += f'''    ld ({RESULT + 8}), hl
'''
    body += call_stack('_ftp_xfer_read', (OUTPUT + 4, 4))
    body += f'''    ld ({RESULT + 10}), hl
    call _ftp_transfer_end
    ld ({RESULT}), hl
    jp fixture_end
''' + connect_data + receive_data + 'fixture_end:\n'
    # A previous abort reply must not answer the next transfer.
    ring = b'1,CLOSED\r\n' + ipd(1, b'OLD!') + ipd(0, b'426 cancelled\r\n')
    mem = run(cpu, body, ring)
    assert mem[cpu.sym['_g_user_cancel']] == 1 and mem[FAILS] == 1, 'record cancelled transfer'
    assert word(mem, RESULT + 6) == 1, 'reopen data connection'
    assert word(mem, RESULT + 8) == 4 and mem[OUTPUT:OUTPUT + 4] == b'NEW!', 'reopened link accepts data'
    assert word(mem, RESULT + 10) == 65535 and word(mem, RESULT) == 250, 'new transfer EOF/final reply'
    assert word(mem, WAIT) == expected_wait + expected_finished_wait and mem[TX] == 2
    assert_drained(cpu, mem)
    report(cpu, 'cancel-discard-reopen-next-data', mem)



def test_late_final_before_next_command(cpu, expected_wait, abort=None):
    # A bounded transport probe: a previous 250 arrives on the second poll
    # after close, while the real next command has its own delayed 200 reply.
    old = ipd(0, b'250 old complete\r\n')
    prompt = b'> '
    genuine = ipd(0, b'200 NOOP complete\r\n')
    pending = 0x5026
    mocks = f'''probe_drain:
    push af
    push bc
    push de
    push hl
    ld hl, ({DRAINS})
    inc hl
    ld ({DRAINS}), hl
    ld hl, ({LEFT})
    ld a, h
    or l
    jr z, probe_drain_done
    ld a, ({DELAY})
    or a
    jr z, probe_old
    dec a
    ld ({DELAY}), a
    jr probe_drain_done
probe_old:
    ld hl, 0
    ld ({LEFT}), hl
    ld hl, probe_old_bytes
    ld bc, {len(old)}
    call probe_append
probe_drain_done:
    pop hl
    pop de
    pop bc
    pop af
    ret
probe_tx:
    ld a, ({TX})
    inc a
    ld ({TX}), a
    cp 2
    ret nz
    ld hl, probe_prompt_bytes
    ld bc, {len(prompt)}
    jp probe_append
probe_send:
    pop hl
    pop bc
    pop bc
    push hl
    ld a, 1
    ld ({pending}), a
    ret
probe_key:
    ld a, ({pending})
    or a
    jr z, probe_key_done
    xor a
    ld ({pending}), a
    ld hl, probe_genuine_bytes
    ld bc, {len(genuine)}
    call probe_append
probe_key_done:
    ld hl, 0
    ret
probe_append:
    ld de, (_rb_head)
    push de
    push bc
    push hl
    ld hl, _ring_buffer
    add hl, de
    ex de, hl
    pop hl
    ldir
    pop bc
    pop hl
    add hl, bc
    ld (_rb_head), hl
    ret
probe_old_bytes:
    defb {','.join(str(b) for b in old)}
probe_prompt_bytes:
    defb {','.join(str(b) for b in prompt)}
probe_genuine_bytes:
    defb {','.join(str(b) for b in genuine)}
'''
    body = '    call _net_poll\n' + enable_feed(old)
    if abort is None:
        body += '    call _net_data_close\n'
    else:
        body += '    ld hl, 426\n    ld (_xfer_code), hl\n'
        body += call_stack('_ftp_transfer_abort', (65536 + abort,))
    body += call_stack('_ftp_cmd_reply', ('probe_command', 10))
    body += f'''    ld ({RESULT}), hl
    jp probe_fixture_end
probe_command:
    defb 78,79,79,80,0
probe_fixture_end:
'''
    patches = {'_uart_drain_to_buffer': 'probe_drain',
               '_uart_send_string': 'probe_tx', '_ay_uart_send_block': 'probe_send',
               '_key_break_down': 'probe_key'}
    mem = run(cpu, body, b'1,CLOSED\r\n', delay=1,
              extra_mocks=mocks, extra_patches=patches)
    print(f'PROBE {cpu.variant} abort={abort}: next_reply={word(mem, RESULT)} '
          f'expected=200 scheduled_wait_frames={word(mem, WAIT)} '
          f'cpu_cycles={cpu.cycles} genuine_reply_pending={mem[pending]}')
    assert word(mem, WAIT) == expected_wait
    assert word(mem, RESULT) == 200, 'late previous 250 answered next real NOOP command'
    assert mem[pending] == 0, 'genuine next-command reply was consumed'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe-late-final', action='store_true', help='run only the bounded late-250 next-command probe')
    parser.add_argument('--build-dir', type=Path, help='directory containing linked TAP/map snapshots')
    parser.add_argument('--variant', choices=VARIANTS, action='append', help='default: classic ESP builds')
    parser.add_argument('--expect-finished-wait', type=int, choices=(0, 5), default=0)
    parser.add_argument('--expect-closed-wait', type=int, choices=(0, 5), default=5)
    args = parser.parse_args()
    for variant in args.variant or VARIANTS:
        cpu = CPU(variant, args.build_dir)
        if args.probe_late_final:
            for abort in (None, -2, -3):
                test_late_final_before_next_command(cpu, args.expect_closed_wait, abort)
            continue
        test_closed(cpu, args.expect_closed_wait)
        test_open_and_failed_close(cpu)
        test_fragmented_final(cpu, args.expect_closed_wait)
        test_consumed_final(cpu, args.expect_finished_wait)
        test_finish_code_gate(cpu, args.expect_closed_wait, args.expect_finished_wait)
        test_cancel_reopen(cpu, args.expect_closed_wait, args.expect_finished_wait)
        for abort in (None, -2, -3):
            test_late_final_before_next_command(cpu, args.expect_closed_wait, abort)
    print('ALL ESP CLOSE CPU TESTS PASSED (hardware timing excluded; Spectranext uses another backend)')


if __name__ == '__main__':
    main()
