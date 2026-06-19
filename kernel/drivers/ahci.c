/*
 * kernel/drivers/ahci.c — AHCI (SATA) block device driver, polled.
 *
 * Brings up the first AHCI HBA found on the PCI bus and registers a
 * block_device for every implemented port that reports an attached
 * SATA disk. Each port owns a single command slot plus a one-page DMA
 * bounce buffer; reads and writes are issued as READ/WRITE DMA EXT
 * (LBA48) commands and completion is detected by polling PxCI.
 *
 * A single MSI vector serves the whole HBA (Phase 2). On completion the
 * handler reads the HBA-level IS register, clears the flagged ports'
 * PxIS, and wakes the task sleeping on that port. If MSI allocation
 * fails the driver falls back to the Phase 1 poll path, spinning on the
 * command-issue register exactly like the legacy ATA PIO driver. This
 * mirrors the virtio-blk driver.
 *
 * Per-port DMA layout (one 4 KiB page):
 *   offset 0x000  command list   (32 headers * 32 B = 1024 B, 1 KiB aligned)
 *   offset 0x400  received FIS    (256 B, 256 B aligned)
 *   offset 0x500  command table   (128 B aligned; CFIS + one PRDT entry)
 *
 * Reference: Intel AHCI 1.3.1 spec; SATA 3.0; OSDev "AHCI".
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/arch/irq.h>
#include <jnu/base/types.h>
#include <jnu/drivers/ahci.h>
#include <jnu/drivers/apic.h>
#include <jnu/drivers/msi.h>
#include <jnu/drivers/pci.h>
#include <jnu/fs/block.h>
#include <jnu/kernel/sched.h>
#include <jnu/lib/klog.h>
#include <jnu/lib/spinlock.h>
#include <jnu/lib/string.h>
#include <jnu/mm/paging.h>
#include <jnu/mm/pmm.h>
#include <uapi/jnu/errno.h>

/* ---- HBA (generic host control) registers, dword indexed ---------------- */
#define HBA_CAP 0x00 /* host capabilities */
#define HBA_GHC 0x04 /* global host control */
#define HBA_IS 0x08  /* interrupt status */
#define HBA_PI 0x0C  /* ports implemented */
#define HBA_VS 0x10  /* version */

#define GHC_HR (1u << 0)  /* HBA reset */
#define GHC_IE (1u << 1)  /* interrupt enable */
#define GHC_AE (1u << 31) /* AHCI enable */

#define HBA_PORT_BASE 0x100u
#define HBA_PORT_STRIDE 0x80u

/* ---- per-port registers, offsets from the port register block ----------- */
#define PORT_CLB 0x00  /* command list base (low) */
#define PORT_CLBU 0x04 /* command list base (high) */
#define PORT_FB 0x08   /* FIS base (low) */
#define PORT_FBU 0x0C  /* FIS base (high) */
#define PORT_IS 0x10   /* interrupt status */
#define PORT_IE 0x14   /* interrupt enable */
#define PORT_CMD 0x18  /* command and status */
#define PORT_TFD 0x20  /* task file data */
#define PORT_SIG 0x24  /* signature */
#define PORT_SSTS 0x28 /* SATA status (SCR0) */
#define PORT_SCTL 0x2C /* SATA control (SCR2) */
#define PORT_SERR 0x30 /* SATA error (SCR1) */
#define PORT_SACT 0x34 /* SATA active (SCR3) */
#define PORT_CI 0x38   /* command issue */

#define PXCMD_ST (1u << 0)  /* start */
#define PXCMD_FRE (1u << 4) /* FIS receive enable */
#define PXCMD_FR (1u << 14) /* FIS receive running */
#define PXCMD_CR (1u << 15) /* command list running */

#define PXTFD_ERR (1u << 0)
#define PXTFD_DRQ (1u << 3)
#define PXTFD_BSY (1u << 7)

#define PXIS_TFES (1u << 30) /* task file error status */

