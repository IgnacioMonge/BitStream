// ============================================================================
// main.c - Screen initialization, main loop
// ============================================================================

// ============================================================================
// SCREEN INITIALIZATION
// ============================================================================

extern void draw_badge_dither(uint8_t count) __z88dk_fastcall;
static void draw_banner(void)
{
    // Identical to SpectalkZX default theme banner
    // Row 0: BRIGHT, Row 1: dim (gradient effect)
    clear_line(0, ATTR_BANNER | 0x40);
    clear_line(1, ATTR_BANNER & 0xBF);

    // Double-height banner text across rows 0-1
    print_big_str(0, 0, APP_NAME_UPPER " " APP_VERSION " - FTP Client / " UART_INTERFACE);

    // Dithered badge: 5 cells (cols 27-31), SpectalkZX default theme values
    // Row 0: bridge(blk/blk), blk/red, red/yel, yel/grn, grn/blu
    *(uint8_t *)(0x5800 + 27) = 0x40;  // sb: BRIGHT|PAPER_BLACK|INK_BLACK
    *(uint8_t *)(0x5800 + 28) = 0x42;  // b1: BRIGHT|PAPER_BLACK|INK_RED
    *(uint8_t *)(0x5800 + 29) = 0x56;  // b2: BRIGHT|PAPER_RED|INK_YELLOW
    *(uint8_t *)(0x5800 + 30) = 0x74;  // b3: BRIGHT|PAPER_YELLOW|INK_GREEN
    *(uint8_t *)(0x5800 + 31) = 0x61;  // b4: BRIGHT|PAPER_GREEN|INK_BLUE

    // Row 1: staggered one position (diagonal cascade)
    *(uint8_t *)(0x5820 + 27) = 0x42;  // b1
    *(uint8_t *)(0x5820 + 28) = 0x56;  // b2
    *(uint8_t *)(0x5820 + 29) = 0x74;  // b3
    *(uint8_t *)(0x5820 + 30) = 0x61;  // b4
    *(uint8_t *)(0x5820 + 31) = 0x48;  // end: BRIGHT|PAPER_BLUE|INK_BLACK

    draw_badge_dither(5);

    // 1px separator line below banner (row 2, scanline 0)
    memset((uint8_t *)0x5840, ATTR_MAIN_BG, 32);   // attrs: INK_WHITE on PAPER_BLACK
    memset((uint8_t *)0x4040, 0xFF, 32);            // pixels: white line
}

static void init_screen(void)
{
    uint8_t i;
    zx_border(INK_BLACK);
    for (i = 0; i < 24; i++) clear_line(i, PAPER_BLACK);

    draw_banner();

    clear_zone(MAIN_START, MAIN_LINES, ATTR_MAIN_BG);

    clear_line(19, ATTR_MAIN_BG);              // separator

    // Status bar: double height on rows 20-21
    clear_line(20, ATTR_STATUS);
    clear_line(21, ATTR_STATUS);

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

    main_print(APP_NAME " " APP_VERSION " - FTP Client / " UART_INTERFACE);
    main_print("(C) 2026 M. Ignacio Monge Garcia");
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
    main_print("Type HELP or !HELP. BREAK cancels.");
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
            static uint8_t prev_caps_mode;
            static uint8_t prev_shift_state;

            check_caps_toggle();

            uint8_t curr_shift_state = key_shift_held();

            if (prev_caps_mode != caps_lock_mode || prev_shift_state != curr_shift_state) {

                prev_caps_mode = caps_lock_mode;
                prev_shift_state = curr_shift_state;

                uint16_t char_abs = cursor_pos + input_prompt_len;
                uint8_t cur_row = INPUT_START + (char_abs >> 6);
                uint8_t cur_col = char_abs & 63;
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
                main_puts2("> ", cmd_copy);
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
