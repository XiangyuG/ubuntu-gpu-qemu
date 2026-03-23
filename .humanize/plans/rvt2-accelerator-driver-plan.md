# RVT2 Ternary MatMul Accelerator — Implementation Plan

## Goal Description

Build a complete Linux driver stack for a RISC-V host-attached ternary matmul accelerator device (`D = A × B + C`), developed and validated entirely on QEMU before real hardware arrives. The stack spans from a QEMU virtual PCI device model, through kernel drivers (`rvt2_core.ko` + `rvt2_gsp_shim.ko`), up to a userspace runtime library (`libtmatmulrt.so`). The architecture targets future evolution to CXL Type-2 semantics.

**Architecture layers:**

```
Userspace        libtmatmulrt.so / hetGPU runtime / compiledd
                          │ ioctl / mmap / poll
Kernel           rvt2_core.ko  +  rvt2_gsp_shim.ko
                          │ MMIO / Doorbell / DMA / Mailbox
Firmware         Device Management Core + Command Processor (emulated in QEMU)
Hardware         Load → Prefetch → Tile → TernaryMatMul → Acc/Quant → Store
```

**Compute semantics:** `D = A × B + C` — standard fused multiply-add. A, B are input matrices, C is bias/accumulator matrix, D is output. QEMU device model performs software emulation of this operation.

## Acceptance Criteria

Following TDD philosophy, each criterion includes positive and negative tests for deterministic verification.

- AC-1: QEMU device model registers as a PCI device visible to the guest
  - Positive Tests (expected to PASS):
    - `lspci` inside guest VM shows a device with vendor `0x1234` and a rvt2-specific device ID
    - Guest can read BAR0 MMIO registers (e.g. device version register returns expected magic value)
    - Guest can write to a BAR0 MMIO register and read back the written value where applicable
  - Negative Tests (expected to FAIL):
    - Reading from an undefined/reserved register offset returns 0 or all-ones (not a crash)
    - Writing to a read-only status register does not change the register value
  - AC-1.1: MSI-X interrupts are functional
    - Positive: Device raises MSI-X completion interrupt after processing a descriptor; guest IRQ handler fires
    - Negative: No spurious interrupts when device is idle and no descriptors are pending
  - AC-1.2: DMA engine reads/writes guest memory correctly
    - Positive: Device DMA-reads input buffers from guest physical addresses specified in descriptor
    - Negative: DMA to an invalid/unmapped address does not crash QEMU; device reports fault status

- AC-2: Kernel module `rvt2_core.ko` loads, probes the PCI device, and exposes a char device
  - Positive Tests (expected to PASS):
    - `modprobe rvt2_core` succeeds with return code 0; `dmesg` shows probe success message
    - `/dev/rvt2_0` exists after module load
    - `cat /sys/class/rvt2/rvt2_0/status` returns `OK`
    - `lspci -k` shows `rvt2_core` as the kernel driver in use for the device
  - Negative Tests (expected to FAIL):
    - `modprobe rvt2_core` on a VM without the rvt2 QEMU device does not create `/dev/rvt2_0`
    - Opening `/dev/rvt2_0` with insufficient permissions returns `-EACCES`
    - Issuing an unknown ioctl number returns `-EINVAL` or `-ENOTTY`

- AC-3: Buffer Object (BO) subsystem supports alloc, mmap, and destroy lifecycle
  - Positive Tests (expected to PASS):
    - `ioctl(fd, RVT2_IOCTL_BO_CREATE, {size=4096})` returns a valid BO handle
    - `mmap` the BO into userspace, write pattern, read back — data matches
    - `ioctl(fd, RVT2_IOCTL_BO_DESTROY, {handle})` frees the BO; subsequent access via handle returns error
    - `ioctl(fd, RVT2_IOCTL_BO_INFO, {handle})` returns correct size and DMA address
  - Negative Tests (expected to FAIL):
    - `BO_CREATE` with size=0 returns `-EINVAL`
    - `BO_DESTROY` with an invalid handle returns `-ENOENT`
    - Accessing a destroyed BO's mmap region triggers `SIGBUS` or equivalent

