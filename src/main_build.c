// ============================================================================
// main_build.c - Single Compilation Unit (SCU) orchestrator
// ============================================================================
// All .c files are #included here so the compiler can:
//   - Inline across module boundaries
//   - Eliminate dead code globally
//   - Deduplicate string constants
// This saves ~200+ bytes vs separate compilation on Z80.
//
// Include order matters: each module can see symbols from all previous ones.
// Forward declarations in globals.c bridge references to later modules.
// ============================================================================

// 1. Global state, constants, keyboard ASM, common strings
#include "globals.c"

// 2. Video, widgets, input zone, keyboard
#include "ui.c"

// 3. Ring buffer, UART helpers, ESP layer, TCP
#include "comms.c"

// 4. FTP protocol, esxDOS, download, list
#include "ftp.c"

// 5. Command parser, help, status, special commands
#include "commands.c"

// 6. Screen initialization, main loop
#include "main.c"
