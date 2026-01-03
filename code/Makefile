# BitStream v1.0 Makefile
ZCC = zcc
PLATFORM = +zx
TARGET = BitStream

# Sources
SOURCES = bitstream.c ay_uart.asm

# Compiler flags with size optimizations
CFLAGS = -vn -SO3 -startup=0 -clib=new -zorg=24576
PRAGMAS = -pragma-define:CLIB_MALLOC_HEAP_SIZE=0 \
          -pragma-define:CLIB_STDIO_HEAP_SIZE=0 \
          -pragma-define:CRT_STACK_SIZE=512

# Default target
all: $(TARGET).tap

$(TARGET).tap: $(SOURCES) font64_data.h
	$(ZCC) $(PLATFORM) $(CFLAGS) $(PRAGMAS) $(SOURCES) -O3 -o $(TARGET) -create-app

clean:
	rm -f $(TARGET).tap $(TARGET).bin $(TARGET)_*.bin *.o *.lis

.PHONY: all clean
