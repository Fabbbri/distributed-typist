#!/usr/bin/env bash
# Pruebas locales: Bash coordina los ejecutables C y las herramientas de integridad.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."

testDir=$(mktemp -d)
serverPid=""
peerPid=""

cleanup() {
    if [[ -n "$peerPid" ]]; then
        kill -TERM "$peerPid" 2>/dev/null || true
        wait "$peerPid" 2>/dev/null || true
    fi
    if [[ -n "$serverPid" ]]; then
        kill -TERM "$serverPid" 2>/dev/null || true
        wait "$serverPid" 2>/dev/null || true
    fi
    rm -rf -- "$testDir"
}
trap cleanup EXIT

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    cat "$testDir"/*.log >&2
    exit 1
}

expectFailure() {
    local exitStatus=0
    timeout 20 "$@" >"$testDir/failure.log" 2>&1 || exitStatus=$?
    [[ "$exitStatus" -eq 1 ]] || fail "expected exit 1, got $exitStatus: $*"
}

waitReady() {
    local processPid=$1
    local logPath=$2
    for ((attempt = 0; attempt < 100; attempt++)); do
        if grep -q 'INFO: listening' "$logPath"; then
            return
        fi
        kill -0 "$processPid" 2>/dev/null || fail "listener exited before startup"
        sleep 0.05
    done
    fail "listener startup timed out"
}

startServer() {
    serverPort=$(bin/tcpPeer port)
    bin/server 127.0.0.1 "$serverPort" "$1" >"$testDir/server.log" 2>&1 &
    serverPid=$!
    waitReady "$serverPid" "$testDir/server.log"
}

stopServer() {
    kill -TERM "$serverPid"
    wait "$serverPid" || fail "server did not stop cleanly"
    serverPid=""
}

verifyTransfer() {
    timeout 20 bin/client 127.0.0.1 "$serverPort" "$1"
    cmp -- "$1" "$testDir/received.txt" || fail "content mismatch"
    local sourceHash receivedHash
    sourceHash=$(sha256sum -- "$1")
    receivedHash=$(sha256sum -- "$testDir/received.txt")
    [[ "${sourceHash%% *}" == "${receivedHash%% *}" ]] || fail "SHA-256 mismatch"
    printf 'PASS: %s (cmp + SHA-256: %s)\n' "$2" "${sourceHash%% *}"
}

verifyPreserved() {
    cmp -- "$testDir/small.txt" "$testDir/received.txt" || fail "previous file changed"
    local partialFiles=("$testDir"/received.txt.part.*)
    [[ ! -e "${partialFiles[0]}" ]] || fail "staging file was not removed"
}

printf 'Texto de prueba TCP. Acentos: áéíóú.\nSegunda línea.\n' >"$testDir/small.txt"
# Archivo de texto de 32 MiB; supera ampliamente el bloque y los buffers TCP habituales.
head -c 33554432 /dev/zero | LC_ALL=C tr '\000' 'A' >"$testDir/large.txt"
: >"$testDir/empty.txt"

startServer "$testDir/received.txt"
verifyTransfer "$testDir/small.txt" 'small text file'
verifyTransfer "$testDir/large.txt" '32 MiB text file'
verifyTransfer "$testDir/empty.txt" 'empty file'
verifyTransfer "$testDir/small.txt" 'successive connections'

# La consola debe recuperarse de órdenes inválidas y aceptar varios envíos en una sesión.
printf 'Archivo con espacios en el nombre.\n' >"$testDir/with spaces.txt"
{
    printf '\nunknown\nenviar\nsalir extra\nenviar missing.txt\n'
    head -c 5000 /dev/zero | tr '\000' 'x'
    printf '\nenviar bad\000name\n  enviar small.txt  \r\nenviar %s\nsalir\n' \
        "$testDir/with spaces.txt"
    printf 'enviar missing-after-exit.txt\n'
} >"$testDir/commands.txt"
timeout 20 make --no-print-directory client ADDRESS=127.0.0.1 PORT="$serverPort" \
    INPUT_DIR="$testDir" <"$testDir/commands.txt" >"$testDir/interactive.log" 2>&1
[[ $(grep -c 'INFO: transfer confirmed' "$testDir/interactive.log") -eq 2 ]] ||
    fail 'interactive client did not complete exactly two transfers'
grep -q 'command too long or contains a null byte' "$testDir/interactive.log" ||
    fail 'invalid command input was not rejected'
grep -q 'session remains active' "$testDir/interactive.log" ||
    fail 'missing file did not report a recoverable transfer error'
cmp -- "$testDir/with spaces.txt" "$testDir/received.txt" ||
    fail 'explicit path with spaces was not transferred'

# EOF termina la sesión; una última línea sin salto también debe ejecutarse.
printf 'enviar small.txt' >"$testDir/commands.txt"
timeout 20 bin/client --interactive 127.0.0.1 "$serverPort" "$testDir" \
    <"$testDir/commands.txt" >"$testDir/interactive.log" 2>&1
verifyPreserved
timeout 20 bin/client --interactive 127.0.0.1 "$serverPort" "$testDir" </dev/null
expectFailure bin/client --interactive 192.0.2.1 "$serverPort" "$testDir"
printf 'PASS: interactive commands, recovery, repeated sends, spaces, input limits and EOF\n'

expectFailure bin/client 127.0.0.1 "$serverPort" "$testDir/missing.txt"
expectFailure bin/client 127.0.0.1 "$serverPort" "$testDir"
mkfifo "$testDir/inputFifo"
expectFailure bin/client 127.0.0.1 "$serverPort" "$testDir/inputFifo"
truncate -s 1073741825 "$testDir/oversize.txt"
expectFailure bin/client 127.0.0.1 "$serverPort" "$testDir/oversize.txt"
printf 'PASS: missing, nonregular and oversized input files\n'

for invalidPort in 0 65536 -1 +5 abc ' 123' 999999999999999999999999; do
    expectFailure bin/client 127.0.0.1 "$invalidPort" "$testDir/small.txt"
    expectFailure bin/server 127.0.0.1 "$invalidPort" "$testDir/unused.txt"
done
for invalidAddress in 0.0.0.0 192.0.2.1 localhost invalid; do
    expectFailure bin/client "$invalidAddress" "$serverPort" "$testDir/small.txt"
    expectFailure bin/server "$invalidAddress" "$serverPort" "$testDir/unused.txt"
done
expectFailure bin/client
expectFailure bin/server
expectFailure bin/server 127.0.0.1 "$serverPort" "$testDir/unused.txt"
printf 'PASS: arguments, local-only endpoints and occupied port\n'

for testMode in partialHeader badMagic oversize truncated extra reset stall; do
    timeout 20 bin/tcpPeer "$testMode" "$serverPort"
    # Una conexión posterior confirma también que la limpieza del caso previo terminó.
    timeout 20 bin/tcpPeer badMagic "$serverPort"
    verifyPreserved
    kill -0 "$serverPid" || fail "server died after $testMode"
    printf 'PASS: %s; previous output preserved, no partial file\n' "$testMode"
done

timeout 20 bin/tcpPeer fragmented "$serverPort"
printf 'fragmented\n' >"$testDir/fragmented.txt"
cmp -- "$testDir/fragmented.txt" "$testDir/received.txt"
printf 'PASS: header and body sent one byte at a time\n'
verifyTransfer "$testDir/small.txt" 'recovery after invalid transfers'
stopServer
expectFailure bin/client 127.0.0.1 "$serverPort" "$testDir/small.txt"
printf 'PASS: connection refused and graceful server shutdown\n'

# Un fallo de conexión no debe cerrar la consola antes de leer salir.
printf 'enviar small.txt\nsalir\n' >"$testDir/commands.txt"
timeout 20 make --no-print-directory client ADDRESS=127.0.0.1 PORT="$serverPort" \
    INPUT_DIR="$testDir" <"$testDir/commands.txt" >"$testDir/interactive.log" 2>&1
grep -q 'session remains active' "$testDir/interactive.log" ||
    fail 'interactive connection failure did not report recovery'
printf 'PASS: interactive session survives connection errors\n'

# Ejecuta ambos objetivos Make, incluida la creación automática del directorio de salida.
serverPort=$(bin/tcpPeer port)
timeout --signal=TERM 30 make --no-print-directory server ADDRESS=127.0.0.1 \
    PORT="$serverPort" OUTPUT_FILE="$testDir/make output/received.txt" \
    >"$testDir/server.log" 2>&1 &
serverPid=$!
waitReady "$serverPid" "$testDir/server.log"
printf 'enviar small.txt\nsalir\n' >"$testDir/commands.txt"
timeout 20 make --no-print-directory client ADDRESS=127.0.0.1 PORT="$serverPort" \
    INPUT_DIR="$testDir" <"$testDir/commands.txt" >"$testDir/interactive.log" 2>&1
cmp -- "$testDir/small.txt" "$testDir/make output/received.txt" || fail 'Make transfer failed'
# timeout retransmite la señal al grupo para detener también el servidor lanzado por Make.
kill -TERM "$serverPid"
wait "$serverPid" 2>/dev/null || true
serverPid=""
expectFailure bin/client 127.0.0.1 "$serverPort" "$testDir/small.txt"
printf 'PASS: make server and make client launch working programs with configurable paths\n'

startServer "$testDir/missingDirectory/received.txt"
expectFailure bin/client 127.0.0.1 "$serverPort" "$testDir/small.txt"
stopServer
printf 'PASS: server rejects unavailable output before body\n'

mkdir "$testDir/outputDirectory"
startServer "$testDir/outputDirectory"
expectFailure bin/client 127.0.0.1 "$serverPort" "$testDir/small.txt"
[[ -d "$testDir/outputDirectory" ]] || fail "output directory changed"
partialFiles=("$testDir"/outputDirectory.part.*)
[[ ! -e "${partialFiles[0]}" ]] || fail "staging file remains after rename failure"
stopServer
printf 'PASS: final publication failure is reported and cleaned up\n'

for testMode in drop noAck invalidStatus; do
    peerPort=$(bin/tcpPeer port)
    bin/tcpPeer "$testMode" "$peerPort" >"$testDir/peer.log" 2>&1 &
    peerPid=$!
    waitReady "$peerPid" "$testDir/peer.log"
    expectFailure bin/client 127.0.0.1 "$peerPort" "$testDir/large.txt"
    wait "$peerPid" || fail "test peer failed: $testMode"
    peerPid=""
    printf 'PASS: client detects %s\n' "$testMode"
done

printf 'PASS: all TCP integration tests\n'
