// ============================================================================
// ftp.c - FTP protocol, esxDOS, download, list
// ============================================================================

// ============================================================================
// FTP PROTOCOL LAYER
// ============================================================================

static uint8_t ftp_command(const char *cmd)
{
    uint16_t len = strlen(cmd);

    if (len > (sizeof(ftp_cmd_buffer) - 3)) {
        fail("Buffer overflow!");
        return 0;
    }

    st_copy_n(ftp_cmd_buffer, cmd, sizeof(ftp_cmd_buffer) - 2);
    strcat(ftp_cmd_buffer, S_CRLF);

    return esp_tcp_send(0, ftp_cmd_buffer, len + 2);
}

// parse_decimal is in asm/bitstream_asm.asm (sccz80 cdecl)
extern uint16_t parse_decimal(char **pp);

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
    esp_tcp_close(1);

    uint16_t i;
    for (i = 0; i < 25; i++) {
        HALT();
        uart_drain_to_buffer();
    }

    rb_flush();
}

// Setup PASV + data connection + send LIST command
static uint8_t setup_list_transfer(void)
{
    rx_reset_all();

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

    wait_frames(3);

    return 1;
}

// ============================================================================
// ESXDOS FILE OPERATIONS
// ============================================================================

static uint8_t esx_fopen_write(const char *filename)
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
        jr c, esx_open_fail2

        pop ix
        ld b, 0x0E          ; FMODE_CREATE = create/truncate + write
        rst 0x08
        defb 0x9A           ; ESX_FOPEN
        jr c, esx_open_fail
        ld l, a
        jr esx_open_done
    esx_open_fail2:
        pop hl              ; Clean stacked filename
    esx_open_fail:
        ld l, 255
    esx_open_done:
        ld h, 0
    __endasm;
}

// Global variables for esxDOS operations
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
        pop ix
        ld bc, (_esx_length)
        rst 0x08
        defb 0x9E           ; ESX_FWRITE
        jr c, esx_write_fail
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
static void cmd_user(const char *user, const char *pass);
static void interactive_login(void);

// ============================================================================
// ENSURE LOGGED IN
// ============================================================================

static uint8_t ensure_logged_in(void)
{
    if (connection_state >= STATE_FTP_CONNECTED) {
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

    if (connection_state == STATE_LOGGED_IN) return 1;

    if (connection_state == STATE_DISCONNECTED || connection_state == STATE_WIFI_OK) {
        fail("Not connected. Use OPEN.");
    } else if (connection_state == STATE_FTP_CONNECTED) {
        fail("Not logged in. Use USER.");
    }

    return 0;
}

// ============================================================================
// HELPER: Parse host[:port][/path] format
// ============================================================================
static uint16_t parse_host_port_path(char *input, char **out_host, char **out_path)
{
    uint16_t port = 21;
    char *host = input;
    char *path = NULL;

    char *slash = strchr(host, '/');
    if (slash) {
        *slash = 0;
        path = slash + 1;
    }

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
    if (out_path) *out_path = path;
    return port;
}

static void cmd_open(const char *host, uint16_t port)
{
    if (!confirm_disconnect()) return;

    safe_copy(ftp_path, "---", sizeof(ftp_path));

    last_path[0] = 0;
    draw_status_bar();

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
                draw_status_bar();
                return;
            }

            if (strstr(rx_line, S_CLOSED) || strstr(rx_line, S_ERROR) || strstr(rx_line, "421")) {
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

// Interactive login prompt (called after OPEN, not from !CONNECT)
static void interactive_login(void)
{
    static char user_buf[20];
    static char pass_buf[32];
    uint8_t ulen, plen;

    if (connection_state != STATE_FTP_CONNECTED) return;

    ulen = prompt_input_zone("User (ENTER=anonymous): ", user_buf, 19, 0);
    if (connection_state < STATE_FTP_CONNECTED) return;

    if (ulen == 0) {
        safe_copy(user_buf, "anonymous", sizeof(user_buf));
    }
    current_attr = ATTR_USER;
    main_puts(user_buf);
    main_newline();

    plen = prompt_input_zone("Password (ENTER=skip): ", pass_buf, 31, 1);
    if (connection_state < STATE_FTP_CONNECTED) return;

    if (plen == 0 && ulen == 0) {
        safe_copy(pass_buf, "zx@zx.net", sizeof(pass_buf));
    }
    current_attr = ATTR_USER;
    if (plen > 0) {
        uint8_t pi;
        for (pi = 0; pi < plen; pi++) main_putc('*');
    } else {
        main_puts("****");
    }
    main_newline();

    cmd_user(user_buf, pass_buf);
}

// Wait for FTP response code
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

    return 0;
}

// Fast FTP code wait
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
                    if (p[0] == code3[0] && p[1] == code3[1] && p[2] == code3[2]) {
                        if (p[3] == '-') {
                            continue;
                        }
                        return 1;
                    }
                }
            }
            rx_pos = 0;
        }
        frames++;
    }

    return 0;
}

