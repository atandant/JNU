/*
 * kernel/drivers/virtio_blk.c — VirtIO block device driver (PCI modern).
 *
 * Probes 0x1AF4:0x1042 (modern) and 0x1AF4:0x1001 (transitional),
 * sets up one polled virtqueue, and registers "vda" with the block
 * layer. Reads and writes use DMA bounce buffers in ZONE_DMA.
 *
 * The single virtqueue is serialized with a per-device spinlock so the
 * shared descriptor ring and request metadata cannot be corrupted by
 * concurrent block-layer callers. If the device fails to complete a
 * request within the poll budget the device is latched "dead": the
 * request is still outstanding (the device may DMA into the bounce
 * buffer at any later time), so that buffer is intentionally leaked
 * rather than freed, and all subsequent I/O is rejected.
 *
 * Reference: VirtIO 1.1/1.2 spec (block device, PCI transport).
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/base/types.h>
#include <jnu/drivers/io.h>
#include <jnu/drivers/pci.h>
#include <jnu/drivers/virtio_blk.h>
#include <jnu/fs/block.h>
#include <jnu/lib/klog.h>
#include <jnu/lib/spinlock.h>
#include <jnu/lib/string.h>
#include <jnu/mm/paging.h>
#include <jnu/mm/pmm.h>
#include <uapi/jnu/errno.h>

#define VIRTIO_VENDOR_ID 0x1AF4u
#define VIRTIO_DEV_BLK_MODERN 0x1042u
#define VIRTIO_DEV_BLK_LEGACY 0x1001u

#define VIRTIO_PCI_CAP_VENDOR 0x09u

#define VIRTIO_PCI_CAP_COMMON_CFG 1u
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2u
#define VIRTIO_PCI_CAP_DEVICE_CFG 4u

#define VIRTIO_F_VERSION_1 (1u << 0) /* feature word 1, bit 0 */

#define VIRTIO_STATUS_ACKNOWLEDGE 1u
#define VIRTIO_STATUS_DRIVER 2u
#define VIRTIO_STATUS_DRIVER_OK 4u
#define VIRTIO_STATUS_FEATURES_OK 8u
#define VIRTIO_STATUS_FAILED 128u

#define VIRTIO_NO_VECTOR 0xFFFFu

#define VIRTIO_BLK_F_RO (1u << 5)

#define VIRTIO_BLK_T_IN 0u
#define VIRTIO_BLK_T_OUT 1u
#define VIRTIO_BLK_T_FLUSH 4u
#define VIRTIO_BLK_S_OK 0u

#define VIRTQ_DESC_F_NEXT 1u
#define VIRTQ_DESC_F_WRITE 2u

#define VIRTQ_AVAIL_F_NO_INTERRUPT 1u

#define VIRTIO_BLK_SECTOR_SIZE 512u
#define VIRTQ_SIZE 32u
#define VIRTIO_PCI_CAP_MAX 256u

#define VIRTIO_POLL_BUDGET 1000000u

#define PAGE_DATA_BYTES PAGE_SIZE

struct virtq_desc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
} __packed;

struct virtq_avail_hdr {
	uint16_t flags;
	uint16_t idx;
} __packed;

struct virtq_used_hdr {
	uint16_t flags;
	uint16_t idx;
} __packed;

struct virtq_used_elem {
	uint32_t id;
	uint32_t len;
} __packed;

struct virtio_pci_common_cfg {
	uint32_t device_feature_select;
	uint32_t device_feature;
	uint32_t driver_feature_select;
	uint32_t driver_feature;
	uint16_t msix_config;
	uint16_t num_queues;
	uint8_t device_status;
	uint8_t config_generation;
	uint16_t queue_select;
	uint16_t queue_size;
	uint16_t queue_msix_vector;
	uint16_t queue_enable;
	uint16_t queue_notify_off;
	uint64_t queue_desc;
	uint64_t queue_driver;
	uint64_t queue_device;
} __packed;

