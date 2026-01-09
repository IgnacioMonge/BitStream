![BitStream Banner](images/bitstream-logo-white.png)

# BitStream v1.0

**FTP Client for ZX Spectrum**

BitStream is a fully-featured FTP client for the ZX Spectrum, enabling file downloads from FTP servers over WiFi using an ESP8266/ESP-12 module connected via AY-UART bit-banging at 9600 baud.


> 🇪🇸 **[Leer en Español](READMEsp.md)**

## Features

- **64-column display** - Clean, readable interface with color-coded output
- **Standard FTP commands** - OPEN, USER, PWD, CD, LS, GET, QUIT
- **Quick connect** - `!CONNECT host/path user [pass]` for one-line server access
- **File search** - `!SEARCH` to find files by pattern and minimum size
- **Batch downloads** - Download multiple files with `GET file1 file2 file3`
- **Progress bar** - Visual feedback during file transfers
- **Connection monitoring** - Automatic detection of timeouts and disconnections
- **Command history** - Navigate previous commands with UP/DOWN arrows
- **Cancellable operations** - Press EDIT key to abort any operation

## Requirements

### Hardware
- ZX Spectrum (48K/128K/+2/+3)
- divMMC or similar esxDOS-compatible interface
- ESP8266 or ESP-12 WiFi module connected to AY chip
- SD card with esxDOS

### Software
- esxDOS 0.8.x or higher
- WiFi network pre-configured on ESP module (use [NetManZX](https://github.com/IgnacioMonge/NetManZX) or similar)

## Installation

1. Copy `BitStream.tap` to your SD card
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

## Commands

### Standard FTP Commands

| Command | Description | Example |
|---------|-------------|---------|
| `OPEN host [port]` | Connect to FTP server | `OPEN ftp.scene.org` |
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
| `!DEBUG` | Toggle debug mode | `!DEBUG` |
| `HELP` | Show standard commands | `HELP` |
| `!HELP` | Show special commands | `!HELP` |
| `CLS` | Clear screen | `CLS` |
| `ABOUT` | Show version info | `ABOUT` |

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

- **Baud rate**: 9600 bps (AY-UART bit-banging)
- **Protocol**: FTP passive mode
- **Display**: 64-column text mode (4x8 pixel font)
- **Buffer**: 256-byte ring buffer for UART
- **Timeouts**: Frame-based (50Hz) for accurate timing

## Building from Source

Requires z88dk compiler:

```bash
zcc +zx -vn -SO3 -startup=0 -clib=new -zorg=24576 \
    -pragma-define:CLIB_MALLOC_HEAP_SIZE=0 \
    -pragma-define:CLIB_STDIO_HEAP_SIZE=0 \
    -pragma-define:CRT_STACK_SIZE=512 \
    bitstream.c ay_uart.asm -o BitStream -create-app
```

## Credits

- **Code**: M. Ignacio Monge Garcia
- **AY-UART driver**: Based on code by A. Nihirash
- **Font**: 4x8 64-column font from common ZX sources

## License

This project is released under the MIT License. See [LICENSE](LICENSE) for details.

## Links

- [NetManZX](https://github.com/IgnacioMonge/NetManZX) - WiFi network manager for ZX Spectrum
- [esxDOS](http://esxdos.org) - DOS for divMMC interfaces
- [z88dk](https://github.com/z88dk/z88dk) - Z80 development kit

---

*BitStream v1.0 - (C) 2025 M. Ignacio Monge Garcia*
