// ============================================================================
// BitStream - FTP Client for ZX Spectrum
// Uses AY-UART bit-banging at 9600 baud via ESP8266/ESP-12
// ============================================================================

#include <arch/zx.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <input.h>

// --- INICIO DE DEFINICIONES GLOBALES ---
#define APP_VERSION      "1.1"
#define LINE_BUFFER_SIZE 80
#define TX_BUFFER_SIZE   128
#define PATH_SIZE        48

// --- COLORES / ATRIBUTOS (MOVIDOS AQUÍ ARRIBA) ---
#define ATTR_BANNER     (PAPER_BLUE | INK_WHITE | BRIGHT)
#define ATTR_STATUS     (PAPER_WHITE | INK_BLUE)
#define ATTR_MAIN_BG    (PAPER_BLACK | INK_WHITE)
#define ATTR_LOCAL      (PAPER_BLACK | INK_GREEN | BRIGHT)
#define ATTR_RESPONSE   (PAPER_BLACK | INK_CYAN | BRIGHT)
#define ATTR_ERROR      (PAPER_BLACK | INK_RED | BRIGHT)
#define ATTR_USER       (PAPER_BLACK | INK_WHITE | BRIGHT)
#define ATTR_INPUT_BG   (PAPER_GREEN | INK_BLACK)
#define ATTR_INPUT      (PAPER_GREEN | INK_BLACK)
#define ATTR_PROMPT     (PAPER_GREEN | INK_BLACK)

#define STATUS_RED      (PAPER_WHITE | INK_RED)
#define STATUS_GREEN    (PAPER_WHITE | INK_GREEN)
#define STATUS_YELLOW   (PAPER_WHITE | INK_YELLOW)

// --- FIN DE DEFINICIONES GLOBALES ---

uint8_t  *g_ldir_dst;
uint8_t  *g_ldir_src;
uint16_t  g_ldir_len;

static void ldir_copy_run(void);

static void ldir_copy_fwd(void *dst, const void *src, uint16_t len)
{
    g_ldir_dst = (uint8_t*)dst;
    g_ldir_src = (uint8_t*)src;
    g_ldir_len = len;
    ldir_copy_run();
}

#asm
_ldir_copy_run:
    ld hl,(_g_ldir_src)
    ld de,(_g_ldir_dst)
    ld bc,(_g_ldir_len)
    ld a,b
    or c
    ret z
    ldir
    ret
#endasm


// ============================================================================
// FONT 64 COLUMNS DATA
// ============================================================================

#include "font64_data.h"

// Keyboard: immediate EDIT detection (CAPS SHIFT + 1)
static uint8_t g_user_cancel = 0;

// Keyboard: CAPS SHIFT detection (for uppercase conversion)
// Physical CAPS LOCK key (Spectrum +2/+3): Detected as CAPS SHIFT + "2" simultaneously

// CAPS LOCK state
volatile uint8_t caps_lock_mode = 0;
volatile uint8_t caps_latch = 0;

// 1. Gestiona el encendido/apagado (Toggle) con CAPS SHIFT + 2
static void check_caps_toggle(void)
{
#asm
    ; --- Paso 1: Verificar tecla CAPS SHIFT (Fila FE, bit 0) ---
    ld   bc, 0xFEFE
    in   a, (c)
    bit  0, a            ; Bit 0 = 0 si está pulsado. (Z flag = 1 si pulsado)
    jr   nz, _no_combo   ; Si no es 0 (no pulsado), saltar

    ; --- Paso 2: Verificar tecla '2' (Fila F7, bit 1) ---
    ld   bc, 0xF7FE
    in   a, (c)
    bit  1, a            ; Bit 1 = 0 si está pulsado (Tecla "2")
    jr   nz, _no_combo   ; Si no es 0 (no pulsado), saltar

    ; --- COMBO DETECTADO: SHIFT + 2 ---
    
    ; Chequear latch (usando dirección directa)
    ld   hl, _caps_latch
    ld   a, (hl)
    or   a
    ret  nz              ; Si latch=1 (ya detectado), salir

    ; --- ACCIÓN: Invertir (Toggle) caps_lock_mode ---
    ld   hl, _caps_lock_mode
    ld   a, (hl)
    xor  1               ; Invertir bit 0
    ld   (hl), a
    
    ; Activar latch
    ld   hl, _caps_latch
    ld   (hl), 1
    ret

_no_combo:
    ; Si NO se cumple la combinación, reseteamos el latch
    ld   hl, _caps_latch
    ld   (hl), 0
    ret
#endasm
}

// 2. Verifica si la tecla física CAPS SHIFT está pulsada (para invertir mayúsculas)
static uint8_t key_shift_held(void)
{
#asm
    ; 1. Chequear CAPS SHIFT (Fila 0xFEFE, bit 0)
    ld   bc, 0xFEFE
    in   a, (c)
    bit  0, a            ; Bit 0 = 0 si está pulsado
    jr   nz, _shift_no   ; Si no está pulsado, salir con 0

    ; 2. El Shift está pulsado. Ahora miramos si es "Shift Limpio" o "Shift Función".
    ; Si se pulsa alguna tecla numérica (1-5 o 6-0) a la vez, es una función (Cursor, Edit...).
    
    ; Chequear teclas 1-5 (Fila 0xF7FE)
    ld   bc, 0xF7FE
    in   a, (c)
    and  0x1F            ; Nos interesan los 5 bits bajos (teclas 1,2,3,4,5)
    cp   0x1F            ; ¿Son todos 1 (ninguna pulsada)?
    jr   nz, _shift_no   ; Si alguna está pulsada (ej: Edit, CapsLock, TrueVideo...), ignorar Shift

    ; Chequear teclas 6-0 (Fila 0xEFFE)
    ld   bc, 0xEFFE
    in   a, (c)
    and  0x1F            ; Nos interesan los 5 bits bajos (teclas 0,9,8,7,6)
    cp   0x1F            ; ¿Son todos 1?
    jr   nz, _shift_no   ; Si alguna está pulsada (ej: Cursores, Delete...), ignorar Shift

    ; 3. Shift limpio detectado (para escribir letras)
    ld   l, 1
    ld   h, 0
    ret

_shift_no:
    ld   l, 0
    ld   h, 0
    ret
#endasm
}


static uint8_t key_edit_down(void)
{
#asm
    push bc          ; Guardamos BC por seguridad
    ld   h, 0        ; IMPORTANTE: Limpiar H para que el valor de retorno en HL sea correcto

    ; CAPS SHIFT (Fila 0xFE, bit 0)
    ld   bc, 0xFEFE
    in   a, (c)
    and  0x01
    ld   l, a        ; L = estado CAPS (0=pulsado)

    ; Tecla '1' (Fila 0xF7, bit 0)
    ld   bc, 0xF7FE
    in   a, (c)
    and  0x01
    or   l           ; A = CAPS | '1'. Si ambos son 0, resultado es 0.

    jr   nz, _not_edit

    ld   l, 1        ; Ambas pulsadas -> Devolver 1 (True)
    pop  bc          ; Restaurar BC
    ret

_not_edit:
    ld   l, 0        ; No pulsadas -> Devolver 0 (False)
    pop  bc          ; Restaurar BC
    ret
#endasm
}


// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================
static void main_print(const char *s);
static void main_newline(void);
static char* skip_ws(char *p);
static void invalidate_status_bar(void);
static uint32_t parse_size_arg(const char *s);
static void redraw_input_from(uint8_t start_pos);
static void draw_cursor_underline(uint8_t y, uint8_t col);
static uint8_t wait_for_ftp_code_fast(uint16_t max_frames, const char *code3);

// Screen constants needed by optimization code (full definitions below)
#define SCREEN_COLS     64
#define INPUT_START     22
#define INPUT_LINES     2
#define INPUT_END       23

// ============================================================================
// UI RENDER OPTIMIZATION: Dirty lines (deferred status bar redraw)
// ============================================================================
static void draw_status_bar_real(void);  // forward declaration

static uint8_t status_bar_dirty = 0;

// Se llama desde el bucle principal para aplicar cambios pendientes
static void ui_flush_dirty(void)
{
    if (status_bar_dirty) {
        status_bar_dirty = 0;
        draw_status_bar_real();
    }
}

// Wrapper: marca la barra como "sucia" para pintarla luego
static void draw_status_bar(void)
{
    status_bar_dirty = 1;
}

// Forward declaration for cached print
static void print_char64(uint8_t y, uint8_t col, uint8_t c, uint8_t attr);

// ============================================================================
// UI INPUT CACHE (Optimized character rendering)
// ============================================================================

// Forward declaration
static void print_char64(uint8_t y, uint8_t col, uint8_t c, uint8_t attr);

static uint8_t input_cache_char[INPUT_LINES][SCREEN_COLS];
static uint8_t input_cache_attr[INPUT_LINES][32];

static void input_cache_invalidate_cell(uint8_t y, uint8_t col)
{
    if (y < INPUT_START || y > INPUT_END || col >= SCREEN_COLS) return;
    input_cache_char[y - INPUT_START][col] = 0xFF;
}

static void input_cache_invalidate(void)
{
    uint8_t r, c;
    for (r = 0; r < INPUT_LINES; r++) {
        for (c = 0; c < SCREEN_COLS; c++) input_cache_char[r][c] = 0xFF;
        for (c = 0; c < 32; c++) input_cache_attr[r][c] = 0xFF;
    }
}

static char line_buffer[LINE_BUFFER_SIZE];
static uint8_t line_len = 0;
static uint8_t cursor_pos = 0;

// ============================================================================
// FUNCIÓN CACHÉ (Faltaba esta implementación)
// ============================================================================
static void put_char64_input_cached(uint8_t y, uint8_t col, uint8_t c, uint8_t attr) 
{
    if (y < INPUT_START || y > INPUT_END || col >= SCREEN_COLS) return;
    
    uint8_t local_y = y - INPUT_START;
    
    // Optimización: Si el caracter y color ya están en pantalla, no hacer nada
    if (input_cache_char[local_y][col] == c && input_cache_attr[local_y][col >> 1] == attr) {
        return;
    }
    
    // Actualizar caché y pintar
    input_cache_char[local_y][col] = c;
    input_cache_attr[local_y][col >> 1] = attr;
    print_char64(y, col, c, attr);
}


static void input_add_char(uint8_t c)
{
    // 1. Actualizar estado del bloqueo (Toggle)
    check_caps_toggle();
    
    // 2. Leer estado físico de Shift
    uint8_t shift_is_down = key_shift_held();
    
    // 3. Calcular si debe ser mayúscula (Lógica XOR)
    // caps_lock_mode (1) ^ shift_is_down (1) = 0 -> Minúscula
    // caps_lock_mode (1) ^ shift_is_down (0) = 1 -> Mayúscula
    uint8_t use_uppercase = (caps_lock_mode ^ shift_is_down);
    
    // 4. Excepción comandos Bang (!COMMAND)
    if (line_len > 0 && line_buffer[0] == '!') {
         uint8_t has_space = 0;
         uint8_t i;
         for (i = 0; i < line_len; i++) {
             if (line_buffer[i] == ' ') { has_space = 1; break; }
         }
         if (!has_space) use_uppercase = 1; 
    }
    // Caso especial: Primer caracter es '!'
    if (line_len == 0 && c == '!') use_uppercase = 0; 

    // 5. Conversión ASCII
    if (c >= 'a' && c <= 'z' && use_uppercase) {
        c = c - 32; 
    }
    else if (c >= 'A' && c <= 'Z' && !use_uppercase) {
        c = c + 32; 
    }

    if (c >= 32 && c < 127 && line_len < LINE_BUFFER_SIZE - 1) {
        
        if (cursor_pos < line_len) {
            // Caso Inserción
            uint8_t i = (uint8_t)line_len;
            while (i > (uint8_t)cursor_pos) {
                line_buffer[i] = line_buffer[i - 1];
                --i;
            }
            line_buffer[cursor_pos] = c;
            line_len++;
            cursor_pos++;
            line_buffer[line_len] = 0;
            redraw_input_from(cursor_pos - 1);
        } else {
            // Caso Escritura al final (Append)
            line_buffer[cursor_pos] = c;
            line_len++;
            cursor_pos++;
            line_buffer[line_len] = 0;

            // --- CORRECCIÓN COLOR ---
            // Pintamos el caracter escrito en VERDE (ATTR_INPUT)
            // Esto elimina el rojo del cursor que estaba aquí antes.
            uint16_t char_abs = (cursor_pos - 1) + 2;
            uint8_t row = INPUT_START + (char_abs / SCREEN_COLS);
            uint8_t col = char_abs % SCREEN_COLS;
            
            // Forzamos ATTR_INPUT (Verde/Negro)
            put_char64_input_cached(row, col, c, ATTR_INPUT);

            // Pintamos el NUEVO cursor (que saldrá rojo si check_caps_toggle funcionó)
            uint16_t cur_abs = cursor_pos + 2;
            uint8_t cur_row = INPUT_START + (cur_abs / SCREEN_COLS);
            uint8_t cur_col = cur_abs % SCREEN_COLS;

            if (cur_row <= INPUT_END) {
                put_char64_input_cached(cur_row, cur_col, ' ', ATTR_INPUT);
                draw_cursor_underline(cur_row, cur_col);
            }
        }
    }
}


// ============================================================================
// EXTERNAL AY-UART DRIVER
// ============================================================================

extern void     ay_uart_init(void);
extern void     ay_uart_send(uint8_t byte) __z88dk_fastcall;
extern void     ay_uart_send_block(void *buf, uint16_t len) __z88dk_callee;
extern uint8_t  ay_uart_read(void);
extern uint8_t  ay_uart_ready(void);
extern uint8_t  ay_uart_ready_fast(void);  // Assumes PORT A already selected

// ============================================================================
// SCREEN CONFIGURATION
// ============================================================================

// SCREEN_COLS, INPUT_START, INPUT_LINES, INPUT_END defined above for UI cache
#define SCREEN_PHYS     32

#define BANNER_START    0
#define MAIN_START      2
#define MAIN_LINES      18
#define MAIN_END        (MAIN_START + MAIN_LINES - 1)
#define STATUS_LINE     21

// Pagination
#define LINES_PER_PAGE  17

// Key codes
#define KEY_UP        11
#define KEY_DOWN      10
#define KEY_LEFT      8
#define KEY_RIGHT     9
#define KEY_BACKSPACE 12
#define KEY_ENTER     13

// ============================================================================
// TIMEOUTS
// ============================================================================

// Hybrid loop counters - for loops with periodic HALT (not every iteration)
// These are iteration counts, not wall-clock time
// Used in cmd_ls, cmd_search where HALT is periodic (every 32 iterations)
#define TIMEOUT_LONG    32000UL   // ~20 segundos con HALT cada 32 iter

// Silence thresholds - counters that increment only when no data + HALT
// Wall-clock time = value × 20ms (since HALT happens each increment)
#define SILENCE_SHORT   150UL     // ~3 segundos
#define SILENCE_NORMAL  250UL     // ~5 segundos  
#define SILENCE_LONG    400UL     // ~8 segundos
#define SILENCE_XLONG   750UL     // ~15 segundos

// Frame-based timeouts (wall-clock, 1 frame = 20ms)
#define FRAMES_1S       50
#define FRAMES_5S       (5 * FRAMES_1S)
#define FRAMES_10S      (8 * FRAMES_1S) 
#define FRAMES_15S      (10 * FRAMES_1S)

// List pagination: if the user keeps "-- More --" paused beyond this,
// we will run a low-cost NOOP probe when the listing finishes to avoid
// returning to a "ghost connected" state.
#define FRAMES_LIST_PAUSE_RISKY      (90 * FRAMES_1S)
#define FRAMES_NOOP_QUICK_TIMEOUT    (1 * FRAMES_1S)

// Macro para esperar un frame (ahorra código)
#define HALT() do { __asm__("ei"); __asm__("halt"); } while(0)

#define TIMEOUT_BUSY    800000UL  // ~20-25 segundos de espera activa
#define SILENCE_BUSY    200000UL  // ~5 segundos sin recibir datos

// Forward declaration
static void print_line64_fast(uint8_t y, const char *s, uint8_t attr);

// ============================================================================
// RING BUFFER
// ============================================================================

#define RING_BUFFER_SIZE 512
static uint8_t ring_buffer[RING_BUFFER_SIZE];
static uint16_t rb_head = 0;
static uint16_t rb_tail = 0;

// Line parser state (used by try_read_line and rx_reset_all)
static char rx_line[128];
static uint8_t rx_pos = 0;
static uint8_t rx_overflow = 0;  // Tracks line truncation state

// Adaptive drain control - balances UI responsiveness vs transfer speed
#define DRAIN_NORMAL    32    // UI responsive (keyboard checks) - max ~32ms block
#define DRAIN_FAST      255   // Max throughput (transfers)

static uint8_t uart_drain_limit = DRAIN_NORMAL;

// Switch drain modes
static void drain_mode_fast(void) { uart_drain_limit = DRAIN_FAST; }
static void drain_mode_normal(void) { uart_drain_limit = DRAIN_NORMAL; }

static uint8_t rb_full(void)
{
    return ((uint16_t)(rb_head + 1) & 0x1FF) == rb_tail; // MÁSCARA 0x1FF
}

static void uart_drain_to_buffer(void)
{
    uint8_t max_loop = uart_drain_limit;
    
    // OPTIMIZATION: Select AY PORT A once, then use fast version in loop
    // This saves ~8 cycles per iteration (out + setup)
    // Safe because we control the entire scope
    #asm
        ld   bc, 0xFFFD
        ld   a, 0x0E
        out  (c), a         ; Select PORT A once
    #endasm
    
    while (ay_uart_ready_fast() && max_loop > 0) {
        if (rb_full()) break;
        ring_buffer[rb_head] = ay_uart_read();
        rb_head = (rb_head + 1) & 0x1FF;  // MÁSCARA 0x1FF
        max_loop--;
    }
}

static int16_t rb_pop(void)
{
    if (rb_head == rb_tail) return -1;
    uint8_t result = ring_buffer[rb_tail];
    rb_tail = (rb_tail + 1) & 0x1FF;  // MÁSCARA 0x1FF
    return result;
}

static void rb_flush(void)
{
    uint16_t max = 500;
    while (ay_uart_ready() && max > 0) {
        ay_uart_read();
        max--;
    }
    rb_head = rb_tail = 0;
}

// ============================================================================
// RX STATE MANAGEMENT
// ============================================================================
// Three levels of RX state that must be kept consistent:
//   1. UART hardware buffer (ay_uart_read drains it)
//   2. Ring buffer (rb_head, rb_tail)  
//   3. Line parser (rx_pos, rx_line partial content)
//
// rx_reset_all() - Full reset: UART + ring buffer + parser
// rb_flush()     - Ring buffer + UART drain (legacy, used in data transfers)
// rx_pos = 0     - Parser only (use when just finished processing a line)

static void rx_reset_all(void)
{
    // 1. Drain UART with patience for late bytes
    uint16_t max_wait = 300;
    uint16_t max_bytes = 500;
    while (max_bytes > 0) {
        if (ay_uart_ready()) {
            ay_uart_read();
            max_bytes--;
            max_wait = 50;
        } else {
            if (max_wait == 0) break;
            max_wait--;
        }
    }
    // 2. Clear ring buffer
    rb_head = rb_tail = 0;
    // 3. Reset line parser (rx_pos and rx_overflow are defined later but linked globally)
    rx_pos = 0;
    rx_overflow = 0;
}

// ============================================================================
// BUFFERS
// ============================================================================

