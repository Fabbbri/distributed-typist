CC = cc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64 -Icommon
CFLAGS ?= -std=c11 -O2
CFLAGS += -Wall -Wextra -Wpedantic

ADDRESS ?= 127.0.0.1
PORT ?= 5000
INPUT_DIR ?= input
OUTPUT_FILE ?= output/cifrado.bin
KEY_FILE ?= keys/client.key

.PHONY: all client server keygen test clean

all: bin/client bin/server

client: bin/client
	TYPIST_KEY_FILE="$(KEY_FILE)" exec ./bin/client --interactive "$(ADDRESS)" "$(PORT)" "$(INPUT_DIR)"

server: bin/server
	mkdir -p "$$(dirname -- "$(OUTPUT_FILE)")"
	exec ./bin/server "$(ADDRESS)" "$(PORT)" "$(OUTPUT_FILE)"

keygen:
	mkdir -p "$$(dirname -- "$(KEY_FILE)")"
	(umask 077; set -C; openssl rand 32 >"$(KEY_FILE)")

bin/client: client/client.c client/client.h common/protocol.c common/protocol.h \
            common/crypto.c common/crypto.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) client/client.c common/protocol.c common/crypto.c \
		$(LDFLAGS) $(LDLIBS) -lcrypto -o $@

bin/server: server/server.c server/server.h common/protocol.c common/protocol.h common/crypto.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) server/server.c common/protocol.c $(LDFLAGS) $(LDLIBS) -o $@

bin/tcpPeer: tests/tcpPeer.c common/protocol.c common/protocol.h common/crypto.c common/crypto.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/tcpPeer.c common/protocol.c common/crypto.c \
		$(LDFLAGS) $(LDLIBS) -lcrypto -o $@

bin/cryptoCheck: tests/cryptoCheck.c common/crypto.c common/crypto.h common/protocol.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/cryptoCheck.c common/crypto.c \
		$(LDFLAGS) $(LDLIBS) -lcrypto -o $@

bin:
	mkdir -p $@

test: all bin/tcpPeer bin/cryptoCheck
	bash tests/run.sh

clean:
	rm -f bin/client bin/server bin/tcpPeer bin/cryptoCheck
