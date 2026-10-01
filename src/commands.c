// ============================================================================
// commands.c - Command parser, help, status, special commands
// ============================================================================

// str_to_upper is in asm/bitstream_asm.asm (__z88dk_fastcall)
// skip_ws declared in globals.c (needed by ftp.c which comes before commands.c in SCU)
extern void str_to_upper(char *s) __z88dk_fastcall;

static char* read_token(char *p, char *out, unsigned out_max)
{
    unsigned i = 0;
    p = skip_ws(p);

    if (*p == '"') {
        p++;
        while (*p && *p != '"') {
            if (i + 1 < out_max) out[i++] = *p;
            p++;
        }
        if (*p == '"') p++;
    } else {
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
            if (i + 1 < out_max) out[i++] = *p;
            p++;
        }
    }
    out[i] = '\0';
    return p;
}

static void draw_banner(void);

static void cmd_cls(void)
{
    clear_zone(MAIN_START, MAIN_LINES, ATTR_MAIN_BG);
    main_line = MAIN_START;
    main_col = 0;

    draw_banner();

    invalidate_status_bar();
    draw_status_bar();
}


static void cmd_about(void)
{
    current_attr = ATTR_RESPONSE;
    main_print(APP_NAME " " APP_VERSION " - FTP Client / " UART_INTERFACE);
    print_char_line(22, '-');
    current_attr = ATTR_LOCAL;
    main_print("(C) 2026 M. Ignacio Monge Garcia");
#if defined(BITSTREAM_SPECTRANEXT)
    main_print("Spectranext cartridge sockets + XFS");
#elif defined(DIVMMC_UART)
    main_print("ESP8266 + divMMC/divTiesus UART");
#else
    main_print("ESP8266 + AY-3-8912 bit-banging");
    main_print("AY-UART driver by A. Nihirash");
#endif
}

static void cmd_status(void)
{
    current_attr = ATTR_RESPONSE;
    main_print("--- SYSTEM STATUS ---");

    if (connection_state >= STATE_FTP_CONNECTED) {
        uint16_t code;

        main_puts("Verifying connection... ");
        code = ftp_cmd_reply("NOOP", 150);
        if (code >= 200 && code < 300) {
            main_print(S_OK);
        } else if (code == FTP_CANCEL) {
            fail(S_CANCEL);
        } else if (code == FTP_LOST || code == 421) {
            clear_ftp_state();
            net_ctrl_close();
            fail("FAILED (disconnected)");
        } else if (code) {
            fail("FAILED (bad reply)");
        } else {
            fail("FAILED (timeout)");
        }
        current_attr = ATTR_RESPONSE;
    }

    main_puts("State: ");
    if (connection_state == STATE_DISCONNECTED) main_print(S_DISCONN);
    else if (connection_state == STATE_WIFI_OK) main_print("WiFi OK");
    else if (connection_state == STATE_FTP_CONNECTED) main_print("FTP Connected");
    else main_print("Logged In");

    {
        char *p = tx_buffer;
        p = str_append(p, "IP:    ");
        if (connection_state == STATE_DISCONNECTED || wifi_client_ip[0] == '0') {
             p = str_append(p, "not connected");
        } else {
             p = str_append(p, wifi_client_ip);
        }
        main_print(tx_buffer);
    }

    {
        char *p = tx_buffer;
        p = str_append(p, "Host:  ");
        p = str_append(p, ftp_host);
        main_print(tx_buffer);
    }

    {
        char *p = tx_buffer;
        uint8_t path_len = strlen(ftp_path);
        p = str_append(p, "Path:  ");
        if (path_len > 57) {
            p = char_append(p, '~');
            p = str_append(p, ftp_path + path_len - 56);
        } else {
            p = str_append(p, ftp_path);
        }
        main_print(tx_buffer);
    }
}

static void cmd_help(void)
{
    current_attr = ATTR_RESPONSE;
    main_print("FTP COMMANDS");
    print_char_line(22, '-');
    current_attr = ATTR_LOCAL;
    main_print("  OPEN host[:port] - Connect");
    main_print("  USER name pwd - Login");
    main_print("  QUIT - Disconnect");
    main_print("  PWD  - Show dir");
    main_print("  CD path - Change dir");
    main_print("  LS [filter] - List (-d/-f)");
    main_print("  GET file - Download");
    main_print("Type !HELP for more commands");
}

static void cmd_help_special(void)
{
    current_attr = ATTR_RESPONSE;
    main_print("SPECIAL COMMANDS");
    print_char_line(22, '-');
    current_attr = ATTR_LOCAL;
    main_print("  !CONNECT host[:port][/path] user [pwd]");
    main_print("       Quick connect & login");
    main_print("  !SEARCH [pat] - Search");
    main_print("  !STATUS - WiFi & FTP info");
    main_print("  !CLS - Clear screen");
    main_print("  !INIT - Reset ESP");
    main_print("  !ABOUT - Version");
    current_attr = ATTR_RESPONSE;
    main_print("TIP: BREAK cancels operations");
}

static uint32_t parse_size_arg(const char *s) __z88dk_fastcall
{
    uint32_t val = 0;
    if (*s == '>') s++;

    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }

    if (*s == 'k' || *s == 'K') val <<= 10;
    else if (*s == 'm' || *s == 'M') val <<= 20;

    return val;
}


// ============================================================================
// COMMAND HASH OPTIMIZATION
// ============================================================================

