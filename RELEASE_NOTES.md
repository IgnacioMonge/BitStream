# BitStream v1.0.0 Release Notes

**Release Date:** January 3, 2025

We're excited to announce the first public release of BitStream, a full-featured FTP client for the ZX Spectrum!

## What is BitStream?

BitStream brings modern FTP file transfers to your classic ZX Spectrum. Using an ESP8266/ESP-12 WiFi module connected via the AY chip, you can now download files directly from FTP servers anywhere in the world to your Spectrum's SD card.

## Key Features

### 🌐 Full FTP Support
- Connect to any standard FTP server
- Anonymous and authenticated login
- Navigate directories, list files, download content
- Passive mode transfers for maximum compatibility

### 📺 Beautiful 64-Column Display
- High-density text display (64 characters per line)
- Color-coded output:
  - **Green**: Local commands and status
  - **Cyan**: Server responses
  - **Red**: Errors and warnings
  - **White**: User input

### ⚡ Quick Connect
Skip the multi-step connection process with our `!CONNECT` command:
```
!CONNECT ftp.scene.org/pub/sinclair anonymous
```
One command does it all: connect, login, and navigate to your destination.

### 🔍 Powerful Search
Find exactly what you're looking for with `!SEARCH`:
```
!SEARCH *.tap >16000    # TAP files larger than 16KB
!SEARCH jetpac          # Files containing "jetpac"
```

### 📦 Batch Downloads
Download multiple files in one go:
```
GET game1.tap game2.tap game3.tap
```

### 📊 Visual Feedback
- Progress bar during downloads showing:
  - Filename
  - Bytes transferred / Total size
  - Visual progress indicator with spinner
- Status bar always visible showing connection state

### ⌨️ User-Friendly Interface
- Command history (UP/DOWN arrows)
- Cursor editing (LEFT/RIGHT arrows)
- Cancel any operation with EDIT key
- Comprehensive help (`HELP`, `!HELP`)

## System Requirements

### Hardware
- ZX Spectrum 48K or higher
- divMMC or compatible esxDOS interface
- ESP8266/ESP-12 WiFi module (connected to AY chip pins)
- SD card

### Software
- esxDOS 0.8.x or higher
- WiFi pre-configured on ESP module

### Recommended Setup
1. Use [NetManZX](https://github.com/imnacio/netmanzx) to configure your WiFi connection first
2. Then launch BitStream for FTP access

## Installation

1. Download `BitStream.tap` from the releases page
2. Copy to your SD card
3. On your Spectrum: `LOAD ""`

## Quick Start Guide

1. **Start BitStream** - You'll see "Initializing... OK" if WiFi is ready

2. **Connect to a server:**
   ```
   !CONNECT ftp.scene.org/pub/sinclair anonymous
   ```

3. **Browse files:**
   ```
   LS              # List current directory
   CD games        # Enter subdirectory
   LS *.tap        # Filter by extension
   ```

4. **Download:**
   ```
   GET mygame.tap
   ```

5. **Disconnect:**
   ```
   QUIT
   ```

## Command Reference

### Connection
| Command | Description |
|---------|-------------|
| `OPEN host` | Connect to FTP server |
| `USER name [pass]` | Login |
| `!CONNECT host/path user [pass]` | Quick connect |
| `QUIT` | Disconnect |

### Navigation
| Command | Description |
|---------|-------------|
| `PWD` | Print working directory |
| `CD path` | Change directory |
| `LS [filter]` | List files |
| `!SEARCH [pattern] [>size]` | Search files |

### Transfer
| Command | Description |
|---------|-------------|
| `GET file [file2...]` | Download file(s) |

### System
| Command | Description |
|---------|-------------|
| `!STATUS` | Show detailed status |
| `!INIT` | Re-initialize module |
| `!DEBUG` | Toggle debug output |
| `CLS` | Clear screen |
| `HELP` / `!HELP` | Show help |
| `ABOUT` | Version info |

## Known Limitations

- **Speed**: 9600 baud limits transfer speed to ~1KB/sec
- **File size**: Very large files may timeout on slow servers
- **Connections**: Some servers limit anonymous sessions
- **Protocol**: FTP only (no SFTP/FTPS)

## Troubleshooting

### "No WiFi" at startup
Your ESP module needs WiFi configured first. Use NetManZX or configure manually via AT commands.

### Connection drops
- Server idle timeouts are common; just reconnect
- Check signal strength if drops are frequent
- Use `!STATUS` to verify connection state

### Download fails
- Check SD card space
- Try smaller files first
- Some servers require specific transfer modes

### Stuck operation
Press **EDIT** to cancel. If unresponsive, try `!INIT`.

## Technical Notes

- Uses FTP passive mode for NAT compatibility
- All operations are cancellable via EDIT key
- Automatic detection of server disconnection
- Frame-accurate timing using Z80 HALT instruction

## Credits

**Development:** M. Ignacio Monge Garcia

**AY-UART Driver:** Based on work by A. Nihirash

**Testing & Feedback:** The ZX Spectrum community

## Support

- **Issues:** Report bugs on GitHub
- **Questions:** Open a discussion on the repository

## What's Next?

Planned for future versions:
- Upload support (PUT command)
- Bookmark system for favorite servers
- Configuration file for defaults
- Resume interrupted downloads

---

Thank you for using BitStream! 

*Happy downloading on your Spectrum!* 🎮

---

*BitStream v1.0.0 - (C) 2025 M. Ignacio Monge Garcia*
*Released under MIT License*
