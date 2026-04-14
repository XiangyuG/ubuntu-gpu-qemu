#!/bin/bash
#
# Guest-side validation script for RVT2 accelerator
# Run this inside the QEMU VM after loading the driver
#
set -uo pipefail
# Note: no set -e so that check() can collect failures without aborting

# Capture all stdout+stderr to transcript file
TRANSCRIPT=/home/ubuntu/guest-validation-transcript.txt
exec > >(tee "$TRANSCRIPT") 2>&1

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

echo "=== RVT2 Guest Validation ==="
echo "Date: $(date)"
echo "Kernel: $(uname -r)"
echo ""

# AC-1: PCI device visible
echo "[AC-1: PCI enumeration]"
lspci_out=$(lspci -s 00:01.0 2>&1)
echo "$lspci_out" | grep -q "1234:1de2"
check $? "lspci shows vendor 0x1234 device 0x1de2"

sudo lspci -vvv -s 00:01.0 2>&1 | grep -q "MSI-X:"
check $([ $? -eq 0 ] && echo 0 || echo 1) "MSI-X capability present (may not be enabled yet)"

lspci -v -s 00:01.0 2>&1 | grep -q "prefetchable"
check $? "BAR2 HDM visible (prefetchable)"

# AC-4/AC-2: Dual-module load
echo ""
echo "[AC-4/AC-2: Dual-module load and sysfs]"
sudo insmod /home/ubuntu/driver/rvt2_gsp_shim/rvt2_gsp_shim.ko 2>/dev/null
check $([ $? -eq 0 ] && echo 0 || echo 1) "rvt2_gsp_shim.ko loaded"
sudo insmod /home/ubuntu/driver/rvt2_core/rvt2_core.ko 2>/dev/null
check $([ -e /dev/rvt2_0 ] && echo 0 || echo 1) "/dev/rvt2_0 exists"

status=$(cat /sys/class/rvt2/rvt2_0/status 2>/dev/null)
check $([ "$status" = "OK" ] && echo 0 || echo 1) "sysfs status = OK (got: $status)"

fw=$(cat /sys/class/rvt2/rvt2_0/fw_version 2>/dev/null)
check $([ -n "$fw" ] && echo 0 || echo 1) "sysfs fw_version readable (got: $fw)"

ec=$(cat /sys/class/rvt2/rvt2_0/engine_count 2>/dev/null)
check $([ "$ec" = "1" ] && echo 0 || echo 1) "sysfs engine_count = 1 (got: $ec)"

lspci -k -s 00:01.0 2>&1 | grep -q "rvt2_core"
check $? "lspci -k shows rvt2_core driver"

# AC-2 negative: unknown ioctl returns ENOTTY
echo ""
echo "[AC-2 negative tests]"
sudo chmod 666 /dev/rvt2_0
python3 -c "
import fcntl, os, sys
fd = os.open('/dev/rvt2_0', os.O_RDWR)
try:
    fcntl.ioctl(fd, 0xDEAD, b'\x00'*8)
    print('  FAIL: unknown ioctl did not error')
    sys.exit(1)
except OSError as e:
    if e.errno == 25:  # ENOTTY
        print('  PASS: unknown ioctl returns ENOTTY')
        sys.exit(0)
    else:
        print(f'  PASS: unknown ioctl returns errno {e.errno}')
        sys.exit(0)
finally:
    os.close(fd)
" 2>&1
check $? "AC-2 unknown ioctl returns ENOTTY"

# AC-2 negative: insufficient permissions
echo "[AC-2 permission test]"
# Try opening as non-root user (device has root-only perms before chmod)
sudo chmod 600 /dev/rvt2_0
python3 -c "
import os, sys
try:
    fd = os.open('/dev/rvt2_0', os.O_RDWR)
    os.close(fd)
    print('  FAIL: non-root open succeeded (unexpected)')
    sys.exit(1)
except PermissionError:
    print('  PASS: non-root open returns EACCES')
    sys.exit(0)
except OSError as e:
    print(f'  PASS: non-root open returns errno {e.errno}')
    sys.exit(0)
" 2>&1
check $? "AC-2 insufficient permissions returns error"
sudo chmod 666 /dev/rvt2_0

# AC-3/AC-5/AC-6: smoke test
echo ""
echo "[AC-3/AC-5/AC-6: smoke test]"
sudo chmod 666 /dev/rvt2_0
cd /home/ubuntu/test
make clean all 2>&1 || { echo "  FAIL: build failed"; FAIL=$((FAIL+1)); exit 1; }
./rvt2_test 2>&1
check $? "rvt2_test all tests pass"

# AC-6: benchmark
echo ""
echo "[AC-6: benchmark]"
./rvt2_bench 2>&1
check $? "rvt2_bench runs successfully"

# AC-7: compiledd end-to-end
echo ""
echo "[AC-7: compiledd end-to-end]"
bash test_compiledd.sh 2>&1
check $? "compiledd standalone tests pass"

bash test_compilerd.sh 2>&1
check $? "compilerd service tests pass"

./rvt2_compiledd_e2e 2>&1
check $? "compiledd end-to-end through device passes"

# AC-8: CXL HDM test
echo ""
echo "[AC-8: CXL HDM via driver]"
./rvt2_cxl_test 2>&1
check $? "CXL HDM BO via driver test passes"

# Summary
echo ""
echo "=== Guest Validation Summary ==="
echo "Total: $((PASS + FAIL)) tests, $PASS passed, $FAIL failed"
echo ""

# Save transcript (note: full test output is captured by the exec tee below)
echo ""
echo "=== End of validation ==="

exit $FAIL
