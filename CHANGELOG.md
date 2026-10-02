# Changelog

All notable changes to BitStream are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).


## [Unreleased] - audit-2026-10

See `docs/audit-2026-10.md` for the full audit.

### Fixed
- Memory corruption when server text with bytes >= 0x80 (UTF-8 paths) reached
  the BPE expander; expansion is now bounded and server text is folded to ASCII
- Downloads are verified against `SIZE`; truncated transfers and 4xx/5xx final
  replies fail and remove the partial file
- RETR/LIST errors (425/426/450/451/530/550...) are shown instead of a silent
  timeout; 0-byte files download correctly
- FTP replies are parsed by code (RFC 959 multi-line aware), never by
  substring search over the `+IPD` header
- CIPSEND prompt wait no longer discards socket data nor abandons a send on BREAK
- divMMC UART: no silent byte drops on TX (fail-stop latch), RX drained during TX,
  ~3x faster RX drain
- Garbled text when the glyph LUT crossed a page boundary
- `utf8_to_ascii` no longer eats bytes after a Latin-1 character
- Progress bar for files above 1 MB
- PASV replies with an unroutable (NAT) address connect to the control host
- ESP links left open after failed connects are closed; `!INIT` closes all links
- Cursor caps indicator no longer flickers on CAPS chords (arrows, DELETE, BREAK)
- Static variables were not zeroed at startup (BSS trimmed from the TAP and
  not cleared by the CRT): a warm load inherited stale state

### Changed
- Transport/storage seam (`include/bitstream_net.h`) with an `+IPD`
  demultiplexer for the ESP backend
- Local file is created right before RETR (no esxDOS work while data streams)
- Scroll runs with interrupts enabled, unrolled LDI, UART drained first
- `print_line64_fast` skips glyph unpacking for blank pairs
- Keyboard scan, key repeat and input-line cell drawing moved to asm
  (SpecTalkZX); text keys auto-repeat
- Classic builds run a resident IM2 frame interrupt (NetChessZX): no divMMC
  automap or ROM keyboard scan on every frame
- Plain lines longer than 64 columns wrap at the last space
- About 1 KB less code/BSS than 1.3.0 (more stack headroom)

### Added
- ZX Spectrum Next build (`make next`): internal UART driver
  (`asm/next_uart.asm`, baud from the video timing), CPU at 28 MHz
- Spectranext build (`make spectranext SPXN_DIR=...`): cartridge sockets + XFS
- Word/line editing: SYMBOL+CAPS SHIFT + 5/8 (word), 7/6 (line start/end),
  0 (delete word)
- `tools/test_asm_cpu.py` (z88dk-ticks) and `tools/e2e_zesarux.py`
  (ZEsarUX + ZXESPEmu + pyftpdlib) test harnesses

## [1.3.0] - 2026-04-20

### Major: Safer Transfers and Protocol Hardening
- **Download pipeline hardened**:
  - Local files are now created only after a positive `150/125` reply
  - Partial files are deleted automatically after cancel, timeout, or write error
  - "No data" download timeouts are frame-paced again, avoiding premature aborts
- **LIST/SEARCH made more robust**:
  - Silence during listings is now reported as `LIST timeout` / `incomplete`
  - UTF-8 bytes are preserved until final `utf8_to_ascii()` conversion
  - `NOOP` validation after long listing pauses now accepts `+IPD,0,...:2xx` replies
- **Control-channel response handling tightened**:
  - `wait_for_string()` no longer treats unrelated `OK` replies as success when a specific token is expected
  - This prevents false positives during WiFi and TCP setup sequences

### Major: Build Pipeline and Size Work
- **BPE string compression integrated into the build**:
  - `tools/bpe_compress.py` now compresses UI/server strings before build
  - Original sources are always restored, even after failed builds
- **Additional code size reductions**:
  - Dead code cleanup, tail-call cleanup, string deduplication, and library-drag removal
  - Custom `copt` rules are now part of the project and wired into the Makefile
- **Safer runtime margins**:
  - CRT stack size raised from 256 to 512 bytes
  - BSS trimming remains part of the default TAP pipeline

### Improvements
- **Double-height status and progress UI**:
  - Status bar now uses double-height rendering with partial redraw
  - Progress bar matches the same style and reduces visible flicker
- **Input UX refined**:
  - Password prompts support masked input with `UP` to toggle visibility
  - Input cache now validates the real VRAM attribute before skipping redraws
- **Listing and search quality-of-life**:
  - `LS` supports file/directory filters (`-f`, `-d`)
  - `!SEARCH` supports pattern and minimum-size filters
- **Build system refreshed**:
  - Unified `build/` output directory
  - `make`, `make divmmc`, `make ay`, `make both`, and release targets documented and maintained

### Bug Fixes
- **sccz80 / ABI safety**:
  - `esx_fwrite` inline ASM now preserves IX correctly
  - `_rx_pos_reset` `copt` rule now preserves the expected `HL=0` contract