- AC-4: Firmware shim (`rvt2_gsp_shim.ko`) loads firmware and completes mailbox handshake
  - Positive Tests (expected to PASS):
    - `modprobe rvt2_gsp_shim` triggers `request_firmware()` and loads the firmware blob to device
    - Mailbox init handshake completes; `dmesg` shows "firmware ready" with version string
    - Capability query returns engine count, max descriptor size, supported operations
  - Negative Tests (expected to FAIL):
    - If firmware file is missing, module load fails gracefully with `-ENOENT` in dmesg (no kernel panic)
    - If device does not respond to handshake within timeout, module reports error and sets device to fault state

- AC-5: Command submission pipeline delivers correct ternary matmul results
  - Positive Tests (expected to PASS):
    - Allocate BOs for A, B, C (input) and D (output); fill A, B, C with known values
    - Submit descriptor via `RVT2_IOCTL_SUBMIT`; doorbell triggers device processing
    - `RVT2_IOCTL_WAIT` or `poll()` returns when fence signals completion
    - Read D from mmap — values match `A × B + C` computed on host
  - Negative Tests (expected to FAIL):
    - Submit with invalid BO handle returns `-EINVAL`; no descriptor sent to device
    - `RVT2_IOCTL_WAIT` with timeout=0 on an incomplete fence returns `-ETIMEDOUT`
    - Submit on a device in fault state returns `-EIO`
  - AC-5.1: Fence/sync works correctly
    - Positive: Fence seqno increments monotonically across submissions; `dma_fence_is_signaled()` returns true after completion
    - Negative: Waiting on an already-signaled fence returns immediately (no hang)

- AC-6: Userspace library `libtmatmulrt.so` provides working end-to-end API
  - Positive Tests (expected to PASS):
    - `rvt2_open()` succeeds; `rvt2_bo_alloc()` returns valid handle; `rvt2_bo_map()` returns non-NULL pointer
    - `rvt2_submit()` + `rvt2_wait()` completes; result buffer contains correct `D = A × B + C`
    - `rvt2_test` smoke test passes: alloc → fill → submit → wait → verify
    - `rvt2_close()` releases all resources; no leaked file descriptors
  - Negative Tests (expected to FAIL):
    - `rvt2_open()` on a system without the device returns error code
    - `rvt2_submit()` with NULL descriptor returns error
    - `rvt2_wait()` with negative timeout returns `-EINVAL`

- AC-7: Compilation service translates simple IR to device command stream (deferred, lower priority)
  - Positive Tests (expected to PASS):
    - `compiledd` accepts a simple ternary matmul IR description and produces a valid descriptor chain
    - Descriptor chain submitted to device produces correct results
  - Negative Tests (expected to FAIL):
    - Malformed IR input is rejected with a descriptive error message

- AC-8: CXL Type-2 extension enables coherent device memory access (future)
  - Positive Tests (expected to PASS):
    - QEMU CXL Type-2 device model exposes HDM region
    - BO allocated from CXL HDM is accessible from host CPU with cache coherence
  - Negative Tests (expected to FAIL):
    - Access to HDM beyond the configured range triggers a CXL protocol error

## Path Boundaries

Path boundaries define the acceptable range of implementation quality and choices.

### Upper Bound (Maximum Acceptable Scope)

The implementation includes all components through AC-6 (QEMU device model, kernel driver with BO/submit/fence, firmware shim with mailbox protocol, and userspace library with smoke test and benchmark). The QEMU device model faithfully emulates descriptor processing, DMA, MSI-X, and software-computed `D = A × B + C`. The kernel driver supports multiple concurrent submissions with proper fence ordering. The userspace library has a stable C API suitable for integration with hetGPU runtime. AC-7 (compiledd) and AC-8 (CXL Type-2) are included as design outlines with stub implementations.

### Lower Bound (Minimum Acceptable Scope)

The implementation includes AC-1 through AC-5: a working QEMU PCI device that can be probed by a kernel driver, allocate buffer objects, submit a single ternary matmul job, and return correct results via DMA + interrupt. AC-6 is a thin wrapper over ioctl calls. AC-7 and AC-8 are documented but not implemented.

