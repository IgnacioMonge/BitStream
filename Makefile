# ============================================================
# BitStream Makefile - FTP Client for ZX Spectrum
# Default pipeline: CHECK -> CLEAN -> BUILD -> TRIM -> INFO
# Supports: divMMC/divTiesus UART (115200) and AY bit-bang (9600)
# ============================================================

.DEFAULT_GOAL := all

# ------------------------------------------------------------
# Toolchain / target
# ------------------------------------------------------------
CC      = zcc
TARGET  = +zx

# ------------------------------------------------------------
# Project
# ------------------------------------------------------------
OUTPUT    = BitStream
BUILD_DIR = build
LOG       = $(BUILD_DIR)/build.log

# ------------------------------------------------------------
# Sources
# ------------------------------------------------------------
C_SOURCES   = src/main_build.c
ASM_COMMON  = asm/bitstream_asm.asm
ASM_DIVMMC  = asm/divtiesus_uart.asm
ASM_AY      = asm/ay_uart.asm

# ------------------------------------------------------------
# Build options
# ------------------------------------------------------------
AY_UART     ?= 0
ZORG        = 24000
STACK_SIZE  = 256

# Determine UART-specific settings
ifeq ($(AY_UART),1)
  UART_FLAG   = -DAY_UART
  ASM_SOURCES = $(ASM_COMMON) $(ASM_AY)
  UART_DESC   = AY bit-bang (9600 baud)
  OUTPUT_NAME = $(OUTPUT)_AY
else
  UART_FLAG   = -DDIVMMC_UART
  ASM_SOURCES = $(ASM_COMMON) $(ASM_DIVMMC)
  UART_DESC   = divMMC/divTiesus (115200 baud)
  OUTPUT_NAME = $(OUTPUT)_divTiesus
endif

TAP = $(OUTPUT_NAME).tap
MAP = $(OUTPUT_NAME).map

EXTRA_CFLAGS  ?=
BUILD_PROFILE ?= NORMAL

CFLAGS = -vn -O3 -startup=0 -clib=new \
         -zorg=$(ZORG) --opt-code-size \
         $(UART_FLAG) \
         -pragma-define:CLIB_MALLOC_HEAP_SIZE=0 \
         -pragma-define:CLIB_STDIO_HEAP_SIZE=0 \
         -pragma-define:CRT_STACK_SIZE=$(STACK_SIZE) \
         $(EXTRA_CFLAGS)

SIZE_TAP  = wc -c < "$(TAP)"
BUILD_CMD = $(CC) $(TARGET) $(CFLAGS) $(C_SOURCES) $(ASM_SOURCES) -m -o $(OUTPUT_NAME) -create-app

