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
#include <jnu/paging.h>
#include <jnu/pmm.h>
#include <jnu/string.h>
#include <jnu/usercopy.h>
#include <jnu/vmm.h>

#define EI_NIDENT	16
#define EI_CLASS	4
#define EI_DATA		5
#define ELFCLASS64	2
#define ELFDATA2LSB	1
#define ET_EXEC		2
#define EM_X86_64	62
#define PT_LOAD		1
#define PF_X		1
#define PF_W		2

#define USER_STACK_TOP		0x0000000080000000ull
#define USER_STACK_SIZE		(64 * 1024)

struct elf64_ehdr {
	uint8_t		e_ident[EI_NIDENT];
	uint16_t	e_type;
	uint16_t	e_machine;
	uint32_t	e_version;
	uint64_t	e_entry;
	uint64_t	e_phoff;
	uint64_t	e_shoff;
	uint32_t	e_flags;
	uint16_t	e_ehsize;
	uint16_t	e_phentsize;
	uint16_t	e_phnum;
	uint16_t	e_shentsize;
	uint16_t	e_shnum;
	uint16_t	e_shstrndx;
};

struct elf64_phdr {
	uint32_t	p_type;
	uint32_t	p_flags;
	uint64_t	p_offset;
	uint64_t	p_vaddr;
	uint64_t	p_paddr;
	uint64_t	p_filesz;
	uint64_t	p_memsz;
	uint64_t	p_align;
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
	int err;

	err = image_read_exact(image, 0, &eh, sizeof(eh));
	if (err) {
		return err;
	}

	if (memcmp(eh.e_ident, "\x7f" "ELF", 4) != 0 ||
	    eh.e_ident[EI_CLASS] != ELFCLASS64 ||
	    eh.e_ident[EI_DATA] != ELFDATA2LSB ||
	    eh.e_type != ET_EXEC ||
	    eh.e_machine != EM_X86_64 ||
	    eh.e_phentsize != sizeof(struct elf64_phdr) ||
	    eh.e_phnum == 0) {
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
		if (ph.p_filesz > ph.p_memsz ||
		    ph.p_offset > image->size ||
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
		loads++;
	}

	if (loads == 0 || eh.e_entry < low || eh.e_entry >= high) {
		return -ENOEXEC;
	}

	if (info) {
		info->entry = eh.e_entry;
		info->low = low;
		info->high = high;
	}
	return 0;
}

static uint64_t page_down(uint64_t v)
{
	return v & ~PAGE_MASK;
}

static uint64_t page_up(uint64_t v)
{
	return (v + PAGE_MASK) & ~PAGE_MASK;
}

static uint32_t phdr_vma_flags(const struct elf64_phdr *ph)
{
	uint32_t flags = VMA_READ | VMA_USER;

	if (ph->p_flags & PF_W) {
		flags |= VMA_WRITE;
	}
	if (ph->p_flags & PF_X) {
		flags |= VMA_EXEC;
	}
	return flags;
}

static int map_zeroed_user_pages(struct addr_space *space,
				 uint64_t start, uint64_t end,
				 uint32_t flags)
{
	for (uint64_t va = start; va < end; va += PAGE_SIZE) {
		paddr_t pa = pmm_alloc_user_page();
		int err;

		if (!pa) {
			return -ENOMEM;
		}

		err = vmm_map(space, va, pa, 1, flags);
		if (err) {
			pmm_free_pages(pa, 0);
			return err;
		}
	}
	return 0;
}

int elf64_load_image(struct addr_space *space, const struct exec_image *image,
		     struct exec_load_info *info)
{
	struct elf64_ehdr eh;
	struct elf64_phdr ph;
	int err;

	if (!space) {
		return -EINVAL;
	}

	err = elf64_validate_image(image, info);
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

		err = image_read_exact(image, off, &ph, sizeof(ph));
		if (err) {
			return err;
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
			return err;
		}

		uint64_t remaining = ph.p_filesz;
		uint64_t file_off = ph.p_offset;
		uint64_t curr_va = ph.p_vaddr;

		while (remaining > 0) {
			uint8_t buf[PAGE_SIZE];
			size_t chunk = remaining > PAGE_SIZE ? PAGE_SIZE : remaining;

			err = image_read_exact(image, file_off, buf, chunk);
			if (err) {
				return err;
			}

			err = copy_to_user((void *)curr_va, buf, chunk);
			if (err) {
				return err;
			}

			file_off += chunk;
			curr_va += chunk;
			remaining -= chunk;
		}

		err = vmm_protect(space, start,
				  (end - start) / PAGE_SIZE, final_flags);
		if (err) {
			return err;
		}
	}

	return 0;
}

int elf64_setup_initial_stack(struct addr_space *space, uint64_t *stack_out)
{
	uint64_t guard = USER_STACK_TOP - USER_STACK_SIZE - PAGE_SIZE;
	uint64_t base = guard + PAGE_SIZE;
	uint64_t pages = USER_STACK_SIZE / PAGE_SIZE;
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
	err = copy_to_user((void *)stack, zero, sizeof(zero));
	if (err) {
		return err;
	}
	(void)pages;

	*stack_out = stack;
	return 0;
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
	memcpy(eh->e_ident, "\x7f" "ELF", 4);
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
