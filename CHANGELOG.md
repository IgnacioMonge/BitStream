# Changelog

All notable changes to BitStream are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).



## [1.1.0] - 2026-01-09

### Mejoras de UART y Conectividad
- **Ring buffer ampliado**: 256 → 512 bytes
  - Mejor manejo de ráfagas de datos del ESP
  - Reduce pérdida de caracteres en transferencias
- **Driver UART optimizado**:
  - Eliminado bug crítico con instrucción `exx`
  - `send_block` optimizado para transferencias más rápidas
  - Nueva función `ready_fast` para polling eficiente
- **Detección de timeout mejorada**:
  - Fix crítico: mensajes 421 (timeout FTP) ahora detectados correctamente
  - Problema: consumo agresivo de ring buffer durante parsing de IP WiFi
  - Solución: drenado selectivo y timing ajustado

### Protocolo FTP y Comandos
- **PWD robusto**: 
  - Timeout ampliado: 4s → 8s para servidores lentos
  - Retry automático tras primer intento fallido
  - Función `cmd_pwd_silent()` para uso interno
- **LIST/NLST mejorado**:
  - Espera explícita de respuesta "150" antes de abrir data port
  - Parsing IPD más robusto ante respuestas fragmentadas
  - Manejo de listados consecutivos sin fallos
  - Listado con nombre de ficheros largos mejorado.
- **GET con soporte de comillas**:
  - Sintaxis: `GET "Manual del Usuario.pdf"`
  - Parsing robusto de argumentos con espacios
  - Mantiene compatibilidad con nombres sin espacios
- **USER con detección de sesión**:
  - Avisa si ya existe sesión activa
  - Previene intentos de re-login accidentales

### Interfaz de Usuario
- **Renderizado optimizado**:
  - `print_line64_fast`: 3-4x más rápido en líneas completas
  - Fast-path activado cuando: inicio de línea + texto cabe en 64 cols
  - Mejora notable en listados largos y mensajes de servidor
- **Líneas horizontales de 1 pixel**:
  - Headers en LS/LIST con línea superior eliminada
  - Separadores visuales con scanline 1 (más fino)
  - Corrección de posicionamiento (row vs scanline)
- **Mejoras de login UX**:
  - Flujo de mensajes más claro durante autenticación
  - Estados visuales mejor definidos
  - Feedback inmediato en cada paso
- **Comandos HELP/ABOUT**:
  - Formato mejorado y más legible
  - Información organizada por categorías
  - Ejemplos de uso añadidos
- **Barra de estado**:
  - Repintado optimizado (solo cuando cambia)
  - Indicadores WiFi/FTP más precisos
  - Evita parpadeo innecesario

### Optimización de Código
- **Reducción de tamaño (~1.2KB total)**:
  - Strings compartidos: ~709 bytes
    - Constantes S_IPD0, S_CRLF, S_CANCEL, S_DOTS, etc.
    - Mensajes de error unificados
  - Dead code eliminado: ~430 bytes
    - Funciones no utilizadas removidas
    - Código inalcanzable limpiado
  - Refactorización `fail()` helper: ~175 bytes
    - Error handling sistemático
    - Reduce duplicación de código
  - `format_size` simplificado: ~70 bytes
    - Lógica de formateo más directa
  - Constantes adicionales: ~135 bytes
    - S_CHECKING, S_AT_CIPMUX añadidas
    - Eliminación de función `draw_fullwidth_hline`
- **Mejoras estructurales**:
  - Helper functions mejor organizadas
  - Código más mantenible y legible
  - Menor footprint de stack

### Teclado y Entrada
- **Timers optimizados**:
  - Teclas normales: delay reducido 120ms → 40ms
  - BACKSPACE/cursores: delay inicial 100ms → 60ms
  - Repetición: 40ms → 20ms (más rápida)
- **Mejor respuesta**:
  - Captura correcta de pulsaciones rápidas
  - Ideal para mecanógrafos experimentados
  - Navegación más ágil en historial

### Soporte UTF-8 y Encoding
- **Escape sequences UTF-8**:
  - Conversión de secuencias %XX a caracteres
  - Ejemplos: %C3%AD → í, %C3%B1 → ñ
  - Mejora legibilidad en nombres con acentos
- **Parsing robusto**:
  - Manejo de tokens con comillas
  - Soporte de caracteres especiales
  - Compatible con rutas internacionales

### Monitoreo y Estabilidad
- **Connection alive detection**:
  - Detección de cierre remoto (0,CLOSED)
  - Detección de mensajes 421 (timeout)
  - Limpieza automática de estado FTP
- **Manejo de errores**:
  - Uso sistemático de `fail()` en toda la codebase
  - Mensajes de error consistentes y claros
  - Recovery automático tras fallos

### Debug y Diagnóstico
- **Modo debug mejorado**:
  - Fix: STATUS no cuelga en debug mode
  - Output más limpio y legible
  - Mejor sincronización con frames

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

[1.0.0]: https://github.com/imnacio/bitstream/releases/tag/v1.0.0



