#!/bin/bash
#
# VM SSH interaction script for Ubuntu RISC-V QEMU
# Usage:
#   ./vm-ssh.sh                  - interactive shell
#   ./vm-ssh.sh exec "cmd"       - run a command and return output
#   ./vm-ssh.sh push <local> <remote>  - upload file/dir to VM
#   ./vm-ssh.sh pull <remote> <local>  - download file/dir from VM
#   ./vm-ssh.sh setup-key        - install SSH key for passwordless login
#

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/vm-credentials.conf"

SSH_KEY="${SCRIPT_DIR}/.vm_key"
COMMON_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
SSH_OPTS="${COMMON_OPTS} -p ${VM_SSH_PORT}"
SCP_OPTS="${COMMON_OPTS} -P ${VM_SSH_PORT}"

_has_key() {
    [[ -f "${SSH_KEY}" ]] && ssh ${SSH_OPTS} -i "${SSH_KEY}" -o BatchMode=yes "${VM_USER}@${VM_HOST}" true 2>/dev/null
}

_ssh_pass() {
    sshpass -p "${VM_PASS}" ssh ${SSH_OPTS} "${VM_USER}@${VM_HOST}" "$@"
}

_ssh_key() {
    ssh ${SSH_OPTS} -i "${SSH_KEY}" "${VM_USER}@${VM_HOST}" "$@"
}

_ssh() {
    if _has_key; then
        _ssh_key "$@"
    else
        _ssh_pass "$@"
    fi
}

_scp_pass() {
    sshpass -p "${VM_PASS}" scp ${SCP_OPTS} "$@"
}

_scp_key() {
    scp ${SCP_OPTS} -i "${SSH_KEY}" "$@"
}

_scp() {
    if _has_key; then
        _scp_key "$@"
    else
        _scp_pass "$@"
    fi
}

cmd_setup_key() {
    if [[ -f "${SSH_KEY}" ]]; then
        echo "[*] SSH key already exists: ${SSH_KEY}"
    else
        echo "[*] Generating SSH key pair..."
        ssh-keygen -t ed25519 -f "${SSH_KEY}" -N "" -q
    fi
    echo "[*] Installing public key to VM..."
    _ssh_pass "mkdir -p ~/.ssh && chmod 700 ~/.ssh"
    sshpass -p "${VM_PASS}" scp ${SCP_OPTS} "${SSH_KEY}.pub" "${VM_USER}@${VM_HOST}:/tmp/vm_key.pub"
    _ssh_pass 'cat /tmp/vm_key.pub >> ~/.ssh/authorized_keys && chmod 600 ~/.ssh/authorized_keys && rm /tmp/vm_key.pub'
    echo "[+] SSH key installed. Passwordless login enabled."
}

cmd_exec() {
    _ssh "$*"
}

cmd_push() {
    local src="$1" dst="$2"
    [[ -z "$src" || -z "$dst" ]] && { echo "Usage: $0 push <local_path> <remote_path>"; exit 1; }
    if [[ -d "$src" ]]; then
        _scp -r "$src" "${VM_USER}@${VM_HOST}:${dst}"
    else
        _scp "$src" "${VM_USER}@${VM_HOST}:${dst}"
    fi
}

cmd_pull() {
    local src="$1" dst="$2"
    [[ -z "$src" || -z "$dst" ]] && { echo "Usage: $0 pull <remote_path> <local_path>"; exit 1; }
    _scp -r "${VM_USER}@${VM_HOST}:${src}" "$dst"
}

cmd_shell() {
    _ssh
}

cmd_wait() {
    echo "[*] Waiting for VM SSH to become available..."
    local max_attempts=60
    local i=0
    while (( i < max_attempts )); do
        if _ssh "true" 2>/dev/null; then
            echo "[+] VM is ready."
            return 0
        fi
        sleep 5
        ((i++))
        echo "    attempt ${i}/${max_attempts}..."
    done
    echo "[-] Timeout waiting for VM."
    return 1
}

case "${1:-shell}" in
    exec)       shift; cmd_exec "$@" ;;
    push)       shift; cmd_push "$@" ;;
    pull)       shift; cmd_pull "$@" ;;
    setup-key)  cmd_setup_key ;;
    wait)       cmd_wait ;;
    shell)      cmd_shell ;;
    *)          echo "Usage: $0 {shell|exec <cmd>|push <local> <remote>|pull <remote> <local>|setup-key|wait}"; exit 1 ;;
esac