struct virtio_blk_config {
	uint64_t capacity;
	uint32_t size_max;
	uint32_t seg_max;
} __packed;

struct virtio_blk_outhdr {
	uint32_t type;
	uint32_t reserved;
	uint64_t sector;
} __packed;

struct virtio_pci_cap {
	uint8_t bar;
	uint32_t offset;
	uint32_t length;
};

struct virtio_notify {
	bool is_port;
	uint32_t multiplier;
	uint16_t port;
	volatile uint16_t *mmio;
};

struct virtio_mmio {
	volatile struct virtio_pci_common_cfg *common;
	volatile uint8_t *device_cfg;
	struct virtio_notify notify;
};

struct virtio_queue {
	struct virtq_desc *desc;
	volatile struct virtq_avail_hdr *avail;
	volatile struct virtq_used_hdr *used_hdr;
	struct virtq_used_elem *used;
	uint16_t size;
	uint16_t last_used_idx;
	paddr_t mem_pa;
};

struct virtio_req_meta {
	struct virtio_blk_outhdr hdr;
	uint8_t status;
} __packed;

struct virtio_blk_dev {
	struct virtio_mmio mmio;
	struct virtio_queue vq;
	struct virtio_req_meta *req_meta;
	paddr_t req_meta_pa;
	uint64_t capacity;
	bool read_only;
	struct block_device bdev;
	struct spinlock lock;
	bool ready;
	bool dead;
};

static struct virtio_blk_dev blk_dev;
static bool virtio_present;

static inline uint8_t pci_cfg8(const struct pci_device *pci, uint8_t off)
{
	return pci_read_config_byte(pci->bus, pci->dev, pci->func, off);
}

static int virtio_find_cap(const struct pci_device *pci, uint8_t cfg_type,
			   struct virtio_pci_cap *cap)
{
	uint8_t ptr = pci_cfg8(pci, 0x34);
	unsigned steps = 0;

	while (ptr != 0 && steps++ < VIRTIO_PCI_CAP_MAX) {
		if (pci_cfg8(pci, ptr) == VIRTIO_PCI_CAP_VENDOR &&
		    pci_cfg8(pci, (uint8_t)(ptr + 3)) == cfg_type) {
			cap->bar = pci_cfg8(pci, (uint8_t)(ptr + 4));
			cap->offset = pci_read_config_dword(
			    pci->bus, pci->dev, pci->func, (uint8_t)(ptr + 8));
			cap->length = pci_read_config_dword(
			    pci->bus, pci->dev, pci->func, (uint8_t)(ptr + 12));
			return 0;
		}
		ptr = pci_cfg8(pci, (uint8_t)(ptr + 1));
	}
	return -ENODEV;
}

/*
 * Map `map_len` bytes of a capability's MMIO region into the HHDM.
 * Fails (rather than silently mapping fewer bytes) if the BAR cannot
 * satisfy the requested length, so callers never dereference past the
 * mapped window.
 */
static int virtio_map_mmio_cap(const struct pci_device *pci,
			       const struct virtio_pci_cap *cap, size_t map_len,
			       volatile void **out)
{
	struct pci_bar_info bar;
	uint64_t phys;
	uint64_t avail;
	size_t len;
	int err;

	err = pci_read_bar(pci, cap->bar, &bar);
	if (err || !bar.is_mmio)
		return -ENODEV;

	if (cap->offset >= bar.size)
		return -ENODEV;

	avail = bar.size - cap->offset;
	if ((uint64_t)map_len > avail)
		return -ENODEV;

	len = cap->length ? cap->length : 1;
	if (map_len > len)
		len = map_len;
	if ((uint64_t)len > avail)
		len = (size_t)avail;
	if (len == 0)
		return -ENODEV;

	phys = bar.base + cap->offset;
	if (paging_ensure_hhdm((paddr_t)phys, len))
		return -ENOMEM;

	*out = phys_to_virt((paddr_t)phys);
	return 0;
}

