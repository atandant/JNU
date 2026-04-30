/*
 * kernel/exec/elf64.c - ELF64 static executable validation.
 *
 * Phase 3 starts with format validation over the shared exec_image
 * reader. Mapping into a fresh user address space lands in the next
 * slice, using this same parser path.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/elf64.h>
#include <jnu/errno.h>
#include <jnu/exec.h>
#include <jnu/klog.h>
#include <jnu/kmalloc.h>
#include <jnu/paging.h>
#include <jnu/pmm.h>
#include <jnu/string.h>
#include <jnu/usercopy.h>
#include <jnu/vma.h>
#include <jnu/vmm.h>

#define EI_NIDENT 16
#define EI_CLASS 4
#define EI_DATA 5
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_EXEC 2
#define EM_X86_64 62
#define PT_LOAD 1
#define PF_X 1
#define PF_W 2

#define USER_STACK_TOP 0x0000000080000000ull
#define USER_STACK_SIZE (64 * 1024)

struct elf64_ehdr {
  uint8_t e_ident[EI_NIDENT];
  uint16_t e_type;
  uint16_t e_machine;
  uint32_t e_version;
  uint64_t e_entry;
  uint64_t e_phoff;
  uint64_t e_shoff;
  uint32_t e_flags;
  uint16_t e_ehsize;
  uint16_t e_phentsize;
  uint16_t e_phnum;
  uint16_t e_shentsize;
  uint16_t e_shnum;
  uint16_t e_shstrndx;
};

struct elf64_phdr {
  uint32_t p_type;
  uint32_t p_flags;
  uint64_t p_offset;
  uint64_t p_vaddr;
  uint64_t p_paddr;
  uint64_t p_filesz;
  uint64_t p_memsz;
  uint64_t p_align;
};

static int image_read_exact(const struct exec_image *image, uint64_t off,
                            void *buf, size_t len) {
  ssize_t n;

  if (!image || !image->read_at || !buf) {
    return -EINVAL;
  }
  if (off > image->size || len > image->size - off) {
    return -ENOEXEC;
  }

  n = image->read_at(image->ctx, off, buf, len);
  if (n < 0) {
    return (int)n;
  }
  return n == (ssize_t)len ? 0 : -ENOEXEC;
}

static bool user_range(uint64_t start, uint64_t len, uint64_t *end) {
  uint64_t e = start + len;

  if (len == 0 || e < start) {
    return false;
  }
  if (start < 0x1000 || start >= USER_TOP || e > USER_TOP) {
    return false;
  }

  *end = e;
  return true;
}

int elf64_validate_image(const struct exec_image *image,
                         struct exec_load_info *info) {
  struct elf64_ehdr eh;
  struct elf64_phdr ph;
  uint64_t low = USER_TOP;
  uint64_t high = 0;
  int loads = 0;
  bool entry_in_exec = false;
  int err;

  err = image_read_exact(image, 0, &eh, sizeof(eh));
  if (err) {
    return err;
  }

  if (memcmp(eh.e_ident,
             "\x7f"
             "ELF",
             4) != 0 ||
      eh.e_ident[EI_CLASS] != ELFCLASS64 ||
      eh.e_ident[EI_DATA] != ELFDATA2LSB || eh.e_type != ET_EXEC ||
      eh.e_machine != EM_X86_64 ||
      eh.e_phentsize != sizeof(struct elf64_phdr) || eh.e_phnum == 0) {
    return -ENOEXEC;
  }
  if (eh.e_phoff > image->size ||
      (uint64_t)eh.e_phnum > (image->size - eh.e_phoff) / sizeof(ph)) {
    return -ENOEXEC;
  }

  for (uint16_t i = 0; i < eh.e_phnum; i++) {
    uint64_t off = eh.e_phoff + (uint64_t)i * sizeof(ph);
    uint64_t end;

    err = image_read_exact(image, off, &ph, sizeof(ph));
    if (err) {
      return err;
    }
    if (ph.p_type != PT_LOAD) {
      continue;
    }
    if (ph.p_filesz > ph.p_memsz || ph.p_offset > image->size ||
        ph.p_filesz > image->size - ph.p_offset ||
        (ph.p_flags & (PF_X | PF_W)) == (PF_X | PF_W) ||
        !user_range(ph.p_vaddr, ph.p_memsz, &end)) {
      return -ENOEXEC;
    }
    if (ph.p_vaddr < low) {
      low = ph.p_vaddr;
    }
    if (end > high) {
      high = end;
    }
    /*
     * Entry must land in an executable segment. Without this
     * check, a crafted ELF could set e_entry inside a
     * writable, non-executable segment and still be accepted
     * because the entry only had to fall within [low, high).
     * That would defeat W^X: the first instruction the CPU
     * fetches would come from a page we mapped non-X.
     * (On x86_64 with NX honored that would just trap, but
     * the validator should still reject the image up front.)
     */
    if ((ph.p_flags & PF_X) && eh.e_entry >= ph.p_vaddr && eh.e_entry < end) {
      entry_in_exec = true;
    }
    loads++;
  }

  if (loads == 0 || !entry_in_exec) {
    return -ENOEXEC;
  }

  if (info) {
    info->entry = eh.e_entry;
    info->low = low;
    info->high = high;
  }
  return 0;
}

