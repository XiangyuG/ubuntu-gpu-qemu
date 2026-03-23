# Ubuntu RISC-V QEMU

QEMU-based Ubuntu RISC-V 64 development environment for GPU/CXL driver work.

## VM Specs

| Item | Value |
|------|-------|
| OS | Ubuntu 24.04.4 LTS (Noble) |
| Arch | riscv64 |
| Kernel | 6.17.0-14-generic |
| CPU | rv64, 4 cores |
| RAM | 8 GiB |
| Disk | 64 GiB (qcow2, virtio) |
| Network | User mode, SSH via `localhost:2222` |

## Prerequisites

Arch Linux host:

```bash
pacman -S qemu-system-riscv sshpass expect cloud-image-utils cdrtools
yay -S u-boot-qemu-bin
```

## Quick Start

### 1. Prepare image (first time only)

```bash
# Download
wget https://cdimage.ubuntu.com/releases/24.04/release/ubuntu-24.04.4-preinstalled-server-riscv64.img.xz

# Extract → convert → resize
xz -dk ubuntu-24.04.4-preinstalled-server-riscv64.img.xz
qemu-img convert -f raw -O qcow2 ubuntu-24.04.4-preinstalled-server-riscv64.img ubuntu-riscv64.qcow2
qemu-img resize ubuntu-riscv64.qcow2 64G

# Generate cloud-init seed disk (sets password from user-data)
cloud-localds seed.img user-data

# Create credentials file
cat > vm-credentials.conf <<'EOF'
VM_USER=ubuntu
VM_PASS=<your-password>
VM_SSH_PORT=2222
VM_HOST=localhost
EOF
chmod 600 vm-credentials.conf
```

### 2. Boot

```bash
./start-riscv64.sh
```

### 3. Connect

```bash
# Wait for VM ready, then SSH in
./vm-ssh.sh wait
./vm-ssh.sh
```

## Scripts

| Script | Description |
|--------|-------------|
| `start-riscv64.sh` | Launch QEMU VM (serial console, `-nographic`) |
| `vm-ssh.sh` | SSH interaction: `shell`, `exec`, `push`, `pull`, `setup-key`, `wait` |
| `user-data` | cloud-init config for setting VM password |

## File Transfer

```bash
./vm-ssh.sh push ./src/ /home/ubuntu/src/     # host → VM
./vm-ssh.sh pull /home/ubuntu/out/ ./out/      # VM → host
```

## License

MIT
