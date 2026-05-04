/*
 * user/fuzz/main.c - Userspace attack program for JNU.
 *
 * This is NOT a malicious payload — it is a self-test we run inside
 * QEMU to expose kernel bugs that only show up at runtime: bad
 * usercopy ranges, broken syscall validation, missed-fault paths,
 * fd-table abuse, lseek arithmetic mistakes, and so on. Real OS
 * projects (Linux/syzkaller, FreeBSD/syzkaller, OpenBSD/regress) do
 * exactly this.
 *
 * The contract: every call should either
 *   - return a negative errno, or
 *   - return a sane positive value the test predicted,
 * but the kernel must NOT panic, hang, or deliver kernel data to us.
 *
 * Each attack prints "name -> rc" so a panic at attack N tells us
 * which case to read in the kernel.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu_syscall.h>

/* errno values the kernel returns negated. Mirror jnu/errno.h. */
#define E_PERM 1
#define E_NOENT 2
#define E_IO 5
#define E_BADF 9
#define E_NOMEM 12
#define E_FAULT 14
#define E_BUSY 16
#define E_EXIST 17
#define E_NODEV 19
#define E_NOTDIR 20
#define E_ISDIR 21
#define E_INVAL 22
#define E_MFILE 24
#define E_NOSYS 38
#define E_NAMETOOLONG 36
#define E_NOEXEC 8
#define E_CHILD 10

/* Half-canonical kernel address. Any deref by the kernel should fail. */
#define KERNEL_ADDR_HHDM 0xffff800000000000ull
#define KERNEL_ADDR_TEXT 0xffffffff80000000ull

/* User canonical top — mirrors USER_TOP in the kernel. */
#define USER_TOP_GUESS 0x0000800000000000ull

/* ------------------------------------------------------------------------- */
/* Tiny print helpers                                                         */
/* ------------------------------------------------------------------------- */

static size_t ustrlen(const char *s)
{
	size_t n = 0;
	while (s[n])
		n++;
	return n;
}

static void puts_(const char *s) { (void)write(1, s, ustrlen(s)); }

static void put_long(long v)
{
	char buf[24];
	size_t i = sizeof(buf);
	int neg = 0;
	unsigned long u;

	if (v < 0) {
		neg = 1;
		u = (unsigned long)(-(v + 1)) + 1ul;
	} else {
		u = (unsigned long)v;
	}
	if (u == 0) {
		buf[--i] = '0';
	} else {
		while (u && i > 0) {
			buf[--i] = (char)('0' + u % 10);
			u /= 10;
		}
	}
	if (neg && i > 0)
		buf[--i] = '-';
	(void)write(1, &buf[i], sizeof(buf) - i);
}

static void put_hex(unsigned long v)
{
	static const char hex[] = "0123456789abcdef";
	char buf[18];
	size_t i = sizeof(buf);

	if (v == 0) {
		buf[--i] = '0';
	} else {
		while (v && i > 0) {
			buf[--i] = hex[v & 0xf];
			v >>= 4;
		}
	}
	if (i >= 2) {
		buf[--i] = 'x';
		buf[--i] = '0';
	}
	(void)write(1, &buf[i], sizeof(buf) - i);
}

/* ------------------------------------------------------------------------- */
/* Attack table                                                               */
/* ------------------------------------------------------------------------- */

static int attack_no = 0;

static void report(const char *name, long rc, int expect_negative)
{
	attack_no++;
	puts_("fuzz #");
	put_long(attack_no);
	puts_(" ");
	puts_(name);
	puts_(" rc=");
	put_long(rc);
	if (expect_negative && rc >= 0) {
		puts_("  [LEAK?] kernel did not reject\n");
	} else if (!expect_negative && rc < 0) {
		puts_("  [WAT?] expected success\n");
	} else {
		puts_("  [survived]\n");
	}
}

/* ------------------------------------------------------------------------- */
/* Syscall-number fuzz                                                        */
/* ------------------------------------------------------------------------- */

static void attack_bad_syscall_numbers(void)
{
	report("syscall(0xdead)", jnu_syscall0(0xdead), 1);
	report("syscall(-1)", jnu_syscall0(-1), 1);
	report("syscall(INT64_MAX)", jnu_syscall0((long)0x7fffffffffffffffL),
	       1);
	report("syscall(99) // first unimpl", jnu_syscall0(99), 1);
}

/* ------------------------------------------------------------------------- */
/* Pointer / range validation                                                 */
/* ------------------------------------------------------------------------- */