/* Per-port interrupt enable bits we care about. */
#define PXIE_DHRE (1u << 0)  /* device-to-host register FIS */
#define PXIE_PSE (1u << 1)   /* PIO setup FIS */
#define PXIE_DSE (1u << 2)   /* DMA setup FIS */
#define PXIE_SDBE (1u << 3)  /* set device bits FIS */
#define PXIE_TFEE (1u << 30) /* task file error */
#define PXIE_MASK (PXIE_DHRE | PXIE_PSE | PXIE_DSE | PXIE_SDBE | PXIE_TFEE)

#define PXSSTS_DET_MASK 0x0Fu
#define PXSSTS_DET_PRESENT 0x03u /* device present + PHY comm established */

#define SATA_SIG_ATA 0x00000101u /* non-packet SATA disk */

/* ATA commands. */
#define ATA_CMD_IDENTIFY 0xECu
#define ATA_CMD_READ_DMA_EXT 0x25u
#define ATA_CMD_WRITE_DMA_EXT 0x35u

/* Register FIS — host to device. */
#define FIS_TYPE_REG_H2D 0x27u
#define FIS_H2D_C (1u << 7) /* command (vs control) */

#define AHCI_SECTOR_SIZE 512u
#define AHCI_CMD_SLOT 0u       /* we only ever use slot 0 */
#define AHCI_BOUNCE_SECTORS 8u /* one 4 KiB page */
#define AHCI_POLL_BUDGET 2000000u
#define AHCI_WAIT_BUDGET 1000000u

/*
 * When a command is issued under interrupts we sleep instead of
 * spinning. The timeout is only a safety valve that drops us back onto
 * the bounded poll path if the MSI is missing or misrouted; it is not
 * the I/O deadline.
 */
#define AHCI_IRQ_WAIT_US 10000u
#define AHCI_IRQ_WAIT_BUDGET 1000u

/*
 * A port owns a single command slot and one shared bounce buffer, so
 * only one transfer may be in flight on it at a time. Because the issue
 * path sleeps (see ahci_issue), a second task can be scheduled into
 * ahci_transfer on the same port while the first sleeps; this budget
 * bounds how long it yields waiting for its turn before giving up.
 */
#define AHCI_BUSY_WAIT_BUDGET 1000000u

/* Offsets of the structures inside the per-port DMA page. */
#define AHCI_OFF_CMD_LIST 0x000u
#define AHCI_OFF_RECV_FIS 0x400u
#define AHCI_OFF_CMD_TABLE 0x500u

/* Command header (32 bytes). 32 of these form the command list. */
struct ahci_cmd_header {
	uint16_t flags;		 /* CFL[4:0], A[5], W[6], P[7], PMP[15:12] */
	uint16_t prdtl;		 /* PRDT entry count */
	volatile uint32_t prdbc; /* PRD byte count transferred */
	uint32_t ctba;		 /* command table base (low) */
	uint32_t ctbau;		 /* command table base (high) */
	uint32_t reserved[4];
} __packed;

/* Physical region descriptor table entry (16 bytes). */
struct ahci_prdt_entry {
	uint32_t dba;  /* data base (low) */
	uint32_t dbau; /* data base (high) */
	uint32_t reserved;
	uint32_t dbc; /* byte count - 1 [21:0], interrupt-on-complete [31] */
} __packed;

/* Command table: 64 B CFIS + 16 B ACMD + 48 B reserved + PRDT entries. */
struct ahci_cmd_table {
	uint8_t cfis[64];
	uint8_t acmd[16];
	uint8_t reserved[48];
	struct ahci_prdt_entry prdt[1];
} __packed;

/* Register FIS — host to device (20 meaningful bytes). */
struct ahci_fis_h2d {
	uint8_t fis_type; /* FIS_TYPE_REG_H2D */
	uint8_t flags;	  /* C bit + port multiplier */
	uint8_t command;
	uint8_t featurel;
	uint8_t lba0, lba1, lba2, device;
	uint8_t lba3, lba4, lba5, featureh;
	uint8_t countl, counth, icc, control;
	uint8_t reserved[4];
} __packed;

struct ahci_port {
	volatile uint32_t *regs; /* port register block in the HHDM */
	int port_no;
	bool present;
	uint64_t sectors;

