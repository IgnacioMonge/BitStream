// ============================================================================
// main_build.c - Single Compilation Unit (SCU) orchestrator
// ============================================================================
// All .c files are #included here so the compiler can:
//   - Inline across module boundaries
//   - Eliminate dead code globally
//   - Deduplicate string constants
//
// Include order matters: each module can see symbols from all previous ones.
// Forward declarations in globals.c / bitstream_net.h bridge references to
// later modules.
//
// Platform selection (Makefile):
//   classic      ESP8266 over divMMC UART (-DDIVMMC_UART) or AY bit-bang
//   next         -DBITSTREAM_NEXT: ESP8266 over the Next UART (asm/next_uart.asm)
//   spectranext  -DBITSTREAM_SPECTRANEXT: cartridge sockets + XFS
// ============================================================================

// 1. Global state, constants, keyboard ASM, common strings
#include "globals.c"

// 2. Video, widgets, input zone, keyboard
#include "ui.c"

// 3. Ring buffer, line state, timing helpers
#include "comms.c"

// 4. Transport + storage backends (bitstream_net.h seam)
#ifdef BITSTREAM_SPECTRANEXT
#include "net_spectranext.c"
#include "fs_spectranext.c"
#else
#include "net_esp.c"
#include "fs_esx.c"
#endif

// 5. FTP protocol, download, list
#include "ftp.c"

// 6. Command parser, help, status, special commands
#include "commands.c"

// 7. Screen initialization, main loop
#include "main.c"
