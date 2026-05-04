#include <jnu_syscall.h>

static void puts(const char *s)
{
	size_t len = 0;

	while (s[len]) {
		len++;
	}
	(void)write(1, s, len);
}

static void put_uint(unsigned value)
{
	char buf[16];
	size_t i = sizeof(buf);

	if (value == 0) {
		puts("0");
		return;
	}

	while (value && i > 0) {
		buf[--i] = (char)('0' + value % 10);
		value /= 10;
	}
	(void)write(1, &buf[i], sizeof(buf) - i);
}

static char *const musltest_argv[] = {"musltest", 0};

int main(int argc, char **argv)
{
	int status = 0;
	int child;

	(void)argc;
	(void)argv;

	puts("JNU init: hello from ring 3\n");
	puts("JNU init: pid ");
	put_uint((unsigned)getpid());
	puts("\n");

	child = fork();
	if (child == 0) {
		int err = execve("/bin/musltest", musltest_argv, 0);

		puts("JNU init: execve failed rc=");
		put_uint((unsigned)(-err));
		puts("\n");
		exit(127);
	}

	if (child > 0) {
		(void)waitpid(child, &status);
		puts("JNU init: musltest exited ");
		put_uint((unsigned)status);
		puts("\n");
	} else {
		puts("JNU init: fork failed\n");
	}

	return 0;
}
