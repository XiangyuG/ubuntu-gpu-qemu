#!/bin/bash
#
# Launch Ubuntu RISC-V 64 on QEMU with a locally built kernel image.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

IMAGE="${IMAGE:-${SCRIPT_DIR}/ubuntu-riscv64.qcow2}"
SEED="${SEED:-${SCRIPT_DIR}/seed.img}"
OPENSBI="${OPENSBI:-/usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin}"
KERNEL="${KERNEL:-/mnt/disk2/linux/out-rv64/arch/riscv/boot/Image}"
QEMU_BIN="${QEMU_BIN:-${SCRIPT_DIR}/qemu/build/qemu-system-riscv64}"

# Ubuntu's riscv64 preinstalled image uses the first virtio disk
# partition as rootfs. Override ROOTDEV if your image differs.
ROOTDEV="${ROOTDEV:-/dev/vda1}"
MACHINE="${MACHINE:-virt}"
CPU="${CPU:-rv64}"
RAM="${RAM:-8G}"
SMP="${SMP:-4}"
SSH_PORT="${SSH_PORT:-2222}"
KERNEL_CMDLINE="${KERNEL_CMDLINE:-root=${ROOTDEV} rw rootwait console=ttyS0 earlycon}"

if [ ! -x "${QEMU_BIN}" ]; then
    QEMU_BIN="qemu-system-riscv64"
fi

require_file() {
    local path="$1"
    local hint="$2"

    if [ ! -e "${path}" ]; then
        echo "Missing required file: ${path}" >&2
        echo "${hint}" >&2
        exit 1
    fi
}

require_file "${IMAGE}" "Prepare the Ubuntu qcow2 image first."
require_file "${SEED}" "Run cloud-localds seed.img user-data in this repo."
require_file "${OPENSBI}" "Install the QEMU OpenSBI firmware package for riscv64."
require_file "${KERNEL}" "Build the local kernel image before launching."

"${QEMU_BIN}" \
    -machine "${MACHINE}" \
    -cpu "${CPU}" \
    -m "${RAM}" \
    -smp "${SMP}" \
    -bios "${OPENSBI}" \
    -kernel "${KERNEL}" \
    -append "${KERNEL_CMDLINE}" \
    -drive file="${IMAGE}",format=qcow2,if=virtio \
    -drive file="${SEED}",format=raw,if=virtio \
    -device virtio-net-device,netdev=net0 \
    -netdev user,id=net0,hostfwd=tcp::${SSH_PORT}-:22 \
    -nographic \
    "$@"