/*
 * Map a NOTIFY_CFG capability. Prefer MMIO notify regions: they are
 * easier to reason about and avoid depending on legacy PIO BAR layout.
 * Fall back to the first usable PIO notify cap if no MMIO cap exists.
 */
static int virtio_map_notify(const struct pci_device *pci,
			     struct virtio_notify *notify)
{
	uint8_t ptr = pci_cfg8(pci, 0x34);
	unsigned steps = 0;
	int pio_err = -ENODEV;
	bool pio_saved = false;
	struct virtio_notify pio_candidate;

	memset(notify, 0, sizeof(*notify));
	memset(&pio_candidate, 0, sizeof(pio_candidate));

	while (ptr != 0 && steps++ < VIRTIO_PCI_CAP_MAX) {
		if (pci_cfg8(pci, ptr) == VIRTIO_PCI_CAP_VENDOR &&
		    pci_cfg8(pci, (uint8_t)(ptr + 3)) ==
			VIRTIO_PCI_CAP_NOTIFY_CFG) {
			struct virtio_pci_cap cap;
			struct pci_bar_info bar;
			struct virtio_notify candidate;
			uint32_t mult;
			int err;

			memset(&candidate, 0, sizeof(candidate));
			cap.bar = pci_cfg8(pci, (uint8_t)(ptr + 4));
			cap.offset = pci_read_config_dword(
			    pci->bus, pci->dev, pci->func, (uint8_t)(ptr + 8));
			cap.length = pci_read_config_dword(
			    pci->bus, pci->dev, pci->func, (uint8_t)(ptr + 12));
			mult = pci_read_config_dword(
			    pci->bus, pci->dev, pci->func, (uint8_t)(ptr + 16));
			if (mult == 0)
				goto next_cap;

			err = pci_read_bar(pci, cap.bar, &bar);
			if (err)
				goto next_cap;

			candidate.multiplier = mult;

			if (!bar.is_mmio) {
				uint64_t port = bar.base + cap.offset;
				if (port > 0xFFFFu)
					goto next_cap;
				candidate.is_port = true;
				candidate.port = (uint16_t)port;
				if (!pio_saved) {
					pio_candidate = candidate;
					pio_saved = true;
				}
				pio_err = 0;
				goto next_cap;
			}

			{
				volatile void *mmio = NULL;
				if (virtio_map_mmio_cap(pci, &cap,
							sizeof(uint16_t),
							&mmio) == 0) {
					candidate.is_port = false;
					candidate.mmio =
					    (volatile uint16_t *)mmio;
					*notify = candidate;
					return 0;
				}
			}
		}
	next_cap:
		ptr = pci_cfg8(pci, (uint8_t)(ptr + 1));
	}

	if (pio_err == 0) {
		*notify = pio_candidate;
		return 0;
	}
	return -ENODEV;
}

static uint32_t virtio_dev_feature(volatile struct virtio_pci_common_cfg *c,
				   uint32_t sel)
{
	c->device_feature_select = sel;
	return c->device_feature;
}

static void virtio_set_driver_feature(volatile struct virtio_pci_common_cfg *c,
				      uint32_t sel, uint32_t val)
{
	c->driver_feature_select = sel;
	c->driver_feature = val;
}

static size_t virtq_bytes(uint16_t qsize)
{
	size_t desc_sz = sizeof(struct virtq_desc) * qsize;
	size_t avail_sz =
	    sizeof(struct virtq_avail_hdr) + sizeof(uint16_t) * qsize;
	size_t used_off = (desc_sz + avail_sz + 3u) & ~3u;
	size_t used_sz = sizeof(struct virtq_used_hdr) +
			 sizeof(struct virtq_used_elem) * qsize;

	return used_off + used_sz;
}

