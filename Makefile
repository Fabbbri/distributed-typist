CC = cc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64 -Icommon
CFLAGS ?= -std=c11 -O2
CFLAGS += -Wall -Wextra -Wpedantic

.PHONY: all client server test clean

all: client server

client: bin/client

server: bin/server

bin/client: client/client.c client/client.h common/protocol.c common/protocol.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) client/client.c common/protocol.c $(LDFLAGS) $(LDLIBS) -o $@

bin/server: server/server.c server/server.h common/protocol.c common/protocol.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) server/server.c common/protocol.c $(LDFLAGS) $(LDLIBS) -o $@

bin/tcpPeer: tests/tcpPeer.c common/protocol.c common/protocol.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/tcpPeer.c common/protocol.c $(LDFLAGS) $(LDLIBS) -o $@

bin:
	mkdir -p $@

test: all bin/tcpPeer
	bash tests/run.sh

clean:
	rm -f bin/client bin/server bin/tcpPeer
