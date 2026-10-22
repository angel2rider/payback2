# Payback 2 native port — build
CC      = clang
CFLAGS  = -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
          -fno-stack-protector -arch arm64 -mmacosx-version-min=13.0
LDFLAGS = -arch arm64 -Wl,-no_fixup_chains

SRC_C = src/main.c src/elf_load.c src/shims.c src/gvarargs.c src/stdio.c src/jni_env.c src/pthread_shim.c src/android_shim.c src/egl_shim.c src/opensles_shim.c
SRC_M = src/window.m
SRC_S = src/gstubs.s

pbnative: $(SRC_C) $(SRC_M) $(SRC_S) src/pb.h src/jni_slots.h src/oracle_holes.h
	$(CC) $(CFLAGS) $(SRC_C) $(SRC_M) $(SRC_S) -o $@ $(LDFLAGS) -framework Cocoa -framework QuartzCore

clean:
	rm -f pbnative

.PHONY: clean