	struct ahci_cmd_header *cmd_list;
	struct ahci_cmd_table *cmd_table;
	paddr_t dma_phys; /* base of the per-port DMA page */
	uint8_t *bounce;  /* one-page DMA bounce buffer */
	paddr_t bounce_phys;

	char model[41];
	struct block_device bdev;

	struct task *waiter; /* task sleeping on this port's completion */
	bool busy;	     /* a transfer is in flight; serializes the port */
};

#define MAX_AHCI_PORTS 32
static struct ahci_port ports[MAX_AHCI_PORTS];
static size_t port_count;

static volatile uint32_t *hba_regs;

/*
 * A single MSI vector serves the whole HBA. `ahci_lock` guards the
 * per-port `waiter` fields against the interrupt handler; `irq_enabled`
 * latches whether we took the interrupt path or fell back to polling.
 */
static struct spinlock ahci_lock;
static bool ahci_irq_enabled;
static uint8_t ahci_irq_vector;

static const char *port_names[] = {"sda", "sdb", "sdc", "sdd"};
#define MAX_AHCI_DRIVES (sizeof(port_names) / sizeof(port_names[0]))

/* ---- small helpers ------------------------------------------------------ */

static inline uint32_t port_rd(struct ahci_port *p, unsigned off)
{
	return p->regs[off / 4];
}

static inline void port_wr(struct ahci_port *p, unsigned off, uint32_t val)
{
	p->regs[off / 4] = val;
}

/* Use the global `barrier()` macro from include/jnu/base/compiler.h */

/* ---- interrupts --------------------------------------------------------- */

/*
 * Global HBA interrupt service routine. Reads the HBA-level interrupt
 * status, then for every flagged port clears its PxIS and wakes any task
 * sleeping on that port's completion. The HBA-level IS bits are
 * write-1-to-clear and must be cleared after the per-port status.
 */
static void ahci_irq(struct cpu_state *st)
{
	uint32_t is;
	uint64_t flags;

	(void)st;

	is = hba_regs[HBA_IS / 4];

	flags = spin_lock_irqsave(&ahci_lock);
	for (int i = 0; i < MAX_AHCI_PORTS; i++) {
		struct ahci_port *p = &ports[i];

		if (!(is & (1u << i)) || !p->regs)
			continue;

		/* Clear the port-level status (write-1-to-clear). */
		port_wr(p, PORT_IS, port_rd(p, PORT_IS));

		if (p->waiter)
			sched_wake(p->waiter);
	}
	spin_unlock_irqrestore(&ahci_lock, flags);

	/* Clear the HBA-level status last. */
	hba_regs[HBA_IS / 4] = is;
	apic_eoi();
}

/*
 * Allocate one MSI vector for the HBA and point its MSI capability at
 * it. Mirrors the virtio-blk path: on any failure we free the vector
 * and the caller stays on the polled path.
 */
static int ahci_setup_irq(const struct pci_device *pci)
{
	uint8_t vector = 0;
	int err;

	err = irq_alloc_vector(0, ahci_irq, &vector);
	if (err)
		return err;

	err = msi_enable(pci, vector);
	if (err) {
		irq_free_vector(0, vector);
		return err;
	}

	ahci_irq_vector = vector;
	ahci_irq_enabled = true;
	return 0;
}

/* Stop the port engine (clear ST and FRE, wait for CR and FR to clear). */
static void ahci_port_stop(struct ahci_port *p)
{
	uint32_t cmd = port_rd(p, PORT_CMD);
	cmd &= ~(PXCMD_ST | PXCMD_FRE);
	port_wr(p, PORT_CMD, cmd);

	for (unsigned i = 0; i < AHCI_WAIT_BUDGET; i++) {
		if (!(port_rd(p, PORT_CMD) & (PXCMD_CR | PXCMD_FR)))
			return;
		__asm__ __volatile__("pause");
	}
}

