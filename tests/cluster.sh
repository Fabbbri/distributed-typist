#!/usr/bin/env bash
# Prueba integral real: cliente TCP, servidor y tres rangos OpenMPI en Linux local.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."

testDir=$(mktemp -d)
launcherPid=""
coordinatorPid=""
mpiExec=${MPIEXEC:-mpirun}

cleanup() {
    if [[ -n "$coordinatorPid" ]]; then
        kill -TERM "$coordinatorPid" 2>/dev/null || true
    fi
    if [[ -n "$launcherPid" ]]; then
        kill -TERM "$launcherPid" 2>/dev/null || true
        wait "$launcherPid" 2>/dev/null || true
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
    local status=0
    timeout 20s "$@" >"$testDir/failure.log" 2>&1 || status=$?
    [[ "$status" -eq 1 ]] || fail "expected exit 1, got $status: $*"
}

verifyTransfer() {
    local sourcePath=$1
    local expectedPath=$2
    timeout 20s bin/client 127.0.0.1 "$serverPort" "$sourcePath"
    cmp -- "$sourcePath" "$testDir/plain.txt" || fail 'authenticated plaintext differs'
    bin/cryptoCheck "$testDir/cipher.bin" "$TYPIST_KEY_FILE" "$testDir/checked.txt" ||
        fail 'stored container does not authenticate'
    cmp -- "$sourcePath" "$testDir/checked.txt" || fail 'stored ciphertext differs from source'
    LC_ALL=C sort "$testDir/counts.txt" >"$testDir/actual-sorted.txt"
    LC_ALL=C sort "$expectedPath" >"$testDir/expected-sorted.txt"
    diff -u "$testDir/expected-sorted.txt" "$testDir/actual-sorted.txt" ||
        fail 'global word counts differ'
}

# Cada prueba usa una clave efímera: nunca sobrescribe keys/client.key.
export TYPIST_KEY_FILE="$testDir/client.key"
make --no-print-directory keygen KEY_FILE="$TYPIST_KEY_FILE" >"$testDir/keygen.log" 2>&1
serverPort=$(bin/tcpPeer port)
timeout --signal=TERM --kill-after=5s 90s make --no-print-directory cluster-server \
    MPIEXEC="$mpiExec" MPI_FLAGS='--oversubscribe --tag-output' PORT="$serverPort" \
    OUTPUT_FILE="$testDir/cipher.bin" PLAIN_FILE="$testDir/plain.txt" \
    COUNTS_FILE="$testDir/counts.txt" KEY_FILE="$TYPIST_KEY_FILE" \
    >"$testDir/cluster.log" 2>&1 &
launcherPid=$!

for ((attempt = 0; attempt < 200; attempt++)); do
    if grep -q 'INFO: listening' "$testDir/cluster.log"; then
        break
    fi
    kill -0 "$launcherPid" 2>/dev/null || fail 'MPI launcher exited before startup'
    sleep 0.05
done
grep -q 'INFO: listening' "$testDir/cluster.log" || fail 'cluster startup timed out'
coordinatorPid=$(sed -n 's/.*cluster coordinator pid=\([0-9]*\).*/\1/p' "$testDir/cluster.log")
[[ "$coordinatorPid" =~ ^[0-9]+$ ]] || fail 'missing coordinator PID'

# La palabra global gana aunque no se elija como ganadora local por el desempate.
printf 'aaa aaa comun comun\nbbb bbb comun comun\nccc ccc comun comun\n' >"$testDir/input.txt"
printf 'DTC1\n2\taaa\n2\tbbb\n2\tccc\n6\tcomun\n' >"$testDir/expected.txt"
verifyTransfer "$testDir/input.txt" "$testDir/expected.txt"
grep -q 'cluster result word=comun count=6 totalWords=12' "$testDir/cluster.log" ||
    fail 'incorrect global winner'
for rank in 0 1 2; do
    grep -q "worker rank=$rank .*bytes=20 words=4 unique=2" "$testDir/cluster.log" ||
        fail "rank $rank did not process its own fragment"
done
printf 'PASS: TCP client, three decrypting/counting ranks, global aggregation and exact recovery\n'

printf 'enviar input.txt\nsalir\n' | timeout 20s make --no-print-directory client \
    PORT="$serverPort" KEY_FILE="$TYPIST_KEY_FILE" INPUT_DIR="$testDir" \
    >"$testDir/interactive.log" 2>&1
grep -q 'INFO: transfer confirmed' "$testDir/interactive.log" || fail 'interactive cluster client'
cmp "$testDir/input.txt" "$testDir/plain.txt" || fail 'interactive plaintext differs'
printf 'PASS: make cluster-server and interactive make client\n'

