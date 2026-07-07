CC ?= cc
PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config

CFLAGS += -std=gnu11 -Wall -Wextra -O2 -Iinclude
CFLAGS += $(shell $(PKG_CONFIG) --cflags libusb-1.0)
LIBS := $(shell $(PKG_CONFIG) --libs libusb-1.0) -lpthread

SRCS := $(wildcard src/*.c)
OBJS := $(SRCS:.c=.o)
BIN := eth-c-macos

.PHONY: all clean install uninstall

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(BIN)

install: $(BIN)
	install -d $(PREFIX)/bin
	install -m 0755 $(BIN) $(PREFIX)/bin/$(BIN)

uninstall:
	rm -f $(PREFIX)/bin/$(BIN)
