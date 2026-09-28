# Builds lpcollect, the Listening Post's collector. Runs on the BBS box; needs only gcc.
MAKEFLAGS += -r
.SUFFIXES:
COMMON = $(firstword $(wildcard ../common native/common))
CC     ?= gcc
CFLAGS ?= -O2 -std=gnu99 -Wall -Wextra -Wno-unused-function -I $(COMMON)

lpcollect: lpcollect.c aes.c aes.h $(COMMON)/geo.h $(COMMON)/lpdata.h
	$(CC) $(CFLAGS) lpcollect.c aes.c -lm -o $@
	@./$@ --selftest >/dev/null && echo "lpcollect: AES self-test ok"

clean:
	rm -f lpcollect

.PHONY: clean