// CAPS LOCK state (toggle mode - physical key detection)
// Must be non-static so ASM code can access them with _caps_lock_mode / _caps_lock_was_pressed
static char tx_buffer[TX_BUFFER_SIZE];
static char ftp_cmd_buffer[128];

// File write buffer - 512 bytes for efficient SD writes
static uint8_t file_buffer[512];
static uint16_t file_buf_pos = 0;

// ============================================================================
// COMMON STRINGS (save code space)
// ============================================================================

static const char S_IPD0[] = "+IPD,0,";
static const char S_IPD1[] = "+IPD,1,";
static const char S_CLOSED1[] = "1,CLOSED";
static const char S_PASV_FAIL[] = "PASV failed";
static const char S_DATA_FAIL[] = "Data connect failed";
static const char S_LIST_FAIL[] = "LIST send failed";
static const char S_CRLF[]      = "\r\n";
static const char S_CANCEL[]    = "Cancelled";
static const char S_DOTS[]      = ".";
static const char S_ERROR_TAG[] = "Error: ";
static const char S_AT_CLOSE0[] = "AT+CIPCLOSE=0\r\n";
static const char S_AT_CIPMUX[] = "AT+CIPMUX=1\r\n";
static const char S_CMD_QUIT[]  = "QUIT\r\n";

// Repeated UI strings (saves ~50 bytes)
static const char S_EMPTY[] = "---";
static const char S_NO_CONN[] = "No connection. Use OPEN.";
static const char S_LOGIN_BAD[] = "Login incorrect";
static const char S_CHECKING[] = "Checking connection.";

