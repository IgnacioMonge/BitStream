# Changelog

All notable changes to BitStream are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [1.0.0] - 2025-01-03

### Added
- Initial public release
- Full FTP client functionality with passive mode transfers
- 64-column display with color-coded output
- Standard FTP commands: OPEN, USER, PWD, CD, LS, GET, QUIT
- Quick connect command: `!CONNECT host/path user [pass]`
- File search with pattern and size filtering: `!SEARCH`
- Batch file downloads: `GET file1 file2 file3`
- Progress bar with file size display during transfers
- Connection status monitoring with automatic timeout detection
- Command history navigation (UP/DOWN arrows, 4 entries)
- Cursor movement in input line (LEFT/RIGHT arrows)
- Cancellation support with EDIT key for all operations
- Status bar showing host, user, path, and connection indicator
- Debug mode toggle (`!DEBUG`) for troubleshooting
- Module re-initialization (`!INIT`) for recovery from errors
- Help system (`HELP`, `!HELP`, `ABOUT`)

### Technical Features
- AY-UART bit-banging communication at 9600 baud
- 256-byte ring buffer for UART data
- Frame-based timeouts (50Hz) for accurate timing
- Adaptive drain control for UI responsiveness vs transfer speed
- Robust error detection and recovery
- esxDOS file system integration

### Architecture
- Clean separation of UART, FTP protocol, and UI layers
- Centralized state management (`clear_ftp_state()`)
- Consistent HALT-based timing throughout
- Unified keyboard handling for cancel operations

---

## Development History

### Pre-release Development (December 2024 - January 2025)

#### Phase 1: Core Infrastructure
- Implemented AY-UART driver integration
- Created 64-column text display system
- Built ring buffer for reliable UART communication
- Established basic ESP8266 AT command handling

#### Phase 2: FTP Protocol
- Implemented FTP passive mode connection
- Added USER/PASS authentication flow
- Built directory listing (LIST) parsing
- Created file download (RETR) with chunked writes

#### Phase 3: User Interface
- Designed status bar with live updates
- Implemented command input with history
- Added progress bar for file transfers
- Created help system

#### Phase 4: Robustness
- Added timeout detection for all operations
- Implemented connection loss detection
- Created cancellation system with EDIT key
- Built automatic reconnection prompts

#### Phase 5: Polish & Optimization
- Unified timeout handling (frame-based)
- Consistent keyboard handling (HALT + in_inkey pattern)
- Centralized state cleanup (`clear_ftp_state()`)
- Code size optimization (common strings, helper functions)
- Fixed edge cases in PWD updates after login
- Resolved ls/search hanging issues
- Improved !STATUS verification reliability

### Bug Fixes During Development
- Fixed `!STATUS` hanging at "Verifying connection..."
- Fixed timeout detection not triggering after long idle
- Fixed PWD not updating after login
- Fixed ls command breaking after error detection changes
- Fixed keyboard responsiveness issues (reduced DRAIN_NORMAL)
- Fixed silent failures when server disconnects mid-operation
- Fixed inconsistent cancel behavior across different commands

---

## Versioning

This project uses [Semantic Versioning](https://semver.org/):
- MAJOR: Incompatible changes
- MINOR: New features, backward compatible
- PATCH: Bug fixes, backward compatible

[1.0.0]: https://github.com/imnacio/bitstream/releases/tag/v1.0.0