- **FTP correctness**:
  - Failed or cancelled `GET` no longer leaves zero-byte or truncated files behind
  - `quick_noop_check()` no longer times out on valid IPD-wrapped FTP control replies
  - LIST completion is no longer silently reported as success after a timeout
- **Parser and command cleanup**:
  - Restricted command checks were folded into the command dispatcher
  - Internal silent-PWD wrapper removed in favor of direct `pwd_core(1)` usage
  - 8.3 filename generation and duplicate-name handling were tightened up

### Code Size
- divMMC TAP: 36634 bytes
- AY TAP: 37251 bytes

---


## [1.2.0] - 2026-03-07

### Major: Modular Architecture & ASM Port
- **Complete rewrite to modular SCU (Single Compilation Unit)**:
  - 6 modules: globals, ui, comms, ftp, commands, main
  - Clean separation of concerns with forward declarations
  - Shared header `bitstream.h` for all constants and externs
- **Z80 assembly port** of critical routines (`bitstream_asm.asm`):
  - Screen: `clear_line`, `clear_zone`, `scroll_main_zone` (DI/EI, LDIR)
  - Ring buffer: `rb_pop`, `rb_push`, `try_read_line_nodrain`
  - UART: `uart_send_string`, `uart_drain_to_buffer`
  - String: `str_append`, `char_append`, `str_to_upper`, `skip_ws`, `st_copy_n`
  - Rendering: `print_str64_char`, `main_putc`, `main_newline`, `main_puts`
  - Numeric: `u16_to_dec`, `parse_decimal`, `format_83_name`
- **Compressed font**: 10-byte LUT + 96x3 = 298 bytes (vs 768 uncompressed)
- **BSS trim**: `__data_compiler_tail` symbol eliminates ~3,763 bytes of zeros from TAP

### Major: Dual UART Support
- **divMMC/divTiesus UART driver** (`divtiesus_uart.asm`): 115200 baud
- **AY-3-8912 bit-bang driver** (`ay_uart.asm`): 9600 baud
- Build system supports both: `make` (divMMC) / `make ay` / `make both`
- Conditional compilation via `DIVMMC_UART` / `AY_UART` defines

### New Features
- **Interactive login**: OPEN now prompts for user/password interactively
  - Masked password input with UP arrow to toggle visibility
  - Anonymous defaults clearly indicated in prompts
- **OPEN host:port parsing**: Port number now correctly parsed from host string
- **!CONNECT with path**: `!CONNECT host/path user pass` navigates to path after login
- **esxDOS detection at startup**: Safe detection via ERR_SP trick (works on non-divMMC hardware)
- **Ring buffer expanded**: 256 -> 2048 bytes for reliable high-speed transfers

### Bug Fixes
- **esxDOS stack bug**: `esx_fopen_write/read`, `esx_opendir` didn't pop filename on GETSETDRV fail
- **detect_esxdos IX preservation**: sccz80 frame pointer now saved/restored correctly
- **OPEN ignoring port number**: Was hardcoded to port 21, now parses `host:port`
- **parse_host_port_path NULL crash**: Added guard for NULL `out_path` parameter
- **!CONNECT post-login check**: Changed from `==` to `>=` STATE_FTP_CONNECTED
- **cmd_cd missing drain**: Added `uart_drain_to_buffer()` in wait loop
- **utf8_to_ascii_inplace**: Fixed crash on truncated UTF-8 sequences
- **strncpy dependency eliminated**: Replaced with ASM `st_copy_n`
- **Dead code removed**: `timeout` variable, `transfer_started`, unused BSS arrays

### Improvements
- **UART**: Ring buffer with 2048B and adaptive drain modes (NORMAL/FAST)
- **Rendering**: `print_line64_fast` 3-4x faster for full lines
- **Keyboard**: Faster repeat rates (40ms normal, 20ms repeat)
- **Status bar**: Repaint only on change, eliminates flicker
- **Memory**: ORG 24000, 256-byte stack, ~1,070 bytes free margin
- **Build system**: Full Makefile with CHECK/CLEAN/BUILD/TRIM/INFO pipeline, color output, spinner

### Code Size
- divMMC TAP: ~36.8 KB
- AY TAP: ~37.4 KB

---


## [1.1.0] - 2026-01-09

### Mejoras de UART y Conectividad
- **Ring buffer ampliado**: 256 -> 512 bytes
  - Mejor manejo de rafagas de datos del ESP
  - Reduce perdida de caracteres en transferencias
- **Driver UART optimizado**:
  - Eliminado bug critico con instruccion `exx`
  - `send_block` optimizado para transferencias mas rapidas
  - Nueva funcion `ready_fast` para polling eficiente
- **Deteccion de timeout mejorada**:
  - Fix critico: mensajes 421 (timeout FTP) ahora detectados correctamente
  - Problema: consumo agresivo de ring buffer durante parsing de IP WiFi
  - Solucion: drenado selectivo y timing ajustado

### Protocolo FTP y Comandos
- **PWD robusto**:
  - Timeout ampliado: 4s -> 8s para servidores lentos
  - Retry automatico tras primer intento fallido
  - Funcion `cmd_pwd_silent()` para uso interno
