; -----------------------------------------------------------------------------
; divtiesus_uart.asm - UART backend for divMMC/divTIESUS (ZX-Uno register file)
; BitStream FTP Client
;
; Ports: $FC3B selects a register, $FD3B reads/writes it. With C=$3B the
; switch is just "inc b" / "dec b". Registers: $C6 data, $C7 status
; (bit 7 = RX byte waiting, bit 6 = TX busy).
;
; Contracts (ported from SpecTalkZX / NetChessZX):
;   - Status-only readiness: polling never consumes data, no prefetch latch.
;   - Defensive read: the data register is read only after a fresh
;     RX-ready status sample.
;   - No DI/EI anywhere: the caller's interrupt state is preserved.
;   - TX is bounded. Budget exhaustion latches _uart_tx_failed (fail-stop):
;     no later byte is sent until _ay_uart_init clears it, so an AT or FTP
;     command is never transmitted with holes in it.
;   - While TX is busy, waiting RX bytes are moved into the ring (when it is
;     not full), so a transmit stalled by flow control cannot overrun RX.
;   - _uart_drain_to_buffer is inline (~155 T/byte, was ~450 T/byte; a byte
;     arrives every ~304 T at 115200 baud). The ring-free count bounds the
;     budget up front, so a byte is never read without room to store it.
; -----------------------------------------------------------------------------

SECTION code_user

PUBLIC _ay_uart_init
PUBLIC _ay_uart_send
PUBLIC _ay_uart_send_block
PUBLIC _ay_uart_read
PUBLIC _ay_uart_ready
PUBLIC _ay_uart_ready_fast
PUBLIC _uart_drain_to_buffer
PUBLIC _uart_send_string

EXTERN _uart_tx_failed
EXTERN _uart_drain_limit
EXTERN _ring_buffer
EXTERN _rb_head
EXTERN _rb_tail

UART_DATA_REG  EQU 0xC6
UART_STAT_REG  EQU 0xC7
ZXUNO_ADDR     EQU 0xFC3B
RING_SIZE      EQU 2048
TX_BUDGET      EQU 0x4000        ; status samples per byte (~0.2 s)

; -----------------------------------------------------------------------------
; uartRead - internal. CF=1 and A=byte if one was waiting, CF=0 otherwise.
; Clobbers A, BC. (IN r,(C), OUT and DEC B preserve CF.)
; -----------------------------------------------------------------------------
uartRead:
    ld bc, ZXUNO_ADDR
    ld a, UART_STAT_REG
    out (c), a
    inc b
    in a, (c)
    add a, a                ; RX-ready -> CF
    ret nc
    dec b
    ld a, UART_DATA_REG
    out (c), a
    inc b
    in a, (c)
    ret

; -----------------------------------------------------------------------------
; void ay_uart_init(void)
; Clears the TX fail-stop latch and primes both registers. Startup waits and
; RX flushing belong to the transport (smart_init).
; -----------------------------------------------------------------------------
_ay_uart_init:
    xor a
    ld (_uart_tx_failed), a
    ld bc, ZXUNO_ADDR
    ld a, UART_STAT_REG
    out (c), a
    inc b
    in a, (c)
    dec b
    ld a, UART_DATA_REG
    out (c), a
    inc b
    in a, (c)
    ret

; -----------------------------------------------------------------------------
; uint8_t ay_uart_send(uint8_t byte) __z88dk_fastcall      L = byte
; Returns L=0 (CF=0) when sent, L=1 (CF=1) when failed or latched.
; -----------------------------------------------------------------------------
_ay_uart_send:
    ld a, (_uart_tx_failed)
    or a
    jr nz, us_failed
    ld de, TX_BUDGET
    ld bc, ZXUNO_ADDR
us_select:
    ld a, UART_STAT_REG
    out (c), a
    inc b                   ; B=$FD: status register stays selected
us_poll:
    in a, (c)
    bit 6, a
    jr z, us_ready          ; TX idle
    add a, a                ; RX-ready -> CF
    jr nc, us_count
    call us_rx              ; move one RX byte into the ring (if room)
    dec b
    jr us_count_sel
us_count:
    dec de
    ld a, d
    or e
    jr nz, us_poll
    jr us_timeout
us_count_sel:
    dec de
    ld a, d
    or e
    jr nz, us_select
us_timeout:
    ld a, 1
    ld (_uart_tx_failed), a
us_failed:
    ld l, 1
    scf
    ret

us_ready:
    dec b
    ld a, UART_DATA_REG
    out (c), a
    inc b
    out (c), l
    ld l, 0
    or a                    ; CF=0
    ret

