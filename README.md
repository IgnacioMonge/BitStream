![BitStream Banner](images/bitstream-logo-white.png)


**FTP Client for ZX Spectrum**

BitStream is a fully-featured FTP client for the ZX Spectrum, enabling file downloads from FTP servers over WiFi using an ESP8266/ESP-12 module. Supports two UART interfaces: **divMMC/divTiesus** at 115200 baud and **AY-UART bit-banging** at 9600 baud.


> [Leer en Espanol](READMEsp.md)

## Features

- **Dual UART support** - divMMC/divTiesus (115200 baud) or AY bit-banging (9600 baud)
- **64-column display** - Clean, readable interface with color-coded output using a compressed 4x8 pixel font
- **Standard FTP commands** - OPEN, USER, PWD, CD, LS, GET, QUIT
- **Quick connect** - `!CONNECT host/path user [pass]` for one-line server access
- **Interactive login** - Prompts for user/password with masked input and anonymous defaults
- **File search** - `!SEARCH` to find files by pattern and minimum size
- **Batch downloads** - Download multiple files with `GET file1 file2 file3`
- **Progress bar** - Visual feedback during file transfers (red for downloads)
- **Connection monitoring** - Automatic detection of timeouts and disconnections
- **Command history** - Navigate previous commands with UP/DOWN arrows (4 entries)
- **Cancellable operations** - Press EDIT key to abort any operation
- **esxDOS integration** - Direct SD card writes via RST 0x08 traps

[![BitStream1](images/BTS1_1.png)](images/BTS1.png) [![BitStream2](images/BTS2_1.png)](images/BTS2.png) [![BitStream3](images/BTS3_1.png)](images/BTS3.png)


## Requirements

### Hardware
- ZX Spectrum (48K/128K/+2/+3)
- divMMC or similar esxDOS-compatible interface
- ESP8266 or ESP-12 WiFi module connected via:
  - **divMMC/divTiesus UART** (directly on the interface, 115200 baud) - recommended
  - **AY-3-8912 chip** (bit-banging, 9600 baud)
- SD card with esxDOS

