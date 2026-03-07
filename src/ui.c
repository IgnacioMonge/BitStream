// ============================================================================
// ui.c - Video, widgets, input zone, keyboard
// ============================================================================

// ============================================================================
// UI RENDER OPTIMIZATION: Dirty lines (deferred status bar redraw)
// ============================================================================

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

// ============================================================================
// UI INPUT CACHE (Optimized character rendering)
// ============================================================================

static uint8_t input_cache_char[INPUT_LINES][SCREEN_COLS];
static uint8_t input_cache_attr[INPUT_LINES][32];

static void input_cache_invalidate_cell(uint8_t y, uint8_t col)
{
    if (y < INPUT_START || y > INPUT_END || col >= SCREEN_COLS) return;
    input_cache_char[y - INPUT_START][col] = 0xFF;
}

// OPTIMIZED: memset is faster than nested loops (uses LDIR internally)
static void input_cache_invalidate(void)
{
    memset(input_cache_char, 0xFF, sizeof(input_cache_char));  // 192 bytes
    memset(input_cache_attr, 0xFF, sizeof(input_cache_attr));  // 96 bytes
}

// line_buffer, line_len, cursor_pos are defined in globals.c

// ============================================================================
// CACHED INPUT CHARACTER
// ============================================================================
static void put_char64_input_cached(uint8_t y, uint8_t col, uint8_t c, uint8_t attr)
{
    if (y < INPUT_START || y > INPUT_END || col >= SCREEN_COLS) return;

    uint8_t local_y = y - INPUT_START;

    if (input_cache_char[local_y][col] == c && input_cache_attr[local_y][col >> 1] == attr) {
        return;
    }

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

            uint16_t char_abs = (cursor_pos - 1) + 2;
            uint8_t row = INPUT_START + (char_abs / SCREEN_COLS);
            uint8_t col = char_abs % SCREEN_COLS;

            put_char64_input_cached(row, col, c, ATTR_INPUT);

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
// VIDEO MEMORY FUNCTIONS
// ============================================================================

// Pre-calculated screen base addresses for all 24 text lines (scanline 0)
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
    return (uint8_t*)(screen_row_base[y] + ((uint16_t)scanline << 8) + phys_x);
}

static uint8_t* attr_addr(uint8_t y, uint8_t phys_x)
{
    return (uint8_t*)(0x5800 + (uint16_t)y * 32 + phys_x);
}

// ============================================================================
// DRAW HORIZONTAL LINE (1 pixel height) - Fast and compact
// ============================================================================
static void draw_hline(uint8_t y, uint8_t x_start, uint8_t width, uint8_t scanline, uint8_t attr)
{
    uint8_t x;
    uint8_t *screen_ptr;
    uint8_t *attr_ptr;

    screen_ptr = (uint8_t*)(screen_row_base[y] + ((uint16_t)scanline << 8) + x_start);
    attr_ptr = (uint8_t*)(0x5800 + (uint16_t)y * 32 + x_start);

    for (x = 0; x < width; x++) {
        *screen_ptr++ = 0xFF;
        *attr_ptr++ = attr;
    }
}


// print_str64_char and renderer globals are in asm/bitstream_asm.asm
extern void print_str64_char(uint8_t ch) __z88dk_fastcall;
extern uint8_t g_ps64_y;
extern uint8_t g_ps64_col;
extern uint8_t g_ps64_attr;

// C wrapper: sets globals and calls ASM renderer
static void print_char64(uint8_t y, uint8_t col, uint8_t c, uint8_t attr) __z88dk_callee
{
    g_ps64_y = y;
    g_ps64_col = col;
    g_ps64_attr = attr;
    print_str64_char(c);
}




// clear_line, clear_zone are in asm/bitstream_asm.asm
extern void clear_line(uint8_t y, uint8_t attr);
extern void clear_zone(uint8_t start, uint8_t lines, uint8_t attr);

static void print_str64(uint8_t y, uint8_t col, const char *s, uint8_t attr) __z88dk_callee
{
    while (*s && col < SCREEN_COLS) print_char64(y, col++, *s++, attr);
}


// scroll_main_zone is in asm/bitstream_asm.asm (includes DI/EI for artifact-free scroll)
extern void scroll_main_zone(void);

// ============================================================================
// STATUS BAR HELPERS
// ============================================================================

#define ATTR_LBL (PAPER_WHITE | INK_BLUE)
#define ATTR_VAL (PAPER_WHITE | INK_BLACK)

static void print_padded(uint8_t y, uint8_t col, const char *s, uint8_t attr, uint8_t width) __z88dk_callee
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
    static const uint8_t gfx[] = {0x00, 0x3C, 0x7E, 0x7E, 0x7E, 0x7E, 0x3C, 0x00};
    uint8_t i;

    uint8_t *ptr = screen_line_addr(y, phys_x, 0);

    for (i = 0; i < 8; i++) {
        *ptr = gfx[i];
        ptr += 256;
    }
    *attr_addr(y, phys_x) = attr;
}

