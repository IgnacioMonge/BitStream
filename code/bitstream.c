// ============================================================================
// BitStream v1.0 - FTP Client for ZX Spectrum
// Uses AY-UART bit-banging at 9600 baud via ESP8266/ESP-12
// Full 64-column UI based on espATZX
// ============================================================================

#include <string.h>
#include <stdint.h>
#include <input.h>
#include <arch/zx.h>

// ============================================================================
// FONT 64 COLUMNS DATA
// ============================================================================

#include "font64_data.h"

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================
static void main_print(const char *s);
static void main_newline(void);
static char* skip_ws(char *p);
static void invalidate_status_bar(void);


// ============================================================================
// EXTERNAL AY-UART DRIVER
// ============================================================================

extern void     ay_uart_init(void);
extern void     ay_uart_send(uint8_t byte) __z88dk_fastcall;
extern void     ay_uart_send_block(void *buf, uint16_t len) __z88dk_callee;
extern uint8_t  ay_uart_read(void);
extern uint8_t  ay_uart_ready(void);

// ============================================================================
// SCREEN CONFIGURATION
// ============================================================================

#define SCREEN_COLS     64
#define SCREEN_PHYS     32

#define BANNER_START    0
#define MAIN_START      2
#define MAIN_LINES      17
#define MAIN_END        (MAIN_START + MAIN_LINES - 1)
#define STATUS_LINE     20
#define INPUT_START     21
#define INPUT_LINES     3
#define INPUT_END       23

// Pagination
#define LINES_PER_PAGE  14

// Colors / Attributes
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

// Macro para esperar un frame (ahorra código)
#define HALT() do { __asm__("ei"); __asm__("halt"); } while(0)

// Forward declaration
static void print_line64_fast(uint8_t y, const char *s, uint8_t attr);

// ============================================================================
// RING BUFFER
// ============================================================================

#define RING_BUFFER_SIZE 256
static uint8_t ring_buffer[RING_BUFFER_SIZE];
static uint8_t rb_head = 0;
static uint8_t rb_tail = 0;

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
    return ((uint8_t)(rb_head + 1) == rb_tail);
}

static void uart_drain_to_buffer(void)
{
    uint8_t max_loop = uart_drain_limit; 
    
    while (ay_uart_ready() && max_loop > 0) {
        if (rb_full()) break;
        ring_buffer[rb_head++] = ay_uart_read();
        max_loop--;
    }
}

// ESTA FUNCIÓN TAMBIÉN FALTABA
static int16_t rb_pop(void)
{
    if (rb_head == rb_tail) return -1;
    return ring_buffer[rb_tail++];
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

#define LINE_BUFFER_SIZE 80
#define TX_BUFFER_SIZE   128
#define PATH_SIZE        48

static char line_buffer[LINE_BUFFER_SIZE];
static uint8_t line_len = 0;
static uint8_t cursor_pos = 0;
static char tx_buffer[TX_BUFFER_SIZE];

static char ftp_cmd_buffer[128];

// ============================================================================
// COMMON STRINGS (save code space)
// ============================================================================

static const char S_IPD0[] = "+IPD,0,";
static const char S_IPD1[] = "+IPD,1,";
static const char S_CLOSED1[] = "1,CLOSED";
static const char S_PASV_FAIL[] = "PASV failed";
static const char S_DATA_FAIL[] = "Data connect failed";
static const char S_LIST_FAIL[] = "LIST send failed";

// Repeated UI strings (saves ~50 bytes)
static const char S_EMPTY[] = "---";
static const char S_NO_CONN[] = "No connection. Use OPEN.";
static const char S_LOGIN_BAD[] = "Login incorrect";

// ============================================================================
// FTP STATE
// ============================================================================

#define STATE_DISCONNECTED  0
#define STATE_WIFI_OK       1
#define STATE_FTP_CONNECTED 2
#define STATE_LOGGED_IN     3

static char ftp_host[32] = "---";
static char ftp_user[20] = "---";
static char ftp_path[PATH_SIZE] = "---";
static char data_ip[16];
static uint16_t data_port = 0;
static uint8_t connection_state = STATE_DISCONNECTED;

// Helper para limpiar estado FTP (evita duplicación)
static void clear_ftp_state(void)
{
    strcpy(ftp_host, S_EMPTY);
    strcpy(ftp_user, S_EMPTY);
    strcpy(ftp_path, S_EMPTY);
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
    strcpy(line_buffer, history[idx]);
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
        strcpy(line_buffer, history[idx]);
        line_len = strlen(line_buffer);
    }
    cursor_pos = line_len;
}

// ============================================================================
// VIDEO MEMORY FUNCTIONS
// ============================================================================

static uint8_t* screen_line_addr(uint8_t y, uint8_t phys_x, uint8_t scanline)
{
    uint16_t addr = 0x4000;
    addr |= ((uint16_t)(y & 0x18) << 8);
    addr |= ((uint16_t)scanline << 8);
    addr |= ((y & 0x07) << 5);
    addr |= phys_x;
    return (uint8_t*)addr;
}

static uint8_t* attr_addr(uint8_t y, uint8_t phys_x)
{
    return (uint8_t*)(0x5800 + (uint16_t)y * 32 + phys_x);
}

static void print_char64(uint8_t y, uint8_t col, uint8_t c, uint8_t attr)
{
    uint8_t phys_x = col >> 1;
    uint8_t half = col & 1;
    uint8_t *font_ptr;
    uint8_t *screen_ptr;
    uint8_t font_byte;
    
    uint8_t ch = c;
    if (ch < 32 || ch > 127) ch = 32;
    font_ptr = (uint8_t*)&font64[((uint16_t)(ch - 32)) << 3];
    
    // Calculate base screen address once
    // addr = 0x4000 + ((y & 0x18) << 8) + ((y & 7) << 5) + phys_x
    screen_ptr = (uint8_t*)(0x4000 + ((uint16_t)(y & 0x18) << 8) + ((y & 0x07) << 5) + phys_x);
    
    if (half == 0) {
        // Left half - use high nibble of font
        *screen_ptr = (*screen_ptr & 0x0F);  // Scanline 0: clear
        screen_ptr += 256;
        *screen_ptr = (*screen_ptr & 0x0F) | (font_ptr[0] & 0xF0);
        screen_ptr += 256;
        *screen_ptr = (*screen_ptr & 0x0F) | (font_ptr[1] & 0xF0);
        screen_ptr += 256;
        *screen_ptr = (*screen_ptr & 0x0F) | (font_ptr[2] & 0xF0);
        screen_ptr += 256;
        *screen_ptr = (*screen_ptr & 0x0F) | (font_ptr[3] & 0xF0);
        screen_ptr += 256;
        *screen_ptr = (*screen_ptr & 0x0F) | (font_ptr[4] & 0xF0);
        screen_ptr += 256;
        *screen_ptr = (*screen_ptr & 0x0F) | (font_ptr[5] & 0xF0);
        screen_ptr += 256;
        *screen_ptr = (*screen_ptr & 0x0F) | (font_ptr[6] & 0xF0);
    } else {
        // Right half - use low nibble of font
        *screen_ptr = (*screen_ptr & 0xF0);  // Scanline 0: clear
        screen_ptr += 256;
        *screen_ptr = (*screen_ptr & 0xF0) | (font_ptr[0] & 0x0F);
        screen_ptr += 256;
        *screen_ptr = (*screen_ptr & 0xF0) | (font_ptr[1] & 0x0F);
        screen_ptr += 256;
        *screen_ptr = (*screen_ptr & 0xF0) | (font_ptr[2] & 0x0F);
        screen_ptr += 256;
        *screen_ptr = (*screen_ptr & 0xF0) | (font_ptr[3] & 0x0F);
        screen_ptr += 256;
        *screen_ptr = (*screen_ptr & 0xF0) | (font_ptr[4] & 0x0F);
        screen_ptr += 256;
        *screen_ptr = (*screen_ptr & 0xF0) | (font_ptr[5] & 0x0F);
        screen_ptr += 256;
        *screen_ptr = (*screen_ptr & 0xF0) | (font_ptr[6] & 0x0F);
    }
    
    // Write attribute
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


static void clear_line(uint8_t y, uint8_t attr)
{
    uint8_t i;
    for (i = 0; i < 8; i++) memset(screen_line_addr(y, 0, i), 0, 32);
    memset(attr_addr(y, 0), attr, 32);
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

static void copy_screen_line(uint8_t dst_y, uint8_t src_y)
{
    uint8_t i;
    for (i = 0; i < 8; i++) {
        memcpy(screen_line_addr(dst_y, 0, i), screen_line_addr(src_y, 0, i), 32);
    }
    memcpy(attr_addr(dst_y, 0), attr_addr(src_y, 0), 32);
}

static void scroll_main_zone(void)
{
    uint8_t y;
    for (y = MAIN_START; y < MAIN_END; y++) copy_screen_line(y, y + 1);
    clear_line(MAIN_END, current_attr);
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
    uint8_t *ptr;
    ptr = screen_line_addr(y, phys_x, 0); *ptr = 0x00;
    ptr = screen_line_addr(y, phys_x, 1); *ptr = 0x3C;
    ptr = screen_line_addr(y, phys_x, 2); *ptr = 0x7E;
    ptr = screen_line_addr(y, phys_x, 3); *ptr = 0x7E;
    ptr = screen_line_addr(y, phys_x, 4); *ptr = 0x7E;
    ptr = screen_line_addr(y, phys_x, 5); *ptr = 0x7E;
    ptr = screen_line_addr(y, phys_x, 6); *ptr = 0x3C;
    ptr = screen_line_addr(y, phys_x, 7); *ptr = 0x00;
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
        uint32_t whole = bytes / 1048576UL;
        uint32_t rem   = bytes % 1048576UL;
        uint8_t  frac  = (uint8_t)((rem * 10UL) / 1048576UL); // 1 decimal
        p = u32_to_dec(p, whole);
        *p++ = '.';
        *p++ = (char)('0' + frac);
        *p++ = 'M';
        *p++ = 'B';
        *p   = 0;
    } else if (bytes >= 1024UL) {
        uint32_t whole = bytes / 1024UL;
        uint32_t rem   = bytes % 1024UL;
        uint8_t  frac  = (uint8_t)((rem * 10UL) / 1024UL); // 1 decimal
        p = u32_to_dec(p, whole);
        *p++ = '.';
        *p++ = (char)('0' + frac);
        *p++ = 'K';
        *p++ = 'B';
        *p   = 0;
    } else {
        p = u32_to_dec(p, bytes);
        *p++ = 'B';
        *p   = 0;
    }
}

// ============================================================================
// MODIFIED DRAW_PROGRESS_BAR (NO FLICKER)
// ============================================================================

// --- COLORES CORREGIDOS (Texto Negro) ---
#define ATTR_DL_TEXT    (PAPER_WHITE | INK_BLACK)  // <--- CAMBIO: Antes era INK_BLUE
#define ATTR_DL_BAR_ON  (PAPER_WHITE | INK_RED)    // Rojo sobre blanco
#define ATTR_DL_BAR_OFF (PAPER_WHITE | INK_BLACK)  // Negro sobre blanco

// Track current download to detect file changes
static char progress_current_file[9] = "";

static void draw_progress_bar(const char *filename, uint32_t received, uint32_t total)
{
    char size_buf[24]; 
    char total_buf[12];
    uint8_t i;
    char name_short[9];
    
    // Prepare short name (max 8 chars)
    if (strlen(filename) > 8) {
        memcpy(name_short, filename, 8);
        name_short[8] = 0;
    } else {
        strcpy(name_short, filename);
    }
    
    // Detect file change or start - force full redraw
    uint8_t force_redraw = 0;
    if (received == 0 || strcmp(progress_current_file, name_short) != 0) {
        force_redraw = 1;
        strcpy(progress_current_file, name_short);
        clear_line(STATUS_LINE, ATTR_DL_TEXT);
    }
    
    // Format "received/total" string
    format_size(received, size_buf);
    strcat(size_buf, "/");
    format_size(total, total_buf);
    strcat(size_buf, total_buf);
    
    uint8_t col = 0;

    // "Downloading: " label (13 chars)
    print_str64(STATUS_LINE, col, "Downloading: ", ATTR_DL_TEXT);
    col += 13;
    
    // Filename (8 chars padded)
    if (force_redraw) {
        print_padded(STATUS_LINE, col, name_short, ATTR_DL_TEXT, 8);
    }
    col += 8;
    
    print_char64(STATUS_LINE, col++, ' ', ATTR_DL_TEXT);
    
    // Size "received/total" (15 chars padded for "999MB/999MB")
    print_padded(STATUS_LINE, col, size_buf, ATTR_DL_TEXT, 15);
    col += 15;
    
    // Progress bar - 24 chars total (22 inner + 2 brackets)
    #define BAR_WIDTH 22
    
    print_char64(STATUS_LINE, col++, '[', ATTR_DL_TEXT);
    
    uint8_t filled = 0;
    if (total > 0) {
        filled = (uint8_t)((received * BAR_WIDTH) / total);
        if (filled > BAR_WIDTH) filled = BAR_WIDTH;
    }
    
    for (i = 0; i < BAR_WIDTH; i++) {
        if (i < filled) {
            print_char64(STATUS_LINE, col++, '#', ATTR_DL_BAR_ON);
        } else {
            print_char64(STATUS_LINE, col++, '.', ATTR_DL_BAR_OFF);
        }
    }
    
    print_char64(STATUS_LINE, col++, ']', ATTR_DL_TEXT);

    // Spinner at position 63 (col 62 stays empty)
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

static void draw_status_bar(void)
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
            strcpy(buf_short, ftp_host);
        }
        print_padded(STATUS_LINE, P_HOST, buf_short, ATTR_VAL, W_HOST);
        strcpy(last_host, ftp_host);
    }
    
    // USER
    if (strcmp(ftp_user, last_user) != 0) {
        if (strlen(ftp_user) > W_USER) {
            memcpy(buf_short, ftp_user, W_USER - 1);
            buf_short[W_USER - 1] = '~';
            buf_short[W_USER] = 0;
        } else {
            strcpy(buf_short, ftp_user);
        }
        print_padded(STATUS_LINE, P_USER, buf_short, ATTR_VAL, W_USER);
        strcpy(last_user, ftp_user);
    }
    
    // PATH (PWD)
    if (strcmp(ftp_path, last_path) != 0) {
        uint8_t len = strlen(ftp_path);
        if (len > W_PATH) {
            // Recorte inteligente por la izquierda
            buf_short[0] = '~';
            strcpy(buf_short + 1, ftp_path + len - (W_PATH - 1));
        } else {
            strcpy(buf_short, ftp_path);
        }
        print_padded(STATUS_LINE, P_PATH, buf_short, ATTR_VAL, W_PATH);
        strcpy(last_path, ftp_path);
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
    main_puts(s);
    main_newline();
}


