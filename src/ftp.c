// ============================================================================
// ftp.c - FTP protocol, download, list (transport-independent)
// ============================================================================
// Everything here goes through the seam in include/bitstream_net.h.
// Replies are matched by their code (RFC 959 "ddd " / multi-line "ddd-"),
// never by searching the whole line.
// ============================================================================

#define FTP_CANCEL  1       // ftp_wait_reply pseudo-codes (never real replies)
#define FTP_LOST    2

static const char S_SIZE_FAIL[] = "Size mismatch";

static uint8_t quick_noop_check(uint16_t max_frames) __z88dk_fastcall;

static uint16_t ml_code;            // multi-line reply in progress ("ddd-")
static char *reply_text;            // text of the last reply returned

// "ddd " -> ddd. Continuation lines and text return 0. Inside a multi-line
// reply only the line starting with the same code and a space ends it.
static uint16_t reply_code(const char *p) __z88dk_fastcall
{
    uint16_t code;
    uint8_t d;

    if (p[0] < '1' || p[0] > '5') return 0;
    if (p[1] < '0' || p[1] > '9' || p[2] < '0' || p[2] > '9') return 0;
    if (p[3] && p[3] != ' ' && p[3] != '-') return 0;
    d = (uint8_t)(p[0] - '0');
    code = ((uint16_t)d << 6) + ((uint16_t)d << 5) + ((uint16_t)d << 2);   // *100
    d = (uint8_t)(p[1] - '0');
    code += ((uint16_t)d << 3) + ((uint16_t)d << 1);                     // *10
    code += (uint8_t)(p[2] - '0');

    if (p[3] == '-') {
        if (!ml_code) ml_code = code;
        return 0;
    }
    if (ml_code) {
        if (code != ml_code) return 0;
        ml_code = 0;
    }
    return code;
}

// Wait for the final reply to a command. Outside a transfer, preliminary
// (1xx) and stale transfer replies (226/426 from an earlier LIST/RETR) are
// skipped. Returns the code, 0 on timeout, FTP_CANCEL or FTP_LOST.
static uint16_t ftp_wait_reply(uint16_t max_frames, uint8_t transfer)
{
    char *p;
    uint16_t code;

    while (max_frames--) {
        while ((p = net_ctrl_line()) != NULL) {
            code = reply_code(p);
            if (code < 200) continue;           // none, or preliminary 1xx
            if (!transfer && (code == 226 || code == 426)) continue;
            reply_text = p;
            return code;
        }
        if (net_ctrl_lost()) return FTP_LOST;
        if (key_break_down()) return FTP_CANCEL;
        HALT();
    }
    return 0;
}

static uint8_t ftp_command(const char *cmd) __z88dk_fastcall
{
    uint16_t len = strlen(cmd);

    if (len > (sizeof(ftp_cmd_buffer) - 3)) {
        fail("Command too long");
        return 0;
    }

    st_copy_n(ftp_cmd_buffer, cmd, sizeof(ftp_cmd_buffer) - 2);
    ftp_cmd_buffer[len] = '\r';
    ftp_cmd_buffer[len + 1] = '\n';
    ftp_cmd_buffer[len + 2] = 0;

    net_ctrl_discard();             // stale lines must not answer this command
    ml_code = 0;
    return net_ctrl_send(ftp_cmd_buffer, len + 2);
}

static uint16_t ftp_cmd_reply(const char *cmd, uint16_t max_frames)
{
    if (!ftp_command(cmd)) return 0;
    return ftp_wait_reply(max_frames, 0);
}

// Show a server reply safely: UTF-8/Latin-1 folded to ASCII first, so no
// byte >= 0x80 reaches the BPE expander.
static void print_reply(const char *prefix, char *text)
{
    utf8_to_ascii(text);
    current_attr = ATTR_ERROR;
    main_puts(prefix);
    main_print(text);
}

static void connection_lost(void)
{
    net_ctrl_close();
    clear_ftp_state();
    draw_status_bar();
    fail("Connection lost");
}

// Common failure report for a non-success reply code
static void report_reply(uint16_t code, const char *what)
{
    if (code == FTP_CANCEL) fail(S_CANCEL);
    else if (code == FTP_LOST || code == 421) connection_lost();
    else if (code == 0) { main_puts2(what, ": "); fail("timeout"); }
    else print_reply(S_ERROR_TAG, reply_text);
}

// parse_decimal is in asm/bitstream_asm.asm (declared in comms.c)

