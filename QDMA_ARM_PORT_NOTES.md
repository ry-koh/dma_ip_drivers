# QDMA ARM Compatibility — Internship Change Notes

## Overview

This document summarizes the changes made to the QDMA Linux kernel driver
(`QDMA/linux-kernel/driver`) during this internship to make it compatible
with the ARM64 (`aarch64`) architecture, in addition to the x86_64 platform
it originally targeted.

The port was validated on an **NVIDIA Jetson Orin Nano**, and the driver
built and ran successfully on that ARM64 platform. Further validation
(e.g. sustained-throughput / stress testing against a live FPGA endpoint)
could not be completed before the end of the internship due to hardware
issues unrelated to the driver code itself.

These five commits represent the complete diff between this fork's
`master` branch and the last synced point with the Xilinx (AMD) upstream
`dma_ip_drivers` repository (upstream commit `c510835`,
"Merge pull request #376 from mjthimm/patch-2").

| # | Commit | Summary |
|---|--------|---------|
| 1 | `c741512` | Add ARM architecture-specific time retrieval in `rdtsc_gettime` |
| 2 | `0065702` | Redefine `FIELD_GET` macro after undefining |
| 3 | `525fe91` | Rearrange `qdma_platform.h` header include order in `qdma_platform.c` |
| 4 | `96ac18e` | Update `qdma_mod.c` to size the queue-data array from device capabilities |
| 5 | `809fdeb` | Add DMA memory barriers in the C2H streaming read path |

---

## 1. Add ARM architecture-specific time retrieval in `rdtsc_gettime`

**File:** `QDMA/linux-kernel/driver/libqdma/qdma_descq.c`

**What changed:**
`rdtsc_gettime()` previously always used the x86 `rdtscp` instruction
(inline assembly) to read the CPU timestamp counter. This instruction
does not exist on ARM and would fail to build/assemble on `aarch64`.
The function was updated to branch on architecture at compile time:

- On `__i386__` / `__x86_64__`: keep the existing `rdtscp`-based read.
- On `__aarch64__` / `__arm__`: read the ARM generic timer's virtual
  count register via `mrs %0, cntvct_el0`, which is the ARM equivalent
  of a free-running, low-overhead hardware cycle/time counter.
- Any other architecture now hits a `#error`, so an unsupported target
  fails loudly at compile time instead of silently miscompiling.

**Purpose:**
This is the driver's core high-resolution timestamp source, used for
latency/throughput bookkeeping in the descriptor queue engine. Without
an ARM code path, the file simply would not compile on Jetson's
`aarch64` kernel. This change is what makes the driver buildable on ARM
at all, while leaving x86_64 behavior completely unchanged.

---

## 2. Redefine `FIELD_GET` macro after undefining

**File:** `QDMA/linux-kernel/driver/libqdma/qdma_access/qdma_access_common.h`

**What changed:**
Added an `#ifdef FIELD_GET / #undef FIELD_GET / #endif` guard immediately
before the driver's own `FIELD_GET` macro definition.

**Purpose:**
The Linux kernel's ARM64 headers (`<linux/bitfield.h>`, pulled in
transitively by other ARM64 kernel headers) already define a
`FIELD_GET` macro. On x86_64 kernel builds this collision generally
doesn't surface, but on the Jetson's `aarch64` kernel it triggered a
macro-redefinition warning/error during compilation. Explicitly
undefining any pre-existing `FIELD_GET` before redefining it lets the
driver keep its own bit-field extraction semantics without depending on
whether the kernel happened to have already defined the symbol,
resolving the ARM build failure.

---

## 3. Rearrange `qdma_platform.h` header include order in `qdma_platform.c`

**File:** `QDMA/linux-kernel/driver/libqdma/qdma_platform.c`

**What changed:**
Moved the `#include "qdma_platform.h"` line from the top of the include
block to after `qdma_regs.h`, `qdma_access_errors.h`,
`<linux/errno.h>`, and `<linux/delay.h>`.