// ============================================================================
// INPUT ZONE
// ============================================================================

static void draw_cursor_underline(uint8_t y, uint8_t col)
{
    uint8_t phys_x = col >> 1;
    uint8_t half = col & 1;
    uint8_t *screen_ptr;
    
    screen_ptr = screen_line_addr(y, phys_x, 7);
    
    if (half == 0) {
        *screen_ptr |= 0xF0;
    } else {
        *screen_ptr |= 0x0F;
    }
    *attr_addr(y, phys_x) = ATTR_INPUT;
}

static void redraw_input_from(uint8_t start_pos)
{
    uint8_t row, col, i;
    uint16_t abs_pos;

    if (start_pos == 0) {
        print_char64(INPUT_START, 0, '>', ATTR_PROMPT);
    }

    for (i = start_pos; i < line_len; i++) {
        abs_pos = i + 2;
        row = INPUT_START + (abs_pos / SCREEN_COLS);
        col = abs_pos % SCREEN_COLS;
        if (row > INPUT_END) break;
        print_char64(row, col, line_buffer[i], ATTR_INPUT);
    }

    uint16_t cur_abs = cursor_pos + 2;
    uint8_t cur_row = INPUT_START + (cur_abs / SCREEN_COLS);
    uint8_t cur_col = cur_abs % SCREEN_COLS;

    if (cur_row <= INPUT_END) {
        char c_under = (cursor_pos < line_len) ? line_buffer[cursor_pos] : ' ';
        print_char64(cur_row, cur_col, c_under, ATTR_INPUT);
        draw_cursor_underline(cur_row, cur_col);
    }

    uint16_t end_abs = line_len + 2;
    row = INPUT_START + (end_abs / SCREEN_COLS);
    col = end_abs % SCREEN_COLS;
    
    uint8_t clear_count = 0;
    while (row <= INPUT_END && clear_count < 8) {
        if (!(row == cur_row && col == cur_col)) {
            print_char64(row, col, ' ', ATTR_INPUT_BG);
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
    
    clear_zone(INPUT_START, INPUT_LINES, ATTR_INPUT_BG);
    
    print_char64(INPUT_START, 0, '>', ATTR_PROMPT);
    print_char64(INPUT_START, 2, ' ', ATTR_INPUT);
    draw_cursor_underline(INPUT_START, 2);
}

static void refresh_cursor_char(uint8_t idx, uint8_t show_cursor)
{
    uint16_t abs_pos = idx + 2;
    uint8_t row = INPUT_START + (abs_pos / SCREEN_COLS);
    uint8_t col = abs_pos % SCREEN_COLS;
    
    if (row > INPUT_END) return;

    char c = (idx < line_len) ? line_buffer[idx] : ' ';
    print_char64(row, col, c, ATTR_INPUT);

    if (show_cursor) {
        draw_cursor_underline(row, col);
    }
}

static void input_add_char(uint8_t c)
{
    // Lógica de "Bang Command": Si empieza por '!', forzamos mayúsculas
    // hasta encontrar el primer espacio.
    if (c >= 'a' && c <= 'z') {
        if (line_len == 0 && c == '!') {
            // No hacemos nada, es el primer caracter
        } else if (line_len > 0 && line_buffer[0] == '!') {
            // Verificamos si ya hay un espacio en el buffer
            uint8_t has_space = 0;
            uint8_t i;
            for (i = 0; i < line_len; i++) {
                if (line_buffer[i] == ' ') {
                    has_space = 1;
                    break;
                }
            }
            // Si no hay espacio, estamos escribiendo el comando -> MAYÚSCULAS
            if (!has_space) {
                c -= 32;
            }
        }
    }

    if (c >= 32 && c < 127 && line_len < LINE_BUFFER_SIZE - 1) {
        if (cursor_pos < line_len) {
            memmove(&line_buffer[cursor_pos + 1], &line_buffer[cursor_pos], line_len - cursor_pos);
            line_buffer[cursor_pos] = c;
            line_len++;
            cursor_pos++;
            line_buffer[line_len] = 0;
            redraw_input_from(cursor_pos - 1);
        } else {
            line_buffer[cursor_pos] = c;
            line_len++;
            cursor_pos++;
            line_buffer[line_len] = 0;
            
            uint16_t char_abs = (cursor_pos - 1) + 2;
            uint8_t row = INPUT_START + (char_abs / SCREEN_COLS);
            uint8_t col = char_abs % SCREEN_COLS;
            print_char64(row, col, c, ATTR_INPUT);
            
            uint16_t cur_abs = cursor_pos + 2;
            uint8_t cur_row = INPUT_START + (cur_abs / SCREEN_COLS);
            uint8_t cur_col = cur_abs % SCREEN_COLS;
            
            if (cur_row <= INPUT_END) {
                print_char64(cur_row, cur_col, ' ', ATTR_INPUT);
                draw_cursor_underline(cur_row, cur_col);
            }
        }
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
                print_char64(old_row, old_col, ' ', ATTR_INPUT_BG);
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
            print_char64(row, col, c_under, ATTR_INPUT);
        }
    } else {
        redraw_input_from(cursor_pos);
    }
}

// ============================================================================
// KEYBOARD HANDLING (from espATZX)
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

    if (k == '0' && debounce_zero > 0) {
        debounce_zero--;
        return 0;
    }

    // New key - return immediately
    if (k != last_k) {
        last_k = k;
        
        if (k == KEY_BACKSPACE) {
            repeat_timer = 12;
        } else if (k == KEY_LEFT || k == KEY_RIGHT) {
            repeat_timer = 15;
        } else {
            repeat_timer = 20;
        }
        
        if (k == KEY_BACKSPACE) debounce_zero = 8;
        else debounce_zero = 0;

        return k;
    }

    // Holding key - auto-repeat
    if (k == KEY_BACKSPACE) debounce_zero = 8;

    if (repeat_timer > 0) {
        repeat_timer--;
        return 0;
    } else {
        if (k == KEY_BACKSPACE) {
            repeat_timer = 1;
            return k;
        }
        if (k == KEY_LEFT || k == KEY_RIGHT) {
            repeat_timer = 2;
            return k;
        }
        if (k == KEY_UP || k == KEY_DOWN) {
            repeat_timer = 5;
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
    ay_uart_send('\r');
    ay_uart_send('\n');
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
        if (in_inkey() == 7) {
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

// Check if server has disconnected us (call during idle waits)
// Returns 1 if disconnected, 0 if still connected
static uint8_t poll_for_disconnect(void)
{
    if (connection_state < STATE_FTP_CONNECTED) return 0;
    
    // Drain UART briefly
    uart_drain_to_buffer();
    
    // Check for disconnect messages without blocking
    if (try_read_line()) {
        // Server timeout (421)
        if (strncmp(rx_line, "421", 3) == 0) {
            clear_ftp_state();
            current_attr = ATTR_ERROR;
            main_print("Server timeout (421)");
            return 1;
        }
        // Socket closed
        if (strncmp(rx_line, "0,CLOSED", 8) == 0) {
            clear_ftp_state();
            current_attr = ATTR_ERROR;
            main_print("Server closed connection");
            return 1;
        }
        rx_pos = 0;
    }
    return 0;
}


// ============================================================================
// ESP INITIALIZATION
// ============================================================================

// Read single byte with timeout (like NetManZX Uart.readTimeout)
// Returns -1 on timeout, 0-255 on success
// timeout of 0x8000 = ~1.6 seconds at 3.5MHz
static int16_t read_byte_timeout(void)
{
    uint16_t timeout = 0x4000;  // Shorter timeout (~0.8s)
    uint8_t delay;
    
    while (timeout > 0) {
        if (ay_uart_ready()) {
            return ay_uart_read();
        }
        // Small delay to not saturate (like NetManZX)
        for (delay = 4; delay > 0; delay--) {
            __asm__("nop");
        }
        timeout--;
    }
    return -1;
}

// Flush all pending input
static void flush_input(void)
{
    while (ay_uart_ready()) {
        ay_uart_read();
    }
}

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
    
    flush_input();
    uart_send_string("AT+CIFSR\r\n");
    
    // Timeout ~4 segundos (200 frames)
    while (frames < 200 && !found_ip) {
        HALT();
        
        // Cancelación con EDIT
        if (in_inkey() == 7) {
            flush_input();
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
                first_digit = c;
                digit_count = 1;
                dot_count = 0;
                
                // Leer resto de posible IP del buffer
                while ((c = rb_pop()) != -1) {
                    if (c >= '0' && c <= '9') {
                        digit_count++;
                    } else if (c == '.') {
                        dot_count++;
                        digit_count = 0;
                    } else {
                        break;
                    }
                }
                
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
    flush_input();
    return found_ip ? 1 : 0;
}

static uint8_t check_has_ip(void)
{
    int16_t c;
    
    flush_input();
    uart_send_string("AT+CIFSR\r\n");
    
    // Search for STAIP," pattern
    while (1) {
        c = read_byte_timeout();
        if (c < 0) break;  // Timeout
        
        if (c == 'S') {
            c = read_byte_timeout(); if (c < 0 || c != 'T') continue;
            c = read_byte_timeout(); if (c < 0 || c != 'A') continue;
            c = read_byte_timeout(); if (c < 0 || c != 'I') continue;
            c = read_byte_timeout(); if (c < 0 || c != 'P') continue;
            c = read_byte_timeout(); if (c < 0 || c != ',') continue;
            c = read_byte_timeout(); if (c < 0 || c != '"') continue;
            // Found STAIP," - read first char of IP
            c = read_byte_timeout();
            if (c > 0 && c != '0') {
                // Valid IP (doesn't start with 0)
                flush_input();
                return 1;
            }
            // IP is 0.0.0.0 - no connection
            break;
        }
        
        if (c == 'O') {
            c = read_byte_timeout();
            if (c == 'K') break;  // End of response
        }
    }
    
    flush_input();
    return 0;
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
    uart_send_string("AT+CIPMUX=1\r\n");
    for (i=0; i<5; i++) HALT();
    while (ay_uart_ready()) ay_uart_read();
}

// 2. Inicialización completa (Fallback cuando algo va mal)
// Esta es la versión robusta que discutimos antes
static void full_initialization_sequence(void)
{
    uint8_t i;
    
    current_attr = ATTR_LOCAL;
    main_puts("Full initialization...");
    main_newline();

    // Reset lógico de la comunicación (ATE0, flushes...)
    // Nota: No enviamos +++ ni RST para no romper más cosas
    setup_ftp_mode();

    main_puts("Probing ESP...");
    
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
    main_puts("Checking connection...");
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
        main_puts("Cancelled");
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
    main_puts("Initializing...");

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
    uart_send_string("AT+CIPMUX=1\r\n");
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
    main_puts("Checking connection...");
    
    uart_flush_rx();
    uart_send_string("AT+CWJAP?\r\n");
    
    // Timeout ~4 segundos (200 frames)
    if (wait_for_string("+CWJAP:", 200)) {
        // Fix color: espacio en verde, OK en azul
        main_puts(" ");
        current_attr = ATTR_RESPONSE;
        main_puts("OK");
        main_newline();
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

// Asks user to confirm disconnect if already connected.
// Returns 1 if OK to proceed (was disconnected or user confirmed)
// Returns 0 if user cancelled
static uint8_t confirm_disconnect(void)
{
    if (connection_state < STATE_FTP_CONNECTED) {
        return 1;  // Not connected, OK to proceed
    }
    
    current_attr = ATTR_ERROR;
    main_print("Already connected. Disconnect? (Y/N)");
    
    while(1) {
        // Drain UART to avoid buildup
        if (ay_uart_ready()) ay_uart_read();
        
        uint8_t k = in_inkey();
        if (k == 'n' || k == 'N' || k == 7) {  // 7 = EDIT key
            current_attr = ATTR_LOCAL;
            main_print("Cancelled");
            return 0;
        }
        if (k == 'y' || k == 'Y' || k == 13) break;  // 13 = ENTER
    }
    
    // User confirmed - disconnect cleanly
    current_attr = ATTR_LOCAL;
    main_print("Disconnecting...");
    
    // Send QUIT to server (polite disconnect)
    strcpy(ftp_cmd_buffer, "QUIT\r\n");
    esp_tcp_send(0, ftp_cmd_buffer, 6);
    wait_frames(15);
    
    // Close socket
    esp_tcp_close(0);
    rb_flush();
    rx_pos = 0;
    
    // Reset state
    clear_ftp_state();
    
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
    ay_uart_send('\r');
    ay_uart_send('\n');
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
    
    {
        char *p = tx_buffer;
        p = str_append(p, "AT+CIPSEND=");
        p = u16_to_dec(p, (uint16_t)sock);
        p = char_append(p, ',');
        p = u16_to_dec(p, len);
    }
    esp_send_at(tx_buffer);
    
    // Wait for '>' prompt - timeout ~3 segundos (150 frames)
    frames = 0;
    while (frames < 150) {
        HALT();
        
        // Cancelación con EDIT
        if (in_inkey() == 7) {
            return 0;
        }
        
        uart_drain_to_buffer();
        while ((c = rb_pop()) != -1) {
            if (c == '>') goto send_data;
        }
        frames++;
    }
    return 0;  // Timeout waiting for prompt
    
send_data:
    // Small delay after >
    for (i = 0; i < 100; i++) { __asm__("nop"); }
    
    // Send actual data
    for (i = 0; i < len; i++) {
        ay_uart_send(data[i]);
    }
    
    // Brief wait for SEND OK
    wait_frames(5);
    
    return 1;
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
        current_attr = ATTR_ERROR;
        main_print("Buffer overflow!");
        return 0;
    }
    
    strcpy(ftp_cmd_buffer, cmd);
    strcat(ftp_cmd_buffer, "\r\n");
    
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
        
        if (in_inkey() == 7) {
            main_print("Cancelled");
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
    if (ftp_passive() == 0) {
        current_attr = ATTR_ERROR;
        main_print(S_PASV_FAIL);
        return 0;
    }
    
    if (!ftp_open_data()) {
        current_attr = ATTR_ERROR;
        main_print(S_DATA_FAIL);
        return 0;
    }
    
    if (!ftp_command("LIST")) {
        ftp_close_data();
        current_attr = ATTR_ERROR;
        main_print(S_LIST_FAIL);
        return 0;
    }
    
    // Small delay to let server start sending data
    wait_frames(10);
    
    return 1;
}

// ============================================================================
// ESXDOS FILE OPERATIONS
// ============================================================================

// Sanitize filename for esxDOS - 8.3 format (8 chars name + 3 chars extension)
static void sanitize_filename(const char *src, char *dst, uint8_t max_len)
{
    const char *p;
    const char *last_dot = 0;
    uint8_t i = 0;
    uint8_t name_len = 0;
    uint8_t ext_len = 0;
    
    (void)max_len;  // We use fixed 8.3 format
    
    // Find last / or \ to get basename
    p = src;
    while (*p) {
        if (*p == '/' || *p == '\\') {
            src = p + 1;
        }
        p++;
    }
    
    // Find last dot for extension
    p = src;
    while (*p) {
        if (*p == '.') {
            last_dot = p;
        }
        p++;
    }
    
    // Copy name part (max 8 chars)
    p = src;
    while (*p && p != last_dot && name_len < 8) {
        char c = *p++;
        // Convert to uppercase, allow only alphanumeric and underscore
        if (c >= 'a' && c <= 'z') c -= 32;
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') {
            dst[i++] = c;
            name_len++;
        }
    }
    
    // Add extension if present
    if (last_dot && *(last_dot + 1)) {
        dst[i++] = '.';
        p = last_dot + 1;
        while (*p && ext_len < 3) {
            char c = *p++;
            if (c >= 'a' && c <= 'z') c -= 32;
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
                dst[i++] = c;
                ext_len++;
            }
        }
    }
    
    // Ensure we have a valid name
    if (i == 0 || (i == 1 && dst[0] == '.')) {
        dst[0] = 'D';
        dst[1] = 'O';
        dst[2] = 'W';
        dst[3] = 'N';
        dst[4] = '.';
        dst[5] = 'B';
        dst[6] = 'I';
        dst[7] = 'N';
        i = 8;
    }
    dst[i] = 0;
}

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

// Open file for reading (returns handle or 0xFF on error)
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
        
        pop ix              ; IX = filename
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

// Read from file (returns bytes read, 0 on error/EOF)
static uint16_t esx_fread(uint8_t handle, void *buf, uint16_t len)
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
        defb 0x9D           ; ESX_FREAD
        jr c, esx_read_fail
        ld h, b
        ld l, c
        jr esx_read_done
    esx_read_fail:
        ld hl, 0
    esx_read_done:
    __endasm;
}


// ============================================================================
// COMMAND HANDLERS
// ============================================================================

static void cmd_pwd(void); 
static void cmd_ls(const char *filter);
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
            if (strncmp(rx_line, "0,CLOSED", 8) == 0 ||
                strncmp(rx_line, "421", 3) == 0) {
                clear_ftp_state();
                draw_status_bar();
                current_attr = ATTR_ERROR;
                main_print("Connection lost");
                return 0;
            }
            rx_pos = 0;
        }
    }
    
    // Si ya estamos logueados, todo perfecto
    if (connection_state == STATE_LOGGED_IN) return 1;

    // Si no, mostramos el error adecuado
    current_attr = ATTR_ERROR;
    
    if (connection_state == STATE_DISCONNECTED || connection_state == STATE_WIFI_OK) {
        main_print("Not connected. Use OPEN.");
    } else if (connection_state == STATE_FTP_CONNECTED) {
        main_print("Not logged in. Use USER.");
    }
    
    return 0; // Indica que NO se puede continuar
}

static void cmd_open(const char *host, uint16_t port)
{
    // Si ya hay una conexión activa, pedir confirmación
    if (!confirm_disconnect()) {
        return;  // Usuario canceló
    }
    
    uint16_t frames;
    
    current_attr = ATTR_LOCAL;
    {
        char *p = tx_buffer;
        p = str_append(p, "Connecting to ");
        p = str_append(p, host);
        p = char_append(p, ':');
        p = u16_to_dec(p, port);
        p = str_append(p, "...");
    }
    main_print(tx_buffer);
    
    debug_enabled = 0;
    
    if (!esp_tcp_connect(0, host, port)) {
        debug_enabled = 1;
        // Asegurar que el socket quede limpio aunque haya fallado
        esp_tcp_close(0);
        wait_frames(5);
        rb_flush();
        current_attr = ATTR_ERROR;
        main_print("Connect failed");
        return;
    }
    
    current_attr = ATTR_LOCAL;
    main_print("Waiting for banner...");
    
    rx_pos = 0;
    
    // Timeout ~10 segundos (500 frames)
    for (frames = 0; frames < 500; frames++) {
        HALT();
        
        if (in_inkey() == 7) {
            debug_enabled = 1;
            esp_tcp_close(0);
            current_attr = ATTR_ERROR;
            main_print("Cancelled");
            return;
        }
        
        uart_drain_to_buffer();

        if (try_read_line()) {
            if (strstr(rx_line, "220")) {
                debug_enabled = 1;
                strncpy(ftp_host, host, sizeof(ftp_host) - 1);
                ftp_host[sizeof(ftp_host) - 1] = '\0';
                strcpy(ftp_user, S_EMPTY);  // Mostrar "---"
                strcpy(ftp_path, S_EMPTY);  // Mostrar "---"
                connection_state = STATE_FTP_CONNECTED;
                current_attr = ATTR_RESPONSE;
                main_print("Connected!");
                draw_status_bar();
                return;
            }
            rx_pos = 0;
        }
    }
    
    debug_enabled = 1;
    current_attr = ATTR_ERROR;
    main_print("No FTP banner (timeout)");
    esp_tcp_close(0);
}

static void cmd_user(const char *user, const char *pass)
{
    // Verificación de estado
    if (connection_state < STATE_FTP_CONNECTED) {
        current_attr = ATTR_ERROR;
        main_print(S_NO_CONN);
        return;
    }

    // --- LIMPIEZA DE SEGURIDAD MEJORADA (FLUSH AGRESIVO) ---
    // Drenamos el buffer y esperamos silencio real en la línea
    // Repetimos hasta que no llegue nada durante 2 frames seguidos
    {
        uint8_t silence_checks = 0;
        while(silence_checks < 2) {
            if (ay_uart_ready()) {
                ay_uart_read(); // Descartar byte
                silence_checks = 0; // Reiniciar contador de silencio
            } else {
                wait_frames(1); // Esperar 20ms (1 frame)
                silence_checks++;
            }
        }
        rb_flush(); // Limpiar también el buffer circular de software
    }
    // --------------------------------------------------------

    uint16_t frames;
    char *p;
    uint16_t code = 0;
    
    current_attr = ATTR_LOCAL;
    {
        char *p = tx_buffer;
        p = str_append(p, "Login as ");
        p = str_append(p, user);
        p = str_append(p, "...");
    }
    main_print(tx_buffer);
    
    // 1. Enviar USER
    {
        char *p = ftp_cmd_buffer;
        p = str_append(p, "USER ");
        p = str_append(p, user);
        p = str_append(p, "\r\n");
    }
    if (!esp_tcp_send(0, ftp_cmd_buffer, strlen(ftp_cmd_buffer))) {
        current_attr = ATTR_ERROR;
        main_print("Send USER failed");
        return;
    }
    
    // 2. Esperar respuesta (331 o 230) - timeout basado en frames
    frames = 0;
    rx_pos = 0;
    
    // ~8 segundos = 400 frames
    while (frames < 400) {
        HALT();
        
        if (in_inkey() == 7) {
            current_attr = ATTR_ERROR;
            main_print("Cancelled");
            return;
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
                    if (code > 0) break;
                }
            }
            rx_pos = 0;
        }
        frames++;
    }
    
    // Gestión de respuestas USER
    if (code == 230) goto login_success; // Ya logueado (sin pass)
    
    if (code != 331) {
        current_attr = ATTR_ERROR;
        // Si obtenemos 530 aquí, es que el usuario no existe o server muy estricto
        if (code == 530) main_print(S_LOGIN_BAD); 
        else if (code > 0) {
            char *p = tx_buffer;
            p = str_append(p, "USER error: ");
            p = u16_to_dec(p, code);
            main_print(tx_buffer);
        } else {
            main_print("No response to USER");
        }
        return;
    }
    
    // 3. Enviar PASS
    {
        char *p = ftp_cmd_buffer;
        p = str_append(p, "PASS ");
        p = str_append(p, pass);
        p = str_append(p, "\r\n");
    }
    if (!esp_tcp_send(0, ftp_cmd_buffer, strlen(ftp_cmd_buffer))) {
        current_attr = ATTR_ERROR;
        main_print("Send PASS failed");
        return;
    }
    
    // 4. Esperar respuesta final (230) - timeout basado en frames
    frames = 0;
    code = 0;
    rx_pos = 0;
    
    while (frames < 400) {
        HALT();
        
        if (in_inkey() == 7) {
            current_attr = ATTR_ERROR;
            main_print("Cancelled");
            return;
        }
        
        uart_drain_to_buffer();

        if (try_read_line()) {
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
                    if (code > 0) break;
                }
            }
            rx_pos = 0;
        }
        frames++;
    }
    
    if (code != 230) {
        current_attr = ATTR_ERROR;
        if (code == 530) main_print(S_LOGIN_BAD);
        else {
            char *p = tx_buffer;
            p = str_append(p, "Login failed: ");
            p = u16_to_dec(p, code);
            main_print(tx_buffer);
        }
        return;
    }
    