; Move one waiting RX byte into the ring unless the ring is full (then the
; byte stays in the UART). In: B=$FD, C=$3B. Out: B=$FD. Preserves DE, HL.
us_rx:
    push hl
    push de
    ld hl, (_rb_head)
    ld de, (_rb_tail)
    push hl
    inc hl
    res 3, h                ; (head + 1) & 0x07FF
    or a
    sbc hl, de
    pop hl                  ; HL = head (POP keeps flags)
    jr z, us_rx_done        ; full
    dec b
    ld a, UART_DATA_REG
    out (c), a
    inc b
    in a, (c)
    ex de, hl               ; DE = head
    ld hl, _ring_buffer
    add hl, de
    ld (hl), a
    inc de
    res 3, d
    ld (_rb_head), de
us_rx_done:
    pop de
    pop hl
    ret

; -----------------------------------------------------------------------------
; void ay_uart_send_block(void *buf, uint16_t len) __z88dk_callee
; sccz80 pushes left-to-right: SP+2=len, SP+4=buf. Stops at the first
; unsent byte (check uart_tx_failed).
; -----------------------------------------------------------------------------
_ay_uart_send_block:
    pop bc                  ; return address
    pop de                  ; len
    pop hl                  ; buf
    push bc
usb_loop:
    ld a, d
    or e
    ret z
    push hl
    push de
    ld l, (hl)
    call _ay_uart_send
    pop de
    pop hl
    ret c
    inc hl
    dec de
    jr usb_loop

; -----------------------------------------------------------------------------
; void uart_send_string(const char *s) __z88dk_fastcall
; Stops at the first unsent byte.
; -----------------------------------------------------------------------------
_uart_send_string:
    ld a, (hl)
    or a
    ret z
    push hl
    ld l, a
    call _ay_uart_send
    pop hl
    ret c
    inc hl
    jr _uart_send_string

; -----------------------------------------------------------------------------
; uint8_t ay_uart_ready(void) - L=1 if a byte is waiting. Status only.
; -----------------------------------------------------------------------------
_ay_uart_ready:
_ay_uart_ready_fast:
    ld bc, ZXUNO_ADDR
    ld a, UART_STAT_REG
    out (c), a
    inc b
    in a, (c)
    rlca
    and 1
    ld l, a
    ld h, 0
    ret

; -----------------------------------------------------------------------------
; uint8_t ay_uart_read(void) - L=byte if one was waiting, else L=0.
; -----------------------------------------------------------------------------
_ay_uart_read:
    call uartRead
    ld h, 0
    ld l, a
    ret c
    ld l, h
    ret

; -----------------------------------------------------------------------------
; void uart_drain_to_buffer(void)
; Moves up to min(uart_drain_limit (0 = 255), ring free) bytes from the UART
; into ring_buffer. After the first byte it keeps polling for ~1.3 byte times
; before declaring the line idle, so back-to-back bytes are not split across
; calls. Uses the shadow set for the ring pointer (nothing else keeps state
; there; unpack_glyph reloads it on every call).
; -----------------------------------------------------------------------------
_uart_drain_to_buffer:
    ld hl, (_rb_tail)
    ld de, (_rb_head)
    scf
    sbc hl, de              ; tail - head - 1
    ld a, h
    and 0x07
    ld h, a                 ; HL = free bytes
    or l
    ret z                   ; full: leave bytes in the UART
    ld a, (_uart_drain_limit)
    or a
    jr nz, dr_lim
    dec a                   ; 0 -> 255
dr_lim:
    ld c, a                 ; C = budget
    ld a, h
    or a
    jr nz, dr_have          ; free >= 256
    ld a, l
    cp c
    jr nc, dr_have          ; free >= budget
    ld c, a                 ; budget = free
dr_have:
    push bc                 ; C = budget
    ld hl, RING_SIZE
    or a
    sbc hl, de              ; bytes until wrap
    push hl
    ld hl, _ring_buffer
    add hl, de              ; write pointer
    pop bc
    exx                     ; shadow: HL'=write ptr, BC'=bytes until wrap
    pop de
    ld d, e                 ; D = budget
    ld e, 0                 ; E = moved at least one byte
    ld bc, ZXUNO_ADDR
dr_poll:
    ld a, UART_STAT_REG
    out (c), a
    inc b
    in a, (c)
    dec b
    add a, a                ; RX-ready -> CF
    jr nc, dr_empty
dr_read:
    ld a, UART_DATA_REG
    out (c), a
    inc b
    in a, (c)
    dec b
    exx
    ld (hl), a
    inc hl
    dec bc
    ld a, b
    or c
    jr z, dr_wrap
dr_stored:
    exx
    ld e, 1
    dec d
    jr nz, dr_poll
    jr dr_commit
dr_wrap:
    ld hl, _ring_buffer
    ld bc, RING_SIZE
    jr dr_stored

dr_empty:
    ld a, e
    or a
    ret z                   ; nothing moved: head unchanged
    ld l, 6
dr_wait:
    ld a, UART_STAT_REG
    out (c), a
    inc b
    in a, (c)
    dec b
    add a, a
    jr c, dr_read
    dec l
    jr nz, dr_wait
dr_commit:
    exx
    ld de, _ring_buffer
    or a
    sbc hl, de
    ld (_rb_head), hl
    ret