// Small helper to avoid repeated strncpy()+NUL boilerplate (saves code size)
static void safe_copy(char *dst, const char *src, uint16_t dst_size)
{
    if (dst_size == 0) return;
    // dst_size is compile-time constant in all call sites here
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

// ============================================================================
// FTP STATE
// ============================================================================

#define STATE_DISCONNECTED  0
#define STATE_WIFI_OK       1
#define STATE_FTP_CONNECTED 2
#define STATE_LOGGED_IN     3

static char wifi_client_ip[16] = "0.0.0.0";
static char ftp_host[32] = "---";
static char ftp_user[20] = "---";
static char ftp_path[PATH_SIZE] = "---";
static char data_ip[16];

static uint16_t data_port = 0;
static uint8_t connection_state = STATE_DISCONNECTED;

// Helper para limpiar estado FTP (evita duplicación)
static void clear_ftp_state(void)
{
    safe_copy(ftp_host, S_EMPTY, sizeof(ftp_host));
safe_copy(ftp_user, S_EMPTY, sizeof(ftp_user));
safe_copy(ftp_path, S_EMPTY, sizeof(ftp_path));
connection_state = STATE_WIFI_OK;
    invalidate_status_bar();
}

// ============================================================================
// SCREEN STATE
// ============================================================================

static uint8_t main_line = MAIN_START;
static uint8_t main_col = 0;
static uint8_t current_attr = ATTR_LOCAL;

// Debug mode flags
static uint8_t debug_mode = 0;
static uint8_t debug_enabled = 1;  // Can be temporarily disabled

// Progress bar state
static uint8_t status_bar_overwritten = 0;
static char spinner_chars[] = "|/-\\";
static uint8_t spinner_idx = 0;

// ============================================================================
// COMMAND HISTORY
// ============================================================================

#define HISTORY_SIZE    4
#define HISTORY_LEN     40

static char history[HISTORY_SIZE][HISTORY_LEN];
static uint8_t hist_head = 0;
static uint8_t hist_count = 0;
static int8_t hist_pos = -1;
static char temp_input[LINE_BUFFER_SIZE];

static void history_add(const char *cmd, uint8_t len)
{
    uint8_t i;
    if (len == 0) return;
    if (hist_count > 0) {
        uint8_t last = (hist_head + HISTORY_SIZE - 1) % HISTORY_SIZE;
        if (strcmp(history[last], cmd) == 0) return;
    }
    for (i = 0; i < len && i < HISTORY_LEN - 1; i++) {
        history[hist_head][i] = cmd[i];
    }
    history[hist_head][i] = 0;
    hist_head = (hist_head + 1) % HISTORY_SIZE;
    if (hist_count < HISTORY_SIZE) hist_count++;
    hist_pos = -1;
}

static void history_nav_up(void)
{
    uint8_t idx;
    if (hist_count == 0) return;
    if (hist_pos == -1) memcpy(temp_input, line_buffer, line_len + 1);
    if (hist_pos < (int8_t)(hist_count - 1)) hist_pos++;
    idx = (hist_head + HISTORY_SIZE - 1 - hist_pos) % HISTORY_SIZE;
    safe_copy(line_buffer, history[idx], sizeof(line_buffer));
line_len = strlen(line_buffer);
    cursor_pos = line_len;
}

static void history_nav_down(void)
{
    uint8_t idx;
    if (hist_pos < 0) return;
    hist_pos--;
    if (hist_pos < 0) {
        memcpy(line_buffer, temp_input, LINE_BUFFER_SIZE);
        line_len = strlen(line_buffer);
    } else {
        idx = (hist_head + HISTORY_SIZE - 1 - hist_pos) % HISTORY_SIZE;
        safe_copy(line_buffer, history[idx], sizeof(line_buffer));
line_len = strlen(line_buffer);
    }
    cursor_pos = line_len;
}

static void history_nav_and_redraw(int8_t direction)
{
    uint8_t prev_len = line_len;
    
    if (direction > 0) history_nav_up();
    else history_nav_down();
    
    cursor_pos = line_len;
    
    // Si la línea nueva es más corta, borrar los caracteres sobrantes del final
    if (prev_len >= line_len) {
        uint8_t i;
        for (i = line_len; i <= prev_len; i++) {
            uint16_t abs_pos = i + 2;
            uint8_t row = INPUT_START + (abs_pos / SCREEN_COLS);
            uint8_t col = abs_pos % SCREEN_COLS;
            if (row <= INPUT_END) put_char64_input_cached(row, col, ' ', ATTR_INPUT_BG);
        }
    }
    redraw_input_from(0);
}

// ============================================================================
// VIDEO MEMORY FUNCTIONS
// ============================================================================

// ============================================================================
// VIDEO ADDRESS LOOKUP TABLE - Critical optimization for ZX Spectrum
// ============================================================================
// Pre-calculated screen base addresses for all 24 text lines (scanline 0)
// Replaces complex bit-shifting math with simple array lookup
// ZX Spectrum video layout: 3 thirds of 8 lines each (non-linear addressing)

static const uint16_t screen_row_base[24] = {
    // Top third (lines 0-7)
    0x4000, 0x4020, 0x4040, 0x4060, 0x4080, 0x40A0, 0x40C0, 0x40E0,
    // Middle third (lines 8-15)
    0x4800, 0x4820, 0x4840, 0x4860, 0x4880, 0x48A0, 0x48C0, 0x48E0,
    // Bottom third (lines 16-23)
    0x5000, 0x5020, 0x5040, 0x5060, 0x5080, 0x50A0, 0x50C0, 0x50E0
};

static uint8_t* screen_line_addr(uint8_t y, uint8_t phys_x, uint8_t scanline)
{
    // LUT optimization: base address from table + scanline offset + phys_x
    // Old: 5 operations (2 AND, 3 shifts, 4 OR) = ~80 cycles
    // New: 1 array read + 2 additions = ~20 cycles
    return (uint8_t*)(screen_row_base[y] + ((uint16_t)scanline << 8) + phys_x);
}

static uint8_t* attr_addr(uint8_t y, uint8_t phys_x)
{
    return (uint8_t*)(0x5800 + (uint16_t)y * 32 + phys_x);
}

// ============================================================================
// DRAW HORIZONTAL LINE (1 pixel height) - Fast and compact
// ============================================================================
// Draws a 1-pixel horizontal line directly in video memory
// Much faster than drawing '-' characters (no font rendering needed)
// More elegant visual appearance
// Smaller code than repeated character rendering
//
// Parameters:
//   y: character row (0-23)
//   x_start: start column in chars (0-31) 
//   width: width in chars (1-32)
//   scanline: pixel row within char (0-7), typically 3 or 4 for centered line
//   attr: color attribute (INK/PAPER/BRIGHT)
static void draw_hline(uint8_t y, uint8_t x_start, uint8_t width, uint8_t scanline, uint8_t attr)
{
    uint8_t x;
    uint8_t *screen_ptr;
    uint8_t *attr_ptr;
    
    // Get base address for this character row and scanline
    screen_ptr = (uint8_t*)(screen_row_base[y] + ((uint16_t)scanline << 8) + x_start);
    attr_ptr = (uint8_t*)(0x5800 + (uint16_t)y * 32 + x_start);
    
    // Draw line: set all 8 pixels per character to 0xFF (solid line)
    for (x = 0; x < width; x++) {
        *screen_ptr++ = 0xFF;  // 8 pixels on
        *attr_ptr++ = attr;     // Set color
    }
}


static void print_char64(uint8_t y, uint8_t col, uint8_t c, uint8_t attr)
{
    uint8_t phys_x = col >> 1;
    uint8_t half = col & 1;
    uint8_t *font_ptr;
    uint8_t *screen_ptr;
    
    uint8_t ch = c;
    if (ch < 32 || ch > 127) ch = 32;
    
    // Calcular dirección base
    screen_ptr = (uint8_t*)(screen_row_base[y] + phys_x);

if (ch == 127) {
        uint8_t pattern = (half == 0) ? 0xE0 : 0x0E;
        uint8_t mask    = (half == 0) ? 0x0F : 0xF0;
        uint8_t k;
        
        // Scanline 0: BORRAR
        *screen_ptr = (*screen_ptr & mask); screen_ptr += 256;
        
        // Scanline 1-6: DIBUJAR BLOQUE (Bucle para ahorrar memoria)
        for (k = 0; k < 6; k++) {
            *screen_ptr = (*screen_ptr & mask) | pattern; 
            screen_ptr += 256;
        }
        
        // Scanline 7: BORRAR
        *screen_ptr = (*screen_ptr & mask);
        
        goto write_attr;
    }

    // --- RENDERIZADO ESTÁNDAR (LÓGICA ORIGINAL RESTAURADA) ---
    font_ptr = (uint8_t*)&font64[((uint16_t)(ch - 32)) << 3];
    
    if (half == 0) {
        // Izquierda
        *screen_ptr = (*screen_ptr & 0x0F); screen_ptr += 256; // Línea 0: Borrar (Fix altura)
        *screen_ptr = (*screen_ptr & 0x0F) | (font_ptr[0] & 0xF0); screen_ptr += 256;
        *screen_ptr = (*screen_ptr & 0x0F) | (font_ptr[1] & 0xF0); screen_ptr += 256;
        *screen_ptr = (*screen_ptr & 0x0F) | (font_ptr[2] & 0xF0); screen_ptr += 256;
        *screen_ptr = (*screen_ptr & 0x0F) | (font_ptr[3] & 0xF0); screen_ptr += 256;
        *screen_ptr = (*screen_ptr & 0x0F) | (font_ptr[4] & 0xF0); screen_ptr += 256;
        *screen_ptr = (*screen_ptr & 0x0F) | (font_ptr[5] & 0xF0); screen_ptr += 256;
        *screen_ptr = (*screen_ptr & 0x0F) | (font_ptr[6] & 0xF0); // Línea 7
    } else {
        // Derecha
        *screen_ptr = (*screen_ptr & 0xF0); screen_ptr += 256; // Línea 0: Borrar (Fix altura)
        *screen_ptr = (*screen_ptr & 0xF0) | (font_ptr[0] & 0x0F); screen_ptr += 256;
        *screen_ptr = (*screen_ptr & 0xF0) | (font_ptr[1] & 0x0F); screen_ptr += 256;
        *screen_ptr = (*screen_ptr & 0xF0) | (font_ptr[2] & 0x0F); screen_ptr += 256;
        *screen_ptr = (*screen_ptr & 0xF0) | (font_ptr[3] & 0x0F); screen_ptr += 256;
        *screen_ptr = (*screen_ptr & 0xF0) | (font_ptr[4] & 0x0F); screen_ptr += 256;
        *screen_ptr = (*screen_ptr & 0xF0) | (font_ptr[5] & 0x0F); screen_ptr += 256;
        *screen_ptr = (*screen_ptr & 0xF0) | (font_ptr[6] & 0x0F); // Línea 7
    }
    
write_attr:
    *((uint8_t*)(0x5800 + (uint16_t)y * 32 + phys_x)) = attr;
}

// ---------------------------------------------------------------------------
// Fast 64-column line renderer
static void print_line64_fast(uint8_t y, const char *s, uint8_t attr)
{
    const uint8_t *gL[32];
    const uint8_t *gR[32];
    uint8_t x, scan;
    uint8_t *ap, *sp;
    const char *p;
    uint8_t c1, c2;

    // Atributos (32 bytes)
    ap = attr_addr(y, 0);
    for (x = 0; x < 32; x++) ap[x] = attr;

    // Pre-calcular punteros a glyphs
    p = s;
    for (x = 0; x < 32; x++) {
        c1 = (uint8_t)*p;
        if (c1 == 0) {
            c1 = ' '; c2 = ' ';
        } else {
            if (c1 < 32 || c1 > 127) c1 = ' ';
            p++;
            c2 = (uint8_t)*p;
            if (c2 == 0) {
                c2 = ' ';
            } else {
                if (c2 < 32 || c2 > 127) c2 = ' ';
                p++;
            }
        }
        gL[x] = (const uint8_t*)&font64[((uint16_t)(c1 - 32)) << 3];
        gR[x] = (const uint8_t*)&font64[((uint16_t)(c2 - 32)) << 3];
    }

    // Renderizar 8 scanlines
    for (scan = 0; scan < 8; scan++) {
        sp = screen_line_addr(y, 0, scan);
        for (x = 0; x < 32; x++) {
            sp[x] = (gL[x][scan] & 0xF0) | (gR[x][scan] & 0x0F);
        }
    }
}



static uint8_t g_zero32[32] = {0};
static uint8_t g_attr32[32];
static uint8_t g_attr32_cached = 0xFF;

static void clear_line(uint8_t y, uint8_t attr)
{
    uint8_t i;

    // Bitmap: 8 scanlines of 32 bytes each -> copy pre-zeroed block with LDIR
    for (i = 0; i < 8; i++) ldir_copy_fwd(screen_line_addr(y, 0, i), g_zero32, 32);

    // Attributes: cache 32 bytes filled with the requested attr
    if (g_attr32_cached != attr) {
        memset(g_attr32, attr, 32);
        g_attr32_cached = attr;
    }
    ldir_copy_fwd(attr_addr(y, 0), g_attr32, 32);
}

static void clear_zone(uint8_t start, uint8_t lines, uint8_t attr)
{
    uint8_t i;
    for (i = 0; i < lines; i++) clear_line(start + i, attr);
}

static void print_str64(uint8_t y, uint8_t col, const char *s, uint8_t attr)
{
    while (*s && col < SCREEN_COLS) print_char64(y, col++, *s++, attr);
}


static void scroll_main_zone_fast(void)
{
    uint8_t i;

    // Bitmap: copy by scanline in contiguous blocks inside each third.
    for (i = 0; i < 8; i++) {

        // Top third: y=2..6 <- 3..7 (5 rows)
        ldir_copy_fwd(screen_line_addr(2, 0, i), screen_line_addr(3, 0, i), (uint16_t)5 * 32);

        // Boundary: y=7 <- y=8
        ldir_copy_fwd(screen_line_addr(7, 0, i), screen_line_addr(8, 0, i), 32);

        // Middle third: y=8..14 <- 9..15 (7 rows)
        ldir_copy_fwd(screen_line_addr(8, 0, i), screen_line_addr(9, 0, i), (uint16_t)7 * 32);

        // Boundary: y=15 <- y=16
        ldir_copy_fwd(screen_line_addr(15, 0, i), screen_line_addr(16, 0, i), 32);

        // Bottom third: y=16..17 <- 17..18 (2 rows)
        ldir_copy_fwd(screen_line_addr(16, 0, i), screen_line_addr(17, 0, i), (uint16_t)3 * 32);
    }

    // Attributes: fully linear
    ldir_copy_fwd(attr_addr(MAIN_START, 0), attr_addr(MAIN_START + 1, 0), (uint16_t)(MAIN_LINES - 1) * 32);

    // Clear last line
    clear_line(MAIN_END, current_attr);
}

static void scroll_main_zone(void)
{
    scroll_main_zone_fast();
}

// ============================================================================
// STATUS BAR
// ============================================================================

#define ATTR_LBL (PAPER_WHITE | INK_BLUE)
#define ATTR_VAL (PAPER_WHITE | INK_BLACK)

static void print_padded(uint8_t y, uint8_t col, const char *s, uint8_t attr, uint8_t width)
{
    uint8_t count = 0;
    while (*s && count < width) {
        print_char64(y, col++, *s++, attr);
        count++;
    }
    while (count < width) {
        print_char64(y, col++, ' ', attr);
        count++;
    }
}

static void draw_indicator(uint8_t y, uint8_t phys_x, uint8_t attr)
{
    // Patrón del indicador (círculo sólido)
    static const uint8_t gfx[] = {0x00, 0x3C, 0x7E, 0x7E, 0x7E, 0x7E, 0x3C, 0x00};
    uint8_t i;
    
    // Calculamos la dirección base UNA sola vez
    uint8_t *ptr = screen_line_addr(y, phys_x, 0);
    
    for (i = 0; i < 8; i++) {
        *ptr = gfx[i];
        ptr += 256; // Saltar al siguiente scanline (offset de 256 bytes)
    }
    *attr_addr(y, phys_x) = attr;
}

// ============================================================================
// PROGRESS BAR FOR DOWNLOADS
// ============================================================================

// --------------------------------------------------------------------------
// Minimal decimal formatting helpers (avoid pulling in printf/sprintf)
// --------------------------------------------------------------------------

static char* u32_to_dec(char *dst, uint32_t v)
{
    char tmp[10];
    uint8_t n = 0;
    if (v == 0) {
        *dst++ = '0';
        *dst = 0;
        return dst;
    }
    while (v > 0 && n < (uint8_t)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n) *dst++ = tmp[--n];
    *dst = 0;
    return dst;
}

static char* u16_to_dec(char *dst, uint16_t v)
{
    return u32_to_dec(dst, (uint32_t)v);
}

static char* str_append(char *dst, const char *src)
{
    while (*src) *dst++ = *src++;
    *dst = 0;
    return dst;
}

static char* char_append(char *dst, char c)
{
    *dst++ = c;
    *dst = 0;
    return dst;
}

// Format size in human-readable form
static void format_size(uint32_t bytes, char *buf)
{
    char *p = buf;
    if (bytes >= 1048576UL) {
        // MB con 1 decimal
        uint32_t whole = bytes / 1048576UL;
        uint32_t rem   = bytes % 1048576UL;
        uint8_t  frac  = (uint8_t)((rem * 10UL) / 1048576UL);
        p = u32_to_dec(p, whole);
        *p++ = '.';
        *p++ = (char)('0' + frac);
        *p++ = 'M';
        *p++ = 'B';
        *p = 0;
    } else if (bytes >= 1024UL) {
        // KB sin decimales
        p = u32_to_dec(p, bytes / 1024UL);
        *p++ = 'K';
        *p++ = 'B';
        *p = 0;
    } else {
        // Bytes
        p = u32_to_dec(p, bytes);
        *p++ = 'B';
        *p = 0;
    }
}

// ============================================================================
// MODIFIED DRAW_PROGRESS_BAR (NO FLICKER)
// ============================================================================

// --- COLORES CORREGIDOS (Texto Negro) ---
#define ATTR_DL_TEXT    (PAPER_WHITE | INK_BLACK)
#define ATTR_DL_NAME    (PAPER_WHITE | INK_BLUE)   
#define ATTR_DL_BAR_ON  (PAPER_WHITE | INK_RED)    
#define ATTR_DL_BAR_OFF (PAPER_WHITE | INK_BLACK) 

// Track current download to detect file changes (13 bytes = 12 chars + null)
static char progress_current_file[13] = "";

static void draw_progress_bar(const char *filename, uint32_t received, uint32_t total)
{
    char size_buf[24]; 
    char total_buf[12];
    uint8_t i;
    char name_short[13];
    
    // CAMBIO: Reducido a 16 para evitar que el ']' choque con el spinner
    #define BAR_WIDTH 16
    
    const char CHAR_BLOCK = '\x7F'; 
    
    status_bar_overwritten = 1;

    if (strlen(filename) > 12) {
        memcpy(name_short, filename, 12);
        name_short[12] = 0;
    } else {
        safe_copy(name_short, filename, sizeof(name_short));
    }
    
    uint8_t force_redraw = 0;
    if (strcmp(progress_current_file, name_short) != 0) {
        force_redraw = 1;
        safe_copy(progress_current_file, name_short, sizeof(progress_current_file));
        clear_line(STATUS_LINE, ATTR_DL_TEXT);
    }
    
    format_size(received, size_buf);
    strcat(size_buf, "/");
    format_size(total, total_buf);
    strcat(size_buf, total_buf);
    
    uint8_t col = 0;

    print_str64(STATUS_LINE, col, "Downloading:", ATTR_DL_TEXT);
    col += 12;
    
    if (force_redraw) {
        print_padded(STATUS_LINE, col, name_short, ATTR_DL_NAME, 12);
    }
    col += 12;
    
    print_char64(STATUS_LINE, col++, ' ', ATTR_DL_TEXT);
    print_char64(STATUS_LINE, col++, ' ', ATTR_DL_TEXT);
    
    print_padded(STATUS_LINE, col, size_buf, ATTR_DL_TEXT, 15);
    col += 15;
    
    // --- ZONA DE DIBUJO AJUSTADA ---
    
    // Cols 41, 42: Espacios
    print_char64(STATUS_LINE, col++, ' ', ATTR_DL_TEXT);
    print_char64(STATUS_LINE, col++, ' ', ATTR_DL_TEXT);
    
    // Col 43: Corchete Apertura
    print_char64(STATUS_LINE, col++, '[', ATTR_DL_TEXT);
    
    // Barra (Cols 44 a 59)
    uint8_t extra_blocks = 0;
    if (total > 0) {
        extra_blocks = (uint8_t)((received * BAR_WIDTH) / total);
        if (extra_blocks > BAR_WIDTH) extra_blocks = BAR_WIDTH;
    }
    
    // --- CORRECCIÓN VISUAL ---
    // Si no hemos recibido nada (0 bytes), barra vacía.
    // En cuanto entra el primer byte, forzamos al menos 1 bloque de feedback.
    uint8_t visual_fill = 0;
    if (received > 0) {
        visual_fill = 1 + extra_blocks;
    }

    for (i = 0; i < BAR_WIDTH; i++) {
        char c;
        if (i < visual_fill) {
            c = CHAR_BLOCK; 
        } else {
            c = ' ';
        }
        print_char64(STATUS_LINE, col++, c, ATTR_DL_BAR_ON);
    }

    // --- FINAL DE LÍNEA ANTI-CLASH ---
    
    // Col 60: Corchete Cierre (PAR -> Nuevo atributo)
    print_char64(STATUS_LINE, col++, ']', ATTR_DL_TEXT);
    
    // Col 61: Espacio relleno (IMPAR -> Cierra celda NEGRA)
    print_char64(STATUS_LINE, col++, ' ', ATTR_DL_TEXT);
    
    // Col 62: Espacio relleno (PAR -> Nuevo atributo)
    print_char64(STATUS_LINE, col++, ' ', PAPER_WHITE | INK_BLUE);

    // Col 63: Spinner (IMPAR -> Cierra celda AZUL)
    spinner_idx = (spinner_idx + 1) % 4;
    print_char64(STATUS_LINE, 63, spinner_chars[spinner_idx], PAPER_WHITE | INK_BLUE);
}

// Imprime una ruta ajustada al ancho de pantalla (Truncamiento por la izquierda)
// Ejemplo: "PWD: ~/juegos/aventuras"
static void print_smart_path(const char *prefix, const char *path)
{
    char buf[SCREEN_COLS + 1]; // Buffer seguro de 64+1 chars
    uint8_t prefix_len = strlen(prefix);
    uint8_t path_len = strlen(path);
    // Espacio útil = 64 - prefijo - 1 (para la tilde ~)
    int16_t max_path_space = SCREEN_COLS - prefix_len - 1; 
    
    char *p = buf;
    
    // 1. Copiamos el prefijo (ej: "PWD: ")
    p = str_append(p, prefix);
    
    // 2. Decidimos si recortar
    if (path_len > max_path_space && max_path_space > 0) {
        // Caso: Ruta muy larga -> Ponemos tilde y el final de la ruta
        p = char_append(p, '~'); 
        
        // Calculamos desde dónde copiar para que quepa justo el final
        const char *ptr_start = path + (path_len - max_path_space);
        p = str_append(p, ptr_start);
    } else {
        // Caso: Cabe entera -> La copiamos tal cual
        p = str_append(p, path);
    }
    
    // 3. Imprimimos usando el sistema principal
    // (Opcional: Forzar color de respuesta para que destaque)
    uint8_t old_attr = current_attr;
    current_attr = ATTR_RESPONSE; 
    main_print(buf);
    current_attr = old_attr;
}

// ============================================================================
// STATUS BAR (OPTIMIZADA - ACTUALIZACIÓN PARCIAL)
// ============================================================================

// Variables de caché para recordar qué hay dibujado en pantalla
static char last_host[32] = "";
static char last_user[20] = "";
static char last_path[PATH_SIZE] = "";
static uint8_t last_conn_state = 255; // 255 = Estado inválido forzar dibujo
static uint8_t force_status_redraw = 1; // Bandera para obligar a pintar todo (ej. tras CLS)

// Función para obligar a redibujar todo (usar tras borrar pantalla)
static void invalidate_status_bar(void) {
    force_status_redraw = 1;
    last_conn_state = 255;
    last_host[0] = 0;
    last_user[0] = 0;
    last_path[0] = 0;
}

static void draw_status_bar_real(void)
{
    uint8_t ind_attr;
    char buf_short[40]; 
    
    // --- NUEVA DISTRIBUCIÓN SOLICITADA ---
    // Total ancho: 64 caracteres.
    // FTP: (4) + Host (15) + Sep(1) = 20
    // USER: (5) + User (13) + Sep(1) = 19
    // PWD: (4) + Path (19) + Espacio(1) + Ind(1) = 25
    
    const uint8_t W_HOST = 15; 
    const uint8_t W_USER = 13; 
    const uint8_t W_PATH = 19; // <--- REDUCIDO DE 20 A 19 PARA EVITAR SOLAPAMIENTO
    
    // Coordenadas de inicio de los DATOS
    const uint8_t P_HOST = 4;   // Col 0 "FTP:" -> Dato en 4
    const uint8_t P_USER = 25;  // Col 20 "USER:" -> Dato en 25
    const uint8_t P_PATH = 43;  // Col 39 "PWD:" -> Dato en 43
    
    if (force_status_redraw) {
        clear_line(STATUS_LINE, ATTR_STATUS);
        print_str64(STATUS_LINE, 0,  "FTP:",  ATTR_LBL);
        print_str64(STATUS_LINE, 20, "USER:", ATTR_LBL); 
        print_str64(STATUS_LINE, 39, "PWD:",  ATTR_LBL); 
        force_status_redraw = 0;
        last_host[0] = 0; last_user[0] = 0; last_path[0] = 0; last_conn_state = 255;
    }
    
    // HOST
    if (strcmp(ftp_host, last_host) != 0) {
        if (strlen(ftp_host) > W_HOST) {
            memcpy(buf_short, ftp_host, W_HOST - 1);
            buf_short[W_HOST - 1] = '~';
            buf_short[W_HOST] = 0;
        } else {
            safe_copy(buf_short, ftp_host, sizeof(buf_short));
}
        print_padded(STATUS_LINE, P_HOST, buf_short, ATTR_VAL, W_HOST);
        safe_copy(last_host, ftp_host, sizeof(last_host));
}
    
    // USER
    if (strcmp(ftp_user, last_user) != 0) {
        if (strlen(ftp_user) > W_USER) {
            memcpy(buf_short, ftp_user, W_USER - 1);
            buf_short[W_USER - 1] = '~';
            buf_short[W_USER] = 0;
        } else {
            safe_copy(buf_short, ftp_user, sizeof(buf_short));
}
        print_padded(STATUS_LINE, P_USER, buf_short, ATTR_VAL, W_USER);
        safe_copy(last_user, ftp_user, sizeof(last_user));
}
    
    // PATH (PWD)
    if (strcmp(ftp_path, last_path) != 0) {
        uint8_t len = strlen(ftp_path);
        if (len > W_PATH) {
            // Recorte inteligente por la izquierda
            buf_short[0] = '~';
            strncpy(buf_short + 1, ftp_path + len - (W_PATH - 1), sizeof(buf_short) - 2);
            buf_short[sizeof(buf_short) - 1] = '\0';
        } else {
            safe_copy(buf_short, ftp_path, sizeof(buf_short));
}
        print_padded(STATUS_LINE, P_PATH, buf_short, ATTR_VAL, W_PATH);
        safe_copy(last_path, ftp_path, sizeof(last_path));
}
    
    // INDICADOR
    // Se dibuja en físico 31, que ocupa las columnas lógicas 62 y 63.
    // Al haber limitado el PATH a acabar en la 61, ya no hay conflicto.
    if (connection_state != last_conn_state) {
        if (connection_state == STATE_DISCONNECTED) ind_attr = STATUS_RED;
        else if (connection_state == STATE_LOGGED_IN) ind_attr = STATUS_GREEN;
        else ind_attr = STATUS_YELLOW;
        
        draw_indicator(STATUS_LINE, 31, ind_attr); 
        last_conn_state = connection_state;
    }
}

// ============================================================================
// MAIN ZONE OUTPUT
// ============================================================================

static void main_newline(void)
{
    main_col = 0;
    main_line++;
    if (main_line > MAIN_END) {
        scroll_main_zone();
        main_line = MAIN_END;
    }
}

static void main_putchar(uint8_t c)
{
    if (c == 13 || c == 10) { main_newline(); return; }
    if (c < 32) return;
    if (main_col >= SCREEN_COLS) main_newline();
    print_char64(main_line, main_col++, c, current_attr);
}

static void main_puts(const char *s)
{
    while (*s) main_putchar(*s++);
}

static void main_print(const char *s)
{
    // Optimization: if we're at start of line and string fits in one line,
    // use the fast renderer which is 3-4x faster than char-by-char
    uint8_t len = strlen(s);
    
    if (main_col == 0 && len <= SCREEN_COLS) {
        // Fast path: render entire line at once
        print_line64_fast(main_line, s, current_attr);
        main_col = 0;  // Reset column for next line
        main_newline();
    } else {
        // Slow path: char-by-char (handles wrapping, partial lines)
        main_puts(s);
        main_newline();
    }

}

// Centralized error print (reduces duplicated ATTR_ERROR + main_print sequences)
static void fail(const char *msg)
{
    current_attr = ATTR_ERROR;
    main_print(msg);
}

// Helper: print a line of repeated characters (saves ~50 bytes)
// Helper: print a line of repeated characters (or fast 1-pixel line for '-')
static void print_char_line(uint8_t len, char ch)
{
    // OPTIMIZATION: For '-' character, draw a real 1-pixel line
    // Much faster and visually cleaner than character rendering
    if (ch == '-') {
        // Raise the rule by ~6 pixels within the character cell
        draw_hline(main_line, 0, len, 1, current_attr);  // Scanline 1 (near top)
        main_newline();
        return;
    }
    
    // For other characters, use the old method
    uint8_t i;
    for (i = 0; i < len; i++) main_putchar(ch);
    main_newline();
}


// ============================================================================
// INPUT ZONE
// ============================================================================

static void draw_cursor_underline(uint8_t y, uint8_t col)
{
    uint8_t phys_x = col >> 1;
    uint8_t half = col & 1;
    uint8_t *ptr0, *ptr7;
    
    // Máscara: 0xF0 para izquierda, 0x0F para derecha
    uint8_t mask = (half == 0) ? 0xF0 : 0x0F;
    uint8_t inv_mask = ~mask;
    
    // 1. Forzar atributo Verde (limpieza de color)
    *attr_addr(y, phys_x) = ATTR_INPUT;
    
    // 2. LIMPIEZA PREVIA (Crucial para evitar que se vean dos cursores)
    // Obtenemos punteros a la línea superior (0) e inferior (7)
    ptr0 = screen_line_addr(y, phys_x, 0);
    ptr7 = screen_line_addr(y, phys_x, 7);
    
    // Borramos los píxeles del cursor en AMBAS líneas usando la máscara inversa
    // Esto borra el cursor viejo antes de pintar el nuevo
    *ptr0 &= inv_mask; 
    *ptr7 &= inv_mask; 
    
    // 3. LÓGICA EFECTIVA (Caps Lock XOR Shift)
    // Si Caps=1 y Shift=0 -> Efectivo=1 (Mayús -> Arriba)
    // Si Caps=1 y Shift=1 -> Efectivo=0 (Minús -> Abajo) <-- Lo que pediste
    uint8_t shift_pressed = key_shift_held();
    uint8_t effective_caps = (caps_lock_mode ^ shift_pressed);

    // 4. DIBUJAR NUEVO CURSOR
    if (effective_caps) {
        // Modo Mayúsculas efectivas: Sobrelínea (Scanline 0)
        *ptr0 |= mask;
    } else {
        // Modo Minúsculas efectivas: Subrayado (Scanline 7)
        *ptr7 |= mask;
    }
    
    input_cache_invalidate_cell(y, col);
}

static void redraw_input_from(uint8_t start_pos)
{
    uint8_t row, col, i;
    uint16_t abs_pos;

    if (start_pos == 0) {
        put_char64_input_cached(INPUT_START, 0, '>', ATTR_PROMPT);
    }

    for (i = start_pos; i < line_len; i++) {
        abs_pos = i + 2;
        row = INPUT_START + (abs_pos / SCREEN_COLS);
        col = abs_pos % SCREEN_COLS;
        if (row > INPUT_END) break;
        put_char64_input_cached(row, col, line_buffer[i], ATTR_INPUT);
    }

    uint16_t cur_abs = cursor_pos + 2;
    uint8_t cur_row = INPUT_START + (cur_abs / SCREEN_COLS);
    uint8_t cur_col = cur_abs % SCREEN_COLS;

    if (cur_row <= INPUT_END) {
        char c_under = (cursor_pos < line_len) ? line_buffer[cursor_pos] : ' ';
        put_char64_input_cached(cur_row, cur_col, c_under, ATTR_INPUT);
        draw_cursor_underline(cur_row, cur_col);
    }

    uint16_t end_abs = line_len + 2;
    row = INPUT_START + (end_abs / SCREEN_COLS);
    col = end_abs % SCREEN_COLS;
    
    uint8_t clear_count = 0;
    while (row <= INPUT_END && clear_count < 8) {
        if (!(row == cur_row && col == cur_col)) {
            put_char64_input_cached(row, col, ' ', ATTR_INPUT_BG);
        }
        col++;
        if (col >= SCREEN_COLS) {
            col = 0;
            row++;
        }
        clear_count++;
    }
}

static void input_clear(void)
{
    line_len = 0;
    line_buffer[0] = 0;
    cursor_pos = 0;
    hist_pos = -1;
    
    input_cache_invalidate();
    clear_zone(INPUT_START, INPUT_LINES, ATTR_INPUT_BG);
    
    put_char64_input_cached(INPUT_START, 0, '>', ATTR_PROMPT);
    put_char64_input_cached(INPUT_START, 2, ' ', ATTR_INPUT);
    draw_cursor_underline(INPUT_START, 2);
}

static void refresh_cursor_char(uint8_t idx, uint8_t show_cursor)
{
    uint16_t abs_pos = idx + 2;
    uint8_t row = INPUT_START + (abs_pos / SCREEN_COLS);
    uint8_t col = abs_pos % SCREEN_COLS;
    
    if (row > INPUT_END) return;

    char c = (idx < line_len) ? line_buffer[idx] : ' ';
    put_char64_input_cached(row, col, c, ATTR_INPUT);

    if (show_cursor) {
        draw_cursor_underline(row, col);
    }
}

static void input_backspace(void)
{
    if (cursor_pos > 0) {
        uint8_t was_at_end = (cursor_pos == line_len);
        
        cursor_pos--;
        
        if (cursor_pos < line_len - 1) {
            memmove(&line_buffer[cursor_pos], &line_buffer[cursor_pos + 1], line_len - cursor_pos - 1);
        }
        line_len--;
        line_buffer[line_len] = 0;
        
        if (was_at_end) {
            uint16_t old_pos = cursor_pos + 1 + 2;
            uint8_t old_row = INPUT_START + (old_pos / SCREEN_COLS);
            uint8_t old_col = old_pos % SCREEN_COLS;
            if (old_row <= INPUT_END) {
                put_char64_input_cached(old_row, old_col, ' ', ATTR_INPUT_BG);
            }
            refresh_cursor_char(cursor_pos, 1);
        } else {
            redraw_input_from(cursor_pos);
        }
    }
}

static void input_left(void)
{
    if (cursor_pos > 0) {
        refresh_cursor_char(cursor_pos, 0);
        cursor_pos--;
        refresh_cursor_char(cursor_pos, 1);
    }
}

static void input_right(void)
{
    if (cursor_pos < line_len) {
        refresh_cursor_char(cursor_pos, 0);
        cursor_pos++;
        refresh_cursor_char(cursor_pos, 1);
    }
}

static void set_input_busy(uint8_t is_busy)
{
    uint16_t cur_abs;
    uint8_t row, col;
    char c_under;

    if (is_busy) {
        cur_abs = cursor_pos + 2;
        row = INPUT_START + (cur_abs / SCREEN_COLS);
        col = cur_abs % SCREEN_COLS;
        
        if (row <= INPUT_END) {
            c_under = (cursor_pos < line_len) ? line_buffer[cursor_pos] : ' ';
            put_char64_input_cached(row, col, c_under, ATTR_INPUT);
        }
    } else {
        redraw_input_from(cursor_pos);
    }
}

// ============================================================================
// KEYBOARD HANDLING (OPTIMIZED)
// ============================================================================

static uint8_t last_k = 0;
static uint16_t repeat_timer = 0;
static uint8_t debounce_zero = 0;

static uint8_t read_key(void)
{
    uint8_t k = in_inkey();

    if (k == 0) {
        last_k = 0;
        repeat_timer = 0;
        if (debounce_zero > 0) debounce_zero--;
        return 0;
    }

    // Debounce simplificado para '0'
    if (k == '0' && debounce_zero > 0) {
        debounce_zero--;
        return 0;
    }

    // --- NUEVA PULSACIÓN ---
    if (k != last_k) {
        last_k = k;
        
        // TIEMPOS INICIALES (Delay antes de empezar a repetir)
        if (k == KEY_BACKSPACE) {
            repeat_timer = 3;  // 60ms - Más rápido
            debounce_zero = 5; 
        } else if (k == KEY_LEFT || k == KEY_RIGHT) {
            repeat_timer = 3;  // 60ms
        } else if (k == KEY_UP || k == KEY_DOWN) {
            repeat_timer = 3;  // 60ms
        } else {
            repeat_timer = 2; // 40ms - Menos delay para teclas normales
        }

        return k;
    }

    // --- TECLA MANTENIDA (AUTO-REPEAT) ---
    
    if (k == KEY_BACKSPACE) debounce_zero = 5;

    if (repeat_timer > 0) {
        repeat_timer--;
        return 0;
    } else {
        // VELOCIDADES DE REPETICIÓN
        
        if (k == KEY_BACKSPACE) {
            repeat_timer = 1;  // 20ms - Más rápido
            return k;
        }
        if (k == KEY_LEFT || k == KEY_RIGHT) {
            repeat_timer = 1;  // 20ms - Más ágil
            return k;
        }
        if (k == KEY_UP || k == KEY_DOWN) {
            repeat_timer = 1;  // 20ms
            return k;
        }
        
        return 0;
    }
}

// ============================================================================
// UART LOW LEVEL
// ============================================================================

static void uart_flush_rx(void)
{
    uint16_t max_wait = 500;
    uint16_t max_bytes = 500;
    
    while (max_bytes > 0) {
        if (ay_uart_ready()) {
            ay_uart_read();
            max_bytes--;
            max_wait = 100;
        } else {
            if (max_wait == 0) break;
            max_wait--;
        }
    }
}

static void uart_flush_hard(void)
{
    uint8_t i;
    // Espera breve para datos pendientes (logica de espatzx)
    for (i = 0; i < 2; i++) HALT();
    uart_flush_rx(); // Llama a tu funcion existente uart_flush_rx
    HALT();
    uart_flush_rx();
}

static void uart_send_string(const char *s)
{
    while (*s) ay_uart_send(*s++);
}

// Wait N video frames (wall-clock pacing). Assumes interrupts enabled.
static void wait_frames(uint16_t frames)
{
    while (frames--) HALT();
}

static void wait_drain(uint16_t frames)
{
    while (frames--) {
        HALT();
        uart_drain_to_buffer(); // <--- ESTO ES LA CLAVE
    }
}

static void esp_send_at(const char *cmd)
{
    if (debug_mode && debug_enabled) {
        uint8_t saved_attr = current_attr;
        current_attr = ATTR_LOCAL;
        main_puts(">> ");
        main_print(cmd);
        current_attr = saved_attr;
    }
    uart_send_string(cmd);
    uart_send_string(S_CRLF);
}

static uint8_t try_read_line(void)
{
    uart_drain_to_buffer();
    
    int16_t c;
    while ((c = rb_pop()) != -1) {
        if (c == '\r') continue;
        if (c == '\n') {
            rx_line[rx_pos] = '\0';
            
            // If we had overflow, discard this line entirely
            if (rx_overflow) {
                rx_overflow = 0;
                rx_pos = 0;
                continue;  // Skip this truncated line
            }
            
            if (rx_pos > 0) {
                // Debug output (only if both flags set)
                if (debug_mode && debug_enabled) {
                    uint8_t saved_attr = current_attr;
                    current_attr = ATTR_RESPONSE;
                    main_puts("<< ");
                    main_print(rx_line);
                    current_attr = saved_attr;
                }
                rx_pos = 0;
                return 1;
            }
            continue;
        }
        
        // Accumulate character or set overflow flag
        if (rx_pos < sizeof(rx_line) - 1) {
            rx_line[rx_pos++] = (char)c;
        } else {
            // Buffer full - enter overflow mode, discard until \n
            rx_overflow = 1;
        }
    }
    return 0;
}

// Unified response wait function
// expected = NULL: wait for OK/ERROR (AT command response)
// expected = string: wait for specific pattern (e.g., "CONNECT", "+CWJAP:")
// Returns 1 on success, 0 on failure/timeout
static uint8_t wait_for_string(const char *expected, uint16_t max_frames)
{
    uint16_t frames = 0;
    
    rx_pos = 0;
    
    while (frames < max_frames) {
        HALT();
        
        // Cancelación con EDIT después de HALT (sincronizado con vsync)
        if (key_edit_down()) {
            return 0;
        }
        
        uart_drain_to_buffer();
        
        if (try_read_line()) {
            // Check failure conditions FIRST (critical for CIPSTART)
            if (strstr(rx_line, "CONNECT FAIL") != NULL) return 0;
            if (strstr(rx_line, "DNS Fail") != NULL) return 0;
            if (rx_line[0] == 'E' && rx_line[1] == 'R' && rx_line[2] == 'R') return 0;
            if (rx_line[0] == 'F' && rx_line[1] == 'A' && rx_line[2] == 'I') return 0;
            
            // Check success conditions
            if (expected != NULL && strstr(rx_line, expected) != NULL) return 1;
            if (rx_line[0] == 'O' && rx_line[1] == 'K') return 1;
            
            rx_pos = 0;
        }
        
        frames++;
    }
    
    return 0;
}

// Legacy wrapper for code clarity
#define wait_for_response(max_frames) wait_for_string(NULL, max_frames)

// ============================================================================
// DETECCIÓN DE DESCONEXIÓN FTP
// ============================================================================
// El servidor envía 421 dentro de +IPD: "+IPD,0,XX:421 Timeout".
// También puede enviar "0,CLOSED" directamente del ESP
// Returns: 0=no disconnect, 1=socket closed, 2=server 421

static uint8_t check_disconnect_message(void)
{
    // Socket cerrado por ESP
    if (strncmp(rx_line, "0,CLOSED", 8) == 0) {
        return 1;
    }
    // 421 viene dentro de +IPD,0,XX:421...
   if (strncmp(rx_line, S_IPD0, 7) == 0) {
        char *payload = strchr(rx_line, ':');
        if (payload && strncmp(payload + 1, "421", 3) == 0) {
            return 2;
        }
    }
    return 0;
}

// Check if server has disconnected us (call during idle waits)
// Returns 1 if disconnected, 0 if still connected


// ============================================================================
// ESP INITIALIZATION
// ============================================================================


static uint8_t probe_esp(void)
{
    uint8_t tries;
    uint16_t timeout;
    
    // Intentar 3 veces, igual que espATZX
    for (tries = 0; tries < 3; tries++) {
        
        // Limpieza agresiva antes de enviar
        uart_flush_hard(); 
        
        uart_send_string("AT\r\n");
        
        rx_pos = 0;
        rb_flush(); // Reseteamos el buffer circular
        timeout = 0;
        
        // Usamos FRAMES_1S (50 frames) como timeout, igual que la lógica de espATZX
        while (timeout < FRAMES_1S) {
            
            // Drenar hardware al buffer circular
            uart_drain_to_buffer();
            
            if (try_read_line()) {
                // Si vemos un OK, el ESP está vivo y sincronizado
                if (strcmp(rx_line, "OK") == 0) return 1;
                
                // Si vemos ERROR, también está vivo (solo que no le gustó el comando anterior)
                if (strstr(rx_line, "ERROR") != NULL) return 1;
                
                // Debug: Si quieres ver qué recibe, descomenta esto temporalmente:
                // main_print(rx_line); 
                
                rx_pos = 0;
            }
            
            wait_frames(1);
            timeout++;
        }
    }
    return 0;
}

static uint8_t check_wifi_connection(void)
{
    uint16_t frames = 0;
    int16_t c;
    uint8_t dot_count = 0;
    uint8_t digit_count = 0;
    uint8_t first_digit = 0;
    uint8_t found_ip = 0;
    
    uart_flush_rx();  // Limpiar buffer antes de comando
    uart_send_string("AT+CIFSR\r\n");
    
    // Timeout ~4 segundos (200 frames)
    while (frames < 200 && !found_ip) {
        HALT();
        
        // Cancelación con EDIT
        if (key_edit_down()) {
            uart_flush_rx();  // Limpiar buffer en cancelación
            return 2;
        }
        
        uart_drain_to_buffer();
        
        // Procesar bytes disponibles
        while ((c = rb_pop()) != -1) {
            // Detectar fin de respuesta
            if (c == 'O') {
                int16_t c2 = rb_pop();
                if (c2 == 'K') goto done;
                continue;
            }
            
            // Buscar inicio de IP (dígito 1-9)
            if (c >= '1' && c <= '9' && digit_count == 0) {
                uint8_t ip_idx = 0;
                
                // Guardar primer dígito
                first_digit = c;
                wifi_client_ip[ip_idx++] = (char)c;
                
                digit_count = 1;
                dot_count = 0;
                
                // Leer resto de posible IP del buffer
                while ((c = rb_pop()) != -1) {
                    if (c >= '0' && c <= '9') {
                        if (ip_idx < 15) wifi_client_ip[ip_idx++] = (char)c;
                        digit_count++;
                    } else if (c == '.') {
                        if (ip_idx < 15) wifi_client_ip[ip_idx++] = (char)c;
                        dot_count++;
                        digit_count = 0;
                    } else {
                        break;
                    }
                }
                
                wifi_client_ip[ip_idx] = 0; // Terminador nulo
                
                // Si encontramos 3 puntos, es una IP válida
                if (dot_count == 3 && first_digit != '0') {
                    found_ip = 1;
                    break;
                }
                
                digit_count = 0;
                dot_count = 0;
            }
        }
        
        frames++;
    }
    
done:
    uart_flush_rx();  // Limpiar buffer al finalizar
    return found_ip ? 1 : 0;
}


// ============================================================================
// NUEVA INICIALIZACIÓN OPTIMISTA (Estilo espATZX + Lógica FTP)
// ============================================================================

// 1. Función auxiliar para configurar modo FTP (CIPMUX=1)
// Se usa tanto en el arranque rápido como en el lento.
static void setup_ftp_mode(void)
{
    uint8_t i;
    // Aseguramos ATE0 por si acaso
    uart_send_string("ATE0\r\n");
    for (i=0; i<5; i++) HALT();
    while (ay_uart_ready()) ay_uart_read();

    // IMPRESCINDIBLE PARA FTP: Múltiples conexiones
    uart_send_string(S_AT_CIPMUX);
    for (i=0; i<5; i++) HALT();
    while (ay_uart_ready()) ay_uart_read();
}

// 2. Inicialización completa (Fallback cuando algo va mal)
// Esta es la versión robusta que discutimos antes
static void full_initialization_sequence(void)
{
    uint8_t i;
    
    current_attr = ATTR_LOCAL;
    main_puts("Full initialization.");
    main_newline();

    // Reset lógico de la comunicación (ATE0, flushes...)
    // Nota: No enviamos +++ ni RST para no romper más cosas
    setup_ftp_mode();

    main_puts("Probing ESP.");
    
    // Usamos el probe_esp robusto (definido en tu código anterior)
    if (!probe_esp()) {
        main_newline();
        current_attr = ATTR_ERROR;
        main_puts("ESP not responding!");
        main_newline();
        connection_state = STATE_DISCONNECTED;
        draw_status_bar();
        return;
    }
    
    // Fix color: espacio en verde, luego OK en azul
    main_puts(" ");
    current_attr = ATTR_RESPONSE;
    main_puts("OK");
    main_newline();
    
    current_attr = ATTR_LOCAL;
    main_puts(S_CHECKING);
    main_newline();
    
    // Usamos el check_wifi_connection robusto (definido en tu código anterior)
    uint8_t wifi_result = check_wifi_connection();
    if (wifi_result == 1) {
        current_attr = ATTR_RESPONSE;
        main_puts("WiFi connected");
        main_newline();
        connection_state = STATE_WIFI_OK;
    } else if (wifi_result == 2) {
        current_attr = ATTR_ERROR;
        main_puts(S_CANCEL);
        main_newline();
        connection_state = STATE_DISCONNECTED;
    } else {
        current_attr = ATTR_ERROR;
        main_puts("No WiFi connection");
        main_newline();
        connection_state = STATE_DISCONNECTED;
    }
    draw_status_bar();
}

static void smart_init(void)
{
    uint8_t i;
    uint16_t frames;

    current_attr = ATTR_LOCAL;
    main_puts("Initializing.");

    ay_uart_init();

    // Wait for UART to be ready (10 frames = 200ms) - CRITICAL!
    for (i = 0; i < 10; i++) HALT();
    
    uart_flush_rx();
    
    // Exit transparent/data mode if stuck
    uart_send_string("+++");
    for (i = 0; i < 10; i++) HALT();
    uart_flush_rx();
    
    // Disable echo
    uart_send_string("ATE0\r\n");
    for (i = 0; i < 5; i++) HALT();
    uart_flush_rx();
    
    // Stop any TCP server
    uart_send_string("AT+CIPSERVER=0\r\n");
    for (i = 0; i < 5; i++) HALT();
    uart_flush_rx();
    
    // Close ALL connections
    uart_send_string("AT+CIPCLOSE=5\r\n");
    for (i = 0; i < 5; i++) HALT();
    uart_flush_rx();
    
    // Multi-connection mode (needed for FTP)
    uart_send_string(S_AT_CIPMUX);
    for (i = 0; i < 5; i++) HALT();
    uart_flush_rx();
    
    // Test AT
    uart_send_string("AT\r\n");
    
    rx_pos = 0;
    
    // Timeout ~3 segundos (150 frames)
    for (frames = 0; frames < 150; frames++) {
        HALT();
        uart_drain_to_buffer();
        
        if (try_read_line()) {
            if (rx_line[0] == 'O' && rx_line[1] == 'K') {
                // Fix color: espacio en verde, OK en azul
                main_puts(" ");
                current_attr = ATTR_RESPONSE;
                main_puts("OK");
                main_newline();
                goto esp_ok;
            }
            rx_pos = 0;
        }
    }
    
    // Fix color: espacio en verde, FAIL en rojo
    main_puts(" ");
    current_attr = ATTR_ERROR;
    main_puts("FAIL");
    main_newline();
    connection_state = STATE_DISCONNECTED;
    draw_status_bar();
    return;

esp_ok:
    // Check WiFi
    current_attr = ATTR_LOCAL;
    main_puts(S_CHECKING);
    
    uart_flush_rx();
    uart_send_string("AT+CWJAP?\r\n");
    
    // Timeout ~4 segundos (200 frames)
    if (wait_for_string("+CWJAP:", 200)) {
        // Fix color: espacio en verde, OK en azul
        main_puts(" ");
        current_attr = ATTR_RESPONSE;
        main_puts("OK");
        main_newline();
        check_wifi_connection();
        connection_state = STATE_WIFI_OK;
    } else {
        // Fix color: espacio en verde, No WiFi en rojo
        main_puts(" ");
        current_attr = ATTR_ERROR;
        main_puts("No WiFi");
        main_newline();
        connection_state = STATE_DISCONNECTED;
    }
    
    draw_status_bar();
}

// ============================================================================
// DISCONNECT CONFIRMATION HELPER
// ============================================================================

// Forward declarations (needed by confirm_disconnect)
static void esp_tcp_close(uint8_t sock);
static uint8_t esp_tcp_send(uint8_t sock, const char *data, uint16_t len);
static uint8_t quick_noop_check(uint16_t max_frames);

// Asks user to confirm disconnect if already connected.
// Returns 1 if OK to proceed (was disconnected or user confirmed)
// Returns 0 if user cancelled
static uint8_t confirm_disconnect(void)
{
    if (connection_state < STATE_FTP_CONNECTED) {
        return 1;  // Not connected, OK to proceed
    }
    
    fail("Already connected. Disconnect? (Y/N)");
    while(1) {
        if (ay_uart_ready()) ay_uart_read();
        
        // Usar key_edit_down() combinado con lectura normal
        uint8_t k = in_inkey();
        if (k == 'n' || k == 'N' || key_edit_down()) { 
            current_attr = ATTR_LOCAL;
            main_print(S_CANCEL);
            return 0;
        }
        if (k == 'y' || k == 'Y' || k == 13) break;
        HALT();  // Procesar interrupciones mientras esperamos
    }
    
    // User confirmed - disconnect cleanly
    current_attr = ATTR_LOCAL;
    main_print("Disconnecting.");
    
    // Send QUIT to server (polite disconnect)
    safe_copy(ftp_cmd_buffer, S_CMD_QUIT, sizeof(ftp_cmd_buffer));
    esp_tcp_send(0, ftp_cmd_buffer, 6);
    wait_frames(15);
    
    // Close socket
    esp_tcp_close(0);
    rb_flush();
    rx_pos = 0;
    
    // Reset state
    clear_ftp_state();
    
    // --- CAMBIO: FORZAR REDIBUJADO INMEDIATO ---
    // Esto limpia visualmente la barra (pone "---") ANTES de
    // que cmd_open empiece a imprimir "Connecting to".
    draw_status_bar_real();
    
    return 1;  // OK to proceed
}

// ============================================================================
// ESP TCP LAYER
// ============================================================================

static uint8_t esp_tcp_connect(uint8_t sock, const char *host, uint16_t port)
{
    uint8_t result;
    
    // Temporarily disable debug output during connection (timing critical)
    debug_enabled = 0;
    
    uart_flush_rx();
    {
        char *p = tx_buffer;
        p = str_append(p, "AT+CIPSTART=");
        p = u16_to_dec(p, (uint16_t)sock);
        p = str_append(p, ",\"TCP\",\"");
        p = str_append(p, host);
        p = str_append(p, "\",");
        p = u16_to_dec(p, port);
    }
    uart_send_string(tx_buffer);
    uart_send_string(S_CRLF);
    result = wait_for_string("CONNECT", 500);  // ~10 segundos
    
    debug_enabled = 1;
    return result;
}

static void esp_tcp_close(uint8_t sock)
{
    {
        char *p = tx_buffer;
        p = str_append(p, "AT+CIPCLOSE=");
        p = u16_to_dec(p, (uint16_t)sock);
    }
    esp_send_at(tx_buffer);
    wait_for_response(100);  // ~2 segundos
}

static uint8_t esp_tcp_send(uint8_t sock, const char *data, uint16_t len)
{
    uint16_t i;
    uint16_t frames;
    int16_t c;
    
    // Limpiamos el parser de respuestas para usarlo como detector de errores
    rx_pos = 0; 
    
    {
        char *p = tx_buffer;
        p = str_append(p, "AT+CIPSEND=");
        p = u16_to_dec(p, (uint16_t)sock);
        p = char_append(p, ',');
        p = u16_to_dec(p, len);
    }
    esp_send_at(tx_buffer);
    
    // CRITICAL FIX: When debug is active, printing to screen takes time.
    // During that time, ESP sends its response but nobody is draining to ring buffer.
    // Add small delay + drain to capture bytes that arrived during screen print.
    wait_frames(2);
    uart_drain_to_buffer();
    
    // Esperar prompt '>' - Timeout ~3 segundos (150 frames)
    frames = 0;
    while (frames < 150) {
        HALT();
        
        // Cancelación manual
        if (key_edit_down()) {
            return 0;
        }
        
        uart_drain_to_buffer();
        while ((c = rb_pop()) != -1) {
            // 1. ÉXITO: Recibimos el prompt '>'
            if (c == '>') goto send_data;
            
            // 2. DETECCIÓN DE ERROR RÁPIDA (Fail-Fast)
            // Si el ESP responde ERROR, no tiene sentido esperar 3 segundos.
            if (c == '\n') {
                rx_line[rx_pos] = 0;
                if (strstr(rx_line, "ERROR") || 
                    strstr(rx_line, "link is not") || 
                    strstr(rx_line, "CLOSED")) {
                    return 0; // Abortar inmediatamente
                }
                rx_pos = 0;
            } else if (c != '\r' && rx_pos < 120) {
                rx_line[rx_pos++] = (char)c;
            }
        }
        frames++;
    }
    return 0;  // Timeout real (si el ESP no responde nada)
    
send_data:
    // Pequeño retardo técnico post-prompt
    
    // Enviar datos crudos
    for (i = 0; i < len; i++) {
        ay_uart_send(data[i]);
    }
    
    // Breve espera para asegurar que el buffer de salida se vacíe antes de seguir
    wait_frames(2);
    
    return 1;
}

// ============================================================================
// QUICK CONTROL-CHANNEL PROBE (LOW COST)
// ============================================================================
// Sends NOOP and waits briefly for a 2xx reply. Hard-bounded by max_frames.
// Returns 1 if a 2xx reply is observed, else 0.
static uint8_t quick_noop_check(uint16_t max_frames)
{
    uint16_t frames = 0;

    // If we are not in an FTP-connected state, nothing to probe.
    if (connection_state < STATE_FTP_CONNECTED) {
        return 0;
    }

    // Send NOOP on control socket (0)
    if (!esp_tcp_send(0, "NOOP\r\n", 6)) {
        return 0;
    }

    // Wait for a short, bounded time for any 2xx response line.
    while (frames < max_frames) {
        HALT();
        uart_drain_to_buffer();
        if (try_read_line()) {
            // Accept any 2xx (200, 220, 221, etc.) as a positive liveness signal
            if (rx_line[0] == '2' && rx_line[1] >= '0' && rx_line[1] <= '9' && rx_line[2] >= '0' && rx_line[2] <= '9') {
                return 1;
            }
            // If we explicitly see CLOSED, treat as dead
            if (strstr(rx_line, S_CLOSED1)) {
                return 0;
            }
        }
        frames++;
    }

    return 0;
}

// ============================================================================
// FTP PROTOCOL LAYER
// ============================================================================

static uint8_t ftp_command(const char *cmd)
{
    uint16_t len = strlen(cmd);
    
    // PROTECCIÓN CRÍTICA: Evitar Buffer Overflow
    // Reservamos 3 bytes: 2 para \r\n y 1 para el terminador nulo
    if (len > (sizeof(ftp_cmd_buffer) - 3)) {
        fail("Buffer overflow!");
        return 0;
    }
    
    strncpy(ftp_cmd_buffer, cmd, sizeof(ftp_cmd_buffer) - 3);
    ftp_cmd_buffer[sizeof(ftp_cmd_buffer) - 3] = '\0';
    strcat(ftp_cmd_buffer, S_CRLF);
    
    return esp_tcp_send(0, ftp_cmd_buffer, len + 2);
}

// Parse decimal number from string, advance pointer
static uint16_t parse_decimal(char **pp)
{
    uint16_t val = 0;
    char *p = *pp;
    while (*p >= '0' && *p <= '9') {
        val = val * 10 + (*p - '0');
        p++;
    }
    *pp = p;
    return val;
}

static uint16_t ftp_passive(void)
{
    uint16_t p1, p2;
    char *p;
    uint8_t i;
    uint8_t octets[4];
    uint16_t frames = 0;
    
    if (!ftp_command("PASV")) {
        main_print("[PASV send fail]");
        return 0;
    }
    
    rx_pos = 0;
    
    // Timeout ~5 segundos (250 frames)
    while (frames < 250) {
        HALT();
        
        if (key_edit_down()) {
            main_print(S_CANCEL);
            return 0;
        }
        
        uart_drain_to_buffer();
        
        if (try_read_line()) {
            if (strncmp(rx_line, S_IPD0, 7) == 0) {
                p = strchr(rx_line, ':');
                if (p && strstr(p, "227")) {
                    p = strchr(p, '(');
                    if (p) {
                        p++;
                        for (i = 0; i < 4; i++) {
                            octets[i] = (uint8_t)parse_decimal(&p);
                            if (*p == ',') p++;
                        }
                        {
                            char *q = data_ip;
                            q = u16_to_dec(q, (uint16_t)octets[0]);
                            q = char_append(q, '.');
                            q = u16_to_dec(q, (uint16_t)octets[1]);
                            q = char_append(q, '.');
                            q = u16_to_dec(q, (uint16_t)octets[2]);
                            q = char_append(q, '.');
                            q = u16_to_dec(q, (uint16_t)octets[3]);
                        }
                        
                        p1 = parse_decimal(&p);
                        if (*p == ',') p++;
                        p2 = parse_decimal(&p);
                        
                        data_port = (p1 << 8) | p2;
                        
                        return data_port;
                    }
                }
            }
            rx_pos = 0;
        }
        frames++;
    }
    main_print("[PASV timeout]");
    return 0;
}

static uint8_t ftp_open_data(void)
{
    uint8_t result;
    if (data_port == 0) {
        main_print("[No data port]");
        return 0;
    }
        
    result = esp_tcp_connect(1, data_ip, data_port);
     
    return result;
}

static void ftp_close_data(void)
{
    // Cierra el socket de datos (que es el 1 según ftp_open_data)
    esp_tcp_close(1);
    
    // Esperar y drenar datos residuales que puedan llegar
    // después de cerrar el socket
    uint16_t i;
    for (i = 0; i < 25; i++) {
        HALT();
        uart_drain_to_buffer();
    }
    
    // Vaciar el ring buffer para descartar datos residuales
    rb_flush();
}

// Setup PASV + data connection + send LIST command
// Returns 1 on success, 0 on failure (with error message printed)
static uint8_t setup_list_transfer(void)
{
    rx_reset_all();  // Garantizar estado limpio antes de LIST
    
    if (ftp_passive() == 0) {
        fail(S_PASV_FAIL);
        return 0;
    }
    
    if (!ftp_open_data()) {
        fail(S_DATA_FAIL);
        return 0;
    }
    
    if (!ftp_command("LIST")) {
        ftp_close_data();
        fail(S_LIST_FAIL);
        return 0;
    }
    
    // Wait for "150 Opening data connection" response
    // This is critical - without it, we start reading before server sends data
    uint16_t frames = 0;
    char resp_buf[64];
    uint8_t resp_pos = 0;
    int16_t c;
    
    while (frames < 200) {  // ~4 segundos
        uart_drain_to_buffer();
        c = rb_pop();
        
        if (c == -1) {
            HALT();
            if (key_edit_down()) {
                ftp_close_data();
                fail(S_CANCEL);
                return 0;
            }
            frames++;
            continue;
        }
        
        if (c == '\r') continue;
        
        if (c == '\n') {
            resp_buf[resp_pos] = 0;
            
            // Check for "150" or "125" response on control channel
            if (strncmp(resp_buf, "+IPD,0,", 7) == 0) {
                if (strstr(resp_buf, "150") || strstr(resp_buf, "125")) {
                    // Server confirmed - data will start coming
                    wait_frames(5);  // Small delay for data to arrive
                    return 1;
                }
                if (strstr(resp_buf, "550") || strstr(resp_buf, "226")) {
                    // Error or empty directory
                    ftp_close_data();
                    return 1;  // Continue anyway, let cmd_list_core handle it
                }
            }
            
            // Check if data channel is already sending (early data)
            if (strncmp(resp_buf, "+IPD,1,", 7) == 0) {
                // Data arriving - flush this line and let cmd_list_core handle it
                while (c != '\n' && c != -1) {
                    c = rb_pop();
                    if (c == -1) break;
                }
                return 1;
            }
            
            resp_pos = 0;
            continue;
        }
        
        if (resp_pos < sizeof(resp_buf) - 1) {
            resp_buf[resp_pos++] = (char)c;
        }
    }
    
    // Timeout - try anyway, might work
    return 1;
}

// ============================================================================
// ESXDOS FILE OPERATIONS
// ============================================================================

static uint8_t esx_fopen_write(const char *filename)
{
    (void)filename;
    __asm
        ; Get filename pointer from stack
        ld hl, 2
        add hl, sp
        ld hl, (hl)
        push hl             ; Save for IX
        
        ; Get current drive first (like BridgeZX)
        xor a
        rst 0x08
        defb 0x89           ; ESX_GETSETDRV
        jr c, esx_open_fail
        
        ; A now has drive, set up for FOPEN
        pop ix              ; IX = filename (required by esxDOS)
        ld b, 0x0E          ; FMODE_CREATE = create/truncate + write
        rst 0x08
        defb 0x9A           ; ESX_FOPEN
        jr c, esx_open_fail
        ld l, a
        jr esx_open_done
    esx_open_fail:
        ld l, 255
    esx_open_done:
        ld h, 0
    __endasm;
}

// Global variables for esxDOS operations (avoid stack parameter issues)
static uint8_t esx_handle;
static void *esx_buffer;
static uint16_t esx_length;

static uint16_t esx_fwrite(uint8_t handle, void *buf, uint16_t len)
{
    esx_handle = handle;
    esx_buffer = buf;
    esx_length = len;
    
    __asm
        ld a, (_esx_handle)
        ld hl, (_esx_buffer)
        push hl
        pop ix              ; IX = buffer address
        ld bc, (_esx_length)
        rst 0x08
        defb 0x9E           ; ESX_FWRITE
        jr c, esx_write_fail
        ; BC = bytes written
        ld h, b
        ld l, c
        jr esx_write_done
    esx_write_fail:
        ld hl, 0
    esx_write_done:
    __endasm;
}

static void esx_fclose(uint8_t handle)
{
    (void)handle;
    __asm
        ld hl, 2
        add hl, sp
        ld a, (hl)
        rst 0x08
        defb 0x9C           ; ESX_FSYNC first
        ld hl, 2
        add hl, sp
        ld a, (hl)
        rst 0x08
        defb 0x9B           ; ESX_FCLOSE
    __endasm;
}


// ============================================================================
// COMMAND HANDLERS
// ============================================================================

static void cmd_pwd(void); 
static void cmd_pwd_silent(void);
static void cmd_cd(const char *path);

// ============================================================================
// CMD_OPEN MODIFICADO (Soporte para Puertos)
// ============================================================================

static uint8_t ensure_logged_in(void)
{
    // Primero verificar si hay mensajes de desconexión pendientes
    if (connection_state >= STATE_FTP_CONNECTED) {
        // Drenar y verificar
        uart_drain_to_buffer();
        while (try_read_line()) {
            if (check_disconnect_message()) {
                clear_ftp_state();
                draw_status_bar();
                fail("Connection lost");
                return 0;
            }
            rx_pos = 0;
        }
    }
    
    // Si ya estamos logueados, todo perfecto
    if (connection_state == STATE_LOGGED_IN) return 1;

    // Si no, mostramos el error adecuado
    if (connection_state == STATE_DISCONNECTED || connection_state == STATE_WIFI_OK) {
        fail("Not connected. Use OPEN.");
    } else if (connection_state == STATE_FTP_CONNECTED) {
        fail("Not logged in. Use USER.");
    }
    
    return 0; // Indica que NO se puede continuar
}

// ============================================================================
// HELPER: Parse host[:port][/path] format
// ============================================================================
// Modifies input string in place, returns parsed components
// Returns: port number (21 if not specified)
static uint16_t parse_host_port_path(char *input, char **out_host, char **out_path)
{
    uint16_t port = 21;
    char *host = input;
    char *path = NULL;
    
    // Separate path (after '/')
    char *slash = strchr(host, '/');
    if (slash) {
        *slash = 0;
        path = slash + 1;
    }
    
    // Separate port (after ':')
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = 0;
        char *p_port = colon + 1;
        uint16_t p_val = 0;
        while (*p_port >= '0' && *p_port <= '9') {
            p_val = p_val * 10 + (*p_port - '0');
            p_port++;
        }
        if (p_val > 0) port = p_val;
    }
    
    *out_host = host;
    *out_path = path;
    return port;
}

