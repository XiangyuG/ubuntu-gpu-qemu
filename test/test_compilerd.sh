#!/bin/bash
# Integration tests for compilerd.

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

COMPILERD="$SCRIPT_DIR/../lib/compiledd/compilerd"
PASS=0
FAIL=0

check() {
    if [ "$1" -eq 0 ]; then
        echo "  PASS: $2"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $2"
        FAIL=$((FAIL + 1))
    fi
}

wait_for_socket() {
    local sock="$1"
    local i

    for i in $(seq 1 50); do
        [ -S "$sock" ] && return 0
        sleep 0.02
    done
    return 1
}

echo "=== compilerd tests ==="

SOCK="/tmp/rvt2_compilerd_test.$$.$RANDOM.sock"
OUT="/tmp/rvt2_compilerd_desc.$$.$RANDOM.bin"

"$COMPILERD" --socket "$SOCK" --once &
PID=$!
if wait_for_socket "$SOCK"; then
    python3 - "$SOCK" "$OUT" <<'PY'
import socket
import sys

sock_path, out_path = sys.argv[1], sys.argv[2]
ir = b"ternary_matmul 4 4 4 0 1000 2000 3000 4000\n"

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
    sock.connect(sock_path)
    sock.sendall(b"COMPILE %d\n" % len(ir) + ir)
    header = b""
    while not header.endswith(b"\n"):
        chunk = sock.recv(1)
        if not chunk:
            raise SystemExit(2)
        header += chunk
    status, size = header.decode().strip().split()
    payload = b""
    while len(payload) < int(size):
        chunk = sock.recv(int(size) - len(payload))
        if not chunk:
            raise SystemExit(3)
        payload += chunk

if status != "OK":
    sys.stderr.write(payload.decode(errors="replace"))
    raise SystemExit(1)

with open(out_path, "wb") as f:
    f.write(payload)
PY
    SIZE=$(stat -c%s "$OUT" 2>/dev/null || echo 0)
    check $([ "$SIZE" -eq 64 ] && echo 0 || echo 1) "valid request returns 64-byte descriptor"
else
    check 1 "compilerd socket appears"
fi
wait "$PID" 2>/dev/null
rm -f "$SOCK" "$OUT"

SOCK="/tmp/rvt2_compilerd_test.$$.$RANDOM.sock"
"$COMPILERD" --socket "$SOCK" --once &
PID=$!
if wait_for_socket "$SOCK"; then
    python3 - "$SOCK" <<'PY'
import socket
import sys

sock_path = sys.argv[1]
ir = b"conv2d 4 4 4 0 1000 2000 3000 4000\n"

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
    sock.connect(sock_path)
    sock.sendall(b"COMPILE %d\n" % len(ir) + ir)
    header = b""
    while not header.endswith(b"\n"):
        chunk = sock.recv(1)
        if not chunk:
            raise SystemExit(2)
        header += chunk
    status, size = header.decode().strip().split()
    payload = b""
    while len(payload) < int(size):
        chunk = sock.recv(int(size) - len(payload))
        if not chunk:
            raise SystemExit(3)
        payload += chunk

if status != "ERR" or b"unsupported operation" not in payload:
    raise SystemExit(1)
PY
    check $? "invalid request returns diagnostic error"
else
    check 1 "compilerd socket appears for negative test"
fi
wait "$PID" 2>/dev/null
rm -f "$SOCK"

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
exit $FAIL
