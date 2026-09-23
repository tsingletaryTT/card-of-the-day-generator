# Card of the Day — GTK4 trading-card app
#
# Build:  make
# Run:    ./card-of-the-day
# Clean:  make clean

CC      ?= cc
PKG     ?= pkg-config
PKGS     = gtk4 librsvg-2.0 json-glib-1.0

CFLAGS  += $(shell $(PKG) --cflags $(PKGS)) -O2 -Wall -Wextra
LDLIBS  += $(shell $(PKG) --libs $(PKGS)) -lm

TARGET   = card-of-the-day
SRC      = src/main.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all run clean