static void cmd_open(const char *host, uint16_t port)
{
    // 1. Confirmar desconexión si es necesario
    if (!confirm_disconnect()) return;
    
    // 2. Limpieza de estado visual
    // Aseguramos que el path sea "---" hasta que el servidor diga lo contrario
    safe_copy(ftp_path, "---", sizeof(ftp_path));
    
    // Invalidar solo el campo PWD en lugar de toda la barra
    last_path[0] = 0;
    draw_status_bar();

    // 3. Iniciar Conexión
    current_attr = ATTR_LOCAL;
    {
        char *p = tx_buffer;
        p = str_append(p, "Connecting to ");
        p = str_append(p, host);
        p = char_append(p, ':');
        p = u16_to_dec(p, port);
        p = str_append(p, S_DOTS);
    }
    main_print(tx_buffer);
    
    debug_enabled = 0;
    
    // 4. TCP Connect
    if (!esp_tcp_connect(0, host, port)) {
        debug_enabled = 1;
        esp_tcp_close(0);
        wait_frames(2);
        rb_flush();
        fail("Connect failed");
        return;
    }
    
    current_attr = ATTR_LOCAL;
    main_print("Waiting for banner.");
    drain_mode_fast(); 
    wait_drain(5);
    rx_pos = 0;
    
    // 5. Espera de Banner
    uint16_t frames;
    for (frames = 0; frames < 350; frames++) {
        HALT();
        if (key_edit_down()) {
            debug_enabled = 1;
            esp_tcp_close(0);
            fail(S_CANCEL);
            return;
        }
        uart_drain_to_buffer();

        if (try_read_line()) {
            if (strstr(rx_line, "220")) {
                debug_enabled = 1;
                safe_copy(ftp_host, host, sizeof(ftp_host));
                safe_copy(ftp_user, S_EMPTY, sizeof(ftp_user));
                
                connection_state = STATE_FTP_CONNECTED;
                current_attr = ATTR_RESPONSE;
                if (main_col > 0) main_newline();
                main_print("Connected!");
                draw_status_bar(); // Mostrará FTP: host, USER: ---, PWD: ---
                return;
            }
            
            if (strstr(rx_line, "CLOSED") || strstr(rx_line, "ERROR") || strstr(rx_line, "421")) {
                debug_enabled = 1;
                esp_tcp_close(0);
                rx_reset_all();
                main_newline();
                fail("Connection rejected");
                return;
            }
            rx_pos = 0;
        }
    }
    
    debug_enabled = 1;
    fail("No FTP banner (timeout)");
    esp_tcp_close(0);
    rx_reset_all();
}

