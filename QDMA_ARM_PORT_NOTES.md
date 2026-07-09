# QDMA ARM Compatibility — Internship Change Notes

## Overview

This document summarizes the changes made to the QDMA Linux kernel driver
(`QDMA/linux-kernel/driver`) during this internship to make it compatible
with the ARM64 (`aarch64`) architecture, in addition to the x86_64 platform
it originally targeted.

The port was validated on an **NVIDIA Jetson Orin Nano**: the driver
built, loaded, and full DMA transfers were verified working on that
ARM64 platform. Further work could not be completed before the end of
the internship due to hardware issues unrelated to the driver code
itself.

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
`rdtsc_gettime()` is called from two confirmed sites in the driver:
the data-interrupt handler in `qdma_intr.c` (timestamps each IRQ) and
the streaming TX ping-pong path in `qdma_descq.c` (records a
`tx_time`). `rdtscp` is an x86-only instruction mnemonic, so the
original code would fail to assemble on an ARM target — this is a
general fact about the instruction, not something I confirmed via an
actual ARM build log. Adding the `__aarch64__`/`__arm__` branch (using
the ARM generic timer's `cntvct_el0` register) gives this function a
working implementation on ARM, which is necessary for the file to
build on that architecture. x86_64 behavior is unchanged.

---

## 2. Redefine `FIELD_GET` macro after undefining

**File:** `QDMA/linux-kernel/driver/libqdma/qdma_access/qdma_access_common.h`

**What changed:**
Added an `#ifdef FIELD_GET / #undef FIELD_GET / #endif` guard immediately
before the driver's own `FIELD_GET` macro definition.

**Purpose:**
On the ARM build, `FIELD_GET` was already defined as a macro before
this driver header's own definition was reached (via the kernel header
include chain), causing a macro-redefinition conflict. The
`#undef`/redefine guard clears the existing definition first so the
driver's own `FIELD_GET` (with its own semantics, defined via
`FIELD_SHIFT`/the mask-based shift above it) takes effect instead of
whichever `FIELD_GET` the kernel headers pulled in.

---

## 3. Rearrange `qdma_platform.h` header include order in `qdma_platform.c`

**File:** `QDMA/linux-kernel/driver/libqdma/qdma_platform.c`

**What changed:**
Moved the `#include "qdma_platform.h"` line from the top of the include
block to after `qdma_regs.h`, `qdma_access_errors.h`,
`<linux/errno.h>`, and `<linux/delay.h>`.

**Purpose:**
This reorder is tied to change #2 above. `qdma_platform.h` was being
included before `qdma_regs.h`/`qdma_access_errors.h`, which meant the
`FIELD_GET` `#undef`/redefine guard (in `qdma_access_common.h`, pulled
in via this chain) ran before the kernel's own `FIELD_GET` definition
was in scope — so there was nothing yet to `#undef`, and the driver's
redefinition would not actually take effect as intended. Moving the
`qdma_platform.h` include after the other headers ensures the kernel's
`FIELD_GET` is already defined by the time the guard in change #2 runs,
so the `#undef`/redefine actually takes effect.

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
This was fixing an observed bug: without this change, the max queue
pairs (`num_qs`/`qmax`) kept ending up not set — i.e. stayed at `0` —
so the driver had no usable queues configured. Explicitly reading
`num_qs` from the device capabilities and calling `qdma_set_qmax()`
during probe ensures `qmax` is actually set from the hardware-reported
value, and `xpdev_qdata_realloc()` resizes the queue-data array to
match.

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
This was a proactive/defensive change, not a fix for an observed bug.
As general Linux DMA-API background: `dma_sync_single_for_cpu()` /
`dma_sync_single_for_device()` are the standard kernel calls for
handing a DMA buffer's ownership back and forth between the CPU and the
device, used on platforms where the CPU and DMA engine are not fully
cache-coherent by default. Adding these around the C2H buffer read was
a precaution for ARM64's memory-ordering model rather than a response
to a specific data-corruption symptom seen during testing.

---

## Testing status

- **Build:** Driver compiles for `aarch64` after these five changes.
- **Hardware validation:** Tested on an **NVIDIA Jetson Orin Nano** —
  driver built, loaded, and full DMA transfers were verified working.
- **Incomplete work:** Extended validation could not be completed
  before the end of the internship due to a hardware issue unrelated
  to the driver code: the Zynq UltraScale+ board stopped being
  detected on `lspci`. It worked intermittently at first, then the
  frequency of successful detection decreased over time until it
  stopped working entirely.

## Suggested follow-ups

- Extended testing on ARM64 once hardware is available.
- Audit other architecture-specific assumptions in the DPDK reference
  driver (`QDMA/DPDK`) and Windows driver (`QDMA/windows`), which were
  not touched by this port (confirmed — no commits in this diff touch
  those directories).
