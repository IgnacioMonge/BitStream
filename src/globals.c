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
static void draw_cursor_underline(uint8_t y, uint8_t col);
static uint8_t wait_for_ftp_code_fast(uint16_t max_frames, const char *code3);
static void draw_status_bar_real(void);
static void print_char64(uint8_t y, uint8_t col, uint8_t c, uint8_t attr) __z88dk_callee;
static void put_char64_input_cached(uint8_t y, uint8_t col, uint8_t c, uint8_t attr);
static void fail(const char *msg) __z88dk_fastcall;
static void close_connection_sequence(void);
extern void uart_drain_to_buffer(void);
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

// Repeated UI strings (String Tail Merging optimization)
static const char S_EMPTY[] = "---";
static const char S_NO_CONN[] = "Not connected";
static const char S_LOGIN_BAD[] = "Login incorrect";
static const char S_CHECKING[] = "Checking connection.";
static const char S_OK[] = "OK";
static const char S_ERROR[] = "ERROR";
static const char S_UNKNOWN_CMD[] = "Unknown cmd. Type HELP";
static const char S_DISCONN[] = "Disconnected";
static const char S_CONNECT[] = "CONNECT";
static const char S_CLOSED[] = "CLOSED";
static const char S_ATE0[] = "ATE0\r\n";
static const char S_AT[] = "AT\r\n";
static const char S_NO_WIFI[] = "No WiFi";
static const char S_0CLOSED[] = "0,CLOSED";
static const char S_550[] = "550";
static const char S_553[] = "553";
static const char S_SLASH[] = "/";
static const char S_LIST_HDR[] = "T      Size Filename";

// st_copy_n is in asm/bitstream_asm.asm
extern void st_copy_n(char *dst, const char *src, uint8_t max_len);
#define safe_copy(dst, src, sz) st_copy_n((dst), (src), (uint8_t)(sz))

// ============================================================================
// FTP STATE
// ============================================================================

static char wifi_client_ip[16] = "0.0.0.0";
static char ftp_host[32] = "---";
static char ftp_user[20] = "---";
static char ftp_path[PATH_SIZE] = "---";
static char data_ip[16];

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

// esxDOS detection
extern uint8_t detect_esxdos(void);
static uint8_t esxdos_available;

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