// Wait for FTP response code (like 331, 230, 530)
// Returns: FTP code (0 if timeout or cancelled)
static uint16_t user_wait_ftp_response(void)
{
    uint16_t frames = 0;
    uint16_t code = 0;
    char *p;
    
    rx_pos = 0;
    
    while (frames < 200) {
        HALT();
        
        if (key_edit_down()) {
            fail(S_CANCEL);
            return 0;
        }
        
        uart_drain_to_buffer();
        
        if (try_read_line()) {
            // Buscamos código de respuesta FTP
            if (strncmp(rx_line, S_IPD0, 7) == 0) {
                p = strchr(rx_line, ':');
                if (p) {
                    p++;
                    code = 0;
                    if (*p >= '1' && *p <= '5') {
                        code = (*p++ - '0') * 100;
                        if (*p >= '0' && *p <= '9') code += (*p++ - '0') * 10;
                        if (*p >= '0' && *p <= '9') code += (*p++ - '0');
                    }
                    if (code > 0) return code;
                }
            }
            rx_pos = 0;
        }
        frames++;
    }
    
    return 0; // Timeout
}

// Fast FTP code wait - exits immediately on match
static uint8_t wait_for_ftp_code_fast(uint16_t max_frames, const char *code3)
{
    uint16_t frames = 0;
    char *p;
    
    rx_pos = 0;
    
    while (frames < max_frames) {
        HALT();
        
        if (key_edit_down()) {
            return 0;
        }
        
        uart_drain_to_buffer();
        
        while (try_read_line()) {
            if (strncmp(rx_line, S_IPD0, 7) == 0) {
                p = strchr(rx_line, ':');
                if (p) {
                    p++;
                    // Check if matches expected code
                    if (p[0] == code3[0] && p[1] == code3[1] && p[2] == code3[2]) {
                        // Handle multiline: "200-" continues, "200 " ends
                        if (p[3] == '-') {
                            continue; // Wait for terminator
                        }
                        return 1; // Success - immediate exit
                    }
                }
            }
            rx_pos = 0;
        }
        frames++;
    }
    
    return 0; // Timeout
}

// Core function for PWD - shared by cmd_pwd() and cmd_pwd_silent()
static void pwd_core(uint8_t silent)
{
    if (!ensure_logged_in()) return;    
    uint16_t frames = 0;
    
    if (!ftp_command("PWD")) return;
    
    rx_pos = 0;
    
    // Timeout ~4 segundos
    while (frames < 200) {
        HALT();
        
        if (key_edit_down()) {
            if (!silent) {
                fail(S_CANCEL);
            }
            return;
        }
        
        uart_drain_to_buffer();
        
        if (try_read_line()) {
            if (strncmp(rx_line, "+IPD,0,", 7) == 0) {
                char *start = strchr(rx_line, '"');
                if (start) {
                    start++;
                    char *end = strchr(start, '"');
                    if (end) *end = 0;
                    
                    // Guardar path
                    safe_copy(ftp_path, start, sizeof(ftp_path));
                    
                    // Si no es silencioso, imprimir
                    if (!silent) {
                        print_smart_path("PWD: ", ftp_path);
                    }
                    
                    draw_status_bar_real();
                    return;
                }
            }
            rx_pos = 0;
        }
        frames++;
    }
}

// Versión silenciosa de PWD - solo guarda el path, no imprime nada
static void cmd_pwd_silent(void)
{
    pwd_core(1);
}