static int virtq_init(struct virtio_blk_dev *d, uint16_t qsize)
{
	struct virtio_queue *vq = &d->vq;
	volatile struct virtio_pci_common_cfg *c = d->mmio.common;
	size_t total;
	paddr_t pa;
	void *va;
	size_t desc_sz;
	size_t avail_sz;
	size_t used_off;
	uint16_t dev_qsize;

	if (qsize == 0 || (qsize & (qsize - 1)) != 0)
		return -EINVAL;

	/*
	 * Clamp the requested ring size to what the device supports
	 * *before* deriving any of the ring offsets and sizes, otherwise
	 * the driver and device would disagree on where avail/used live.
	 */
	c->queue_select = 0;
	dev_qsize = c->queue_size;
	if (dev_qsize == 0)
		return -EINVAL;
	if (qsize > dev_qsize)
		qsize = dev_qsize;
	if (qsize == 0 || (qsize & (qsize - 1)) != 0)
		return -EINVAL;

	desc_sz = sizeof(struct virtq_desc) * qsize;
	avail_sz = sizeof(struct virtq_avail_hdr) + sizeof(uint16_t) * qsize;
	used_off = (desc_sz + avail_sz + 3u) & ~3u;
	total = virtq_bytes(qsize);
	if (total > PAGE_SIZE)
		return -EINVAL;

	pa = pmm_alloc_dma(0);
	if (!pa)
		return -ENOMEM;

	va = phys_to_virt(pa);
	memset(va, 0, total);

	vq->mem_pa = pa;
	vq->size = qsize;
	vq->desc = (struct virtq_desc *)va;
	vq->avail =
	    (volatile struct virtq_avail_hdr *)((uint8_t *)va + desc_sz);
	vq->used_hdr =
	    (volatile struct virtq_used_hdr *)((uint8_t *)va + used_off);
	vq->used = (struct virtq_used_elem *)((uint8_t *)vq->used_hdr +
					      sizeof(struct virtq_used_hdr));
	vq->last_used_idx = 0;

	/*
	 * This is a polling-only driver with no MSI-X vector assigned
	 * (queue_msix_vector == VIRTIO_NO_VECTOR), so ask the device not to
	 * raise used-buffer notifications. Otherwise the device would assert
	 * its legacy INTx line for every completion with no handler to clear
	 * it. Must be set before the queue is enabled.
	 */
	vq->avail->flags = VIRTQ_AVAIL_F_NO_INTERRUPT;

	c->queue_size = qsize;
	c->queue_msix_vector = VIRTIO_NO_VECTOR;
	c->queue_desc = (uint64_t)pa;
	c->queue_driver = (uint64_t)virt_to_phys((void *)vq->avail);
	c->queue_device = (uint64_t)virt_to_phys((void *)vq->used_hdr);
	__asm__ __volatile__("" ::: "memory");
	c->queue_enable = 1;
	return 0;
}

static void virtio_notify(struct virtio_blk_dev *d, uint16_t qsel)
{
	volatile struct virtio_pci_common_cfg *c = d->mmio.common;
	struct virtio_notify *n = &d->mmio.notify;
	uint32_t off = c->queue_notify_off;

	if (n->is_port) {
		outw((uint16_t)(n->port + off * n->multiplier), qsel);
		return;
	}

	*(volatile uint16_t *)((uint8_t *)n->mmio +
			       (uintptr_t)off * n->multiplier) = qsel;
}

/* Latch the device dead: outstanding DMA must be assumed in flight. */
static void virtio_mark_dead(struct virtio_blk_dev *d)
{
	d->dead = true;
	d->ready = false;
	if (d->mmio.common)
		d->mmio.common->device_status = VIRTIO_STATUS_FAILED;
}