/* Start the port engine (set FRE, wait for CR clear, then set ST). */
static void ahci_port_start(struct ahci_port *p)
{
	for (unsigned i = 0; i < AHCI_WAIT_BUDGET; i++) {
		if (!(port_rd(p, PORT_CMD) & PXCMD_CR))
			break;
		__asm__ __volatile__("pause");
	}
	uint32_t cmd = port_rd(p, PORT_CMD);
	cmd |= PXCMD_FRE;
	port_wr(p, PORT_CMD, cmd);
	cmd |= PXCMD_ST;
	port_wr(p, PORT_CMD, cmd);
}

/* Wait until the port is neither busy nor requesting data. */
static int ahci_wait_ready(struct ahci_port *p)
{
	for (unsigned i = 0; i < AHCI_WAIT_BUDGET; i++) {
		uint32_t tfd = port_rd(p, PORT_TFD);
		if (!(tfd & (PXTFD_BSY | PXTFD_DRQ)))
			return 0;
		__asm__ __volatile__("pause");
	}
	return -EIO;
}

/*
 * Issue the command already programmed into slot 0 and poll PxCI for
 * completion. Returns 0 on success, negative errno on error/timeout.
 */
static int ahci_issue(struct ahci_port *p)
{
	uint64_t flags;
	unsigned wait = 0;
	int err;

	/* Clear any stale interrupt/error status before issuing. */
	port_wr(p, PORT_IS, 0xFFFFFFFFu);
	port_wr(p, PORT_SERR, 0xFFFFFFFFu);

	err = ahci_wait_ready(p);
	if (err)
		return err;

	flags = spin_lock_irqsave(&ahci_lock);
	if (ahci_irq_enabled)
		p->waiter = sched_current();

	barrier();
	port_wr(p, PORT_CI, 1u << AHCI_CMD_SLOT);

	/*
	 * Interrupt path: sleep until the handler wakes us (or the command
	 * slot clears under us). The timeout only guards against a missing
	 * or misrouted MSI; on expiry we drop through to the poll loop.
	 */
	while (ahci_irq_enabled && p->waiter &&
	       (port_rd(p, PORT_CI) & (1u << AHCI_CMD_SLOT)) &&
	       !(port_rd(p, PORT_IS) & PXIS_TFES)) {
		spin_unlock_irqrestore(&ahci_lock, flags);
		(void)sched_sleep_timed_interruptible(AHCI_IRQ_WAIT_US);
		flags = spin_lock_irqsave(&ahci_lock);
		if (++wait > AHCI_IRQ_WAIT_BUDGET)
			break;
	}
	p->waiter = NULL;
	spin_unlock_irqrestore(&ahci_lock, flags);

	/*
	 * Confirm completion. The handler clears PxIS, so completion is read
	 * from PxCI and any error from the task-file register, both of which
	 * survive the interrupt being serviced.
	 */
	for (unsigned i = 0; i < AHCI_POLL_BUDGET; i++) {
		if (port_rd(p, PORT_IS) & PXIS_TFES)
			return -EIO;
		if (!(port_rd(p, PORT_CI) & (1u << AHCI_CMD_SLOT))) {
			if (port_rd(p, PORT_TFD) & PXTFD_ERR)
				return -EIO;
			return 0;
		}
		__asm__ __volatile__("pause");
	}
	return -EIO;
}

/*
 * Build the command header + command table for a transfer of `bytes`
 * bytes to/from the bounce buffer and fill the H2D FIS with `command`.
 */
