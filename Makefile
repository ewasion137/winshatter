SHELL := cmd.exe

ifeq ($(origin CC),default)
CC = gcc
endif

BIN      = winshatter.exe
SRC      = winshatter.c

CFLAGS   = -std=c11 -Wall -Wextra -Werror -Wshadow -Wpedantic \
           -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNTDDI_VERSION=0x0A000006 \
           -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DCOBJMACROS
LDFLAGS  = -luser32 -ldwmapi -ld3d11 -ldxgi -ldcomp -ld3dcompiler -luuid -ldxguid

all: CFLAGS += -O2
all: LDFLAGS += -mwindows
all: $(BIN)

debug: CFLAGS += -g -O0 -DDEBUG
debug: $(BIN)

$(BIN): $(SRC) physics.h Makefile
	$(CC) $(CFLAGS) $(SRC) -o $(BIN) $(LDFLAGS)

run: all
	$(BIN)

clean:
	if exist $(BIN) del /q $(BIN)

.PHONY: all debug run clean
