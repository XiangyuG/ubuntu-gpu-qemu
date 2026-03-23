# RVT2 Accelerator Register Specification

## Overview

The RVT2 ternary matmul accelerator is a PCI device (vendor 0x1234, device 0x1de2, class 0x0b40 co-processor, revision 0x01) with two BARs:

- **BAR0** (4KiB): MMIO register space
- **BAR4** (4KiB): MSI-X table (exclusive, 2 vectors)

## BAR0 Register Map

All registers are 32-bit, little-endian, aligned to 4-byte boundaries.

### Device Identification and Status (0x00–0x1F)

| Offset | Name | Access | Reset | Description |
|--------|------|--------|-------|-------------|
| 0x00 | ID | RO | 0x52565432 | Device magic ("RVT2" in ASCII) |
| 0x04 | VERSION | RO | 0x00010000 | Hardware version (v1.0.0) |
| 0x08 | STATUS | RO | 0x00000000 | Device status (see below) |
| 0x0C | CONTROL | RW | 0x00000000 | Device control (see below) |
| 0x10 | IRQ_STATUS | RC | 0x00000000 | Interrupt status (clear on read) |
| 0x14 | IRQ_MASK | RW | 0x00000000 | Interrupt mask (1 = masked) |

**STATUS register bits:**

| Bit | Name | Description |
|-----|------|-------------|
| 0 | READY | Device initialized and ready to accept commands |
| 1 | BUSY | Device is processing descriptors |
| 2 | ERROR | Device encountered a fault (DMA failure, invalid queue, etc.) |
| 3 | FW_LOADED | Firmware loaded and handshake completed |

**CONTROL register bits:**

| Bit | Name | Description |
|-----|------|-------------|
| 0 | ENABLE | Enable command processing (must be set before writing doorbell) |
| 1 | RESET | Writing 1 triggers soft reset: clears all status, queues, and timers |

**IRQ_STATUS / IRQ_MASK bits:**

| Bit | Name | MSI-X Vector | Description |
|-----|------|-------------|-------------|
| 0 | COMPLETION | 0 | At least one descriptor completed successfully |
| 1 | FAULT | 1 | Device encountered a fault condition |

IRQ_STATUS is **clear-on-read**: reading the register returns the current value and atomically clears all bits. The COMPLETION bit is only set when at least one descriptor was actually processed. An idle doorbell (head == tail) does **not** generate any interrupt.

### Command Queue Registers (0x20–0x3F)

| Offset | Name | Access | Description |
|--------|------|--------|-------------|
| 0x20 | CMDQ_BASE_LO | RW | Command queue base DMA address [31:0] |
| 0x24 | CMDQ_BASE_HI | RW | Command queue base DMA address [63:32] |
| 0x28 | CMDQ_SIZE | RW | Queue capacity in number of entries (must be > 0 before use) |
| 0x2C | CMDQ_HEAD | RO | Device read pointer (index of next descriptor to process) |
| 0x30 | CMDQ_TAIL | RW | Host write pointer (index past last written descriptor) |
| 0x34 | DOORBELL | WO | Write any value to trigger command processing |

**Queue operation:**
1. Host allocates a contiguous DMA buffer of `CMDQ_SIZE × 64` bytes
2. Host writes `CMDQ_BASE`, `CMDQ_SIZE`
3. Host writes descriptor(s) starting at `CMDQ_TAIL`
4. Host updates `CMDQ_TAIL` to point past the last descriptor
5. Host writes `DOORBELL` to trigger processing
6. Device processes from `CMDQ_HEAD` to `CMDQ_TAIL`, advancing `CMDQ_HEAD`

**Validation:** If `CMDQ_SIZE == 0` or `CMDQ_BASE == 0` when doorbell is written with pending work (head != tail), the device sets `STATUS.ERROR`, raises `IRQ_FAULT`, and does not process any descriptors.

### Completion Queue Registers (0x40–0x5F)