### Allowed Choices

- **QEMU device model**: Must be developed in `./qemu/hw/misc/rvt2.c` within the QEMU worktree, following QOM patterns (as in `edu.c`)
- **PCI ID**: Must use `PCI_VENDOR_ID_QEMU (0x1234)` with a custom device ID
- **Char device registration**: Can use `misc_register()` or `cdev` — `misc_register()` preferred for simplicity
- **BO backing memory**: Can use `dma_alloc_coherent` or page-based allocation with `sg_table`; must not use DRM/GEM in initial implementation
- **Descriptor format**: Fixed at 64-byte cache-line-aligned structs
- **Fence model**: Must use monotonic seqno; `dma_fence` integration is required
- **Build system**: Out-of-tree kernel module build (`make -C /lib/modules/.../build M=...`)
- **Cannot use**: DRM subsystem (initially), in-tree kernel build, non-standard ioctl encoding

## Feasibility Hints and Suggestions

> **Note**: This section is for reference and understanding only. These are conceptual suggestions, not prescriptive requirements.

### Conceptual Approach

**QEMU device model** — follow the `edu.c` pattern:

```
EduState pattern → Rvt2State pattern:
  struct Rvt2State {
      PCIDevice pdev;
      MemoryRegion mmio_bar;         // BAR0: registers
      MemoryRegion msix_bar;         // BAR4: MSI-X (exclusive)
      // Device state
      uint32_t status;
      uint32_t doorbell;
      // Descriptor ring
      dma_addr_t cmdq_base;
      uint32_t cmdq_head, cmdq_tail;
      // DMA
      QEMUTimer compute_timer;       // Simulate async compute
  };

  realize():
    memory_region_init_io(&mmio_bar, &rvt2_mmio_ops, ...)
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &mmio_bar)
    msix_init_exclusive_bar(pdev, 2, 4, ...)   // 2 vectors: completion + fault
    // Register CONFIG_RVT2 in hw/misc/Kconfig, hw/misc/meson.build
```

**Descriptor format sketch (64 bytes):**

```
struct rvt2_descriptor {
    uint32_t opcode;          // 0x01 = ternary_matmul
    uint32_t flags;
    uint64_t input_a_addr;    // DMA address of matrix A
    uint64_t input_b_addr;    // DMA address of matrix B
    uint64_t input_c_addr;    // DMA address of matrix C
    uint64_t output_d_addr;   // DMA address of result D
    uint32_t m, n, k;         // Matrix dimensions: A(m×k) × B(k×n) + C(m×n) = D(m×n)
    uint32_t dtype;           // Data type: 0=float32, 1=float16, 2=int8
    uint64_t fence_seqno;     // Fence sequence number
};  // Total: 64 bytes
```

**Development loop:**

```
Host: build QEMU (./qemu/build/) → start VM with -device rvt2
      → vm-ssh.sh push driver/ → VM: make → insmod → test
      → vm-ssh.sh pull results
```

### Relevant References

- `qemu/hw/misc/edu.c` — Simple PCI device model with MMIO, DMA, MSI (446 lines, ideal starting template)
- `qemu/hw/mem/cxl_type3.c` — CXL Type-3 device with MSI-X, mailbox, events (2497 lines, reference for CXL extension)
- `qemu/hw/cxl/cxl-mailbox-utils.c` — Mailbox command processing (reference for firmware shim protocol)
- `qemu/hw/cxl/cxl-events.c` — Event log + MSI-X notification pattern
- `qemu/hw/misc/meson.build` — Where to register new device (add `CONFIG_RVT2` entry)
- `qemu/include/hw/pci/msix.h` — MSI-X API: `msix_init_exclusive_bar()`, `msix_notify()`

## Dependencies and Sequence

### Milestones

1. **Milestone 1: Virtual Hardware Foundation**
   - Section A: QEMU `rvt2` PCI device model — BAR0 MMIO, register read/write, MSI-X, basic DMA (AC-1)
   - Section B: Register specification document and descriptor format definition