static void ahci_setup(struct ahci_port *p, uint8_t command, uint64_t lba,
		       uint16_t sector_count, uint32_t bytes, bool write)
{
	struct ahci_cmd_header *hdr = &p->cmd_list[AHCI_CMD_SLOT];
	struct ahci_cmd_table *tbl = p->cmd_table;
	struct ahci_fis_h2d *fis;

	memset(tbl, 0, sizeof(*tbl));

	/* Command FIS length in dwords, set the write bit if needed. */
	hdr->flags = (uint16_t)(sizeof(struct ahci_fis_h2d) / 4);
	if (write)
		hdr->flags |= (1u << 6);
	hdr->prdtl = 1;
	hdr->prdbc = 0;
	hdr->ctba = (uint32_t)(p->dma_phys + AHCI_OFF_CMD_TABLE);
	hdr->ctbau = (uint32_t)((p->dma_phys + AHCI_OFF_CMD_TABLE) >> 32);

	tbl->prdt[0].dba = (uint32_t)p->bounce_phys;
	tbl->prdt[0].dbau = (uint32_t)(p->bounce_phys >> 32);
	tbl->prdt[0].dbc = bytes - 1u; /* byte count is zero-based */

	fis = (struct ahci_fis_h2d *)tbl->cfis;
	fis->fis_type = FIS_TYPE_REG_H2D;
	fis->flags = FIS_H2D_C;
	fis->command = command;
	fis->device = 0x40; /* LBA mode */

	fis->lba0 = (uint8_t)(lba & 0xFFu);
	fis->lba1 = (uint8_t)((lba >> 8) & 0xFFu);
	fis->lba2 = (uint8_t)((lba >> 16) & 0xFFu);
	fis->lba3 = (uint8_t)((lba >> 24) & 0xFFu);
	fis->lba4 = (uint8_t)((lba >> 32) & 0xFFu);
	fis->lba5 = (uint8_t)((lba >> 40) & 0xFFu);

	fis->countl = (uint8_t)(sector_count & 0xFFu);
	fis->counth = (uint8_t)((sector_count >> 8) & 0xFFu);
}

/* ---- IDENTIFY ----------------------------------------------------------- */

static uint64_t ahci_identify_sectors(const uint16_t *id)
{
	uint64_t lba48 = (uint64_t)id[100] | ((uint64_t)id[101] << 16) |
			 ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
	if (lba48 != 0)
		return lba48;

	return (uint64_t)id[60] | ((uint64_t)id[61] << 16);
}

static void extract_model(const uint16_t *id, char *out)
{
	for (int i = 0; i < 20; i++) {
		uint16_t w = id[27 + i];
		out[i * 2] = (char)(w >> 8);
		out[i * 2 + 1] = (char)(w & 0xFF);
	}
	out[40] = '\0';
	for (int i = 39; i >= 0 && out[i] == ' '; i--)
		out[i] = '\0';
}

static int ahci_identify(struct ahci_port *p, uint16_t *id_out)
{
	int err;

	ahci_setup(p, ATA_CMD_IDENTIFY, 0, 0, AHCI_SECTOR_SIZE, false);
	err = ahci_issue(p);
	if (err)
		return err;

	memcpy(id_out, p->bounce, AHCI_SECTOR_SIZE);
	return 0;
}

/* ---- block_device ops --------------------------------------------------- */

static int ahci_transfer(struct ahci_port *p, uint64_t lba, size_t count,
			 void *buf, bool write)
{
	uint8_t *p8 = buf;
	unsigned busy_wait = 0;
	uint64_t flags;
	int ret = 0;

	/*
	 * Claim the port. The command slot and bounce buffer are shared, and
	 * ahci_issue() sleeps, so a concurrent caller would otherwise corrupt
	 * an in-flight transfer. Yield until the port is free (or give up).
	 */
	flags = spin_lock_irqsave(&ahci_lock);
	while (p->busy) {
		if (++busy_wait > AHCI_BUSY_WAIT_BUDGET) {
			spin_unlock_irqrestore(&ahci_lock, flags);
			return -EBUSY;
		}
		spin_unlock_irqrestore(&ahci_lock, flags);
		sched_yield();
		flags = spin_lock_irqsave(&ahci_lock);
	}
	p->busy = true;
	spin_unlock_irqrestore(&ahci_lock, flags);

	while (count > 0) {
		uint32_t chunk = (uint32_t)(count < AHCI_BOUNCE_SECTORS
						? count
						: AHCI_BOUNCE_SECTORS);
		uint32_t bytes = chunk * AHCI_SECTOR_SIZE;
		int err;

		if (lba >= p->sectors || lba + chunk > p->sectors) {
			ret = -EINVAL;
			break;
		}

		if (write)
			memcpy(p->bounce, p8, bytes);

		ahci_setup(p,
			   write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT,
			   lba, (uint16_t)chunk, bytes, write);
		err = ahci_issue(p);
		if (err) {
			ret = err;
			break;
		}

		if (!write)
			memcpy(p8, p->bounce, bytes);

		lba += chunk;
		count -= chunk;
		p8 += bytes;
	}

	flags = spin_lock_irqsave(&ahci_lock);
	p->busy = false;
	spin_unlock_irqrestore(&ahci_lock, flags);

	return ret;
}

