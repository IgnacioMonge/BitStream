;;
;; bitstream_asm.asm - Optimized assembly routines for BitStream
;; Adapted from SpectalkZX ASM routines
;; Copyright (C) 2026 M. Ignacio Monge Garcia
;;

SECTION code_user

; =============================================================================
; PUBLIC FUNCTIONS
; =============================================================================
PUBLIC _str_to_upper
PUBLIC _skip_ws
PUBLIC _str_append
PUBLIC _char_append
PUBLIC _u16_to_dec
PUBLIC _uart_send_string
PUBLIC _clear_line
PUBLIC _clear_zone
PUBLIC _scroll_main_zone
PUBLIC _rb_pop
PUBLIC _try_read_line_nodrain
PUBLIC _uart_drain_to_buffer
PUBLIC _parse_decimal
PUBLIC _st_copy_n
PUBLIC _print_str64_char
PUBLIC _print_line64_fast
PUBLIC _main_putc
PUBLIC _main_newline
PUBLIC _main_puts
PUBLIC _g_ps64_y
PUBLIC _g_ps64_col
PUBLIC _g_ps64_attr
PUBLIC _detect_esxdos
PUBLIC _asm_row_base
PUBLIC _draw_big_char
PUBLIC _draw_badge_dither
PUBLIC _main_print_asm
PUBLIC _main_puts2
PUBLIC _utf8_to_ascii
PUBLIC _hl_mul32
PUBLIC _l_mul32
PUBLIC _rx_pos_reset
EXTERN _ay_uart_send
EXTERN _ay_uart_ready_fast
EXTERN _ay_uart_read
EXTERN _current_attr
EXTERN _main_col
EXTERN _main_line
EXTERN _ring_buffer
EXTERN _rb_head
EXTERN _rb_tail
EXTERN _rx_line
EXTERN _rx_pos
EXTERN _rx_overflow
EXTERN _uart_drain_limit

; -----------------------------------------------------------------------------
; void str_to_upper(char *s) __z88dk_fastcall
; Converts lowercase a-z to uppercase A-Z in-place.
; Input: HL = string pointer
; -----------------------------------------------------------------------------
_str_to_upper:
stu_loop:
    ld a, (hl)
    or a
    ret z
    cp 'a'
    jr c, stu_next
    cp 'z' + 1
    jr nc, stu_next
    sub 32
    ld (hl), a
stu_next:
    inc hl
    jr stu_loop

; -----------------------------------------------------------------------------
; char* skip_ws(char *p) __z88dk_fastcall
; Skips spaces, tabs, CR, LF. Returns pointer to first non-whitespace.
; Input: HL = pointer
; Returns: HL = pointer past whitespace
; -----------------------------------------------------------------------------
_skip_ws:
sw_loop:
    ld a, (hl)
    cp ' '
    jr z, sw_next
    cp 9             ; \t
    jr z, sw_next
    cp 13            ; \r
    jr z, sw_next
    cp 10            ; \n
    ret nz
sw_next:
    inc hl
    jr sw_loop

; =============================================================================
; RING BUFFER
; =============================================================================

; -----------------------------------------------------------------------------
; void uart_drain_to_buffer(void)
; Reads bytes from UART into ring buffer, up to uart_drain_limit times.
; Calls: _ay_uart_ready_fast (clobbers A,BC,HL)
;        _ay_uart_read       (clobbers A,BC,DE,HL, byte in L)
; Local: IXL = max_loop counter (safe, no DI/EI issues)
; -----------------------------------------------------------------------------
_uart_drain_to_buffer:
    push ix
    ld a, (_uart_drain_limit)
    ld ixl, a               ; IXL = max_loop counter

udtb_loop:
    ; Check max_loop > 0
    ld a, ixl
    or a
    jr z, udtb_done

    ; Check UART ready
    call _ay_uart_ready_fast
    ld a, l
    or a
    jr z, udtb_done

    ; Check RB_FULL: ((rb_head + 1) & MASK) == rb_tail
    ld hl, (_rb_head)
    inc hl
    ld a, h
    and 0x07                ; RING_BUFFER_MASK high byte
    ld h, a
    ld de, (_rb_tail)
    or a
    sbc hl, de
    jr z, udtb_done         ; Buffer full

    ; Read byte from UART
    call _ay_uart_read
    ; L = byte read

    ; Store: ring_buffer[rb_head] = byte
    ld a, l                 ; A = byte
    ld hl, (_rb_head)
    ld de, _ring_buffer
    add hl, de
    ld (hl), a              ; Store byte

    ; Advance head: rb_head = (rb_head + 1) & 0x07FF
    ld hl, (_rb_head)
    inc hl
    ld a, h
    and 0x07
    ld h, a
    ld (_rb_head), hl

    ; max_loop--
    dec ixl
    jr udtb_loop

udtb_done:
    pop ix
    ret

; -----------------------------------------------------------------------------
; int16_t rb_pop(void)
; Returns next byte from ring buffer (0..255), or -1 if empty.
; sccz80 return: HL
; -----------------------------------------------------------------------------
_rb_pop:
    ld hl, (_rb_head)
    ld de, (_rb_tail)
    or a
    sbc hl, de
    jr nz, rbp_has_data
    ; Empty: return -1
    ld hl, 0xFFFF
    ret
rbp_has_data:
    ; DE = rb_tail (still valid)
    ld hl, _ring_buffer
    add hl, de
    ld l, (hl)              ; L = byte
    ld h, 0                 ; HL = 0x00xx (return value)
    ; Advance tail: (rb_tail + 1) & 0x07FF
    inc de
    ld a, d
    and 0x07                ; Mask high byte (RING_BUFFER_MASK = 0x07FF)
    ld d, a
    ld (_rb_tail), de
    ret

; -----------------------------------------------------------------------------
; uint8_t try_read_line_nodrain(void)
; Reads bytes from ring buffer via rb_pop, builds a line in rx_line.
; Returns: HL=1 if complete line ready, HL=0 if no complete line yet.
; rx_line[128], rx_pos (uint8_t), rx_overflow (uint8_t)
; -----------------------------------------------------------------------------
_try_read_line_nodrain:
trln_loop:
    call _rb_pop
    ; HL = byte (0..255) or 0xFFFF (-1)
    ld a, h
    inc a                   ; h==0xFF -> A=0 (Z set)
    jr z, trln_ret0         ; -1 = buffer empty

    ; L = character
    ld a, l
    cp 13                   ; \r?
    jr z, trln_loop         ; skip CR

    cp 10                   ; \n?
    jr z, trln_newline

    ; HARDENED: check overflow BEFORE writing to memory
    ld a, (_rx_pos)
    cp 127                  ; sizeof(rx_line) - 1
    jr nc, trln_overflow

    ; rx_line[rx_pos] = c (safe: rx_pos < 127)
    ld e, a
    ld d, 0
    push hl                 ; save char in L
    ld hl, _rx_line
    add hl, de
    pop de                  ; D=garbage, E=char (was L)
    ld (hl), e

    ; rx_pos++
    inc a
    ld (_rx_pos), a
    jr trln_loop

trln_overflow:
    ; Do NOT write to memory, do NOT increment rx_pos
    ld a, 1
    ld (_rx_overflow), a
    jr trln_loop

trln_newline:
    ; NUL-terminate: rx_line[rx_pos] = 0
    ld a, (_rx_pos)
    ld e, a
    ld d, 0
    ld hl, _rx_line
    add hl, de
    ld (hl), 0

    ; Check overflow
    ld a, (_rx_overflow)
    or a
    jr z, trln_check_pos

    ; Overflow: discard line, reset, continue
    xor a
    ld (_rx_overflow), a
    ld (_rx_pos), a
    jr trln_loop

trln_check_pos:
    ; If rx_pos > 0, we have a valid line
    ld a, (_rx_pos)
    or a
    jr z, trln_loop         ; Empty line (\n\n), continue

    ; Valid line! Reset rx_pos, return 1
    xor a
    ld (_rx_pos), a
    ld hl, 1
    ret

trln_ret0:
    ld hl, 0
    ret

