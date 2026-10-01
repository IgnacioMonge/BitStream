// ============================================================================
// globals.c - Global state, constants, keyboard ASM, common strings
// ============================================================================

#include "../include/bitstream.h"

// Keyboard: immediate BREAK detection (CAPS SHIFT + SPACE)
static uint8_t g_user_cancel;

// CAPS LOCK state
uint8_t caps_lock_mode = 0;
uint8_t caps_latch = 0;

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
// "Clean" shift only: CAPS combined with a number key (cursors, DELETE,
// EDIT...), SPACE (BREAK) or SYMBOL SHIFT is a function chord, not a
// letter shift (SpecTalkZX _key_shift_held contract).
static uint8_t key_shift_held(void)
{
#asm
    ld   bc, 0xFEFE
    in   a, (c)
    bit  0, a            ; CAPS SHIFT
    jr   nz, _shift_no

    ld   b, 0xF7         ; 1-5
    in   a, (c)
    and  0x1F
    cp   0x1F
    jr   nz, _shift_no

    ld   b, 0xEF         ; 0-6
    in   a, (c)
    and  0x1F
    cp   0x1F
    jr   nz, _shift_no

    ld   b, 0x7F         ; SPACE (bit 0) / SYMBOL SHIFT (bit 1)
    in   a, (c)
    and  0x03
    cp   0x03
    jr   nz, _shift_no

    ld   hl, 1
    ret

_shift_no:
    ld   hl, 0
    ret
#endasm
}

// Shift state shown by the cursor. Sampled once per frame by the main loop
// and promoted only after it has been stable for a few frames, so the CAPS
// half of a chord (arrows, DELETE, BREAK) never flickers the cursor.
uint8_t cursor_shift_held;      // read by asm _draw_cursor_underline


static uint8_t key_break_down(void)
{
#asm
    push bc
    ld   h, 0

    ; CAPS SHIFT (Fila 0xFE, bit 0)
    ld   bc, 0xFEFE
    in   a, (c)
    and  0x01
    ld   l, a        ; L = estado CAPS (0=pulsado)

    ; SPACE (Fila 0x7F, bit 0)
    ld   bc, 0x7FFE
    in   a, (c)
    and  0x01
    or   l           ; A = CAPS | SPACE. Si ambos son 0, resultado es 0.

    jr   nz, _not_break

    ld   l, 1        ; BREAK pulsado -> Devolver 1
    pop  bc
    ret

_not_break:
    ld   l, 0
    pop  bc
    ret
#endasm
}


// ============================================================================
// FORWARD DECLARATIONS (bridge across SCU modules)
// ============================================================================
static void main_print(const char *s) __z88dk_fastcall;
extern void main_newline(void);
extern void main_puts(const char *s) __z88dk_fastcall;
extern char* skip_ws(char *p) __z88dk_fastcall;
static void invalidate_status_bar(void);
static uint32_t parse_size_arg(const char *s) __z88dk_fastcall;
static void redraw_input_from(uint8_t start_pos) __z88dk_fastcall;
extern void draw_cursor_underline(uint8_t y, uint8_t col) __z88dk_callee;
static void draw_status_bar_real(void);
extern void print_char64(uint8_t y, uint8_t col, uint8_t c, uint8_t attr) __z88dk_callee;
extern void put_char64_input_cached(uint8_t y, uint8_t col, uint8_t c, uint8_t attr) __z88dk_callee;
extern uint8_t key_scan(void);
extern uint8_t read_key(void);
extern uint8_t key_ss_arrow(void);
static void fail(const char *msg) __z88dk_fastcall;
static void close_connection_sequence(void);
static void wait_poll(uint16_t frames) __z88dk_fastcall;
static void wait_frames(uint16_t frames) __z88dk_fastcall;
extern uint16_t parse_decimal(char **pp);
static uint8_t prompt_input_zone(const char *prompt, char *buf, uint8_t max_len, uint8_t masked);

// ============================================================================
// INPUT LINE STATE (shared across modules)
// ============================================================================

static char line_buffer[LINE_BUFFER_SIZE];
static uint8_t line_len;
static uint8_t cursor_pos;
static uint8_t input_prompt_len = 2;  // "> " = 2 chars; prompt mode may be longer