// ============================================================================
// DECIMAL FORMATTING HELPERS
// ============================================================================

static char* u32_to_dec(char *dst, uint32_t v) __z88dk_callee
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

// str_append, char_append, u16_to_dec are in asm/bitstream_asm.asm
extern char* str_append(char *dst, const char *src) __z88dk_callee;
extern char* char_append(char *dst, char c) __z88dk_callee;
extern char* u16_to_dec(char *dst, uint16_t v) __z88dk_callee;

// Format size in human-readable form
static void format_size(uint32_t bytes, char *buf)
{
    char *p = buf;
    if (bytes >= 1048576UL) {
        uint32_t whole = bytes >> 20;
        uint32_t rem = bytes & 0xFFFFF;
        uint8_t frac = (uint8_t)(((rem >> 17) * 10) >> 3);
        if (frac > 9) frac = 9;
        p = u32_to_dec(p, whole);
        *p++ = '.';
        *p++ = (char)('0' + frac);
        *p++ = 'M';
        *p++ = 'B';
        *p = 0;
    } else if (bytes >= 1024UL) {
        p = u32_to_dec(p, bytes >> 10);
        *p++ = 'K';
        *p++ = 'B';
        *p = 0;
    } else {
        p = u32_to_dec(p, bytes);
        *p++ = 'B';
        *p = 0;
    }
}

// ============================================================================
// PROGRESS BAR FOR DOWNLOADS
// ============================================================================

#define ATTR_DL_TEXT    (PAPER_WHITE | INK_BLACK)
#define ATTR_DL_NAME    (PAPER_WHITE | INK_BLUE)
#define ATTR_DL_BAR_ON  (PAPER_WHITE | INK_RED)
#define ATTR_DL_BAR_OFF (PAPER_WHITE | INK_BLACK)

static char progress_current_file[13] = "";

static void draw_progress_bar(const char *filename, uint32_t received, uint32_t total)
{
    char size_buf[24];
    char total_buf[12];
    uint8_t i;
    char name_short[13];

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

    print_char64(STATUS_LINE, col++, ' ', ATTR_DL_TEXT);
    print_char64(STATUS_LINE, col++, ' ', ATTR_DL_TEXT);

    print_char64(STATUS_LINE, col++, '[', ATTR_DL_TEXT);

    uint8_t extra_blocks = 0;
    if (total > 0) {
        extra_blocks = (uint8_t)((received * BAR_WIDTH) / total);
        if (extra_blocks > BAR_WIDTH) extra_blocks = BAR_WIDTH;
    }

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

    print_char64(STATUS_LINE, col++, ']', ATTR_DL_TEXT);
    print_char64(STATUS_LINE, col++, ' ', ATTR_DL_TEXT);
    print_char64(STATUS_LINE, col++, ' ', PAPER_WHITE | INK_BLUE);

    spinner_idx = (spinner_idx + 1) % 4;
    print_char64(STATUS_LINE, 63, spinner_chars[spinner_idx], PAPER_WHITE | INK_BLUE);
}

// Imprime una ruta ajustada al ancho de pantalla (Truncamiento por la izquierda)
static void print_smart_path(const char *prefix, const char *path)
{
    char buf[SCREEN_COLS + 1];
    uint8_t prefix_len = strlen(prefix);
    uint8_t path_len = strlen(path);
    uint8_t avail = SCREEN_COLS - prefix_len;

    char *p = buf;
    p = str_append(p, prefix);

    if (path_len > avail) {
        p = char_append(p, '~');
        p = str_append(p, path + path_len - (avail - 1));
    } else {
        p = str_append(p, path);
    }

    uint8_t old_attr = current_attr;
    current_attr = ATTR_RESPONSE;
    main_print(buf);
    current_attr = old_attr;
}

// ============================================================================
// STATUS BAR (OPTIMIZADA - ACTUALIZACIÓN PARCIAL)
// ============================================================================

static char last_host[32] = "";
static char last_user[20] = "";
static char last_path[PATH_SIZE] = "";
static uint8_t last_conn_state = 255;
static uint8_t force_status_redraw = 1;

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

    const uint8_t W_HOST = 15;
    const uint8_t W_USER = 13;
    const uint8_t W_PATH = 19;

    const uint8_t P_HOST = 4;
    const uint8_t P_USER = 25;
    const uint8_t P_PATH = 43;

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
            buf_short[0] = '~';
            strncpy(buf_short + 1, ftp_path + len - (W_PATH - 1), W_PATH - 1);
            buf_short[W_PATH] = '\0';
        } else {
            safe_copy(buf_short, ftp_path, sizeof(buf_short));
        }
        print_padded(STATUS_LINE, P_PATH, buf_short, ATTR_VAL, W_PATH);
        safe_copy(last_path, ftp_path, sizeof(last_path));
    }

    // INDICADOR
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