; =============================================================================
; STRING / NUMERIC UTILITIES (callee convention)
; =============================================================================

; -----------------------------------------------------------------------------
; char* str_append(char *dst, const char *src) __z88dk_callee
; Appends src to dst, returns pointer to NUL terminator.
; sccz80 callee: SP+2=src, SP+4=dst
; -----------------------------------------------------------------------------
_str_append:
    pop bc          ; Return address
    pop hl          ; src (2nd arg, pushed last)
    pop de          ; dst (1st arg, pushed first)
    push bc         ; Restore return address (callee cleaned 4 bytes)
sa_loop:
    ld a, (hl)
    or a
    jr z, sa_done
    ld (de), a
    inc hl
    inc de
    jr sa_loop
sa_done:
    ld (de), a      ; NUL terminator (A=0)
    ex de, hl       ; HL = pointer to NUL
    ret

; -----------------------------------------------------------------------------
; char* char_append(char *dst, char c) __z88dk_callee
; Writes c at dst, NUL after it, returns pointer to NUL.
; sccz80 callee: SP+2=c, SP+4=dst
; -----------------------------------------------------------------------------
_char_append:
    pop bc          ; Return address
    pop hl          ; c (2nd arg, L = char)
    ld a, l
    pop hl          ; dst (1st arg)
    push bc         ; Restore return address
    ld (hl), a      ; Write char
    inc hl
    ld (hl), 0      ; NUL terminator
    ret             ; HL = pointer to NUL

; -----------------------------------------------------------------------------
; char* u16_to_dec(char *dst, uint16_t v) __z88dk_callee
; Converts uint16 to decimal string.
; Returns: HL = pointer to NUL terminator
; sccz80 callee: SP+2=v, SP+4=dst
; -----------------------------------------------------------------------------
_u16_to_dec:
    pop bc                  ; Return address
    pop hl                  ; v   (second arg, pushed last)
    pop de                  ; dst (first arg, pushed first)
    push bc                 ; Only return address remains (callee cleaned 4 bytes)

    push ix
    ld ix, 0                ; IXL = "started printing" flag

    ld bc, -10000
    call u16_digit
    ld bc, -1000
    call u16_digit
    ld bc, -100
    call u16_digit
    ld bc, -10
    call u16_digit

    ; Units digit (always printed)
    ld a, l
    add a, '0'
    ld (de), a
    inc de

    ; NUL terminator
    xor a
    ld (de), a

    ex de, hl               ; Return pointer to NUL in HL
    pop ix
    ret

; Subroutine: extract one digit by repeated subtraction
; Input: HL = value, BC = -power, DE = buffer pointer
; Output: HL = remainder, DE advanced if digit printed
u16_digit:
    ld a, '0' - 1
u16_sub_loop:
    inc a
    add hl, bc
    jr c, u16_sub_loop

    ; Undo last subtraction
    sbc hl, bc

    ; Print?
    cp '0'
    jr nz, u16_do_print

    ; It's zero - have we printed anything yet?
    push af
    ld a, ixl
    or a
    jr nz, u16_print_zero
    pop af
    ret                     ; Suppress leading zero

u16_print_zero:
    pop af
u16_do_print:
    ld (de), a
    inc de
    ld ixl, 1               ; Mark that we've started printing
    ret

; =============================================================================
; UART
; =============================================================================

; -----------------------------------------------------------------------------
; void uart_send_string(const char *s) __z88dk_fastcall
; Sends a NUL-terminated string via UART, one byte at a time.
; Input: HL = string pointer
; -----------------------------------------------------------------------------
_uart_send_string:
    ld a, (hl)
    or a
    ret z
    push hl             ; save string pointer (ay_uart_send may clobber HL)
    ld l, a             ; fastcall: byte in L
    call _ay_uart_send
    pop hl              ; restore string pointer
    inc hl
    jr _uart_send_string

; =============================================================================
; SCREEN FUNCTIONS
; =============================================================================

; Pre-calculated screen base addresses (internal copy for ASM use)
_asm_row_base:
asm_row_base:
    defw 0x4000, 0x4020, 0x4040, 0x4060, 0x4080, 0x40A0, 0x40C0, 0x40E0
    defw 0x4800, 0x4820, 0x4840, 0x4860, 0x4880, 0x48A0, 0x48C0, 0x48E0
    defw 0x5000, 0x5020, 0x5040, 0x5060, 0x5080, 0x50A0, 0x50C0, 0x50E0

; Tabla de direcciones de atributos (24 filas)
; Elimina calculo repetido de 0x5800 + y*32
attr_row_base:
    defw 0x5800, 0x5820, 0x5840, 0x5860, 0x5880, 0x58A0, 0x58C0, 0x58E0
    defw 0x5900, 0x5920, 0x5940, 0x5960, 0x5980, 0x59A0, 0x59C0, 0x59E0
    defw 0x5A00, 0x5A20, 0x5A40, 0x5A60, 0x5A80, 0x5AA0, 0x5AC0, 0x5AE0

; -----------------------------------------------------------------------------
; Internal helper: clear one screen line
; Input: A = line Y, C = attribute
; Preserves: nothing (caller must save)
; -----------------------------------------------------------------------------
cli_internal:
    push bc
    push de
    push hl
    push af

    ; Calculate screen address from table
    ld l, a
    ld h, 0
    add hl, hl
    ld de, asm_row_base
    add hl, de
    ld e, (hl)
    inc hl
    ld d, (hl)              ; DE = screen base for row

    ; Clear 8 scanlines (32 bytes each)
    ld b, 8
cli_px_loop:
    push de
    push bc
    ld h, d
    ld l, e
    ld (hl), 0
    inc de
    ld bc, 31
    ldir
    pop bc
    pop de
    inc d                   ; Next scanline (add 256)
    djnz cli_px_loop

    ; Set attributes: 0x5800 + y*32
    pop af                  ; Recover Y
    ld l, a
    ld h, 0
    add hl, hl              ; *2
    add hl, hl              ; *4
    add hl, hl              ; *8
    add hl, hl              ; *16
    add hl, hl              ; *32
    ld de, 0x5800
    add hl, de

    ld (hl), c              ; Write first attribute byte
    ld d, h
    ld e, l
    inc de
    ld bc, 31
    ldir                    ; Fill remaining 31 bytes

    pop hl
    pop de
    pop bc
    ret

; -----------------------------------------------------------------------------
; void clear_line(uint8_t y, uint8_t attr)
; sccz80 cdecl: args pushed left-to-right
;   SP+2 = attr (second arg, pushed last)
;   SP+4 = y    (first arg, pushed first)
; -----------------------------------------------------------------------------
_clear_line:
    pop bc                  ; Return address
    pop hl                  ; attr (L = attr) - second arg
    pop de                  ; y (E = y)       - first arg
    push de
    push hl
    push bc

    ld a, e
    ld c, l
    jp cli_internal         ; tail call

; -----------------------------------------------------------------------------
; void clear_zone(uint8_t start, uint8_t lines, uint8_t attr)
; sccz80 cdecl: args pushed left-to-right
;   IX+4 = attr  (third arg, pushed last)
;   IX+6 = lines (second arg)
;   IX+8 = start (first arg, pushed first)
; -----------------------------------------------------------------------------
_clear_zone:
    push ix
    ld ix, 0
    add ix, sp

    ld a, (ix+8)            ; start (first arg)
    ld b, (ix+6)            ; lines (second arg)
    ld c, (ix+4)            ; attr  (third arg)

    ; If lines == 0, exit
    ld d, a
    ld a, b
    or a
    jr z, cz_done
    ld a, d

cz_loop:
    call cli_internal
    inc a
    djnz cz_loop

cz_done:
    pop ix
    ret

; =============================================================================
; SCROLL MAIN ZONE
; Scrolls lines 4-18 -> 3-17, clears line 18
; MAIN_START=3, MAIN_END=18
; Pure ASM with DI/EI to avoid visual artifacts
; =============================================================================

; Helper: apply scanline offset to HL(src) and DE(dst), preserves A=offset
smz_apply_offset:
    ld c, a
    ld a, h
    add a, c
    ld h, a
    ld a, d
    add a, c
    ld d, a
    ld a, c
    ret