// ============================================================================
// BUFFERS
// ============================================================================

static char tx_buffer[TX_BUFFER_SIZE];
static char ftp_cmd_buffer[128];

// File write buffer - 512 bytes for efficient SD writes
static uint8_t file_buffer[512];
static uint16_t file_buf_pos;

// ============================================================================
// COMMON STRINGS (save code space)
// ============================================================================

static const char S_DATA_FAIL[] = "Data connect failed";
static const char S_CANCEL[]    = "Cancelled";
static const char S_DOTS[]      = ".";
static const char S_ERROR_TAG[] = "Error: ";

// Repeated UI strings (String Tail Merging optimization)
static const char S_EMPTY[] = "---";
static const char S_NO_CONN[] = "Not connected";
static const char S_LOGIN_BAD[] = "Login incorrect";
static const char S_CHECKING[] = "Checking connection.";
static const char S_OK[] = "OK";
static const char S_UNKNOWN_CMD[] = "Unknown cmd. Type HELP";
static const char S_DISCONN[] = "Disconnected";
static const char S_NO_WIFI[] = "No WiFi";
static const char S_SLASH[] = "/";
static const char S_LIST_HDR[] = "T      Size Filename";

// st_copy_n is in asm/bitstream_asm.asm
extern void st_copy_n(char *dst, const char *src, uint8_t max_len);
#define safe_copy(dst, src, sz) st_copy_n((dst), (src), (uint8_t)(sz))

// ============================================================================
// FTP STATE
// ============================================================================

static char wifi_client_ip[16] = "0.0.0.0";
static char ftp_host[48] = "---";   // full !CONNECT host (PASV fallback uses it)
static char ftp_user[20] = "---";
static char ftp_path[PATH_SIZE] = "---";
static char data_ip[48];   // dotted PASV address, or ftp_host when unroutable

static uint16_t data_port;
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

uint8_t main_line = MAIN_START;
uint8_t main_col = 0;
uint8_t current_attr = ATTR_LOCAL;

// Progress bar state
static uint8_t status_bar_overwritten;
static char spinner_chars[] = "|/-\\";
static uint8_t spinner_idx;

// ============================================================================
// COMMAND HISTORY
// ============================================================================

static char history[HISTORY_SIZE][HISTORY_LEN];
static uint8_t hist_head;
static uint8_t hist_count;
static int8_t hist_pos = -1;
static char temp_input[LINE_BUFFER_SIZE];

static void history_add(const char *cmd, uint8_t len)
{
    uint8_t i;
    if (len == 0) return;
    if (hist_count > 0) {
        uint8_t last = (hist_head + HISTORY_SIZE - 1) & (HISTORY_SIZE - 1);
        if (strcmp(history[last], cmd) == 0) return;
    }
    for (i = 0; i < len && i < HISTORY_LEN - 1; i++) {
        history[hist_head][i] = cmd[i];
    }
    history[hist_head][i] = 0;
    hist_head = (hist_head + 1) & (HISTORY_SIZE - 1);
    if (hist_count < HISTORY_SIZE) hist_count++;
    hist_pos = -1;
}

static void history_nav_up(void)
{
    uint8_t idx;
    if (hist_count == 0) return;
    if (hist_pos == -1) memcpy(temp_input, line_buffer, line_len + 1);
    if (hist_pos < (int8_t)(hist_count - 1)) hist_pos++;
    idx = (hist_head + HISTORY_SIZE - 1 - hist_pos) & (HISTORY_SIZE - 1);
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
        idx = (hist_head + HISTORY_SIZE - 1 - hist_pos) & (HISTORY_SIZE - 1);
        safe_copy(line_buffer, history[idx], sizeof(line_buffer));
        line_len = strlen(line_buffer);
    }
    cursor_pos = line_len;
}

static void history_nav_and_redraw(int8_t direction) __z88dk_fastcall
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
            uint8_t row = INPUT_START + (abs_pos >> 6);
            uint8_t col = abs_pos & 63;
            if (row <= INPUT_END) put_char64_input_cached(row, col, ' ', ATTR_INPUT_BG);
        }
    }
    redraw_input_from(0);
}
