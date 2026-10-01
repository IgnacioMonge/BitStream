// ============================================================================
// net_spectranext.c - Spectranext transport: cartridge ROM sockets
// ============================================================================
// No UART, no AT dialogue. The cartridge's jump table is reached through the
// SpectraNext driver bridge (spxn_rom.asm: spxn_rom_hlcall / spxn_rom_ixcall,
// registers passed in the spxn_regs block). Two BitStream-owned descriptors:
// control and PASV data. The packaged spxn.c state machine is not linked; it
// owns a single connection (same choice as the SpecTalkZX port).
//
// Rules from docs/sockets.md and docs/porting.md:
//   - POLLFD before RECV; one POLLIN permits one RECV (RECV blocks otherwise).
//   - On POLLIN|POLLHUP drain first, then treat the hangup.
//   - Destination ports 22 and 443 start the cart's SSH/TLS offload: refused.
//   - Buffers must live at $4000+ (all of BitStream's do).
// Control bytes are received into ring_buffer and assembled into rx_line by
// the shared try_read_line_nodrain; data is received straight into the
// caller's buffer.
// ============================================================================

// ROM entry points (spectranext/driver/spxn_rom.h)
#define ROM_SOCKET         0x3E00
#define ROM_CLOSE          0x3E03
#define ROM_CONNECT        0x3E0F
#define ROM_SEND           0x3E12
#define ROM_RECV           0x3E15
#define ROM_POLLFD         0x3E24
#define ROM_GETHOSTBYNAME  0x3E27   // IXCALL
#define ROM_SPECTRANEXT    0x3EF0   // IXCALL, operation in A

#define SOCK_STREAM        0x01
#define ROM_CARRY          0x01     // bridge result: ROM reported an error
#define ROM_ZERO           0x02     // bridge result: POLLFD idle
#define POLLHUP            0x02
#define POLLIN             0x04
#define POLLNVAL           0x80

#define NX_NONE            0xFF
#define NX_CTRL            0
#define NX_DATA            1
#define NX_SEND_ZERO_BUDGET 50

// Register block shared with spxn_rom.asm (7 bytes: a, bc, de, hl)
struct spxn_regs_t {
    uint8_t  a;
    uint16_t bc;
    uint16_t de;
    uint16_t hl;
};
extern struct spxn_regs_t spxn_regs;
extern uint8_t spxn_rom_hlcall(uint16_t addr) __z88dk_fastcall;
extern uint8_t spxn_rom_ixcall(uint16_t addr) __z88dk_fastcall;
extern int16_t spxn_rom_detect(void);

static uint8_t nx_fd[2] = { NX_NONE, NX_NONE };
#ifdef BITSTREAM_SELFTEST
static uint16_t nx_dbg_polls;
static uint8_t nx_dbg_r, nx_dbg_fl;
static uint16_t nx_dbg_rx;
static uint16_t nx_dbg_recvs, nx_dbg_maxbc, nx_dbg_short;
#endif
static uint8_t nx_hup[2];           // peer closed and fully drained

static void nx_close(uint8_t s) __z88dk_fastcall
{
    if (nx_fd[s] != NX_NONE) {
        spxn_regs.a = nx_fd[s];
        spxn_rom_hlcall(ROM_CLOSE);
        nx_fd[s] = NX_NONE;
    }
    nx_hup[s] = 0;
}

static uint8_t nx_connect(uint8_t s, const char *host, uint16_t port)
{
    uint8_t ip4be[4];

    nx_close(s);
    if (port == 0 || port == 22 || port == 443) return 0;

    // GETHOSTBYNAME also accepts a dotted address (PASV) and returns the
    // big-endian form CONNECT expects.
    spxn_regs.hl = (uint16_t)host;
    spxn_regs.de = (uint16_t)ip4be;
    if (spxn_rom_ixcall(ROM_GETHOSTBYNAME) & ROM_CARRY) return 0;

    spxn_regs.bc = SOCK_STREAM;
    if (spxn_rom_hlcall(ROM_SOCKET) & ROM_CARRY) return 0;
    nx_fd[s] = spxn_regs.a;

    spxn_regs.a = nx_fd[s];
    spxn_regs.de = (uint16_t)ip4be;
    spxn_regs.bc = port;
    if (spxn_rom_hlcall(ROM_CONNECT) & ROM_CARRY) {
        nx_close(s);
        return 0;
    }
    nx_hup[s] = 0;
    return 1;
}

// >0 bytes received, 0 nothing yet, NET_EOF closed (after draining)
static int16_t nx_recv(uint8_t s, uint8_t *dst, uint16_t max)
{
    uint8_t r;

    if (nx_fd[s] == NX_NONE || nx_hup[s]) return NET_EOF;
    spxn_regs.a = nx_fd[s];
    r = spxn_rom_hlcall(ROM_POLLFD);
#ifdef BITSTREAM_SELFTEST
    nx_dbg_polls++; nx_dbg_r = r; nx_dbg_fl = (uint8_t)spxn_regs.bc;
#endif
    if (r & ROM_CARRY) goto hup;
    if (r & ROM_ZERO) return 0;
    r = (uint8_t)spxn_regs.bc;
    if (r & POLLIN) {
        spxn_regs.a = nx_fd[s];
        spxn_regs.de = (uint16_t)dst;
        spxn_regs.bc = max;
        if (spxn_rom_hlcall(ROM_RECV) & ROM_CARRY) goto hup;
        if (spxn_regs.bc > max) goto hup;
#ifdef BITSTREAM_SELFTEST
        nx_dbg_rx += spxn_regs.bc;
        nx_dbg_recvs++;
        if (spxn_regs.bc > nx_dbg_maxbc) nx_dbg_maxbc = spxn_regs.bc;
        if (spxn_regs.bc < max) nx_dbg_short++;
#endif
        return (int16_t)spxn_regs.bc;
    }
    if (r & (POLLHUP | POLLNVAL)) goto hup;
    return 0;
hup:
    nx_hup[s] = 1;
    return NET_EOF;
}

