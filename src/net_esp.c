// ============================================================================
// net_esp.c - Classic transport: ESP8266 AT firmware over UART (CIPMUX=1)
// ============================================================================
// Link 0 = FTP control, link 1 = PASV data.
//
// The ESP interleaves three things on one byte stream:
//   "+IPD,<link>,<len>:<len payload bytes>"   socket payload
//   "OK", "ERROR", "SEND OK", "n,CONNECT", "n,CLOSED", ...   AT lines
//   "> "                                         CIPSEND prompt
// dm_process() demultiplexes it: link-0 payload is assembled into reply lines
// (rx_line), link-1 payload is left in the ring for net_data_read() to copy
// out in blocks, and AT lines become event flags. Before this layer each
// command parsed "+IPD" framing on its own with strstr() over the whole line,
// which matched reply codes inside the length field, lost replies that were
// not the first line of an IPD frame, and let the CIPSEND prompt wait swallow
// socket payload.
// ============================================================================

static const char S_CRLF[]      = "\r\n";
static const char S_AT_CIPMUX[] = "AT+CIPMUX=1\r\n";
static const char S_ATE0[]      = "ATE0\r\n";
static const char S_AT[]        = "AT\r\n";
static const char S_ERROR[]     = "ERROR";

// ============================================================================
// RAW UART HELPERS (used only before a TCP link exists)
// ============================================================================

static void rb_flush(void)
{
    uint16_t max = 500;
    while (ay_uart_ready() && max > 0) {
        ay_uart_read();
        max--;
    }
    rb_head = rb_tail = 0;
}

static void uart_flush_rx(void)
{
    uint8_t max_wait = 255;
    uint8_t max_bytes = 255;

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
    wait_frames(2);
    uart_flush_rx();
    HALT();
    uart_flush_rx();
}

static void esp_send_at(const char *cmd) __z88dk_fastcall
{
    uart_send_string(cmd);
    uart_send_string(S_CRLF);
}

static uint8_t try_read_line(void)
{
    uart_drain_to_buffer();
    return try_read_line_nodrain();
}

// Raw line wait used by the AT bring-up sequence
static uint8_t wait_for_string(const char *expected, uint16_t max_frames)
{
    uint16_t frames = 0;

    rx_pos = 0;
    rx_overflow = 0;

    while (frames < max_frames) {
        HALT();
        if (key_break_down()) return 0;

        if (try_read_line()) {
            if (strstr(rx_line, "FAIL") != NULL) return 0;
            if (rx_line[0] == 'E' && rx_line[1] == 'R' && rx_line[2] == 'R') return 0;
            if (strstr(rx_line, expected) != NULL) return 1;
        }
        frames++;
    }
    return 0;
}

// ============================================================================
// ESP BRING-UP
// ============================================================================