static int virtio_blk_submit(struct virtio_blk_dev *d, uint32_t type,
			     uint64_t sector, void *bounce, size_t bytes,
			     bool data_write)
{
	struct virtio_queue *vq = &d->vq;
	struct virtq_desc *desc;
	struct virtio_req_meta *meta = d->req_meta;
	uint16_t head;
	uint16_t slot;
	uint16_t old_used;
	uint16_t *avail_ring;
	uint32_t poll = 0;
	uint16_t ndesc;
	uint64_t flags;
	int ret;

	if (type == VIRTIO_BLK_T_FLUSH) {
		if (bytes != 0)
			return -EINVAL;
	} else if (bytes == 0 || (bytes % VIRTIO_BLK_SECTOR_SIZE) != 0 ||
		   bytes > PAGE_DATA_BYTES) {
		return -EINVAL;
	}

	flags = spin_lock_irqsave(&d->lock);

	if (d->dead) {
		ret = -ENODEV;
		goto out;
	}

	desc = vq->desc;
	head = vq->avail->idx;
	slot = (uint16_t)(head % vq->size);
	old_used = vq->last_used_idx;
	avail_ring =
	    (uint16_t *)((uint8_t *)vq->avail + sizeof(struct virtq_avail_hdr));

	memset(meta, 0, sizeof(*meta));
	meta->status = 0xFF;
	meta->hdr.type = type;
	meta->hdr.sector = sector;

	desc[0].addr = (uint64_t)d->req_meta_pa;
	desc[0].len = sizeof(meta->hdr);
	desc[0].flags = VIRTQ_DESC_F_NEXT;
	desc[0].next = 1;
	ndesc = 1;

	if (type != VIRTIO_BLK_T_FLUSH) {
		desc[1].addr = (uint64_t)virt_to_phys(bounce);
		desc[1].len = (uint32_t)bytes;
		desc[1].flags =
		    (uint16_t)(VIRTQ_DESC_F_NEXT |
			       (data_write ? 0u : VIRTQ_DESC_F_WRITE));
		desc[1].next = 2;
		ndesc = 2;
	}

	desc[ndesc].addr =
	    (uint64_t)(d->req_meta_pa + sizeof(struct virtio_blk_outhdr));
	desc[ndesc].len = 1;
	desc[ndesc].flags = VIRTQ_DESC_F_WRITE;
	desc[ndesc].next = 0;

	avail_ring[slot] = 0;
	vq->avail->idx = (uint16_t)(head + 1);
	__asm__ __volatile__("" ::: "memory");

	virtio_notify(d, 0);

	while (vq->used_hdr->idx == old_used) {
		if (++poll > VIRTIO_POLL_BUDGET) {
			/*
			 * The request is still outstanding; the device may
			 * complete it (and DMA into the bounce buffer) at any
			 * point later. Latch the device dead so callers leak
			 * rather than free buffers the device still owns.
			 */
			virtio_mark_dead(d);
			ret = -EIO;
			goto out;
		}
		__asm__ __volatile__("pause");
	}

	if (vq->used[old_used % vq->size].id != 0) {
		/* Protocol desync: queue state is no longer trustworthy. */
		virtio_mark_dead(d);
		ret = -EIO;
		goto out;
	}

	vq->last_used_idx = (uint16_t)(old_used + 1);
	__asm__ __volatile__("" ::: "memory");

	ret = (meta->status != VIRTIO_BLK_S_OK) ? -EIO : 0;

out:
	spin_unlock_irqrestore(&d->lock, flags);
	return ret;
}

static int virtio_blk_request(struct virtio_blk_dev *d, uint64_t sector,
			      void *bounce, size_t bytes, bool write)
{
	return virtio_blk_submit(d, write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN,
				 sector, bounce, bytes, write);
}

static int virtio_blk_flush(struct virtio_blk_dev *d)
{
	return virtio_blk_submit(d, VIRTIO_BLK_T_FLUSH, 0, NULL, 0, false);
}