// ============================================================================
// PASSIVE MODE
// ============================================================================

static uint8_t is_unroutable(const uint8_t *o)
{
    if (o[0] == 0 || o[0] == 10 || o[0] == 127) return 1;
    if (o[0] == 192 && o[1] == 168) return 1;
    if (o[0] == 172 && o[1] >= 16 && o[1] <= 31) return 1;
    if (o[0] == 169 && o[1] == 254) return 1;
    return 0;
}

static uint16_t ftp_passive(void)
{
    char *p;
    uint8_t i;
    uint8_t octets[6];
    uint16_t v;
    uint16_t code = ftp_cmd_reply("PASV", 250);

    if (code != 227) {
        report_reply(code, "PASV");
        return 0;
    }

    // "227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)" - parentheses optional
    p = reply_text + 3;
    while (*p && (*p < '0' || *p > '9')) p++;
    for (i = 0; i < 6; i++) {
        if (*p < '0' || *p > '9') goto bad;
        v = parse_decimal(&p);
        if (v > 255) goto bad;
        octets[i] = (uint8_t)v;
        if (i < 5) {
            if (*p != ',') goto bad;
            p++;
        }
    }

    data_port = ((uint16_t)octets[4] << 8) | octets[5];
    if (!data_port) goto bad;

    // A server behind NAT (or a container) often advertises an address the
    // client cannot reach. The data server is the control server, so use the
    // host the control connection already reaches (FileZilla does the same).
    if (is_unroutable(octets)) {
        safe_copy(data_ip, ftp_host, sizeof(data_ip));
    } else {
        char *q = data_ip;
        for (i = 0; i < 4; i++) {
            q = u16_to_dec(q, (uint16_t)octets[i]);
            if (i < 3) q = char_append(q, '.');
        }
    }
    return data_port;

bad:
    fail("Bad PASV reply");
    return 0;
}

// ============================================================================
// TRANSFER ENGINE (LIST / RETR)
// ============================================================================

#define XFER_ERR     (-2)   // server error reply (xfer_code / reply_text)
#define XFER_CANCEL  (-3)
#define XFER_TIMEOUT (-4)
#define XFER_SILENCE 500    // frames without data (10 s)

static uint16_t xfer_code;          // last final reply seen during the transfer
static uint16_t xfer_silence;
static uint8_t xfer_spin;           // polls without HALT while a frame is mid-way

// PASV, open the data connection, send the command
static uint8_t ftp_transfer_begin(const char *cmd) __z88dk_fastcall
{
    xfer_code = 0;
    xfer_silence = 0;
    if (!ftp_passive()) return 0;
    if (!net_data_open(data_ip, data_port)) {
        net_data_close();           // a late "1,CONNECT" must not leave link 1 busy
        fail(S_DATA_FAIL);
        return 0;
    }
    if (!ftp_command(cmd)) {
        net_data_close();
        fail("Send failed");
        return 0;
    }
    return 1;
}

// Next block of transfer data. Returns >0 bytes, 0 (nothing yet; one frame
// was waited), NET_EOF, XFER_ERR, XFER_CANCEL or XFER_TIMEOUT.
static int16_t ftp_xfer_read(uint8_t *buf, uint16_t max)
{
    int16_t n = net_data_read(buf, max);
    char *p;
    uint16_t code;

    if (n > 0) {
        xfer_silence = 0;
        return n;
    }
    while ((p = net_ctrl_line()) != NULL) {
        code = reply_code(p);
        if (code >= 400) {
            xfer_code = code;
            reply_text = p;
            return XFER_ERR;
        }
        if (code >= 200) xfer_code = code;
    }
    if (n == NET_EOF) return NET_EOF;
    if (net_ctrl_lost()) {
        xfer_code = FTP_LOST;
        return XFER_ERR;
    }
    if (key_break_down()) return XFER_CANCEL;
    if (net_data_midframe() && ++xfer_spin) return 0;   // bytes due: poll, a HALT costs ~230 B
    xfer_spin = 0;
    HALT();
    if (++xfer_silence > XFER_SILENCE) return XFER_TIMEOUT;
    return 0;
}

// Close the data connection and collect the final reply (226/250 or error).
static uint16_t ftp_transfer_end(void)
{
    net_data_close();
    if (!xfer_code) xfer_code = ftp_wait_reply(150, 1);
    return xfer_code;
}