login_success:
    strncpy(ftp_user, user, sizeof(ftp_user) - 1);
    ftp_user[sizeof(ftp_user) - 1] = '\0';
    strcpy(ftp_path, "/");  // Inicializar a raíz - cmd_pwd() lo actualizará
    connection_state = STATE_LOGGED_IN;
    current_attr = ATTR_RESPONSE;
    main_print("Logged in!");
    
    // Configurar modo binario
    strcpy(ftp_cmd_buffer, "TYPE I\r\n");
    esp_tcp_send(0, ftp_cmd_buffer, strlen(ftp_cmd_buffer));
    
    // Breve pausa para respuesta TYPE I
    wait_frames(5);
    rb_flush();
    
    // NO llamamos cmd_pwd() aquí - el caller decide cuándo actualizar el path
}

static void cmd_pwd(void)
{
    if (!ensure_logged_in()) return;    
    uint16_t frames = 0;
    
    if (!ftp_command("PWD")) return;
    
    rx_pos = 0;
    
    // Timeout ~4 segundos (200 frames)
    while (frames < 200) {
        HALT();
        
        if (in_inkey() == 7) {
            current_attr = ATTR_ERROR;
            main_print("Cancelled");
            return;
        }
        
        uart_drain_to_buffer();
        
        if (try_read_line()) {
            // Respuesta típica: 257 "/pub/incoming" is the current directory
            if (strncmp(rx_line, "+IPD,0,", 7) == 0) {
                // Buscamos las comillas que encierran la ruta
                char *start = strchr(rx_line, '"');
                if (start) {
                    start++; // Saltar la primera comilla
                    char *end = strchr(start, '"');
                    if (end) *end = 0; // Cortar en la segunda comilla
                    
                    // Guardar en variable global
                    strncpy(ftp_path, start, sizeof(ftp_path) - 1);
                    ftp_path[sizeof(ftp_path) - 1] = '\0';
                    
                    print_smart_path("PWD: ", ftp_path);
                    
                    draw_status_bar();
                    return;
                }
            }
            rx_pos = 0;
        }
        frames++;
    }
}