static int virtio_bdev_read(struct block_device *bdev, uint64_t lba,
			    size_t count, void *buf)
{
	struct virtio_blk_dev *d = bdev->priv;
	paddr_t bounce_pa;
	void *bounce;
	uint8_t *p = buf;
	int err;

	if (!d || !d->ready || d->dead)
		return -ENODEV;
	if (count == 0)
		return 0;
	if (count > d->capacity || lba > d->capacity - count)
		return -EINVAL;

	bounce_pa = pmm_alloc_dma(0);
	if (!bounce_pa)
		return -ENOMEM;
	bounce = phys_to_virt(bounce_pa);

	for (size_t i = 0; i < count; i++) {
		err = virtio_blk_request(d, lba + i, bounce,
					 VIRTIO_BLK_SECTOR_SIZE, false);
		if (err)
			goto out;
		memcpy(p, bounce, VIRTIO_BLK_SECTOR_SIZE);
		p += VIRTIO_BLK_SECTOR_SIZE;
	}
	err = 0;

out:
	/*
	 * If the device went dead the request is still outstanding and the
	 * device may DMA into this page later — leak it rather than return
	 * it to the allocator.
	 */
	if (!d->dead)
		pmm_free_pages(bounce_pa, 0);
	return err;
}

static int virtio_bdev_write(struct block_device *bdev, uint64_t lba,
			     size_t count, const void *buf)
{
	struct virtio_blk_dev *d = bdev->priv;
	paddr_t bounce_pa;
	void *bounce;
	const uint8_t *p = buf;
	int err;

	if (!d || !d->ready || d->dead)
		return -ENODEV;
	if (d->read_only)
		return -EROFS;
	if (count == 0)
		return 0;
	if (count > d->capacity || lba > d->capacity - count)
		return -EINVAL;

	bounce_pa = pmm_alloc_dma(0);
	if (!bounce_pa)
		return -ENOMEM;
	bounce = phys_to_virt(bounce_pa);

	for (size_t i = 0; i < count; i++) {
		memcpy(bounce, p, VIRTIO_BLK_SECTOR_SIZE);
		err = virtio_blk_request(d, lba + i, bounce,
					 VIRTIO_BLK_SECTOR_SIZE, true);
		if (err)
			goto out;
		p += VIRTIO_BLK_SECTOR_SIZE;
	}
	err = 0;

out:
	/* See virtio_bdev_read(): leak the bounce page if the device died. */
	if (!d->dead)
		pmm_free_pages(bounce_pa, 0);
	if (!err)
		err = virtio_blk_flush(d);
	return err;
}

static const struct block_ops virtio_blk_ops = {
    .read = virtio_bdev_read,
    .write = virtio_bdev_write,
};

static const struct pci_device *virtio_find_blk_pci(void)
{
	const struct pci_device *d;

	d = pci_find_vendor(VIRTIO_VENDOR_ID, VIRTIO_DEV_BLK_MODERN);
	if (d)
		return d;
	return pci_find_vendor(VIRTIO_VENDOR_ID, VIRTIO_DEV_BLK_LEGACY);
}

static void virtio_blk_teardown(struct virtio_blk_dev *d)
{
	if (d->mmio.common)
		d->mmio.common->device_status = VIRTIO_STATUS_FAILED;
	if (d->vq.mem_pa)
		pmm_free_pages(d->vq.mem_pa, 0);
	if (d->req_meta_pa)
		pmm_free_pages(d->req_meta_pa, 0);
	memset(d, 0, sizeof(*d));
}