static void cmd_user(const char *user, const char *pass)
{
    // Verificación de conexión
    if (connection_state < STATE_FTP_CONNECTED) {
        fail(S_NO_CONN);
        return;
    }
    
    // Verificar si ya está logeado
    if (connection_state == STATE_LOGGED_IN) {
        fail("Already logged in. Use QUIT first");
        return;
    }
    
    // Limpieza de buffers
    {
        uint8_t silence_checks = 0;
        while(silence_checks < 2) {
            if (ay_uart_ready()) { ay_uart_read(); silence_checks = 0; } 
            else { wait_frames(1); silence_checks++; }
        }
        rb_flush(); 
    }

    uint16_t code = 0;
    
    current_attr = ATTR_LOCAL;
    {
        char *p = tx_buffer;
        p = str_append(p, "Login as ");
        p = str_append(p, user);
        p = str_append(p, S_DOTS);
    }
    main_print(tx_buffer);
    
    // 1. Enviar USER
    {
        char *p = ftp_cmd_buffer;
        p = str_append(p, "USER ");
        p = str_append(p, user);
        p = str_append(p, S_CRLF);
    }
    if (!esp_tcp_send(0, ftp_cmd_buffer, strlen(ftp_cmd_buffer))) {
        fail("Send USER failed");
        return;
    }
    
    // Respuesta USER
    code = user_wait_ftp_response();
    if (code == 230) goto login_success; 
    
    if (code != 331) {
        if (code == 530) fail(S_LOGIN_BAD); 
        else if (code > 0) {
            char *p = tx_buffer;
            p = str_append(p, "USER error: ");
            p = u16_to_dec(p, code);
            fail(tx_buffer);
        } else fail("No response to USER");
        return;
    }
    
    // 2. Enviar PASS
    {
        char *p = ftp_cmd_buffer;
        p = str_append(p, "PASS ");
        p = str_append(p, pass);
        p = str_append(p, S_CRLF);
    }
    if (!esp_tcp_send(0, ftp_cmd_buffer, strlen(ftp_cmd_buffer))) {
        fail("Send PASS failed");
        return;
    }
    
    // Respuesta PASS
    code = user_wait_ftp_response();
    if (code != 230) {
        if (code == 530) fail(S_LOGIN_BAD);
        else {
            char *p = tx_buffer;
            p = str_append(p, "Login failed: ");
            p = u16_to_dec(p, code);
            fail(tx_buffer);
        }
        return;
    }
    
login_success:
    // --- ESTADO VISUAL ---
    safe_copy(ftp_user, user, sizeof(ftp_user));
    connection_state = STATE_LOGGED_IN;
    
    // PWD a "---" hasta confirmación.
    safe_copy(ftp_path, "---", sizeof(ftp_path));

    // FIX PARPADEO: Quitamos invalidate_status_bar(). 
    // Al llamar a draw_status_bar_real(), el sistema verá que ftp_user cambió
    // pero el resto no, y solo repintará el nombre de usuario suavemente.
    draw_status_bar_real(); 
    
    // Mensaje de éxito
    current_attr = ATTR_LOCAL;
    main_print("Logged in!");
    
    // --- CONFIGURACIÓN BINARIA ---
    safe_copy(ftp_cmd_buffer, "TYPE I\r\n", sizeof(ftp_cmd_buffer));
    esp_tcp_send(0, ftp_cmd_buffer, strlen(ftp_cmd_buffer));
    
    // Espera rápida del 200 - sale inmediatamente al detectarlo
    wait_for_ftp_code_fast(50, "200"); // 1 segundo máximo

    // Comportamiento estándar: Pedir PWD con mensaje mejorado
    main_puts("Getting PWD: ");  // Sin newline - continúa en misma línea
    cmd_pwd_silent();  // Nueva versión silenciosa que solo guarda el path
    
    // Imprimir path en la misma línea
    if (ftp_path[0] && strcmp(ftp_path, "---") != 0) {
        current_attr = ATTR_RESPONSE;
        main_puts(ftp_path);
        main_newline();
    } else {
        main_print("(unknown)");
    }
}

static void cmd_pwd(void)
{
    pwd_core(0);
}

// ============================================================================
// UTF-8 / ESCAPE DECODING HELPERS
// ============================================================================

static uint8_t hex_to_nibble(char c)
{
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    return 0xFF;
}

// Decode user-typed escapes into raw bytes for FTP commands.
// Supported forms:
//   %HH    (URL-style, e.g. "Gu%C3%ADas")
//   \xHH   (C-style, e.g. "Gu\xC3\xADas")
// This allows accessing UTF-8 named directories/files.
// Any invalid sequence is copied verbatim.
// Returns 1 on success, 0 on output overflow (still produces NUL-terminated result).
static uint8_t decode_path_escapes(const char *in, char *out, uint8_t out_sz)
{
    uint8_t oi = 0;
    while (*in) {
        if (oi + 1 >= out_sz) { out[oi] = 0; return 0; }

        if (*in == '%' && in[1] && in[2]) {
            uint8_t h1 = hex_to_nibble(in[1]);
            uint8_t h2 = hex_to_nibble(in[2]);
            if (h1 != 0xFF && h2 != 0xFF) {
                out[oi++] = (char)((h1 << 4) | h2);
                in += 3;
                continue;
            }
        } else if (*in == '\\' && in[1] == 'x' && in[2] && in[3]) {
            uint8_t h1 = hex_to_nibble(in[2]);
            uint8_t h2 = hex_to_nibble(in[3]);
            if (h1 != 0xFF && h2 != 0xFF) {
                out[oi++] = (char)((h1 << 4) | h2);
                in += 4;
                continue;
            }
        }

        out[oi++] = *in++;
    }
    out[oi] = 0;
    return 1;
}

// ============================================================================
// COMMAND: CD
// ============================================================================

static void cmd_cd(const char *path)
{
    if (!ensure_logged_in()) return;
    uint16_t frames = 0;
    
    // Allow accessing UTF-8 directory names by typing escaped bytes.
    // Example: "Gu%C3%ADas" or "Gu\xC3\xADas" (UTF-8 for "Guías")
    // Also works with quoted paths: CD "Mis juegos"
    char path_dec[64];
    decode_path_escapes(path, path_dec, sizeof(path_dec));
    
    {
        char *p = tx_buffer;
        p = str_append(p, "CWD ");
        p = str_append(p, path_dec);
    }
    if (!ftp_command(tx_buffer)) return;
    
    rx_pos = 0;
    
    // Timeout ~5 segundos (250 frames)
    while (frames < 250) {
        HALT();
        
        if (key_edit_down()) {
            fail(S_CANCEL);
            return;
        }

        if (try_read_line()) {
            if (strncmp(rx_line, S_IPD0, 7) == 0) {
                // Success: 250
                if (strstr(rx_line, "250")) {
                    current_attr = ATTR_RESPONSE;
                    
                    // NORMALIZAR ftp_path si está "desconocido" antes de operar
                    if (ftp_path[0] == '-' || strcmp(ftp_path, "---") == 0) {
                        safe_copy(ftp_path, "/", sizeof(ftp_path));
                    }
                    
                    // Guardar el path DECODIFICADO (cmd_pwd() lo sobreescribirá si consigue el real)
                    if (path_dec[0] == '/') {
                        // Path absoluto - usarlo directamente
                        safe_copy(ftp_path, path_dec, sizeof(ftp_path));
                        } else if (strcmp(path_dec, "..") == 0) {
                        // Subir un nivel - cortar último componente
                        char *last_slash = strrchr(ftp_path, '/');
                        if (last_slash && last_slash != ftp_path) {
                            *last_slash = '\0';
                        } else {
                            safe_copy(ftp_path, "/", sizeof(ftp_path));
                        }
                        } else {
                        // Path relativo - concatenar
                        size_t len = strlen(ftp_path);
                        if (len > 0 && ftp_path[len-1] != '/') {
                            strncat(ftp_path, "/", sizeof(ftp_path) - len - 1);
                        }
                        strncat(ftp_path, path_dec, sizeof(ftp_path) - strlen(ftp_path) - 1);
                    }
                    
                    // Invalidar solo el campo PWD en lugar de toda la barra
                    last_path[0] = 0;
                    draw_status_bar();
                    cmd_pwd();  // Intentar obtener el path real (sobreescribe si tiene éxito)
                    return;
                }
                // Error: 550 (not found), 553, etc.
                if (strstr(rx_line, "550") || strstr(rx_line, "553") || 
                    strstr(rx_line, "501") || strstr(rx_line, "500")) {
                    fail("Directory not found");
                    return;
                }
            }
            rx_pos = 0;
        }
        frames++;
    }
    fail("CD timeout");
}

// ============================================================================
// CMD_LS CON FILTRADO (LIMPIA)
// ============================================================================

// Simple case-insensitive substring search
static uint8_t str_contains(const char *haystack, const char *needle)
{
    char h, n;
    const char *hp, *np, *start;
    
    if (!*needle) return 1;
    
    for (start = haystack; *start; start++) {
        hp = start;
        np = needle;
        while (*hp && *np) {
            h = *hp;
            n = *np;
            // Convert to uppercase
            if (h >= 'a' && h <= 'z') h -= 32;
            if (n >= 'a' && n <= 'z') n -= 32;
            if (h != n) break;
            hp++;
            np++;
        }
        if (!*np) return 1;  // Found
    }
    return 0;
}

// ============================================================================
// FILE SYSTEM HELPERS (8.3 COMPLIANCE & COLLISION)
// ============================================================================

// Función necesaria para comprobar existencia (handle 0xFF = no existe)
static uint8_t esx_fopen_read(const char *filename)
{
    (void)filename;
    __asm
        ld hl, 2
        add hl, sp
        ld hl, (hl)
        push hl
        xor a
        rst 0x08
        defb 0x89           ; ESX_GETSETDRV
        jr c, esx_openr_fail
        pop ix
        ld b, 0x01          ; FMODE_READ
        rst 0x08
        defb 0x9A           ; ESX_FOPEN
        jr c, esx_openr_fail
        ld l, a
        jr esx_openr_done
    esx_openr_fail:
        ld l, 255
    esx_openr_done:
        ld h, 0
    __endasm;
}

// Convierte un nombre largo a formato estricto 8.3
// "LONG-FILENAME.EXTENSION" -> "LONG-FIL.EXT"
// "ARCHIVE.TAR.GZ" -> "ARCHIVE.GZ" (Toma la última extensión)
static void sanitize_filename_83(const char *src, char *dst)
{
    char base[9]; // 8 chars + null
    char ext[5];  // . + 3 chars + null
    const char *p_ext = NULL;
    const char *p;
    uint8_t i;

    // 1. Buscar la última extensión (último punto)
    //    Ignoramos puntos al inicio (archivos ocultos linux)
    p = src;
    while (*p) {
        if (*p == '.' && p != src) p_ext = p;
        p++;
    }

    // 2. Copiar BASE (hasta 8 caracteres válidos)
    //    Paramos si llegamos al final, al punto de extensión, o a 8 chars.
    i = 0;
    p = src;
    while (*p && p != p_ext && i < 8) {
        char c = *p++;
        // Filtro de caracteres prohibidos en FAT
        if (c >= 'a' && c <= 'z') c -= 32; // A mayúsculas
        if (c == ' ' || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c == '.') c = '_';
        if (c < 32) c = '_'; // Control chars
        
        base[i++] = c;
    }
    base[i] = 0;

    // 3. Copiar EXTENSIÓN (si existe, hasta 3 caracteres)
    ext[0] = 0;
    if (p_ext) {
        ext[0] = '.';
        p = p_ext + 1;
        i = 1; // índice 1 para empezar después del punto
        while (*p && i < 4) {
            char c = *p++;
            if (c >= 'a' && c <= 'z') c -= 32;
            if (c == ' ' || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c == '.') c = '_';
            if (c < 32) c = '_';
            ext[i++] = c;
        }
        ext[i] = 0;
    }

    // 4. Combinar (dst debe ser al menos 13 bytes)
    memcpy(dst, base, strlen(base) + 1);
    strcat(dst, ext);
}

// Si "FILE.EXT" existe, prueba "FILE~1.EXT", "FILE~2.EXT"...
static void ensure_unique_filename(char *dst)
{
    uint8_t h;
    char base[9]; 
    char ext[5];
    char *p;
    uint8_t len = 0;
    uint8_t i;

    // ¿Existe tal cual?
    h = esx_fopen_read(dst);
    if (h == 0xFF) return; // No existe, perfecto.
    esx_fclose(h); // Existe, hay conflicto.

    // Descomponer nombre ya sanitizado (sabemos que es 8.3)
    p = dst;
    len = 0;
    while (*p && *p != '.' && len < 8) {
        base[len++] = *p++;
    }
    base[len] = 0;
    
    ext[0] = 0;
    if (*p == '.') {
        safe_copy(ext, p, sizeof(ext));
}

    // Si la base es muy larga (7 u 8 chars), hay que cortarla para meter el ~1
    // Necesitamos 2 chars para "~1". Max base length = 6.
    if (strlen(base) > 6) {
        base[6] = 0;
    }

    // Probar sufijos ~1 a ~9
    for (i = 1; i <= 9; i++) {
        memcpy(dst, base, strlen(base) + 1);
        strcat(dst, "~");
        dst[strlen(dst) + 1] = 0; // Null terminator temporal
        dst[strlen(dst)] = '0' + i; // Poner número
        strcat(dst, ext);
        
        h = esx_fopen_read(dst);
        if (h == 0xFF) return; // Encontrado hueco libre
        esx_fclose(h);
    }
    // Si hay más de 9 colisiones, sobrescribirá el ~9 (caso extremo raro)
}

// ============================================================================
// CMD_GET
// ============================================================================

// Request file size via SIZE command
// Returns file size (0 if unavailable or failed)
static uint32_t download_request_size(const char *remote)
{
    uint32_t file_size = 0;
    uint16_t frames = 0;
    
    char *p = tx_buffer;
    p = str_append(p, "SIZE ");
    p = str_append(p, remote);
    
    if (!ftp_command(tx_buffer)) {
        return 0;
    }
    
    rx_pos = 0;
    
    // Timeout ~2 segundos (100 frames)
    while (frames < 100) {
        HALT();
        uart_drain_to_buffer();
        
        if (try_read_line()) {
            if (strncmp(rx_line, S_IPD0, 7) == 0) {
                char *ps = strstr(rx_line, "213 ");
                if (ps) {
                    ps += 4;
                    while (*ps >= '0' && *ps <= '9') {
                        file_size = file_size * 10 + (*ps - '0');
                        ps++;
                    }
                    break;
                }
                if (strstr(rx_line, "550") || strstr(rx_line, "ERROR")) break;
            }
            rx_pos = 0;
        }
        frames++;
    }
    
    rx_pos = 0;
    return file_size;
}

// Wait for transfer start confirmation (150/125 response or IPD data)
// Returns 1 on success, 0 on timeout/error
static uint8_t download_wait_transfer_start(uint16_t *ipd_remaining, uint8_t *in_data, uint8_t *user_cancel)
{
    uint16_t frames = 0;
    char ctrl_buf[64];
    uint8_t ctrl_pos = 0;
    int16_t c;
    
    *in_data = 0;
    *ipd_remaining = 0;
    
    while (frames < 400) {
        uart_drain_to_buffer();
        c = rb_pop();
        
        if (c == -1) {
            HALT();
            if (key_edit_down()) {
                *user_cancel = 1;
                return 0;
            }
            frames++;
            continue;
        }
        
        if (c == '\r') continue;
        
        if (c == '\n') {
            ctrl_buf[ctrl_pos] = 0;
            
            if (strncmp(ctrl_buf, S_IPD0, 7) == 0) {
                if (strstr(ctrl_buf, "550") || strstr(ctrl_buf, "553") || 
                    strstr(ctrl_buf, "ERROR") || strstr(ctrl_buf, "Fail")) {
                    
                    debug_enabled = 1;
                    current_attr = ATTR_ERROR;
                    main_puts(S_ERROR_TAG);
                    main_print("File not found");
                    return 0;
                }
                
                if (strstr(ctrl_buf, "150") || strstr(ctrl_buf, "125")) {
                    return 1;
                }
            }
            ctrl_pos = 0;
            continue;
        }
        
        if (c == ':' && ctrl_pos >= 7 && strncmp(ctrl_buf, S_IPD1, 7) == 0) {
            ctrl_buf[ctrl_pos] = 0;
            char *p = ctrl_buf + 7;
            *ipd_remaining = parse_decimal(&p);
            *in_data = 1;
            return 1;
        }
        
        if (ctrl_pos < sizeof(ctrl_buf) - 1) {
            ctrl_buf[ctrl_pos++] = (char)c;
        }
    }
    
    return 0; // Timeout
}

static uint8_t download_file_core(const char *remote, const char *local, uint8_t b_cur, uint8_t b_tot, uint32_t *out_bytes)
{
    uint32_t received = 0;
    uint32_t file_size = 0;
    uint32_t timeout = 0;
    uint32_t silence = 0;
    uint8_t handle = 0xFF; 
    uint8_t in_data = 0;
    uint16_t ipd_remaining = 0;
    uint32_t last_progress = 0;
    char hdr_buf[64];
    uint8_t hdr_pos = 0;
    char local_name[32];
    uint8_t user_cancel = 0;
    uint8_t download_success = 0;
    uint8_t transfer_started = 0;
    int16_t c; // <--- MOVIDO AQUÍ (C89 compatible)
    
    *out_bytes = 0;
    file_buf_pos = 0;  // Reset file buffer
    sanitize_filename_83(local, local_name);
    ensure_unique_filename(local_name);
    // Aseguramos modo normal y limpieza completa para la negociación
    drain_mode_normal();
    rx_reset_all();  // Reset completo antes de descarga
    progress_current_file[0] = '\0';  // Reset progress tracking para forzar redraw completo
    
    current_attr = ATTR_LOCAL;
    {
        char *p = tx_buffer;
        p = str_append(p, "Requesting: ");
        p = str_append(p, remote);
        if (b_tot > 1) {
            p = str_append(p, " (");
            p = u16_to_dec(p, b_cur);
            p = char_append(p, '/');
            p = u16_to_dec(p, b_tot);
            p = char_append(p, ')');
        }
    }
    main_print(tx_buffer);
    
    // Mostrar nombre en barra de progreso inmediatamente
    draw_progress_bar(local_name, 0, 0);
    
    // Get file size (may return 0 if SIZE not supported)
    file_size = download_request_size(remote);
    
    // PASV + DATA
    if (ftp_passive() == 0) { fail(S_PASV_FAIL); return 0; }
    if (!ftp_open_data()) { fail(S_DATA_FAIL); return 0; }
    
    // FILE OPEN
    handle = esx_fopen_write(local_name);
    if (handle == 0xFF) {
        fail("Cannot create local file");
        ftp_close_data();
        return 0;
    }
    
    // RETR
    {
        char *p = ftp_cmd_buffer;
        p = str_append(p, "RETR ");
        p = str_append(p, remote);
    }
    if (!ftp_command(ftp_cmd_buffer)) goto get_cleanup; 
    
    debug_enabled = 0;
    
    // Wait for transfer start confirmation
    if (!download_wait_transfer_start(&ipd_remaining, &in_data, &user_cancel)) {
        if (user_cancel) {
            goto get_cleanup;
        }
        // Transfer didn't start (error already printed or timeout)
        if (handle != 0xFF) esx_fclose(handle);
        esp_tcp_close(1);
        rb_flush();
        return 0;
    }
    
    transfer_started = 1;

    // DRAW BAR
    draw_progress_bar(local_name, 0, file_size);
    
    // --- ACTIVAMOS MODO RÁPIDO ---
    drain_mode_fast();
    
    // ========================================================================
    // BUCLE DE DESCARGA OPTIMIZADO + SEGURO
    // ========================================================================
    while (timeout < TIMEOUT_LONG) {
        
        uart_drain_to_buffer();
        
        c = rb_pop(); // Asignación simple, variable ya declarada
        
        // --- NO HAY DATOS ---
        if (c == -1) {
            // SEGURIDAD EXTRA: Permitir cancelar (EDIT) incluso durante pausas
            if (key_edit_down()) {
                user_cancel = 1;
                break;
            }

            silence++;
            if (silence > SILENCE_XLONG) {
                debug_enabled = 1;
                main_print("Timeout (No data)");
                break;
            }
            continue; 
        }
        
        // --- HAY DATOS ---
        silence = 0;
        timeout = 0; 
        
        // Check de cancelación rápido (cada 32 bytes)
        if (key_edit_down()) {
            user_cancel = 1;
            break;
        }
        
        if (in_data && ipd_remaining > 0) {
            file_buffer[file_buf_pos++] = (uint8_t)c;
            ipd_remaining--;
            
            if (file_buf_pos >= 512 || ipd_remaining == 0) {
                esx_fwrite(handle, file_buffer, file_buf_pos);
                received += file_buf_pos;
                file_buf_pos = 0;
            }
            
            if (received - last_progress >= 1024) {
                draw_progress_bar(local_name, received, file_size);
                last_progress = received;
            }
            
            if (ipd_remaining == 0) { in_data = 0; hdr_pos = 0; }
        } else {
            // MÁQUINA DE ESTADOS PARA CABECERAS
            if (c == '\r' || c == '\n') {
                hdr_buf[hdr_pos] = 0;
                
                if (strstr(hdr_buf, S_CLOSED1)) { download_success = 1; goto get_cleanup; }
                
                // CORREGIDO: 'hdr_buf' en lugar de 'hhdr_buf'
                if (hdr_pos > 7 && strncmp(hdr_buf, S_IPD1, 7) == 0) {
                    char *p = hdr_buf + 7;
                    ipd_remaining = parse_decimal(&p);
                    if (*p == ':') { in_data = 1; file_buf_pos = 0; }
                }
                hdr_pos = 0;
            } else if (c == ':' && hdr_pos > 7 && strncmp(hdr_buf, S_IPD1, 7) == 0) {
                hdr_buf[hdr_pos] = 0;
                char *p = hdr_buf + 7;
                ipd_remaining = parse_decimal(&p);
                in_data = 1; file_buf_pos = 0; hdr_pos = 0;
            } else if (hdr_pos < 63) {
                hdr_buf[hdr_pos++] = (char)c;
            }
        }
    }

get_cleanup:
    drain_mode_normal();
    if (!user_cancel && file_buf_pos > 0) {
        esx_fwrite(handle, file_buffer, file_buf_pos);
        received += file_buf_pos;
    }
    debug_enabled = 1;
    if (handle != 0xFF) esx_fclose(handle);
    ftp_close_data(); 
    
    if (user_cancel) {
        g_user_cancel = 1;
        uart_flush_rx();
        if (b_tot <= 1) {
            fail("Download cancelled by user");
        }
        return 0;
    } else if (download_success) {
        draw_progress_bar(local_name, received, file_size > 0 ? file_size : received);
        
        current_attr = ATTR_RESPONSE;
        char size_buf[12];
        format_size(received, size_buf);
        {
            char *p = tx_buffer;
            p = str_append(p, "OK: ");
            p = str_append(p, local_name);
            p = str_append(p, " (");
            p = str_append(p, size_buf);
            p = char_append(p, ')');
        }
        main_print(tx_buffer);
        
        *out_bytes = received; 
        return 1;
    }
    return 0;
}