static void cmd_cd(const char *path)
{
    if (!ensure_logged_in()) return;
    uint16_t frames = 0;
    
    {
        char *p = tx_buffer;
        p = str_append(p, "CWD ");
        p = str_append(p, path);
    }
    if (!ftp_command(tx_buffer)) return;
    
    rx_pos = 0;
    
    // Timeout ~5 segundos (250 frames)
    while (frames < 250) {
        HALT();
        
        if (in_inkey() == 7) {
            current_attr = ATTR_ERROR;
            main_print("Cancelled");
            return;
        }

        if (try_read_line()) {
            if (strncmp(rx_line, S_IPD0, 7) == 0) {
                // Success: 250
                if (strstr(rx_line, "250")) {
                    current_attr = ATTR_RESPONSE;
                    
                    // Guardar el path que usamos (cmd_pwd() lo sobreescribirá si consigue el real)
                    if (path[0] == '/') {
                        // Path absoluto - usarlo directamente
                        strncpy(ftp_path, path, sizeof(ftp_path) - 1);
                        ftp_path[sizeof(ftp_path) - 1] = '\0';
                    } else if (strcmp(path, "..") == 0) {
                        // Subir un nivel - cortar último componente
                        char *last_slash = strrchr(ftp_path, '/');
                        if (last_slash && last_slash != ftp_path) {
                            *last_slash = '\0';
                        } else {
                            strcpy(ftp_path, "/");
                        }
                    } else {
                        // Path relativo - concatenar
                        size_t len = strlen(ftp_path);
                        if (len > 0 && ftp_path[len-1] != '/') {
                            strncat(ftp_path, "/", sizeof(ftp_path) - len - 1);
                        }
                        strncat(ftp_path, path, sizeof(ftp_path) - strlen(ftp_path) - 1);
                    }
                    
                    invalidate_status_bar();
                    cmd_pwd();  // Intentar obtener el path real (sobreescribe si tiene éxito)
                    return;
                }
                // Error: 550 (not found), 553, etc.
                if (strstr(rx_line, "550") || strstr(rx_line, "553") || 
                    strstr(rx_line, "501") || strstr(rx_line, "500")) {
                    current_attr = ATTR_ERROR;
                    main_print("Directory not found");
                    return;
                }
            }
            rx_pos = 0;
        }
        frames++;
    }
    current_attr = ATTR_ERROR;
    main_print("CD timeout");
}

