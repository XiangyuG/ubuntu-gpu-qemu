# Ubuntu RISC-V QEMU Development Environment

## Project Overview

QEMU-based Ubuntu RISC-V 64 virtual machine for GPU/CXL driver development.

- **VM**: Ubuntu 24.04.4 LTS, kernel 6.17.0, riscv64
- **QEMU machine**: virt, rv64, 4 cores, 8G RAM, 64G disk
- **SSH**: port 2222 forwarded to VM port 22

## Host Prerequisites (Arch Linux)

```bash
# QEMU and firmware
pacman -S qemu-system-riscv
yay -S u-boot-qemu-bin

# Utilities
pacman -S sshpass expect cloud-image-utils cdrtools
```

Firmware paths:
- OpenSBI: `/usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin`
- U-Boot: `/usr/share/u-boot-qemu-bin/qemu-riscv64_smode/uboot.elf`

## Image Setup (from scratch)

### 1. Download Ubuntu RISC-V image

```bash
wget https://cdimage.ubuntu.com/releases/24.04/release/ubuntu-24.04.4-preinstalled-server-riscv64.img.xz
```

### 2. Extract and convert to qcow2, resize to 64G

```bash
xz -dk ubuntu-24.04.4-preinstalled-server-riscv64.img.xz
qemu-img convert -f raw -O qcow2 ubuntu-24.04.4-preinstalled-server-riscv64.img ubuntu-riscv64.qcow2
qemu-img resize ubuntu-riscv64.qcow2 64G
```

### 3. Configure VM password via cloud-init

The preinstalled image uses cloud-init with an internal NoCloud seed (`/var/lib/cloud/seed/nocloud-net`). The embedded `user-data` sets password `ubuntu` with `expire: True`, which blocks SSH login.

To override, create a cloud-init seed disk:

```bash
# user-data file is already in the repo
cloud-localds seed.img user-data
```

The `user-data` sets password and disables expiration so SSH works immediately.

### 4. Create credentials file

```bash
cat > vm-credentials.conf <<'EOF'
VM_USER=ubuntu
VM_PASS=<your-password>
VM_SSH_PORT=2222
VM_HOST=localhost
EOF
chmod 600 vm-credentials.conf
```

This file is in `.gitignore` — never commit it.

## Daily Workflow

```bash
# Start VM (background for scripting)
./start-riscv64.sh              # foreground with serial console
# or
nohup ./start-riscv64.sh > vm-console.log 2>&1 &  # background

# Wait for SSH ready
./vm-ssh.sh wait

# First time: install SSH key for passwordless login
./vm-ssh.sh setup-key

# Interactive shell
./vm-ssh.sh

# Run commands
./vm-ssh.sh exec "uname -a"

# File transfer
./vm-ssh.sh push ./driver-src/ /home/ubuntu/driver-src/
./vm-ssh.sh pull /home/ubuntu/build/driver.ko ./output/

# Stop VM
kill $(cat vm.pid)
```

## Driver Development Notes

- Upload source to VM via `vm-ssh.sh push`, compile inside VM with riscv64 toolchain
- VM root partition auto-expanded to full 64G on first boot
- Extra QEMU args can be appended: `./start-riscv64.sh -m 16G`