// Convierte UTF-8 común a ASCII simple in-place (para visualización en Spectrum)
// Convierte: áéíóúñ ÁÉÍÓÚÑ y elimina caracteres raros
static void utf8_to_ascii_inplace(char *s)
{
    char *write = s;
    char *read = s;
    
    while (*read) {
        uint8_t c = (uint8_t)*read;
        
        if (c < 128) {
            // ASCII normal
            *write++ = *read++;
        } else if (c == 0xC3) {
            // Cabecera UTF-8 para Latin1 (á, ñ, etc.)
            read++; // Saltamos el prefijo 0xC3
            uint8_t c2 = (uint8_t)*read;
            
            // Mapeo manual de lo más común
            if (c2 >= 0xA0 && c2 <= 0xA5) *write++ = 'a'; // àáâãäå
            else if (c2 == 0xA7) *write++ = 'c'; // ç
            else if (c2 >= 0xA8 && c2 <= 0xAB) *write++ = 'e'; // èéêë
            else if (c2 >= 0xAC && c2 <= 0xAF) *write++ = 'i'; // ìíîï
            else if (c2 == 0xB1) *write++ = 'n'; // ñ
            else if (c2 >= 0xB2 && c2 <= 0xB6) *write++ = 'o'; // òóôõö
            else if (c2 >= 0xB9 && c2 <= 0xBC) *write++ = 'u'; // ùúûü
            else if (c2 >= 0x80 && c2 <= 0x85) *write++ = 'A'; // ÀÁ...
            else if (c2 == 0x91) *write++ = 'N'; // Ñ
            else *write++ = '?'; // Desconocido dentro de Latin1
            
            read++; // Consumimos el segundo byte
        } else {
            // Otros caracteres multibyte (emojis, etc) -> Reemplazar por _
            *write++ = '_';
            read++;
            // Nota: Una implementación perfecta saltaría todos los bytes de continuación (0x80-0xBF)
            // pero para nombres de archivo simples esto suele bastar.
        }
    }
    *write = 0; // Nuevo terminador nulo
}

// Version FINAL de list_parse_line: UTF-8 fix + Width fix + Total fix
static uint8_t list_parse_line(const char *line_buf, uint8_t line_pos, 
                                uint8_t type_mode, uint32_t min_size, const char *pattern,
                                char *type_out, uint8_t *is_dir, uint32_t *size, char *name_out)
{
    char *p = (char*)line_buf;
    uint8_t col;
    
    while (*p == ' ') p++;
    if (*p == 0) return 0;

    // Ignorar línea "total"
    if (*p == 't' || *p == 'T') {
        if ((p[1] == 'o' || p[1] == 'O') && (p[2] == 't' || p[2] == 'T')) return 0;
    }
    
    // 1. Tipo
    char type = *p;
    *type_out = type;
    *is_dir = (type == 'd' || type == 'l');
    
    while (*p && *p != ' ') p++; while (*p == ' ') p++; if (*p == 0) return 0;
    
    // 2,3,4. Saltar Links, User, Group
    for (col = 0; col < 3; col++) {
        while (*p && *p != ' ') p++; while (*p == ' ') p++; if (*p == 0) return 0;
    }
    
    // 5. Size
    *size = 0;
    while (*p >= '0' && *p <= '9') { *size = *size * 10 + (*p - '0'); p++; }
    
    while (*p && *p != ' ') p++; while (*p == ' ') p++; if (*p == 0) return 0;
    
    // 6,7,8. Saltar Fecha
    for (col = 0; col < 3; col++) {
        while (*p && *p != ' ') p++; while (*p == ' ') p++; if (*p == 0) return 0; 
    }
    
    // 9. NOMBRE (Captura segura - TODO lo que queda)
    // El buffer name_out debe ser de al menos 41 bytes
    
    // A. Copiamos todo lo que queda
    strncpy(name_out, p, 40);
    name_out[40] = 0;
    
    // B. Limpieza de espacios/saltos al final
    {
        char *end = name_out + strlen(name_out) - 1;
        while (end >= name_out && (*end == '\r' || *end == '\n' || *end == ' ')) {
            *end = 0;
            end--;
        }
    }
    
    // C. FIX UTF-8: Aplanar acentos (Carátula -> Caratula)
    utf8_to_ascii_inplace(name_out);

    // D. FIX ANCHO: Si el nombre sigue siendo muy largo (más de 38 chars), poner ".."
    // Esto asegura que la columna de nombre nunca empuje la tabla más allá de 64 chars
    // (1 tipo + 1 espacio + 9 size + 1 espacio + 38 nombre = 50 chars, margen de seguridad)
    if (strlen(name_out) > 38) {
        name_out[37] = '.';
        name_out[38] = '.';
        name_out[39] = 0;
    }

    // --- FILTROS ---
    if (type_mode == 1 && !*is_dir) return 0;
    if (type_mode == 2 && *is_dir) return 0;
    if (min_size > 0 && *size < min_size) return 0;
    // Nota: El filtro por patrón ahora busca sobre el nombre "aplanado" (sin acentos),
    // lo cual es mucho más fácil para el usuario del Spectrum.
    if (pattern[0] && !str_contains(name_out, pattern)) return 0;
    
    return 1;
}

// ============================================================================
// UNIFIED LIST/SEARCH COMMAND (Core optimizado para LS y SEARCH)
// ============================================================================

static void cmd_list_core(const char *a1, const char *a2, const char *a3)
{
    if (!ensure_logged_in()) return;
    g_user_cancel = 0;
    drain_mode_fast(); // Velocidad máxima
    
    uint32_t t = 0;
    uint32_t silence = 0;
    int16_t c;
    char line_buf[128];
    uint8_t line_pos = 0;
    uint8_t matches = 0;
    uint8_t page_lines = 0;
    uint8_t in_data = 0;
    uint16_t ipd_remaining = 0;
    char hdr_buf[24];
    uint8_t hdr_pos = 0;
    uint8_t header_printed = 0;
    uint8_t list_pause_risky = 0;
    
    // --- PARSEO DE ARGUMENTOS ---
    char pattern[32]; pattern[0] = 0;
    uint8_t type_mode = 0; // 0=All, 1=Dirs, 2=Files
    uint32_t min_size = 0;
    
    const char *args[3];
    args[0] = a1; args[1] = a2; args[2] = a3;
    
    uint8_t i;
    for (i = 0; i < 3; i++) {
        const char *arg = args[i];
        if (!arg || !*arg) continue;
        if (strcmp(arg, "-d") == 0 || strcmp(arg, "-D") == 0 || strcmp(arg, "dirs") == 0) type_mode = 1;
        else if (strcmp(arg, "-f") == 0 || strcmp(arg, "-F") == 0 || strcmp(arg, "files") == 0) type_mode = 2;
        else if (arg[0] == '>') min_size = parse_size_arg(arg);
        else strncpy(pattern, arg, 31);
    }
    
    current_attr = ATTR_LOCAL;
    {
        char *p = tx_buffer;
        
        // Mensaje específico según tipo de filtro
        if (pattern[0]) {
            p = str_append(p, "Searching");
        } else if (type_mode == 1) {
            p = str_append(p, "Retrieving directories");
        } else if (type_mode == 2) {
            p = str_append(p, "Retrieving files");
        } else {
            p = str_append(p, "Retrieving directory contents");
        }
        
        if (pattern[0]) { p = str_append(p, " '"); p = str_append(p, pattern); p = char_append(p, '\''); }
        if (min_size) { p = str_append(p, " >"); p = u32_to_dec(p, min_size); p = char_append(p, 'B'); }
        p = str_append(p, S_DOTS);
    }
    main_print(tx_buffer);

    if (!setup_list_transfer()) return;

    while (t < TIMEOUT_BUSY) {
        if ((t & 0x1FF) == 0) {
            if (key_edit_down()) {
                fail(S_CANCEL);
                goto list_done;
            }
        }
        
        uart_drain_to_buffer();
        c = rb_pop();
        
        if (c == -1) {
            silence++;
            if (silence > SILENCE_BUSY) break; // Timeout de silencio
            t++;
            continue;
        }
        silence = 0;
        t++;
        
        if (!in_data) {
            // Máquina de estados para cabeceras IPD
            if (c == '\r' || c == '\n') {
                hdr_buf[hdr_pos] = 0;
                
                // Check for connection closed
                if (strstr(hdr_buf, S_CLOSED1)) goto list_done;
                
                // Check for IPD header (data channel)
                if (hdr_pos > 7 && (strncmp(hdr_buf, S_IPD1, 7) == 0 || strncmp(hdr_buf, S_IPD0, 7) == 0)) {
                    char *p = hdr_buf + 7;
                    ipd_remaining = parse_decimal(&p);
                    if (*p == ':') in_data = 1;
                }
                
                // Check for "226 Transfer complete" - normal end
                if (strstr(hdr_buf, "226")) goto list_done;
                
                hdr_pos = 0;
            } else if (c == ':' && hdr_pos > 7 && (strncmp(hdr_buf, S_IPD1, 7) == 0 || strncmp(hdr_buf, S_IPD0, 7) == 0)) {
                // Immediate IPD detection on ':' character
                hdr_buf[hdr_pos] = 0;
                char *p = hdr_buf + 7;
                ipd_remaining = parse_decimal(&p);
                in_data = 1;
                hdr_pos = 0;
            } else if (hdr_pos < 23) {
                hdr_buf[hdr_pos++] = c;
            } else {
                // Buffer overflow - reset to avoid getting stuck
                hdr_pos = 0;
            }
        } else {
            // Procesamiento de datos de lista
            ipd_remaining--;
            if (c == '\n') {
                line_buf[line_pos] = 0;
                if (line_pos > 10) {
                    uint8_t is_dir;
                    uint32_t size;
                    char name[41];
                    char type;
                    
                    if (list_parse_line(line_buf, line_pos, type_mode, min_size, pattern, 
                                       &type, &is_dir, &size, name)) {
                        
                        if (!header_printed) {
                            current_attr = ATTR_RESPONSE;
                            main_print("T      Size Filename");
                            print_char_line(22, '-');  // Como el banner
                            header_printed = 1;
                            page_lines = 1;  // Solo 1 línea extra (antes eran 2)
                        }
                        
                        char size_str[16];
                        format_size(size, size_str);
                        current_attr = is_dir ? ATTR_USER : ATTR_LOCAL;
                        
                        {
                            char *q = tx_buffer;
                            uint8_t slen;
                            q = char_append(q, type); 
                            q = char_append(q, ' ');
                            slen = strlen(size_str);
                            while(slen < 9) { q=char_append(q,' '); slen++; }
                            q = str_append(q, size_str);
                            q = char_append(q, ' ');
                            q = str_append(q, name);
                        }
                        main_print(tx_buffer);
                        matches++;
                        page_lines++;
                        
                        // PAGINACIÓN
                        if (page_lines >= LINES_PER_PAGE) {
                            current_attr = ATTR_RESPONSE;
                            main_print("-- More? EDIT=stop --");
                            drain_mode_normal();
                            {
                                uint16_t idle_frames = 0;
                                while(1) {
                                    HALT();
                                    uart_drain_to_buffer();

                                    if (key_edit_down()) goto list_done;
                                    if (in_inkey() != 0) break;

                                    // No parsing here. Just time tracking.
                                    if (idle_frames < 65535) idle_frames++;
                                    if (idle_frames >= FRAMES_LIST_PAUSE_RISKY) list_pause_risky = 1;
                                }
                            }
                            drain_mode_fast();
                            page_lines = 0;
                        }
                    }
                }
                line_pos = 0;
            } else if (c >= 32 && c < 127 && line_pos < 127) {
                line_buf[line_pos++] = c;
            }
            if (ipd_remaining == 0) in_data = 0;
        }
    }

list_done:
    drain_mode_normal();
    ftp_close_data();
    
    // CRITICAL: Limpiar buffers para evitar problemas en listados consecutivos
    rx_pos = 0;
    rx_overflow = 0;
    
    current_attr = ATTR_RESPONSE;
    {
        char *p = tx_buffer;
        p = char_append(p, '(');
        p = u16_to_dec(p, matches);
        // Si hay patrón de búsqueda, decir "matches", si no, "items"
        p = str_append(p, pattern[0] ? " matches)" : " items)");
    }
    main_print(tx_buffer);

    // If the user kept the listing paused long enough to risk server idle timeout,
    // probe the control channel cheaply before returning to the prompt.
    if (list_pause_risky && connection_state >= STATE_FTP_CONNECTED) {
        if (!quick_noop_check(FRAMES_NOOP_QUICK_TIMEOUT)) {
            clear_ftp_state();
            fail("Disconnected (NOOP timeout)");
            draw_status_bar();
        }
    }
}

static void cmd_get(char *args)
{
    if (!ensure_logged_in()) return;
    g_user_cancel = 0;
    status_bar_overwritten = 0;

    // Parsear argumentos en un array local
    #define MAX_BATCH 10
    char *argv[MAX_BATCH];
    uint8_t argc = 0;
    
    char *p = args;
    while (*p && argc < MAX_BATCH) {
        p = skip_ws(p);
        if (!*p) break;
        
        if (*p == '"') {
            argv[argc++] = ++p;
            while (*p && *p != '"') p++;
            if (*p) *p++ = 0;
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ') p++;
            if (*p) *p++ = 0;
        }
    }
    
    if (argc == 0) {
        main_print("GET file1 [file2 ...]");
        return;
    }
    
    // Contadores para el resumen
    uint8_t total_success = 0;
    uint32_t total_bytes = 0;
    
    uint8_t i;
    for (i = 0; i < argc; i++) {
        uint32_t bytes_this_file = 0;
        
        // Llamada al core
        if (download_file_core(argv[i], argv[i], i + 1, argc, &bytes_this_file)) {
            total_success++;
            total_bytes += bytes_this_file;
        } else {
            // Verificamos cancelación manual
            if (g_user_cancel) {
                if (argc > 1) {
                    main_print("Batch cancelled by user");
                }
                break;
            }
        }
        
        // Pausa de seguridad entre ficheros con drenaje activo
        {
            uint8_t w;
            for (w = 0; w < 25; w++) {
                uart_drain_to_buffer();
                wait_frames(1);
            }
        }
    }
    
    // RESUMEN FINAL EN CYAN (ATTR_RESPONSE)
    current_attr = ATTR_RESPONSE;
    
    // Solo mostramos resumen si era batch (>1) o si hubo éxito
    if (argc > 1 || total_success > 0) {
        char bytes_buf[16];
        format_size(total_bytes, bytes_buf);
        
        char *p = tx_buffer;
        p = u16_to_dec(p, total_success);
        p = str_append(p, " files downloaded (Total ");
        p = str_append(p, bytes_buf);
        p = char_append(p, ')');
        
        main_print(tx_buffer);
    }
    
    // Reset progress tracking
    progress_current_file[0] = '\0';
    
    if (status_bar_overwritten) {
        invalidate_status_bar();
        draw_status_bar();
        status_bar_overwritten = 0;
    }
}


// ============================================================================
// HELPER: DESCONEXIÓN SILENCIOSA (Para !CONNECT y QUIT)
// ============================================================================
static void close_connection_sequence(void)
{
    uint16_t t;
    
    current_attr = ATTR_LOCAL;
    main_print("Closing connection.");
    
    // 1. Envío QUIT (Protocolo FTP)
    safe_copy(ftp_cmd_buffer, S_CMD_QUIT, sizeof(ftp_cmd_buffer));
esp_tcp_send(0, ftp_cmd_buffer, strlen(ftp_cmd_buffer));
    
    // Espera breve
    for (t = 0; t < 25; t++) { uart_drain_to_buffer(); wait_frames(1); }

    // 2. Forzamos cierre TCP
    uart_send_string(S_AT_CLOSE0);
    
    // Espera breve
    for (t = 0; t < 10; t++) { uart_drain_to_buffer(); wait_frames(1); }
    
    // 3. LIMPIEZA TOTAL
    rb_flush();
    rx_pos = 0;
    
    clear_ftp_state();
    
    current_attr = ATTR_RESPONSE;
    main_print("Disconnected");
    draw_status_bar();
}

static void cmd_quit(void)
{
    // 1. Confirmación de seguridad en ROJO
    current_attr = ATTR_ERROR;       // <--- Color Rojo Brillante
    main_print("Disconnect (Y/N)?"); // <--- Texto cambiado
    
    while(1) {
        if (ay_uart_ready()) ay_uart_read(); // Drenar basura
        
        uint8_t k = in_inkey();
        
        // N o EDIT (7) -> Cancelar
        if (k == 'n' || k == 'N' || k == 7) { 
            current_attr = ATTR_LOCAL;
            main_print("Aborted");
            return;
        }
        // Y o ENTER -> Confirmar y salir del bucle
        if (k == 'y' || k == 'Y' || k == 13) {
            break; 
        }
        HALT();  // Procesar interrupciones mientras esperamos
    }

    // 2. Ejecutar la secuencia de cierre
    close_connection_sequence();
}

// ============================================================================
// COMMAND PARSER
// ============================================================================

static void str_to_upper(char *s)
{
    while (*s) {
        if (*s >= 'a' && *s <= 'z') *s -= 32;
        s++;
    }
}


