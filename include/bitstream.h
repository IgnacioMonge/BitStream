// ============================================================================
// bitstream.h - Master header for BitStreamZX FTP Client
// ============================================================================
#pragma once


#include <arch/zx.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

// --- VERSION ---
#define APP_NAME         "BitStreamZX"
#define APP_NAME_UPPER   "BitStreamZX"
#define APP_VERSION      "1.3.0"

#if defined(BITSTREAM_SPECTRANEXT)
#define UART_INTERFACE   "Spectranext"
#elif defined(BITSTREAM_NEXT)
#define UART_INTERFACE   "Next"
#elif defined(DIVMMC_UART)
#define UART_INTERFACE   "divMMC"
#else
#define UART_INTERFACE   "AY-3-8912"
#endif

// --- BUFFER SIZES ---
#define LINE_BUFFER_SIZE 80
#define TX_BUFFER_SIZE   128
#define PATH_SIZE        48

// --- COLORES / ATRIBUTOS ---
#define ATTR_BANNER     (PAPER_BLACK | INK_WHITE | BRIGHT)
#define ATTR_STATUS     (PAPER_WHITE | INK_BLUE)
#define ATTR_MAIN_BG    (PAPER_BLACK | INK_WHITE)
#define ATTR_LOCAL      (PAPER_BLACK | INK_GREEN | BRIGHT)
#define ATTR_RESPONSE   (PAPER_BLACK | INK_CYAN | BRIGHT)
#define ATTR_ERROR      (PAPER_BLACK | INK_RED | BRIGHT)
#define ATTR_USER       (PAPER_BLACK | INK_WHITE | BRIGHT)
#define ATTR_INPUT_BG   (PAPER_GREEN | INK_BLACK)
#define ATTR_INPUT      (PAPER_GREEN | INK_BLACK)
#define ATTR_PROMPT     (PAPER_GREEN | INK_BLACK)

#define STATUS_RED      (PAPER_WHITE | INK_RED)
#define STATUS_GREEN    (PAPER_WHITE | INK_GREEN)
#define STATUS_YELLOW   (PAPER_WHITE | INK_YELLOW)

// --- SCREEN LAYOUT ---
#define SCREEN_COLS     64
#define SCREEN_PHYS     32
#define BANNER_START    0
#define BANNER_LINES    2
#define MAIN_START      3
#define MAIN_LINES      16
#define MAIN_END        (MAIN_START + MAIN_LINES - 1)
#define STATUS_LINE     20
#define INPUT_START     22
#define INPUT_LINES     2
#define INPUT_END       23
#define LINES_PER_PAGE  15

// --- FTP STATE ---
#define STATE_DISCONNECTED  0
#define STATE_WIFI_OK       1
#define STATE_FTP_CONNECTED 2
#define STATE_LOGGED_IN     3

// --- KEY CODES ---
#define KEY_UP        11
#define KEY_DOWN      10
#define KEY_LEFT      8
#define KEY_RIGHT     9
#define KEY_BACKSPACE 12
#define KEY_ENTER     13

// --- TIMEOUTS ---
#define SILENCE_SHORT   150UL
#define SILENCE_NORMAL  250UL
#define SILENCE_LONG    400UL
#define SILENCE_XLONG   750UL
#define FRAMES_1S       50
#define FRAMES_5S       (5 * FRAMES_1S)
#define FRAMES_8S       (8 * FRAMES_1S)
#define FRAMES_10S      (10 * FRAMES_1S)
#define FRAMES_LIST_PAUSE_RISKY      (90 * FRAMES_1S)
#define FRAMES_NOOP_QUICK_TIMEOUT    (1 * FRAMES_1S)

// --- HALT MACRO ---
#define HALT() do { __asm__("ei"); __asm__("halt"); } while(0)

// --- RING BUFFER ---
// 2048 bytes: at 115200 baud the ESP can push ~11.5KB/s.
// During SD writes (~5-10ms) a 512B buffer overflows easily.
#define RING_BUFFER_SIZE 2048
#define RING_BUFFER_MASK 0x07FF

// --- DRAIN MODES ---
#if defined(DIVMMC_UART) || defined(BITSTREAM_NEXT)
#define DRAIN_NORMAL    128
#else
#define DRAIN_NORMAL    32
#endif
#define DRAIN_FAST      255

// --- HISTORY ---
#define HISTORY_SIZE    4
#define HISTORY_LEN     40

// --- EXTERNAL UART DRIVER (classic builds) ---
// divMMC backend: TX failures latch uart_tx_failed (fail-stop) until
// ay_uart_init(). The AY bit-bang backend never fails a transmit.
#ifndef BITSTREAM_SPECTRANEXT
extern void     ay_uart_init(void);
extern void     ay_uart_send(uint8_t byte) __z88dk_fastcall;
extern void     ay_uart_send_block(void *buf, uint16_t len) __z88dk_callee;
extern uint8_t  ay_uart_read(void);
extern uint8_t  ay_uart_ready(void);
extern uint8_t  ay_uart_ready_fast(void);
extern void     uart_drain_to_buffer(void);
extern void     uart_send_string(const char *s) __z88dk_fastcall;
#endif

#include "bitstream_net.h"
