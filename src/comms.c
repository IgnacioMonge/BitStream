// ============================================================================
// comms.c - Ring buffer, UART helpers, ESP layer, TCP
// ============================================================================

// ============================================================================
// RING BUFFER
// ============================================================================
uint8_t ring_buffer[RING_BUFFER_SIZE];
uint16_t rb_head;
uint16_t rb_tail;

// Line parser state
char rx_line[128];
uint8_t rx_pos;
uint8_t rx_overflow;

uint8_t uart_drain_limit = DRAIN_NORMAL;

static void drain_mode_fast(void) { uart_drain_limit = DRAIN_FAST; }
static void drain_mode_normal(void) { uart_drain_limit = DRAIN_NORMAL; }

#define RB_FULL() (((rb_head + 1) & RING_BUFFER_MASK) == rb_tail)

// uart_drain_to_buffer is in asm/bitstream_asm.asm
extern void uart_drain_to_buffer(void);

// rb_pop is in asm/bitstream_asm.asm
extern int16_t rb_pop(void);

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

// ============================================================================
// UART LOW LEVEL
// ============================================================================

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

static void rx_reset_all(void)
{
    uart_flush_rx();
    rb_head = rb_tail = 0;
    rx_pos = 0;
    rx_overflow = 0;
}

static void uart_flush_hard(void)
{
    uint8_t i;
    for (i = 0; i < 2; i++) HALT();
    uart_flush_rx();
    HALT();
    uart_flush_rx();
}

// uart_send_string is in asm/bitstream_asm.asm (__z88dk_fastcall)
extern void uart_send_string(const char *s) __z88dk_fastcall;

static void wait_frames(uint16_t frames) __z88dk_fastcall
{
    while (frames--) HALT();
}

static void wait_drain(uint16_t frames) __z88dk_fastcall
{
    while (frames--) {
        HALT();
        uart_drain_to_buffer();
    }
}

static void esp_send_at(const char *cmd) __z88dk_fastcall
{
    uart_send_string(cmd);
    uart_send_string(S_CRLF);
}

// try_read_line_nodrain is in asm/bitstream_asm.asm
extern uint8_t try_read_line_nodrain(void);

// Convenience wrapper: drain UART then try to read a line.
static uint8_t try_read_line(void)
{
    uart_drain_to_buffer();
    return try_read_line_nodrain();
}

// Unified response wait function
static uint8_t wait_for_string(const char *expected, uint16_t max_frames)
{
    uint16_t frames = 0;

    rx_pos = 0;
    rx_overflow = 0;

    while (frames < max_frames) {
        HALT();

        if (key_break_down()) {
            return 0;
        }

        uart_drain_to_buffer();

        if (try_read_line()) {
            if (strstr(rx_line, "CONNECT FAIL") != NULL) return 0;
            if (strstr(rx_line, "DNS Fail") != NULL) return 0;
            if (rx_line[0] == 'E' && rx_line[1] == 'R' && rx_line[2] == 'R') return 0;
            if (rx_line[0] == 'F' && rx_line[1] == 'A' && rx_line[2] == 'I') return 0;

            if (expected != NULL) {
                if (strstr(rx_line, expected) != NULL) return 1;
            } else if (rx_line[0] == 'O' && rx_line[1] == 'K') {
                return 1;
            }

            rx_pos = 0;
        }

        frames++;
    }

    return 0;
}

#define wait_for_response(max_frames) wait_for_string(NULL, max_frames)

// ============================================================================
// DISCONNECT DETECTION
// ============================================================================

static uint8_t check_disconnect_message(void)
{
    if (strncmp(rx_line, S_0CLOSED, 8) == 0) {
        return 1;
    }
   if (strncmp(rx_line, S_IPD0, 7) == 0) {
        char *payload = strchr(rx_line, ':');
        if (payload && strncmp(payload + 1, "421", 3) == 0) {
            return 2;
        }
    }
    return 0;
}