### Software
- esxDOS 0.8.x or higher
- WiFi network pre-configured on ESP module (use [NetManZX](https://github.com/IgnacioMonge/NetManZX) or similar)

## Installation

1. Copy the appropriate `.tap` file to your SD card:
   - `BitStream_divTiesus.tap` for divMMC/divTiesus UART
   - `BitStream_AY.tap` for AY bit-banging
2. Load with `LOAD ""`
3. Or copy the compiled binary to run directly from esxDOS

## Quick Start

```
!CONNECT ftp.example.com/pub/spectrum anonymous
LS
CD games
GET game.tap
QUIT
```

Or connect step by step:

```
OPEN ftp.scene.org
```

BitStream will prompt for username and password interactively. Press ENTER to accept the anonymous defaults.

## Commands

### Standard FTP Commands

| Command | Description | Example |
|---------|-------------|---------|
| `OPEN host[:port]` | Connect to FTP server | `OPEN ftp.scene.org` |
| `USER name [pass]` | Login with credentials | `USER anonymous` |
| `PWD` | Show current directory | `PWD` |
| `CD path` | Change directory | `CD /pub/games` |
| `LS [filter]` | List directory contents | `LS *.tap` |
| `GET file [...]` | Download file(s) | `GET game.tap` |
| `QUIT` | Disconnect from server | `QUIT` |

### Special Commands

| Command | Description | Example |
|---------|-------------|---------|
| `!CONNECT` | Quick connect with path | `!CONNECT ftp.site.com/path user pass` |
| `!STATUS` | Show connection status | `!STATUS` |
| `!SEARCH [pattern] [>size]` | Search files | `!SEARCH *.sna >16000` |
| `!INIT` | Re-initialize WiFi module | `!INIT` |
| `HELP` | Show standard commands | `HELP` |
| `!HELP` | Show special commands | `!HELP` |
| `!CLS` | Clear screen | `!CLS` |
| `!ABOUT` | Show version info | `!ABOUT` |

### Navigation

- **UP/DOWN** - Command history
- **LEFT/RIGHT** - Move cursor in input line
- **EDIT** - Cancel current operation
- **ENTER** - Execute command

## File Search

The `!SEARCH` command allows filtering by name pattern and minimum file size:

```
!SEARCH *.tap          # Find all .tap files
!SEARCH game           # Find files containing "game"
!SEARCH *.sna >48000   # Find .sna files larger than 48KB
!SEARCH >16384         # Find any file larger than 16KB
```

## Status Bar

The bottom status bar shows:
- **Host** - Connected server (or "---" if disconnected)
- **User** - Logged in username
- **Path** - Current remote directory
- **Indicator** - Connection state (green=logged in, yellow=connected, red=disconnected)

[![BitStream4](images/BTS4_1.png)](images/BTS4.png) [![BitStream5](images/BTS5_1.png)](images/BTS5.png) [![BitStream6](images/BTS6_1.png)](images/BTS6.png)


## Troubleshooting

### "No WiFi" on startup
- Ensure ESP module is properly connected
- Check WiFi is configured (use NetManZX first)
- Try `!INIT` to re-initialize

### Connection timeouts
- Server may have idle timeout; reconnect with `!CONNECT`
- Check WiFi signal strength
- Some servers limit anonymous connections

### Transfer errors
- Ensure sufficient space on SD card
- Large files may timeout on slow connections
- Use `!STATUS` to verify connection is alive

### Commands not responding
- Press EDIT to cancel stuck operations
- Try `!INIT` to reset module state

## Technical Details

- **UART**: divMMC/divTiesus at 115200 bps, or AY bit-banging at 9600 bps
- **Protocol**: FTP passive mode (CIPMUX=1, socket 0=control, 1=data)
- **Display**: 64-column text mode (compressed 4x8 pixel font, 298 bytes)
- **Buffer**: 2048-byte ring buffer for UART
- **Memory**: ORG 24000, 256-byte stack, BSS trim via `__data_compiler_tail`
- **Timeouts**: Frame-based (50Hz) for accurate timing

## Architecture

BitStream v1.2.0 uses a modular Single Compilation Unit (SCU) design:

```
src/
  main_build.c    # SCU orchestrator (#includes all modules)
  globals.c       # Global state, constants, shared strings
  ui.c            # Video, widgets, status bar, input zone
  comms.c         # Ring buffer, UART helpers, ESP init, TCP
  ftp.c           # FTP protocol, esxDOS, download, list
  commands.c      # Command parser, help, status
  main.c          # Screen init, main loop
include/
  bitstream.h     # Master header (constants, externs, ASM decls)
  font64_data.h   # 4px wide font (96 chars, packed nibbles)
asm/
  bitstream_asm.asm     # Core ASM routines (screen, ring buffer, rendering)
  divtiesus_uart.asm    # divMMC UART driver (115200 baud)
  ay_uart.asm           # AY bit-bang UART driver (9600 baud)
```

Critical routines (screen scrolling, ring buffer, string operations, text rendering) are implemented in Z80 assembly for performance on the 3.5MHz CPU.

## Building from Source

Requires [z88dk](https://github.com/z88dk/z88dk) compiler.

```bash
# Build divMMC version (default)
make

# Build AY bit-bang version
make ay

# Build both versions
make both

# Release build (aggressive optimization)
make release
```

## Credits

- **Code**: M. Ignacio Monge Garcia
- **AY-UART driver**: Based on code by A. Nihirash
- **Font**: 4x8 64-column compressed font

## License

This project is released under the MIT License. See [LICENSE](LICENSE) for details.

## Links

- [NetManZX](https://github.com/IgnacioMonge/NetManZX) - WiFi network manager for ZX Spectrum
- [esxDOS](http://esxdos.org) - DOS for divMMC interfaces
- [z88dk](https://github.com/z88dk/z88dk) - Z80 development kit

---

*BitStream v1.2.0 - (C) 2026 M. Ignacio Monge Garcia*