#define CMD_HASH(a,b) (((uint16_t)(a) << 8) | (uint16_t)(b))

#define H_OP CMD_HASH('O','P')
#define H_US CMD_HASH('U','S')
#define H_CD CMD_HASH('C','D')
#define H_PW CMD_HASH('P','W')
#define H_LS CMD_HASH('L','S')
#define H_GE CMD_HASH('G','E')
#define H_QU CMD_HASH('Q','U')
#define H_HE CMD_HASH('H','E')
#define H_QM CMD_HASH('?','\0')

#define H_CO CMD_HASH('C','O')
#define H_SE CMD_HASH('S','E')
#define H_ST CMD_HASH('S','T')
#define H_AB CMD_HASH('A','B')
#define H_CL CMD_HASH('C','L')
#define H_IN CMD_HASH('I','N')
#define H_BANG_HE CMD_HASH('H','E')

static void parse_command(char *line) __z88dk_fastcall
{
    static char cmd[16];
    static char arg1[48];
    static char arg2[32];
    static char arg3[32];
    char *get_args;
    uint16_t h;

    cmd[0] = 0; arg1[0] = 0; arg2[0] = 0; arg3[0] = 0;
    get_args = NULL;

    {
        char *p = line;
        p = read_token(p, cmd, sizeof(cmd));
        get_args = skip_ws(p);
        p = read_token(p, arg1, sizeof(arg1));
        p = read_token(p, arg2, sizeof(arg2));
        p = read_token(p, arg3, sizeof(arg3));
    }

    str_to_upper(cmd);

    // --- BANG COMMANDS (!) ---
    if (cmd[0] == '!') {
        h = CMD_HASH(cmd[1], cmd[2]);
        if (h == H_SE && !ensure_logged_in()) return;

        switch (h) {
            case H_CO:  // !CONNECT
                if (arg1[0] && arg2[0]) {
                    char *host = NULL;
                    char *init_path = NULL;
                    uint16_t port = parse_host_port_path(arg1, &host, &init_path);

                    cmd_open(host, port);

                    if (connection_state >= STATE_FTP_CONNECTED) {
                        cmd_user(arg2, arg3[0] ? arg3 : "zx@zx.net");
                        if (connection_state == STATE_LOGGED_IN && init_path && init_path[0]) {
                            current_attr = ATTR_LOCAL;
                            {
                                char *p = tx_buffer;
                                p = str_append(p, "Navigating to: ");
                                p = str_append(p, init_path);
                            }
                            main_print(tx_buffer);
                            cmd_cd(init_path);
                        }
                        else if (connection_state == STATE_LOGGED_IN) {
                            cmd_pwd();
                        }
                    }
                } else {
                    fail("Usage: !CONNECT host/path user [pass]");
                }
                return;

            case H_SE:  // !SEARCH
                cmd_list_core(arg1, arg2, arg3);
                return;

            case H_ST:  // !STATUS
                cmd_status();
                return;

            case H_AB:  // !ABOUT
                cmd_about();
                return;

            case H_CL:  // !CLS
                cmd_cls();
                return;

            case H_IN:  // !INIT
                current_attr = ATTR_LOCAL;
                main_print("Re-initializing.");
                connection_state = STATE_DISCONNECTED;
                safe_copy(ftp_host, S_EMPTY, sizeof(ftp_host));
                safe_copy(ftp_user, S_EMPTY, sizeof(ftp_user));
                safe_copy(ftp_path, S_EMPTY, sizeof(ftp_path));
                net_reinit();
                return;

            case H_BANG_HE:  // !HELP
                cmd_help_special();
                return;
        }
        fail(S_UNKNOWN_CMD);
        return;
    }

    // --- STANDARD COMMANDS ---
    h = CMD_HASH(cmd[0], cmd[1]);
    if (h == H_CD || h == H_PW || h == H_LS || h == H_GE) {
        if (!ensure_logged_in()) return;
    } else if (h == H_US && connection_state < STATE_FTP_CONNECTED) {
        fail(S_NO_CONN);
        return;
    }

    switch (h) {
        case H_OP:  // OPEN
            if (arg1[0]) {
                char *host = NULL;
                uint16_t port = parse_host_port_path(arg1, &host, NULL);
                cmd_open(host, port);
                interactive_login();
            } else {
                fail("Usage: OPEN host[:port]");
            }
            break;

        case H_US:  // USER
            if (arg1[0]) {
                cmd_user(arg1, arg2[0] ? arg2 : "zx@zx.net");
            } else {
                fail("Usage: USER name [password]");
            }
            break;

        case H_CD:  // CD
            if (arg1[0]) {
                cmd_cd(arg1);
            } else {
                fail("Usage: CD path");
            }
            break;

        case H_PW:  // PWD
            cmd_pwd();
            break;

        case H_LS:  // LS
            cmd_list_core(arg1, NULL, NULL);
            break;

        case H_GE:  // GET
            if (get_args && get_args[0]) {
                cmd_get(get_args);
            } else {
                fail("Usage: GET file1 [file2 ...]");
            }
            break;

        case H_QU:  // QUIT
            if (strlen(cmd) >= 4 && cmd[2] == 'I' && cmd[3] == 'T') cmd_quit();
            else fail(S_UNKNOWN_CMD);
            break;

        case H_HE:  // HELP
            cmd_help();
            break;

        case H_QM:  // ?
            cmd_help();
            break;

        default:
            fail(S_UNKNOWN_CMD);
            break;
    }
}
