ifeq ($(origin CC),default)
CC = gcc
endif

BIN      = winshatter.exe
SRC      = winshatter.c

CFLAGS   = -std=c11 -Wall -Wextra -Werror -Wshadow -Wpedantic \
           -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DCOBJMACROS
LDFLAGS  = -luser32 -ldwmapi -ld3d11 -ldxgi -ldcomp -luuid -ldxguid

all: CFLAGS += -O2
all: LDFLAGS += -mwindows
all: $(BIN)

debug: CFLAGS += -g -O0 -DDEBUG
debug: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(BIN) $(LDFLAGS)

run: all
	./$(BIN)

clean:
	rm -f $(BIN)

.PHONY: all debug run clean
