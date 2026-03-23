#!/bin/bash
# Integration tests for compiledd

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

COMPILEDD="$SCRIPT_DIR/../lib/compiledd/compiledd"
PASS=0
FAIL=0

check() {
    if [ $1 -eq 0 ]; then
        echo "  PASS: $2"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $2"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== compiledd tests ==="

# Positive: valid IR produces 64-byte descriptor
echo "ternary_matmul 4 4 4 0 1000 2000 3000 4000" | $COMPILEDD > /tmp/rvt2_desc.bin 2>/dev/null
SIZE=$(stat -c%s /tmp/rvt2_desc.bin 2>/dev/null || echo 0)
check $([ "$SIZE" -eq 64 ] && echo 0 || echo 1) "valid IR produces 64-byte descriptor"

# Positive: multiple descriptors
printf "ternary_matmul 4 4 4 0 1000 2000 3000 4000\nternary_matmul 8 8 8 0 5000 6000 7000 8000\n" | $COMPILEDD > /tmp/rvt2_desc2.bin 2>/dev/null
SIZE=$(stat -c%s /tmp/rvt2_desc2.bin 2>/dev/null || echo 0)
check $([ "$SIZE" -eq 128 ] && echo 0 || echo 1) "two IR lines produce 128-byte output"

# Negative: malformed IR
echo "garbage input" | $COMPILEDD > /dev/null 2>/dev/null
check $([ $? -ne 0 ] && echo 0 || echo 1) "malformed IR returns error"

# Negative: invalid dimensions
echo "ternary_matmul 0 4 4 0 1000 2000 3000 4000" | $COMPILEDD > /dev/null 2>/dev/null
check $([ $? -ne 0 ] && echo 0 || echo 1) "zero dimension returns error"

# Negative: unsupported operation
echo "conv2d 4 4 4 0 1000 2000 3000 4000" | $COMPILEDD > /dev/null 2>/dev/null
check $([ $? -ne 0 ] && echo 0 || echo 1) "unsupported operation returns error"

rm -f /tmp/rvt2_desc.bin /tmp/rvt2_desc2.bin

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
exit $FAIL