static uint64_t page_down(uint64_t v) { return v & ~PAGE_MASK; }

static uint64_t page_up(uint64_t v) { return (v + PAGE_MASK) & ~PAGE_MASK; }

/*
 * Write `len` bytes from kernel `src` to virtual address `va` in the
 * given address space, bypassing copy_to_user.  We look up the PTE
 * to find the physical page, then memcpy through the HHDM.  This is
 * needed because copy_to_user validates against the *current*
 * process's address space, which is the parent during process_spawn.
 */
static int write_to_space(struct addr_space *space, uint64_t va,
                          const void *src, size_t len) {
  const uint8_t *p = src;

  while (len > 0) {
    uint64_t pte;
    paddr_t pa;
    size_t page_off = va & PAGE_MASK;
    size_t chunk = PAGE_SIZE - page_off;
    int err;

    if (chunk > len) {
      chunk = len;
    }

    err = paging_get_flags(space, va, &pte);
    if (err) {
      return err;
    }
    pa = (pte & PTE_ADDR_MASK) + page_off;
    memcpy(phys_to_virt(pa), p, chunk);

    va += chunk;
    p += chunk;
    len -= chunk;
  }
  return 0;
}

static uint32_t phdr_vma_flags(const struct elf64_phdr *ph) {
  uint32_t flags = VMA_READ | VMA_USER;

  if (ph->p_flags & PF_W) {
    flags |= VMA_WRITE;
  }
  if (ph->p_flags & PF_X) {
    flags |= VMA_EXEC;
  }
  return flags;
}

static int map_zeroed_user_pages(struct addr_space *space, uint64_t start,
                                 uint64_t end, uint32_t flags) {
  uint64_t va;
  struct vma *v;
  int err;

  /*
   * Allocate and insert a VMA descriptor before touching the page
   * tables. This gives the address space an authoritative record of
   * every mapped range so that:
   *   (a) the #PF handler can distinguish a valid unmapped-page fault
   *       from a wild-pointer access, and
   *   (b) vmm_destroy_space() can eventually walk VMAs to free frames.
   */
  v = kzalloc(sizeof(*v));
  if (!v) {
    return -ENOMEM;
  }
  v->start = start;
  v->end = end;
  v->flags = flags;
  err = vma_insert(&space->vmas, v);
  if (err) {
    /* Overlapping VMA — caller has a logic error.
        author here: we could replace this in the future
        with a goto, need to debate on this. FIXME: */
    kfree(v);
    return err;
  }

  for (va = start; va < end; va += PAGE_SIZE) {
    paddr_t pa = pmm_alloc_user_page();

    if (!pa) {
      goto fail_partial;
    }

    err = vmm_map(space, va, pa, 1, flags);
    if (err) {
      pmm_free_pages(pa, 0);
      goto fail_partial;
    }
  }
  return 0;

fail_partial:
  /*
   * Undo every page we successfully mapped in this call. Without
   * this, a partial map would leak both the physical pages and
   * the residual user-visible mappings — and the higher-level
   * unmap-on-error path in elf64_load_image() would silently
   * skip these pages because it tracks ranges per segment, not
   * per attempted mapping.
   *
   * Remove the VMA we inserted above so the tree stays consistent
   * with the actual page-table state.
   */
  if (va > start) {
    vmm_unmap(space, start, (va - start) / PAGE_SIZE);
  }
  vma_remove(&space->vmas, v);
  kfree(v);
  return -ENOMEM;
}