static uint8_t probe_esp(void)
{
    uint8_t tries;
    uint16_t timeout;

    for (tries = 0; tries < 3; tries++) {
        rb_head = rb_tail = 0;
        rx_pos = 0;
        uart_flush_hard();

        uart_send_string(S_AT);

        for (timeout = 0; timeout < FRAMES_1S; timeout++) {
            if (try_read_line()) {
                if (strcmp(rx_line, S_OK) == 0) return 1;
                if (strstr(rx_line, S_ERROR) != NULL) return 1;
            }
            wait_frames(1);
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
    uint8_t got_ok = 0;

    uart_flush_rx();
    uart_send_string("AT+CIFSR\r\n");

    while (frames < 200 && !found_ip && !got_ok) {
        HALT();

        if (key_break_down()) {
            uart_flush_rx();
            return 2;
        }

        uart_drain_to_buffer();

        while ((c = rb_pop()) != -1) {
            if (c == 'O') {
                int16_t c2 = rb_pop();
                if (c2 == 'K') { got_ok = 1; break; }
                continue;
            }

            if (c >= '1' && c <= '9' && digit_count == 0) {
                uint8_t ip_idx = 0;

                first_digit = c;
                wifi_client_ip[ip_idx++] = (char)c;
                digit_count = 1;
                dot_count = 0;

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

                wifi_client_ip[ip_idx] = 0;

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

    uart_flush_rx();
    return found_ip ? 1 : 0;
}

static void setup_ftp_mode(void)
{
    while (ay_uart_ready()) ay_uart_read();

    uart_send_string(S_ATE0);
    wait_frames(10);
    uart_flush_rx();

    uart_send_string(S_AT_CIPMUX);
    wait_frames(10);
    uart_flush_rx();
    rb_head = rb_tail = 0;
}

static void status_result(const char *msg, uint8_t attr)
{
    main_puts(" ");
    current_attr = attr;
    main_print(msg);
}

static void dm_reset(void);

static void full_initialization_sequence(void)
{
    uint8_t wifi_result;

    current_attr = ATTR_LOCAL;
    main_print("Full initialization.");

    ay_uart_init();             // also clears the TX fail-stop latch
    setup_ftp_mode();

    main_puts("Probing ESP.");

    if (!probe_esp()) {
        status_result("ESP not responding!", ATTR_ERROR);
        connection_state = STATE_DISCONNECTED;
        draw_status_bar();
        return;
    }

    status_result(S_OK, ATTR_RESPONSE);

    current_attr = ATTR_LOCAL;
    main_print(S_CHECKING);

    wifi_result = check_wifi_connection();
    if (wifi_result == 1) {
        current_attr = ATTR_RESPONSE;
        main_print("WiFi connected");
        connection_state = STATE_WIFI_OK;
    } else if (wifi_result == 2) {
        current_attr = ATTR_ERROR;
        main_print(S_CANCEL);
        connection_state = STATE_DISCONNECTED;
    } else {
        current_attr = ATTR_ERROR;
        main_print(S_NO_WIFI);
        connection_state = STATE_DISCONNECTED;
    }
    dm_reset();
    draw_status_bar();
}

static void smart_init(void)
{
    uint16_t frames;

    current_attr = ATTR_LOCAL;
    main_puts("Initializing.");

    ay_uart_init();

    wait_frames(10);
    uart_flush_rx();

    uart_send_string("+++");
    wait_frames(55);
    uart_flush_rx();

    uart_send_string(S_ATE0);
    wait_frames(5);
    uart_flush_rx();

    uart_send_string("AT+CIPSERVER=0\r\n");
    wait_frames(5);
    uart_flush_rx();

    uart_send_string("AT+CIPCLOSE=5\r\n");
    wait_frames(5);
    uart_flush_rx();

    uart_send_string(S_AT_CIPMUX);
    wait_frames(5);
    uart_flush_rx();

    uart_send_string(S_AT);

    rx_pos = 0;

    for (frames = 0; frames < 150; frames++) {
        HALT();
        if (try_read_line()) {
            if (rx_line[0] == 'O' && rx_line[1] == 'K') {
                status_result(S_OK, ATTR_RESPONSE);
                goto esp_ok;
            }
        }
    }

    status_result(uart_tx_failed ? "No UART" : "FAIL", ATTR_ERROR);
    connection_state = STATE_DISCONNECTED;
    draw_status_bar();
    return;

esp_ok:
    current_attr = ATTR_LOCAL;
    main_puts(S_CHECKING);

    uart_flush_rx();
    uart_send_string("AT+CWJAP?\r\n");

    if (wait_for_string("+CWJAP:", 200)) {
        status_result(S_OK, ATTR_RESPONSE);
        check_wifi_connection();
        connection_state = STATE_WIFI_OK;
    } else {
        status_result(S_NO_WIFI, ATTR_ERROR);
        connection_state = STATE_DISCONNECTED;
    }

    dm_reset();
    draw_status_bar();
}

// ============================================================================
// IPD DEMULTIPLEXER
// ============================================================================

#define DM_LINE 0   // between frames: AT lines and "+IPD" headers
#define DM_CTRL 1   // inside a link-0 frame
#define DM_DATA 2   // inside a link-1 frame

#define EV_OK      0x01
#define EV_ERROR   0x02
#define EV_PROMPT  0x04
#define EV_SENT    0x08
#define EV_CONN0   0x10
#define EV_CONN1   0x20
#define EV_CLOSED0 0x40
#define EV_CLOSED1 0x80

static uint8_t  dm_state;
static uint16_t dm_left;        // payload bytes left in the current frame
static uint8_t  dm_hpos;
static char     dm_hdr[24];     // out-of-frame line (truncated, enough to classify)
static uint8_t  dm_discard;     // drop link-1 payload (after net_data_close)
static uint8_t  ev_flags;
static uint8_t  ctrl_ready;     // rx_line holds a complete, unread reply line
static uint8_t  send_pending;   // CIPSEND issued, "SEND OK" not seen yet

static void dm_reset(void)
{
    dm_state = DM_LINE;
    dm_left = 0;
    dm_hpos = 0;
    dm_discard = 0;
    ev_flags = 0;
    ctrl_ready = 0;
    send_pending = 0;
    rx_pos = 0;
    rx_overflow = 0;
}

// Out-of-frame line complete: turn AT responses into events
static void dm_classify(void)
{
    char *h = dm_hdr;

    if ((h[0] == '0' || h[0] == '1') && h[1] == ',') {
        uint8_t s = (uint8_t)(h[0] - '0');
        if (h[2] == 'C' && h[3] == 'L') {                 // n,CLOSED
            ev_flags |= s ? EV_CLOSED1 : EV_CLOSED0;
            return;
        }
        if (h[2] == 'C' && h[3] == 'O') {                 // n,CONNECT[ FAIL]
            if (h[9] == ' ') ev_flags |= EV_ERROR;
            else ev_flags |= s ? EV_CONN1 : EV_CONN0;
            return;
        }
    }
    if (h[0] == 'O' && h[1] == 'K' && h[2] == 0) { ev_flags |= EV_OK; return; }
    if (h[0] == 'S' && h[1] == 'E' && h[5] == 'O') { ev_flags |= EV_SENT; return; }   // SEND OK
    if (strstr(h, S_ERROR) || strstr(h, "FAIL") ||
        (h[0] == 'l' && h[1] == 'i' && h[2] == 'n')) {    // "link is not valid"
        ev_flags |= EV_ERROR;
    }
}

// One link-0 payload byte into the reply-line assembler
static void dm_ctrl_byte(uint8_t c) __z88dk_fastcall
{
    if (c == '\r') return;
    if (c == '\n') {
        if (rx_pos && !rx_overflow) {
            rx_line[rx_pos] = 0;
            ctrl_ready = 1;
        }
        rx_pos = 0;
        rx_overflow = 0;
        return;
    }
    // A new line starts: an unread older line is dropped. The final line of
    // a (multi-line) reply is always the last one, so it is never lost.
    if (rx_pos == 0) ctrl_ready = 0;
    if (rx_pos < sizeof(rx_line) - 1) rx_line[rx_pos++] = (char)c;
    else rx_overflow = 1;
}

// One out-of-frame byte
static void dm_line_byte(uint8_t c) __z88dk_fastcall
{
    if (c == '\r') return;
    if (c == '\n') {
        dm_hdr[dm_hpos] = 0;
        if (dm_hpos) dm_classify();
        dm_hpos = 0;
        return;
    }
    if (dm_hpos == 0) {
        if (c == ' ') return;
        if (c == '>') { ev_flags |= EV_PROMPT; return; }
    }
    if (c == ':' && dm_hpos >= 7 && dm_hdr[0] == '+' && dm_hdr[1] == 'I' &&
        dm_hdr[3] == 'D' && dm_hdr[4] == ',') {
        char *p = dm_hdr + 7;               // "+IPD,n," -> length
        dm_hdr[dm_hpos] = 0;
        dm_left = parse_decimal(&p);
        dm_hpos = 0;
        if (dm_left) dm_state = (dm_hdr[5] == '1') ? DM_DATA : DM_CTRL;
        return;
    }
    if (dm_hpos < sizeof(dm_hdr) - 1) dm_hdr[dm_hpos++] = (char)c;
}

// Drain the UART and consume the ring up to the next link-1 payload byte
// (which stays in the ring for net_data_read).
static void dm_process(void)
{
    int16_t c;

    uart_drain_to_buffer();
    while (1) {
        if (dm_state == DM_DATA) {
            if (!dm_discard) return;
            c = rb_pop();
            if (c < 0) return;
            if (--dm_left == 0) dm_state = DM_LINE;
            continue;
        }
        c = rb_pop();
        if (c < 0) return;
        if (dm_state == DM_CTRL) {
            dm_ctrl_byte((uint8_t)c);
            if (--dm_left == 0) dm_state = DM_LINE;
        } else {
            dm_line_byte((uint8_t)c);
        }
    }
}

// Wait for an AT event. Returns 1 when one of `mask` arrived, 0 on ERROR,
// timeout, BREAK or transport failure.
static uint8_t esp_wait_ev(uint8_t mask, uint16_t frames)
{
    while (frames--) {
        dm_process();
        if (ev_flags & mask) return 1;
        if ((ev_flags & EV_ERROR) || uart_tx_failed) return 0;
        if (key_break_down()) return 0;
        HALT();
    }
    return 0;
}

// Before a new AT command: let an outstanding CIPSEND finish ("busy s...")
static void esp_settle(void)
{
    if (send_pending) {
        send_pending = 0;
        esp_wait_ev(EV_SENT, 25);
    }
    ev_flags &= (uint8_t)~(EV_OK | EV_ERROR | EV_PROMPT | EV_SENT);
}

static uint8_t esp_tcp_connect(uint8_t sock, const char *host, uint16_t port)
{
    uint8_t ev = sock ? EV_CONN1 : EV_CONN0;

    esp_settle();
    ev_flags &= (uint8_t)~(sock ? (EV_CONN1 | EV_CLOSED1) : (EV_CONN0 | EV_CLOSED0));
    {
        char *p = tx_buffer;
        p = str_append(p, "AT+CIPSTART=");
        p = u16_to_dec(p, (uint16_t)sock);
        p = str_append(p, ",\"TCP\",\"");
        p = str_append(p, host);
        p = str_append(p, "\",");
        p = u16_to_dec(p, port);
    }
    esp_send_at(tx_buffer);
    return esp_wait_ev(ev, 500);
}

static void esp_tcp_close(uint8_t sock) __z88dk_fastcall
{
    esp_settle();
    {
        char *p = tx_buffer;
        p = str_append(p, "AT+CIPCLOSE=");
        p = u16_to_dec(p, (uint16_t)sock);
    }
    esp_send_at(tx_buffer);
    esp_wait_ev(EV_OK, 100);
    ev_flags |= sock ? EV_CLOSED1 : EV_CLOSED0;
}

static uint8_t esp_tcp_send(uint8_t sock, const char *data, uint16_t len)
{
    if (uart_tx_failed) return 0;
    esp_settle();
    {
        char *p = tx_buffer;
        p = str_append(p, "AT+CIPSEND=");
        p = u16_to_dec(p, (uint16_t)sock);
        p = char_append(p, ',');
        p = u16_to_dec(p, len);
    }
    esp_send_at(tx_buffer);
    if (!esp_wait_ev(EV_PROMPT, 150)) return 0;

    ay_uart_send_block((void *)data, len);
    send_pending = 1;
    return !uart_tx_failed;
}

// ============================================================================
// SEAM IMPLEMENTATION
// ============================================================================

static void net_boot(void) { smart_init(); }

static void net_reinit(void) { full_initialization_sequence(); }

static void net_poll(void) { dm_process(); }

static uint8_t net_ctrl_open(const char *host, uint16_t port)
{
    uart_flush_rx();
    rb_head = rb_tail = 0;
    dm_reset();
    return esp_tcp_connect(0, host, port);
}

static void net_ctrl_close(void)
{
    esp_tcp_close(0);
    rb_flush();
    dm_reset();
}

static uint8_t net_ctrl_send(const char *data, uint16_t len)
{
    return esp_tcp_send(0, data, len);
}

static char *net_ctrl_line(void)
{
    dm_process();
    if (!ctrl_ready) return NULL;
    ctrl_ready = 0;
    return rx_line;
}

static void net_ctrl_discard(void)
{
    while (net_ctrl_line() != NULL) {}
}

static uint8_t net_ctrl_lost(void)
{
    return (ev_flags & EV_CLOSED0) || uart_tx_failed;
}

static uint8_t net_data_open(const char *ip, uint16_t port)
{
    dm_discard = 0;
    return esp_tcp_connect(1, ip, port);
}

static void net_data_close(void)
{
    dm_discard = 1;                     // skip whatever link-1 payload is left
    if (!(ev_flags & EV_CLOSED1)) esp_tcp_close(1);
    wait_poll(5);
}

static int16_t net_data_read(uint8_t *dst, uint16_t max)
{
    uint16_t n;

    dm_process();
    if (dm_state != DM_DATA || dm_discard) {
        return (ev_flags & EV_CLOSED1) ? NET_EOF : 0;
    }
    if (max > dm_left) max = dm_left;
    n = rb_read_block(dst, max);
    dm_left -= n;
    if (!dm_left) dm_state = DM_LINE;
    return (int16_t)n;
}
