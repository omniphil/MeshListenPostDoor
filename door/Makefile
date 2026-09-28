# Builds the Listening Post door (lpdoor). Runs on the BBS box: it reads what the
# collector keeps and draws the ANSI view. Needs gcc only.
MAKEFLAGS += -r
.SUFFIXES:

# Sibling folders on a dev machine, ./native after `make bundle` for the BBS.
MODSRC = $(firstword $(wildcard ../module/src native/module/src))
COMMON = $(firstword $(wildcard ../common native/common))

CC     ?= gcc
CFLAGS ?= -O2 -std=gnu99 -Wall -Wextra -Wno-unused-function -I $(MODSRC) -I $(COMMON) -I .

SRC = main.c lp.c ansi_lp.c door.c sha256.c trace_door.c
OBJ = $(SRC:%.c=obj/%.o)

TARGET = lpdoor

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $^ -lm -o $@
	@ls -l $@

obj/%.o: %.c $(MODSRC)/protocol.h $(COMMON)/geo.h $(COMMON)/lpdata.h $(COMMON)/hwmodels.h lp.h
	@mkdir -p obj
	$(CC) $(CFLAGS) -c $< -o $@

# Snapshot the shared headers so door/ compiles on the BBS with nothing beside it.
bundle:
	rm -rf native || (sleep 2; rm -rf native)
	mkdir -p native/module/src native/common
	cp ../module/src/protocol.h native/module/src/
	cp ../common/geo.h ../common/lpdata.h ../common/hwmodels.h native/common/
	@echo "bundled"

clean:
	rm -rf obj $(TARGET)

.PHONY: all bundle clean