// Core function for PWD
static void pwd_core(uint8_t silent)
{
    if (!ensure_logged_in()) return;
    uint16_t frames = 0;

    if (!ftp_command("PWD")) return;

    rx_pos = 0;

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

                    safe_copy(ftp_path, start, sizeof(ftp_path));

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

static void cmd_pwd_silent(void)
{
    pwd_core(1);
}

static void cmd_user(const char *user, const char *pass)
{
    if (connection_state < STATE_FTP_CONNECTED) {
        fail(S_NO_CONN);
        return;
    }

    if (connection_state == STATE_LOGGED_IN) {
        fail("Already logged in. Use QUIT first");
        return;
    }

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

    {
        char *p = tx_buffer;
        p = str_append(p, "USER ");
        p = str_append(p, user);
    }
    if (!ftp_command(tx_buffer)) {
        fail("Send USER failed");
        return;
    }

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

    {
        char *p = tx_buffer;
        p = str_append(p, "PASS ");
        p = str_append(p, pass);
    }
    if (!ftp_command(tx_buffer)) {
        fail("Send PASS failed");
        return;
    }

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
    safe_copy(ftp_user, user, sizeof(ftp_user));
    connection_state = STATE_LOGGED_IN;

    safe_copy(ftp_path, "---", sizeof(ftp_path));

    draw_status_bar_real();

    current_attr = ATTR_LOCAL;
    main_print("Logged in!");

    safe_copy(ftp_cmd_buffer, "TYPE I\r\n", sizeof(ftp_cmd_buffer));
    esp_tcp_send(0, ftp_cmd_buffer, strlen(ftp_cmd_buffer));

    wait_for_ftp_code_fast(50, "200");

    main_puts("Getting PWD: ");
    cmd_pwd_silent();

    if (ftp_path[0] && strcmp(ftp_path, "---") != 0) {
        current_attr = ATTR_RESPONSE;
        uint8_t path_len = strlen(ftp_path);
        if (path_len > 51) {
            main_puts("~");
            main_puts(ftp_path + path_len - 50);
        } else {
            main_puts(ftp_path);
        }
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

    char path_dec[64];
    decode_path_escapes(path, path_dec, sizeof(path_dec));

    {
        char *p = tx_buffer;
        p = str_append(p, "CWD ");
        p = str_append(p, path_dec);
    }
    if (!ftp_command(tx_buffer)) return;

    rx_pos = 0;

    while (frames < 250) {
        HALT();

        if (key_edit_down()) {
            fail(S_CANCEL);
            return;
        }

        if (try_read_line()) {
            if (strncmp(rx_line, S_IPD0, 7) == 0) {
                if (strstr(rx_line, "250")) {
                    current_attr = ATTR_RESPONSE;

                    if (ftp_path[0] == '-' || strcmp(ftp_path, "---") == 0) {
                        safe_copy(ftp_path, "/", sizeof(ftp_path));
                    }

                    if (path_dec[0] == '/') {
                        safe_copy(ftp_path, path_dec, sizeof(ftp_path));
                        } else if (strcmp(path_dec, "..") == 0) {
                        char *last_slash = strrchr(ftp_path, '/');
                        if (last_slash && last_slash != ftp_path) {
                            *last_slash = '\0';
                        } else {
                            safe_copy(ftp_path, "/", sizeof(ftp_path));
                        }
                        } else {
                        size_t len = strlen(ftp_path);
                        if (len > 0 && ftp_path[len-1] != '/') {
                            strncat(ftp_path, "/", sizeof(ftp_path) - len - 1);
                        }
                        strncat(ftp_path, path_dec, sizeof(ftp_path) - strlen(ftp_path) - 1);
                    }

                    last_path[0] = 0;
                    draw_status_bar();
                    // Drain any leftover CWD response data before PWD
                    { uint8_t d; for (d = 0; d < 15; d++) { uart_drain_to_buffer(); HALT(); } }
                    rb_flush();
                    rx_pos = 0;
                    cmd_pwd();
                    return;
                }
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
// HELPER FUNCTIONS
// ============================================================================

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
            if (h >= 'a' && h <= 'z') h -= 32;
            if (n >= 'a' && n <= 'z') n -= 32;
            if (h != n) break;
            hp++;
            np++;
        }
        if (!*np) return 1;
    }
    return 0;
}

// ============================================================================
// FILE SYSTEM HELPERS (8.3 COMPLIANCE & COLLISION)
// ============================================================================

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
        jr c, esx_openr_fail2
        pop ix
        ld b, 0x01          ; FMODE_READ
        rst 0x08
        defb 0x9A           ; ESX_FOPEN
        jr c, esx_openr_fail
        ld l, a
        jr esx_openr_done
    esx_openr_fail2:
        pop hl              ; Clean stacked filename
    esx_openr_fail:
        ld l, 255
    esx_openr_done:
        ld h, 0
    __endasm;
}

static void sanitize_filename_83(const char *src, char *dst)
{
    char base[9];
    char ext[5];
    const char *p_ext = NULL;
    const char *p;
    uint8_t i;

    p = src;
    while (*p) {
        if (*p == '.' && p != src) p_ext = p;
        p++;
    }

    i = 0;
    p = src;
    while (*p && p != p_ext && i < 8) {
        char c = *p++;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c == ' ' || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c == '.') c = '_';
        if (c < 32) c = '_';

        base[i++] = c;
    }
    base[i] = 0;

    ext[0] = 0;
    if (p_ext) {
        ext[0] = '.';
        p = p_ext + 1;
        i = 1;
        while (*p && i < 4) {
            char c = *p++;
            if (c >= 'a' && c <= 'z') c -= 32;
            if (c == ' ' || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c == '.') c = '_';
            if (c < 32) c = '_';
            ext[i++] = c;
        }
        ext[i] = 0;
    }

    memcpy(dst, base, strlen(base) + 1);
    strcat(dst, ext);
}

static void ensure_unique_filename(char *dst)
{
    uint8_t h;
    char base[9];
    char ext[5];
    char *p;
    uint8_t len = 0;
    uint8_t i;

    h = esx_fopen_read(dst);
    if (h == 0xFF) return;
    esx_fclose(h);

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

    if (strlen(base) > 6) {
        base[6] = 0;
    }

    for (i = 1; i <= 9; i++) {
        memcpy(dst, base, strlen(base) + 1);
        strcat(dst, "~");
        dst[strlen(dst) + 1] = 0;
        dst[strlen(dst)] = '0' + i;
        strcat(dst, ext);

        h = esx_fopen_read(dst);
        if (h == 0xFF) return;
        esx_fclose(h);
    }
}

// ============================================================================
// DOWNLOAD
// ============================================================================

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
                if (strstr(rx_line, "550") || strstr(rx_line, S_ERROR)) break;
            }
            rx_pos = 0;
        }
        frames++;
    }

    rx_pos = 0;
    return file_size;
}

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
                    strstr(ctrl_buf, S_ERROR) || strstr(ctrl_buf, "Fail")) {

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

    return 0;
}

