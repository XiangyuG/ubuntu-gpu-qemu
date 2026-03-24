# RVT2 Guest Validation Checklist

This document captures the minimum guest-side validation flow for AC-1.
Run the commands inside the RISC-V guest after booting QEMU with the RVT2 device enabled.

## 1. PCI Enumeration

```bash
lspci -nn
lspci -s 00:01.0 -vv
```

Expected checks:

- Vendor/device ID is `1234:1de2`
- Class code is `0b40` (co-processor)
- BAR0 is a 4 KiB MMIO region
- MSI-X advertises two vectors

## 2. BAR0 MMIO Readback

Map BAR0 with `devmem` or a small helper:

```bash
devmem 0x40000000 32
devmem 0x40000004 32
devmem 0x40000008 32
```

Expected values:

- `ID` = `0x52565432`
- `VERSION` = `0x00010000`
- `STATUS` transitions from `0x0` to `READY|FW_LOADED` after mailbox init

## 3. Idle Doorbell Negative Test

With `CMDQ_HEAD == CMDQ_TAIL`, write the doorbell without populating descriptors:

```bash
devmem 0x40000034 32 1
devmem 0x40000010 32
```

Expected result:

- `IRQ_STATUS` completion bit remains clear
- No guest-visible completion interrupt is delivered

## 4. Bad Queue State Negative Test

Program an invalid queue state and ring the doorbell:

```bash
devmem 0x40000028 32 0
devmem 0x40000030 32 1
devmem 0x40000034 32 1
devmem 0x40000008 32
devmem 0x40000010 32
```

Expected result:

- `STATUS.ERROR` becomes set
- `IRQ_STATUS` fault bit is set
- The device does not crash QEMU

## 5. Bad DMA Address Negative Test

Submit one descriptor whose input/output DMA address points to an unmapped guest address.

Expected result:

- Completion entry status is `4`
- `STATUS.ERROR` is set
- Fault IRQ is raised
- The batch stops after the faulting descriptor

## 6. Reset Recovery

Clear the faulted state:

```bash
devmem 0x4000000c 32 2
devmem 0x40000008 32
```

Expected result:

- `STATUS` returns to `0x0`
- Queue pointers are reset
- A new mailbox init sequence can be issued afterwards
