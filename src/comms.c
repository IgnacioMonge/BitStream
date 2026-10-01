// ============================================================================
// comms.c - Ring buffer, line state and timing helpers shared by every
//           transport backend (net_esp.c / net_spectranext.c)
// ============================================================================

// ============================================================================
// RING BUFFER
// ============================================================================
// Classic: the UART drain fills it with the raw ESP stream.
// Spectranext: the control socket is received into it.
uint8_t ring_buffer[RING_BUFFER_SIZE];
uint16_t rb_head;
uint16_t rb_tail;

// Control-reply line under assembly / last complete line
char rx_line[128];
uint8_t rx_pos;
uint8_t rx_overflow;

// TX fail-stop latch (set by the divMMC UART driver, cleared by ay_uart_init)
uint8_t uart_tx_failed;

uint8_t uart_drain_limit = DRAIN_NORMAL;

static void drain_mode_fast(void) { uart_drain_limit = DRAIN_FAST; }
static void drain_mode_normal(void) { uart_drain_limit = DRAIN_NORMAL; }

// rb_pop / rb_read_block / try_read_line_nodrain are in asm/bitstream_asm.asm
extern int16_t rb_pop(void);
extern uint16_t rb_read_block(uint8_t *dst, uint16_t max) __z88dk_callee;
extern uint8_t try_read_line_nodrain(void);

static void wait_frames(uint16_t frames) __z88dk_fastcall
{
    while (frames--) HALT();
}

// HALT while keeping the transport flowing
static void wait_poll(uint16_t frames) __z88dk_fastcall
{
    while (frames--) {
        HALT();
        net_poll();
    }
}

// ============================================================================
// DISCONNECT CONFIRMATION HELPER
// ============================================================================

static uint8_t confirm_disconnect(void)
{
    if (connection_state < STATE_FTP_CONNECTED) {
        return 1;
    }

    fail("Already connected. Disconnect? (Y/N)");
    while (1) {
        uint8_t k;
        net_poll();
        k = in_inkey();
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
