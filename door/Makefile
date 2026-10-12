# Builds the Listening Post door (lpdoor). Runs on the BBS box: it reads what the
# collector keeps, draws the ANSI view, and runs the TRACE module itself for the
# JPEG XL mode. Needs gcc; libjxl is loaded at run time if the box has it.
MAKEFLAGS += -r
.SUFFIXES:

# Sibling folders on a dev machine, ./native after `make bundle` for the BBS.
MODSRC = $(firstword $(wildcard ../module/src native/module/src))
COMMON = $(firstword $(wildcard ../common native/common))

CC     ?= gcc
CFLAGS ?= -O2 -std=gnu99 -Wall -Wextra -Wno-unused-function -I $(MODSRC) -I $(COMMON) -I .

SRC = main.c lp.c link.c ansi_lp.c door.c sha256.c trace_door.c apc.c
OBJ = $(SRC:%.c=obj/%.o) obj/jxl_enc.o obj/pix_lp.o

TARGET = lpdoor

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $^ -pthread -lm -ldl -o $@
	@ls -l $@

obj/%.o: %.c $(MODSRC)/protocol.h $(COMMON)/geo.h $(COMMON)/lpdata.h $(COMMON)/hwmodels.h lp.h
	@mkdir -p obj
	$(CC) $(CFLAGS) -c $< -o $@

# libjxl is loaded at run time (dlopen); only its headers are needed here.
obj/jxl_enc.o: jxl_enc.c
	@mkdir -p obj
	$(CC) $(CFLAGS) -I jxl_include -c $< -o $@

# The TRACE module itself, compiled in for the JPEG XL mode.
obj/pix_lp.o: pix_lp.c $(wildcard $(MODSRC)/*.h $(MODSRC)/lpmodule.c $(COMMON)/*.h) lp.h link.h
	@mkdir -p obj
	$(CC) -O2 -std=gnu99 -pthread -Wall -Wno-attributes -Wno-unused-function -Wno-format-truncation -Wno-unknown-pragmas \
		-I $(MODSRC) -I $(COMMON) -I . -c $< -o $@

# Snapshot the shared headers and the module source so door/ compiles on the BBS with nothing beside it.
bundle:
	rm -rf native || (sleep 2; rm -rf native)
	mkdir -p native/module/src native/common
	cp ../module/src/lpmodule.c ../module/src/protocol.h ../module/src/gfx.h ../module/src/fonts.h \
		../module/src/trace_api.h native/module/src/
	cp ../common/geo.h ../common/lpdata.h ../common/hwmodels.h native/common/
	@echo "bundled"

clean:
	rm -rf obj $(TARGET)

.PHONY: all bundle clean