static void attack_user_ptr_validation(void)
{
	char dummy[16];

	/* write() with NULL buffer */
	report("write(1, NULL, 1)", write(1, (const void *)0, 1), 1);

	/* write() into the kernel half */
	report("write(1, kernel_hhdm, 1)",
	       write(1, (const void *)KERNEL_ADDR_HHDM, 1), 1);

	report("write(1, kernel_text, 1)",
	       write(1, (const void *)KERNEL_ADDR_TEXT, 1), 1);

	/* write() exactly at USER_TOP - should be entirely in kernel half */
	report("write(1, USER_TOP, 1)",
	       write(1, (const void *)USER_TOP_GUESS, 1), 1);

	/* write() that straddles the user/kernel boundary */
	report("write(1, USER_TOP-1, 16)",
	       write(1, (const void *)(USER_TOP_GUESS - 1), 16), 1);

	/* write() with absurdly large length - SIZE_MAX */
	report("write(1, &dummy, SIZE_MAX)", write(1, dummy, (size_t)-1), 1);

	/* read() into the kernel half - tries to make kernel write us */
	report("read(0, kernel_text, 16)",
	       read(0, (void *)KERNEL_ADDR_TEXT, 16), 1);
}

/* ------------------------------------------------------------------------- */
/* fd-table abuse                                                             */
/* ------------------------------------------------------------------------- */

static void attack_fd_abuse(void)
{
	char buf[8];

	/* fd 0 is "stdin" - should not be readable since nothing wired it */
	report("read(0, &buf, 8)", read(0, buf, sizeof(buf)), 1);

	/* fd that has never been opened */
	report("read(7, &buf, 8)", read(7, buf, sizeof(buf)), 1);

	/* negative fd */
	report("read(-1, &buf, 8)", read(-1, buf, sizeof(buf)), 1);

	/* INT_MIN fd */
	report("read(INT_MIN, &buf, 8)",
	       read((int)0x80000000, buf, sizeof(buf)), 1);

	/* INT_MAX fd */
	report("read(INT_MAX, &buf, 8)",
	       read((int)0x7fffffff, buf, sizeof(buf)), 1);

	/* close on never-opened fd */
	report("close(42)", close(42), 1);

	/* close on negative fd */
	report("close(-99)", close(-99), 1);

	/* write to bad fd 5 (unopened) — sys_write rejects fd != 1 && != 2 */
	report("write(5, ..)", write(5, "x", 1), 1);
}

/* ------------------------------------------------------------------------- */
/* lseek arithmetic                                                           */
/* ------------------------------------------------------------------------- */

static void attack_lseek_extremes(void)
{
	int fd = open("/test.txt", 0);
	if (fd < 0) {
		puts_("fuzz: lseek skipped, /test.txt missing rc=");
		put_long(fd);
		puts_("\n");
		return;
	}

	report("lseek(fd, INT64_MIN, SEEK_SET)",
	       lseek(fd, (int64_t)0x8000000000000000L, SEEK_SET), 1);
	report("lseek(fd, INT64_MAX, SEEK_SET)",
	       lseek(fd, (int64_t)0x7fffffffffffffffL, SEEK_SET), 1);
	report("lseek(fd, -1, SEEK_SET)", lseek(fd, -1, SEEK_SET), 1);
	report("lseek(fd, 0, 99)  // bogus whence", lseek(fd, 0, 99), 1);
	report("lseek(fd, INT64_MAX, SEEK_END)  // overflow",
	       lseek(fd, (int64_t)0x7fffffffffffffffL, SEEK_END), 1);
	report("lseek(fd, INT64_MAX, SEEK_CUR)  // overflow",
	       lseek(fd, (int64_t)0x7fffffffffffffffL, SEEK_CUR), 1);
	report("lseek(-1, 0, SEEK_SET)  // bad fd", lseek(-1, 0, SEEK_SET), 1);
	(void)close(fd);
}

/* ------------------------------------------------------------------------- */
/* String / path attacks (copy_string_from_user)                              */
/* ------------------------------------------------------------------------- */

/*
 * Build a path that has no NUL within the first page and starts close
 * to a page boundary. This used to make the buggy
 * copy_string_from_user reject valid short strings; now it should
 * cleanly return -ENAMETOOLONG (or -EFAULT if it runs off into
 * unmapped territory).
 */
static char giant_path[8192];
static char huge_exec_arg[70000];
static char *unterminated_argv[300];

static void attack_string_paths(void)
{
	for (size_t i = 0; i < sizeof(giant_path) - 1; i++) {
		giant_path[i] = 'A';
	}
	giant_path[sizeof(giant_path) - 1] = '\0';

	report("open(8KiB-path)", open(giant_path, 0), 1);

	/* path pointer in kernel half */
	report("open(kernel_text, 0)", open((const char *)KERNEL_ADDR_TEXT, 0),
	       1);

	/* NULL path */
	report("open(NULL, 0)", open((const char *)0, 0), 1);

	/* path right at USER_TOP-1 (no room for any byte) */
	report("open(USER_TOP-1, 0)",
	       open((const char *)(USER_TOP_GUESS - 1), 0), 1);
}

