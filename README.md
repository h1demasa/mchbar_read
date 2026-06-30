# MCHBAR Register Reader

Tools for reading the **Processor Memory Controller (MCHBAR) registers** of
13th Gen Intel Core processors (datasheet Vol.2, section 3.2) on Linux.

Two independent front-ends are provided that read the same register table:

- a **user-space tool** (`mchbar_read`) that maps the MMIO window via `/dev/mem`, and
- a **kernel module** (`mchbar_kmod`) that maps it via `ioremap` and exposes it through debugfs.

Both resolve the MCHBAR base the same way and share the register table in
`mchbar_regs.h`.

## Files

| File | Description |
|------|-------------|
| `mchbar_regs.h` | Table of all 118 registers extracted from the datasheet (offset, width, name). Shared by both front-ends. |
| `mchbar_read.c` | User-space tool: resolves the MCHBAR base and reads MMIO via `/dev/mem`. |
| `mchbar_kmod.c` | Kernel module: resolves the base, maps via `ioremap`, and exposes registers through debugfs. |
| `Makefile` | Build definitions (the kernel module is built by default). |

## How the MCHBAR base is resolved

1. Read the 64-bit BAR at offset `0x48` in the config space of the
   Host Bridge/DRAM Controller `0000:00:00.0`.
   - `bit[0]` = `MCHBAREN` (1 = enabled)
   - `bit[41:17]` = physical base address (128KB aligned)
2. Map a 128KB MMIO window read-only starting at that physical base.
3. Read each register with a `volatile` width-specific access (32/64-bit).
   64-bit registers are read as two 32-bit lo/hi accesses and combined, as a
   safety measure for platforms where a single 64-bit MMIO access is split.

---

## User-space tool (`mchbar_read`)

### Build

```sh
cc -O2 -Wall -o mchbar_read mchbar_read.c
```

### Usage

```sh
sudo ./mchbar_read                # dump all registers
sudo ./mchbar_read TC_PRE         # filter by name substring (case-insensitive)
sudo ./mchbar_read 0xE000         # select by offset
sudo ./mchbar_read --bar          # show the MCHBAR base address only
./mchbar_read --list              # list registers (no privileges, no access)
```

### Notes

- **Requires root** (for `/dev/mem` and the MMIO mapping).
- If the kernel is built with `CONFIG_STRICT_DEVMEM=y`, MMIO access through
  `/dev/mem` may be denied. In that case add **`iomem=relaxed`** to the kernel
  boot options, or use the kernel module instead (which is not affected).
- **Read-only**; the tool never writes.

---

## Kernel module (`mchbar_kmod`)

Maps the MCHBAR MMIO region via `ioremap` (not `/dev/mem`) and exposes all 118
registers through debugfs. It is **not affected by `CONFIG_STRICT_DEVMEM`**.

### Prerequisites

Kernel headers (a build environment) are required.

```sh
# Debian/Ubuntu
sudo apt install linux-headers-$(uname -r) build-essential
```

If Secure Boot is enabled, an unsigned module is rejected with
`Key was rejected by service`. Either sign the module with an enrolled MOK or
disable Secure Boot.

### Build and use

```sh
make
sudo insmod mchbar_kmod.ko

# dump all registers
sudo cat /sys/kernel/debug/mchbar/registers

# base address info only
sudo cat /sys/kernel/debug/mchbar/bar

# read an individual register (file name = register name)
sudo cat /sys/kernel/debug/mchbar/TC_PRE_0_0_0_MCHBAR
sudo cat /sys/kernel/debug/mchbar/PWM_RDCAS_COUNT_0_0_0_MCHBAR

sudo rmmod mchbar_kmod
```

Makefile helper targets:

```sh
make load     # build + insmod
make dump     # dump registers
make unload   # rmmod
```

### Design notes

- **Why `request_mem_region` is not called.**
  The MCHBAR region is often reserved by firmware (ACPI `PNP0C02` board
  reservations, etc.), so `request_mem_region` would fail with `-EBUSY` and the
  probe would fail. Since this is read-only, exclusive reservation is
  unnecessary and only `ioremap` is performed.

- **Read-only.**
  No write interface is provided. Writing to timing registers and the like
  would destabilize the memory controller, so it is intentionally excluded.

- **64-bit registers.**
  Read as two `readl` (lo/hi) accesses and combined rather than `readq`, as a
  safety measure for platforms where a single 64-bit MMIO access is split or
  unsupported. If you need an atomic 64-bit snapshot (e.g. to handle RAPL /
  counter wraparound), consider switching to `readq`.

- **Single socket / single controller.**
  The module assumes the single Host Bridge at domain0/bus0/dev0:func0, i.e.
  desktop/mobile single-package 13th Gen configurations.

---

## Register groups

The 118 extracted registers include the following groups:

- `*_MCHBAR_PCU` — power/thermal/RAPL/frequency (PKG/IA/GT C0 counters,
  `DDR_RAPL_LIMIT`, `*_PERF_STATUS`, etc.)
- `IMRnBASE/MASK_*_MCHBAR_IMPH` — Isolated Memory Regions (IMR) 0–18
- `MAD_*_MCHBAR` — memory address decode (channel/DIMM configuration, hashing)
- `TC_*_MCHBAR` — DRAM timing constraints (tPRE/tACT/RDRD/refresh, etc.)
- `PWM_*_COUNT_MCHBAR` — RdCAS/WrCAS/Command counters
- `ECC*_MCHBAR` — ECC error log and injection
- `SC_*_MCHBAR` — scheduler configuration, ODT matrix, round-trip latency

The register table was machine-extracted from datasheet Vol.2. Offsets and
widths have been verified, but for the bit-field definition of each register
refer to the datasheet itself.

### Example: RowHammer / memory controller research

Registers useful for inferring the physical-address-to-DRAM-bank mapping and
for observing access frequency:

```sh
# address decode (channel/DIMM configuration)
sudo cat /sys/kernel/debug/mchbar/MAD_INTER_CHANNEL_0_0_0_MCHBAR
sudo cat /sys/kernel/debug/mchbar/MAD_INTRA_CH0_0_0_0_MCHBAR
sudo cat /sys/kernel/debug/mchbar/MAD_DIMM_CH0_0_0_0_MCHBAR

# channel hash (address interleaving)
sudo cat /sys/kernel/debug/mchbar/CHANNEL_HASH_0_0_0_MCHBAR
sudo cat /sys/kernel/debug/mchbar/CHANNEL_EHASH_0_0_0_MCHBAR

# CAS counters (access volume)
sudo cat /sys/kernel/debug/mchbar/PWM_RDCAS_COUNT_0_0_0_MCHBAR
sudo cat /sys/kernel/debug/mchbar/PWM_WRCAS_COUNT_0_0_0_MCHBAR

# refresh timing
sudo cat /sys/kernel/debug/mchbar/TC_RFTP_0_0_0_MCHBAR
sudo cat /sys/kernel/debug/mchbar/TC_RFP_0_0_0_MCHBAR
```

For periodic sampling, poll the debugfs files with `watch` or a small script;
this lets the user-space side avoid root `/dev/mem` access entirely.