// Report an aborted transfer. The server text is shown before the data
// connection is closed: closing pumps the transport, which reuses rx_line.
static void ftp_transfer_abort(int16_t why)
{
    uint8_t lost = (why == XFER_ERR) && (xfer_code == FTP_LOST || xfer_code == 421);

    if (why == XFER_ERR && !lost) print_reply(S_ERROR_TAG, reply_text);
    net_data_close();
    if (why == XFER_CANCEL) {
        g_user_cancel = 1;
        fail(S_CANCEL);
    } else if (why == XFER_TIMEOUT) {
        fail("Timeout (no data)");
    } else if (lost) {
        connection_lost();
    }
}

// Outcome of the final reply after EOF: 0 ok, 1 failed (already reported)
static uint8_t ftp_transfer_failed(uint16_t code) __z88dk_fastcall
{
    if (code == FTP_LOST || code == 421) { connection_lost(); return 1; }
    if (code >= 400) { print_reply(S_ERROR_TAG, reply_text); return 1; }
    return 0;
}

// ============================================================================
// COMMAND HANDLERS
// ============================================================================

static void cmd_pwd(void);
static void cmd_cd(const char *path) __z88dk_fastcall;
static void cmd_user(const char *user, const char *pass);
static void interactive_login(void);

static uint8_t ensure_logged_in(void)
{
    if (connection_state >= STATE_FTP_CONNECTED) {
        char *p;
        while ((p = net_ctrl_line()) != NULL) {
            if (reply_code(p) == 421) { connection_lost(); return 0; }
        }
        if (net_ctrl_lost()) { connection_lost(); return 0; }
    }

    if (connection_state == STATE_LOGGED_IN) return 1;

    if (connection_state == STATE_FTP_CONNECTED) fail("Not logged in");
    else fail(S_NO_CONN);
    return 0;
}

// Parse host[:port][/path]
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
        uint32_t p_val = 0;
        while (*p_port >= '0' && *p_port <= '9' && p_val <= 65535UL) {
            p_val = p_val * 10 + (*p_port - '0');
            p_port++;
        }
        if (p_val > 0 && p_val <= 65535UL) port = (uint16_t)p_val;
    }

    *out_host = host;
    if (out_path) *out_path = path;
    return port;
}

static void cmd_open(const char *host, uint16_t port)
{
    uint16_t code;

    if (connection_state == STATE_DISCONNECTED) {
        fail(S_NO_WIFI);
        return;
    }

    if (!confirm_disconnect()) return;

    safe_copy(ftp_path, S_EMPTY, sizeof(ftp_path));
    invalidate_status_bar();
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

    if (!net_ctrl_open(host, port)) {
        net_ctrl_close();
        fail("Connect failed");
        return;
    }

    current_attr = ATTR_LOCAL;
    main_print("Waiting for banner.");
    drain_mode_fast();
    ml_code = 0;
    code = ftp_wait_reply(350, 0);
    drain_mode_normal();

    if (code == 220) {
        safe_copy(ftp_host, host, sizeof(ftp_host));
        safe_copy(ftp_user, S_EMPTY, sizeof(ftp_user));
        connection_state = STATE_FTP_CONNECTED;
        current_attr = ATTR_RESPONSE;
        main_print("Connected!");
        draw_status_bar();
        return;
    }

    net_ctrl_close();
    if (code == FTP_CANCEL) fail(S_CANCEL);
    else if (code == 0) {
        fail("FTP banner timeout");
#ifdef BITSTREAM_DEBUG_RX
        net_debug_dump();
#endif
    }
    else if (code == FTP_LOST) fail("Connection rejected");
    else print_reply("Rejected: ", reply_text);
}

// Interactive login prompt (called after OPEN, not from !CONNECT)
static void interactive_login(void)
{
    static char user_buf[20];
    static char pass_buf[32];
    uint8_t ulen, plen;

    if (connection_state != STATE_FTP_CONNECTED) return;

    ulen = prompt_input_zone("User (default: anonymous)> ", user_buf, 19, 0);
    if (ulen == 0xFF) return;
    if (connection_state < STATE_FTP_CONNECTED) return;

    if (ulen == 0) {
        safe_copy(user_buf, "anonymous", sizeof(user_buf));
    }

    plen = prompt_input_zone("Password (up=show)> ", pass_buf, 31, 1);
    if (plen == 0xFF) return;
    if (connection_state < STATE_FTP_CONNECTED) return;

    if (plen == 0 && ulen == 0) {
        safe_copy(pass_buf, "zx@zx.net", sizeof(pass_buf));
    }

    cmd_user(user_buf, pass_buf);
}