2. **Milestone 2: Kernel Driver Skeleton**
   - Section A: `rvt2_core.ko` — PCI probe/remove, BAR ioremap, MSI-X IRQ, `/dev/rvt2_0` char device, sysfs (AC-2)
   - Section B: `rvt2_gsp_shim.ko` — firmware loading via `request_firmware()`, mailbox handshake, capability query (AC-4)
   - Note: Section B must complete before Milestone 3 Section B, as engine init is a prerequisite for command submission

3. **Milestone 3: Data Path**
   - Section A: BO subsystem — alloc, mmap, DMA mapping, destroy, refcounting (AC-3)
   - Section B: Command submission — descriptor ring, doorbell, fence/job, submit/wait ioctl (AC-5)
   - Section C: Update QEMU device model to process descriptors, perform DMA, compute D=A×B+C, raise completion IRQ

4. **Milestone 4: Userspace Integration**
   - Section A: `libtmatmulrt.so` — C API wrapping ioctl/mmap (AC-6)
   - Section B: `rvt2_test` smoke test — end-to-end verification
   - Section C: `rvt2_bench` — throughput/latency measurement

5. **Milestone 5: Advanced Features (Future)**
   - Section A: `compiledd` compilation service — IR to descriptor chain translation (AC-7)
   - Section B: CXL Type-2 device model and driver extension (AC-8)

**Dependency chain:**

```
Milestone 1 (QEMU device)
   │
   └──► Milestone 2 (driver skeleton + firmware shim)
           │
           └──► Milestone 3 (BO + submit, requires firmware init)
                   │
                   └──► Milestone 4 (userspace lib + tests)
                           │
                           └──► Milestone 5 (compiledd + CXL)
```

### Directory Layout

```
ubuntu-gpu-cxl-qemu/
├── qemu/                              # QEMU worktree (submodule, rvt2-dev branch)
│   └── hw/misc/rvt2.c                 # Device model source
│       include/hw/misc/rvt2.h         # Device model header
├── driver/                            # Kernel modules (out-of-tree)
│   ├── rvt2_core/
│   │   ├── Makefile
│   │   ├── rvt2_drv.c                 # PCI probe, char dev
│   │   ├── rvt2_bo.c                  # Buffer object
│   │   ├── rvt2_submit.c              # Command submission
│   │   ├── rvt2_fence.c               # Fence / sync
│   │   ├── rvt2_irq.c                 # Interrupt handling
│   │   └── rvt2_sysfs.c              # sysfs attributes
│   └── rvt2_gsp_shim/
│       ├── Makefile
│       ├── rvt2_gsp.c                 # Firmware load, mailbox
│       └── rvt2_gsp_rpc.c            # RPC protocol
├── include/uapi/
│   └── rvt2_drm.h                     # Shared ioctl/descriptor definitions
├── lib/
│   ├── libtmatmulrt/
│   │   ├── Makefile
│   │   ├── rvt2_lib.c
│   │   └── rvt2_lib.h
│   └── compiledd/                     # Future
├── test/
│   ├── rvt2_test.c
│   └── rvt2_bench.c
├── start-riscv64.sh
├── vm-ssh.sh
└── user-data
```

## Implementation Notes

### Key Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Device node path | `/dev/rvt2X` (not `/dev/accel/accelX`) | Simpler; migrate to accel subsystem when stable |
| BO model | Custom (not DRM/GEM) | Avoid DRM complexity for compute-only device |
| Module split | `rvt2_core` + `rvt2_gsp_shim` | Separate firmware management from data path; mirrors GSP architecture |
| Descriptor size | 64-byte aligned | Cache-line friendly; simple for QEMU emulation |
| Fence model | Monotonic seqno + `dma_fence` | Standard kernel pattern; enables future DRM scheduler integration |
| PCI ID | `0x1234:TBD` (QEMU vendor) | Avoid conflict with real hardware during development |
| QEMU device path | `hw/misc/rvt2.c` | Follow existing QEMU patterns (edu.c) |

### RISC-V Considerations

- Use `readl()`/`writel()` for MMIO access (memory ordering barriers)
- QEMU virt machine uses PLIC for interrupts; MSI-X works through IOMMU emulation
- DMA buffers must use `dma_alloc_coherent()` for proper alignment
- Kernel build: out-of-tree against VM's installed headers (`linux-headers-6.17.0-14-generic`)

