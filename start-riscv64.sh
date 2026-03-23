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

qemu-system-riscv64 \
    -machine virt \
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