// main_newline, main_putc, main_puts are in asm/bitstream_asm.asm
extern void main_newline(void);
extern void main_putc(uint8_t c) __z88dk_fastcall;
extern void main_puts(const char *s) __z88dk_fastcall;

static void main_print(const char *s)
{
    main_puts(s);
    main_newline();
}

static void fail(const char *msg)
{
    current_attr = ATTR_ERROR;
    main_print(msg);
}

// Forward declarations for input functions used by prompt_input_zone
static void input_clear(void);
static void input_backspace(void);
static void input_left(void);
static void input_right(void);
static void input_add_char(uint8_t c);
static uint8_t read_key(void);

// Prompt: shows prompt text in main zone, uses normal "> " input zone for typing.
// masked: if 1, shows '*' (UP toggles visibility).
// Returns length of input. EDIT cancels (returns 0 with buf[0]=0).
static uint8_t prompt_input_zone(const char *prompt, char *buf, uint8_t max_len, uint8_t masked)
{
    uint8_t hide = masked;
    uint8_t c;

    // Show prompt in main zone
    current_attr = ATTR_LOCAL;
    main_puts(prompt);

    // Clear input zone and set up fresh "> " prompt
    input_clear();

    // Wait for any held key to release (debounce between consecutive prompts)
    { uint8_t w; for (w = 0; w < 10; w++) { HALT(); uart_drain_to_buffer(); } }
    while (in_inkey() != 0) { HALT(); uart_drain_to_buffer(); }

    while (1) {
        HALT();
        uart_drain_to_buffer();

        if (key_edit_down()) {
            buf[0] = 0;
            input_clear();
            return 0;
        }

        c = read_key();
        ui_flush_dirty();
        if (c == 0) continue;

        if (c == KEY_ENTER) {
            uint8_t rlen = line_len;
            if (rlen > max_len) rlen = max_len;
            memcpy(buf, line_buffer, rlen);
            buf[rlen] = 0;

            input_clear();
            return rlen;
        }

        if (c == KEY_UP && masked) {
            uint8_t i;
            hide = !hide;
            for (i = 0; i < line_len; i++) {
                uint16_t abs = i + 2;
                uint8_t row = INPUT_START + (abs / SCREEN_COLS);
                uint8_t col = abs % SCREEN_COLS;
                if (row > INPUT_END) break;
                put_char64_input_cached(row, col, hide ? '*' : line_buffer[i], ATTR_INPUT);
            }
            continue;
        }

        if (c == KEY_LEFT) { input_left(); }
        else if (c == KEY_RIGHT) { input_right(); }
        else if (c == KEY_BACKSPACE) { input_backspace(); }
        else if (c >= 32 && c <= 126) {
            input_add_char(c);
        }

        // If masked, overwrite visible chars with '*'
        if (masked && hide) {
            uint8_t i;
            for (i = 0; i < line_len; i++) {
                uint16_t abs = i + 2;
                uint8_t row = INPUT_START + (abs / SCREEN_COLS);
                uint8_t col = abs % SCREEN_COLS;
                if (row > INPUT_END) break;
                put_char64_input_cached(row, col, '*', ATTR_INPUT);
            }
        }
    }
}

static void print_char_line(uint8_t len, char ch)
{
    if (ch == '-') {
        draw_hline(main_line, 0, len, 1, current_attr);
        main_newline();
        return;
    }

    uint8_t i;
    for (i = 0; i < len; i++) main_putc(ch);
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

    uint8_t mask = (half == 0) ? 0xF0 : 0x0F;
    uint8_t inv_mask = ~mask;

    *attr_addr(y, phys_x) = ATTR_INPUT;

    ptr0 = screen_line_addr(y, phys_x, 0);
    ptr7 = screen_line_addr(y, phys_x, 7);

    *ptr0 &= inv_mask;
    *ptr7 &= inv_mask;

    uint8_t shift_pressed = key_shift_held();
    uint8_t effective_caps = (caps_lock_mode ^ shift_pressed);

    if (effective_caps) {
        *ptr0 |= mask;
    } else {
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

    if (k == '0' && debounce_zero > 0) {
        debounce_zero--;
        return 0;
    }

    if (k != last_k) {
        // Case-fold check: ignore shift release (e.g. 'S' -> 's')
        uint8_t lk_fold = last_k | 32;
        uint8_t k_fold = k | 32;
        if (lk_fold == k_fold && lk_fold >= 'a' && lk_fold <= 'z') {
            last_k = k;
            return 0;
        }

        last_k = k;

        if (k == KEY_BACKSPACE) {
            repeat_timer = 12;
            debounce_zero = 8;
        } else if (k == KEY_LEFT || k == KEY_RIGHT) {
            repeat_timer = 15;
        } else if (k == KEY_UP || k == KEY_DOWN) {
            repeat_timer = 15;
        } else {
            repeat_timer = 20;
        }

        return k;
    }

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

        return 0;  // Normal keys don't repeat
    }
}
