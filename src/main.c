// ============================================================================
// main.c - Screen initialization, main loop
// ============================================================================

// ============================================================================
// SCREEN INITIALIZATION
// ============================================================================

extern void draw_badge_dither(uint8_t count) __z88dk_fastcall;

static void draw_banner(void)
{
    uint8_t *attr;
    uint8_t i;

    clear_line(BANNER_START, ATTR_BANNER);
    print_str64(BANNER_START, 1, "BITSTREAM " APP_VERSION " - FTP Client / " UART_INTERFACE, ATTR_BANNER);

    // Badge: 4 physical cells at cols 28-31
    // Transition: Banner(blue) → Red → Yellow → Green → Blue(banner)
    attr = (uint8_t *)(0x5800 + BANNER_START * 32 + 28);
    attr[0] = PAPER_BLUE | INK_RED | BRIGHT;
    attr[1] = PAPER_RED | INK_YELLOW | BRIGHT;
    attr[2] = PAPER_YELLOW | INK_GREEN | BRIGHT;
    attr[3] = PAPER_GREEN | INK_BLUE | BRIGHT;

    draw_badge_dither(4);
}

static void init_screen(void)
{
    uint8_t i;
    zx_border(INK_BLACK);
    for (i = 0; i < 24; i++) clear_line(i, PAPER_BLACK);

    clear_line(BANNER_START, ATTR_BANNER);
    draw_banner();

    clear_line(1, ATTR_MAIN_BG);

    clear_zone(MAIN_START, MAIN_LINES, ATTR_MAIN_BG);

    clear_line(20, ATTR_MAIN_BG);

    clear_line(STATUS_LINE, ATTR_STATUS);
    clear_zone(INPUT_START, INPUT_LINES, ATTR_INPUT_BG);

    main_line = MAIN_START;
    main_col = 0;

    invalidate_status_bar();

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
    if (connection_state < STATE_FTP_CONNECTED) {
        if (ay_uart_ready()) ay_uart_read();
        return;
    }

    uint8_t prev_limit = uart_drain_limit;
    uart_drain_limit = 16;

    if (try_read_line()) {
        uint8_t disc = check_disconnect_message();
        if (disc) {
            const char *reason;
            if (disc == 1) reason = "Remote host closed socket";
            else if (str_contains(rx_line, "imeout")) reason = "Idle Timeout (421)";
            else reason = "Service Closing (421)";

            current_attr = ATTR_ERROR;
            main_newline();
            {
                char *p = tx_buffer;
                p = str_append(p, "Disconnected: ");
                p = str_append(p, reason);
            }
            main_print(tx_buffer);
            clear_ftp_state();
            uart_send_string(S_AT_CLOSE0);
            draw_status_bar();
            main_newline();
            redraw_input_from(0);
        }
        rx_pos = 0;
    }

    uart_drain_limit = prev_limit;
}

static void print_intro_banner(void)
{
    current_attr = PAPER_BLACK | INK_WHITE | BRIGHT;

    main_print("BitStream " APP_VERSION " - FTP Client / " UART_INTERFACE);
    main_print("(C) 2026 M. Ignacio Monge Garcia");
    print_char_line(32, '-');
}

// ~4 minutes at 50fps = 12000 frames
#define KEEPALIVE_INTERVAL 12000

void main(void)
{
    uint8_t c;
    uint16_t idle_frames = 0;

    init_screen();

    print_intro_banner();

    esxdos_available = detect_esxdos();

    smart_init();

    if (esxdos_available) {
        current_attr = ATTR_RESPONSE;
        main_puts("esxDOS detected");
    } else {
        current_attr = ATTR_ERROR;
        main_puts("No esxDOS");
    }
    main_newline();

    current_attr = ATTR_LOCAL;
    main_print("Type HELP or !HELP. EDIT cancels.");
    main_newline();

    redraw_input_from(0);

    while (1) {
        HALT();

        check_connection_alive();

        if (connection_state >= STATE_FTP_CONNECTED) {
            idle_frames++;
            if (idle_frames >= KEEPALIVE_INTERVAL) {
                idle_frames = 0;
                quick_noop_check(FRAMES_NOOP_QUICK_TIMEOUT);
            }
        }

        {
            static uint8_t prev_caps_mode = 0;
            static uint8_t prev_shift_state = 0;

            check_caps_toggle();

            uint8_t curr_shift_state = key_shift_held();

            if (prev_caps_mode != caps_lock_mode || prev_shift_state != curr_shift_state) {

                prev_caps_mode = caps_lock_mode;
                prev_shift_state = curr_shift_state;

                uint16_t char_abs = cursor_pos + 2;
                uint8_t cur_row = INPUT_START + (char_abs / SCREEN_COLS);
                uint8_t cur_col = char_abs % SCREEN_COLS;
                draw_cursor_underline(cur_row, cur_col);
            }
        }


        c = read_key();

        ui_flush_dirty();

        if (c == 0) continue;

        idle_frames = 0;

        if (c == KEY_UP) {
            history_nav_and_redraw(1);
        }
        else if (c == KEY_DOWN) {
            history_nav_and_redraw(-1);
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
                static char cmd_copy[LINE_BUFFER_SIZE];
                memcpy(cmd_copy, line_buffer, line_len + 1);

                history_add(cmd_copy, line_len);

                current_attr = ATTR_USER;
                main_puts("> ");
                main_puts(cmd_copy);
                main_newline();

                input_clear();

                set_input_busy(1);

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