### Code Style Requirements

- Implementation code and comments must NOT contain plan-specific terminology such as "AC-", "Milestone", "Step", "Phase", or similar workflow markers
- These terms are for plan documentation only, not for the resulting codebase
- Use descriptive, domain-appropriate naming in code instead

--- Original Design Draft Start ---

# RVT2 Ternary MatMul Accelerator — Full-Stack Driver Plan

## 1. Background

A RISC-V host-attached accelerator device with a linear compute pipeline, core operator is **ternary matmul** (`D = f(A, B, C)`). Target CXL Type-2 semantics in the future; current priority is a working Linux driver stack on QEMU.

The device exposes PCI BAR(s), MSI-X interrupts, DMA, and a descriptor-based command queue. The firmware/management core (GSP-like) handles init, dispatch, fault, and telemetry.

## 2. Architecture Layers

```
Userspace        libtmatmulrt.so / hetGPU runtime / compiledd
                          │ ioctl / mmap / poll
Kernel           rvt2_core.ko  +  rvt2_gsp_shim.ko
                          │ MMIO / Doorbell / DMA / Mailbox
Firmware         Device Management Core + Command Processor
Hardware         Load → Prefetch → Tile → TernaryMatMul → Acc/Quant → Store
```

## 3. Phase Breakdown

### Phase 0 — QEMU Device Model (`qemu-rvt2`)

**Goal**: A virtual PCI device that behaves like the real accelerator, enough to develop and test the entire driver stack without hardware.

| Item | Detail |
|------|--------|
| PCI vendor/device | Custom (e.g. `0x1de5:0x0001`) |
| BAR0 | MMIO registers: doorbell, status, config, engine info |
| BAR2 | Descriptor ring / mailbox SRAM (optional) |
| DMA | Read input buffers, write output buffers via IOMMU-aware DMA |
| MSI-X | Completion interrupt, fault interrupt |
| Compute | Software emulation: receive descriptor → read A,B,C → compute D=A×B+C (simplified ternary matmul) → write D → raise IRQ |

Deliverables:
- `hw/misc/rvt2.c` (or `hw/accel/rvt2.c`) — QEMU device model source
- Register spec document
- Descriptor format definition
- Can be probed by `lspci` inside guest VM

### Phase 1 — Kernel Driver: PCI Infrastructure (`rvt2_core.ko` skeleton)

**Goal**: A loadable kernel module that claims the PCI device, maps BARs, and registers a char device.

| Item | Detail |
|------|--------|
| PCI probe/remove | Match vendor/device, `pci_enable_device`, request regions |
| BAR ioremap | `pci_iomap` BAR0 (registers), optionally BAR2 |
| IRQ | `pci_alloc_irq_vectors` MSI-X, register handler |
| Char device | `/dev/rvt2X` via `misc_register` or `cdev` |
| sysfs | Device info: firmware version, engine count, status |
| Module params | `modprobe rvt2_core debug_level=3` |

Kernel objects introduced: `struct rvt2_device`.

### Phase 2 — Memory Management (BO Subsystem)

**Goal**: Allocate, map, and share buffers between userspace and device.

| Item | Detail |
|------|--------|
| BO allocator | `struct rvt2_bo` — size, DMA addr, kernel VA, flags |
| Backing memory | CMA / `dma_alloc_coherent` / `sg_table` for scatter-gather |
| ioctl | `RVT2_IOCTL_BO_CREATE`, `RVT2_IOCTL_BO_INFO`, `RVT2_IOCTL_BO_DESTROY` |
| mmap | `vm_operations_struct` — map BO pages into userspace VMA |
| DMA mapping | `dma_map_sg` / `dma_map_single` for device access |
| Refcounting | `kref` on BO lifecycle; handle in-flight jobs holding references |

Consider DRM/GEM integration in later phases; start with custom BO for simplicity.

### Phase 3 — Command Submission & Synchronization

**Goal**: Submit work to the device and wait for completion.