// ============================================================================
// CMD_LS CON FILTRADO (LIMPIA)
// ============================================================================

#define ATTR_THEME_BLUE (PAPER_BLACK | INK_BLUE | BRIGHT)

static void cmd_ls(const char *filter) 
{
    if (!ensure_logged_in()) return;
    
    drain_mode_fast();  // Max throughput for listing
    
    uint32_t t = 0;
    uint32_t silence = 0;
    int16_t c;
    char line_buf[128];
    uint8_t line_pos = 0;
    uint8_t lines_shown = 0;
    uint8_t page_lines = 0;
    uint8_t in_data = 0;
    uint16_t ipd_remaining = 0;
    char hdr_buf[24];
    uint8_t hdr_pos = 0;
    uint8_t header_printed = 0;
    
    uint8_t filter_mode = 0; 
    
    // 1. FEEDBACK AZUL (Tu petición anterior)
    current_attr = ATTR_RESPONSE;
    if (filter && filter[0]) {
        if (strcmp(filter, "-d") == 0 || strcmp(filter, "dirs") == 0) {
            filter_mode = 1;
            main_print("Listing directories...");
        } else if (strcmp(filter, "-f") == 0 || strcmp(filter, "files") == 0) {
            filter_mode = 2;
            main_print("Listing files...");
        } else {
            main_print("Listing...");
        }
    } else {
        main_print("Listing...");
    }
    
    current_attr = ATTR_LOCAL; // Volvemos a verde técnico
    
    if (!setup_list_transfer()) return;
    
    while (t < TIMEOUT_LONG && lines_shown < 200) {
        
        // HALT periódico para permitir cancelación y evitar saturación
        if ((t & 0x1F) == 0) {
            HALT();
            if (in_inkey() == 7) {
                current_attr = ATTR_ERROR;
                main_print("Cancelled");
                goto done_list;
            }
        }
        
        uart_drain_to_buffer();
        
        c = rb_pop();
        if (c == -1) {
            HALT();
            // También verificar cancelación cuando no hay datos
            if (in_inkey() == 7) {
                current_attr = ATTR_ERROR;
                main_print("Cancelled");
                goto done_list;
            }
            silence++;
            if (silence > SILENCE_LONG) break; 
            t++;
            continue;
        }
        silence = 0;
        t++;  // Incrementar t también cuando hay datos
        
        if (!in_data) {
            if (c == '\r' || c == '\n') {
                hdr_buf[hdr_pos] = 0;
                
                if (strstr(hdr_buf, S_CLOSED1)) goto done_list;
                
                if (hdr_pos > 7 && strncmp(hdr_buf, S_IPD1, 7) == 0) {
                    char *p = hdr_buf + 7;
                    ipd_remaining = parse_decimal(&p);
                    if (*p == ':') in_data = 1;
                }
                hdr_pos = 0;
            } else if (c == ':' && hdr_pos > 7 && strncmp(hdr_buf, S_IPD1, 7) == 0) {
                hdr_buf[hdr_pos] = 0;
                char *p = hdr_buf + 7;
                ipd_remaining = parse_decimal(&p);
                in_data = 1;
                hdr_pos = 0;
            } else if (hdr_pos < 23) {
                hdr_buf[hdr_pos++] = c;
            }
        } else {
            ipd_remaining--;
            
            if (c == '\r') {
                // Ignore
            } else if (c == '\n') {
                line_buf[line_pos] = 0;
                
                if (line_pos > 10) {
                    // --- PARSER MEJORADO (Ignora espacios + detecta links) ---
                    
                    char *p_start = line_buf;
                    while (*p_start == ' ') p_start++; // Saltar espacios iniciales
                    
                    char type = *p_start; // Leemos el tipo real (d, l, -)
                    
                    // Consideramos directorio si es 'd' O 'l' (link)
                    // Esto hace que ftp.gnu.org se vea bien
                    uint8_t is_dir = (type == 'd' || type == 'l');
                    uint8_t show_this = 1;
                    
                    if (filter_mode == 1 && !is_dir) show_this = 0;
                    if (filter_mode == 2 && is_dir) show_this = 0;
                    
                    if (show_this) {
                        if (!header_printed) {
                            current_attr = ATTR_RESPONSE; 
                            main_print("T      Size Filename");
                            main_print("- --------- ---------------------");
                            header_printed = 1;
                            page_lines = 2;
                        }
                        
                        // Extracción de nombre (última palabra de la línea)
                        char *name = line_buf + line_pos;
                        while (name > line_buf && *(name-1) != ' ') name--;
                        
                        // Extracción de tamaño (buscar por la izquierda tras permisos/user/group)
                        // Buscamos el token numérico antes de la fecha (simplificado: primer número largo)
                        // Como fallback, usaremos el mismo método visual de antes
                        char *p = p_start; // Usar p_start para saltar los espacios iniciales
                        uint8_t spaces = 0;
                        while (*p && spaces < 4) {
                            if (*p == ' ') { spaces++; while (*p == ' ') p++; }
                            if (spaces < 4) p++;
                        }
                        
                        char raw_size[12];
                        uint8_t si = 0;
                        while (*p && *p != ' ' && si < 11) { raw_size[si++] = *p++; }
                        raw_size[si] = 0;
                        
                        uint32_t bytes = 0;
                        char *rp = raw_size;
                        while (*rp >= '0' && *rp <= '9') {
                            bytes = bytes * 10 + (*rp - '0');
                            rp++;
                        }
                        
                        char size_str[16];
                        format_size(bytes, size_str);
                        
                        if (strlen(name) > 40) name[40] = 0;
                        
                        // Si es DIR o LINK, lo pintamos de BLANCO (User)
                        if (is_dir) current_attr = ATTR_USER;
                        else current_attr = ATTR_LOCAL;

                        {
                            char *q = tx_buffer;
                            uint8_t slen;
                            q = char_append(q, type); // Imprime 'd' o 'l' o '-'
                            q = char_append(q, ' ');
                            
                            slen = (uint8_t)strlen(size_str);
                            while (slen < 9) { q = char_append(q, ' '); slen++; }
                            
                            q = str_append(q, size_str);
                            q = char_append(q, ' ');
                            q = str_append(q, name);
                        }
                        main_print(tx_buffer);
                        uart_drain_to_buffer();  // Drenar UART después de renderizar
                        lines_shown++;
                        page_lines++;
                        
                        if (page_lines >= LINES_PER_PAGE) {
                            uint8_t key;
                            uint16_t idle_count = 0;
                            current_attr = ATTR_RESPONSE;
                            main_print("-- More? EDIT=stop --");
                            
                            while(1) {
                                // Seguir drenando UART durante la espera
                                uart_drain_to_buffer();
                                
                                key = in_inkey();
                                if (key == 7) goto done_list; 
                                if (key != 0) break;
                                
                                // Check for server disconnect every ~2 seconds
                                idle_count++;
                                if (idle_count > 100) {
                                    idle_count = 0;
                                    if (poll_for_disconnect()) goto done_list;
                                }
                                HALT();
                            }
                            page_lines = 0;
                        }
                    } 
                }
                line_pos = 0;
            } else if (c >= 32 && c < 127 && line_pos < 127) {
                line_buf[line_pos++] = c;
            }
            
            if (ipd_remaining == 0) {
                in_data = 0;
            }
        }
    }
    
done_list:
    drain_mode_normal();  // Restore UI responsiveness
    ftp_close_data();
    
    current_attr = ATTR_RESPONSE;
    {
        char *p = tx_buffer;
        p = char_append(p, '(');
        p = u16_to_dec(p, lines_shown);
        p = str_append(p, " entries)");
    }
    main_print(tx_buffer);
}

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
// CMD_GET
// ============================================================================