printf 'zeta alfa zeta alfa' >"$testDir/input.txt"
printf 'DTC1\n2\talfa\n2\tzeta\n' >"$testDir/expected.txt"
verifyTransfer "$testDir/input.txt" "$testDir/expected.txt"
grep -q 'cluster result word=alfa count=2 totalWords=4' "$testDir/cluster.log" || fail 'tie break'
printf 'Gato,gato! perro; GATO.\nniño niño árbol\n' >"$testDir/input.txt"
printf 'DTC1\n3\tgato\n1\tperro\n2\tniño\n1\tárbol\n' >"$testDir/expected.txt"
verifyTransfer "$testDir/input.txt" "$testDir/expected.txt"
printf 'PASS: deterministic ties, final word without newline, ASCII case, punctuation and UTF-8\n'

# Muchas palabras únicas fuerzan bloques múltiples tanto en tareas como en resultados MPI.
awk 'BEGIN {for (i=0; i<3000; i++) printf "palabra%04d palabra%04d comun\n", i, i}' \
    >"$testDir/input.txt"
awk 'BEGIN {print "DTC1"; print "3000\tcomun";
    for (i=0; i<3000; i++) printf "2\tpalabra%04d\n", i}' >"$testDir/expected.txt"
verifyTransfer "$testDir/input.txt" "$testDir/expected.txt"
printf 'PASS: multiple MPI blocks and 3001 unique global words\n'

head -c 4096 /dev/zero | tr '\000' 'x' >"$testDir/boundary-word.txt"
{ cat "$testDir/boundary-word.txt"; printf ' '; cat "$testDir/boundary-word.txt"; } \
    >"$testDir/input.txt"
{ printf 'DTC1\n2\t'; cat "$testDir/boundary-word.txt"; printf '\n'; } \
    >"$testDir/expected.txt"
verifyTransfer "$testDir/input.txt" "$testDir/expected.txt"
printf 'PASS: maximum-length words remain whole across fragment boundaries\n'

printf 'DTC1\n' >"$testDir/expected.txt"
: >"$testDir/input.txt"
verifyTransfer "$testDir/input.txt" "$testDir/expected.txt"
printf ' ,.!\t\n\n' >"$testDir/input.txt"
verifyTransfer "$testDir/input.txt" "$testDir/expected.txt"
grep -q 'cluster result: no words' "$testDir/cluster.log" || fail 'empty document result'
printf 'PASS: empty and separator-only documents\n'

# Los errores no deben publicar texto ni conteos parciales; luego se admite otro documento.
cp "$testDir/plain.txt" "$testDir/previous-plain.txt"
cp "$testDir/counts.txt" "$testDir/previous-counts.txt"
openssl rand 32 >"$testDir/wrong.key"
chmod 600 "$testDir/wrong.key"
printf 'texto cifrado con otra clave\n' >"$testDir/input.txt"
expectFailure env TYPIST_KEY_FILE="$testDir/wrong.key" \
    bin/client 127.0.0.1 "$serverPort" "$testDir/input.txt"
timeout 20s bin/tcpPeer badGcm "$serverPort"
head -c 4097 /dev/zero | tr '\000' 'x' >"$testDir/long-word.txt"
expectFailure bin/client 127.0.0.1 "$serverPort" "$testDir/long-word.txt"
cmp "$testDir/plain.txt" "$testDir/previous-plain.txt" || fail 'failed task changed plaintext'
cmp "$testDir/counts.txt" "$testDir/previous-counts.txt" || fail 'failed task changed counts'
printf 'recuperado recuperado' >"$testDir/input.txt"
printf 'DTC1\n2\trecuperado\n' >"$testDir/expected.txt"
verifyTransfer "$testDir/input.txt" "$testDir/expected.txt"
printf 'PASS: wrong key, invalid container and oversized word rejected; cluster recovers\n'

# Detener solo al rango servidor permite enviar la parada autenticada y finalizar MPI limpiamente.
kill -TERM "$coordinatorPid"
wait "$launcherPid" || fail 'cluster did not shut down cleanly'
coordinatorPid=""
launcherPid=""
expectFailure "$mpiExec" --oversubscribe -np 2 ./bin/clusterServer 127.0.0.1 "$serverPort" \
    "$testDir/cipher.bin" "$testDir/plain.txt" "$testDir/counts.txt" "$TYPIST_KEY_FILE"
expectFailure "$mpiExec" --oversubscribe -np 3 ./bin/clusterServer 127.0.0.1 "$serverPort" \
    "$testDir/cipher.bin" "$testDir/plain.txt" "$testDir/counts.txt" "$testDir/missing.key"
expectFailure "$mpiExec" --oversubscribe \
    -np 1 ./bin/clusterServer 127.0.0.1 "$serverPort" "$testDir/cipher.bin" \
    "$testDir/plain.txt" "$testDir/counts.txt" "$TYPIST_KEY_FILE" : \
    -np 2 ./bin/clusterServer 127.0.0.1 "$serverPort" "$testDir/cipher.bin" \
    "$testDir/plain.txt" "$testDir/counts.txt" "$testDir/wrong.key"
printf 'PASS: graceful shutdown, incorrect process count, missing and mismatched cluster keys\n'
printf 'PASS: all client/server/OpenMPI integration tests\n'