static uint8_t nx_send(uint8_t s, const char *data, uint16_t len)
{
    uint8_t zero_budget = NX_SEND_ZERO_BUDGET;

    if (nx_fd[s] == NX_NONE) return 0;
    while (len) {
        spxn_regs.a = nx_fd[s];
        spxn_regs.de = (uint16_t)data;
        spxn_regs.bc = len;
        if ((spxn_rom_hlcall(ROM_SEND) & ROM_CARRY) || spxn_regs.bc > len) {
            nx_hup[s] = 1;
            return 0;
        }
        if (!spxn_regs.bc) {                // backpressure
            if (!--zero_budget) return 0;
            HALT();
            continue;
        }
        data += spxn_regs.bc;
        len -= spxn_regs.bc;
    }
    return 1;
}

// Control socket -> ring (contiguous free space, bounded chunk)
static void nx_pump_ctrl(void)
{
    uint16_t room = (uint16_t)(rb_tail - rb_head - 1u) & RING_BUFFER_MASK;
    uint16_t contig = RING_BUFFER_SIZE - rb_head;
    int16_t n;

    if (!room) return;                      // backpressure: nothing lost
    if (contig > room) contig = room;
    if (contig > 256) contig = 256;
    n = nx_recv(NX_CTRL, ring_buffer + rb_head, contig);
    if (n > 0) rb_head = (rb_head + (uint16_t)n) & RING_BUFFER_MASK;
}

// ============================================================================
// SEAM IMPLEMENTATION
// ============================================================================

static void net_boot(void)
{
    uint8_t ip[4];

    current_attr = ATTR_LOCAL;
    main_puts("Spectranext.");
    connection_state = STATE_DISCONNECTED;

    if (spxn_rom_detect() != 1) {
        current_attr = ATTR_ERROR;
        main_print(" Not found");
        draw_status_bar();
        return;
    }

    // $3EF0 op 0: controller status + 4 IPv4 bytes. FuseX returns them
    // first-octet-first (127.0.0.1 -> 7F 00 00 01); not yet checked on the
    // physical cart, whose docs call the order "host order".
    ip[0] = ip[1] = ip[2] = ip[3] = 0;
    spxn_regs.a = 0;
    spxn_regs.de = (uint16_t)ip;
    spxn_rom_ixcall(ROM_SPECTRANEXT);

    if (ip[0] | ip[1] | ip[2] | ip[3]) {
        char *p = wifi_client_ip;
        p = u16_to_dec(p, ip[0]); p = char_append(p, '.');
        p = u16_to_dec(p, ip[1]); p = char_append(p, '.');
        p = u16_to_dec(p, ip[2]); p = char_append(p, '.');
        u16_to_dec(p, ip[3]);
        connection_state = STATE_WIFI_OK;
        current_attr = ATTR_RESPONSE;
        main_print(" WiFi OK");
    } else {
        current_attr = ATTR_ERROR;
        main_print(S_NO_WIFI);
    }
    draw_status_bar();
}

static void net_reinit(void)
{
    nx_close(NX_DATA);
    nx_close(NX_CTRL);
    rb_head = rb_tail = 0;
    rx_pos = 0;
    rx_overflow = 0;
    net_boot();
}

static void net_poll(void)
{
    if (nx_fd[NX_CTRL] != NX_NONE) nx_pump_ctrl();
}

static uint8_t net_ctrl_open(const char *host, uint16_t port)
{
    rb_head = rb_tail = 0;
    rx_pos = 0;
    rx_overflow = 0;
    return nx_connect(NX_CTRL, host, port);
}

static void net_ctrl_close(void)
{
    nx_close(NX_DATA);
    nx_close(NX_CTRL);
    rb_head = rb_tail = 0;
    rx_pos = 0;
}

static uint8_t net_ctrl_send(const char *data, uint16_t len)
{
    return nx_send(NX_CTRL, data, len);
}

static char *net_ctrl_line(void)
{
    net_poll();
    return try_read_line_nodrain() ? rx_line : NULL;
}

static void net_ctrl_discard(void)
{
    while (net_ctrl_line() != NULL) {}
}

static uint8_t net_ctrl_lost(void)
{
    // A hangup counts only once every buffered reply has been read
    return nx_hup[NX_CTRL] && rb_head == rb_tail;
}

static uint8_t net_data_open(const char *ip, uint16_t port)
{
    return nx_connect(NX_DATA, ip, port);
}

static void net_data_close(void)
{
    nx_close(NX_DATA);
}

static uint8_t net_data_midframe(void)
{
    return 0;                       // TCP backpressure in the cart: nothing to lose
}

static int16_t net_data_read(uint8_t *dst, uint16_t max)
{
    return nx_recv(NX_DATA, dst, max);
}
