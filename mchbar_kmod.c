// SPDX-License-Identifier: GPL-2.0
/*
 * mchbar_kmod.c
 *
 *   Module that reads the Processor Memory Controller (MCHBAR) registers of
 *   13th Gen Intel(R) Core(TM) processors from kernel space via ioremap.
 *
 *   Because it does not use /dev/mem, it is unaffected by CONFIG_STRICT_DEVMEM.
 *
 *   Resolving MCHBAR:
 *     Read the 64-bit BAR at offset 48h in the config space of the
 *     Host Bridge/DRAM Controller (Bus0:Dev0:Func0) with
 *     pci_read_config_dword.
 *       - bit[0]     : MCHBAREN (1 = enabled)
 *       - bit[41:17] : base address (128KB aligned)
 *     (datasheet Vol.2, section 3.1.4)
 *
 *   Interface (debugfs, /sys/kernel/debug/mchbar/):
 *     bar          - MCHBAR base address info (text)
 *     registers    - dump of every register's name/offset/value (text)
 *     <REG_NAME>   - per-register file; read returns the current value
 *
 *   Build / use:
 *     make
 *     sudo insmod mchbar_kmod.ko
 *     sudo cat /sys/kernel/debug/mchbar/registers
 *     sudo cat /sys/kernel/debug/mchbar/TC_PRE_0_0_0_MCHBAR
 *     sudo rmmod mchbar_kmod
 *
 *   Notes:
 *     - Read-only; no write interface is provided.
 *     - The MCHBAR region may be reserved by firmware (e.g. ACPI PNP0C02),
 *       so request_mem_region is not called and only ioremap is used
 *       (to avoid probe failure from conflicting with the firmware
 *       reservation).
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "mchbar_regs.h"

#define MCHBAR_DIR_NAME		"mchbar"	/* debugfs directory name */
#define MCHBAR_WINDOW_SIZE	(128 * 1024)	/* 128KB */
#define MCHBAR_CFG_OFFSET	0x48		/* offset within Dev0 config space */

/* The Host Bridge is fixed at PCI domain0, bus0, devfn 0:0 */
#define HOSTBRIDGE_BUS		0
#define HOSTBRIDGE_DEVFN	PCI_DEVFN(0, 0)

static void __iomem	*mchbar_base;	/* ioremap'd virtual address */
static u64		mchbar_phys;	/* physical base address */
static u64		mchbar_bar_raw;	/* raw BAR value */
static int		mchbar_enabled;	/* MCHBAREN */
static struct dentry	*mchbar_dir;	/* debugfs root */

/* ---- register read -------------------------------------------------- */

static u64 mchbar_read_reg(const struct mchbar_reg *r)
{
	if (r->width == 64) {
		/* read 64-bit as separate lo/hi and combine (safety measure) */
		u32 lo = readl(mchbar_base + r->offset);
		u32 hi = readl(mchbar_base + r->offset + 4);

		return ((u64)hi << 32) | lo;
	}
	return readl(mchbar_base + r->offset);
}

/* ---- debugfs: registers (full dump) --------------------------------- */