static int ahci_bdev_read(struct block_device *bdev, uint64_t lba, size_t count,
			  void *buf)
{
	return ahci_transfer(bdev->priv, lba, count, buf, false);
}

static int ahci_bdev_write(struct block_device *bdev, uint64_t lba,
			   size_t count, const void *buf)
{
	return ahci_transfer(bdev->priv, lba, count, (void *)buf, true);
}

static const struct block_ops ahci_blk_ops = {
    .read = ahci_bdev_read,
    .write = ahci_bdev_write,
};

/* ---- port bring-up ------------------------------------------------------ */

static int ahci_port_bringup(struct ahci_port *p)
{
	paddr_t dma;
	paddr_t bounce;

	/* One zeroed page holds the command list, FIS area, and table. */
	dma = pmm_alloc_dma(0);
	if (!dma)
		return -ENOMEM;
	bounce = pmm_alloc_dma(0);
	if (!bounce) {
		pmm_free_pages(dma, 0);
		return -ENOMEM;
	}

	memset(phys_to_virt(dma), 0, PAGE_SIZE);

	p->dma_phys = dma;
	p->cmd_list = (struct ahci_cmd_header *)((uint8_t *)phys_to_virt(dma) +
						 AHCI_OFF_CMD_LIST);
	p->cmd_table = (struct ahci_cmd_table *)((uint8_t *)phys_to_virt(dma) +
						 AHCI_OFF_CMD_TABLE);
	p->bounce_phys = bounce;
	p->bounce = phys_to_virt(bounce);

	ahci_port_stop(p);

	/* Program command list and received-FIS base addresses. */
	port_wr(p, PORT_CLB, (uint32_t)dma);
	port_wr(p, PORT_CLBU, (uint32_t)(dma >> 32));
	port_wr(p, PORT_FB, (uint32_t)(dma + AHCI_OFF_RECV_FIS));
	port_wr(p, PORT_FBU, (uint32_t)((dma + AHCI_OFF_RECV_FIS) >> 32));

	/* Point the single command slot at our command table. */
	p->cmd_list[AHCI_CMD_SLOT].ctba = (uint32_t)(dma + AHCI_OFF_CMD_TABLE);
	p->cmd_list[AHCI_CMD_SLOT].ctbau =
	    (uint32_t)((dma + AHCI_OFF_CMD_TABLE) >> 32);

	port_wr(p, PORT_SERR, 0xFFFFFFFFu);
	port_wr(p, PORT_IS, 0xFFFFFFFFu);

	/* Under MSI, arm the per-port completion/error interrupts. */
	port_wr(p, PORT_IE, ahci_irq_enabled ? PXIE_MASK : 0u);

	ahci_port_start(p);
	return 0;
}

/* ---- public ------------------------------------------------------------- */

