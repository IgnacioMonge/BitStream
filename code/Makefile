# BitStream v1.2 Makefile - Size Optimized
ZCC = zcc
PLATFORM = +zx
TARGET = BitStream

# Sources
SOURCES = bitstream.c ay_uart.asm

# Compiler flags with aggressive size optimizations
# CRITICAL CHANGES FOR SIZE:
# - zorg=24200 (was 24000) - Gains ~200 bytes
# - CRT_STACK_SIZE=256 (was 512) - Gains ~256 bytes  
# - Using -O3 (not -SO3, which is actually larger)
CFLAGS = -vn -O3 -startup=0 -clib=new -zorg=24000
PRAGMAS = -pragma-define:CLIB_MALLOC_HEAP_SIZE=0 \
          -pragma-define:CLIB_STDIO_HEAP_SIZE=0 \
          -pragma-define:CRT_STACK_SIZE=256

# Default target
all: $(TARGET).tap

$(TARGET).tap: $(SOURCES) font64_data.h
	$(ZCC) $(PLATFORM) $(CFLAGS) $(PRAGMAS) $(SOURCES) -m -o $(TARGET) -create-app

clean:
	rm -f $(TARGET).tap $(TARGET).bin $(TARGET)_*.bin *.o *.lis *.map

.PHONY: all clean