| Offset | Name | Access | Description |
|--------|------|--------|-------------|
| 0x40 | CPLQ_BASE_LO | RW | Completion queue base DMA address [31:0] |
| 0x44 | CPLQ_BASE_HI | RW | Completion queue base DMA address [63:32] |
| 0x48 | CPLQ_SIZE | RW | Queue capacity in number of entries |
| 0x4C | CPLQ_HEAD | RW | Host read pointer |
| 0x50 | CPLQ_TAIL | RO | Device write pointer |

Each completion entry is 16 bytes (see Completion Entry format below).

### Device Capabilities (0x60–0x7F)

| Offset | Name | Access | Description |
|--------|------|--------|-------------|
| 0x60 | ENGINE_COUNT | RO | Number of compute engines (1) |
| 0x64 | MAX_DESC_SIZE | RO | Maximum descriptor size in bytes (64) |

### Mailbox Registers (0x80–0x9F)

| Offset | Name | Access | Description |
|--------|------|--------|-------------|
| 0x80 | MBOX_CMD | RW | Write to issue a mailbox command (processed immediately) |
| 0x84 | MBOX_STATUS | RO | Mailbox status: 0=idle, 1=busy, 2=done, 3=error |
| 0x88 | MBOX_DATA0 | RW | Mailbox data word 0 |
| 0x8C | MBOX_DATA1 | RW | Mailbox data word 1 |
| 0x90 | MBOX_DATA2 | RW | Mailbox data word 2 |
| 0x94 | MBOX_DATA3 | RW | Mailbox data word 3 |

**Mailbox commands:**

| Code | Name | Input | Output (DATA0-3) |
|------|------|-------|-------------------|
| 0x00 | NOP | — | — |
| 0x01 | INIT | — | DATA0=0 (success). Sets STATUS.FW_LOADED and STATUS.READY |
| 0x02 | QUERY_CAP | — | DATA0=engine_count(1), DATA1=max_desc_size(64), DATA2=fw_version(0x0100), DATA3=supported_ops(0x01=ternary_matmul) |
| 0x03 | HEARTBEAT | — | DATA0=1 (alive) |

**Protocol:** Write MBOX_CMD → device processes immediately → MBOX_STATUS becomes DONE (2) or ERROR (3) → read MBOX_DATA0-3 for results.

## Descriptor Format (64 bytes)

Descriptors are placed in the command queue, 64-byte aligned.

```
Offset  Size  Field           Description
0x00    4     opcode          Operation code (0x01 = ternary_matmul)
0x04    4     flags           Reserved (must be 0)
0x08    8     input_a_addr    DMA address of matrix A (m×k, row-major)
0x10    8     input_b_addr    DMA address of matrix B (k×n, row-major)
0x18    8     input_c_addr    DMA address of matrix C (m×n, row-major)
0x20    8     output_d_addr   DMA address of result D (m×n, row-major)
0x28    4     m               Rows of A, C, D (1–4096)
0x2C    4     n               Columns of B, C, D (1–4096)
0x30    4     k               Columns of A / rows of B (1–4096)
0x34    4     dtype           Data type: 0=float32 (only supported type)
0x38    8     fence_seqno     Fence sequence number (echoed in completion)
```

**Compute:** `D[i][j] = sum(A[i][p] * B[p][j], p=0..k-1) + C[i][j]`

## Completion Entry Format (16 bytes)

```
Offset  Size  Field           Description
0x00    8     fence_seqno     Matching fence sequence number from descriptor
0x08    4     status          0=success, 1=bad opcode, 2=bad dtype, 3=bad dims, 4=DMA fault
0x0C    4     reserved        Must be 0
```

## Fault Behavior

When a fault occurs (DMA failure, invalid queue config, bad descriptor fetch):

1. `STATUS.ERROR` bit is set
2. A completion entry with non-zero status is written to the completion queue
3. `IRQ_FAULT` (MSI-X vector 1) is raised
4. The device stops processing further descriptors in this batch
5. Host must write `CONTROL.RESET` to clear the error state

## MSI-X Vectors

| Vector | Name | Trigger |
|--------|------|---------|
| 0 | COMPLETION | At least one descriptor processed successfully |
| 1 | FAULT | DMA failure, queue misconfiguration, or descriptor fetch error |

Interrupts are masked when the corresponding `IRQ_MASK` bit is set to 1.