void ahci_init(void)
{
	const struct pci_device *pci;
	struct pci_bar_info abar;
	uint32_t pi;
	uint16_t id_buf[256];

	pci = pci_find_class(0x01, 0x06, 0x01);
	if (!pci) {
		pr_info("ahci: no AHCI controller found\n");
		return;
	}

	pci_enable_device(pci);

	/* ABAR is always BAR5 for AHCI. */
	if (pci_read_bar(pci, 5, &abar) != 0 || !abar.is_mmio) {
		pr_err("ahci: ABAR (BAR5) not usable\n");
		return;
	}

	if (paging_ensure_hhdm((paddr_t)abar.base, abar.size) != 0) {
		pr_err("ahci: failed to map ABAR\n");
		return;
	}

	hba_regs = (volatile uint32_t *)phys_to_virt((paddr_t)abar.base);

	spin_lock_init(&ahci_lock);

	/* Enable AHCI mode before touching any port or interrupt state. */
	hba_regs[HBA_GHC / 4] |= GHC_AE;
	barrier();

	/* Clear stale HBA-level interrupt status (write-1-to-clear). */
	hba_regs[HBA_IS / 4] = hba_regs[HBA_IS / 4];

	/*
	 * Try for a single MSI vector covering the whole HBA. On success
	 * enable global interrupts; otherwise stay on the Phase 1 poll path.
	 */
	if (ahci_setup_irq(pci) == 0)
		hba_regs[HBA_GHC / 4] |= GHC_IE;
	else
		hba_regs[HBA_GHC / 4] &= ~GHC_IE;

	pi = hba_regs[HBA_PI / 4];
	pr_info(
	    "ahci: HBA at %02x:%02x.%x ABAR=0x%llx PI=0x%x VS=0x%x irq=%s\n",
	    (unsigned)pci->bus, (unsigned)pci->dev, (unsigned)pci->func,
	    (unsigned long long)abar.base, (unsigned)pi,
	    (unsigned)hba_regs[HBA_VS / 4], ahci_irq_enabled ? "msi" : "poll");

	for (int i = 0; i < MAX_AHCI_PORTS; i++) {
		struct ahci_port *p;
		uint32_t ssts;
		uint32_t sig;

		if (!(pi & (1u << i)))
			continue;

		p = &ports[i];
		p->port_no = i;
		p->regs =
		    (volatile uint32_t *)((uint8_t *)hba_regs + HBA_PORT_BASE +
					  (unsigned)i * HBA_PORT_STRIDE);

		ssts = port_rd(p, PORT_SSTS);
		if ((ssts & PXSSTS_DET_MASK) != PXSSTS_DET_PRESENT)
			continue;

		sig = port_rd(p, PORT_SIG);
		if (sig != SATA_SIG_ATA) {
			pr_info("ahci: port %d sig=0x%x (not a SATA disk), "
				"skipping\n",
				i, (unsigned)sig);
			continue;
		}

		if (port_count >= MAX_AHCI_DRIVES)
			break;

		if (ahci_port_bringup(p) != 0) {
			pr_err("ahci: port %d bring-up failed\n", i);
			continue;
		}

		memset(id_buf, 0, sizeof(id_buf));
		if (ahci_identify(p, id_buf) != 0) {
			pr_err("ahci: port %d IDENTIFY failed\n", i);
			continue;
		}

		p->present = true;
		p->sectors = ahci_identify_sectors(id_buf);
		extract_model(id_buf, p->model);

		p->bdev.name = port_names[port_count];
		p->bdev.sector_size = AHCI_SECTOR_SIZE;
		p->bdev.sector_count = p->sectors;
		p->bdev.ops = &ahci_blk_ops;
		p->bdev.priv = p;

		block_register(&p->bdev);

		pr_info("ahci: %s: '%s' %llu sectors (%llu MiB)\n",
			p->bdev.name, p->model, (unsigned long long)p->sectors,
			(unsigned long long)(p->sectors / 2048));

		port_count++;
	}

	if (port_count == 0)
		pr_info("ahci: no SATA disks found\n");
}

int ahci_selftest(void)
{
	struct ahci_port *p = NULL;
	uint8_t buf[AHCI_SECTOR_SIZE];
	int err;

	for (int i = 0; i < MAX_AHCI_PORTS; i++) {
		if (ports[i].present) {
			p = &ports[i];
			break;
		}
	}

	if (!p) {
		pr_info("ahci_selftest: skipped (no drives)\n");
		return 0;
	}

	memset(buf, 0, sizeof(buf));
	err = ahci_bdev_read(&p->bdev, 0, 1, buf);
	if (err) {
		pr_err("ahci_selftest: read sector 0 failed (err=%d)\n", err);
		return err;
	}

	pr_info("ahci_selftest: sector 0 read OK\n");
	return 0;
}