// ============================================================================
// ESP INITIALIZATION
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

        timeout = 0;

        while (timeout < FRAMES_1S) {
            uart_drain_to_buffer();

            if (try_read_line()) {
                if (strcmp(rx_line, S_OK) == 0) return 1;
                if (strstr(rx_line, S_ERROR) != NULL) return 1;
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
    uint8_t i;

    while (ay_uart_ready()) ay_uart_read();

    uart_send_string(S_ATE0);
    for (i=0; i<10; i++) { HALT(); uart_drain_to_buffer(); }
    rb_head = rb_tail = 0;

    uart_send_string(S_AT_CIPMUX);
    for (i=0; i<10; i++) { HALT(); uart_drain_to_buffer(); }
    rb_head = rb_tail = 0;
}

static void status_result(const char *msg, uint8_t attr)
{
    main_puts(" ");
    current_attr = attr;
    main_print(msg);
}

static void full_initialization_sequence(void)
{
    uint8_t i;

    current_attr = ATTR_LOCAL;
    main_print("Full initialization.");

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

    uint8_t wifi_result = check_wifi_connection();
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
    draw_status_bar();
}

static void smart_init(void)
{
    uint8_t i;
    uint16_t frames;

    current_attr = ATTR_LOCAL;
    main_puts("Initializing.");

    ay_uart_init();

    for (i = 0; i < 10; i++) HALT();

    uart_flush_rx();

    uart_send_string("+++");
    for (i = 0; i < 55; i++) HALT();
    uart_flush_rx();

    uart_send_string(S_ATE0);
    for (i = 0; i < 5; i++) HALT();
    uart_flush_rx();

    uart_send_string("AT+CIPSERVER=0\r\n");
    for (i = 0; i < 5; i++) HALT();
    uart_flush_rx();

    uart_send_string("AT+CIPCLOSE=5\r\n");
    for (i = 0; i < 5; i++) HALT();
    uart_flush_rx();

    uart_send_string(S_AT_CIPMUX);
    for (i = 0; i < 5; i++) HALT();
    uart_flush_rx();

    uart_send_string(S_AT);

    rx_pos = 0;

    for (frames = 0; frames < 150; frames++) {
        HALT();
        uart_drain_to_buffer();

        if (try_read_line()) {
            if (rx_line[0] == 'O' && rx_line[1] == 'K') {
                status_result(S_OK, ATTR_RESPONSE);
                goto esp_ok;
            }
            rx_pos = 0;
        }
    }

    status_result("FAIL", ATTR_ERROR);
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

    draw_status_bar();
}

// ============================================================================
// DISCONNECT CONFIRMATION HELPER
// ============================================================================

// Forward declarations (needed by confirm_disconnect)
static void esp_tcp_close(uint8_t sock) __z88dk_fastcall;
static uint8_t esp_tcp_send(uint8_t sock, const char *data, uint16_t len);
static uint8_t quick_noop_check(uint16_t max_frames) __z88dk_fastcall;

static uint8_t confirm_disconnect(void)
{
    if (connection_state < STATE_FTP_CONNECTED) {
        return 1;
    }

    fail("Already connected. Disconnect? (Y/N)");
    while(1) {
        if (ay_uart_ready()) ay_uart_read();

        uint8_t k = in_inkey();
        if (k == 'n' || k == 'N' || key_break_down()) {
            current_attr = ATTR_LOCAL;
            main_print(S_CANCEL);
            return 0;
        }
        if (k == 'y' || k == 'Y' || k == 13) break;
        HALT();
    }

    close_connection_sequence();

    draw_status_bar_real();

    return 1;
}

// ============================================================================
// ESP TCP LAYER
// ============================================================================

static uint8_t esp_tcp_connect(uint8_t sock, const char *host, uint16_t port)
{
    uint8_t result;

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
    esp_send_at(tx_buffer);
    result = wait_for_string(S_CONNECT, 500);

    return result;
}

static void esp_tcp_close(uint8_t sock) __z88dk_fastcall
{
    {
        char *p = tx_buffer;
        p = str_append(p, "AT+CIPCLOSE=");
        p = u16_to_dec(p, (uint16_t)sock);
    }
    esp_send_at(tx_buffer);
    wait_for_response(100);
}

static uint8_t esp_tcp_send(uint8_t sock, const char *data, uint16_t len)
{
    uint16_t frames;
    int16_t c;

    rx_pos = 0;

    {
        char *p = tx_buffer;
        p = str_append(p, "AT+CIPSEND=");
        p = u16_to_dec(p, (uint16_t)sock);
        p = char_append(p, ',');
        p = u16_to_dec(p, len);
    }
    esp_send_at(tx_buffer);

    wait_frames(2);
    uart_drain_to_buffer();

    frames = 0;
    while (frames < 150) {
        HALT();

        if (key_break_down()) {
            uart_flush_rx();
            return 0;
        }

        uart_drain_to_buffer();
        while ((c = rb_pop()) != -1) {
            if (c == '>') goto send_data;

            if (c == '\n') {
                rx_line[rx_pos] = 0;
                if (strstr(rx_line, S_ERROR) ||
                    strstr(rx_line, "link is not") ||
                    strstr(rx_line, S_CLOSED)) {
                    uart_flush_rx();
                    return 0;
                }
                rx_pos = 0;
            } else if (c != '\r' && rx_pos < 120) {
                rx_line[rx_pos++] = (char)c;
            }
        }
        frames++;
    }
    // Timeout or error waiting for '>': flush to clean ESP state
    uart_flush_rx();
    return 0;

send_data:
    ay_uart_send_block((void *)data, len);

    wait_frames(2);

    return 1;
}

// ============================================================================
// QUICK CONTROL-CHANNEL PROBE (LOW COST)
// ============================================================================
static uint8_t quick_noop_check(uint16_t max_frames) __z88dk_fastcall
{
    uint16_t frames = 0;
    char *payload;

    if (connection_state < STATE_FTP_CONNECTED) {
        return 0;
    }

    if (!esp_tcp_send(0, "NOOP\r\n", 6)) {
        return 0;
    }

    while (frames < max_frames) {
        HALT();
        uart_drain_to_buffer();
        if (try_read_line()) {
            if (rx_line[0] == '2' && rx_line[1] >= '0' && rx_line[1] <= '9' && rx_line[2] >= '0' && rx_line[2] <= '9') {
                return 1;
            }
            if (strncmp(rx_line, S_IPD0, 7) == 0) {
                payload = strchr(rx_line, ':');
                if (payload &&
                    payload[1] == '2' &&
                    payload[2] >= '0' && payload[2] <= '9' &&
                    payload[3] >= '0' && payload[3] <= '9') {
                    return 1;
                }
            }
            if (strstr(rx_line, S_CLOSED1)) {
                return 0;
            }
        }
        frames++;
    }

    return 0;
}
