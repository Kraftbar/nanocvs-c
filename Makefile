CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
LDFLAGS ?=
LDLIBS ?= -lsqlite3

OBJS = main.o db.o scan.o history.o util.o sha256.o diff.o commands.o
BIN = nanocvs-c

all: config.h $(BIN)

config.h:
	cp config.def.h config.h

$(BIN): config.h $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(OBJS) $(BIN) sqlite_test sqlite_test.o

.PHONY: all clean