**Purpose:**
`qdma_platform.h` relies on type/macro definitions that are pulled in by
the kernel headers included afterward. On x86_64 this ordering happened
to work because of how those headers transitively resolved, but on the
ARM64 kernel header set the original order caused missing-definition
compile errors. Including the kernel headers first ensures all symbols
`qdma_platform.h` depends on are already visible, fixing the ARM64
build without altering any runtime logic.

---

## 4. Update `qdma_mod.c` — size queue metadata from device capabilities

**File:** `QDMA/linux-kernel/driver/src/qdma_mod.c`

**What changed:**
In `probe_one()`, after the device handle (`dev_hndl`) is obtained, the
change reads the device's queue capability (`num_qs`) from the
`xlnx_dma_dev` struct, calls `qdma_set_qmax()` with that value, and on
success reallocates the per-PCIe-function queue data array via
`xpdev_qdata_realloc()` (failing the probe cleanly via `close_device` if
`qdma_set_qmax()` fails).

**Purpose:**
Previously the queue-data structures were not being explicitly sized
against the card's actual maximum queue capability at probe time on
this code path, which surfaced as a functional/robustness issue once
the driver was brought up on the Jetson's PCIe root complex during
testing (different queue/BAR sizing behavior than on the x86 platforms
the driver was originally validated on). This change makes the queue
array allocation track the hardware-reported `num_qs` explicitly during
probe, so downstream queue configuration operates on correctly-sized
data structures regardless of host architecture.

---

## 5. Add memory barriers in the C2H streaming read path

**File:** `QDMA/linux-kernel/driver/libqdma/qdma_st_c2h.c`

**What changed:**
In `descq_st_c2h_read()`:
- Resolves the `struct device *` for the queue's parent PCIe device.
- Before reading a completed descriptor's payload out of a receive
  buffer page, calls `dma_sync_single_for_cpu(..., DMA_FROM_DEVICE)` on
  that buffer's DMA address.
- After the copy-out loop finishes, calls
  `dma_sync_single_for_device(..., DMA_FROM_DEVICE)` on the buffer
  before it is handed back to the refill/completion path.

**Purpose:**
ARM64 has a weaker memory ordering model than x86_64 and does not give
the same implicit CPU/DMA coherency guarantees. Without explicit DMA
sync calls, it is possible for the CPU to read stale (pre-DMA-write)
data out of a completion-queue buffer on ARM, since the architecture
does not guarantee the CPU will observe the device's writes to that
memory without an explicit barrier/sync. Adding
`dma_sync_single_for_cpu()`/`dma_sync_single_for_device()` around the
read of each host-to-card ("C2H", card-to-host) streaming buffer
enforces the correct cache/coherency barrier on architectures that need
it, which is required for correct data integrity on ARM64 and is a
no-op-equivalent safety measure on x86_64.

---

## Testing status

- **Build:** Driver compiles successfully for `aarch64` after these
  five changes.
- **Hardware validation:** Tested on an **NVIDIA Jetson Orin Nano**;
  the driver loaded and the QDMA PCIe endpoint enumerated and operated
  correctly.
- **Incomplete work:** Due to hardware issues encountered late in the
  internship (unrelated to the driver logic itself), extended
  stress/performance testing on ARM could not be completed. The changes
  above represent the full scope of what was implemented and validated
  at a functional level within the internship timeframe.

## Suggested follow-ups

- Extended soak/throughput testing on ARM64 once hardware is available,
  to validate the memory-barrier fix (#5) under sustained load.
- Audit other architecture-specific assumptions in the DPDK reference
  driver (`QDMA/DPDK`) and Windows driver (`QDMA/windows`), which were
  out of scope for this port.
- Upstream these changes to the Xilinx/AMD `dma_ip_drivers` project for
  broader ARM64 support.
