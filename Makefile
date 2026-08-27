UNAME := $(shell uname -s)
CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -O2
CFLAGS += -Iinclude -Isrc
LDFLAGS ?=

ifeq ($(UNAME),Darwin)
  CFLAGS += -D_DARWIN_C_SOURCE
else
  CFLAGS += -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64
endif

SRCS := src/main.c src/device.c src/util.c src/journal.c src/fs.c \
        src/fat32.c src/exfat.c src/cmd_check.c src/cmd_defrag.c src/cmd_copy.c
OBJS := $(SRCS:.c=.o)

.PHONY: all clean test

all: fatdefrag

fatdefrag: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

tests/mkfat32: tests/mkfat32.c
	$(CC) $(CFLAGS) -o $@ tests/mkfat32.c

tests/mkexfat: tests/mkexfat.c
	$(CC) $(CFLAGS) -o $@ tests/mkexfat.c

tests/plantjnl: tests/plantjnl.c src/journal.c src/util.c src/device.c
	$(CC) $(CFLAGS) -o $@ tests/plantjnl.c src/journal.c src/util.c src/device.c

clean:
	rm -f fatdefrag $(OBJS) tests/mkfat32 tests/mkexfat tests/plantjnl tests/*.img

test: fatdefrag tests/mkfat32 tests/mkexfat tests/plantjnl
	sh tests/run.sh
