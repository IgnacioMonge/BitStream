; -----------------------------------------------------------------------------
; divmmc_uart.asm - Optimized UART backend for divMMC/divTIESUS
; BitStream FTP Client
;
; The divTIESUS Maple Edition implements a ZX-Uno register file accessed through
; ports 0xFC3B (register select) and 0xFD3B (register read/write). Its UART is
; exposed in registers 0xC6 (data) and 0xC7 (status).
;
; OPTIMIZATION: inc b to switch FC3B -> FD3B (1 byte/4T vs 3 bytes/10T)
; FIX: DI/EI in uartRead to protect shared flags from race conditions
; -----------------------------------------------------------------------------

SECTION code_user

PUBLIC _ay_uart_init
PUBLIC _ay_uart_send
PUBLIC _ay_uart_send_block
PUBLIC _ay_uart_read
PUBLIC _ay_uart_ready
PUBLIC _ay_uart_ready_fast

; ZX-Uno compatible register interface
UART_DATA_REG     EQU 0xC6
UART_STAT_REG     EQU 0xC7
UART_BYTE_RECIVED EQU 0x80
UART_BYTE_SENDING EQU 0x40
ZXUNO_ADDR        EQU 0xFC3B
ZXUNO_REG         EQU 0xFD3B

SECTION bss_user

_poked_byte: DEFS 1
_byte_buff:  DEFS 1
_is_recv:    DEFS 1

SECTION code_user

; -----------------------------------------------------------------------------
; Internal helper: uartRead
;   Returns: CF=1 if a byte was read, A=byte
;            CF=0 if nothing to read
;   Unified path for buffered, latched, and hardware reads.
;   Protected by DI/EI against interrupt race conditions.
; -----------------------------------------------------------------------------
uartRead:
    di

    ld a, (_poked_byte)
    or a
    jr nz, uartRead_retBuff

    ld a, (_is_recv)
    or a
    jr nz, uartRead_do_read

    ; Check hardware status
    ld bc, ZXUNO_ADDR
    ld a, UART_STAT_REG
    out (c), a
    inc b                   ; FC3B -> FD3B
    in a, (c)
    and UART_BYTE_RECIVED
    jr nz, uartRead_do_read

    or a                    ; CF=0 (no data)
    ei
    ret

uartRead_retBuff:
    xor a
    ld (_poked_byte), a
    ld a, (_byte_buff)
    scf                     ; CF=1 (data available)
    ei
    ret

uartRead_do_read:
    ; Unified read path for latched (_is_recv) and hardware-detected data
    ld bc, ZXUNO_ADDR
    ld a, UART_DATA_REG
    out (c), a
    inc b                   ; FC3B -> FD3B
    in a, (c)

    ; Clear flags, preserve data in E (avoids clobbering HL)
    ld e, a
    xor a
    ld (_is_recv), a
    ld (_poked_byte), a
    ld a, e
    scf                     ; CF=1 (data available)
    ei
    ret

; -----------------------------------------------------------------------------
; _ay_uart_init
; -----------------------------------------------------------------------------
_ay_uart_init:
    xor a
    ld (_poked_byte), a
    ld (_is_recv), a

    ; Prime reads (settle UART registers)
    ld bc, ZXUNO_ADDR
    ld a, UART_STAT_REG
    out (c), a
    inc b                   ; FC3B -> FD3B
    in a, (c)

    ld bc, ZXUNO_ADDR
    ld a, UART_DATA_REG
    out (c), a
    inc b                   ; FC3B -> FD3B
    in a, (c)

    ; Boot wait + drain RX garbage
    ei
    ld b, 10                ; 10 HALTs (~200ms)
uartInit_wait:
    push bc
    call uartRead
    pop bc
    halt
    djnz uartInit_wait

    ; Bounded drain (no HALT)
    ld bc, 0x0200           ; 512 iterations
uartInit_flush:
    push bc
    call uartRead
    pop bc
    dec bc
    ld a, b
    or c
    jr nz, uartInit_flush

    ret

; -----------------------------------------------------------------------------
; _ay_uart_send
;   fastcall: byte in L
;   Timeout protects against missing hardware (emulators).
; -----------------------------------------------------------------------------
_ay_uart_send:
    ld a, l
    push af

    ; Check for pending RX while preparing to send
    ld bc, ZXUNO_ADDR
    ld a, UART_STAT_REG
    out (c), a
    inc b                   ; FC3B -> FD3B
    in a, (c)
    and UART_BYTE_RECIVED
    jr z, uartSend_checkSent

    ld a, 1
    ld (_is_recv), a

uartSend_checkSent:
    ; Wait until TX is not busy (with timeout for emulators without HW)
    ld bc, ZXUNO_REG
    ld d, 0                 ; D = timeout counter (256 iterations)
uartSend_wait_tx:
    in a, (c)
    and UART_BYTE_SENDING
    jr z, uartSend_ready
    dec d
    jr nz, uartSend_wait_tx
    pop af                  ; Timeout: discard byte, clean stack, return
    ret

uartSend_ready:
    ; Select data register and output byte
    ld bc, ZXUNO_ADDR
    ld a, UART_DATA_REG
    out (c), a
    inc b                   ; FC3B -> FD3B
    pop af
    out (c), a
    ret

; -----------------------------------------------------------------------------
; _ay_uart_send_block
;   callee: buf=first arg, len=second arg
;   sccz80 pushes left-to-right: SP+2=len, SP+4=buf
; -----------------------------------------------------------------------------
_ay_uart_send_block:
    pop bc          ; return address
    pop de          ; len (second arg, pushed last)
    pop hl          ; buf (first arg, pushed first)
    push bc         ; restore return address

    ld a, d
    or e
    ret z

uartSendBlock_loop:
    ld a, (hl)
    push hl
    push de
    ld l, a
    call _ay_uart_send
    pop de
    pop hl

    inc hl
    dec de
    ld a, d
    or e
    jr nz, uartSendBlock_loop

    ret

; -----------------------------------------------------------------------------
; _ay_uart_ready
;   Returns L=1 if there is at least one byte available, else L=0
; -----------------------------------------------------------------------------
_ay_uart_ready:
    ld a, (_poked_byte)
    or a
    jr nz, uartReady_yes

    ld a, (_is_recv)
    or a
    jr nz, uartReady_yes

    ld bc, ZXUNO_ADDR
    ld a, UART_STAT_REG
    out (c), a
    inc b                   ; FC3B -> FD3B
    in a, (c)
    and UART_BYTE_RECIVED
    jr z, uartReady_no

    ld a, 1
    ld (_is_recv), a

uartReady_yes:
    ld l, 1
    ret

uartReady_no:
    ld l, 0
    ret

_ay_uart_ready_fast:
    jp _ay_uart_ready

; -----------------------------------------------------------------------------
; _ay_uart_read
;   Returns L=byte if available, else L=0
; -----------------------------------------------------------------------------
_ay_uart_read:
    call uartRead
    jr nc, uartRead_none
    ld l, a
    ret

uartRead_none:
    xor a
    ld l, a
    ret