| Item | Detail |
|------|--------|
| Descriptor format | `struct rvt2_descriptor` — opcode, input BO handles, output BO handle, size/shape params |
| Submit queue (cmdq) | Ring buffer in device-visible memory; host writes, device reads |
| Completion queue (cplq) | Device writes completion entries; host reads via poll/IRQ |
| Doorbell | MMIO write to BAR0 doorbell register to notify device |
| Fence | `struct rvt2_fence` — seqno, `dma_fence` integration |
| Job | `struct rvt2_job` — links descriptor + fence + BO refs |
| ioctl | `RVT2_IOCTL_SUBMIT`, `RVT2_IOCTL_WAIT` |
| poll | `POLLIN` on fd when completion available |
| Timeout | Watchdog timer; escalate to engine reset on hang |

### Phase 4 — Firmware Shim (`rvt2_gsp_shim.ko`)

**Goal**: Manage the device management core (GSP-like firmware unit).

| Item | Detail |
|------|--------|
| Firmware blob | Load via `request_firmware()`, DMA to device SRAM |
| Mailbox | Shared-memory + doorbell RPC protocol |
| Init sequence | Reset → load FW → handshake → engine bring-up |
| Health check | Periodic heartbeat via mailbox; detect FW crash |
| Capability query | Query engine count, max descriptor size, supported ops |
| Module relationship | `rvt2_gsp_shim` exports symbols consumed by `rvt2_core` |

For QEMU simulation, the "firmware" is emulated inside the device model; the shim still exercises the mailbox protocol.

### Phase 5 — Userspace Runtime (`libtmatmulrt.so`)

**Goal**: Minimal C library for applications to use the device.

```c
// Core API sketch
int rvt2_open(rvt2_dev_t *dev);
void rvt2_close(rvt2_dev_t *dev);

rvt2_bo_t rvt2_bo_alloc(rvt2_dev_t *dev, size_t size, uint32_t flags);
void *rvt2_bo_map(rvt2_bo_t bo);
void rvt2_bo_free(rvt2_bo_t bo);

int rvt2_submit(rvt2_dev_t *dev, rvt2_descriptor_t *desc, rvt2_fence_t *fence);
int rvt2_wait(rvt2_fence_t *fence, int64_t timeout_ns);
```

Deliverables:
- `libtmatmulrt.so` with stable C API
- `rvt2_test` — smoke test: alloc A,B,C → fill data → submit ternary matmul → wait → verify D
- `rvt2_bench` — throughput / latency benchmark

### Phase 6 — Compilation Service (`compiledd`)

**Goal**: Translate high-level IR / PTX / hetGPU IR into device command streams.

| Item | Detail |
|------|--------|
| Input format | Simple IR (initially hand-written descriptors; later hetGPU IR) |
| Output format | Ternary matmul command stream (descriptor chain) |
| Lowering | Shape analysis → tiling → descriptor generation |
| Caching | Code cache keyed by (op, shape, dtype) |
| IPC | Unix socket or shared BO between runtime and compiledd |
| Mode | AOT (pre-compile) + JIT (on-demand) hybrid |

This phase can be deferred; Phase 5 tests can submit hand-crafted descriptors directly.

### Phase 7 — CXL Type-2 Extension (Future)

**Goal**: Evolve the PCI device model and driver to CXL Type-2 semantics.

| Item | Detail |
|------|--------|
| CXL device model | QEMU CXL Type-2: HDM (Host-managed Device Memory) via CXL.mem |
| HDM-DB | Device-managed memory exposed to host as cacheable, coherent region |
| Driver changes | Use `cxl_mem` subsystem; map HDM for BO allocations |
| Coherency | CXL.cache protocol for host-device cache coherence |
| Benefits | Zero-copy: host writes inputs directly to device-visible coherent memory |

## 4. Dependency Graph