static uint8_t download_file_core(const char *remote, const char *local, uint8_t b_cur, uint8_t b_tot, uint32_t *out_bytes)
{
    uint32_t received = 0;
    uint32_t file_size = 0;
    uint32_t timeout = 0;
    uint32_t silence = 0;
    uint8_t handle = 0xFF; 
    uint8_t in_data = 0;
    uint16_t ipd_remaining = 0;
    char chunk[64];
    uint8_t chunk_pos = 0;
    uint32_t last_progress = 0;
    char hdr_buf[24];
    uint8_t hdr_pos = 0;
    char local_name[32];
    uint8_t user_cancel = 0;
    uint8_t download_success = 0;
    uint8_t transfer_started = 0;
    
    *out_bytes = 0;
    sanitize_filename(local, local_name, 31);
    
    // Aseguramos modo normal y limpieza para la negociación (CRÍTICO para fix 0 bytes)
    drain_mode_normal();
    uart_flush_rx(); 
    
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
    
    // SIZE
    {
        char *p = tx_buffer;
        p = str_append(p, "SIZE ");
        p = str_append(p, remote);
    }
    if (ftp_command(tx_buffer)) {
        uint16_t frames = 0;
        rx_pos = 0;
        // Timeout ~2 segundos (100 frames)
        while (frames < 100) {
            HALT();
            uart_drain_to_buffer();
            
            if (try_read_line()) {
                if (strncmp(rx_line, "+IPD,0,", 7) == 0) {
                    char *p = strstr(rx_line, "213 ");
                    if (p) {
                        p += 4;
                        while (*p >= '0' && *p <= '9') {
                            file_size = file_size * 10 + (*p - '0');
                            p++;
                        }
                        break;
                    }
                    if (strstr(rx_line, "550") || strstr(rx_line, "ERROR")) break; 
                }
                rx_pos = 0;
            }
            frames++;
        }
    }
    rx_pos = 0;
    
    // PASV + DATA
    if (ftp_passive() == 0) { current_attr = ATTR_ERROR; main_print("PASV failed"); return 0; }
    if (!ftp_open_data()) { current_attr = ATTR_ERROR; main_print("Data connect failed"); return 0; }
    
    // FILE OPEN - Solo abrimos si la conexión de datos está lista
    handle = esx_fopen_write(local_name);
    if (handle == 0xFF) {
        current_attr = ATTR_ERROR;
        main_print("Cannot create local file");
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
    
    // WAIT START - Read byte by byte to avoid losing data
    // Only process control channel (+IPD,0,), pass data channel to download loop
    {
        uint16_t frames = 0;
        char ctrl_buf[64];
        uint8_t ctrl_pos = 0;
        
        // Timeout ~8 segundos (400 frames)
        while (frames < 400) {
            uart_drain_to_buffer();
            int16_t c = rb_pop();
            
            if (c == -1) {
                HALT();
                // Verificar cancelación cuando no hay datos
                if (in_inkey() == 7) { user_cancel = 1; goto get_cleanup; }
                frames++;
                continue;
            }
            
            // Accumulate into control buffer
            if (c == '\r') continue;
            
            if (c == '\n') {
                ctrl_buf[ctrl_pos] = 0;
                
                // Only process control channel responses (+IPD,0,)
                if (strncmp(ctrl_buf, "+IPD,0,", 7) == 0) {
                    if (strstr(ctrl_buf, "550") || strstr(ctrl_buf, "553") || 
                        strstr(ctrl_buf, "ERROR") || strstr(ctrl_buf, "Fail")) {
                        debug_enabled = 1;
                        current_attr = ATTR_ERROR;
                        main_print("Error: File not found");
                        if (handle != 0xFF) esx_fclose(handle);
                        ftp_close_data();
                        return 0; 
                    }
                    if (strstr(ctrl_buf, "150") || strstr(ctrl_buf, "125")) { 
                        transfer_started = 1; 
                        break; 
                    }
                }
                ctrl_pos = 0;
                continue;
            }
            
            // Check if this looks like data channel header starting
            if (ctrl_pos == 7 && strncmp(ctrl_buf, "+IPD,1,", 7) == 0) {
                // Data is arriving! Push back what we have and let download loop handle it
                // We can't push back, but we can initialize the download loop state
                // Continue accumulating until we see ':'
            }
            
            if (c == ':' && ctrl_pos >= 7 && strncmp(ctrl_buf, "+IPD,1,", 7) == 0) {
                // Data channel header complete - extract length and start download
                ctrl_buf[ctrl_pos] = 0;
                char *p = ctrl_buf + 7;
                ipd_remaining = parse_decimal(&p);
                in_data = 1;
                transfer_started = 1;
                break;  // Exit wait loop, download loop will receive the data bytes
            }
            
            if (ctrl_pos < sizeof(ctrl_buf) - 1) {
                ctrl_buf[ctrl_pos++] = c;
            }
        }
    }
    
    if (!transfer_started) {
        debug_enabled = 1;
        current_attr = ATTR_ERROR;
        main_print("No response from server");
        goto get_cleanup;
    }

    // DRAW BAR
    draw_progress_bar(local_name, 0, file_size);
    
    // --- ACTIVAMOS MODO RÁPIDO AHORA, JUSTO PARA LOS DATOS ---
    drain_mode_fast();
    
    // DOWNLOAD LOOP
    while (timeout < TIMEOUT_LONG) {
        uart_drain_to_buffer();
        
        int16_t c = rb_pop();
        if (c == -1) {
            silence++;
            // HALT periódico cuando no hay datos (cada 8 iteraciones)
            if ((silence & 0x07) == 0) {
                HALT();
                // Verificar cancelación en cada HALT
                if (in_inkey() == 7) { 
                    user_cancel = 1; 
                    break; 
                }
            }
            if (silence > SILENCE_XLONG) {
                debug_enabled = 1;
                main_print("Timeout (No data)");
                break;
            }
            continue;
        }
        silence = 0;
        
        if (in_data && ipd_remaining > 0) {
            chunk[chunk_pos++] = (uint8_t)c;
            ipd_remaining--;
            
            if (chunk_pos >= 60 || ipd_remaining == 0) {
                esx_fwrite(handle, chunk, chunk_pos);
                received += chunk_pos;
                chunk_pos = 0;
            }
            
            if (received - last_progress >= 256) {
                draw_progress_bar(local_name, received, file_size);
                last_progress = received;
            }
            
            if (ipd_remaining == 0) { in_data = 0; hdr_pos = 0; }
        } else {
            if (c == '\r' || c == '\n') {
                hdr_buf[hdr_pos] = 0;
                if (strstr(hdr_buf, "1,CLOSED")) { download_success = 1; goto get_cleanup; }
                if (hdr_pos > 7 && strncmp(hdr_buf, "+IPD,1,", 7) == 0) {
                    char *p = hdr_buf + 7;
                    ipd_remaining = parse_decimal(&p);
                    if (*p == ':') { in_data = 1; chunk_pos = 0; }
                }
                hdr_pos = 0;
            } else if (c == ':' && hdr_pos > 7 && strncmp(hdr_buf, "+IPD,1,", 7) == 0) {
                hdr_buf[hdr_pos] = 0;
                char *p = hdr_buf + 7;
                ipd_remaining = parse_decimal(&p);
                in_data = 1; chunk_pos = 0; hdr_pos = 0;
            } else if (hdr_pos < 23) {
                hdr_buf[hdr_pos++] = c;
            }
        }
    }

get_cleanup:
    drain_mode_normal(); // RESTAURAMOS MODO NORMAL
    if (!user_cancel && chunk_pos > 0) {
        esx_fwrite(handle, chunk, chunk_pos);
        received += chunk_pos;
    }
    debug_enabled = 1;
    if (handle != 0xFF) esx_fclose(handle);
    ftp_close_data(); 
    
    // Eliminado: invalidate_status_bar() y draw_status_bar() para mantener la UI limpia
    
    if (user_cancel) {
        uart_flush_rx(); 
        current_attr = ATTR_ERROR;
        main_print("Aborted by User");
        return 0; 
    } else if (download_success) {
        // Final progress update to show 100%
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
        
        *out_bytes = received; // Reportar bytes
        return 1; // Éxito
    }
    return 0; // Fallo
}

static void cmd_get(char *args)
{
    if (!ensure_logged_in()) return;
    
    // Parsear argumentos en un array local
    #define MAX_BATCH 10
    char *argv[MAX_BATCH];
    uint8_t argc = 0;
    
    char *p = args;
    while (*p && argc < MAX_BATCH) {
        p = skip_ws(p);
        if (*p == 0) break;
        
        argv[argc++] = p; // Guardar inicio del token
        while (*p && *p != ' ') p++;
        if (*p == ' ') { *p = 0; p++; }
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
        
        // Llamada al core corregida
        if (download_file_core(argv[i], argv[i], i + 1, argc, &bytes_this_file)) {
            total_success++;
            total_bytes += bytes_this_file;
        } else {
            // Verificamos cancelación manual
            if (in_inkey() == 7) {
                main_print("Batch aborted.");
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
    
    // Reset progress tracking and restore status bar
    progress_current_file[0] = '\0';
    invalidate_status_bar();
    draw_status_bar();
}

// ============================================================================
// HELPER: DESCONEXIÓN SILENCIOSA (Para !CONNECT y QUIT)
// ============================================================================
static void close_connection_sequence(void)
{
    uint16_t t;
    
    current_attr = ATTR_LOCAL;
    main_print("Closing connection...");
    
    // 1. Envío QUIT (Protocolo FTP)
    strcpy(ftp_cmd_buffer, "QUIT\r\n");
    esp_tcp_send(0, ftp_cmd_buffer, strlen(ftp_cmd_buffer));
    
    // Espera breve
    for (t = 0; t < 25; t++) { uart_drain_to_buffer(); wait_frames(1); }

    // 2. Forzamos cierre TCP
    uart_send_string("AT+CIPCLOSE=0\r\n");
    
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
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
        if (i + 1 < out_max) out[i++] = *p;
        p++;
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
    current_attr = ATTR_LOCAL;
    main_print("BitStream v1.0");
    main_print("FTP Client for ZX Spectrum");
    main_print("(C) 2025 M. Ignacio Monge Garcia");
    main_print("ESP8266/AY-UART - Z88DK");
    main_print("UART driver based on code by A. Nihirash");
}

static void cmd_status(void)
{
    current_attr = ATTR_RESPONSE;
    main_print("--- SYSTEM STATUS ---");
    
    // Si creemos estar conectados, verificar activamente
    if (connection_state >= STATE_FTP_CONNECTED) {
        main_puts("Verifying connection... ");
        
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
                if (in_inkey() == 7) {
                    cancelled = 1;
                    break;
                }
                
                if (try_read_line()) {
                    // Cualquier respuesta numérica indica conexión viva
                    if (rx_line[0] >= '1' && rx_line[0] <= '5') {
                        got_response = 1;
                        break;
                    }
                    // También aceptar respuesta dentro de +IPD
                    if (strncmp(rx_line, S_IPD0, 7) == 0) {
                        char *p = strchr(rx_line, ':');
                        if (p && p[1] >= '1' && p[1] <= '5') {
                            got_response = 1;
                            break;
                        }
                    }
                    // Detectar desconexión
                    if (strncmp(rx_line, "0,CLOSED", 8) == 0 ||
                        strncmp(rx_line, "421", 3) == 0) {
                        clear_ftp_state();
                        got_disconnect = 1;
                        break;
                    }
                    rx_pos = 0;
                }
                frames++;
            }
            
            if (cancelled) {
                current_attr = ATTR_ERROR;
                main_print("Cancelled");
            } else if (got_response) {
                main_print("OK");
            } else if (got_disconnect) {
                current_attr = ATTR_ERROR;
                main_print("FAILED (disconnected)");
            } else if (connection_state >= STATE_FTP_CONNECTED) {
                // No response but no explicit close - assume dead
                clear_ftp_state();
                current_attr = ATTR_ERROR;
                main_print("FAILED (no response)");
            }
        } else {
            // Couldn't even send command
            clear_ftp_state();
            current_attr = ATTR_ERROR;
            main_print("FAILED (send error)");
        }
        current_attr = ATTR_RESPONSE;
    }
    
    // Estado de conexión
    main_puts("State: ");
    if (connection_state == STATE_DISCONNECTED) main_print("Disconnected");
    else if (connection_state == STATE_WIFI_OK) main_print("WiFi OK (No FTP)");
    else if (connection_state == STATE_FTP_CONNECTED) main_print("FTP Connected (No Login)");
    else if (connection_state == STATE_LOGGED_IN) main_print("Logged In (Ready)");
    else main_print("Unknown");
    
    // Info del Host
    {
        char *p = tx_buffer;
        p = str_append(p, "Host:  ");
        p = str_append(p, ftp_host);
    }
    main_print(tx_buffer);
    
    // Info del Path actual
    {
        char *p = tx_buffer;
        p = str_append(p, "Path:  ");
        p = str_append(p, ftp_path);
    }
    main_print(tx_buffer);
    
    // Debug
    if (debug_mode) main_print("Debug: ON");
    else main_print("Debug: OFF");
}

static void cmd_wifi(void)
{
    uint8_t result;
    
    current_attr = ATTR_LOCAL;
    main_print("Checking WiFi status...");
    
    result = check_wifi_connection();
    
    if (result == 2) {
        // Cancelado por usuario
        current_attr = ATTR_ERROR;
        main_print("Cancelled");
    } else if (result == 1) {
        current_attr = ATTR_RESPONSE;
        main_print("WiFi: Connected");
    } else {
        current_attr = ATTR_ERROR;
        main_print("WiFi: Disconnected");
        main_print("Check ESP8266.");
    }
}

static void cmd_help(void)
{
    current_attr = ATTR_LOCAL;
    main_print("--- FTP COMMANDS ---");
    main_print("  OPEN host     - Connect to FTP server");
    main_print("  USER name pwd - Login with credentials");
    main_print("  QUIT          - Disconnect from server");
    main_print("  PWD           - Show current directory");
    main_print("  CD path       - Change directory");
    main_print("  LS [filter]   - List files (-d/-f)");
    main_print("  GET file      - Download file(s)");
    main_print("Type !HELP for special commands");
}

static void cmd_help_special(void)
{
    current_attr = ATTR_LOCAL;
    main_print("--- SPECIAL COMMANDS ---");
    main_print("  !CONNECT host[:port][/path] user [pwd]");
    main_print("       Quick connect, login & cd");
    main_print("  !SEARCH [pat]  - Search files");
    main_print("  !STATUS        - Connection info");
    main_print("  !WIFI          - Check ESP connection");
    main_print("  !CLS           - Clear screen");
    main_print("  !DEBUG         - Toggle AT debug logs");
    main_print("  !INIT          - Reset ESP module");
    main_print("  !ABOUT         - Version info");
    current_attr = ATTR_RESPONSE;
    main_print("TIP: Press EDIT to cancel any operation");
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


// ============================================================================
// !SEARCH: BÚSQUEDA AVANZADA (LIMPIA)
// ============================================================================

static void cmd_search(const char *a1, const char *a2, const char *a3)
{
    if (!ensure_logged_in()) return;
    
    drain_mode_fast();  // Max throughput for search
    
    uint32_t t = 0;
    int16_t c;
    char line_buf[128];
    uint8_t line_pos = 0;
    uint8_t matches = 0;
    uint8_t in_data = 0;
    uint16_t ipd_remaining = 0;
    char hdr_buf[24];
    uint8_t hdr_pos = 0;
    
    // Filtros
    char pattern[32]; pattern[0] = 0;
    uint8_t type_mode = 0; 
    uint32_t min_size = 0;
    
    const char *args[3];
    args[0] = a1; args[1] = a2; args[2] = a3;
    
    uint8_t i;
    for (i = 0; i < 3; i++) {
        const char *arg = args[i];
        if (!arg || !*arg) continue;
        if (strcmp(arg, "-d") == 0 || strcmp(arg, "-D") == 0) type_mode = 1;
        else if (strcmp(arg, "-f") == 0 || strcmp(arg, "-F") == 0) type_mode = 2;
        else if (arg[0] == '>') min_size = parse_size_arg(arg);
        else strncpy(pattern, arg, 31);
    }
    
    current_attr = ATTR_LOCAL;
    {
        char *p = tx_buffer;
        p = str_append(p, "Searching");
        if (pattern[0]) { p = str_append(p, " '"); p = str_append(p, pattern); p = char_append(p, '\''); }
        if (min_size) { p = str_append(p, " >"); p = u32_to_dec(p, min_size); p = char_append(p, 'B'); }
        p = str_append(p, "...");
    }
    main_print(tx_buffer);

    if (!setup_list_transfer()) return;

    while (t < TIMEOUT_LONG) {
        // HALT periódico para permitir cancelación
        if ((t & 0x1F) == 0) {
            HALT();
            if (in_inkey() == 7) {
                current_attr = ATTR_ERROR;
                main_print("Cancelled");
                goto search_done;
            }
        }
        
        uart_drain_to_buffer();
        c = rb_pop();
        if (c == -1) {
            HALT();
            // También verificar cancelación cuando no hay datos
            if (in_inkey() == 7) {
                current_attr = ATTR_ERROR;
                main_print("Cancelled");
                goto search_done;
            }
            t++;
            continue;
        }
        t++;
        
        if (!in_data) {
            if (c == '\r' || c == '\n') {
                hdr_buf[hdr_pos] = 0;
                if (strstr(hdr_buf, S_CLOSED1)) goto search_done;
                if (hdr_pos > 7 && strncmp(hdr_buf, S_IPD1, 7) == 0) {
                    char *p = hdr_buf + 7;
                    ipd_remaining = parse_decimal(&p);
                    if (*p == ':') in_data = 1;
                }
                hdr_pos = 0;
            } else if (c == ':' && hdr_pos > 7 && strncmp(hdr_buf, S_IPD1, 7) == 0) {
                hdr_buf[hdr_pos] = 0;
                char *p = hdr_buf + 7;
                ipd_remaining = parse_decimal(&p);
                in_data = 1;
                hdr_pos = 0;
            } else if (hdr_pos < 23) hdr_buf[hdr_pos++] = c;
        } else {
            ipd_remaining--;
            if (c == '\n') {
                line_buf[line_pos] = 0;
                if (line_pos > 10) {
                    // --- PARSER ROBUSTO TAMBIÉN AQUÍ ---
                    char *p_start = line_buf;
                    while (*p_start == ' ') p_start++;
                    
                    char type_char = *p_start;
                    // Consideramos DIR si es 'd' o 'l'
                    uint8_t is_dir = (type_char == 'd' || type_char == 'l');
                    
                    char *name = line_buf + line_pos;
                    while (name > line_buf && *(name-1) != ' ') name--;
                    
                    uint32_t size = 0;
                    if (!is_dir) {
                        // ... (código existente para leer tamaño) ...
                        char *p = p_start; // Usar p_start
                        uint8_t spaces = 0;
                        while (*p && spaces < 4) {
                            if (*p == ' ') { spaces++; while(*p==' ') p++; }
                            if (spaces < 4) p++;
                        }
                        while (*p >= '0' && *p <= '9') { size = size * 10 + (*p - '0'); p++; }
                    }

                    uint8_t pass = 1;
                    if (type_mode == 1 && !is_dir) pass = 0;
                    if (type_mode == 2 && is_dir)  pass = 0;
                    if (min_size > 0 && size < min_size) pass = 0;
                    if (pattern[0] && !str_contains(name, pattern)) pass = 0;

                    if (pass) {
                        char size_str[16];
                        if (is_dir) strcpy(size_str, "<DIR>");
                        else format_size(size, size_str);
                        
                        current_attr = is_dir ? ATTR_USER : ATTR_LOCAL;
                        
                        {
                            char *q = tx_buffer;
                            uint8_t slen;
                            slen = strlen(size_str);
                            while(slen < 8) { q=char_append(q,' '); slen++; }
                            q = str_append(q, size_str);
                            q = char_append(q, ' ');
                            q = str_append(q, name);
                        }
                        main_print(tx_buffer);
                        matches++;
                        
                        // --- PAGINACIÓN SEGURA ---
                        if ((matches % 16) == 0) {
                            uint16_t idle_count = 0;
                            current_attr = ATTR_RESPONSE;
                            main_print("-- More? EDIT=stop --");
                            while(1) {
                                // Seguir drenando UART durante la espera
                                uart_drain_to_buffer();
                                
                                uint8_t k = in_inkey();
                                if (k == 7) goto search_done;
                                if (k != 0) break;
                                
                                // Check for server disconnect
                                idle_count++;
                                if (idle_count > 100) {
                                    idle_count = 0;
                                    if (poll_for_disconnect()) goto search_done;
                                }
                                HALT();
                            }
                        }
                        // -------------------------
                    }
                }
                line_pos = 0;
            } else if (c >= 32 && c < 127 && line_pos < 127) {
                line_buf[line_pos++] = c;
            }
            if (ipd_remaining == 0) in_data = 0;
        }
    }

search_done:
    drain_mode_normal();  // Restore UI responsiveness
    ftp_close_data();
    current_attr = ATTR_RESPONSE;
    {
        char *p = tx_buffer;
        p = str_append(p, "Found: ");
        p = u16_to_dec(p, matches);
    }
    main_print(tx_buffer);
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
            current_attr = ATTR_ERROR;
            main_print(S_NO_CONN);
            return;
        }
    }
    
    // --- COMANDOS "BANG" (!) ---
    if (strcmp(cmd, "!CONNECT") == 0) {
        if (arg1[0] && arg2[0]) {
            char *host = arg1;
            char *init_path = NULL; 
            uint16_t port = 21;
            
            // 1. Separar PATH (Buscamos la primera barra '/')
            // IMPORTANTE: El formato debe ser ftp.sitio.com/carpeta/juegos
            char *slash = strchr(host, '/');
            if (slash) {
                *slash = 0;       // Cortamos el host aquí
                init_path = slash + 1; // El path es lo que sigue
            }

            // 2. Separar PUERTO (Buscamos ':')
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
            
            // 3. Ejecutar OPEN
            cmd_open(host, port);
            
            // 4. Si conecta, ejecutar USER
            if (connection_state == STATE_FTP_CONNECTED) {
                // Pequeña pausa antes del user para estabilizar
                wait_frames(10); 
                
                cmd_user(arg2, arg3[0] ? arg3 : "zx@zx.net");
                
                // 5. Si loguea y había path, ejecutar CD
                if (connection_state == STATE_LOGGED_IN && init_path && init_path[0]) {
                    
                    current_attr = ATTR_LOCAL;
                    {
                        char *p = tx_buffer;
                        p = str_append(p, "Navigating to: ");
                        p = str_append(p, init_path);
                    }
                    main_print(tx_buffer);

                    // --- RETARDO CRÍTICO ---
                    // Esperamos 0.5 segundos a que el servidor respire tras el Login
                    // antes de lanzarle el comando CWD.
                    uint8_t w;
                    for(w=0; w<25; w++) { 
                        uart_drain_to_buffer(); 
                        wait_frames(1); 
                    }
                    
                    cmd_cd(init_path);
                }
                // Si no hay init_path, obtener el path actual
                else if (connection_state == STATE_LOGGED_IN) {
                    cmd_pwd();
                }
            }
        } else {
            current_attr = ATTR_ERROR;
            // Recordatorio de la sintaxis correcta
            main_print("Usage: !CONNECT host/path user [pass]");
        }
        return;
    }
    
    if (strcmp(cmd, "!SEARCH") == 0) {
        cmd_search(arg1, arg2, arg3);
        return;
    }
    
    if (strcmp(cmd, "!STATUS") == 0) {
        cmd_status();
        return;
    }

    if (strcmp(cmd, "!WIFI") == 0) {
        cmd_wifi();
        return;
    }

    if (strcmp(cmd, "!ABOUT") == 0) {
        cmd_about();
        return;
    }
    
    
    if (strcmp(cmd, "!CLS") == 0) {
        cmd_cls();
        return;
    }

    if (strcmp(cmd, "!DEBUG") == 0) {
        debug_mode = !debug_mode;
        current_attr = ATTR_LOCAL;
        main_print(debug_mode ? "Debug mode ON" : "Debug mode OFF");
        return;
    }
    
    if (strcmp(cmd, "!INIT") == 0) {
        current_attr = ATTR_LOCAL;
        main_print("Re-initializing...");
        connection_state = STATE_DISCONNECTED;
        strcpy(ftp_host, S_EMPTY);
        strcpy(ftp_user, S_EMPTY);
        strcpy(ftp_path, S_EMPTY);
        full_initialization_sequence();
        return;
    }
    
    if (strcmp(cmd, "!HELP") == 0) {
        cmd_help_special();
        return;
    }

    // --- COMANDOS ESTÁNDAR ---
    
    if (strcmp(cmd, "OPEN") == 0 && arg1[0]) {
        cmd_open(arg1, 21);
    }
    else if (strcmp(cmd, "USER") == 0 && arg1[0]) {
        cmd_user(arg1, arg2[0] ? arg2 : "zx@zx.net");
        if (connection_state == STATE_LOGGED_IN) {
            cmd_pwd();
        }
    }
    else if (strcmp(cmd, "PWD") == 0) {
        cmd_pwd();
    }
    else if (strcmp(cmd, "CD") == 0 && arg1[0]) {
        cmd_cd(arg1);
    }
    else if (strcmp(cmd, "LS") == 0) {
        cmd_ls(arg1); 
    }
    else if (strcmp(cmd, "GET") == 0) {
        // TRUCO: Pasamos el puntero al resto de la línea original
        // 'line' contiene toda la linea.
        // Hemos extraído 'cmd' al principio.
        // Buscamos dónde termina el comando 'GET' en la línea original
        char *args_ptr = line;
        
        // Saltar el comando "GET"
        while (*args_ptr && *args_ptr != ' ') args_ptr++;
        // Saltar espacios hasta el primer argumento
        args_ptr = skip_ws(args_ptr);
        
        if (*args_ptr) {
            cmd_get(args_ptr);
        } else {
            main_print("Usage: GET file...");
        }
    }
    else if (strcmp(cmd, "QUIT") == 0) {
        cmd_quit();
    }
    else if (strcmp(cmd, "HELP") == 0 || strcmp(cmd, "?") == 0) {
        cmd_help();
    }
    else {
        current_attr = ATTR_ERROR;
        main_print("Unknown command. Type HELP");
    }
}

// ============================================================================
// SCREEN INITIALIZATION
// ============================================================================

static void draw_banner(void)
{
    clear_line(BANNER_START, ATTR_BANNER);
    print_str64(BANNER_START, 2, "BitStream v1.0 - A FTP client for ZX Spectrum", ATTR_BANNER);
}

static void init_screen(void)
{
    uint8_t i;
    zx_border(INK_BLACK);
    for (i = 0; i < 24; i++) clear_line(i, PAPER_BLACK);
    
    clear_line(BANNER_START, ATTR_BANNER);
    clear_line(1, ATTR_MAIN_BG);
    clear_zone(MAIN_START, MAIN_LINES, ATTR_MAIN_BG);
    clear_line(19, ATTR_MAIN_BG);
    clear_line(STATUS_LINE, ATTR_STATUS);
    clear_zone(INPUT_START, INPUT_LINES, ATTR_INPUT_BG);
    
    draw_banner();
    invalidate_status_bar(); 
    draw_status_bar();
    
    main_line = MAIN_START;
    main_col = 0;
    
    line_len = 0;
    line_buffer[0] = 0;
    cursor_pos = 0;
    
    print_char64(INPUT_START, 0, '>', ATTR_PROMPT);
}

// ============================================================================
// MAIN
// ============================================================================

// ============================================================================
// BACKGROUND MONITORING
// ============================================================================

static void check_connection_alive(void)
{
    // Si no estamos conectados por FTP, solo limpiamos basura rápida y salimos
    if (connection_state < STATE_FTP_CONNECTED) {
        if (ay_uart_ready()) ay_uart_read();
        return;
    }

    // Guardamos el límite actual
    uint8_t prev_limit = uart_drain_limit;
    
    // Subimos a 16 bytes para capturar mensajes completos como "0,CLOSED\r\n"
    uart_drain_limit = 16;

    if (try_read_line()) {
        const char *reason = NULL;
        
        // Detección estricta para evitar falsos positivos
        if (strncmp(rx_line, "0,CLOSED", 8) == 0) {
            reason = "Remote host closed socket";
        }
        else if (strncmp(rx_line, "421", 3) == 0) {
            if (str_contains(rx_line, "imeout")) reason = "Idle Timeout (421)";
            else reason = "Service Closing (421)";
        }

        if (reason) {
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
        // Solo resetear rx_pos si procesamos algo (evitar perder mensajes fragmentados)
        rx_pos = 0;
    }
    // NO resetear rx_pos aquí - puede haber mensaje parcial acumulándose
    
    // Restauramos el límite
    uart_drain_limit = prev_limit;
}

static void print_intro_banner(void)
{
    // Texto blanco brillante sobre fondo negro (sin el azul "pretencioso")
    current_attr = PAPER_BLACK | INK_WHITE | BRIGHT;
    
    main_print("BitStream v1.0 - FTP Client");
    main_print("(C) M. Ignacio Monge Garcia 2025");
    main_print("----------------------------------------------------------------");
}

void main(void)
{
    uint8_t c;
    
    init_screen();
    
    // 1. Mensaje de bienvenida (Antes de inicializar)
    print_intro_banner();
    
    smart_init();
    
    
    // 2. Mensaje informativo de una sola línea (Sustituye al anterior)
    current_attr = ATTR_LOCAL; 
    main_print("Type HELP or !HELP. Use EDIT key to cancel operations.");
    main_newline();
    
    redraw_input_from(0);
    while (1) {
        HALT();
        
        // Monitor de conexión en segundo plano
        check_connection_alive();
        
        c = read_key();
        if (c == 0) continue;
        
        // Navegación
        if (c == KEY_UP) {
            uint8_t prev_len = line_len;
            history_nav_up();
            cursor_pos = line_len;
            if (prev_len >= line_len) {
                uint8_t i;
                for (i = line_len; i <= prev_len; i++) {
                    uint16_t abs_pos = i + 2;
                    uint8_t row = INPUT_START + (abs_pos / SCREEN_COLS);
                    uint8_t col = abs_pos % SCREEN_COLS;
                    if (row <= INPUT_END) print_char64(row, col, ' ', ATTR_INPUT_BG);
                }
            }
            redraw_input_from(0);
        }
        else if (c == KEY_DOWN) {
            uint8_t prev_len = line_len;
            history_nav_down();
            cursor_pos = line_len;
            if (prev_len >= line_len) {
                uint8_t i;
                for (i = line_len; i <= prev_len; i++) {
                    uint16_t abs_pos = i + 2;
                    uint8_t row = INPUT_START + (abs_pos / SCREEN_COLS);
                    uint8_t col = abs_pos % SCREEN_COLS;
                    if (row <= INPUT_END) print_char64(row, col, ' ', ATTR_INPUT_BG);
                }
            }
            redraw_input_from(0);
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