# ------------------------------------------------------------
# ANSI colors (disable with NO_COLOR=1)
# ------------------------------------------------------------
ifeq ($(NO_COLOR),1)
C_RESET :=
C_BOLD  :=
C_DIM   :=
C_RED   :=
C_GRN   :=
C_YEL   :=
C_BLU   :=
C_CYNB  :=
else
C_RESET := \\033[0m
C_BOLD  := \\033[1m
C_DIM   := \\033[2m
C_RED   := \\033[31m
C_GRN   := \\033[32m
C_YEL   := \\033[33m
C_BLU   := \\033[34m
C_CYNB  := \\033[96m
endif

# ------------------------------------------------------------
# Pretty printing
# ------------------------------------------------------------
define HR
	@printf "$(C_DIM)============================================================$(C_RESET)\n"
endef

define STEP
	@printf "$(C_BOLD)$(C_BLU)[%s]$(C_RESET) %s\n" "$(1)" "$(2)"
endef

define OK
	@printf "$(C_GRN)[OK]$(C_RESET) %s\n" "$(1)"
endef

define WARN
	@printf "$(C_YEL)[WARN]$(C_RESET) %s\n" "$(1)"
endef

define ERR
	@printf "$(C_RED)[ERR]$(C_RESET) %s\n" "$(1)"
endef

# ------------------------------------------------------------
# Spinner (POSIX sh)
# ------------------------------------------------------------
define RUN_SPINNER
	@sh -c 'i=0; printf "\033[?25l"; spin() { while :; do case $$((i % 4)) in 0) c="|" ;; 1) c="/" ;; 2) c="-" ;; 3) c="\\" ;; esac; i=$$((i+1)); printf "\033[?25l\r\033[K$(C_YEL)%s$(C_RESET) %s" "$(2)" "$$c"; sleep 0.12; done }; spin & spid=$$!; trap "kill $$spid 2>/dev/null; wait $$spid 2>/dev/null; printf \"\r\033[K\033[?25h\"; exit 130" INT TERM; ( $(1) ); rc=$$?; kill $$spid 2>/dev/null; wait $$spid 2>/dev/null; printf "\r\033[K\033[?25h"; if [ "$$rc" = "0" ]; then printf "$(C_GRN)%s$(C_RESET)\n" "$(3)"; else printf "$(C_RED)%s$(C_RESET)\n" "$(4)"; fi; exit "$$rc"'
endef

# ------------------------------------------------------------
# Phony targets
# ------------------------------------------------------------
.PHONY: all check clean build trim info help ay divmmc both release version

# ------------------------------------------------------------
# Default pipeline
# ------------------------------------------------------------
all: check clean build trim info

# Convenience targets
ay:
	@$(MAKE) AY_UART=1 all
divmmc:
	@$(MAKE) AY_UART=0 all
both:
	@$(MAKE) divmmc
	@$(MAKE) ay

help:
	$(call HR)
	@printf "$(C_BOLD)$(C_CYNB)BitStream - FTP Client for ZX Spectrum$(C_RESET)\n"
	$(call HR)
	@printf "Targets:\n"
	@printf "  make            Build divMMC version (default)\n"
	@printf "  make ay         Build AY bit-bang version\n"
	@printf "  make divmmc     Build divMMC version\n"
	@printf "  make both       Build both versions\n"
	@printf "  make release    Release build (aggressive optimization)\n"
	@printf "  make check      Preflight dependency checks\n"
	@printf "  make clean      Remove build artifacts\n"
	@printf "  make info       Print build info\n"
	@printf "  make version    Save current build as version snapshot\n"
	@printf "\nOptions:\n"
	@printf "  NO_COLOR=1      Disable ANSI colors\n"
	@printf "  AY_UART=1       Target AY bit-bang UART\n"
	$(call HR)

# ------------------------------------------------------------
# CHECK phase
# ------------------------------------------------------------
check:
	$(call HR)
	@printf "$(C_BOLD)$(C_CYNB)BitStream - Build Pipeline$(C_RESET)\n"
	$(call HR)
	$(call STEP,0/3,Checking toolchain and sources)
	@mkdir -p $(BUILD_DIR)
	@sh -c '\
		fail=0; \
		for t in zcc wc sh; do \
			command -v "$$t" >/dev/null 2>&1 || { echo "[ERR] Missing tool: $$t"; fail=1; }; \
		done; \
		for f in $(C_SOURCES) $(ASM_SOURCES); do \
			[ -f "$$f" ] || { echo "[ERR] Missing file: $$f"; fail=1; }; \
		done; \
		[ "$$fail" = "0" ] || exit 2; \
	'
	$(call OK,Dependencies OK)
	$(call HR)

# ------------------------------------------------------------
# CLEAN phase
# ------------------------------------------------------------
clean:
	$(call STEP,1/3,Cleaning)
	@rm -f $(OUTPUT)_*.tap $(OUTPUT)_*.map $(OUTPUT)_*_CODE.bin $(OUTPUT)_divTiesus $(OUTPUT)_AY $(LOG) *.o *.bin *.sym 2>/dev/null || true
	$(call OK,Clean complete.)
	$(call HR)

# ------------------------------------------------------------
# BUILD phase
# ------------------------------------------------------------
build: $(TAP)

$(TAP): $(C_SOURCES) $(ASM_SOURCES) include/bitstream.h include/font64_data.h
	$(call STEP,2/3,Build)
	@echo "Compiling BitStream..."
	@echo "UART mode: $(UART_DESC)"
	@echo "Log: $(LOG)"
	$(call RUN_SPINNER,$(BUILD_CMD) 2>&1 | tee "$(LOG)",Compiling...,Build complete!,BUILD FAILED - see $(LOG))
	$(call HR)

# ------------------------------------------------------------
# TRIM phase - strip BSS zeros from TAP
# ------------------------------------------------------------
trim: $(TAP) $(MAP)
	@sh -c ' \
	  bss=$$(grep "__data_compiler_tail" $(MAP) | head -1 | sed "s/.*= .\\([0-9A-Fa-f]*\\).*/\\1/"); \
	  if [ -z "$$bss" ]; then \
	    printf "$(C_YEL)[WARN]$(C_RESET) BSS trim skipped (symbol not found in map)\n"; \
	    exit 0; \
	  fi; \
	  trim=$$((0x$$bss - $(ZORG))); \
	  bin="$(OUTPUT_NAME)_CODE.bin"; \
	  if [ ! -f "$$bin" ]; then bin="$(OUTPUT_NAME)"; fi; \
	  if [ ! -f "$$bin" ]; then \
	    printf "$(C_YEL)[WARN]$(C_RESET) BSS trim skipped (binary not found)\n"; \
	    exit 0; \
	  fi; \
	  full=$$(wc -c < "$$bin"); \
	  saved=$$((full - trim)); \
	  if [ "$$saved" -le 0 ]; then \
	    printf "$(C_YEL)[WARN]$(C_RESET) BSS trim skipped (nothing to trim)\n"; \
	    exit 0; \
	  fi; \
	  head -c $$trim "$$bin" > $(BUILD_DIR)/trimmed.bin; \
	  z88dk-appmake +zx -b $(BUILD_DIR)/trimmed.bin --org $(ZORG) -o $(TAP) 2>/dev/null; \
	  rm -f $(BUILD_DIR)/trimmed.bin; \
	  printf "$(C_GRN)[OK]$(C_RESET) BSS trimmed: %d -> %d bytes (-%d bytes of zeros)\n" "$$full" "$$trim" "$$saved"; \
	'

# ------------------------------------------------------------
# INFO phase
# ------------------------------------------------------------
info: $(TAP)
	$(call STEP,3/3,Info)
	@printf "$(C_BOLD)Output:$(C_RESET)      $(C_YEL)%s$(C_RESET)\n" "$(TAP)"
	@printf "$(C_BOLD)Memory map:$(C_RESET)  $(C_YEL)%s$(C_RESET)\n" "$(MAP)"
	@printf "$(C_BOLD)Build log:$(C_RESET)   $(C_YEL)%s$(C_RESET)\n" "$(LOG)"
	@printf "$(C_BOLD)UART mode:$(C_RESET)   $(C_YEL)%s$(C_RESET)\n" "$(UART_DESC)"
	@printf "$(C_BOLD)Code origin:$(C_RESET) $(C_YEL)%s$(C_RESET)\n" "$(ZORG)"
	@printf "$(C_BOLD)Stack size:$(C_RESET)  $(C_YEL)%s bytes$(C_RESET)\n" "$(STACK_SIZE)"
	@printf "$(C_BOLD)TAP size:$(C_RESET)    $(C_YEL)%s bytes$(C_RESET)\n" "$$($(SIZE_TAP))"
	$(call HR)

# ------------------------------------------------------------
# Release build (aggressive optimization)
# ------------------------------------------------------------
release:
	@$(MAKE) BUILD_PROFILE=RELEASE EXTRA_CFLAGS=--max-allocs-per-node200000 all
release-ay:
	@$(MAKE) BUILD_PROFILE=RELEASE EXTRA_CFLAGS=--max-allocs-per-node200000 AY_UART=1 all

# ------------------------------------------------------------
# Version snapshot (save current build to Versions/)
# ------------------------------------------------------------
version:
	@sh -c ' \
	  ver=$$(grep "APP_VERSION" src/main_build.c include/bitstream.h src/bitstream.c 2>/dev/null | grep -o "\"[^\"]*\"" | head -1 | tr -d "\""); \
	  if [ -z "$$ver" ]; then ver="unknown"; fi; \
	  dir="Versions/v$$ver"; \
	  mkdir -p "$$dir"; \
	  cp -r src include asm Makefile "$$dir/" 2>/dev/null; \
	  for f in $(OUTPUT)_*.tap; do [ -f "$$f" ] && cp "$$f" "$$dir/"; done 2>/dev/null; \
	  printf "$(C_GRN)[OK]$(C_RESET) Version snapshot saved to %s\n" "$$dir"; \
	'