_scroll_main_zone:
    push iy
    di

    ; Looped scroll: 8 scanlines x 5 blocks
    ; Block layout per scanline:
    ;   B1: rows 4-7 -> 3-6    (128 bytes)  src=40+N:80 dst=40+N:60
    ;   B2: row 8 -> 7         (32 bytes)   src=48+N:00 dst=40+N:E0
    ;   B3: rows 9-15 -> 8-14  (224 bytes)  src=48+N:20 dst=48+N:00
    ;   B4: row 16 -> 15       (32 bytes)   src=50+N:00 dst=48+N:E0
    ;   B5: rows 17-18 -> 16-17 (64 bytes)  src=50+N:20 dst=50+N:00

    xor a                   ; A = scanline offset (0..7)

smz_scanline_loop:
    ; Block 1: rows 4-7 -> 3-6
    ld h, 0x40
    ld l, 0x80
    ld d, 0x40
    ld e, 0x60
    call smz_apply_offset
    ld bc, 128
    ldir

    ; Block 2: row 8 -> 7
    ld h, 0x48
    ld l, 0x00
    ld d, 0x40
    ld e, 0xE0
    call smz_apply_offset
    ld bc, 32
    ldir

    ; Block 3: rows 9-15 -> 8-14
    ld h, 0x48
    ld l, 0x20
    ld d, 0x48
    ld e, 0x00
    call smz_apply_offset
    ld bc, 224
    ldir

    ; Block 4: row 16 -> 15
    ld h, 0x50
    ld l, 0x00
    ld d, 0x48
    ld e, 0xE0
    call smz_apply_offset
    ld bc, 32
    ldir

    ; Block 5: rows 17-18 -> 16-17
    ld h, 0x50
    ld l, 0x20
    ld d, 0x50
    ld e, 0x00
    call smz_apply_offset
    ld bc, 64
    ldir

    ; Next scanline
    inc a
    cp 8
    jr nz, smz_scanline_loop

    ; Scroll attributes (15 rows: row 4->3 ... row 18->17)
    ld hl, 0x5880
    ld de, 0x5860
    ld bc, 480
    ldir

    ; Clear last line (row 18) pixels + attributes
    ld a, (_current_attr)
    ld c, a
    ld a, 18                ; MAIN_END
    call cli_internal

    ei
    pop iy
    ret

; -----------------------------------------------------------------------------
; uint16_t parse_decimal(char **pp)
; sccz80 cdecl: SP+2 = pp (only arg, pushed last = pushed only)
; Parses decimal digits from *pp, updates *pp, returns value in HL.
; Does NOT clean stack (cdecl: caller cleans).
; -----------------------------------------------------------------------------
_parse_decimal:
    ; Get pp from stack without consuming it (cdecl)
    pop bc              ; Return address
    pop hl              ; pp
    push hl             ; Restore stack
    push bc

    ; HL = pp, load *pp into BC
    ld c, (hl)
    inc hl
    ld b, (hl)          ; BC = *pp = current char position
    dec hl
    push hl              ; Save pp for later update

    ld hl, 0             ; val = 0

pd_loop:
    ld a, (bc)
    sub '0'
    jr c, pd_done
    cp 10
    jr nc, pd_done

    ; val = val * 10 + digit
    ld d, h
    ld e, l              ; DE = val
    add hl, hl           ; *2
    add hl, hl           ; *4
    add hl, de           ; *5
    add hl, hl           ; *10

    ld e, a
    ld d, 0
    add hl, de           ; + digit

    inc bc
    jr pd_loop

pd_done:
    ; Update *pp = BC (current position after digits)
    pop de               ; DE = pp
    ld a, c
    ld (de), a
    inc de
    ld a, b
    ld (de), a
    ; HL = val (return value)
    ret

; -----------------------------------------------------------------------------
; void st_copy_n(char *dst, const char *src, uint8_t max_len)
; sccz80 cdecl: SP+2=max_len, SP+4=src, SP+6=dst
; Copies up to max_len-1 chars, always NUL-terminates.
; -----------------------------------------------------------------------------
_st_copy_n:
    pop af              ; Return address
    pop bc              ; max_len (C = byte)
    pop hl              ; src
    pop de              ; dst
    push de
    push hl
    push bc
    push af             ; Restore stack + return addr

    ld a, c
    or a
    ret z               ; max_len == 0
    dec a
    ld b, a             ; B = max chars to copy
    jr z, stcn_term     ; max_len was 1, just write NUL

stcn_loop:
    ld a, (hl)
    or a
    jr z, stcn_term
    ld (de), a
    inc hl
    inc de
    djnz stcn_loop

stcn_term:
    xor a
    ld (de), a
    ret

; =============================================================================
; 64-COLUMN FONT: COMPRESSED DATA + DECOMPRESSOR
; =============================================================================

; Renderer state variables (global, accessed by C wrapper)
SECTION bss_user
_g_ps64_y:    defs 1
_g_ps64_col:  defs 1
_g_ps64_attr: defs 1
glyph_buffer: defs 8
plf_left_buf: defs 8    ; Buffer para nibbles izquierdos (print_line64_fast)
plf_str_ptr:  defs 2    ; String pointer temporal (print_line64_fast)
plf_attr_val: defs 1    ; Atributo a escribir (print_line64_fast)
plf_y_val:    defs 1    ; Fila Y (print_line64_fast)
cache_scr_base: defs 2  ; Screen base addr cacheada
cache_atr_base: defs 2  ; Attr base addr cacheada
cache_row_y:   defs 1   ; Fila Y del cache (0xFF = invalido)

; BPE decompression state
bpe_rstack:    defs 16  ; Return stack for BPE expansion (8 levels x 2 bytes)
bpe_rsp:       defs 2   ; Current position in bpe_rstack

SECTION code_user

; LUT: 4-bit index -> 8-bit expanded pattern (both nibbles filled)
ALIGN 16
font_lut:
    defb 0x00, 0x22, 0x44, 0x55, 0x66, 0x88, 0xAA, 0xCC, 0xEE, 0xFF