int elf64_load_image(struct addr_space *space, const struct exec_image *image,
                     struct exec_load_info *info) {
  struct elf64_ehdr eh;
  struct elf64_phdr ph;
  struct exec_load_info local_info;
  uint64_t mapped_low = USER_TOP;
  uint64_t mapped_high = 0;
  int err;

  if (!space) {
    return -EINVAL;
  }

  /*
   * Always validate into a local info first so we have low/high
   * available for the cleanup path even if the caller passed
   * NULL for `info`.
   */
  err = elf64_validate_image(image, &local_info);
  if (err) {
    return err;
  }

  err = image_read_exact(image, 0, &eh, sizeof(eh));
  if (err) {
    return err;
  }

  for (uint16_t i = 0; i < eh.e_phnum; i++) {
    uint64_t off = eh.e_phoff + (uint64_t)i * sizeof(ph);
    uint64_t start;
    uint64_t end;
    uint32_t final_flags;
    uint64_t remaining;
    uint64_t file_off;
    uint64_t curr_va;

    err = image_read_exact(image, off, &ph, sizeof(ph));
    if (err) {
      goto fail_unmap;
    }
    if (ph.p_type != PT_LOAD) {
      continue;
    }

    start = page_down(ph.p_vaddr);
    end = page_up(ph.p_vaddr + ph.p_memsz);
    final_flags = phdr_vma_flags(&ph);

    err = map_zeroed_user_pages(space, start, end,
                                VMA_READ | VMA_WRITE | VMA_USER);
    if (err) {
      goto fail_unmap;
    }

    /*
     * Track the union of every successfully mapped range so
     * the failure path can unmap all of them in one shot.
     * Per spec §3.1 the ELF loader must not leak pages on
     * partial failure.
     */
    if (start < mapped_low) {
      mapped_low = start;
    }
    if (end > mapped_high) {
      mapped_high = end;
    }

    remaining = ph.p_filesz;
    file_off = ph.p_offset;
    curr_va = ph.p_vaddr;

    while (remaining > 0) {
      uint8_t buf[PAGE_SIZE];
      size_t chunk = remaining > PAGE_SIZE ? PAGE_SIZE : remaining;

      err = image_read_exact(image, file_off, buf, chunk);
      if (err) {
        goto fail_unmap;
      }

      err = write_to_space(space, curr_va, buf, chunk);
      if (err) {
        goto fail_unmap;
      }

      file_off += chunk;
      curr_va += chunk;
      remaining -= chunk;
    }

    err = vmm_protect(space, start, (end - start) / PAGE_SIZE, final_flags);
    if (err) {
      goto fail_unmap;
    }

    /*
     * map_zeroed_user_pages() recorded the VMA with the temporary
     * writable flags we used during segment copying. Now that the
     * page tables carry the final protection, patch the VMA to
     * match so that future VMA consumers (the #PF handler, any
     * audit code) see the correct permissions.
     */
    {
      struct vma *seg_vma = vma_find(&space->vmas, start);
      if (seg_vma) {
        seg_vma->flags = final_flags;
      }
    }
  }

  if (info) {
    *info = local_info;
  }
  return 0;

fail_unmap:
  if (mapped_high > mapped_low) {
    vmm_unmap(space, mapped_low, (mapped_high - mapped_low) / PAGE_SIZE);
  }
  return err;
}

int elf64_setup_initial_stack(struct addr_space *space, uint64_t *stack_out) {
  uint64_t guard = USER_STACK_TOP - USER_STACK_SIZE - PAGE_SIZE;
  uint64_t base = guard + PAGE_SIZE;
  uint64_t stack = USER_STACK_TOP - 16;
  int err;

  if (!space || !stack_out) {
    return -EINVAL;
  }

  err = map_zeroed_user_pages(space, base, USER_STACK_TOP,
                              VMA_READ | VMA_WRITE | VMA_USER);
  if (err) {
    return err;
  }

  uint64_t zero[2] = {0, 0};
  err = write_to_space(space, stack, zero, sizeof(zero));
  if (err) {
    vmm_unmap(space, base, (USER_STACK_TOP - base) / PAGE_SIZE);
    return err;
  }

  *stack_out = stack;
  return 0;
}

static ssize_t test_read(void *ctx, uint64_t off, void *buf, size_t len) {
  const uint8_t *data = ctx;

  memcpy(buf, data + off, len);
  return (ssize_t)len;
}

int elf64_selftest(void) {
  uint8_t image[sizeof(struct elf64_ehdr) + sizeof(struct elf64_phdr)];
  struct elf64_ehdr *eh = (struct elf64_ehdr *)image;
  struct elf64_phdr *ph = (struct elf64_phdr *)(image + sizeof(*eh));
  struct exec_image exec = {
      .read_at = test_read,
      .size = sizeof(image),
      .ctx = image,
  };

  memset(image, 0, sizeof(image));
  memcpy(eh->e_ident,
         "\x7f"
         "ELF",
         4);
  eh->e_ident[EI_CLASS] = ELFCLASS64;
  eh->e_ident[EI_DATA] = ELFDATA2LSB;
  eh->e_type = ET_EXEC;
  eh->e_machine = EM_X86_64;
  eh->e_entry = 0x400000;
  eh->e_phoff = sizeof(*eh);
  eh->e_phentsize = sizeof(*ph);
  eh->e_phnum = 1;
  ph->p_type = PT_LOAD;
  ph->p_flags = PF_X;
  ph->p_offset = 0;
  ph->p_vaddr = 0x400000;
  ph->p_filesz = sizeof(image);
  ph->p_memsz = sizeof(image);

  return elf64_validate_image(&exec, NULL);
}
