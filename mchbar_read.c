// SPDX-License-Identifier: MIT
//
// mchbar_read.c
//
//   Tool that reads the Processor Memory Controller (MCHBAR) registers of
//   13th Gen Intel(R) Core(TM) processors from Linux user space.
//
//   MCHBAR is a 64-bit BAR stored at offset 48h in the config space of the
//   Host Bridge/DRAM Controller (Bus0:Dev0:Func0). It maps a 128KB MMIO
//   window into the CPU physical address space.
//     - bit[0]      : MCHBAREN (1 = enabled)
//     - bit[41:17]  : base address bit[41:17]
//                     (i.e. the physical base aligned to a 128KB boundary)
//   (datasheet Vol.2, section 3.1.4)
//
//   By default the BAR is obtained by reading the PCI config space through
//   sysfs (/sys/bus/pci/devices/0000:00:00.0/config), and the MMIO window
//   itself is read by mmap()ing /dev/mem.
//
//   Build:  cc -O2 -Wall -o mchbar_read mchbar_read.c
//   Run:    sudo ./mchbar_read              (dump all registers)
//           sudo ./mchbar_read TC_PRE        (filter by name substring)
//           sudo ./mchbar_read 0xE000        (select by offset)
//           sudo ./mchbar_read --bar         (show the MCHBAR base only)
//
//   Notes:
//     * Requires root privileges.
//     * If the kernel was built with CONFIG_STRICT_DEVMEM=y, MMIO access
//       through /dev/mem may be restricted. In that case add the kernel
//       boot option iomem=relaxed.
//     * Reading some registers (e.g. RAPL counters) is unlikely to have
//       side effects, and this tool only ever reads.

#define _GNU_SOURCE   /* for strcasestr */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/types.h>

#include "mchbar_regs.h"

#define MCHBAR_WINDOW_SIZE  (128 * 1024)   /* 128KB */
#define PCI_CFG_PATH        "/sys/bus/pci/devices/0000:00:00.0/config"
#define MCHBAR_CFG_OFFSET   0x48           /* offset within Dev0 config space */
#define DEVMEM_PATH         "/dev/mem"

// Read the raw 64-bit MCHBAR value from PCI config space.
static int read_mchbar_bar(uint64_t *bar_raw)
{
    int fd = open(PCI_CFG_PATH, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "cannot open PCI config (%s): %s\n",
                PCI_CFG_PATH, strerror(errno));
        return -1;
    }
    uint64_t v = 0;
    ssize_t n = pread(fd, &v, sizeof(v), MCHBAR_CFG_OFFSET);
    close(fd);
    if (n != (ssize_t)sizeof(v)) {
        fprintf(stderr, "failed to read the MCHBAR register: %s\n",
                strerror(errno));
        return -1;
    }
    *bar_raw = v;
    return 0;
}

// Extract the physical base address and the enable flag from the raw BAR.
static uint64_t decode_mchbar_base(uint64_t bar_raw, int *enabled)
{
    if (enabled)
        *enabled = (int)(bar_raw & 0x1);          /* bit0 = MCHBAREN */
    /* bit[41:17] is the base address; the low 17 bits are 0 (128KB aligned) */
    return bar_raw & (((uint64_t)1 << 42) - 1) & ~(((uint64_t)1 << 17) - 1);
}

// Map the MMIO window from /dev/mem.
static volatile uint8_t *map_mchbar(uint64_t phys_base)
{
    int fd = open(DEVMEM_PATH, O_RDONLY | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "cannot open %s: %s\n", DEVMEM_PATH, strerror(errno));
        return NULL;
    }
    void *p = mmap(NULL, MCHBAR_WINDOW_SIZE, PROT_READ,
                   MAP_SHARED, fd, (off_t)phys_base);
    close(fd);
    if (p == MAP_FAILED) {
        fprintf(stderr, "mmap failed (phys=0x%lx): %s\n",
                (unsigned long)phys_base, strerror(errno));
        fprintf(stderr,
            "  This may be caused by CONFIG_STRICT_DEVMEM.\n"
            "  Add iomem=relaxed to the kernel boot options.\n");
        return NULL;
    }
    return (volatile uint8_t *)p;
}