; Compressed font: 96 chars * 3 bytes = 288 bytes (vs 768 uncompressed)
font64_packed:
    defb 0x00, 0x00, 0x00  ; ' ' (32)
    defb 0x55, 0x55, 0x05  ; '!' (33)
    defb 0x06, 0x60, 0x00  ; '"' (34)
    defb 0x06, 0x86, 0x86  ; '#' (35)
    defb 0x28, 0x58, 0x18  ; '$' (36)
    defb 0x61, 0x47, 0x56  ; '%' (37)
    defb 0x26, 0x26, 0x74  ; '&' (38)
    defb 0x25, 0x00, 0x00  ; ''' (39)
    defb 0x25, 0x55, 0x52  ; '(' (40)
    defb 0x52, 0x22, 0x25  ; ')' (41)
    defb 0x06, 0x28, 0x26  ; '*' (42)
    defb 0x02, 0x28, 0x22  ; '+' (43)
    defb 0x00, 0x02, 0x25  ; ',' (44)
    defb 0x00, 0x08, 0x00  ; '-' (45)
    defb 0x00, 0x00, 0x77  ; '.' (46)
    defb 0x11, 0x22, 0x55  ; '/' (47)
    defb 0x26, 0x68, 0x62  ; '0' (48)
    defb 0x27, 0x22, 0x28  ; '1' (49)
    defb 0x26, 0x12, 0x58  ; '2' (50)
    defb 0x81, 0x21, 0x62  ; '3' (51)
    defb 0x56, 0x68, 0x11  ; '4' (52)
    defb 0x85, 0x71, 0x17  ; '5' (53)
    defb 0x45, 0x76, 0x62  ; '6' (54)
    defb 0x81, 0x12, 0x22  ; '7' (55)
    defb 0x26, 0x26, 0x62  ; '8' (56)
    defb 0x26, 0x64, 0x17  ; '9' (57)
    defb 0x00, 0x50, 0x05  ; ':' (58)
    defb 0x02, 0x02, 0x25  ; ';' (59)
    defb 0x01, 0x25, 0x21  ; '<' (60)
    defb 0x00, 0x80, 0x80  ; '=' (61)
    defb 0x05, 0x21, 0x25  ; '>' (62)
    defb 0x26, 0x12, 0x02  ; '?' (63)
    defb 0x26, 0x88, 0x52  ; '@' (64)
    defb 0x26, 0x68, 0x66  ; 'A' (65)
    defb 0x76, 0x76, 0x67  ; 'B' (66)
    defb 0x45, 0x55, 0x54  ; 'C' (67)
    defb 0x76, 0x66, 0x67  ; 'D' (68)
    defb 0x85, 0x75, 0x58  ; 'E' (69)
    defb 0x85, 0x75, 0x55  ; 'F' (70)
    defb 0x26, 0x58, 0x62  ; 'G' (71)
    defb 0x66, 0x86, 0x66  ; 'H' (72)
    defb 0x82, 0x22, 0x28  ; 'I' (73)
    defb 0x41, 0x11, 0x62  ; 'J' (74)
    defb 0x66, 0x87, 0x66  ; 'K' (75)
    defb 0x55, 0x55, 0x58  ; 'L' (76)
    defb 0x68, 0x88, 0x66  ; 'M' (77)
    defb 0x86, 0x66, 0x66  ; 'N' (78)
    defb 0x26, 0x66, 0x62  ; 'O' (79)
    defb 0x76, 0x67, 0x55  ; 'P' (80)
    defb 0x26, 0x66, 0x74  ; 'Q' (81)
    defb 0x76, 0x67, 0x66  ; 'R' (82)
    defb 0x45, 0x21, 0x62  ; 'S' (83)
    defb 0x82, 0x22, 0x22  ; 'T' (84)
    defb 0x66, 0x66, 0x68  ; 'U' (85)
    defb 0x66, 0x66, 0x62  ; 'V' (86)
    defb 0x66, 0x88, 0x82  ; 'W' (87)
    defb 0x66, 0x26, 0x66  ; 'X' (88)
    defb 0x66, 0x62, 0x22  ; 'Y' (89)
    defb 0x81, 0x22, 0x58  ; 'Z' (90)
    defb 0x75, 0x55, 0x57  ; '[' (91)
    defb 0x55, 0x22, 0x11  ; '\' (92)
    defb 0x72, 0x22, 0x27  ; ']' (93)
    defb 0x26, 0x00, 0x00  ; '^' (94)
    defb 0x00, 0x00, 0x09  ; '_' (95)
    defb 0x55, 0x20, 0x00  ; '`' (96)
    defb 0x07, 0x14, 0x64  ; 'a' (97)
    defb 0x55, 0x76, 0x67  ; 'b' (98)
    defb 0x04, 0x55, 0x54  ; 'c' (99)
    defb 0x11, 0x46, 0x64  ; 'd' (100)
    defb 0x02, 0x68, 0x54  ; 'e' (101)
    defb 0x26, 0x57, 0x55  ; 'f' (102)
    defb 0x46, 0x64, 0x17  ; 'g' (103)
    defb 0x55, 0x76, 0x66  ; 'h' (104)
    defb 0x20, 0x72, 0x28  ; 'i' (105)
    defb 0x10, 0x41, 0x62  ; 'j' (106)
    defb 0x55, 0x67, 0x66  ; 'k' (107)
    defb 0x55, 0x55, 0x62  ; 'l' (108)
    defb 0x06, 0x88, 0x66  ; 'm' (109)
    defb 0x07, 0x66, 0x66  ; 'n' (110)
    defb 0x02, 0x66, 0x62  ; 'o' (111)
    defb 0x07, 0x67, 0x55  ; 'p' (112)
    defb 0x04, 0x64, 0x11  ; 'q' (113)
    defb 0x04, 0x55, 0x55  ; 'r' (114)
    defb 0x04, 0x52, 0x17  ; 's' (115)
    defb 0x28, 0x22, 0x21  ; 't' (116)
    defb 0x06, 0x66, 0x68  ; 'u' (117)
    defb 0x06, 0x66, 0x62  ; 'v' (118)
    defb 0x06, 0x88, 0x82  ; 'w' (119)
    defb 0x06, 0x26, 0x66  ; 'x' (120)
    defb 0x06, 0x64, 0x17  ; 'y' (121)
    defb 0x08, 0x12, 0x58  ; 'z' (122)
    defb 0x42, 0x52, 0x24  ; '{' (123)
    defb 0x22, 0x22, 0x22  ; '|' (124)
    defb 0x72, 0x12, 0x27  ; '}' (125)
    defb 0x36, 0x00, 0x00  ; '~' (126)
    defb 0x99, 0x99, 0x99  ; DEL (127) - solid block for progress bar (LUT[9]=0xFF)

; -----------------------------------------------------------------------------
; unpack_glyph - Decompress one glyph from font64_packed
; Input: A = char (ASCII 32-127)
; Output: HL = pointer to glyph_buffer (8 bytes)
; Destroys: AF, BC, DE
; -----------------------------------------------------------------------------
unpack_glyph:
    sub 32
    jr nz, unpack_not_space
    ; --- Space short-circuit: fill glyph_buffer with 0x00 (~50 t-states) ---
    ld hl, glyph_buffer
    xor a
    ld (hl), a
    inc hl
    ld (hl), a
    inc hl
    ld (hl), a
    inc hl
    ld (hl), a
    inc hl
    ld (hl), a
    inc hl
    ld (hl), a
    inc hl
    ld (hl), a
    inc hl
    ld (hl), a
    ld hl, glyph_buffer
    ret
unpack_not_space:
    ld l, a
    ld h, 0
    ld c, l
    ld b, h
    add hl, hl          ; *2
    add hl, bc          ; *3
    ld bc, font64_packed
    add hl, bc           ; HL = compressed source

    ex de, hl            ; DE = source
    ld hl, glyph_buffer  ; HL = destination

    ; Setup alternate registers for LUT access (font_lut is ALIGN 16, no carry)
    exx
    ld hl, font_lut      ; H' = font_lut high byte (constant)
    ld e, l              ; E' = font_lut low byte (base for add)
    exx

    ; Unpack 3 bytes -> 6 lines (0-5)
    ld b, 3
ug_loop:
    ld a, (de)
    ld c, a

    ; High nibble -> LUT -> line N
    rrca
    rrca
    rrca
    rrca
    and 0x0F
    exx                  ; switch to alt set (H=lut_hi, E=lut_lo)
    add a, e             ; A = font_lut_lo + nibble (no carry: ALIGN 16)
    ld l, a
    ld a, (hl)           ; LUT lookup
    exx                  ; back to main set
    ld (hl), a
    inc hl

    ; Low nibble -> LUT -> line N+1
    ld a, c
    and 0x0F
    exx
    add a, e
    ld l, a
    ld a, (hl)
    exx
    ld (hl), a
    inc hl

    inc de
    djnz ug_loop

    ; Lines 6-7 = 0x00
    xor a
    ld (hl), a
    inc hl
    ld (hl), a

    ld hl, glyph_buffer
    ret

; -----------------------------------------------------------------------------
; Cache helpers: devuelven base de fila screen/attr usando cache si valido.
; Input: (none, usa _g_ps64_y y cache_row_y)
; Output: HL = base addr
; Destroys: AF, DE (solo si cache miss)
; -----------------------------------------------------------------------------
p64_get_scr_base:
    ld a, (cache_row_y)
    ld hl, _g_ps64_y
    cp (hl)
    jr nz, p64_scr_miss
    ld hl, (cache_scr_base)
    ret
p64_scr_miss:
    ld a, (hl)             ; A = g_ps64_y
    add a, a
    ld l, a
    ld h, 0
    ld de, asm_row_base
    add hl, de
    ld a, (hl)
    inc hl
    ld h, (hl)
    ld l, a                ; HL = screen row base
    ld (cache_scr_base), hl
    ; Actualizar cache_row_y y calcular attr base tambien
    ld a, (_g_ps64_y)
    ld (cache_row_y), a
    push hl                ; Guardar screen base
    add a, a
    ld l, a
    ld h, 0
    ld de, attr_row_base
    add hl, de
    ld a, (hl)
    inc hl
    ld h, (hl)
    ld l, a
    ld (cache_atr_base), hl
    pop hl                 ; Devolver screen base
    ret

p64_get_atr_base:
    ld a, (cache_row_y)
    ld hl, _g_ps64_y
    cp (hl)
    jr nz, p64_atr_miss
    ld hl, (cache_atr_base)
    ret
p64_atr_miss:
    call p64_get_scr_base  ; Esto cachea ambos
    ld hl, (cache_atr_base)
    ret

; -----------------------------------------------------------------------------
; void print_str64_char(uint8_t ch) __z88dk_fastcall
; Draw a 4-pixel character at (g_ps64_y, g_ps64_col) with g_ps64_attr
; Input: L = ASCII character
; Destroys: AF, BC, DE, HL
; -----------------------------------------------------------------------------
_print_str64_char:
    ld a, l
    cp 32
    jr c, p64_use_space
    cp 127
    jr z, p64_block         ; Special case: block char for progress bar
    cp 128
    jr c, p64_calc_font
p64_use_space:
    ld a, 32

p64_calc_font:
    ; --- Space short-circuit: borrado directo sin unpack_glyph ---
    cp 32
    jr nz, p64_not_space

    ; Obtener screen base (cache o lookup)
    call p64_get_scr_base   ; HL = screen row base
    ld a, (_g_ps64_col)
    ld b, a
    srl a
    ld e, a
    ld d, 0
    add hl, de              ; HL = screen address

    bit 0, b
    jr nz, p64_space_right

    ; Espacio lado izquierdo: AND 0x0F en 8 scanlines
    ld b, 8
p64_space_left:
    ld a, (hl)
    and 0x0F
    ld (hl), a
    inc h
    djnz p64_space_left
    jr p64_set_attr

p64_space_right:
    ; Espacio lado derecho: AND 0xF0 en 8 scanlines
    ld b, 8
p64_space_right_loop:
    ld a, (hl)
    and 0xF0
    ld (hl), a
    inc h
    djnz p64_space_right_loop
    jr p64_set_attr

p64_not_space:
    ; Descomprimir glifo a glyph_buffer
    call unpack_glyph       ; A = char, returns HL = glyph_buffer
    push hl                 ; Save font source

    ; Obtener screen base (cache o lookup)
    call p64_get_scr_base   ; HL = screen row base

    ; Add col/2 for physical X
    ld a, (_g_ps64_col)
    ld b, a                 ; Save col for parity check
    srl a
    ld e, a
    ld d, 0
    add hl, de              ; HL = screen address

    pop de                  ; DE = glyph_buffer pointer

    bit 0, b
    jr nz, p64_right

    ; === LEFT nibble (even column) ===
    ld a, (hl)
    and 0x0F
    ld (hl), a
    inc h

    ld b, 7
p64_left_loop:
    ld a, (de)
    and 0xF0
    ld c, a
    ld a, (hl)
    and 0x0F
    or c
    ld (hl), a
    inc de
    inc h
    djnz p64_left_loop
    jr p64_set_attr

p64_right:
    ; === RIGHT nibble (odd column) ===
    ld a, (hl)
    and 0xF0
    ld (hl), a
    inc h

    ld b, 7
p64_right_loop:
    ld a, (de)
    and 0x0F
    ld c, a
    ld a, (hl)
    and 0xF0
    or c
    ld (hl), a
    inc de
    inc h
    djnz p64_right_loop
    jr p64_set_attr

; --- Block character (0x7F) for progress bar ---
; Pattern: 0xE0 (left) or 0x0E (right), scanlines 0,7 clear, 1-6 filled
p64_block:
    ; Obtener screen base (cache o lookup)
    call p64_get_scr_base   ; HL = screen row base

    ld a, (_g_ps64_col)
    ld b, a
    srl a
    ld e, a
    ld d, 0
    add hl, de              ; HL = screen address

    bit 0, b
    jr nz, p64_block_right

    ; Left block: pattern=0xE0, mask=0x0F
    ld c, 0xE0
    ld b, 0x0F
    jr p64_block_draw

p64_block_right:
    ; Right block: pattern=0x0E, mask=0xF0
    ld c, 0x0E
    ld b, 0xF0

p64_block_draw:
    ; Scanline 0: clear
    ld a, (hl)
    and b
    ld (hl), a
    inc h
    ; Scanlines 1-6: fill
    ld d, 6
p64_block_fill:
    ld a, (hl)
    and b
    or c
    ld (hl), a
    inc h
    dec d
    jr nz, p64_block_fill
    ; Scanline 7: clear
    ld a, (hl)
    and b
    ld (hl), a

p64_set_attr:
    ; Obtener attr base (cache o lookup)
    call p64_get_atr_base   ; HL = attr row base

    ld a, (_g_ps64_col)
    srl a
    ld c, a
    ld b, 0
    add hl, bc

    ; Escribir atributo solo si cambia
    ld a, (_g_ps64_attr)
    cp (hl)
    ret z                   ; Skip if same attr already
    ld (hl), a
    ret

; -----------------------------------------------------------------------------
; void print_line64_fast(uint8_t y, const char *s, uint8_t attr)
; __z88dk_callee
; Renderiza una linea completa de 64 columnas procesando pares de caracteres.
; Escribe bytes completos (sin AND/OR de preservacion), attr fill al final.
; ~40-50% mas rapido que 64 llamadas individuales a print_str64_char.
;
; sccz80 stack layout (args pushed left-to-right):
;   SP+2 = attr (last pushed)
;   SP+4 = s pointer (low, high)
;   SP+6 = y
; After push ix: IX+0=saved IX, IX+2=ret addr, IX+4=attr, IX+6=s, IX+8=y
; -----------------------------------------------------------------------------
_print_line64_fast:
    push ix
    ld ix, 0
    add ix, sp

    ; --- Calcular screen_row_base[y] una sola vez ---
    ld a, (ix+8)           ; y
    add a, a
    ld l, a
    ld h, 0
    ld bc, asm_row_base
    add hl, bc
    ld c, (hl)
    inc hl
    ld b, (hl)             ; BC = screen base addr de la fila

    ; Guardar attr y string pointer
    ld a, (ix+4)
    ld (plf_attr_val), a   ; attr para despues
    ld e, (ix+6)
    ld d, (ix+7)           ; DE = string pointer

    ; Guardar y para attr fill al final
    ld a, (ix+8)
    ld (plf_y_val), a

    push bc                ; Guardar screen base en stack
    pop hl                 ; HL = screen addr (col 0, scanline 0)

    ld b, 32               ; 32 pares de columnas (= 64 cols)

plf_pair_loop:
    push bc                ; Guardar contador de pares
    push hl                ; Guardar screen addr del byte actual

    ; --- Leer char izquierdo ---
    ld a, (de)
    or a
    jr z, plf_left_pad     ; NUL: no avanzar puntero, usar espacio
    inc de                 ; Avanzar puntero
    cp 32
    jr c, plf_left_space   ; char < 32: tratar como espacio
    cp 128
    jr c, plf_left_ok      ; char 32-127: OK
plf_left_space:
    ld a, 32
    jr plf_left_ok
plf_left_pad:
    ld a, 32
plf_left_ok:
    ; Guardar string pointer antes de unpack_glyph (destruye DE)
    ex de, hl
    ld (plf_str_ptr), hl
    ex de, hl              ; A sigue teniendo el char
    call unpack_glyph      ; HL = glyph_buffer, destruye AF/BC/DE
    ; Copiar nibbles altos a plf_left_buf
    ld hl, glyph_buffer
    ld de, plf_left_buf
    ld b, 8
plf_save_left:
    ld a, (hl)
    and 0xF0               ; Solo nibble alto (lado izquierdo)
    ld (de), a
    inc hl
    inc de
    djnz plf_save_left

    ; --- Leer char derecho ---
    ld hl, (plf_str_ptr)
    ex de, hl              ; DE = string pointer
    ld a, (de)
    or a
    jr z, plf_right_pad    ; NUL: no avanzar puntero, usar espacio
    inc de                 ; Avanzar puntero
    cp 32
    jr c, plf_right_space  ; char < 32: tratar como espacio
    cp 128
    jr c, plf_right_ok     ; char 32-127: OK
plf_right_space:
    ld a, 32
    jr plf_right_ok
plf_right_pad:
    ld a, 32
plf_right_ok:
    ex de, hl
    ld (plf_str_ptr), hl
    ex de, hl              ; A sigue teniendo el char
    call unpack_glyph      ; HL = glyph_buffer, destruye AF/BC/DE

    ; --- Combinar y escribir 8 scanlines ---
    ; Scanline 0 = blank (matches print_str64_char behavior)
    pop hl                 ; HL = screen addr
    push hl                ; Re-guardar para avanzar despues

    ld (hl), 0             ; Scanline 0: clear
    inc h                  ; Avanzar a scanline 1

    ld de, glyph_buffer
    ld ix, plf_left_buf
    ld b, 7                ; Scanlines 1-7: glyph data
plf_write_loop:
    ld a, (ix+0)           ; Nibble alto del char izquierdo
    ld c, a
    ld a, (de)             ; Byte del char derecho
    and 0x0F               ; Solo nibble bajo (lado derecho)
    or c                   ; Combinar: left_high | right_low
    ld (hl), a             ; Escribir byte completo a pantalla
    inc ix
    inc de
    inc h                  ; Siguiente scanline
    djnz plf_write_loop

    ld hl, (plf_str_ptr)
    ex de, hl              ; DE = string pointer para siguiente iteracion
    pop hl                 ; Recuperar screen addr
    inc hl                 ; Siguiente byte (siguiente par de columnas)

    pop bc                 ; Recuperar contador de pares
    djnz plf_pair_loop

    ; --- Attr fill: escribir 32 atributos de golpe ---
    ld a, (plf_y_val)
    add a, a
    ld l, a
    ld h, 0
    ld bc, attr_row_base
    add hl, bc
    ld c, (hl)
    inc hl
    ld b, (hl)
    ; BC = attr base addr
    ld h, b
    ld l, c                ; HL = attr addr
    ld a, (plf_attr_val)
    ld b, 32
plf_attr_fill:
    ld (hl), a
    inc hl
    djnz plf_attr_fill

    ; Actualizar globals para consistencia
    ld a, 64
    ld (_g_ps64_col), a
    ld a, (plf_y_val)
    ld (_g_ps64_y), a
    ld a, (plf_attr_val)
    ld (_g_ps64_attr), a

    ; Invalidar cache (y cambio)
    ld a, 0xFF
    ld (cache_row_y), a

    pop ix
    ret

; =============================================================================
; MAIN ZONE OUTPUT (ASM)
; =============================================================================

; -----------------------------------------------------------------------------
; void main_putc(uint8_t c) __z88dk_fastcall
; Input: L = character
; -----------------------------------------------------------------------------
_main_putc:
    ld a, l
    cp 13
    jr z, _main_newline
    cp 10
    jr z, _main_newline
    cp 32
    ret c                   ; Ignore control chars < 32

    ld b, a                 ; Save char in B

    ; Check column wrap
    ld a, (_main_col)
    cp 64                   ; SCREEN_COLS
    jr c, mputc_no_wrap

    push bc
    call _main_newline
    pop bc

mputc_no_wrap:
    ; Set renderer globals
    ld a, (_main_line)
    ld (_g_ps64_y), a
    ld a, (_main_col)
    ld (_g_ps64_col), a
    ld a, (_current_attr)
    ld (_g_ps64_attr), a

    ; Draw character
    ld l, b
    call _print_str64_char

    ; main_col++
    ld hl, _main_col
    inc (hl)
    ret

; -----------------------------------------------------------------------------
; void main_newline(void)
; -----------------------------------------------------------------------------
_main_newline:
    ; main_col = 0
    xor a
    ld (_main_col), a

    ; main_line++, check overflow
    ld a, (_main_line)
    cp 18                   ; MAIN_END
    jr c, mn_inc
    ; At bottom: scroll, keep main_line = MAIN_END
    jp _scroll_main_zone    ; tail call

mn_inc:
    inc a
    ld (_main_line), a
    ret

; -----------------------------------------------------------------------------
; void main_puts(const char *s) __z88dk_fastcall
; Input: HL = string pointer
; BPE-aware: bytes >= 0x80 are expanded from bpe_dict using a return stack.
; -----------------------------------------------------------------------------
_main_puts:
    ; Initialize BPE return stack (empty)
    push hl
    ld hl, bpe_rstack
    ld (bpe_rsp), hl
    pop hl
    ; Use DE as string pointer (frees HL for other uses)
    ex de, hl               ; DE = string pointer

puts_loop:
    ld a, (de)
    or a
    jr z, puts_bpe_pop      ; 0x00: end of string or end of BPE dict entry

    cp 0x80
    jr nc, puts_bpe_expand  ; >= 0x80: BPE token

    ; Normal ASCII char: render via main_putc
    push de
    ld l, a
    call _main_putc
    pop de
    inc de
    jr puts_loop

; --- BPE expansion ---
; Token >= 0x80 in A: push continuation (DE+1) to BPE stack,
; set DE to dict entry (3 bytes: b1, b2, 0x00). Loop reads from dict.
; On null, pops continuation and resumes original string.
puts_bpe_expand:
    inc de                  ; advance past the token byte
    ; Push continuation address (DE) onto BPE return stack
    push hl                 ; save HL temporarily
    ld hl, (bpe_rsp)
    ld (hl), e
    inc hl
    ld (hl), d
    inc hl
    ld (bpe_rsp), hl
    pop hl                  ; restore HL
    ; Calculate dict entry: bpe_dict + (token - 0x80) * 3
    sub 0x80
    ld l, a
    ld h, 0
    ld c, l
    ld b, h                 ; BC = (token - 0x80)
    add hl, hl              ; * 2
    add hl, bc              ; * 3
    ld bc, bpe_dict
    add hl, bc
    ex de, hl               ; DE = &bpe_dict[(token-0x80)*3]
    jr puts_loop            ; continue reading from dict entry

puts_bpe_pop:
    ; Null byte: end of string or end of BPE dict entry
    ; Check if BPE stack has entries
    push hl
    ld hl, (bpe_rsp)
    ld bc, bpe_rstack
    or a                    ; clear carry
    sbc hl, bc
    pop hl
    jr z, puts_done         ; stack empty -> real end of string
    ; Pop continuation address from BPE stack
    push hl
    ld hl, (bpe_rsp)
    dec hl
    ld d, (hl)
    dec hl
    ld e, (hl)
    ld (bpe_rsp), hl
    pop hl
    jr puts_loop            ; resume original string

puts_done:
    ret

; =============================================================================
; DOUBLE-HEIGHT BANNER CHARACTER
; =============================================================================
;
; void draw_big_char(uint8_t ch) __z88dk_fastcall
; Renders a character at double height across rows Y and Y+1.
; Each glyph line is doubled (rendered on 2 consecutive scanlines).
; Layout per row:
;   Top  (Y)  : scanlines 0-1 blank, 2-3 glyph[0], 4-5 glyph[1], 6-7 glyph[2]
;   Bot (Y+1) : scanlines 0-1 glyph[3], 2-3 glyph[4], 4-5 glyph[5], 6-7 blank
;
; Input: L = ASCII character
; Uses globals: _g_ps64_y (top row), _g_ps64_col
; Attributes are NOT set (caller must clear_line both rows beforehand).
; Destroys: AF, BC, DE, HL
; =============================================================================
_draw_big_char:
    ld a, l
    cp 32
    jr c, dbc_force_space
    cp 128
    jr c, dbc_valid
dbc_force_space:
    ld a, 32
dbc_valid:
    call unpack_glyph       ; HL = glyph_buffer (8 bytes, 0-5 data, 6-7 zero)
    ex de, hl               ; DE = glyph source

    ; --- Lookup screen base for top row (Y) ---
    ld a, (_g_ps64_y)
    add a, a                ; *2 for word index
    ld l, a
    ld h, 0
    ld bc, asm_row_base
    add hl, bc
    ld c, (hl)
    inc hl
    ld b, (hl)              ; BC = screen base for row Y
    inc hl
    ld (dbc_row1_ptr), hl   ; Save pointer to asm_row_base[Y+1]

    ; --- Add column offset ---
    ld a, (_g_ps64_col)
    srl a                   ; col/2 for physical byte
    ld l, a
    ld h, 0
    add hl, bc              ; HL = screen address for top row

    ; --- Branch on column parity ---
    ld a, (_g_ps64_col)
    bit 0, a
    jr nz, dbc_top_right

    ; === TOP ROW, LEFT NIBBLE (even column) ===
    ; Scanlines 0-1: clear left nibble
    ld a, (hl)
    and 0x0F
    ld (hl), a
    inc h
    ld a, (hl)
    and 0x0F
    ld (hl), a
    inc h
    ; Scanlines 2-7: 3 glyph lines, each doubled
    ld b, 3
dbc_tl_loop:
    ld a, (de)
    and 0xF0
    ld c, a
    ld a, (hl)
    and 0x0F
    or c
    ld (hl), a
    inc h
    ld a, (hl)
    and 0x0F
    or c
    ld (hl), a
    inc h
    inc de
    djnz dbc_tl_loop
    jr dbc_bottom

dbc_top_right:
    ; === TOP ROW, RIGHT NIBBLE (odd column) ===
    ; Scanlines 0-1: clear right nibble
    ld a, (hl)
    and 0xF0
    ld (hl), a
    inc h
    ld a, (hl)
    and 0xF0
    ld (hl), a
    inc h
    ; Scanlines 2-7: 3 glyph lines, each doubled
    ld b, 3
dbc_tr_loop:
    ld a, (de)
    and 0x0F
    ld c, a
    ld a, (hl)
    and 0xF0
    or c
    ld (hl), a
    inc h
    ld a, (hl)
    and 0xF0
    or c
    ld (hl), a
    inc h
    inc de
    djnz dbc_tr_loop

dbc_bottom:
    ; --- Lookup screen base for bottom row (Y+1) ---
    push de                 ; Save glyph ptr (now at byte 3)
    ld hl, (dbc_row1_ptr)   ; HL = pointer into asm_row_base for Y+1
    ld e, (hl)
    inc hl
    ld d, (hl)              ; DE = screen base for row Y+1
    ld a, (_g_ps64_col)
    srl a
    ld l, a
    ld h, 0
    add hl, de              ; HL = screen address for bottom row
    pop de                  ; DE = glyph source (byte 3)

    ld a, (_g_ps64_col)
    bit 0, a
    jr nz, dbc_bot_right

    ; === BOTTOM ROW, LEFT NIBBLE ===
    ; Scanlines 0-5: 3 glyph lines, each doubled
    ld b, 3
dbc_bl_loop:
    ld a, (de)
    and 0xF0
    ld c, a
    ld a, (hl)
    and 0x0F
    or c
    ld (hl), a
    inc h
    ld a, (hl)
    and 0x0F
    or c
    ld (hl), a
    inc h
    inc de
    djnz dbc_bl_loop
    ; Scanlines 6-7: clear left nibble
    ld a, (hl)
    and 0x0F
    ld (hl), a
    inc h
    ld a, (hl)
    and 0x0F
    ld (hl), a
    ret

dbc_bot_right:
    ; === BOTTOM ROW, RIGHT NIBBLE ===
    ; Scanlines 0-5: 3 glyph lines, each doubled
    ld b, 3
dbc_br_loop:
    ld a, (de)
    and 0x0F
    ld c, a
    ld a, (hl)
    and 0xF0
    or c
    ld (hl), a
    inc h
    ld a, (hl)
    and 0xF0
    or c
    ld (hl), a
    inc h
    inc de
    djnz dbc_br_loop
    ; Scanlines 6-7: clear right nibble
    ld a, (hl)
    and 0xF0
    ld (hl), a
    inc h
    ld a, (hl)
    and 0xF0
    ld (hl), a
    ret

; Temporary storage for row Y+1 pointer into asm_row_base
dbc_row1_ptr: defw 0

; =============================================================================
; void draw_badge_dither(uint8_t count) __z88dk_fastcall
; Draws triangle dither pattern for the banner badge on rows 0 AND 1.
; count in L = number of physical cells (e.g. 4)
; phys_x = 32 - count (right-aligned)
; Attributes must be set by caller before calling this.
; =============================================================================
_draw_badge_dither:
    ld a, l
    push af                 ; Save count for row 1
    ld b, l                 ; B = cell count
    ld a, 32
    sub l
    ld c, a                 ; C = phys_x = 32 - count

    ; Row 0: screen base 0x4000
    ld hl, 0x4000
    ld e, c
    ld d, 0
    add hl, de
    call dbd_row

    ; Row 1: screen base 0x4020
    pop af
    ld b, a                 ; B = cell count (restored)
    ld hl, 0x4020
    ld e, c                 ; C still = phys_x (preserved by dbd_row)
    ld d, 0
    add hl, de
    ; fall through to dbd_row (ret returns to caller)

dbd_row:
dbd_cell_loop:
    push bc
    push hl
    ld de, dbd_pattern
    ld b, 8
dbd_scanline:
    ld a, (de)
    ld (hl), a
    inc de
    inc h                   ; next scanline
    djnz dbd_scanline
    pop hl
    inc hl                  ; next cell column
    pop bc
    djnz dbd_cell_loop
    ret

dbd_pattern:
    defb 0x00               ; ........
    defb 0x01               ; .......X
    defb 0x03               ; ......XX
    defb 0x07               ; .....XXX
    defb 0x0F               ; ....XXXX
    defb 0x1F               ; ...XXXXX
    defb 0x3F               ; ..XXXXXX
    defb 0x7F               ; .XXXXXXX

; -----------------------------------------------------------------------------
; uint8_t detect_esxdos(void)
; Returns: L=1 if esxDOS present, L=0 if not
;
; divMMC intercepts RST 8 at hardware level (memory paging), so the bytes
; at 0x0008 are unchanged. We must actually try the call.
; ERR_SP trick: if no esxDOS, RST 8 triggers ROM error handler which
; does LD SP,(ERR_SP) then eventually RET to address on that stack.
; We save SP explicitly so we can restore to a known-good state.
; -----------------------------------------------------------------------------
_detect_esxdos:
    push ix                 ; preserve sccz80 frame pointer
    ld (esx_save_sp), sp    ; save current SP (after push ix)
    ld hl, (0x5C3D)        ; save original ERR_SP
    ld (esx_save_errsp), hl

    ; Push recovery address and set ERR_SP
    ld hl, _esx_not_present
    push hl
    ld (0x5C3D), sp         ; ERR_SP points to [_esx_not_present] on stack

    ; Try M_GETSETDRV — divMMC intercepts this at hardware level
    xor a
    rst 0x08
    defb 0x89               ; M_GETSETDRV

    ; If we reach here, esxDOS handled the call (present!)
    pop hl                  ; remove recovery address
    ld hl, (esx_save_errsp)
    ld (0x5C3D), hl         ; restore ERR_SP
    pop ix                  ; restore frame pointer
    ld l, 1
    ld h, 0
    ret

_esx_not_present:
    ; ROM error handler brought us here — stack may be trashed
    ld sp, (esx_save_sp)    ; restore SP (points to saved IX on stack)
    ld hl, (esx_save_errsp)
    ld (0x5C3D), hl         ; restore ERR_SP
    pop ix                  ; restore frame pointer
    ld l, 0
    ld h, 0
    ret

esx_save_sp:     defw 0
esx_save_errsp:  defw 0

; =============================================================================
; void main_print(const char *s) __z88dk_fastcall
; Fast-path: if main_col==0 and string <= 64 chars, renders the entire line
; with print_line64_fast (pair processing, no AND/OR masking = much faster).
; Otherwise falls back to main_puts (char by char).
; Always calls main_newline at the end.
; Input: HL = string pointer
; =============================================================================
_main_print_asm:
    ld a, (_main_col)
    or a
    jr nz, mp_slow

    ; Quick length scan: fits in SCREEN_COLS (64)?
    push hl                 ; save s
    ld d, h
    ld e, l                 ; DE = scan ptr
    ld b, 65                ; max 64+1 iterations
mp_scan:
    ld a, (de)
    or a
    jr z, mp_fast           ; NUL found = fits on one line
    cp 0x80
    jr nc, mp_has_bpe       ; BPE token found: must use slow path
    inc de
    djnz mp_scan
    ; String > 64 chars: slow path
    pop hl
    jr mp_slow

mp_has_bpe:
    ; String has BPE tokens: must decompress via slow path
    pop hl
    jr mp_slow

mp_fast:
    pop hl                  ; HL = original s
    ; Call print_line64_fast(main_line, s, current_attr)
    ; sccz80 cdecl: args pushed left-to-right (y first, s second, attr last)
    ld a, (_main_line)
    ld c, a
    ld b, 0
    push bc                 ; arg1: y (pushed first)
    push hl                 ; arg2: s
    ld a, (_current_attr)
    ld c, a
    push bc                 ; arg3: attr (pushed last)
    call _print_line64_fast
    pop bc
    pop bc
    pop bc                  ; clean 6 bytes
    jp _main_newline        ; tail call

mp_slow:
    call _main_puts         ; HL = s (fastcall)
    jp _main_newline        ; tail call

; =============================================================================
; void main_puts2(const char *a, const char *b)
; Print two strings consecutively (saves code at call sites)
; sccz80 cdecl: SP+2 = b (last pushed), SP+4 = a (first pushed)
; =============================================================================
_main_puts2:
    pop bc          ; ret addr
    pop hl          ; b (second string, pushed last)
    pop de          ; a (first string, pushed first)
    push de
    push hl
    push bc

    push hl         ; save b
    ex de, hl       ; HL = a
    call _main_puts ; print a (fastcall, HL = string)
    pop hl          ; HL = b
    jp _main_puts   ; tail call: print b and return

; =============================================================================
; void utf8_to_ascii(char *s) __z88dk_fastcall
; Convert UTF-8 string to ASCII in-place
; Input: HL = string pointer (modified in-place)
; Handles Latin-1 Supplement (C2-C3 sequences): accented vowels, n-tilde, etc.
; =============================================================================
_utf8_to_ascii:
    push hl                 ; Save start for return
    ld d, h
    ld e, l                 ; DE = write ptr, HL = read ptr

u8a_loop:
    ld a, (hl)
    or a
    jp z, u8a_done

    cp 0x80
    jr c, u8a_copy          ; 00-7F: ASCII, copy

    cp 0xC2
    jr c, u8a_skip1         ; 80-C1: invalid continuation

    cp 0xC4
    jr c, u8a_latin1        ; C2-C3: Latin-1 Supplement

    cp 0xE0
    jr c, u8a_skip2         ; C4-DF: 2-byte, skip

    cp 0xF0
    jr c, u8a_skip3         ; E0-EF: 3-byte, skip

    ; F0+: 4-byte sequence
    inc hl
    ld a, (hl) : or a : jr z, u8a_done
    inc hl
    ld a, (hl) : or a : jr z, u8a_done
    inc hl
    ld a, (hl) : or a : jr z, u8a_done
    inc hl
    ld a, '?'
    jr u8a_store

u8a_skip3:
    inc hl
    ld a, (hl) : or a : jr z, u8a_done
u8a_skip2:
    inc hl
    ld a, (hl) : or a : jr z, u8a_done
u8a_skip1:
    inc hl
    ld a, '?'
    jr u8a_store

u8a_copy:
    ld (de), a
    inc hl
    inc de
    jr u8a_loop

u8a_latin1:
    ; A = C2 or C3
    ld b, a                 ; B = first byte
    inc hl
    ld a, (hl)
    or a
    jr z, u8a_done
    ld c, a                 ; C = second byte (80-BF)
    inc hl

    ; Verify continuation byte (80-BF)
    ld a, c
    and 0xC0
    cp 0x80
    jr nz, u8a_invalid

    ld a, b
    cp 0xC3
    jr z, u8a_c3

    ; C2: codepoints 80-BF (symbols)
    ld a, c
    cp 0xA1                 ; inverted !
    ld a, '!'
    jr z, u8a_store
    ld a, c
    cp 0xBF                 ; inverted ?
    ld a, '?'
    jr z, u8a_store
    ld a, c
    cp 0xAB                 ; <<
    ld a, '<'
    jr z, u8a_store
    ld a, c
    cp 0xBB                 ; >>
    ld a, '>'
    jr z, u8a_store
    ld a, ' '               ; rest -> space
    jr u8a_store

u8a_c3:
    ; C3: codepoints C0-FF (accented vowels, n-tilde, etc.)
    ; Table index = C & 3F
    ld a, c
    and 0x3F
    ld c, a
    ld b, 0
    push hl
    ld hl, u8a_tbl_c0
    add hl, bc
    ld a, (hl)
    pop hl
    jr u8a_store

u8a_invalid:
    ld a, '?'

u8a_store:
    ld (de), a
    inc de
    jp u8a_loop

u8a_done:
    xor a
    ld (de), a
    pop hl
    ret

; Lookup table: Unicode C0-FF -> ASCII (64 bytes)
u8a_tbl_c0:
    defb 'A','A','A','A','A','A','A','C'  ; C0-C7: ÀÁÂÃÄÅÆÇ
    defb 'E','E','E','E','I','I','I','I'  ; C8-CF: ÈÉÊËÌÍÎÏ
    defb 'D','N','O','O','O','O','O','x'  ; D0-D7: ÐÑÒÓÔÕÖ×
    defb 'O','U','U','U','U','Y','T','s'  ; D8-DF: ØÙÚÛÜÝÞß
    defb 'a','a','a','a','a','a','a','c'  ; E0-E7: àáâãäåæç
    defb 'e','e','e','e','i','i','i','i'  ; E8-EF: èéêëìíîï
    defb 'o','n','o','o','o','o','o','/'  ; F0-F7: ðñòóôõö÷
    defb 'o','u','u','u','u','y','t','y'  ; F8-FF: øùúûüýþÿ

; =============================================================================
; COPT HELPER ROUTINES
; Peephole optimizer replaces common patterns with calls to these helpers.
; Adapted from SpectalkZX.
; =============================================================================

; -----------------------------------------------------------------------------
; hl_mul32: HL = HL * 32
; Replaces 5x add hl,hl (5 bytes) with call (3 bytes) saving 2 bytes/site
; -----------------------------------------------------------------------------
_hl_mul32:
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    ret

; -----------------------------------------------------------------------------
; l_mul32: zero-extend L to HL, then HL *= 32
; copt replaces: ld h,0 / 5x add hl,hl (7 bytes -> 3 bytes call)
; sccz80 generates "ld h,0" (not "ld h,0x00" like SDCC)
; -----------------------------------------------------------------------------
_l_mul32:
    ld h, 0
    jp _hl_mul32

; -----------------------------------------------------------------------------
; rx_pos_reset: rx_pos = 0
; sccz80 generates: ld hl,0 / ld a,l / ld (_rx_pos),a (7 bytes -> 3 bytes call)
; Saves 4 bytes per site
; -----------------------------------------------------------------------------
_rx_pos_reset:
    xor a
    ld (_rx_pos), a
    ld h, a              ; preserve HL=0 contract from replaced pattern
    ld l, a
    ret

; =============================================================================
; BPE DICTIONARY
; Auto-generated by tools/bpe_compress.py. Each entry: 2 expansion bytes + 0x00.
; Token 0x80 = entry 0, token 0x81 = entry 1, etc.
; When no BPE is active, this section is empty (just a label).
; =============================================================================
bpe_dict:
; --- BPE DICT START (replaced by bpe_compress.py) ---
; --- BPE DICT END ---
