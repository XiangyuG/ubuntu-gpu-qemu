#!/bin/bash
#
# Launch Ubuntu RISC-V 64 on QEMU
# SSH access: ssh -p 2222 ubuntu@localhost
# Credentials: ubuntu / liuchao1523
#

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IMAGE="${SCRIPT_DIR}/ubuntu-riscv64.qcow2"
SEED="${SCRIPT_DIR}/seed.img"
OPENSBI="/usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin"
UBOOT="/usr/share/u-boot-qemu-bin/qemu-riscv64_smode/uboot.elf"

# Use locally built QEMU if available, otherwise system QEMU
QEMU_BIN="${SCRIPT_DIR}/qemu/build/qemu-system-riscv64"
if [ ! -x "${QEMU_BIN}" ]; then
    QEMU_BIN="qemu-system-riscv64"
fi

"${QEMU_BIN}" \
    -machine virt,aia=aplic-imsic \
    -cpu rv64 \
    -m 8G \
    -smp 4 \
    -bios "${OPENSBI}" \
    -kernel "${UBOOT}" \
    -drive file="${IMAGE}",format=qcow2,if=virtio \
    -drive file="${SEED}",format=raw,if=virtio \
    -device virtio-net-device,netdev=net0 \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 \
    -nographic \
    "$@"
