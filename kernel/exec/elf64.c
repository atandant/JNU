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

#include <jnu/kernel/elf64.h>
#include <jnu/kernel/exec.h>
#include <jnu/lib/klog.h>
#include <jnu/lib/prng.h>
#include <jnu/lib/string.h>
#include <jnu/mm/kmalloc.h>
#include <jnu/mm/paging.h>
#include <jnu/mm/pmm.h>
#include <jnu/mm/vma.h>
#include <jnu/mm/vmm.h>
#include <jnu/user/usercopy.h>
#include <uapi/jnu/errno.h>
#include <uapi/jnu/mman.h>

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
#define USER_STACK_SIZE (128 * 1024)

/*
 * ASLR window for stack randomization.  The stack top is shifted
 * down by a random page-aligned offset in [0, STACK_ASLR_PAGES).
 * 2048 pages = 8 MiB of entropy — enough to make stack address
 * guessing infeasible without /proc/self/maps.
 *
 * Executable ASLR requires PIE (ET_DYN) support and is deferred.
 */
#define STACK_ASLR_PAGES 2048

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
			    void *buf, size_t len)
{
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

static bool user_range(uint64_t start, uint64_t len, uint64_t *end)
{
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
			 struct exec_load_info *info)
{
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
		if ((ph.p_flags & PF_X) && eh.e_entry >= ph.p_vaddr &&
		    eh.e_entry < end) {
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
 * process's address space, which may not be the target space while
 * building a fresh exec image.
 */
static int write_to_space(struct addr_space *space, uint64_t va,
			  const void *src, size_t len)
{
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

static uint32_t phdr_to_prot(const struct elf64_phdr *ph)
{
	uint32_t prot = PROT_READ;

	if (ph->p_flags & PF_W) {
		prot |= PROT_WRITE;
	}
	if (ph->p_flags & PF_X) {
		prot |= PROT_EXEC;
	}
	return prot;
}

/*
 * Eagerly materialise zeroed pages into a VMA that was created by
 * vmm_map_anonymous (which installs no PTEs).  This is needed during
 * execve because we must copy file content into the pages before
 * the process runs.
 */
static int materialise_pages(struct addr_space *space, uint64_t start,
			     uint64_t end, uint32_t vma_flags)
{
	for (uint64_t va = start; va < end; va += PAGE_SIZE) {
		paddr_t pa = pmm_alloc_user_page();
		int err;

		if (!pa) {
			return -ENOMEM;
		}

		err =
		    vmm_map(space, va, pa, 1, VMA_READ | VMA_WRITE | VMA_USER);
		if (err) {
			/*
			 * pa was returned by pmm_alloc_user_page() and
			 * therefore has refcount=1. Releasing it via the
			 * raw pmm_free_pages() path would put a still-
			 * referenced page back on the buddy free list,
			 * later panicking pmm_get_user_page() with
			 * "page is free / refcount is 0". Drop the
			 * reference instead so the refcount falls to 0
			 * and the page is freed cleanly.
			 */
			pmm_put_user_page(pa);
			return err;
		}
	}

	(void)vma_flags;
	return 0;
}

int elf64_load_image(struct addr_space *space, const struct exec_image *image,
		     struct exec_load_info *info)
{
	struct elf64_ehdr eh;
	struct elf64_phdr ph;
	struct exec_load_info local_info;
	int err;

	if (!space) {
		return -EINVAL;
	}

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
		uint32_t seg_prot;
		uint64_t remaining;
		uint64_t file_off;
		uint64_t curr_va;

		err = image_read_exact(image, off, &ph, sizeof(ph));
		if (err) {
			goto fail_space;
		}
		if (ph.p_type != PT_LOAD) {
			continue;
		}

		start = page_down(ph.p_vaddr);
		end = page_up(ph.p_vaddr + ph.p_memsz);
		seg_prot = phdr_to_prot(&ph);

		/*
		 * v0.0.3 §2.8: create segment VMA via vmm_map_anonymous
		 * with MAP_FIXED.  We use PROT_READ|PROT_WRITE for the
		 * initial mapping so we can copy segment content, then
		 * tighten to the final protection afterwards.
		 */
		err = vmm_map_anonymous(
		    space, start, end - start, PROT_READ | PROT_WRITE,
		    MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, NULL);
		if (err) {
			goto fail_space;
		}

		/* Eagerly materialise pages so we can copy file content. */
		err = materialise_pages(space, start, end,
					VMA_READ | VMA_WRITE | VMA_USER);
		if (err) {
			goto fail_space;
		}

		remaining = ph.p_filesz;
		file_off = ph.p_offset;
		curr_va = ph.p_vaddr;

		while (remaining > 0) {
			uint8_t buf[PAGE_SIZE];
			size_t chunk =
			    remaining > PAGE_SIZE ? PAGE_SIZE : remaining;

			err = image_read_exact(image, file_off, buf, chunk);
			if (err) {
				goto fail_space;
			}

			err = write_to_space(space, curr_va, buf, chunk);
			if (err) {
				goto fail_space;
			}

			file_off += chunk;
			curr_va += chunk;
			remaining -= chunk;
		}

		/*
		 * Tighten page-table protection to the segment's real
		 * permissions and update the VMA flags to match.
		 */
		{
			uint32_t final_flags = VMA_READ | VMA_USER;
			if (seg_prot & PROT_WRITE) {
				final_flags |= VMA_WRITE;
			}
			if (seg_prot & PROT_EXEC) {
				final_flags |= VMA_EXEC;
			}
			err =
			    vmm_protect(space, start, (end - start) / PAGE_SIZE,
					final_flags);
			if (err) {
				goto fail_space;
			}

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

fail_space:
	/*
	 * v0.0.3 §3: on any segment-load failure the caller tears
	 * the entire new address space down via vmm_destroy_space(),
	 * so we do not attempt per-segment cleanup here.
	 */
	return err;
}

int elf64_setup_initial_stack(struct addr_space *space,
			      const struct exec_strings *strings,
			      uint64_t *stack_out)
{
	uint64_t aslr_offset = prng_page_offset(STACK_ASLR_PAGES);
	uint64_t stack_top = USER_STACK_TOP - aslr_offset;
	uint64_t guard = stack_top - USER_STACK_SIZE - PAGE_SIZE;
	uint64_t base = guard + PAGE_SIZE;
	uint64_t stack = stack_top;
	size_t argc = strings ? strings->argc : 0;
	size_t envc = strings ? strings->envc : 0;
	uint64_t *argv_user = NULL;
	uint64_t *envp_user = NULL;
	size_t words = 1 + argc + 1 + envc + 1 + 2;
	size_t frame_size = words * sizeof(uint64_t);
	int err;

	if (!space || !stack_out) {
		return -EINVAL;
	}

	/*
	 * v0.0.3 §2.8: stack guard page.  PROT_NONE VMA below the
	 * stack so that stack overflow traps cleanly instead of
	 * silently corrupting an adjacent VMA.
	 */
	err = vmm_map_anonymous(space, guard, PAGE_SIZE, PROT_NONE,
				MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, NULL);
	if (err) {
		return err;
	}

	/*
	 * v0.0.3 §2.8: stack VMA via vmm_map_anonymous.
	 */
	err = vmm_map_anonymous(space, base, USER_STACK_SIZE,
				PROT_READ | PROT_WRITE,
				MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, NULL);
	if (err) {
		return err;
	}

	/* Eagerly materialise stack pages. */
	err = materialise_pages(space, base, stack_top,
				VMA_READ | VMA_WRITE | VMA_USER);
	if (err) {
		return err;
	}

	if (argc > 0) {
		argv_user = kmalloc(argc * sizeof(*argv_user));
		if (!argv_user) {
			err = -ENOMEM;
			goto fail;
		}
	}
	if (envc > 0) {
		envp_user = kmalloc(envc * sizeof(*envp_user));
		if (!envp_user) {
			err = -ENOMEM;
			goto fail;
		}
	}

	for (size_t i = envc; i > 0; i--) {
		const char *s = strings->envp[i - 1];
		size_t len = strlen(s) + 1;

		stack -= len;
		if (stack < base) {
			err = -E2BIG;
			goto fail;
		}
		err = write_to_space(space, stack, s, len);
		if (err) {
			goto fail;
		}
		envp_user[i - 1] = stack;
	}

	for (size_t i = argc; i > 0; i--) {
		const char *s = strings->argv[i - 1];
		size_t len = strlen(s) + 1;

		stack -= len;
		if (stack < base) {
			err = -E2BIG;
			goto fail;
		}
		err = write_to_space(space, stack, s, len);
		if (err) {
			goto fail;
		}
		argv_user[i - 1] = stack;
	}

	stack &= ~0xFull;
	stack = (stack - frame_size) & ~0xFull;
	if (stack < base) {
		err = -E2BIG;
		goto fail;
	}

	uint64_t frame_index = 0;
	uint64_t *frame = kmalloc(frame_size);
	if (!frame) {
		err = -ENOMEM;
		goto fail;
	}
	memset(frame, 0, frame_size);
	frame[frame_index++] = argc;
	for (size_t i = 0; i < argc; i++) {
		frame[frame_index++] = argv_user[i];
	}
	frame_index++;
	for (size_t i = 0; i < envc; i++) {
		frame[frame_index++] = envp_user[i];
	}

	err = write_to_space(space, stack, frame, frame_size);
	kfree(frame);
	if (err) {
		goto fail;
	}

	kfree(envp_user);
	kfree(argv_user);
	*stack_out = stack;
	return 0;

fail:
	kfree(envp_user);
	kfree(argv_user);
	return err;
}

static ssize_t test_read(void *ctx, uint64_t off, void *buf, size_t len)
{
	const uint8_t *data = ctx;

	memcpy(buf, data + off, len);
	return (ssize_t)len;
}

int elf64_selftest(void)
{
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