static char* skip_ws(char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

static char* read_token(char *p, char *out, unsigned out_max)
{
    unsigned i = 0;
    p = skip_ws(p);

    // Allow quoted tokens to preserve spaces in paths/filenames.
    // Examples:
    //   CD "Mis juegos favoritos"
    //   GET "La Isla Misteriosa.tap"
    if (*p == '"') {
        p++; // skip opening quote
        while (*p && *p != '"') {
            if (i + 1 < out_max) out[i++] = *p;
            p++;
        }
        if (*p == '"') p++; // skip closing quote
    } else {
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
            if (i + 1 < out_max) out[i++] = *p;
            p++;
        }
    }
    out[i] = '\0';
    return p;
}

static void draw_banner(void);

static void cmd_cls(void)
{
    // Limpiamos solo la zona principal
    clear_zone(MAIN_START, MAIN_LINES, ATTR_MAIN_BG);
    main_line = MAIN_START;
    main_col = 0;
    
    draw_banner();
    
    // AÑADE ESTO:
    invalidate_status_bar(); // Forzamos repintado de la barra
    draw_status_bar();
}


static void cmd_about(void)
{
    current_attr = ATTR_RESPONSE;  // Azul
    main_print("BitStream " APP_VERSION " - A FTP Client for ZX Spectrum");
    print_char_line(22, '-');  // Como el banner
    current_attr = ATTR_LOCAL;
    main_print("(C) 2026 M. Ignacio Monge Garcia");
    main_print("ESP8266/AY-UART - Z88DK");
    main_print("AY-UART driver by A. Nihirash");
}

static void cmd_status(void)
{
    current_attr = ATTR_RESPONSE;
    main_print("--- SYSTEM STATUS ---");
    
    // ---------------------------------------------------------
    // 1. VERIFICACIÓN ACTIVA (Solo si creemos estar conectados)
    // ---------------------------------------------------------
    if (connection_state >= STATE_FTP_CONNECTED) {
        // Nota operativa:
        // Con !DEBUG activo, imprimir cada línea entrante puede bloquear el drenaje del UART
        // y perder bytes (sobre todo en respuestas cortas como NOOP). Para que !STATUS sea
        // fiable, deshabilitamos temporalmente la salida de debug y drenamos en modo rápido.
        uint8_t saved_debug_enabled = debug_enabled;
        uint8_t saved_drain_limit   = uart_drain_limit;
        debug_enabled = 0;
        drain_mode_fast();

        main_puts("Verifying connection... ");
        uart_flush_rx();
        rb_flush();
        
        // Enviar NOOP para verificar que el servidor responde
        if (ftp_command("NOOP")) {
            uint16_t frames = 0;
            uint8_t got_response = 0;
            uint8_t cancelled = 0;
            uint8_t got_disconnect = 0;
            
            // Timeout reducido: ~3 segundos = 150 frames
            while (frames < 150) {
                HALT();
                
                // Cancelación con EDIT
                if (key_edit_down()) {
                    cancelled = 1;
                    break;
                }
                
                if (try_read_line()) {
                    // Respuesta numérica (200, etc)
                    if (rx_line[0] >= '1' && rx_line[0] <= '5') {
                        got_response = 1;
                        break;
                    }
                    // Respuesta dentro de +IPD
                    if (strncmp(rx_line, S_IPD0, 7) == 0) {
                        char *ptr = strchr(rx_line, ':');
                        if (ptr && ptr[1] >= '1' && ptr[1] <= '5') {
                            got_response = 1;
                            break;
                        }
                    }
                    // Detectar desconexión
                    if (check_disconnect_message()) {
                        clear_ftp_state();
                        got_disconnect = 1;
                        break;
                    }
                    rx_pos = 0;
                }
                frames++;
            }
            
            if (cancelled) {
                fail(S_CANCEL);
            } else if (got_response) {
                main_print("OK");
            } else if (got_disconnect) {
                fail("FAILED (disconnected)");
            } else {
                // Sin respuesta: NO asumimos desconexión. Con algunos firmwares AT,
                // el control puede estar vivo pero la respuesta NOOP perderse.
                fail("FAILED (timeout)");
            }
        } else {
            // Error al enviar: no forzamos reset de estado sin evidencia de CLOSED.
            fail("FAILED (send error)");
        }
        current_attr = ATTR_RESPONSE;

        // Restaurar flags/modo de drenaje
        debug_enabled = saved_debug_enabled;
        uart_drain_limit = saved_drain_limit;
    }
    
    // ---------------------------------------------------------
    // 2. IMPRESIÓN DE DATOS (Formato Solicitado)
    // ---------------------------------------------------------

    // A. STATE
    main_puts("State: ");
    if (connection_state == STATE_DISCONNECTED) main_print("Disconnected");
    else if (connection_state == STATE_WIFI_OK) main_print("WiFi OK (No FTP)");
    else if (connection_state == STATE_FTP_CONNECTED) main_print("FTP Connected (No Login)");
    else if (connection_state == STATE_LOGGED_IN) main_print("Logged In");
    else {
        fail("Unknown");
        current_attr = ATTR_RESPONSE;
    }

    // B. IP (Separada explícitamente)
    {
        char *p = tx_buffer;
        p = str_append(p, "IP:    ");
        
        // Si estamos desconectados o la IP es la default 0.0.0.0
        if (connection_state == STATE_DISCONNECTED || wifi_client_ip[0] == '0') {
             p = str_append(p, "not connected");
        } else {
             p = str_append(p, wifi_client_ip);
        }
        main_print(tx_buffer);
    }

    // C. HOST
    {
        char *p = tx_buffer;
        p = str_append(p, "Host:  ");
        p = str_append(p, ftp_host);
        main_print(tx_buffer);
    }
    
    // D. PATH
    {
        char *p = tx_buffer;
        p = str_append(p, "Path:  ");
        p = str_append(p, ftp_path);
        main_print(tx_buffer);
    }
    
    // E. DEBUG
    if (debug_mode) main_print("Debug: ON");
    else main_print("Debug: OFF");
}

static void cmd_help(void)
{
    current_attr = ATTR_RESPONSE;  // Azul
    main_print("FTP COMMANDS");
    print_char_line(22, '-');  // Como el banner
    current_attr = ATTR_LOCAL;
    main_print("  OPEN host[:port] - Connect");
    main_print("  USER name pwd - Login");
    main_print("  QUIT - Disconnect");
    main_print("  PWD  - Show dir");
    main_print("  CD path - Change dir");
    main_print("  LS [filter] - List (-d/-f)");
    main_print("  GET file - Download");
    main_print("Type !HELP for more commands");
}

static void cmd_help_special(void)
{
    current_attr = ATTR_RESPONSE;  // Azul
    main_print("SPECIAL COMMANDS");
    print_char_line(22, '-');  // Como el banner
    current_attr = ATTR_LOCAL;
    main_print("  !CONNECT host[:port][/path] user [pwd]");
    main_print("       Quick connect & login");
    main_print("  !SEARCH [pat] - Search");
    main_print("  !STATUS - WiFi & FTP info");
    main_print("  !CLS - Clear screen");
    main_print("  !DEBUG - Toggle debug");
    main_print("  !INIT - Reset ESP");
    main_print("  !ABOUT - Version");
    current_attr = ATTR_RESPONSE;
    main_print("TIP: EDIT cancels operations");
}

// Helper para parsear tamaños como ">100k", ">1m"
static uint32_t parse_size_arg(const char *s)
{
    uint32_t val = 0;
    // Saltamos el '>' si existe
    if (*s == '>') s++;
    
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    
    if (*s == 'k' || *s == 'K') val *= 1024UL;
    else if (*s == 'm' || *s == 'M') val *= 1048576UL;
    
    return val;
}


// Función auxiliar para identificar comandos que REQUIEREN estar logueado
static uint8_t is_restricted_cmd(const char *cmd)
{
    // Lista negra: Comandos estándar que piden login
    if (strcmp(cmd, "LS") == 0) return 1;
    if (strcmp(cmd, "PWD") == 0) return 1;
    if (strcmp(cmd, "CD") == 0) return 1;
    if (strcmp(cmd, "GET") == 0) return 1;
    if (strcmp(cmd, "!SEARCH") == 0) return 1;
    
    return 0;
}

static void parse_command(char *line)
{
    char cmd[16];
    char arg1[48];
    char arg2[32];
    char arg3[32]; 
    
    cmd[0] = 0; arg1[0] = 0; arg2[0] = 0; arg3[0] = 0;
    
    {
        char *p = line;
        p = read_token(p, cmd, sizeof(cmd));
        p = read_token(p, arg1, sizeof(arg1));
        p = read_token(p, arg2, sizeof(arg2));
        p = read_token(p, arg3, sizeof(arg3));
    }
    
    str_to_upper(cmd);
    
    // --- ZONA DE SEGURIDAD CENTRALIZADA ---
    
    // 1. Filtro de comandos que requieren LOGIN completo
    if (is_restricted_cmd(cmd)) {
        if (!ensure_logged_in()) return; 
    }

    // 2. Filtro especial para USER (Requiere conexión, pero no login)
    if (strcmp(cmd, "USER") == 0) {
        if (connection_state < STATE_FTP_CONNECTED) {
            fail(S_NO_CONN);
            return;
        }
    }
    
    // --- COMANDOS "BANG" (!) ---
    if (strcmp(cmd, "!CONNECT") == 0) {
        if (arg1[0] && arg2[0]) {
            char *host = NULL;
            char *init_path = NULL;
            uint16_t port = parse_host_port_path(arg1, &host, &init_path);
            
            cmd_open(host, port);
            
            if (connection_state == STATE_FTP_CONNECTED) {
                wait_frames(10); 
                cmd_user(arg2, arg3[0] ? arg3 : "zx@zx.net");
                if (connection_state == STATE_LOGGED_IN && init_path && init_path[0]) {
                    current_attr = ATTR_LOCAL;
                    {
                        char *p = tx_buffer;
                        p = str_append(p, "Navigating to: ");
                        p = str_append(p, init_path);
                    }
                    main_print(tx_buffer);
                    uint8_t w;
                    for(w=0; w<25; w++) { uart_drain_to_buffer(); wait_frames(1); }
                    cmd_cd(init_path);
                }
                else if (connection_state == STATE_LOGGED_IN) {
                    cmd_pwd();
                }
            }
        } else {
            fail("Usage: !CONNECT host/path user [pass]");
        }
        return;
    }
    
    if (strcmp(cmd, "!SEARCH") == 0) { cmd_list_core(arg1, arg2, arg3); return; }
    if (strcmp(cmd, "!STATUS") == 0) { cmd_status(); return; }
    if (strcmp(cmd, "!ABOUT") == 0)  { cmd_about(); return; }
    if (strcmp(cmd, "!CLS") == 0)    { cmd_cls(); return; }
    if (strcmp(cmd, "!DEBUG") == 0) {
        debug_mode = !debug_mode;
        current_attr = ATTR_LOCAL;
        main_print(debug_mode ? "Debug mode ON" : "Debug mode OFF");
        return;
    }
    if (strcmp(cmd, "!INIT") == 0) {
        current_attr = ATTR_LOCAL;
        main_print("Re-initializing.");
        connection_state = STATE_DISCONNECTED;
        safe_copy(ftp_host, S_EMPTY, sizeof(ftp_host));
        safe_copy(ftp_user, S_EMPTY, sizeof(ftp_user));
        safe_copy(ftp_path, S_EMPTY, sizeof(ftp_path));
        full_initialization_sequence();
        return;
    }
    if (strcmp(cmd, "!HELP") == 0) { cmd_help_special(); return; }

    // --- COMANDOS ESTÁNDAR (CORREGIDOS) ---
    
    // AHORA: Chequeamos el comando PRIMERO, y los argumentos DENTRO.
    
    if (strcmp(cmd, "OPEN") == 0) {
        if (arg1[0]) {
            cmd_open(arg1, 21);
        } else {
            fail("Usage: OPEN host[:port]");
        }
    }
    else if (strcmp(cmd, "USER") == 0) {
        if (arg1[0]) {
            cmd_user(arg1, arg2[0] ? arg2 : "zx@zx.net");            
        } else {
            fail("Usage: USER name [password]");
        }
    }
    else if (strcmp(cmd, "CD") == 0) {
        if (arg1[0]) {
            cmd_cd(arg1);
        } else {
            fail("Usage: CD path");
        }
    }
    else if (strcmp(cmd, "PWD") == 0) {
        cmd_pwd();
    }
    else if (strcmp(cmd, "LS") == 0) {
        cmd_list_core(arg1, NULL, NULL); 
    }
    else if (strcmp(cmd, "GET") == 0) {
        // Lógica especial de GET para preservar el resto de la línea
        char *args_ptr = line;
        while (*args_ptr && *args_ptr != ' ') args_ptr++;
        args_ptr = skip_ws(args_ptr);
        
        if (*args_ptr) {
            cmd_get(args_ptr);
        } else {
            fail("Usage: GET file1 [file2 ...]");
        }
    }
    else if (strcmp(cmd, "QUIT") == 0) {
        cmd_quit();
    }
    else if (strcmp(cmd, "HELP") == 0 || strcmp(cmd, "?") == 0) {
        cmd_help();
    }
    else {
        fail("Unknown command. Type HELP");
    }
}

// ============================================================================
// SCREEN INITIALIZATION
// ============================================================================

static void draw_banner(void)
{
    clear_line(BANNER_START, ATTR_BANNER);
    // Concatenación automática: "BitStream " + "v1.1" + " - ".
    print_str64(BANNER_START, 0, "BitStream " APP_VERSION " - A FTP client for ZX Spectrum", ATTR_BANNER);
}

static void init_screen(void)
{
    uint8_t i;
    zx_border(INK_BLACK);
    for (i = 0; i < 24; i++) clear_line(i, PAPER_BLACK);
    
    // Limpiamos zona banner
    clear_line(BANNER_START, ATTR_BANNER);
    draw_banner(); 

    // Limpiamos la línea 1 extra
    clear_line(1, ATTR_MAIN_BG);
    
    clear_zone(MAIN_START, MAIN_LINES, ATTR_MAIN_BG);
    
    // Limpieza hueco separador
    clear_line(20, ATTR_MAIN_BG); 
    
    clear_line(STATUS_LINE, ATTR_STATUS);
    clear_zone(INPUT_START, INPUT_LINES, ATTR_INPUT_BG);
    
    main_line = MAIN_START;
    main_col = 0;
    
    invalidate_status_bar(); // Marca para redibujar
    
    draw_status_bar_real(); 
}

// ============================================================================
// MAIN
// ============================================================================

// ============================================================================
// BACKGROUND MONITORING
// ============================================================================

static void check_connection_alive(void)
{
    // Solo detectamos desconexiones si hay una conexión TCP activa
    // (estados FTP_CONNECTED o LOGGED_IN)
    if (connection_state < STATE_FTP_CONNECTED) {
        if (ay_uart_ready()) ay_uart_read();
        return;
    }

    // Guardamos el límite actual
    uint8_t prev_limit = uart_drain_limit;
    
    // Subimos a 16 bytes para capturar mensajes completos como "0,CLOSED\r\n"
    uart_drain_limit = 16;

    if (try_read_line()) {
        uint8_t disc = check_disconnect_message();
        
        if (disc) {
            const char *reason;
            if (disc == 1) {
                reason = "Remote host closed socket";
            } else {
                // disc == 2: 421 message
                if (str_contains(rx_line, "imeout")) reason = "Idle Timeout (421)";
                else reason = "Service Closing (421)";
            }
            
            current_attr = ATTR_ERROR;
            main_newline();
            
            {
                char *p = tx_buffer;
                p = str_append(p, "Disconnected: ");
                p = str_append(p, reason);
            }
            main_print(tx_buffer);
            
            // Limpiar estado
            clear_ftp_state();
            
            // Asegurar cierre físico
            {
                char *p = tx_buffer;
                p = str_append(p, "AT+CIPCLOSE=0\r\n");
                uart_send_string(tx_buffer);
            }
            
            draw_status_bar();
            main_newline();
            redraw_input_from(0);
        }
        rx_pos = 0;
    }
    
    // Restauramos el límite
    uart_drain_limit = prev_limit;
}

static void print_intro_banner(void)
{
    // Texto blanco brillante sobre fondo negro
    current_attr = PAPER_BLACK | INK_WHITE | BRIGHT;
    
    main_print("BitStream " APP_VERSION " - FTP Client");
    main_print("(C) M. Ignacio Monge Garcia 2025");
    print_char_line(32, '-');
}

void main(void)
{
    uint8_t c;
    uint8_t background_timer = 0;

    init_screen();
    
    // 1. Banner inicial
    print_intro_banner();
    
    smart_init();
    
    // 2. Mensaje de ayuda
    current_attr = ATTR_LOCAL; 
    main_print("Type HELP or !HELP. EDIT cancels.");
    main_newline();
    
    redraw_input_from(0);
    
    while (1) {
        HALT(); // Sincronización 50Hz
        
        // 1. Monitor de conexión SIEMPRE (crítico para detección de timeout)
        check_connection_alive();
        
        // 2. Detectar toggle de CAPS LOCK para actualizar cursor inmediatamente
        {
            static uint8_t prev_caps_mode = 0;
            static uint8_t prev_shift_state = 0; // Para detectar cambios en Shift
            
            check_caps_toggle(); // Gestión del toggle físico
            
            // Leemos el estado actual de Shift
            uint8_t curr_shift_state = key_shift_held();
            
            // Si cambia el bloqueo O si cambia el estado de la tecla Shift...
            // ...redibujamos el cursor inmediatamente.
            if (prev_caps_mode != caps_lock_mode || prev_shift_state != curr_shift_state) {
                
                prev_caps_mode = caps_lock_mode;
                prev_shift_state = curr_shift_state;
                
                // Forzar redibujado del cursor
                uint16_t char_abs = cursor_pos + 2;
                uint8_t cur_row = INPUT_START + (char_abs / SCREEN_COLS);
                uint8_t cur_col = char_abs % SCREEN_COLS;
                draw_cursor_underline(cur_row, cur_col);
            }
        }

        
        // 3. INPUT con latencia mínima
        c = read_key();
        
        // 4. UI updates
        ui_flush_dirty();
        
        // Si no hay tecla, vuelta rápida
        if (c == 0) continue;
        
        // --- PROCESAMIENTO DE TECLAS ---
        
        if (c == KEY_UP) {
            history_nav_and_redraw(1); // 1 = Arriba
        }
        else if (c == KEY_DOWN) {
            history_nav_and_redraw(-1); // -1 = Abajo
        }
        else if (c == KEY_LEFT) {
            input_left();
        }
        else if (c == KEY_RIGHT) {
            input_right();
        }
        else if (c == KEY_BACKSPACE) {
            input_backspace();
        }
        else if (c == KEY_ENTER) {
            if (line_len > 0) {
                char cmd_copy[LINE_BUFFER_SIZE];
                memcpy(cmd_copy, line_buffer, line_len + 1);
                
                history_add(cmd_copy, line_len);
                
                current_attr = ATTR_USER;
                main_puts("> ");
                main_puts(cmd_copy);
                main_newline();
                
                input_clear();
                
                // Ejecutamos comando
                set_input_busy(1);
                
                // IMPORTANTE: Antes de ejecutar cualquier comando,
                // verificamos si había mensajes de desconexión pendientes
                check_connection_alive(); 
                
                parse_command(cmd_copy);
                
                draw_status_bar();
                set_input_busy(0);
            }
        }
        else if (c >= 32 && c <= 126) {
            input_add_char(c);
        }
    }
}