// PWD -> ftp_path. Returns 1 on success.
static uint8_t pwd_core(uint8_t silent) __z88dk_fastcall
{
    char *start, *end;
    uint16_t code;

    if (!ensure_logged_in()) return 0;

    code = ftp_cmd_reply("PWD", 200);
    if (code != 257) {
        if (!silent) report_reply(code, "PWD");
        return 0;
    }

    // 257 "<path>" ... ("" inside the name is an escaped quote)
    start = strchr(reply_text, '"');
    if (start) {
        char *w;
        start++;
        end = w = start;
        while (*end) {
            if (*end == '"') {
                if (end[1] != '"') break;
                end++;
            }
            *w++ = *end++;
        }
        *w = 0;
    } else {
        start = reply_text + 3;
        while (*start == ' ') start++;
        end = start + strlen(start);
        while (end > start && (end[-1] == ' ' || end[-1] == '\r')) *--end = 0;
    }

    utf8_to_ascii(start);               // server bytes never reach the BPE expander
    safe_copy(ftp_path, start, sizeof(ftp_path));

    if (!silent) {
        uint8_t path_len = strlen(ftp_path);
        char *q = tx_buffer;

        current_attr = ATTR_RESPONSE;
        q = str_append(q, "PWD: ");
        if (path_len > SCREEN_COLS - 5) {
            q = char_append(q, '~');
            q = str_append(q, ftp_path + path_len - (SCREEN_COLS - 6));
        } else {
            q = str_append(q, ftp_path);
        }
        main_print(tx_buffer);
    }

    draw_status_bar_real();
    return 1;
}

