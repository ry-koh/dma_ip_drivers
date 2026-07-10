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
`FIELD_GET` was already defined as a macro before this driver header's
own definition was reached (via the kernel header include chain),
causing a macro-redefinition conflict. The `#undef`/redefine guard
clears the existing definition first so the driver's own `FIELD_GET`
(with its own semantics, defined via `FIELD_SHIFT`/the mask-based shift
above it) takes effect instead of whichever `FIELD_GET` the kernel
headers pulled in.

This surfaced during the ARM/Jetson build, but the underlying cause is
**kernel version, not architecture**: on Linux kernel ≥6.9, nearly every
kernel header transitively pulls in `<linux/bitfield.h>` (via
`<linux/fortify-string.h>`, added for `FORTIFY_SOURCE` support), and
that header defines its own `FIELD_GET`. Kernels ≤6.8 don't hit this
because that include path didn't exist yet. This is documented
upstream by AMD in
[Xilinx/dma_ip_drivers#395](https://github.com/Xilinx/dma_ip_drivers/issues/395),
which reports the same collision on RHEL 10, AlmaLinux/Rocky 10
(kernel 6.12), and Ubuntu 24.04 HWE (kernel 6.17) — none of which are
ARM. It just happened to surface here because the Jetson/ARM build in
this project used a kernel ≥6.9, while the x86_64 environment this
driver was previously validated against did not.

Xilinx's own proposed fix in #395 is different in scope: renaming the
driver's `FIELD_GET`/`FIELD_SET`/`FIELD_SHIFT` macros to namespaced
versions (`QDMA_FIELD_GET`, etc.) across roughly 1,000 call sites in
the shared `qdma_access` code used by the Linux, Windows, and DPDK
drivers. The `#undef`/redefine guard used here is a smaller, local
workaround for the same collision, not that fix.

---

## 3. Rearrange `qdma_platform.h` header include order in `qdma_platform.c`

**File:** `QDMA/linux-kernel/driver/libqdma/qdma_platform.c`

**What changed:**
Moved the `#include "qdma_platform.h"` line from the top of the include
block to after `qdma_regs.h`, `qdma_access_errors.h`,
`<linux/errno.h>`, and `<linux/delay.h>`.

**Purpose:**
This reorder is tied to change #2 above and the same kernel-version
root cause described there (kernel ≥6.9 pulling in `<linux/bitfield.h>`
via `<linux/fortify-string.h>`; see
[Xilinx/dma_ip_drivers#395](https://github.com/Xilinx/dma_ip_drivers/issues/395)).
`qdma_platform.h` was being included before `qdma_regs.h`/
`qdma_access_errors.h`, which meant the `FIELD_GET` `#undef`/redefine
guard (in `qdma_access_common.h`, pulled in via this chain) ran before
the kernel's own `FIELD_GET` definition was in scope — so there was
nothing yet to `#undef`, and the driver's redefinition would not
actually take effect as intended. Moving the `qdma_platform.h` include
after the other headers ensures the kernel's `FIELD_GET` is already
defined by the time the guard in change #2 runs, so the
`#undef`/redefine actually takes effect.

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
- Test on other ARM boards/SoCs (e.g. a different Jetson model, or a
  non-Jetson ARM64 platform). Validation so far is limited to a single
  board (Jetson Orin Nano) — success there does not confirm the same
  code path works correctly on other ARM64 hardware, which may differ
  in kernel version, PCIe root complex implementation, or cache/DMA
  coherency behavior.
---

## 6. ARM64 runtime barrier fixes applied to `driver-src/`

**Files:** `libqdma/qdma_descq.c`, `libqdma/qdma_intr.c`

Three `dma_rmb()` / `dma_wmb()` calls were absent from the `driver-src/` codebase and were added at the start of extended testing:

1. **`dma_rmb()` in `descq_mm_n_h2c_cmpl_status()`** — inside the `#ifdef __READ_ONCE_DEFINED__` branch, before the completion status value is used. Ensures the DMA-written status is visible to the CPU before the driver acts on it.

2. **`dma_wmb()` in `descq_mm_proc_request()`** — inserted before the call to `queue_pidx_update()`. Orders all descriptor writes to shared memory before the producer-index doorbell is written to the device.

3. **`dma_rmb()` in `data_intr_aggregate()`** — inside the interrupt aggregation loop, before reading each ring entry. Ensures DMA-written ring entries are visible before the driver reads them.

**Note:** Validation of these fixes was confounded by a probable hardware fault that emerged around the same time (see the hardware status section below). It is not possible to confirm independently that any of these changes were individually necessary or sufficient on the target platform.

---

## 7. Phase 2: `qdma_perf_mon.v` — hardware performance measurement IP

A custom Verilog IP (`rtl/qdma_perf_mon.v`) was written to measure DMA throughput and latency for all four QDMA channel types (ST H2C, ST C2H, MM H2C, MM C2H) directly in FPGA fabric, without relying on software-side timing.

**Interfaces:**

- `s_axil_*` — AXI4-Lite slave, used by the test script to read/write control registers via `/dev/mem`
- `s_axi_*` — AXI4 full slave, connected to QDMA's `M_AXI` port; accepts MM write bursts (discards data, sends BRESP) and handles MM read bursts (returns zeros); no external BRAM
- `m_axis_h2c_*` — ST H2C sink (always asserts `tready`); captures latency from first `TVALID` to `TLAST`
- `s_axis_c2h_*` — ST C2H source; generates `C2H_BEATS` full-width zero beats, then sends a completion

**Parameters:** `DATA_WIDTH = 128` (16 bytes per beat), `AXI_ADDR_WIDTH = 64`, `AXI_ID_WIDTH = 4`

**Register map (AXI4-Lite, 32-bit registers):**

| Offset | Name | R/W | Description |
|--------|------|-----|-------------|
| 0x00 | CTRL | W | [0] = run, [1] = clear |
| 0x04 | STATUS | R | [3:0] = {mm_c2h, mm_h2c, st_c2h, st_h2c}_done |
| 0x08 | C2H_QID | RW | [10:0] queue ID for ST C2H generation |
| 0x0C | C2H_BEATS | RW | [15:0] number of 128-bit beats to generate (max 4095) |
| 0x10 | ST_H2C_LAT_LO | R | ST H2C latency cycles [31:0] |
| 0x14 | ST_H2C_LAT_HI | R | ST H2C latency cycles [47:32] |
| 0x18 | ST_H2C_BYTES | R | Total bytes received |
| 0x1C | ST_H2C_BEATS | R | Total beats received |
| 0x20 | ST_C2H_LAT_LO | R | ST C2H latency cycles [31:0] |
| 0x24 | ST_C2H_LAT_HI | R | ST C2H latency cycles [47:32] |
| 0x28 | ST_C2H_BYTES | R | Total bytes sent |
| 0x2C | ST_C2H_BEATS | R | Total beats sent |
| 0x30 | MM_H2C_LAT_LO | R | MM H2C latency cycles [31:0] (awvalid → bvalid+bready) |
| 0x34 | MM_H2C_LAT_HI | R | [47:32] |
| 0x38 | MM_H2C_BYTES | R | Total bytes written |
| 0x3C | MM_H2C_BEATS | R | Total write beats |
| 0x40 | MM_C2H_LAT_LO | R | MM C2H latency cycles [31:0] (arvalid → rlast+rready) |
| 0x44 | MM_C2H_LAT_HI | R | [47:32] |
| 0x48 | MM_C2H_BYTES | R | Total bytes read |
| 0x4C | MM_C2H_BEATS | R | Total read beats |

Latency counters are 48-bit (free-running at FPGA clock frequency, ~33-hour wrap at 250 MHz). The descriptor credit interface (`dsc_crdt_in`) was handled at the block design level via Constant IPs; it is not a port on `qdma_perf_mon.v`.

---

## 8. Phase 2 hardware test results

Tested on Zynq UltraScale+ ZCU106 (EP) + Jetson Orin Nano (RC). Three of the four channels completed; ST C2H failed.

| Channel | Throughput | Latency |
|---------|-----------|---------|
| ST H2C | 22.947 Gbps | 357 cycles |
| MM H2C | 28.444 Gbps | 72 cycles |
| MM C2H | 31.752 Gbps | 258 cycles |
| ST C2H | FAIL — descriptor never fetched (cidx stayed at 0) | — |

A subsequent test (v2) produced a PCIe error in `dmesg` (`AER: Uncorrected (Fatal) error received`, `SDES (First)`) with the device returning `0xFFFFFFFF` on all reads. The exact cause was not isolated; possible contributors include the credit dispenser configuration issuing continuous credits causing a queue overflow, a C2H abort timeout following the ST C2H descriptor fetch failure, or physical link degradation. The ZCU106 was subsequently retired due to hardware issues.

---

## 9. Hardware platform change — work unfinished

After the ZCU106 was retired, work was attempted on a new hardware platform (Xilinx Versal VCK190 as EP, Nvidia Jetson NX as RC). The VCK190 could not be made to appear in `lspci` at all during the internship period. This work is unfinished and the PCIe enumeration problem was not resolved before the end of the internship.