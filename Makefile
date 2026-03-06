# Makefile -- Skyhawk J73 compiler
# For people who think CMake is a lifestyle choice.

CC       = gcc
WFLAGS   = -std=c99 -Wall -Wextra -Wpedantic -Werror \
           -Wshadow -Wconversion -Wstrict-prototypes \
           -Wmissing-prototypes -Wold-style-definition \
           -Wdouble-promotion -Wundef -Wno-unused-function \
           -Wformat=2 -Wswitch-enum -Wnull-dereference \
           -Wstack-usage=4096 \
           -fno-common -fstack-protector-strong \
           -Isrc
CFLAGS   = $(WFLAGS) -O2
TFLAGS   = $(WFLAGS) -O0
LDFLAGS  =

# ---- Runtime ---- #

SRC_RT   = src/rt/skrt.c
RTOBJ    = skrt.o

# ---- Sources ---- #

SRC_FE   = src/fe/lexer.c src/fe/parser.c src/fe/sema.c src/fe/layout.c
SRC_IR   = src/ir/jir_lower.c src/ir/jir_mem2reg.c
SRC_X86  = src/x86/x86_emit.c src/x86/x86_ra.c src/x86/x86_coff.c
SRC_RV   = src/rv/rv_emit.c src/rv/rv_ra.c src/rv/rv_elf.c
SRC_CPL  = src/cpl/cpl_write.c src/cpl/cpl_read.c
SRC_MAIN = src/main.c
SRCS     = $(SRC_MAIN) $(SRC_FE) $(SRC_IR) $(SRC_X86) $(SRC_RV) $(SRC_CPL)

# ---- Test sources ---- #

TSRC     = tests/tmain.c tests/tsmoke.c tests/tparse.c tests/tsema.c tests/tjir.c tests/tx86.c tests/trv.c \
           tests/tfloat.c tests/tnest.c tests/tstress.c tests/tavion.c
TSRCS    = $(TSRC) $(SRC_FE) $(SRC_IR) $(SRC_X86) $(SRC_RV) $(SRC_CPL)

# ---- Targets ---- #

BIN      = skyhawk
TBIN     = test_skyhawk

ifeq ($(OS),Windows_NT)
  BIN   := $(BIN).exe
  TBIN  := $(TBIN).exe
endif

.PHONY: all clean test

all: $(BIN)

$(BIN): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TBIN): $(TSRCS)
	$(CC) $(TFLAGS) -Itests -o $@ $^ $(LDFLAGS)

test: $(BIN) $(TBIN)
	./$(TBIN)

$(RTOBJ): $(SRC_RT)
	$(CC) -std=c99 -O2 -c -o $@ $<

clean:
	rm -f $(BIN) $(TBIN) *.o tests/fixtures/*.cpl tests/fixtures/*.obj tests/fixtures/*.elf tests/fixtures/*.o
