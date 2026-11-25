CC = cc
MPICC ?= mpicc
MPIEXEC ?= mpirun
MPI_FLAGS ?= --oversubscribe --tag-output
AR ?= ar
CMAKE ?= cmake
KERNEL_BUILD ?= /lib/modules/$(shell uname -r)/build
PICO_SDK_PATH ?=
USB_VID ?=
USB_PID ?=
USB_POWER_MA ?=
FIRMWARE_BUILD ?= build/pico
FIRMWARE_CMAKE_FLAGS ?=
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64 -Icommon
CFLAGS ?= -std=c11 -O2
CFLAGS += -Wall -Wextra -Wpedantic

ADDRESS ?= 127.0.0.1
PORT ?= 5000
INPUT_DIR ?= input
OUTPUT_FILE ?= output/cifrado.bin
KEY_FILE ?= keys/client.key
PLAIN_FILE ?= output/descifrado.txt
COUNTS_FILE ?= output/conteos.txt

.PHONY: all client server cluster-server keygen test test-mpi test-cluster clean \
        hardware-library driver firmware test-hardware clean-driver

all: bin/client bin/server

client: bin/client
	TYPIST_KEY_FILE="$(KEY_FILE)" exec ./bin/client --interactive "$(ADDRESS)" "$(PORT)" "$(INPUT_DIR)"

server: bin/server
	mkdir -p "$$(dirname -- "$(OUTPUT_FILE)")"
	exec ./bin/server "$(ADDRESS)" "$(PORT)" "$(OUTPUT_FILE)"

cluster-server: bin/clusterServer
	mkdir -p "$$(dirname -- "$(OUTPUT_FILE)")" "$$(dirname -- "$(PLAIN_FILE)")" \
		"$$(dirname -- "$(COUNTS_FILE)")"
	exec $(MPIEXEC) $(MPI_FLAGS) -np 3 ./bin/clusterServer "$(ADDRESS)" "$(PORT)" \
		"$(OUTPUT_FILE)" "$(PLAIN_FILE)" "$(COUNTS_FILE)" "$(KEY_FILE)"

keygen:
	mkdir -p "$$(dirname -- "$(KEY_FILE)")"
	(umask 077; set -C; openssl rand 32 >"$(KEY_FILE)")

bin/client: client/client.c client/client.h common/protocol.c common/protocol.h \
            common/crypto.c common/crypto.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) client/client.c common/protocol.c common/crypto.c \
		$(LDFLAGS) $(LDLIBS) -lcrypto -o $@

bin/server: server/main.c server/server.c server/server.h common/protocol.c \
            common/protocol.h common/crypto.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) server/main.c server/server.c common/protocol.c \
		$(LDFLAGS) $(LDLIBS) -o $@

bin/clusterServer: server/cluster.c server/server.c server/server.h server/wordCount.c \
                   server/wordCount.h common/protocol.c common/protocol.h \
                   common/crypto.c common/crypto.h | bin
	$(MPICC) $(CPPFLAGS) $(CFLAGS) server/cluster.c server/server.c server/wordCount.c \
		common/protocol.c common/crypto.c $(LDFLAGS) $(LDLIBS) -lcrypto -o $@

bin/tcpPeer: tests/tcpPeer.c common/protocol.c common/protocol.h common/crypto.c common/crypto.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/tcpPeer.c common/protocol.c common/crypto.c \
		$(LDFLAGS) $(LDLIBS) -lcrypto -o $@

bin/cryptoCheck: tests/cryptoCheck.c common/crypto.c common/crypto.h common/protocol.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/cryptoCheck.c common/crypto.c \
		$(LDFLAGS) $(LDLIBS) -lcrypto -o $@

bin/cryptoCharacterTest: tests/cryptoCharacterTest.c common/crypto.c common/crypto.h \
                       common/protocol.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/cryptoCharacterTest.c common/crypto.c \
		$(LDFLAGS) -Wl,--wrap=EVP_EncryptUpdate $(LDLIBS) -lcrypto -o $@

bin/mpiSmoke: tests/mpiSmoke.c | bin
	$(MPICC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) $(LDLIBS) -o $@

build/armDevice.o: library/armDevice.c library/armDevice.h include/typistIoctl.h \
                   include/typistUsbProtocol.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Iinclude -Ilibrary -c library/armDevice.c -o $@

build/libtypist.a: build/armDevice.o
	$(AR) rcs $@ $<

bin/armStatus: tools/armStatus.c library/armDevice.h build/libtypist.a | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -Ilibrary tools/armStatus.c build/libtypist.a \
		$(LDFLAGS) $(LDLIBS) -o $@

bin/armDeviceTest: tests/armDeviceTest.c firmware/pico/statusProtocol.c \
                   firmware/pico/statusProtocol.h include/typistIoctl.h \
                   include/typistUsbProtocol.h library/armDevice.h build/libtypist.a | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -Iinclude -Ilibrary -Ifirmware/pico tests/armDeviceTest.c \
		firmware/pico/statusProtocol.c build/libtypist.a \
		$(LDFLAGS) -Wl,--wrap=ioctl $(LDLIBS) -o $@

hardware-library: build/libtypist.a bin/armStatus

driver:
	$(MAKE) -C "$(KERNEL_BUILD)" M="$(CURDIR)/driver" modules

firmware:
	$(CMAKE) -S firmware/pico -B "$(FIRMWARE_BUILD)" -DPICO_BOARD=pico -DPICO_PLATFORM=rp2040 \
		-DPICO_SDK_PATH="$(PICO_SDK_PATH)" -DTYPIST_USB_VID="$(USB_VID)" \
		-DTYPIST_USB_PID="$(USB_PID)" -DTYPIST_USB_POWER_MA="$(USB_POWER_MA)" \
		$(FIRMWARE_CMAKE_FLAGS)
	$(CMAKE) --build "$(FIRMWARE_BUILD)"

test-hardware: bin/armDeviceTest
	./bin/armDeviceTest

clean-driver:
	$(MAKE) -C "$(KERNEL_BUILD)" M="$(CURDIR)/driver" clean

build:
	mkdir -p $@

bin:
	mkdir -p $@

test: all bin/tcpPeer bin/cryptoCheck bin/cryptoCharacterTest
	bash tests/run.sh

test-mpi: bin/mpiSmoke
	$(MPIEXEC) $(MPI_FLAGS) -np 3 ./bin/mpiSmoke

test-cluster: bin/client bin/clusterServer bin/tcpPeer bin/cryptoCheck
	MPIEXEC="$(MPIEXEC)" bash tests/cluster.sh

clean:
	rm -f bin/client bin/server bin/tcpPeer bin/cryptoCheck bin/cryptoCharacterTest
	rm -f bin/mpiSmoke
	rm -f bin/clusterServer
	rm -f bin/armStatus bin/armDeviceTest build/armDevice.o build/libtypist.a