static uint8_t download_file_core(const char *remote, const char *local, uint8_t b_cur, uint8_t b_tot, uint32_t *out_bytes)
{
    uint32_t received = 0;
    uint32_t file_size = 0;
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
    int16_t c;

    *out_bytes = 0;
    file_buf_pos = 0;
    sanitize_filename_83(local, local_name);
    ensure_unique_filename(local_name);
    drain_mode_normal();
    rx_reset_all();
    progress_current_file[0] = '\0';

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

    draw_progress_bar(local_name, 0, 0);

    file_size = download_request_size(remote);

    if (ftp_passive() == 0) { fail(S_PASV_FAIL); return 0; }
    if (!ftp_open_data()) { fail(S_DATA_FAIL); return 0; }

    handle = esx_fopen_write(local_name);
    if (handle == 0xFF) {
        fail("Cannot create local file");
        ftp_close_data();
        return 0;
    }

    {
        char *p = ftp_cmd_buffer;
        p = str_append(p, "RETR ");
        p = str_append(p, remote);
    }
    if (!ftp_command(ftp_cmd_buffer)) goto get_cleanup;

    debug_enabled = 0;

    if (!download_wait_transfer_start(&ipd_remaining, &in_data, &user_cancel)) {
        if (user_cancel) {
            goto get_cleanup;
        }
        if (handle != 0xFF) esx_fclose(handle);
        esp_tcp_close(1);
        rb_flush();
        return 0;
    }

    draw_progress_bar(local_name, 0, file_size);

    drain_mode_fast();

    // ========================================================================
    // DOWNLOAD LOOP
    // ========================================================================
    while (1) {

        uart_drain_to_buffer();

        if (in_data && ipd_remaining > 0) {
            while (rb_head != rb_tail && ipd_remaining > 0) {
                file_buffer[file_buf_pos++] = ring_buffer[rb_tail];
                rb_tail = (rb_tail + 1) & RING_BUFFER_MASK;
                ipd_remaining--;

                if (file_buf_pos >= 512) {
                    esx_fwrite(handle, file_buffer, file_buf_pos);
                    received += file_buf_pos;
                    file_buf_pos = 0;

                    if (received - last_progress >= 4096) {
                        draw_progress_bar(local_name, received, file_size);
                        last_progress = received;
                        if (key_edit_down()) { user_cancel = 1; break; }
                    }

                    uart_drain_to_buffer();
                }
            }

            if (user_cancel) break;

            if (ipd_remaining == 0 && file_buf_pos > 0) {
                esx_fwrite(handle, file_buffer, file_buf_pos);
                received += file_buf_pos;
                file_buf_pos = 0;
            }

            if (ipd_remaining == 0) { in_data = 0; hdr_pos = 0; }
            silence = 0;
            continue;
        }

        c = rb_pop();

        if (c == -1) {
            silence++;
            if (silence > SILENCE_XLONG) {
                debug_enabled = 1;
                main_print("Timeout (No data)");
                break;
            }
            if ((silence & 0xFF) == 0 && key_edit_down()) {
                user_cancel = 1;
                break;
            }
            continue;
        }

        silence = 0;

        if (c == '\r' || c == '\n') {
            hdr_buf[hdr_pos] = 0;

            if (strstr(hdr_buf, S_CLOSED1)) { download_success = 1; goto get_cleanup; }

            if (strncmp(hdr_buf, "0,CLOSED", 8) == 0) { goto get_cleanup; }

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

// ============================================================================
// UTF-8 TO ASCII
// ============================================================================

static void utf8_to_ascii_inplace(char *s)
{
    char *write = s;
    char *read = s;

    while (*read) {
        uint8_t c = (uint8_t)*read;

        if (c < 128) {
            *write++ = *read++;
        } else if (c == 0xC3) {
            read++;
            if (!*read) { *write++ = '?'; break; }
            uint8_t c2 = (uint8_t)*read;

            if (c2 >= 0xA0 && c2 <= 0xA5) *write++ = 'a';
            else if (c2 == 0xA7) *write++ = 'c';
            else if (c2 >= 0xA8 && c2 <= 0xAB) *write++ = 'e';
            else if (c2 >= 0xAC && c2 <= 0xAF) *write++ = 'i';
            else if (c2 == 0xB1) *write++ = 'n';
            else if (c2 >= 0xB2 && c2 <= 0xB6) *write++ = 'o';
            else if (c2 >= 0xB9 && c2 <= 0xBC) *write++ = 'u';
            else if (c2 >= 0x80 && c2 <= 0x85) *write++ = 'A';
            else if (c2 == 0x91) *write++ = 'N';
            else *write++ = '?';

            read++;
        } else if (c >= 0xC0 && c <= 0xDF) {
            *write++ = '_';
            read++;
            if ((uint8_t)*read >= 0x80 && (uint8_t)*read <= 0xBF) read++;
        } else if (c >= 0xE0 && c <= 0xEF) {
            *write++ = '_';
            read++;
            if ((uint8_t)*read >= 0x80 && (uint8_t)*read <= 0xBF) read++;
            if ((uint8_t)*read >= 0x80 && (uint8_t)*read <= 0xBF) read++;
        } else if (c >= 0xF0 && c <= 0xF7) {
            *write++ = '_';
            read++;
            if ((uint8_t)*read >= 0x80 && (uint8_t)*read <= 0xBF) read++;
            if ((uint8_t)*read >= 0x80 && (uint8_t)*read <= 0xBF) read++;
            if ((uint8_t)*read >= 0x80 && (uint8_t)*read <= 0xBF) read++;
        } else {
            *write++ = '_';
            read++;
        }
    }
    *write = 0;
}

// ============================================================================
// LIST PARSING
// ============================================================================

static uint8_t list_parse_line(const char *line_buf, uint8_t line_pos,
                                uint8_t type_mode, uint32_t min_size, const char *pattern,
                                char *type_out, uint8_t *is_dir, uint32_t *size, char *name_out)
{
    char *p = (char*)line_buf;
    uint8_t col;

    while (*p == ' ') p++;
    if (*p == 0) return 0;

    if (*p == 't' || *p == 'T') {
        if ((p[1] == 'o' || p[1] == 'O') && (p[2] == 't' || p[2] == 'T')) return 0;
    }

    char type = *p;
    *type_out = type;
    *is_dir = (type == 'd' || type == 'l');

    while (*p && *p != ' ') p++; while (*p == ' ') p++; if (*p == 0) return 0;

    for (col = 0; col < 3; col++) {
        while (*p && *p != ' ') p++; while (*p == ' ') p++; if (*p == 0) return 0;
    }

    *size = 0;
    while (*p >= '0' && *p <= '9') { *size = *size * 10 + (*p - '0'); p++; }

    while (*p && *p != ' ') p++; while (*p == ' ') p++; if (*p == 0) return 0;

    for (col = 0; col < 3; col++) {
        while (*p && *p != ' ') p++; while (*p == ' ') p++; if (*p == 0) return 0;
    }

    st_copy_n(name_out, p, 41);

    {
        char *end = name_out + strlen(name_out) - 1;
        while (end >= name_out && (*end == '\r' || *end == '\n' || *end == ' ')) {
            *end = 0;
            end--;
        }
    }

    utf8_to_ascii_inplace(name_out);

    if (strlen(name_out) > 38) {
        name_out[37] = '.';
        name_out[38] = '.';
        name_out[39] = 0;
    }

    if (type_mode == 1 && !*is_dir) return 0;
    if (type_mode == 2 && *is_dir) return 0;
    if (min_size > 0 && *size < min_size) return 0;
    if (pattern[0] && !str_contains(name_out, pattern)) return 0;

    return 1;
}

// ============================================================================
// UNIFIED LIST/SEARCH COMMAND
// ============================================================================

static void cmd_list_core(const char *a1, const char *a2, const char *a3)
{
    if (!ensure_logged_in()) return;
    g_user_cancel = 0;
    drain_mode_fast();

    uint32_t t = 0;
    int16_t c;
    char line_buf[128];
    uint8_t line_pos = 0;
    uint8_t matches = 0;
    uint8_t page_lines = 0;
    uint8_t in_data = 0;
    uint8_t cancelled = 0;
    uint16_t ipd_remaining = 0;
    char hdr_buf[24];
    uint8_t hdr_pos = 0;
    uint8_t header_printed = 0;
    uint8_t list_pause_risky = 0;

    char pattern[32]; pattern[0] = 0;
    uint8_t type_mode = 0;
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
        else safe_copy(pattern, arg, sizeof(pattern));
    }

    current_attr = ATTR_LOCAL;
    {
        char *p = tx_buffer;

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

    uint16_t silence_frames = 0;

    while (silence_frames < SILENCE_SHORT) {
        if ((t & 0x1FF) == 0) {
            if (key_edit_down()) {
                cancelled = 1;
                goto list_done;
            }
        }

        uart_drain_to_buffer();
        c = rb_pop();

        if (c == -1) {
            HALT();
            uart_drain_to_buffer();
            c = rb_pop();
            if (c == -1) {
                silence_frames++;
                t++;
                continue;
            }
        }
        silence_frames = 0;
        t++;

        if (!in_data) {
            if (c == '\r' || c == '\n') {
                hdr_buf[hdr_pos] = 0;

                if (strstr(hdr_buf, S_CLOSED1)) goto list_done;

                if (hdr_pos > 7 && strncmp(hdr_buf, S_IPD1, 7) == 0) {
                    char *p = hdr_buf + 7;
                    ipd_remaining = parse_decimal(&p);
                    if (ipd_remaining > 0) in_data = 1;
                }

                if (hdr_pos > 7 && strncmp(hdr_buf, S_IPD0, 7) == 0) {
                    if (strstr(hdr_buf, "226")) goto list_done;
                    if (strstr(hdr_buf, "550")) goto list_done;
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
            } else {
                hdr_pos = 0;
            }
        } else {
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
                            print_char_line(22, '-');
                            header_printed = 1;
                            page_lines = 1;
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

                        if (page_lines >= LINES_PER_PAGE) {
                            uint8_t saved_attr = current_attr;
                            uint8_t saved_line = main_line;
                            current_attr = ATTR_RESPONSE;
                            main_puts("-- More? EDIT=stop --");
                            drain_mode_normal();
                            {
                                uint16_t idle_frames = 0;
                                while(1) {
                                    HALT();
                                    uart_drain_to_buffer();

                                    if (key_edit_down()) {
                                        clear_line(saved_line, ATTR_MAIN_BG);
                                        main_line = saved_line;
                                        main_col = 0;
                                        cancelled = 1;
                                        goto list_done;
                                    }
                                    if (in_inkey() != 0) break;

                                    if (idle_frames < 65535) idle_frames++;
                                    if (idle_frames >= FRAMES_LIST_PAUSE_RISKY) list_pause_risky = 1;
                                }
                            }
                            // Erase "More?" message and reuse its line
                            clear_line(saved_line, ATTR_MAIN_BG);
                            main_line = saved_line;
                            main_col = 0;
                            current_attr = saved_attr;
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

    rx_pos = 0;
    rx_overflow = 0;

    current_attr = cancelled ? ATTR_ERROR : ATTR_RESPONSE;
    {
        char *p = tx_buffer;
        p = char_append(p, '(');
        p = u16_to_dec(p, matches);
        p = str_append(p, pattern[0] ? " matches" : " items");
        if (cancelled) p = str_append(p, ", incomplete");
        p = char_append(p, ')');
    }
    main_print(tx_buffer);

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
    if (!esxdos_available) {
        fail("No esxDOS - GET not available");
        return;
    }
    if (!ensure_logged_in()) return;
    g_user_cancel = 0;
    status_bar_overwritten = 0;

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

    uint8_t total_success = 0;
    uint32_t total_bytes = 0;

    uint8_t i;
    for (i = 0; i < argc; i++) {
        uint32_t bytes_this_file = 0;

        if (download_file_core(argv[i], argv[i], i + 1, argc, &bytes_this_file)) {
            total_success++;
            total_bytes += bytes_this_file;
        } else {
            if (g_user_cancel) {
                if (argc > 1) {
                    main_print("Batch cancelled by user");
                }
                break;
            }
        }

        {
            uint8_t w;
            for (w = 0; w < 25; w++) {
                uart_drain_to_buffer();
                wait_frames(1);
            }
        }
    }

    current_attr = ATTR_RESPONSE;

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

    progress_current_file[0] = '\0';

    if (status_bar_overwritten) {
        invalidate_status_bar();
        draw_status_bar();
        status_bar_overwritten = 0;
    }
}


// ============================================================================
// DISCONNECT / QUIT
// ============================================================================

static void close_connection_sequence(void)
{
    uint16_t t;

    current_attr = ATTR_LOCAL;
    main_print("Closing connection.");

    safe_copy(ftp_cmd_buffer, S_CMD_QUIT, sizeof(ftp_cmd_buffer));
    esp_tcp_send(0, ftp_cmd_buffer, strlen(ftp_cmd_buffer));

    for (t = 0; t < 25; t++) { uart_drain_to_buffer(); wait_frames(1); }

    uart_send_string(S_AT_CLOSE0);

    for (t = 0; t < 10; t++) { uart_drain_to_buffer(); wait_frames(1); }

    rb_flush();
    rx_pos = 0;

    clear_ftp_state();

    current_attr = ATTR_RESPONSE;
    main_print(S_DISCONN);
    draw_status_bar();
}

static void cmd_quit(void)
{
    current_attr = ATTR_ERROR;
    main_print("Disconnect (Y/N)?");

    while(1) {
        if (ay_uart_ready()) ay_uart_read();

        uint8_t k = in_inkey();

        if (k == 'n' || k == 'N' || k == 7) {
            current_attr = ATTR_LOCAL;
            main_print("Aborted");
            return;
        }
        if (k == 'y' || k == 'Y' || k == 13) {
            break;
        }
        HALT();
    }

    close_connection_sequence();
}
