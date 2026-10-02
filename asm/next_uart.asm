; -----------------------------------------------------------------------------
; next_uart.asm - UART backend for the ZX Spectrum Next internal ESP
; BitStream FTP Client
;
; Same interface and contracts as divtiesus_uart.asm (status-only readiness,
; defensive read, no DI/EI, fail-stop bounded TX, RX moved into the ring while
; TX waits, inline drain). Port map and baud table from SpecTalkZX/NetChessZX
; next_uart.asm:
;   $133B  read: status (bit 0 RX FIFO not empty, bit 1 TX FIFO full)
;          write: TX byte
;   $143B  read: RX byte; write: baud prescaler (low 7 bits, then bit 7 set +
;          bits 7..13)
;   $153B  UART select ($30 = ESP, prescaler bits 14..16 = 0)
;   $163B  frame format ($98 reset + 8N1, then $18 = 8N1)
; With BC = $133B the RX/baud port is just "inc b".
; The status error flags (framing/overflow, clear-on-read) are not used: a
; lost byte surfaces as a protocol error or a SIZE mismatch upstream.
;
; Also: _next_platform_init sets the CPU to 28 MHz. Polling bounds below are
; sized for 28 MHz and stay safe (only longer) at 3.5 MHz.
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
PUBLIC _next_platform_init
IFDEF BITSTREAM_DEBUG_RX
PUBLIC _uart_dbg_status         ; OR of every drain status sample
PUBLIC _uart_dbg_bytes          ; bytes the drain read from the UART
SECTION bss_user
_uart_dbg_status: defs 1
_uart_dbg_bytes:  defs 2
SECTION code_user
ENDIF

EXTERN _uart_tx_failed
EXTERN _uart_drain_limit
EXTERN _ring_buffer
EXTERN _rb_head
EXTERN _rb_tail

UART_STATUS          EQU 0x133B
UART_SELECT          EQU 0x153B
UART_SELECT_ESP      EQU 0x30
UART_FRAME_RESET_8N1 EQU 0x98
UART_FRAME_8N1       EQU 0x18
NEXTREG_SELECT       EQU 0x243B
NEXTREG_CPU_SPEED    EQU 0x07
NEXTREG_VIDEO_TIMING EQU 0x11
RING_SIZE            EQU 2048
DRAIN_TAIL_POLLS     EQU 80     ; ~1.3 byte times at 115200 baud, 28 MHz

; -----------------------------------------------------------------------------
; void next_platform_init(void) - CPU at 28 MHz (nextreg $07 = 3).
; -----------------------------------------------------------------------------
_next_platform_init:
    ld bc, NEXTREG_SELECT
    ld a, NEXTREG_CPU_SPEED
    out (c), a
    inc b
    ld a, 3
    out (c), a
    ret

; -----------------------------------------------------------------------------
; uartRead - internal. CF=1 and A=byte if one was waiting, CF=0 otherwise.
; Clobbers A, BC.
; -----------------------------------------------------------------------------
uartRead:
    ld bc, UART_STATUS
    in a, (c)
    rrca                    ; RX ready -> CF
    ret nc
    inc b
    in a, (c)
    ret

; -----------------------------------------------------------------------------
; void ay_uart_init(void)
; Clears the TX fail-stop latch, selects the ESP, 8N1, 115200 baud for the
; active video timing. Startup waits and RX flushing belong to the transport.
; -----------------------------------------------------------------------------
_ay_uart_init:
    xor a
    ld (_uart_tx_failed), a
    ld bc, UART_SELECT
    ld a, UART_SELECT_ESP
    out (c), a
    inc b                   ; $163B
    ld a, UART_FRAME_RESET_8N1
    out (c), a
    ld a, UART_FRAME_8N1
    out (c), a

    ; Divisor = table[nextreg $11 & 7]. Only this code touches the nextreg
    ; select port and the IM2 frame ISR does not, so no DI is needed.
    ld bc, NEXTREG_SELECT
    ld a, NEXTREG_VIDEO_TIMING
    out (c), a
    inc b
    in a, (c)
    and 7
    add a, a
    ld e, a
    ld d, 0
    ld hl, baud_115200
    add hl, de
    ld e, (hl)
    inc hl
    ld d, (hl)              ; DE = divisor
    ld bc, UART_STATUS + 0x0100
    ld a, e
    and 0x7F
    out (c), a
    ld a, d
    rl e
    rla
    or 0x80
    out (c), a
    ret

; 115200 baud divisors for nextreg $11 timings VGA0..VGA6, HDMI
baud_115200:
    defw 243, 248, 256, 260, 269, 278, 286, 234

; -----------------------------------------------------------------------------
; uint8_t ay_uart_send(uint8_t byte) __z88dk_fastcall      L = byte
; Returns L=0 (CF=0) when sent, L=1 (CF=1) when failed or latched.
; Budget: 65536 status samples (~0.15 s at 28 MHz, ~1 s at 3.5 MHz).
; -----------------------------------------------------------------------------
_ay_uart_send:
    ld a, (_uart_tx_failed)
    or a
    jr nz, us_failed
    ld de, 0
    ld bc, UART_STATUS
us_poll:
    in a, (c)
    bit 1, a
    jr z, us_ready          ; TX FIFO has room
    rrca                    ; RX ready -> CF
    call c, us_rx
    dec de
    ld a, d
    or e
    jr nz, us_poll
    ld a, 1
    ld (_uart_tx_failed), a
us_failed:
    ld l, 1
    scf
    ret
us_ready:
    out (c), l
    ld l, 0
    or a                    ; CF=0
    ret

; Move one waiting RX byte into the ring unless the ring is full (then the
; byte stays in the UART). In/out: BC = $133B. Preserves DE, HL.
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
    inc b
    in a, (c)
    dec b
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
    ld bc, UART_STATUS
    in a, (c)
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
; into ring_buffer; after the first byte it keeps polling ~1.3 byte times
; before declaring the line idle. Shadow HL' = write pointer, BC' = bytes
; until the ring wraps (same scheme as divtiesus_uart.asm).
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
    ld bc, UART_STATUS
dr_poll:
    in a, (c)
IFDEF BITSTREAM_DEBUG_RX
    push af
    push hl
    ld hl, _uart_dbg_status
    or (hl)
    ld (hl), a
    pop hl
    pop af
ENDIF
    rrca                    ; RX ready -> CF
    jr nc, dr_empty
dr_read:
    inc b
    in a, (c)
    dec b
IFDEF BITSTREAM_DEBUG_RX
    push hl
    ld hl, (_uart_dbg_bytes)
    inc hl
    ld (_uart_dbg_bytes), hl
    pop hl
ENDIF
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
    ld l, DRAIN_TAIL_POLLS
dr_wait:
    in a, (c)
IFDEF BITSTREAM_DEBUG_RX
    push af
    push hl
    ld hl, _uart_dbg_status
    or (hl)
    ld (hl), a
    pop hl
    pop af
ENDIF
    rrca
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