static int virtio_negotiate_features(volatile struct virtio_pci_common_cfg *c,
				     bool *read_only_out)
{
	uint32_t dev_w0;
	uint32_t dev_w1;
	uint32_t drv_w1 = 0;

	dev_w0 = virtio_dev_feature(c, 0);
	dev_w1 = virtio_dev_feature(c, 1);
	if (dev_w1 & VIRTIO_F_VERSION_1)
		drv_w1 |= VIRTIO_F_VERSION_1;

	virtio_set_driver_feature(c, 0, 0);
	virtio_set_driver_feature(c, 1, drv_w1);
	for (uint32_t sel = 2; sel < 4; sel++)
		virtio_set_driver_feature(c, sel, 0);

	c->device_status =
	    (uint8_t)(VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
		      VIRTIO_STATUS_FEATURES_OK);
	__asm__ __volatile__("" ::: "memory");
	if (!(c->device_status & VIRTIO_STATUS_FEATURES_OK))
		return -EINVAL;

	*read_only_out = (dev_w0 & VIRTIO_BLK_F_RO) != 0;
	return 0;
}

static int virtio_blk_setup(const struct pci_device *pci)
{
	struct virtio_blk_dev *d = &blk_dev;
	struct virtio_pci_cap common_cap, dev_cap;
	struct virtio_mmio *m = &d->mmio;
	volatile struct virtio_pci_common_cfg *c;
	volatile struct virtio_blk_config *bcfg;
	int err;

	memset(d, 0, sizeof(*d));
	spin_lock_init(&d->lock);

	err = virtio_find_cap(pci, VIRTIO_PCI_CAP_COMMON_CFG, &common_cap);
	if (err)
		return err;
	err = virtio_find_cap(pci, VIRTIO_PCI_CAP_DEVICE_CFG, &dev_cap);
	if (err)
		return err;

	pci_enable_device(pci);

	err = virtio_map_mmio_cap(pci, &common_cap,
				  sizeof(struct virtio_pci_common_cfg),
				  (volatile void **)&m->common);
	if (err)
		return err;
	err =
	    virtio_map_mmio_cap(pci, &dev_cap, sizeof(struct virtio_blk_config),
				(volatile void **)&m->device_cfg);
	if (err)
		return err;
	err = virtio_map_notify(pci, &m->notify);
	if (err)
		return err;

	c = m->common;

	/*
	 * Reset the device, then wait (bounded) for the reset to complete:
	 * the spec requires device_status to read back 0 before the driver
	 * reinitializes it. On real hardware the reset is not instantaneous.
	 */
	c->device_status = 0;
	{
		uint32_t spin = 0;
		while (c->device_status != 0) {
			if (++spin > VIRTIO_POLL_BUDGET) {
				err = -EIO;
				goto fail;
			}
			__asm__ __volatile__("pause");
		}
	}

	c->device_status = VIRTIO_STATUS_ACKNOWLEDGE;
	c->device_status =
	    (uint8_t)(VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

	err = virtio_negotiate_features(c, &d->read_only);
	if (err)
		goto fail;

	d->req_meta_pa = pmm_alloc_dma(0);
	if (!d->req_meta_pa) {
		err = -ENOMEM;
		goto fail;
	}
	d->req_meta = (struct virtio_req_meta *)phys_to_virt(d->req_meta_pa);

	err = virtq_init(d, VIRTQ_SIZE);
	if (err)
		goto fail;

	bcfg = (volatile struct virtio_blk_config *)m->device_cfg;
	d->capacity = bcfg->capacity;
	if (d->capacity == 0) {
		err = -EINVAL;
		goto fail;
	}

	c->device_status =
	    (uint8_t)(VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
		      VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

	d->bdev.name = "vda";
	d->bdev.sector_size = VIRTIO_BLK_SECTOR_SIZE;
	d->bdev.sector_count = d->capacity;
	d->bdev.ops = &virtio_blk_ops;
	d->bdev.priv = d;
	d->ready = true;

	block_register(&d->bdev);
	pr_info("virtio-blk: vda: %llu sectors (%llu MiB)%s\n",
		(unsigned long long)d->capacity,
		(unsigned long long)(d->capacity / 2048),
		d->read_only ? " [ro]" : "");
	return 0;

fail:
	virtio_blk_teardown(d);
	return err;
}

void virtio_blk_init(void)
{
	const struct pci_device *pci = virtio_find_blk_pci();
	int err;

	if (!pci) {
		pr_info("virtio-blk: no device found\n");
		return;
	}

	err = virtio_blk_setup(pci);
	if (err) {
		pr_warn("virtio-blk: setup failed (%d)\n", err);
		return;
	}
	virtio_present = true;
}

int virtio_blk_selftest(void)
{
	struct block_device *bdev;
	struct virtio_blk_dev *d = &blk_dev;
	uint64_t test_lba;
	uint8_t orig[VIRTIO_BLK_SECTOR_SIZE];
	uint8_t check[VIRTIO_BLK_SECTOR_SIZE];
	static const uint8_t pattern[VIRTIO_BLK_SECTOR_SIZE] = {
	    "JNU virtio test"};
	int ret;

	if (!virtio_present) {
		pr_info("virtio_blk_selftest: skipped (no device)\n");
		return 0;
	}

	bdev = block_lookup("vda");
	if (!bdev)
		return -ENODEV;
	if (bdev->sector_count == 0)
		return -EINVAL;

	test_lba = bdev->sector_count - 1;

	/*
	 * Read path is always exercised non-destructively: read the same
	 * sector twice and require identical results. This proves DMA and
	 * the virtqueue work without modifying any disk contents.
	 */
	if (block_read(bdev, test_lba, 1, orig) != 0) {
		pr_err("virtio_blk_selftest: read sector %llu failed\n",
		       (unsigned long long)test_lba);
		return -EIO;
	}
	if (block_read(bdev, test_lba, 1, check) != 0) {
		pr_err("virtio_blk_selftest: re-read sector %llu failed\n",
		       (unsigned long long)test_lba);
		return -EIO;
	}
	if (memcmp(orig, check, VIRTIO_BLK_SECTOR_SIZE) != 0) {
		pr_err("virtio_blk_selftest: read path unstable\n");
		return -EIO;
	}

	/*
	 * Write path: only on a writable device. The last sector's contents
	 * are saved first, a pattern is written and verified, and then the
	 * original is unconditionally restored and the restore re-verified —
	 * so even a mid-test failure cannot leave the sector corrupted.
	 */
	if (d->read_only) {
		pr_info("virtio_blk_selftest: read OK (sector %llu); "
			"write skipped [ro]\n",
			(unsigned long long)test_lba);
		return 0;
	}

	if (block_write(bdev, test_lba, 1, pattern) != 0) {
		pr_err("virtio_blk_selftest: write sector %llu failed\n",
		       (unsigned long long)test_lba);
		/* Best effort: put back the original contents. */
		(void)block_write(bdev, test_lba, 1, orig);
		return -EIO;
	}

	ret = 0;
	if (block_read(bdev, test_lba, 1, check) != 0) {
		pr_err("virtio_blk_selftest: read-back sector %llu failed\n",
		       (unsigned long long)test_lba);
		ret = -EIO;
	} else if (memcmp(check, pattern, VIRTIO_BLK_SECTOR_SIZE) != 0) {
		pr_err("virtio_blk_selftest: read-back mismatch\n");
		ret = -EIO;
	}

	/* Always restore the original, then verify it really landed. */
	if (block_write(bdev, test_lba, 1, orig) != 0) {
		pr_err("virtio_blk_selftest: FAILED to restore sector %llu\n",
		       (unsigned long long)test_lba);
		return -EIO;
	}
	if (block_read(bdev, test_lba, 1, check) != 0 ||
	    memcmp(check, orig, VIRTIO_BLK_SECTOR_SIZE) != 0) {
		pr_err("virtio_blk_selftest: restore verification failed\n");
		return -EIO;
	}

	if (ret)
		return ret;

	pr_info("virtio_blk_selftest: read/write sector %llu OK\n",
		(unsigned long long)test_lba);
	return 0;
}