static void cmd_user(const char *user, const char *pass)
{
    uint16_t code;

    if (connection_state < STATE_FTP_CONNECTED) {
        fail(S_NO_CONN);
        return;
    }
    if (connection_state == STATE_LOGGED_IN) {
        fail("Already logged in");
        return;
    }

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
    code = ftp_cmd_reply(tx_buffer, 250);
    if (code == 230) goto login_success;

    if (code != 331) {
        if (code == 530) fail(S_LOGIN_BAD);
        else report_reply(code, "USER");
        return;
    }

    {
        char *p = tx_buffer;
        p = str_append(p, "PASS ");
        p = str_append(p, pass);
    }
    code = ftp_cmd_reply(tx_buffer, 500);
    if (code != 230 && code != 202) {
        if (code == 530) fail(S_LOGIN_BAD);
        else report_reply(code, "PASS");
        return;
    }

login_success:
    safe_copy(ftp_user, user, sizeof(ftp_user));
    connection_state = STATE_LOGGED_IN;
    safe_copy(ftp_path, S_EMPTY, sizeof(ftp_path));
    draw_status_bar_real();

    current_attr = ATTR_LOCAL;
    main_print("Logged in!");

    ftp_cmd_reply("TYPE I", 100);

    main_puts("Getting PWD: ");
    if (pwd_core(1)) {
        uint8_t path_len = strlen(ftp_path);
        current_attr = ATTR_RESPONSE;
        if (path_len > 51) {
            main_puts2("~", ftp_path + path_len - 50);
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

static uint8_t hex_to_nibble(char c) __z88dk_fastcall
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

static void cmd_cd(const char *path) __z88dk_fastcall
{
    char path_dec[PATH_SIZE];
    uint16_t code;

    if (!ensure_logged_in()) return;

    decode_path_escapes(path, path_dec, sizeof(path_dec));

    {
        char *p = tx_buffer;
        p = str_append(p, "CWD ");
        p = str_append(p, path_dec);
    }
    code = ftp_cmd_reply(tx_buffer, 250);
    if (code != 250) {
        if (code >= 500 && code < 600) fail("Directory not found");
        else report_reply(code, "CD");
        return;
    }

    // Provisional path; PWD below replaces it with the server's answer
    if (ftp_path[0] == '-') {
        safe_copy(ftp_path, S_SLASH, sizeof(ftp_path));
    }
    if (path_dec[0] == '/') {
        safe_copy(ftp_path, path_dec, sizeof(ftp_path));
    } else if (strcmp(path_dec, "..") == 0) {
        char *last_slash = strrchr(ftp_path, '/');
        if (last_slash && last_slash != ftp_path) *last_slash = '\0';
        else safe_copy(ftp_path, S_SLASH, sizeof(ftp_path));
    } else {
        uint8_t len = (uint8_t)strlen(ftp_path);
        if (len > 0 && ftp_path[len - 1] != '/' && len < sizeof(ftp_path) - 2) {
            ftp_path[len++] = '/';
            ftp_path[len] = '\0';
        }
        st_copy_n(ftp_path + len, path_dec, sizeof(ftp_path) - len);
    }
    utf8_to_ascii(ftp_path);

    invalidate_status_bar();
    draw_status_bar();
    cmd_pwd();
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

static void sanitize_filename_83(const char *src, char *dst)
{
    char base[9];
    char ext[5];
    const char *p_ext = NULL;
    const char *p;
    uint8_t base_len;
    uint8_t ext_len = 0;

    p = src;
    while (*p) {
        if (*p == '.' && p != src) p_ext = p;
        p++;
    }

    base_len = 0;
    p = src;
    while (*p && p != p_ext && base_len < 8) {
        char c = *p++;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c == ' ' || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c == '.') c = '_';
        if (c < 32 || (uint8_t)c >= 127) c = '_';
        base[base_len++] = c;
    }
    if (base_len == 0) base[base_len++] = '_';

    if (p_ext) {
        ext[ext_len++] = '.';
        p = p_ext + 1;
        while (*p && ext_len < sizeof(ext) - 1) {
            char c = *p++;
            if (c >= 'a' && c <= 'z') c -= 32;
            if (c == ' ' || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c == '.') c = '_';
            if (c < 32 || (uint8_t)c >= 127) c = '_';
            ext[ext_len++] = c;
        }
        if (ext_len == 1) ext_len = 0;      // "name." -> "NAME"
    }

    memcpy(dst, base, base_len);
    memcpy(dst + base_len, ext, ext_len);
    dst[base_len + ext_len] = 0;
}

static uint8_t ensure_unique_filename(char *dst) __z88dk_fastcall
{
    char base[9];
    char ext[5];
    char *p;
    uint8_t base_len = 0;
    uint8_t ext_len = 0;
    uint8_t i;

    if (!fs_exists(dst)) return 1;

    p = dst;
    while (*p && *p != '.' && base_len < 8) {
        base[base_len++] = *p++;
    }
    if (*p == '.') {
        while (*p && ext_len < sizeof(ext) - 1) {
            ext[ext_len++] = *p++;
        }
    }
    if (base_len > 6) base_len = 6;

    for (i = 1; i <= 9; i++) {
        memcpy(dst, base, base_len);
        dst[base_len] = '~';
        dst[base_len + 1] = '0' + i;
        memcpy(dst + base_len + 2, ext, ext_len);
        dst[base_len + 2 + ext_len] = 0;
        if (!fs_exists(dst)) return 1;
    }
    return 0;
}

// ============================================================================
// DOWNLOAD
// ============================================================================

// SIZE -> bytes, 0 if unknown
static uint32_t download_request_size(const char *remote)
{
    uint32_t file_size = 0;
    char *ps;
    char *p = tx_buffer;

    p = str_append(p, "SIZE ");
    p = str_append(p, remote);

    if (ftp_cmd_reply(tx_buffer, 100) != 213) return 0;

    ps = reply_text + 3;
    while (*ps == ' ') ps++;
    while (*ps >= '0' && *ps <= '9') {
        file_size = file_size * 10 + (*ps - '0');
        ps++;
    }
    return file_size;
}

static uint8_t download_file_core(const char *remote, const char *local, uint8_t b_cur, uint8_t b_tot, uint32_t *out_bytes)
{
    uint32_t received = 0;
    uint32_t file_size;
    uint32_t last_progress = 0;
    uint8_t handle;
    char local_name[16];
    int16_t n = 0;
    uint16_t code;
    const char *err = NULL;

    *out_bytes = 0;
    file_buf_pos = 0;
    sanitize_filename_83(local, local_name);
    if (!ensure_unique_filename(local_name)) {
        fail("Too many duplicates");
        return 0;
    }
    drain_mode_normal();
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

    {
        char *p = tx_buffer;
        p = str_append(p, "RETR ");
        p = str_append(p, remote);
    }
    // Create the file before RETR: an esxDOS create (directory scan + new
    // entry) must not stall the UART while data is already streaming. A
    // refused or failed transfer unlinks it below.
    handle = fs_create(local_name);
    if (handle == FS_BAD) {
        fail("Cannot create local file");
        return 0;
    }
    if (!ftp_transfer_begin(tx_buffer)) {
        fs_close(handle);
        fs_remove(local_name);
        return 0;
    }

    draw_progress_bar(local_name, 0, file_size);
    drain_mode_fast();

    // ========================================================================
    // DOWNLOAD LOOP
    // ========================================================================
    while (1) {
        n = ftp_xfer_read(file_buffer + file_buf_pos, sizeof(file_buffer) - file_buf_pos);
        if (n == 0) continue;
        if (n < 0) break;

        file_buf_pos += (uint16_t)n;
        if (file_buf_pos == sizeof(file_buffer)) {
            if (fs_write(handle, file_buffer, file_buf_pos) != file_buf_pos) {
                err = "Write error"; break;
            }
            received += file_buf_pos;
            file_buf_pos = 0;

            if (received - last_progress >= 4096) {
                draw_progress_bar(local_name, received, file_size);
                last_progress = received;
                if (key_break_down()) { n = XFER_CANCEL; break; }
            }
        }
    }
    drain_mode_normal();

    if (n == NET_EOF && !err) {
        if (file_buf_pos) {
            if (fs_write(handle, file_buffer, file_buf_pos) != file_buf_pos) err = "Write error";
            else received += file_buf_pos;
        }
        code = ftp_transfer_end();
        if (!err) {
            if (ftp_transfer_failed(code)) err = "";
            else if (file_size && received != file_size) err = S_SIZE_FAIL;
        }
    } else if (!err) {
        ftp_transfer_abort(n);
        err = "";
    } else {
        net_data_close();
    }

    fs_close(handle);
    if (!err && !fs_commit(local_name)) err = "Commit failed";
#ifdef BITSTREAM_SELFTEST
    if (err == S_SIZE_FAIL) { fs_commit(local_name); fail(S_SIZE_FAIL); return 0; }  // keep for analysis
#endif
    if (err) fs_remove(local_name);

    if (err) {
        if (err[0]) fail(err);
        return 0;
    }

    draw_progress_bar(local_name, received, file_size ? file_size : received);
    current_attr = ATTR_RESPONSE;
    {
        char size_buf[12];
        char *p = tx_buffer;
        format_size(received, size_buf);
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

// ============================================================================
// LIST PARSING
// ============================================================================

static uint8_t list_parse_line(const char *line_buf,
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

    utf8_to_ascii(name_out);

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
// Data is read in chunks into file_buffer[0..255]; the line under assembly
// lives in file_buffer[256..383] (file_buffer is idle during LIST), which
// keeps 128 bytes off the stack.

#define LIST_CHUNK  256
#define LIST_LINE   (file_buffer + LIST_CHUNK)
#define LIST_LINE_MAX 127

static uint8_t list_line_pos;
static uint8_t list_page_lines;
static uint16_t list_matches;
static uint8_t list_header_printed;
static uint8_t list_pause_risky;

// Emit one assembled LIST line. Returns 0 if the user stopped the listing.
static uint8_t list_emit(uint8_t type_mode, uint32_t min_size, const char *pattern)
{
    uint8_t is_dir;
    uint32_t size;
    char name[41];
    char type;
    char size_str[16];

    LIST_LINE[list_line_pos] = 0;
    if (list_line_pos <= 10) return 1;
    if (!list_parse_line((const char *)LIST_LINE, type_mode, min_size, pattern,
                         &type, &is_dir, &size, name)) return 1;

    if (!list_header_printed) {
        current_attr = ATTR_RESPONSE;
        main_print(S_LIST_HDR);
        print_char_line(22, '-');
        list_header_printed = 1;
        list_page_lines = 1;
    }

    format_size(size, size_str);
    current_attr = is_dir ? ATTR_USER : ATTR_LOCAL;
    {
        char *q = tx_buffer;
        uint8_t slen;
        q = char_append(q, type);
        q = char_append(q, ' ');
        slen = strlen(size_str);
        while (slen < 9) { q = char_append(q, ' '); slen++; }
        q = str_append(q, size_str);
        q = char_append(q, ' ');
        q = str_append(q, name);
    }
    main_print(tx_buffer);
    list_matches++;
    list_page_lines++;

    if (list_page_lines >= LINES_PER_PAGE) {
        uint8_t saved_attr = current_attr;
        uint8_t saved_line = main_line;
        uint16_t idle_frames = 0;

        current_attr = ATTR_RESPONSE;
        main_puts("-- More? BREAK=stop --");
        drain_mode_normal();
        while (1) {
            HALT();
            net_poll();
            if (key_break_down()) {
                clear_line(saved_line, ATTR_MAIN_BG);
                main_line = saved_line;
                main_col = 0;
                return 0;
            }
            if (key_scan() != 0) break;
            if (idle_frames < 65535) idle_frames++;
            if (idle_frames >= FRAMES_LIST_PAUSE_RISKY) list_pause_risky = 1;
        }
        clear_line(saved_line, ATTR_MAIN_BG);
        main_line = saved_line;
        main_col = 0;
        current_attr = saved_attr;
        drain_mode_fast();
        list_page_lines = 0;
    }
    return 1;
}

// Feed raw LIST bytes to the line assembler. Returns 0 if the user stopped.
static uint8_t list_feed(const uint8_t *b, int16_t n, uint8_t type_mode,
                         uint32_t min_size, const char *pattern)
{
    while (n--) {
        uint8_t c = *b++;
        if (c == '\n') {
            if (!list_emit(type_mode, min_size, pattern)) return 0;
            list_line_pos = 0;
        } else if (c >= 32 && list_line_pos < LIST_LINE_MAX) {
            LIST_LINE[list_line_pos++] = c;
        }
    }
    return 1;
}

#ifndef BITSTREAM_SPECTRANEXT
// The ESP pushes data with no flow control: while the pager waits for a key
// the UART overflows and the +IPD framing is lost (long listings stalled
// after a few pages on hardware). The listing is therefore received into a
// temporary file first and paged from there. Spectranext sockets have TCP
// backpressure and stream directly.
static const char S_LIST_TMP[] = "BSLIST.TMP";

// 1: spool complete (data connection closed, final reply not read yet);
// 0: failed and reported (temp file removed); 2: no storage, stream instead.
static uint8_t list_spool(void)
{
    uint8_t handle;
    uint16_t pos = 0;
    int16_t n;
    uint8_t werr = 0;

    if (!fs_available) return 2;
    handle = fs_create(S_LIST_TMP);
    if (handle == FS_BAD) return 2;
    if (!ftp_transfer_begin("LIST")) {
        fs_close(handle);
        fs_remove(S_LIST_TMP);
        return 0;
    }
    drain_mode_fast();
    while (1) {
        n = ftp_xfer_read(file_buffer + pos, sizeof(file_buffer) - pos);
        if (n == 0) continue;
        if (n < 0) break;
        pos += (uint16_t)n;
        if (pos == sizeof(file_buffer)) {
            if (fs_write(handle, file_buffer, pos) != pos) { werr = 1; break; }
            pos = 0;
        }
    }
    drain_mode_normal();
    if (!werr && n == NET_EOF && pos && fs_write(handle, file_buffer, pos) != pos) werr = 1;
    fs_close(handle);
    if (!werr && n == NET_EOF) return 1;
    fs_remove(S_LIST_TMP);
    if (werr) {
        net_data_close();
        fail("Write error");
    } else {
        ftp_transfer_abort(n);
    }
    return 0;
}
#endif

static void cmd_list_core(const char *a1, const char *a2, const char *a3)
{
    int16_t n;
    uint8_t i;
    uint8_t stopped = 0;
    uint8_t failed = 0;
    char pattern[32];
    uint8_t type_mode = 0;
    uint32_t min_size = 0;
    const char *args[3];

    if (!ensure_logged_in()) return;
    g_user_cancel = 0;
    pattern[0] = 0;

    args[0] = a1; args[1] = a2; args[2] = a3;
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

    list_line_pos = 0;
    list_page_lines = 0;
    list_matches = 0;
    list_header_printed = 0;
    list_pause_risky = 0;

#ifndef BITSTREAM_SPECTRANEXT
    {
        uint8_t r = list_spool();
        if (r == 1) {
            uint8_t handle;
            uint16_t code = ftp_transfer_end();

            failed = ftp_transfer_failed(code);
            handle = fs_open_read(S_LIST_TMP);
            if (handle != FS_BAD) {
                uint16_t got;
                while ((got = fs_read(handle, file_buffer, LIST_CHUNK)) != 0) {
                    if (!list_feed(file_buffer, (int16_t)got, type_mode, min_size, pattern)) {
                        stopped = 1;
                        break;
                    }
                }
                fs_close(handle);
                if (!stopped && list_line_pos) list_emit(type_mode, min_size, pattern);
            } else {
                failed = 1;
            }
            fs_remove(S_LIST_TMP);
            if (stopped) {
                g_user_cancel = 1;
                fail(S_CANCEL);
                failed = 1;
            }
            goto list_done;
        }
        if (r == 0) {
            failed = 1;
            goto list_done;
        }
    }
#endif

    if (!ftp_transfer_begin("LIST")) return;
    drain_mode_fast();

    while (1) {
        n = ftp_xfer_read(file_buffer, LIST_CHUNK);
        if (n == 0) continue;
        if (n < 0) break;
        if (!list_feed(file_buffer, n, type_mode, min_size, pattern)) { stopped = 1; break; }
    }
    drain_mode_normal();

    if (stopped) {
        net_data_close();
        g_user_cancel = 1;
        fail(S_CANCEL);
        failed = 1;
    } else if (n == NET_EOF) {
        if (list_line_pos) list_emit(type_mode, min_size, pattern);   // unterminated last line
        failed = ftp_transfer_failed(ftp_transfer_end());
    } else {
        ftp_transfer_abort(n);
        failed = 1;
    }

#ifndef BITSTREAM_SPECTRANEXT
list_done:
#endif
    current_attr = failed ? ATTR_ERROR : ATTR_RESPONSE;
    {
        char *p = tx_buffer;
        p = char_append(p, '(');
        p = u16_to_dec(p, list_matches);
        p = str_append(p, pattern[0] ? " matches" : " items");
        if (failed) p = str_append(p, ", incomplete");
        p = char_append(p, ')');
    }
    main_print(tx_buffer);

    if (list_pause_risky && !stopped && connection_state >= STATE_FTP_CONNECTED) {
        if (!quick_noop_check(FRAMES_NOOP_QUICK_TIMEOUT)) connection_lost();
    }
}

static void cmd_get(char *args) __z88dk_fastcall
{
    #define MAX_BATCH 10
    char *argv[MAX_BATCH];
    uint8_t argc = 0;
    uint8_t total_success = 0;
    uint32_t total_bytes = 0;
    uint8_t i;
    char *p = args;

    if (!fs_available) {
#ifdef BITSTREAM_SPECTRANEXT
        fail("No storage");
#else
        fail("No esxDOS");
#endif
        return;
    }
    if (!ensure_logged_in()) return;
    g_user_cancel = 0;
    status_bar_overwritten = 0;

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

    for (i = 0; i < argc; i++) {
        uint32_t bytes_this_file = 0;

        if (download_file_core(argv[i], argv[i], i + 1, argc, &bytes_this_file)) {
            total_success++;
            total_bytes += bytes_this_file;
        } else if (g_user_cancel || connection_state < STATE_LOGGED_IN) {
            break;
        }
    }

    current_attr = ATTR_RESPONSE;

    if (argc > 1) {
        char bytes_buf[16];
        format_size(total_bytes, bytes_buf);

        p = tx_buffer;
        p = u16_to_dec(p, total_success);
        p = str_append(p, " files downloaded (Total ");
        p = str_append(p, bytes_buf);
        p = char_append(p, ')');
        main_print(tx_buffer);
    }

    progress_current_file[0] = '\0';

    if (status_bar_overwritten) {
        invalidate_status_bar_full();
        draw_status_bar();
        status_bar_overwritten = 0;
    }
}

// ============================================================================
// QUICK CONTROL-CHANNEL PROBE
// ============================================================================

static uint8_t quick_noop_check(uint16_t max_frames) __z88dk_fastcall
{
    uint16_t code;

    if (connection_state < STATE_FTP_CONNECTED) return 0;
    code = ftp_cmd_reply("NOOP", max_frames);
    return code >= 200 && code < 300;
}

// ============================================================================
// DISCONNECT / QUIT
// ============================================================================

static void close_connection_sequence(void)
{
    current_attr = ATTR_LOCAL;
    main_print("Closing connection.");

    if (ftp_command("QUIT")) ftp_wait_reply(50, 0);
    net_ctrl_close();

    clear_ftp_state();

    current_attr = ATTR_RESPONSE;
    main_print(S_DISCONN);
    draw_status_bar();
}

static void cmd_quit(void)
{
    current_attr = ATTR_ERROR;
    main_print("Disconnect (Y/N)?");

    while (1) {
        uint8_t k;
        net_poll();
        k = key_scan();

        if (k == 'n' || k == 'N' || key_break_down()) {
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
