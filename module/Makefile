# Builds lp.wasm, the Listening Post's TRACE module -- the picture half of the door.
# Needs wasi-sdk (34 or later); override with WASI_SDK=... if yours lives elsewhere.
MAKEFLAGS += -r
.SUFFIXES:

WASI_SDK ?= $(HOME)/tools/wasi-sdk-34.0-x86_64-linux
CC        = $(WASI_SDK)/bin/clang

TRACE_INCLUDE = $(firstword $(wildcard ../../../TERMinator-Windows/trace/include) src)

CFLAGS  = --target=wasm32-wasip1-threads -O2 -DNDEBUG -std=gnu99 -Wall -Wextra -Wno-unused-function \
          -pthread -I src -I ../common -I $(TRACE_INCLUDE)
LDFLAGS = -mexec-model=reactor \
          -Wl,--import-memory,--export-memory,--shared-memory,--max-memory=$(MAX_MEMORY) \
          -Wl,--export=wasi_thread_start -Wl,--export-dynamic -Wl,--strip-all -lm

# 48 MB: the frame at 2x (4.9 MB) and its shrunk copy, the base map and its id buffer, the nodes.
MAX_MEMORY = 50331648

SRC  = src/lpmodule.c
DEPS = $(SRC) src/gfx.h src/protocol.h src/fonts.h ../common/geo.h ../common/hwmodels.h

all: lp.wasm

# Uses trace_mouse_mode, so it needs TERMinator 1.1.3+. The door only offers
# TRACE when the Query reply says mouse=1.
lp.wasm: $(DEPS)
	$(CC) $(CFLAGS) $(SRC) $(LDFLAGS) -o $@
	@ls -l $@

# The module with made-up nodes and the outlines compiled in, for looking at it without a BBS.
src/states_blob.h: ../data/states.bin ../tools/bin2h.py
	python3 ../tools/bin2h.py ../data/states.bin $@ k_states_blob

lpdemo.wasm: $(DEPS) src/demo.h src/states_blob.h
	$(CC) $(CFLAGS) -DLP_DEMO $(SRC) $(LDFLAGS) -o $@
	@ls -l $@

clean:
	rm -f lp.wasm lpdemo.wasm src/states_blob.h

.PHONY: all clean
