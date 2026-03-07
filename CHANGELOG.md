# Changelog

All notable changes to BitStream are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).



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


## [1.0.0] - 2025-25-12

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

[1.2.0]: https://github.com/IgnacioMonge/BitStream/releases/tag/v1.2.0
[1.1.0]: https://github.com/IgnacioMonge/BitStream/releases/tag/v1.1.0
[1.0.0]: https://github.com/IgnacioMonge/BitStream/releases/tag/v1.0.0