/* ------------------------------------------------------------------------- */
/* execve / retired spawn / waitpid abuse                                     */
/* ------------------------------------------------------------------------- */

static void attack_exec_wait(void)
{
	char *const argv[] = {"fuzz-exec", 0};
	char *const bad_env[] = {(char *)KERNEL_ADDR_TEXT, 0};
	char *const huge_argv[] = {"fuzz-exec", huge_exec_arg, 0};

	for (size_t i = 0; i < sizeof(huge_exec_arg) - 1; i++) {
		huge_exec_arg[i] = 'B';
	}
	huge_exec_arg[sizeof(huge_exec_arg) - 1] = '\0';
	for (size_t i = 0; i < sizeof(unterminated_argv) /
					sizeof(unterminated_argv[0]);
	     i++) {
		unterminated_argv[i] = "x";
	}

	/*
	 * sys_spawn is fully retired in v0.0.3 (its slot 9 now belongs
	 * to mmap, landing in Phase 2). Syscall 99 below already
	 * exercises the -ENOSYS path, so we drop the spawn-specific
	 * probe rather than perpetuate the macro.
	 */
	report("execve(NULL)", execve((const char *)0, argv, 0), 1);
	report("execve(kernel_text)",
	       execve((const char *)KERNEL_ADDR_TEXT, argv, 0), 1);
	report("execve(\"/no/such/path\")", execve("/no/such/path", argv, 0),
	       1);
	report("execve(empty argv)", execve("/bin/hello", (char *const *)0, 0),
	       1);
	report("execve(bad argv pointer)",
	       execve("/bin/hello", (char *const *)KERNEL_ADDR_TEXT, 0), 1);
	report("execve(bad envp pointer)",
	       execve("/bin/hello", argv, (char *const *)KERNEL_ADDR_TEXT), 1);
	report("execve(bad env string)", execve("/bin/hello", argv, bad_env),
	       1);
	report("execve(huge argv)", execve("/bin/hello", huge_argv, 0), 1);
	report("execve(argv missing terminator)",
	       execve("/bin/hello", unterminated_argv, 0), 1);

	/* waitpid for a pid that does not exist */
	int st = 0;
	report("waitpid(99999, &st)", waitpid(99999, &st), 1);
	report("waitpid(-1, &st) // no children", waitpid(-1, &st), 1);
	report("waitpid(0, &st) // illegal", waitpid(0, &st), 1);

	/* waitpid with a status pointer in kernel space */
	report("waitpid(99999, kernel_text)",
	       waitpid(99999, (int *)KERNEL_ADDR_TEXT), 1);
}

/* ------------------------------------------------------------------------- */
/* fstat abuse                                                                */
/* ------------------------------------------------------------------------- */

static void attack_fstat(void)
{
	struct jnu_stat st;

	report("fstat(-1, &st)", fstat(-1, &st), 1);
	report("fstat(0, &st)", fstat(0, &st), 1);
	report("fstat(99, &st)", fstat(99, &st), 1);
	report("fstat(1, NULL)", fstat(1, (void *)0), 1);
	report("fstat(1, kernel_text)", fstat(1, (void *)KERNEL_ADDR_TEXT), 1);
}

/* ------------------------------------------------------------------------- */
/* W^X / NX checks                                                            */
/* ------------------------------------------------------------------------- */

/*
 * Attempt to JIT — write a `ret` into our stack and call it. If the
 * stack is mapped X this will succeed (BUG: NX not enforced on stack).
 * If NX is enforced this raises a #PF and the kernel must kill us
 * cleanly without taking the system down.
 *
 * The nxprobe binary is optional; if absent, the child exits 127 and
 * the parent still proves fork preserved enough state to wait cleanly.
 */
static void attack_nx_stack_in_child(void)
{
	char *const argv[] = {"nxprobe", 0};
	int child = fork();
	int st = 0;

	if (child == 0) {
		(void)execve("/bin/nxprobe", argv, 0);
		exit(127);
	}
	if (child < 0) {
		puts_("fuzz: nxprobe fork failed\n");
		return;
	}
	(void)waitpid(child, &st);
	puts_("fuzz: nxprobe child exit status=");
	put_long((long)st);
	puts_("\n");
}

/* ------------------------------------------------------------------------- */
/* Entry                                                                      */
/* ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	puts_("fuzz: pid ");
	put_long((long)getpid());
	puts_("\n");

	attack_bad_syscall_numbers();
	attack_user_ptr_validation();
	attack_fd_abuse();
	attack_lseek_extremes();
	attack_string_paths();
	attack_exec_wait();
	attack_fstat();
	attack_nx_stack_in_child();

	puts_("fuzz: completed ");
	put_long(attack_no);
	puts_(" attacks; kernel survived.\n");
	(void)put_hex(0xc0ffee);
	puts_("\n");
	return 0;
}
