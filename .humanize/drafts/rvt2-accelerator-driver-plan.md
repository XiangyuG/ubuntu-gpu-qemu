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