```
Phase 0 (QEMU device)
   │
   ├──► Phase 1 (PCI skeleton)
   │       │
   │       ├──► Phase 2 (BO / memory)
   │       │       │
   │       │       └──► Phase 3 (submit / sync)
   │       │               │
   │       │               ├──► Phase 5 (userspace lib)
   │       │               │       │
   │       │               │       └──► Phase 6 (compiledd)
   │       │               │
   │       │               └──► Phase 7 (CXL Type-2)
   │       │
   │       └──► Phase 4 (firmware shim)
   │               │
   │               └──► (Phase 3 depends on Phase 4 for engine init)
```

## 5. Directory Layout (Proposed)

```
ubuntu-gpu-cxl-qemu/
├── qemu-rvt2/                  # QEMU device model (out-of-tree or patch)
│   ├── hw/accel/rvt2.c
│   ├── include/hw/accel/rvt2.h
│   └── docs/registers.md
├── driver/                     # Kernel modules
│   ├── rvt2_core/
│   │   ├── Makefile
│   │   ├── rvt2_drv.c          # PCI probe, char dev
│   │   ├── rvt2_bo.c           # Buffer object
│   │   ├── rvt2_submit.c       # Command submission
│   │   ├── rvt2_fence.c        # Fence / sync
│   │   ├── rvt2_irq.c          # Interrupt handling
│   │   └── rvt2_sysfs.c        # sysfs attributes
│   └── rvt2_gsp_shim/
│       ├── Makefile
│       ├── rvt2_gsp.c          # Firmware load, mailbox
│       └── rvt2_gsp_rpc.c      # RPC protocol
├── include/uapi/               # Shared kernel-user headers
│   └── rvt2_drm.h              # ioctl definitions, descriptor struct
├── lib/                        # Userspace runtime
│   ├── libtmatmulrt/
│   │   ├── Makefile
│   │   ├── rvt2_lib.c
│   │   └── rvt2_lib.h
│   └── compiledd/
│       └── ...
├── test/                       # Tests and benchmarks
│   ├── rvt2_test.c
│   └── rvt2_bench.c
├── start-riscv64.sh
├── vm-ssh.sh
└── user-data
```

## 6. QEMU Simulation Strategy

All development happens in this loop:

1. **Build QEMU** with `rvt2` device model on host (native x86_64 or cross)
2. **Boot VM** with `./start-riscv64.sh` adding `-device rvt2` flag
3. **Push driver source** into VM via `./vm-ssh.sh push`
4. **Compile inside VM** against the running kernel headers
5. **Load module**, run tests, collect results
6. **Pull logs/results** back to host

The QEMU device model is the foundation — it defines the hardware contract that every other layer implements against.

## 7. Key Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Start as `/dev/rvt2X` vs `/dev/accel/accelX` | `/dev/rvt2X` first | Simpler; migrate to accel subsystem when stable |
| DRM/GEM vs custom BO | Custom BO first | Avoid DRM complexity for compute-only device; adopt later if needed |
| Single module vs split | Split: `rvt2_core` + `rvt2_gsp_shim` | Separate firmware management from data path; mirrors real GSP architecture |
| Descriptor format | Fixed-size 64-byte aligned | Cache-line friendly; simple for QEMU emulation |
| Fence model | Monotonic seqno + `dma_fence` | Standard kernel pattern; enables future DRM scheduler integration |
| Build system | Out-of-tree `make -C /lib/modules/$(uname -r)/build M=$(pwd)` | Fast iteration; upstream when stable |

## 8. Milestone Criteria

| Phase | "Done" When |
|-------|-------------|
| 0 | `lspci` shows rvt2 device; MMIO read/write works from guest |
| 1 | `modprobe rvt2_core` succeeds; `/dev/rvt2_0` exists; `cat /sys/class/rvt2/rvt2_0/status` returns OK |
| 2 | Userspace can `ioctl(BO_CREATE)` → `mmap` → read/write → `ioctl(BO_DESTROY)` |
| 3 | Submit ternary matmul job → device computes → IRQ fires → fence signals → correct result |
| 4 | Firmware load + mailbox handshake succeeds; engine capabilities reported |
| 5 | `rvt2_test` passes end-to-end: alloc → fill → submit → wait → verify |
| 6 | compiledd translates simple IR to descriptor chain; test passes |
| 7 | BO allocated from CXL HDM; coherent access from host works |

--- Original Design Draft End ---