- **LIST/NLST mejorado**:
  - Espera explicita de respuesta "150" antes de abrir data port
  - Parsing IPD mas robusto ante respuestas fragmentadas
  - Manejo de listados consecutivos sin fallos
  - Listado con nombre de ficheros largos mejorado
- **GET con soporte de comillas**:
  - Sintaxis: `GET "Manual del Usuario.pdf"`
  - Parsing robusto de argumentos con espacios
  - Mantiene compatibilidad con nombres sin espacios
- **USER con deteccion de sesion**:
  - Avisa si ya existe sesion activa
  - Previene intentos de re-login accidentales

### Interfaz de Usuario
- **Renderizado optimizado**:
  - `print_line64_fast`: 3-4x mas rapido en lineas completas
  - Fast-path activado cuando: inicio de linea + texto cabe en 64 cols
  - Mejora notable en listados largos y mensajes de servidor
- **Lineas horizontales de 1 pixel**:
  - Headers en LS/LIST con linea superior eliminada
  - Separadores visuales con scanline 1 (mas fino)
  - Correccion de posicionamiento (row vs scanline)
- **Mejoras de login UX**:
  - Flujo de mensajes mas claro durante autenticacion
  - Estados visuales mejor definidos
  - Feedback inmediato en cada paso
- **Comandos HELP/ABOUT**:
  - Formato mejorado y mas legible
  - Informacion organizada por categorias
  - Ejemplos de uso anadidos
- **Barra de estado**:
  - Repintado optimizado (solo cuando cambia)
  - Indicadores WiFi/FTP mas precisos
  - Evita parpadeo innecesario

### Optimizacion de Codigo
- **Reduccion de tamano (~1.2KB total)**:
  - Strings compartidos: ~709 bytes
  - Dead code eliminado: ~430 bytes
  - Refactorizacion `fail()` helper: ~175 bytes
  - `format_size` simplificado: ~70 bytes
  - Constantes adicionales: ~135 bytes
- **Mejoras estructurales**:
  - Helper functions mejor organizadas
  - Codigo mas mantenible y legible
  - Menor footprint de stack

### Teclado y Entrada
- **Timers optimizados**:
  - Teclas normales: delay reducido 120ms -> 40ms
  - BACKSPACE/cursores: delay inicial 100ms -> 60ms
  - Repeticion: 40ms -> 20ms (mas rapida)
- **Mejor respuesta**:
  - Captura correcta de pulsaciones rapidas
  - Navegacion mas agil en historial

### Soporte UTF-8 y Encoding
- **Escape sequences UTF-8**:
  - Conversion de secuencias %XX a caracteres
  - Mejora legibilidad en nombres con acentos
- **Parsing robusto**:
  - Manejo de tokens con comillas
  - Soporte de caracteres especiales
  - Compatible con rutas internacionales

### Monitoreo y Estabilidad
- **Connection alive detection**:
  - Deteccion de cierre remoto (0,CLOSED)
  - Deteccion de mensajes 421 (timeout)
  - Limpieza automatica de estado FTP
- **Manejo de errores**:
  - Uso sistematico de `fail()` en toda la codebase
  - Mensajes de error consistentes y claros
  - Recovery automatico tras fallos

### Debug y Diagnostico
- **Modo debug mejorado**:
  - Fix: STATUS no cuelga en debug mode
  - Output mas limpio y legible
  - Mejor sincronizacion con frames

---


## [1.0.0] - 2025-12-25

### Added
- Initial public release
- Full FTP client functionality with passive mode transfers
- 64-column display with color-coded output
- Standard FTP commands: OPEN, USER, PWD, CD, LS, GET, QUIT
- Quick connect command: `!CONNECT host/path user [pass]`
- File search with pattern and size filtering: `!SEARCH`
- Progress bar with file size display during transfers
- Connection status monitoring with automatic timeout detection
- Command history navigation (UP/DOWN arrows, 4 entries)
- Cursor movement in input line (LEFT/RIGHT arrows)
- Cancellation support with BREAK key for all operations
- Status bar showing host, user, path, and connection indicator
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
- Added file download (RETR) support
- Integrated timeout handling and error recovery

#### Phase 3: User Interface
- Added color-coded output system
- Implemented status bar with connection info
- Added progress bar for downloads
- Built command history and line editing
- Added HELP and diagnostic commands

## Versioning

This project uses [Semantic Versioning](https://semver.org/):
- **MAJOR**: Incompatible architectural changes
- **MINOR**: New features and significant improvements
- **PATCH**: Bug fixes only

[1.3.0]: https://github.com/IgnacioMonge/BitStream/releases/tag/v1.3.0
[1.2.0]: https://github.com/IgnacioMonge/BitStream/releases/tag/v1.2.0
[1.1.0]: https://github.com/IgnacioMonge/BitStream/releases/tag/v1.1.0
[1.0.0]: https://github.com/IgnacioMonge/BitStream/releases/tag/v1.0.0
