// ============================================================================
// bitstream_net.h - Transport and storage seams
// ============================================================================
// The FTP layer (ftp.c, commands.c, main.c, ui.c) talks only to these
// functions. Two backends implement them, selected at compile time:
//   classic     src/net_esp.c          ESP8266 AT firmware over UART
//               src/fs_esx.c           esxDOS
//   Spectranext src/net_spectranext.c  cartridge ROM sockets (no UART)
//               src/fs_spectranext.c   cartridge XFS
// Direct compile-time calls, no vtable (same rule as SpecTalkZX).
//
// Control-line contract: net_ctrl_line() returns the reply text without any
// transport framing. The pointer is valid until the next net_* call, so a
// caller parses or copies it before calling into the transport again.
// ============================================================================
#pragma once

#define NET_EOF         (-1)    // net_data_read: data connection finished

// --- lifecycle ---
static void     net_boot(void);              // first bring-up, prints progress
static void     net_reinit(void);            // !INIT
static void     net_poll(void);              // keep RX flowing (cheap)

// --- control connection ---
static uint8_t  net_ctrl_open(const char *host, uint16_t port);
static void     net_ctrl_close(void);
static uint8_t  net_ctrl_send(const char *data, uint16_t len);
static char    *net_ctrl_line(void);         // complete reply line or NULL
static void     net_ctrl_discard(void);      // drop every buffered line
static uint8_t  net_ctrl_lost(void);         // peer closed / transport dead

// --- data connection (PASV) ---
static uint8_t  net_data_open(const char *ip, uint16_t port);
static void     net_data_close(void);
static int16_t  net_data_read(uint8_t *dst, uint16_t max);  // >0, 0, NET_EOF

// --- storage ---
#define FS_BAD  0xFF
static uint8_t  fs_available;
static uint8_t  fs_init(void);
static uint8_t  fs_exists(const char *name) __z88dk_fastcall;
static uint8_t  fs_create(const char *name) __z88dk_fastcall;   // FS_BAD on error
static uint16_t fs_write(uint8_t h, const void *buf, uint16_t len);
static void     fs_close(uint8_t h) __z88dk_fastcall;
static void     fs_remove(const char *name) __z88dk_fastcall;
static uint8_t  fs_commit(const char *name) __z88dk_fastcall;  // persist (1 ok)
