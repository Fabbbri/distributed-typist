CC = cc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64 -Icommon
CFLAGS ?= -std=c11 -O2
CFLAGS += -Wall -Wextra -Wpedantic

ADDRESS ?= 127.0.0.1
PORT ?= 5000
INPUT_DIR ?= input
OUTPUT_FILE ?= output/recibido.txt

.PHONY: all client server test clean

all: bin/client bin/server

client: bin/client
	exec ./bin/client --interactive "$(ADDRESS)" "$(PORT)" "$(INPUT_DIR)"

server: bin/server
	mkdir -p "$$(dirname -- "$(OUTPUT_FILE)")"
	exec ./bin/server "$(ADDRESS)" "$(PORT)" "$(OUTPUT_FILE)"

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