/* MMIO must be accessed in a single access of the specified width on a
 * natural boundary. Splitting into byte-wise copies (e.g. memcpy) would
 * corrupt the register read, so use volatile width-specific accesses. */
static inline uint32_t mmio_read32(volatile uint8_t *base, uint32_t off)
{
    return *(volatile uint32_t *)(base + off);
}
static inline uint64_t mmio_read64(volatile uint8_t *base, uint32_t off)
{
    /* For 64-bit registers, read the low/high 32 bits separately and combine
     * them (a safety measure, since a single 64-bit access is split on some
     * platforms). */
    uint32_t lo = *(volatile uint32_t *)(base + off);
    uint32_t hi = *(volatile uint32_t *)(base + off + 4);
    return ((uint64_t)hi << 32) | lo;
}

static void dump_one(volatile uint8_t *base, const struct mchbar_reg *r)
{
    if (r->width == 64) {
        uint64_t v = mmio_read64(base, r->offset);
        printf("  [0x%04X] %-46s = 0x%016lx\n",
               r->offset, r->name, (unsigned long)v);
    } else {
        uint32_t v = mmio_read32(base, r->offset);
        printf("  [0x%04X] %-46s = 0x%08x\n",
               r->offset, r->name, v);
    }
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [option | filter]\n"
        "  (no argument)     dump all MCHBAR registers\n"
        "  <string>          show only registers whose name matches (case-insensitive)\n"
        "  0x<offset>        show only the register at the given offset\n"
        "  --bar             show the MCHBAR base address info only\n"
        "  --list            list registers (name and offset only, no access)\n"
        "  -h, --help        this help\n",
        prog);
}

int main(int argc, char **argv)
{
    const char *filter = NULL;
    int bar_only = 0, list_only = 0;
    long want_offset = -1;

    if (argc > 1) {
        if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
            usage(argv[0]);
            return 0;
        } else if (!strcmp(argv[1], "--bar")) {
            bar_only = 1;
        } else if (!strcmp(argv[1], "--list")) {
            list_only = 1;
        } else if (!strncmp(argv[1], "0x", 2) || !strncmp(argv[1], "0X", 2)) {
            want_offset = strtol(argv[1], NULL, 16);
        } else {
            filter = argv[1];
        }
    }

    /* --list needs no privileges (it only prints the table) */
    if (list_only) {
        printf("MCHBAR register list (%d entries):\n", MCHBAR_REG_COUNT);
        for (int i = 0; i < MCHBAR_REG_COUNT; i++)
            printf("  0x%04X  %2dbit  %s\n",
                   mchbar_regs[i].offset, mchbar_regs[i].width,
                   mchbar_regs[i].name);
        return 0;
    }

    uint64_t bar_raw;
    if (read_mchbar_bar(&bar_raw) < 0)
        return 1;

    int enabled = 0;
    uint64_t phys = decode_mchbar_base(bar_raw, &enabled);

    printf("MCHBAR (Bus0:Dev0:Func0 + 0x48)\n");
    printf("  raw      : 0x%016lx\n", (unsigned long)bar_raw);
    printf("  base     : 0x%010lx\n", (unsigned long)phys);
    printf("  MCHBAREN : %d (%s)\n", enabled, enabled ? "enabled" : "disabled");
    printf("  window   : %d KB\n\n", MCHBAR_WINDOW_SIZE / 1024);

    if (!enabled) {
        fprintf(stderr,
            "warning: MCHBAREN=0. MMIO accesses will not be decoded.\n");
    }
    if (bar_only)
        return 0;

    if (phys == 0) {
        fprintf(stderr, "base address is 0; aborting the read.\n");
        return 1;
    }

    volatile uint8_t *base = map_mchbar(phys);
    if (!base)
        return 1;

    int shown = 0;
    printf("register values:\n");
    for (int i = 0; i < MCHBAR_REG_COUNT; i++) {
        const struct mchbar_reg *r = &mchbar_regs[i];
        if (want_offset >= 0 && (long)r->offset != want_offset)
            continue;
        if (filter && !strcasestr(r->name, filter))
            continue;
        dump_one(base, r);
        shown++;
    }
    if (shown == 0)
        printf("  (no register matched the filter)\n");

    munmap((void *)base, MCHBAR_WINDOW_SIZE);
    return 0;
}