static int mchbar_registers_show(struct seq_file *s, void *unused)
{
	int i;

	seq_printf(s, "MCHBAR base = 0x%010llx  EN=%d  window=%dKB\n\n",
		   (unsigned long long)mchbar_phys, mchbar_enabled,
		   MCHBAR_WINDOW_SIZE / 1024);

	for (i = 0; i < MCHBAR_REG_COUNT; i++) {
		const struct mchbar_reg *r = &mchbar_regs[i];
		u64 v = mchbar_read_reg(r);

		if (r->width == 64)
			seq_printf(s, "[0x%04X] %-46s = 0x%016llx\n",
				   r->offset, r->name,
				   (unsigned long long)v);
		else
			seq_printf(s, "[0x%04X] %-46s = 0x%08x\n",
				   r->offset, r->name, (u32)v);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mchbar_registers);

/* ---- debugfs: bar (base info) --------------------------------------- */

static int mchbar_bar_show(struct seq_file *s, void *unused)
{
	seq_printf(s, "raw    : 0x%016llx\n",
		   (unsigned long long)mchbar_bar_raw);
	seq_printf(s, "base   : 0x%010llx\n",
		   (unsigned long long)mchbar_phys);
	seq_printf(s, "enabled: %d\n", mchbar_enabled);
	seq_printf(s, "window : %d KB\n", MCHBAR_WINDOW_SIZE / 1024);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mchbar_bar);

/* ---- debugfs: per-register files ------------------------------------ */
/*
 * Expose each register as one file. The table element pointer is stored in
 * ->private, and on read the current value is returned as hex text.
 */

static int mchbar_one_show(struct seq_file *s, void *unused)
{
	const struct mchbar_reg *r = s->private;
	u64 v = mchbar_read_reg(r);

	if (r->width == 64)
		seq_printf(s, "0x%016llx\n", (unsigned long long)v);
	else
		seq_printf(s, "0x%08x\n", (u32)v);
	return 0;
}

static int mchbar_one_open(struct inode *inode, struct file *file)
{
	return single_open(file, mchbar_one_show, inode->i_private);
}

static const struct file_operations mchbar_one_fops = {
	.owner		= THIS_MODULE,
	.open		= mchbar_one_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* ---- MCHBAR base resolution ----------------------------------------- */

static int mchbar_resolve_base(void)
{
	struct pci_bus *bus;
	struct pci_dev *hb;
	u32 lo = 0, hi = 0;

	bus = pci_find_bus(0, HOSTBRIDGE_BUS);
	if (!bus) {
		pr_err("PCI bus 0 not found\n");
		return -ENODEV;
	}

	hb = pci_get_slot(bus, HOSTBRIDGE_DEVFN);
	if (!hb) {
		pr_err("Host Bridge (0:0.0) not found\n");
		return -ENODEV;
	}

	/* read the 64-bit BAR as two 32-bit reads */
	pci_read_config_dword(hb, MCHBAR_CFG_OFFSET, &lo);
	pci_read_config_dword(hb, MCHBAR_CFG_OFFSET + 4, &hi);
	pci_dev_put(hb);

	mchbar_bar_raw = ((u64)hi << 32) | lo;
	mchbar_enabled = mchbar_bar_raw & 0x1;		/* bit0 */

	/* bit[41:17] is the base; clear the low 17 bits for 128KB alignment */
	mchbar_phys = mchbar_bar_raw &
		      ((1ULL << 42) - 1) & ~((1ULL << 17) - 1);

	if (!mchbar_enabled)
		pr_warn("MCHBAREN=0: MMIO may not be decoded\n");

	if (!mchbar_phys) {
		pr_err("MCHBAR base address is 0\n");
		return -ENODEV;
	}

	pr_info("MCHBAR base=0x%010llx (raw=0x%016llx, EN=%d)\n",
		(unsigned long long)mchbar_phys,
		(unsigned long long)mchbar_bar_raw, mchbar_enabled);
	return 0;
}

/* ---- debugfs tree construction -------------------------------------- */

static int mchbar_build_debugfs(void)
{
	int i;

	mchbar_dir = debugfs_create_dir(MCHBAR_DIR_NAME, NULL);
	if (IS_ERR(mchbar_dir))
		return PTR_ERR(mchbar_dir);

	debugfs_create_file("bar", 0400, mchbar_dir, NULL,
			    &mchbar_bar_fops);
	debugfs_create_file("registers", 0400, mchbar_dir, NULL,
			    &mchbar_registers_fops);

	/* One file per register; pass the table element pointer as ->private.
	 * The table is static const and lives for the module's lifetime, so
	 * this is safe. */
	for (i = 0; i < MCHBAR_REG_COUNT; i++)
		debugfs_create_file(mchbar_regs[i].name, 0400, mchbar_dir,
				    (void *)&mchbar_regs[i],
				    &mchbar_one_fops);

	return 0;
}

/* ---- module init/exit ----------------------------------------------- */

static int __init mchbar_init(void)
{
	int ret;

	ret = mchbar_resolve_base();
	if (ret)
		return ret;

	/*
	 * request_mem_region is intentionally skipped: the MCHBAR region is
	 * often reserved by firmware (e.g. ACPI PNP0C02), which would cause
	 * -EBUSY. Since this is read-only, only ioremap is performed.
	 */
	mchbar_base = ioremap(mchbar_phys, MCHBAR_WINDOW_SIZE);
	if (!mchbar_base) {
		pr_err("ioremap failed (phys=0x%010llx)\n",
		       (unsigned long long)mchbar_phys);
		return -ENOMEM;
	}

	ret = mchbar_build_debugfs();
	if (ret) {
		pr_err("debugfs construction failed: %d\n", ret);
		iounmap(mchbar_base);
		return ret;
	}

	pr_info("loaded: exposed %d registers under /sys/kernel/debug/%s/\n",
		MCHBAR_REG_COUNT, MCHBAR_DIR_NAME);
	return 0;
}

static void __exit mchbar_exit(void)
{
	debugfs_remove_recursive(mchbar_dir);
	if (mchbar_base)
		iounmap(mchbar_base);
	pr_info("unloaded\n");
}

module_init(mchbar_init);
module_exit(mchbar_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hidemasa Kawasaki");
MODULE_DESCRIPTION("13th Gen Intel Core MCHBAR register reader via ioremap/debugfs");
MODULE_VERSION("1.0");
